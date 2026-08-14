#include "agc.h"
#include <algorithm>
#include <cmath>

namespace vc {

Agc::Agc(const Config& c)
    : cfg_(c),
      atkCoef_(std::exp(-1.0f / (c.attackMs * 0.001f * c.fs))),
      relCoef_(std::exp(-1.0f / (c.releaseMs * 0.001f * c.fs))),
      targetLin_(std::pow(10.0f, c.targetRmsDb / 20.0f)),
      maxG_(c.maxGainDb),
      minG_(c.minGainDb) {}

void Agc::process(const float* in, float* out, size_t n, bool vadActive) noexcept {
  for (size_t i = 0; i < n; ++i) {
    float x = in[i];
    // One-pole smoothing on power -> RMS envelope.
    envRms_ = std::sqrt(relCoef_ * envRms_ * envRms_ + (1.0f - relCoef_) * x * x);
    if (vadActive) {
      float desiredDb =
          20.0f * std::log10(targetLin_ / std::max(envRms_, 1e-6f));
      desiredDb = std::clamp(desiredDb, minG_, maxG_);
      // Attack when reducing gain, release when raising it.
      float coef = (desiredDb < gainDb_) ? atkCoef_ : relCoef_;
      gainDb_ = coef * gainDb_ + (1.0f - coef) * desiredDb;
    }
    // else: gain frozen (no adaptation during pauses).
    out[i] = x * std::pow(10.0f, gainDb_ / 20.0f);
  }
}

float Agc::currentGainDb() const noexcept { return gainDb_; }

void Agc::reset() noexcept {
  gainDb_ = 0.0f;
  envRms_ = 0.0f;
}

}  // namespace vc
