#include "noise_gate.h"
#include <cmath>
namespace vc {

static float onepole(float ms, float fs) { return std::exp(-1.0f / (0.001f * ms * fs)); }
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

NoiseGate::NoiseGate(const Config& c)
    : atkCoef_(onepole(c.attackMs, c.fs)),
      relCoef_(onepole(c.releaseMs, c.fs)),
      floorDb_(c.floorDb) {}

void NoiseGate::reset() noexcept { gain_ = 1.0f; }

void NoiseGate::process(const float* in, float* out, size_t n, bool open, float strength) noexcept {
  strength = clamp01(strength);
  // Closed-state gain: 0 dB at strength 0 (bypass) down to floorDb at strength 1.
  const float closedGain = strength <= 0.0f ? 1.0f : std::pow(10.0f, (strength * floorDb_) / 20.0f);
  for (size_t i = 0; i < n; ++i) {
    const float target = open ? 1.0f : closedGain;
    // Rising toward 1.0 (opening) => fast attack; falling toward the floor (closing) => smooth release.
    const float coef = (target > gain_) ? atkCoef_ : relCoef_;
    gain_ = coef * gain_ + (1.0f - coef) * target;
    out[i] = in[i] * gain_;
  }
}

}  // namespace vc
