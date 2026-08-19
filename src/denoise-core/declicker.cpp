#include "declicker.h"
#include <cmath>
#include <algorithm>
namespace vc {

static float onepole(float ms, float fs) { return std::exp(-1.0f / (0.001f * ms * fs)); }
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

Declicker::Declicker(const Config& c) : L_(c.lookahead) {
  const float rc = 1.0f / (2.0f * 3.14159265f * c.hpHz);
  const float dt = 1.0f / c.fs;
  hpA_ = rc / (rc + dt);
  fastCoef_ = onepole(c.fastMs, c.fs);
  slowCoef_ = onepole(c.slowMs, c.fs);
  relCoef_  = onepole(c.releaseMs, c.fs);
  holdSamples_ = L_ + (size_t)(c.holdMs * 0.001f * c.fs);
  crestK_ = c.crestK;
  voiceFloor_ = clamp01(c.voiceFloor);
  delay_.assign(L_ > 0 ? L_ : 1, 0.0f);
}

void Declicker::setStrength(float s) noexcept { strength_ = clamp01(s); }
void Declicker::setVoiceFloor(float f) noexcept { voiceFloor_ = clamp01(f); }

void Declicker::reset() noexcept {
  hpPrevIn_ = hpPrevOut_ = 0.0f; envF_ = 0.0f; envS_ = 1e-6f; gain_ = 1.0f; holdLeft_ = 0;
  std::fill(delay_.begin(), delay_.end(), 0.0f); widx_ = 0;
}

void Declicker::process(const float* in, float* out, size_t n, float strength, float vad) noexcept {
  strength = clamp01(strength); strength_ = strength; vad = clamp01(vad);
  const size_t sz = delay_.size();
  for (size_t i = 0; i < n; ++i) {
    const float x = in[i];
    const float hp = hpA_ * (hpPrevOut_ + x - hpPrevIn_);   // RC high-pass on the detection copy
    hpPrevIn_ = x; hpPrevOut_ = hp;
    const float mag = std::fabs(hp);
    envF_ = mag > envF_ ? mag : envF_ * fastCoef_;          // fast peak envelope
    envS_ = envS_ * slowCoef_ + mag * (1.0f - slowCoef_);   // slow baseline
    if (strength > 0.0f && envF_ > crestK_ * (envS_ + 1e-6f)) holdLeft_ = (int)holdSamples_;
    // VAD back-off with a floor: at vad=1 the duck is scaled by voiceFloor_ (not to zero), so clicks
    // that land on top of speech are still reduced (matters for engines that pass transients, e.g. DFN3).
    const float backoff = (1.0f - vad) + vad * voiceFloor_;
    const float depth   = holdLeft_ > 0 ? strength * backoff : 0.0f;
    if (holdLeft_ > 0) --holdLeft_;
    const float target = 1.0f - depth;
    gain_ = target < gain_ ? target                          // fast attack (down, within lookahead)
                           : gain_ * relCoef_ + target * (1.0f - relCoef_);  // smooth release (up)
    const float delayed = delay_[widx_];                     // read-before-write => L_-sample delay
    delay_[widx_] = x;
    widx_ = (widx_ + 1 == sz) ? 0 : widx_ + 1;
    out[i] = delayed * gain_;
  }
}

}  // namespace vc
