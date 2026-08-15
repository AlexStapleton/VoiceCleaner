#include "dfn3_postdsp.h"

#include <vector>

// ============================================================================
// DeepFilterNet3 post-inference DSP. Formulas cited against the DeepFilterNet
// reference (github.com/Rikorose/DeepFilterNet, MIT OR Apache-2.0):
//   R  = libDF Rust runtime  (libDF/src/lib.rs, libDF/src/tract.rs)
//   Py = DeepFilterNet Python (DeepFilterNet/df/modules.py) -- authoritative
//        offline definition of the deep filter.
//
// --- (1) ERB mask application --------------------------------------------
// R lib.rs:223-224  DFState::apply_mask(output, gains) -> apply_interp_band_gain
// R lib.rs:314-326  apply_interp_band_gain: PLAIN per-band broadcast --
//     for each band b of width `band_size`, every bin j in that band is
//     multiplied by the single band gain: out[bcsum+j] *= band_e[b].
//   => NOT an interpolation. Bin g in band b just gets multiplied by m[b].
//      Identical band partition to ErbFilterBank (erb_fb, lib.rs:68-99).
//
// --- (2) Deep filter -----------------------------------------------------
// Layout: R tract.rs:499  coefs.set_shape(&[ch, nb_df, df_order, 2]); the
//   df_dec ONNX node emits [1, T, nb_df=96, 10] with 10 = df_order(5)*2, last
//   dim (re,im) INTERLEAVED per tap. Full offline buffer is therefore
//   [T, nbDf, order, 2] row-major:
//       coefs[(( (size_t)t*nbDf + b )*order + k )*2 + c],  c: 0=re, 1=im.
//
// Offline temporal FIR: Py modules.py:352-368 forward_real_loop (the reference
//   uses dfop_method "real_unfold", modules.py:388-406, which is mathematically
//   identical), with padding Py modules.py:481-489 spec_pad(window=df_order=5,
//   lookahead=df_lookahead=2, dim=time):
//       left pad  = window - lookahead - 1 = 5 - 2 - 1 = 2
//       right pad = lookahead              = 2
//   so padded[t] = spec[t-2] and forward_real_loop accumulates
//       spec_f[t] += padded[k + t] * coefs[t,k]   (k = tap 0..order-1)
//   => padded[k+t] = spec[t + k - lookahead]. THEREFORE the exact tap offsets:
//
//       enhanced[t,b] = SUM_{k=0..order-1} coefs[t,b,k] (*) spec[t + k - la, b]
//
//   with `(*)` complex multiply and out-of-range spec frames (t+k-la < 0 or
//   >= T) treated as ZERO (from the constant padding).
//     Tap k=0 -> spec[t-2], k=1 -> spec[t-1], k=2 -> spec[t] (current),
//         k=3 -> spec[t+1], k=4 -> spec[t+2] (2-frame lookahead into future).
//   Complex product per Py modules.py:364-367:
//       re = s_re*c_re - s_im*c_im ;  im = s_im*c_re + s_re*c_im
//
// The FIR replaces bins [0, nbDf); higher bins are untouched (R tract.rs:744
// zeros o_f[..nb_df] then accumulates only those bins).
//
// FIDELITY NOTES for the orchestrator (see final report):
//  * Tap ordering: the provisional spec in the task doc read spec[t+la-k];
//    the authoritative libDF/Py definition is spec[t + k - la] (taps reversed).
//    This impl uses the authoritative form.
//  * In the R streaming runtime the deep filter runs on the NOISY (unmasked)
//    low bins (rolling_spec_buf_x, tract.rs:588-597) and the ERB-masked spec is
//    kept only for bins >= nb_df. This function applies the FIR to whatever
//    spectrum it is handed; the caller chooses which spectrum feeds the low bins.
// ============================================================================

namespace vc {

void apply_erb_mask(const ErbFilterBank& fb, const float* m32,
                    float* re481, float* im481) noexcept {
  // Plain per-band broadcast (lib.rs:314-326). Reuses the ErbFilterBank band
  // widths so the partition is bit-identical to compress()/expand().
  const std::vector<int>& widths = fb.bandWidths();
  const int nBands = fb.nBands();
  int base = 0;
  for (int b = 0; b < nBands; ++b) {
    const int w = widths[b];
    const float g = m32[b];
    for (int j = 0; j < w; ++j) {
      re481[base + j] *= g;
      im481[base + j] *= g;
    }
    base += w;
  }
}

void apply_deep_filter(float* specRe, float* specIm, const float* coefs,
                       int T, int nbDf, int order, int lookahead) noexcept {
  if (T <= 0 || nbDf <= 0 || order <= 0) return;
  constexpr int kNFreq = 481;

  // Snapshot the original low-bin spectrum: the FIR at output frame t reads
  // input frames [t-la, t-la+order-1], which overlap frames whose low bins we
  // are about to overwrite. Reading from a copy keeps it a pure convolution.
  const size_t lowCount = static_cast<size_t>(T) * static_cast<size_t>(nbDf);
  std::vector<float> srcRe(lowCount), srcIm(lowCount);
  for (int t = 0; t < T; ++t) {
    const float* re = specRe + static_cast<size_t>(t) * kNFreq;
    const float* im = specIm + static_cast<size_t>(t) * kNFreq;
    float* dRe = srcRe.data() + static_cast<size_t>(t) * nbDf;
    float* dIm = srcIm.data() + static_cast<size_t>(t) * nbDf;
    for (int b = 0; b < nbDf; ++b) { dRe[b] = re[b]; dIm[b] = im[b]; }
  }

  for (int t = 0; t < T; ++t) {
    float* outRe = specRe + static_cast<size_t>(t) * kNFreq;
    float* outIm = specIm + static_cast<size_t>(t) * kNFreq;
    for (int b = 0; b < nbDf; ++b) {
      float accRe = 0.0f, accIm = 0.0f;
      for (int k = 0; k < order; ++k) {
        const int ti = t + k - lookahead;         // input frame for tap k
        if (ti < 0 || ti >= T) continue;           // constant zero padding
        const size_t si = static_cast<size_t>(ti) * nbDf + b;
        const float sRe = srcRe[si];
        const float sIm = srcIm[si];
        const size_t ci =
            ((static_cast<size_t>(t) * nbDf + b) * order + k) * 2;
        const float cRe = coefs[ci];
        const float cIm = coefs[ci + 1];
        accRe += sRe * cRe - sIm * cIm;            // complex multiply
        accIm += sIm * cRe + sRe * cIm;
      }
      outRe[b] = accRe;
      outIm[b] = accIm;
    }
  }
}

// --- Streaming (ring-based) deep filter ------------------------------------
// Emits one output frame at a time, reproducing apply_deep_filter EXACTLY.
// The tap for output frame outT reads input frames [outT-leftPad, outT+future],
// future = order-1-leftPad. Frames outside the pushed range are zero (constant
// padding), matching the whole-buffer form's ti<0 / ti>=T checks.
DeepFilterRing::DeepFilterRing(int nbDf, int order, int leftPad)
    : nbDf_(nbDf),
      order_(order),
      leftPad_(leftPad),
      future_(order - 1 - leftPad),
      depth_(order + (order - 1 - leftPad) + 1) {
  if (nbDf_ < 0) nbDf_ = 0;
  if (order_ < 1) order_ = 1;
  if (future_ < 0) future_ = 0;
  if (depth_ < 1) depth_ = 1;
  re_.assign(static_cast<size_t>(depth_) * nbDf_, 0.0f);
  im_.assign(static_cast<size_t>(depth_) * nbDf_, 0.0f);
  cf_.assign(static_cast<size_t>(depth_) * nbDf_ * order_ * 2, 0.0f);
}

bool DeepFilterRing::emit(int outT, float* outRe, float* outIm) noexcept {
  const size_t coefStride = static_cast<size_t>(nbDf_) * order_ * 2;
  const size_t cslot = static_cast<size_t>(outT % depth_) * coefStride;
  for (int b = 0; b < nbDf_; ++b) {
    float accRe = 0.0f, accIm = 0.0f;
    for (int k = 0; k < order_; ++k) {
      const int ti = outT + k - leftPad_;          // input frame for tap k
      if (ti < 0 || ti >= pushed_) continue;        // constant zero padding
      const size_t si = static_cast<size_t>(ti % depth_) * nbDf_ + b;
      const float sRe = re_[si];
      const float sIm = im_[si];
      const size_t ci = cslot + (static_cast<size_t>(b) * order_ + k) * 2;
      const float cRe = cf_[ci];
      const float cIm = cf_[ci + 1];
      accRe += sRe * cRe - sIm * cIm;               // complex multiply
      accIm += sIm * cRe + sRe * cIm;
    }
    outRe[b] = accRe;
    outIm[b] = accIm;
  }
  return true;
}

bool DeepFilterRing::push(const float* re481, const float* im481,
                          const float* coefs_nbDf_order2, int& outT,
                          float* outRe, float* outIm) noexcept {
  const int slot = pushed_ % depth_;
  float* dRe = re_.data() + static_cast<size_t>(slot) * nbDf_;
  float* dIm = im_.data() + static_cast<size_t>(slot) * nbDf_;
  for (int b = 0; b < nbDf_; ++b) { dRe[b] = re481[b]; dIm[b] = im481[b]; }
  const size_t coefStride = static_cast<size_t>(nbDf_) * order_ * 2;
  float* dCf = cf_.data() + static_cast<size_t>(slot) * coefStride;
  for (size_t i = 0; i < coefStride; ++i) dCf[i] = coefs_nbDf_order2[i];
  ++pushed_;

  const int ready = pushed_ - 1 - future_;          // newest emittable frame
  if (ready < 0) return false;
  outT = ready;
  emit(ready, outRe, outIm);
  emitted_ = ready + 1;
  return true;
}

bool DeepFilterRing::flush(int& outT, float* outRe, float* outIm) noexcept {
  if (emitted_ >= pushed_) return false;             // all frames drained
  outT = emitted_;
  emit(emitted_, outRe, outIm);
  ++emitted_;
  return true;
}

}  // namespace vc
