#pragma once
#include <memory>
#include <string>
#include "inoise_filter.h"
#include "input_level_normalizer.h"
#include "pre_gain_stage.h"
namespace vc {
class Dfn3Streaming;
// Adapts the real-time DFN3 orchestrator to INoiseFilter (480 samples/frame).
class Dfn3StreamAdapter : public INoiseFilter {
public:
  struct Options {
    // true: an InputLevelNormalizer + PreGainStage lift quiet input into the model's working range and
    // undo the gain exactly on the output (spec Part 1). false: the stages are bypassed entirely and
    // the adapter is bit-identical to its pre-gain behavior (unity).
    bool autoInputLevel = true;
  };
  explicit Dfn3StreamAdapter(const std::string& modelDir) : Dfn3StreamAdapter(modelDir, Options{}) {}
  Dfn3StreamAdapter(const std::string& modelDir, const Options& opt);   // throws std::runtime_error on model load failure; warms up
  ~Dfn3StreamAdapter() override;
  void     ProcessFrame(const float* in, float* out) noexcept override;
  float    LastVadProbability() const noexcept override;      // from lsnr
  void     SetAggression(float z) noexcept override;   // forwards to Dfn3Streaming (DFN3 mask shaping)
  uint32_t AlgorithmicDelaySamples() const noexcept override; // = Dfn3Streaming::latencySamples() (480 for _ll, 1440 for standard)
  float    inputGainDb() const noexcept { return lastGainDb_; }   // current pre-gain (0 when bypassed); diagnostics
  uint64_t failedHops() const noexcept;                       // silenced ORT-failure hops (observability)
private:
  std::unique_ptr<Dfn3Streaming> eng_;
  bool autoLevel_;
  InputLevelNormalizer norm_;
  PreGainStage pre_;
  float lastGainDb_ = 0.0f;
};
}
