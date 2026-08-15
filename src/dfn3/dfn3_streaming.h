#pragma once
#include <cstdint>
#include <string>
namespace vc {
// Real-time DFN3. Loads the *_streaming ONNX models from modelDir (e.g. "models/dfn3-ll") and
// processes one 480-sample hop per call, carrying GRU + normalization + window state.
// Output for hop t is the enhanced hop t (causal _ll model; algorithmic latency = STFT one hop).
class Dfn3Streaming {
public:
  explicit Dfn3Streaming(const std::string& modelDir);  // throws std::runtime_error on load failure
  ~Dfn3Streaming();
  Dfn3Streaming(const Dfn3Streaming&) = delete;
  Dfn3Streaming& operator=(const Dfn3Streaming&) = delete;
  void process(const float* in480, float* out480) noexcept;  // exactly 480 in/out
  uint32_t latencySamples() const noexcept;                  // ~kHop (480) for _ll
  float lastLsnr() const noexcept;
  bool lastHopFailed() const noexcept;  // true if the last process() hop threw (silenced)
  void reset() noexcept;
private:
  struct Impl; Impl* p_;
};
}
