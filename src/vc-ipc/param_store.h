#pragma once
#include <atomic>
#include <cstdint>
#include "params.h"
namespace vc::ipc {
// Double-buffered parameter snapshot with an atomic index flip (blueprint §8.3).
// Writer (non-RT command thread) calls publish(); reader (RT thread) calls snapshot() wait-free.
//
// Invariant: single-writer. Only one thread may ever call publish(); concurrent publish()
// calls would race on writing the same inactive slot. snapshot() may be called from any
// number of reader threads.
//
// Two slots suffice because publish() is UI-cadence (at most a handful of calls per second,
// driven by user interaction) while snapshot() is a sub-microsecond POD copy of a small
// struct. A reader that loads active_ is guaranteed to see a fully-written slot (release/
// acquire pairing), and in practice no publish() can complete between a snapshot()'s load
// and its copy given the enormous gap between UI cadence and the RT thread's read latency.
// This is not enforced by the type — it is a usage contract on the caller.
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
