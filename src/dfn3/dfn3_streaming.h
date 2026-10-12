#pragma once
#include <cstdint>
#include <string>
namespace vc {
// Real-time DFN3. Loads the *_streaming ONNX models from modelDir and processes one
// 480-sample hop per call, carrying GRU + normalization + window state. All model-specific
// numbers (hidden size, GRU layer counts, conv/df lookahead) are read at construction from
// the model state shapes + config.ini, so any DFN3 split works (it was validated on both the
// standard set the app ships, models/dfn3, and a since-removed causal `_ll` set). Algorithmic
// latency is latencySamples(): one STFT hop plus the encoder lookahead -- 1440 (30 ms) for
// models/dfn3; 480 for a lookahead-0 model.
class Dfn3Streaming {
public:
  explicit Dfn3Streaming(const std::string& modelDir);  // throws std::runtime_error on load failure
  ~Dfn3Streaming();
  Dfn3Streaming(const Dfn3Streaming&) = delete;
  Dfn3Streaming& operator=(const Dfn3Streaming&) = delete;
  // process() is the RT-safe hot path: noexcept and (after warm-up) allocation-free on the
  // success path. On the rare ORT-failure path it silences the hop, counts it, and flags a
  // one-off resync (a reset() before the next hop) -- that resync reallocates, so a single
  // post-failure hop is not RT-safe, but steady-state processing is.
  void process(const float* in480, float* out480) noexcept;  // exactly 480 in/out
  uint32_t latencySamples() const noexcept;                  // 1440 for models/dfn3
  float lastLsnr() const noexcept;
  bool lastHopFailed() const noexcept;    // true if the last process() hop threw (silenced)
  uint64_t failedHops() const noexcept;   // sticky count of silenced hops since construction
  // reset() reallocates (ring + ORT re-bind) -- NOT real-time-safe; call only off the audio
  // thread (e.g. before streaming starts). process() is the RT-safe hot path.
  void reset() noexcept;
  void setAggression(float slider) noexcept;   // 0..1; shapes the ERB mask (DFN3-only). 0 = off (identity).
private:
  struct Impl; Impl* p_;
};
}
