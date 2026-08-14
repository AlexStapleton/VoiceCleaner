#include "dfn3_features.h"

#include <cmath>

// ============================================================================
// libDF fidelity notes -- feature extraction for the DFN3 ONNX encoder.
//
// Reference: Rikorose/DeepFilterNet (cloned at implementation time). Two feature
// tensors are produced per frame, matching df/enhance.py:df_features():
//     erb_feat  = erb_norm(erb(spec, erb_fb), a)         # feat_erb  [.,32]
//     spec_feat = as_real(unit_norm(spec[..,:nb_df], a)) # feat_spec [.,2,.,96]
//
// Constants (models/dfn3/config.ini [df]): sr=48000, fft_size=960, hop_size=480,
//   nb_erb=32, nb_df=96, norm_tau=1, min_nb_erb_freqs=2.  freq_size = 960/2+1 = 481.
//
// --- alpha (exponential-mean decay factor) ---------------------------------
// DeepFilterNet/df/utils.py:124  _calculate_norm_alpha:
//     dt = hop_size / sr;  a_ = exp(-dt / tau)
// DeepFilterNet/df/utils.py:111  get_norm_alpha: rounds a_ up in precision until
//   the rounded value is < 1.0 (starts at precision 3).
//     dt = 480/48000 = 0.01;  a_ = exp(-0.01) = 0.99004983...
//     round(a_, 3) = 0.990 (< 1.0)  =>  alpha = 0.99f exactly.
//
// --- feat_erb  (libDF/src/lib.rs:206-212 feat_erb) -------------------------
// 1. compute_band_corr(out, x, x, erb_fb)  (lib.rs:280-295): per ERB band,
//    out_b = sum_over_band( (re*re + im*im) * (1/band_size) )
//          = mean power over the band  ==  ErbFilterBank::compress(power).
// 2. dB:  o = (o + 1e-10).log10() * 10.       (lib.rs:209 / transforms.rs:250)
// 3. band_mean_norm_erb (lib.rs:244-251):     alpha = 0.99
//        s = x*(1-alpha) + s*alpha;  x -= s;  x /= 40.
//    mean_norm_state init (lib.rs:184-194 / transforms.rs:310):
//        linspace(MEAN_NORM_INIT[0]=-60, MEAN_NORM_INIT[1]=-90, 32)
//        state[i] = -60 + i*(-30/31),  i=0..31.
//
// --- feat_spec  (libDF/src/lib.rs:253-259 band_unit_norm) ------------------
// unit_norm over the first nb_df=96 complex bins, alpha = 0.99:
//        s = |x|*(1-alpha) + s*alpha;   x /= sqrt(s)        (|x| = magnitude)
//   unit_norm_state init (lib.rs:195-204 / transforms.rs:341):
//        linspace(UNIT_NORM_INIT[0]=0.001, UNIT_NORM_INIT[1]=0.0001, 96)
//        state[i] = 0.001 + i*(-0.0009/95),  i=0..95.
// Output layout: as_real of the complex tensor gives ONNX feat_spec [1,2,T,96];
//   we emit the transposed real-then-imag plane layout exactly as libDF's
//   band_unit_norm_t (lib.rs:264-278): featSpec[0..95]=re/sqrt(s),
//   featSpec[96..191]=im/sqrt(s).
//
// --- Interpretation / divergence notes -------------------------------------
// * ErbFilterBank::compress averages the per-bin POWER (re^2+im^2); this is
//   arithmetically identical to compute_band_corr(x,x) with k=1/band_size, so
//   the ERB band widths and means match libDF bit-for-bit (same erb_fb algo).
// * reset(): libDF's DFState::reset (lib.rs:156-159) clears only the STFT
//   analysis/synthesis buffers, NOT the norm states -- those are (re)initialized
//   separately via init_norm_states. Here reset() restores the initial norm
//   states, matching this class's documented contract and the ResetRestoresInitial
//   test. No STFT buffers live in this class.
// * alpha is hard-coded to 0.99f (the exact value get_norm_alpha yields for the
//   config.ini constants) rather than recomputed, to guarantee the same rounded
//   constant libDF uses at inference.
// ============================================================================

namespace vc {

Dfn3Features::Dfn3Features()
    : erb_(481, 32, 48000.0f, 2),
      alpha_(0.99f),
      meanNormState_(32),
      unitNormState_(96),
      power_(481) {
  reset();
}

void Dfn3Features::reset() noexcept {
  // MEAN_NORM_INIT = [-60, -90]; linspace over 32 ERB bands (lib.rs:184-194).
  {
    const float mn = -60.0f, mx = -90.0f;
    const float step = (mx - mn) / static_cast<float>(32 - 1);
    for (int i = 0; i < 32; ++i) meanNormState_[i] = mn + static_cast<float>(i) * step;
  }
  // UNIT_NORM_INIT = [0.001, 0.0001]; linspace over 96 DF bins (lib.rs:195-204).
  {
    const float mn = 0.001f, mx = 0.0001f;
    const float step = (mx - mn) / static_cast<float>(96 - 1);
    for (int i = 0; i < 96; ++i) unitNormState_[i] = mn + static_cast<float>(i) * step;
  }
}

void Dfn3Features::process(const float* re, const float* im, float* featErb,
                           float* featSpec) noexcept {
  const float alpha = alpha_;

  // ---- feat_erb -----------------------------------------------------------
  // 1. per-bin power (re^2 + im^2), then ERB-band mean via the filterbank.
  for (int i = 0; i < 481; ++i) power_[i] = re[i] * re[i] + im[i] * im[i];
  erb_.compress(power_.data(), featErb);  // == compute_band_corr(x,x, erb_fb)
  // 2. dB scale, 3. exponential mean normalization.
  for (int b = 0; b < 32; ++b) {
    float x = std::log10(featErb[b] + 1e-10f) * 10.0f;
    float& s = meanNormState_[b];
    s = x * (1.0f - alpha) + s * alpha;
    x -= s;
    x /= 40.0f;
    featErb[b] = x;
  }

  // ---- feat_spec (first nb_df=96 complex bins, unit norm, transposed) ------
  for (int k = 0; k < 96; ++k) {
    const float r = re[k];
    const float ii = im[k];
    const float mag = std::sqrt(r * r + ii * ii);
    float& s = unitNormState_[k];
    s = mag * (1.0f - alpha) + s * alpha;
    const float inv = 1.0f / std::sqrt(s);
    featSpec[k] = r * inv;        // real plane  [0..95]
    featSpec[96 + k] = ii * inv;  // imag plane  [96..191]
  }
}

}  // namespace vc
