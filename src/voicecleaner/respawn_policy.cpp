#include "respawn_policy.h"
namespace vc::ui {

uint64_t RespawnPolicy::delayFor(int attempt) const noexcept {
  uint64_t d = cfg_.baseDelayMs;
  for (int i = 0; i < attempt && d < cfg_.maxDelayMs; ++i) d <<= 1;
  return d > cfg_.maxDelayMs ? cfg_.maxDelayMs : d;
}

void RespawnPolicy::reset() noexcept {
  attempts_ = 0; inReconnect_ = false; gaveUp_ = false; scheduled_ = false; scheduledAtMs_ = 0;
}

RespawnPolicy::Action RespawnPolicy::onTick(bool alive, bool ready, bool everReady, uint64_t nowMs) noexcept {
  if (alive && ready) { reset(); return Action::None; }   // healthy -> clear the episode
  if (gaveUp_) return Action::None;                        // terminal until reset()
  if (alive) return Action::None;                          // connecting (initial or post-respawn)

  // Worker is down. Reconnect-worthy if this instance ran, or we're already mid-episode.
  if (!everReady && !inReconnect_) return Action::GiveUp;  // genuine launch failure: don't loop

  inReconnect_ = true;
  if (attempts_ >= cfg_.maxAttempts) { gaveUp_ = true; return Action::GiveUp; }
  if (!scheduled_) { scheduled_ = true; scheduledAtMs_ = nowMs + delayFor(attempts_); return Action::None; }
  if (nowMs >= scheduledAtMs_) { ++attempts_; scheduled_ = false; return Action::Respawn; }
  return Action::None;                                     // waiting out the backoff
}

}  // namespace vc::ui
