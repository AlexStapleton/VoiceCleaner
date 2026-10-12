#pragma once
#include "inoise_filter.h"

struct DenoiseState;  // fwd (rnnoise C type)

namespace vc {

// RNNoise's real algorithmic delay: 2 frames = 960 samples (20 ms at 48 kHz). denoise.c analyses the
// window [previous 480 | current 480] but synthesizes `delayed_X`, the PREVIOUS frame's spectrum, with a
// 480-sample overlap-add. So input frame k leaves at call k+2 (calls 0 and 1 output ~zeros). DryWetMix
// (STRENGTH) and the latency readout both rely on this being right.
constexpr uint32_t kRnnoiseDelaySamples = 960;

// RNNoise engine behind INoiseFilter. Processes exactly 480 samples/frame at
// 48 kHz mono. RNNoise expects int16-range samples, so ProcessFrame scales
// [-1,1] float in/out by +/-32768 around the C call.
class RnnoiseFilter : public INoiseFilter {
public:
  RnnoiseFilter();
  ~RnnoiseFilter() override;

  RnnoiseFilter(const RnnoiseFilter&) = delete;
  RnnoiseFilter& operator=(const RnnoiseFilter&) = delete;

  void     ProcessFrame(const float* in, float* out) noexcept override;  // 480 samples
  float    LastVadProbability() const noexcept override { return vad_; }
  uint32_t AlgorithmicDelaySamples() const noexcept override { return kRnnoiseDelaySamples; }  // 2 frames: see kRnnoiseDelaySamples

private:
  DenoiseState* st_ = nullptr;
  float vad_ = 0.0f;
  float scratchIn_[480];
  float scratchOut_[480];
};

}  // namespace vc
