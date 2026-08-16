#pragma once
#include <atomic>
#include <cstdint>
#include "params.h"
namespace vc::ipc {
// Double-buffered parameter snapshot with an atomic index flip (blueprint §8.3).
// Writer (non-RT command thread) calls publish(); reader (RT thread) calls snapshot() wait-free.
class ParamStore {
public:
  ParamStore() { slot_[0] = Params::defaults(); slot_[1] = Params::defaults(); }
  void publish(const Params& p) noexcept {
    const uint32_t next = 1u - active_.load(std::memory_order_relaxed);
    slot_[next] = p;                                   // plain store into the inactive slot
    active_.store(next, std::memory_order_release);    // publish
  }
  Params snapshot() const noexcept {
    return slot_[active_.load(std::memory_order_acquire)];   // one acquire load, wait-free
  }
private:
  Params slot_[2];
  std::atomic<uint32_t> active_{0};
};
}  // namespace vc::ipc
