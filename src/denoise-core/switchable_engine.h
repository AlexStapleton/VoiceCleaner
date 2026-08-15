#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include "inoise_filter.h"
namespace vc {
// Runs two engines in parallel (both kept warm), emits the active one delay-aligned to the
// higher-latency engine, and equal-power crossfades over `fadeFrames` frames on switch.
// requestActive() is safe to call from a non-RT thread; ProcessFrame reads it once per frame.
class SwitchableEngine : public INoiseFilter {
public:
  SwitchableEngine(INoiseFilter* a, INoiseFilter* b, int fadeFrames = 15);
  void     ProcessFrame(const float* in, float* out) noexcept override;  // 480 in/out
  float    LastVadProbability() const noexcept override;
  void     SetAggression(float z) noexcept override;
  uint32_t AlgorithmicDelaySamples() const noexcept override;            // == max(a,b)
  void     requestActive(int idx) noexcept { want_.store(idx, std::memory_order_relaxed); }
  int      activeIndex() const noexcept { return cur_; }
private:
  INoiseFilter* eng_[2];
  int   delaySamples_[2];        // per-engine delay lines to align to the max latency
  std::vector<float> dline_[2];  // circular sample delay buffers
  int   dpos_[2] = {0,0};
  int   fadeFrames_, fadeLeft_ = 0, cur_ = 0, from_ = 0;
  std::atomic<int> want_{0};
  float pushDelay(int e, float x) noexcept;   // push sample into delay line e, return delayed sample
};
}
