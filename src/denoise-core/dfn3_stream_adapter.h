#pragma once
#include <memory>
#include <string>
#include "inoise_filter.h"
namespace vc {
class Dfn3Streaming;
// Adapts the real-time DFN3 orchestrator to INoiseFilter (480 samples/frame).
class Dfn3StreamAdapter : public INoiseFilter {
public:
  explicit Dfn3StreamAdapter(const std::string& modelDir);   // throws std::runtime_error on model load failure; warms up
  ~Dfn3StreamAdapter() override;
  void     ProcessFrame(const float* in, float* out) noexcept override;
  float    LastVadProbability() const noexcept override;      // from lsnr
  void     SetAggression(float) noexcept override {}          // DFN3 aggression: future work
  uint32_t AlgorithmicDelaySamples() const noexcept override; // = Dfn3Streaming::latencySamples() (480)
  uint64_t failedHops() const noexcept;                       // silenced ORT-failure hops (observability)
private:
  std::unique_ptr<Dfn3Streaming> eng_;
};
}
