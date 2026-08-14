#pragma once
#include <cstddef>
#include <vector>
#include "erb.h"
namespace vc {
// DFN3 input-feature extractor. Stateful (running normalization) -- one instance per stream.
// Reproduces libDF's feat_erb (ERB power -> dB -> exponential mean norm) and unit_norm
// (per-bin running magnitude normalization of the first nb_df complex bins). See dfn3_features.cpp
// for the libDF file:line citations and the exact constants (alpha, init states).
class Dfn3Features {
public:
  Dfn3Features();                       // 481 bins, 32 ERB, 96 DF, norm_tau=1, sr=48000, hop=480
  // in: complex spectrum re[481], im[481] for the current frame.
  // out: featErb[32], featSpec[2*96] (layout: featSpec[0..95]=real of bins 0..95, featSpec[96..191]=imag).
  void process(const float* re, const float* im, float* featErb, float* featSpec) noexcept;
  void reset() noexcept;                // restore initial normalization state
  int nErb() const noexcept { return 32; }
  int nDf()  const noexcept { return 96; }
private:
  ErbFilterBank erb_;
  float alpha_;                         // exponential-mean decay factor (exp(-hop/sr/tau), rounded)
  std::vector<float> meanNormState_;    // erb dB running-mean state[32]
  std::vector<float> unitNormState_;    // per-bin running magnitude state[96]
  std::vector<float> power_;            // scratch: per-bin power[481]
};
}
