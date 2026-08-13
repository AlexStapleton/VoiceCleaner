#pragma once
#include <cstdint>
namespace vc {
class INoiseFilter {
public:
  virtual ~INoiseFilter() = default;
  virtual void     ProcessFrame(const float* in, float* out) noexcept = 0;  // exactly 480 in/out
  virtual float    LastVadProbability() const noexcept = 0;
  virtual void     SetAggression(float zeroToOne) noexcept = 0;
  virtual uint32_t AlgorithmicDelaySamples() const noexcept = 0;
};
}
