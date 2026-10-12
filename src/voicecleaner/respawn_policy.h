#pragma once
#include <cstdint>
namespace vc::ui {

// Pure auto-respawn policy. Fed the supervisor's per-tick status, it decides
// whether to leave the worker down, respawn it now (bounded backoff), or give up. Windows-free +
// deterministic (time is an argument), so it is fully unit-testable.
//
// A worker that dies AFTER it was ready (e.g. device loss) triggers a reconnect episode: respawn with
// exponential backoff up to maxAttempts, then give up. A reconnect-episode LATCH keeps counting the
// respawned worker's pre-ready deaths toward the cap, so a device that stays gone still stops after
// maxAttempts rather than being mistaken for a fresh launch failure after the first retry. A worker
// that never became ready in the first place (genuine launch failure) does NOT auto-loop.
class RespawnPolicy {
 public:
  struct Config { int maxAttempts = 5; uint64_t baseDelayMs = 500; uint64_t maxDelayMs = 8000; };
  enum class Action { None, Respawn, GiveUp };

  explicit RespawnPolicy(Config c = {}) : cfg_(c) {}

  // alive: worker process alive; ready: telemetry advancing; everReady: this instance was ready at
  // some point (at a death tick, pass !PollResult.launchFailure). nowMs: monotonic ms (GetTickCount64).
  Action onTick(bool alive, bool ready, bool everReady, uint64_t nowMs) noexcept;
  void   reset() noexcept;                 // manual Relaunch clears the episode

  bool reconnecting() const noexcept { return inReconnect_ && !gaveUp_; }
  int  attempts() const noexcept { return attempts_; }
  int  maxAttempts() const noexcept { return cfg_.maxAttempts; }

 private:
  uint64_t delayFor(int attempt) const noexcept;   // baseDelayMs << attempt, capped at maxDelayMs

  Config   cfg_;
  int      attempts_ = 0;
  bool     inReconnect_ = false;
  bool     gaveUp_ = false;
  bool     scheduled_ = false;
  uint64_t scheduledAtMs_ = 0;
};

}  // namespace vc::ui
