#pragma once
#include "inoise_filter.h"
namespace vc {
class PassthroughFilter : public INoiseFilter {
public:
  void     ProcessFrame(const float* in, float* out) noexcept override;
  float    LastVadProbability() const noexcept override { return vad_; }
  uint32_t AlgorithmicDelaySamples() const noexcept override { return 0; }
private:
  float vad_ = 0.0f;
};
}
