#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "inoise_filter.h"
#include "spsc_ring.h"
#include "frame.h"   // FRAME_SAMPLES
namespace vc {

// Two-slot engine container (index 0 = RNNoise, 1 = DFN3) with an equal-power crossfade.
//  Warm: both leaves resident, BOTH run every frame (inactive stays warm), never retire.
//  Lean: only the active leaf resident; a switch loads the target on demand (off the RT thread),
//        crossfades it in, then retires (frees) the outgoing leaf -> back to lean.
// Output algorithmic delay is pinned to max(delaySamples[0], delaySamples[1]) in BOTH modes so a
// switch never shifts output timing.
//
// Threading: ProcessFrame() runs on the RT thread. requestActive()/reclaim() run on non-RT threads
// and MUST be serialized by the caller (one command mutex). The RT path takes no locks.
class DynamicEngine : public INoiseFilter {
public:
  enum class Mode { Warm, Lean };
  using Loader = std::function<std::unique_ptr<INoiseFilter>(int idx)>;  // idx 0/1 -> engine; may throw

  struct Config {
    Mode     mode = Mode::Lean;
    int      initialActive = 0;           // 0 = RNNoise, 1 = DFN3
    int      fadeFrames = 15;
    uint32_t delaySamples[2] = {0, 0};    // each engine's own algorithmic delay
  };

  DynamicEngine(Loader loader, const Config& cfg);   // builds initial leaf(s); may throw from loader
  ~DynamicEngine() override;

  // INoiseFilter (RT thread):
  void     ProcessFrame(const float* in, float* out) noexcept override;
  float    LastVadProbability() const noexcept override;
  void     SetAggression(float z) noexcept override;
  uint32_t AlgorithmicDelaySamples() const noexcept override { return maxDelay_; }

  // Non-RT control (serialize externally):
  void requestActive(int idx);                          // installs idx on demand if absent, then requests the fade
  int  activeIndex() const noexcept { return cur_.load(std::memory_order_relaxed); }
  bool resident(int idx) const noexcept { return eng_[clamp01(idx)].load(std::memory_order_acquire) != nullptr; }

  // Non-RT reclaim (serialize externally; never on the RT thread). Drains retired leaves; for each,
  // calls onRetire(ptr, idx) BEFORE deleting, then deletes.
  void reclaim(const std::function<void(INoiseFilter*, int)>& onRetire = {}) noexcept;

private:
  struct Retired { INoiseFilter* ptr; int idx; };
  static int clamp01(int i) noexcept { return i < 0 ? 0 : (i > 1 ? 1 : i); }
  float pushDelay(int slot, float x) noexcept;
  void  installLeaf(int idx);   // loader + aggression + delay-line reset + publish (non-RT)

  Loader   loader_;
  Mode     mode_;
  int      fadeFrames_;
  uint32_t maxDelay_ = 0;
  int      alignDelay_[2] = {0, 0};      // maxDelay_ - delaySamples[idx]
  std::vector<float> dline_[2];          // per-slot alignment buffers (size alignDelay_+1)
  int      dpos_[2] = {0, 0};

  std::atomic<INoiseFilter*> eng_[2]{nullptr, nullptr};  // owning raw pointers; nullptr = not resident
  std::atomic<int>   want_{0};
  std::atomic<float> lastAggression_{0.0f};
  std::atomic<int> cur_{0};       // published so activeIndex()/LastVadProbability are safe from any thread
  int from_ = 0, fadeLeft_ = 0;   // RT-owned (only ProcessFrame touches these)
  SpscRing<Retired>  retiredQ_{4};                       // RT producer, reclaim consumer
};
}  // namespace vc
