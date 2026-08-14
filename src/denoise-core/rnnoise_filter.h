#pragma once
#include "inoise_filter.h"

struct DenoiseState;  // fwd (rnnoise C type)

namespace vc {

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
  void     SetAggression(float) noexcept override {}   // RNNoise has no native aggression; no-op for now
  uint32_t AlgorithmicDelaySamples() const noexcept override { return 0; }  // RNNoise adds no extra look-ahead beyond the frame

private:
  DenoiseState* st_ = nullptr;
  float vad_ = 0.0f;
  float scratchIn_[480];
  float scratchOut_[480];
};

}  // namespace vc
