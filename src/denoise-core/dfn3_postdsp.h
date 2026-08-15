#pragma once
#include <cstddef>
#include <vector>
#include "erb.h"
namespace vc {
// DeepFilterNet3 post-inference DSP: turn the ONNX outputs (ERB gain mask `m`
// and deep-filter coefficients `coefs`) into an enhanced spectrum, matching
// libDF. Offline whole-utterance form (the DFN3 ONNX export carries no recurrent
// state I/O, so it must be run over the full time sequence).

// Expand a 32-band ERB gain mask to per-bin gains and multiply onto one frame's
// complex spectrum. libDF `apply_mask` -> `apply_interp_band_gain` is a PLAIN
// per-band broadcast: every bin in a band is multiplied by that band's gain
// (lib.rs:223-224, 314-326). Uses the same band widths as ErbFilterBank::expand.
void apply_erb_mask(const ErbFilterBank& fb, const float* m32,
                    float* re481, float* im481) noexcept;

// Apply the DFN3 deep filter over the first nb_df bins of a whole spectrogram
// (offline form).
//   specRe/specIm: [T*481] row-major (frame t, bin f at t*481+f). The first
//     nbDf bins are replaced in place by the deep-filter output; bins >= nbDf
//     are left untouched. The pre-call low-bin values are snapshotted so the
//     temporal FIR reads the original spectrum, not partially-written output.
//   coefs: [T*nbDf*order*2] row-major = [T, nbDf, order, 2], last dim (re,im)
//     interleaved per tap. This is exactly what the df_dec ONNX node emits
//     ([1,T,96,10], 10 = order*2) and how libDF reshapes it ([ch,nb_df,df_order,2],
//     tract.rs:499).
//   T frames, nbDf=96, order=5, lookahead=2.
void apply_deep_filter(float* specRe, float* specIm, const float* coefs,
                       int T, int nbDf, int order, int lookahead) noexcept;

// Streaming deep filter: push one frame's low-bin noisy spectrum + its coefs per hop.
// Reproduces the whole-buffer apply_deep_filter(..., leftPad) EXACTLY, one output frame at a time.
//   tap: enhanced[outT,b] = Sum_{k<order} coefs[outT,b,k] (*) spec[outT + k - leftPad, b]  (zero-padded ends)
// leftPad = order - df_lookahead - 1 (causal past reach); future = order - 1 - leftPad frames of look-ahead.
// Output frame outT is emitted once frame (outT + future) has been pushed. One instance per stream.
class DeepFilterRing {
public:
  DeepFilterRing(int nbDf, int order, int leftPad);
  // push frame t in arrival order; returns true and fills outT / outRe[nbDf] / outIm[nbDf]
  // when an output frame becomes ready (outT = pushed_so_far - 1 - future).
  bool push(const float* re481, const float* im481, const float* coefs_nbDf_order2,
            int& outT, float* outRe, float* outIm) noexcept;
  // after the last push, drain the final `future` frames (constant-zero future).
  bool flush(int& outT, float* outRe, float* outIm) noexcept;
private:
  int nbDf_, order_, leftPad_, future_, depth_;
  int pushed_ = 0, emitted_ = 0;
  std::vector<float> re_, im_, cf_;   // ring buffers (choose depth to cover the tap span)
  bool emit(int outT, float* outRe, float* outIm) noexcept;
};
}  // namespace vc
