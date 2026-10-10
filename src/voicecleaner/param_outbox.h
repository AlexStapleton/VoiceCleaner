#pragma once
#include <cstdint>
#include "params.h"
namespace vc::ui {
// Delivery state for UI -> worker parameter updates (pure, Windows-free; unit-tested).
//
// Each SetParams carries the FULL Params snapshot, so only the newest value matters and re-sending
// it is idempotent. The outbox holds that newest value until a send succeeds:
//   * stage() records the latest snapshot and marks it undelivered;
//   * due() says whether to attempt a send now. Slider drags call it unforced, which throttles
//     sends to one per minIntervalMs (a drag produces mouse-rate updates, and every send is a
//     synchronous pipe connect on the UI thread). Drag-end and discrete clicks force it;
//   * onAttempt() clears the pending value only when the send was delivered. A failed or
//     throttled value stays pending and is retried (next move, drag end, or the 100 ms tick),
//     so the worker always converges on the last value the UI shows.
class ParamOutbox {
 public:
  explicit ParamOutbox(uint64_t minIntervalMs = 30) : minIntervalMs_(minIntervalMs) {}

  void stage(const vc::ipc::Params& p) noexcept { pending_ = p; dirty_ = true; }
  bool dirty() const noexcept { return dirty_; }
  const vc::ipc::Params& pending() const noexcept { return pending_; }

  bool due(uint64_t nowMs, bool force) const noexcept {
    if (!dirty_) return false;
    if (force || !attempted_) return true;
    return nowMs - lastAttemptMs_ >= minIntervalMs_;
  }
  void onAttempt(uint64_t nowMs, bool delivered) noexcept {
    attempted_ = true;
    lastAttemptMs_ = nowMs;
    if (delivered) dirty_ = false;
  }

 private:
  uint64_t minIntervalMs_;
  vc::ipc::Params pending_{};
  bool dirty_ = false;
  bool attempted_ = false;
  uint64_t lastAttemptMs_ = 0;
};
}  // namespace vc::ui
