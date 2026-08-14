#include "limiter.h"
#include <algorithm>
#include <cmath>

namespace vc {

Limiter::Limiter(float ceilingDb, size_t lookahead, float releaseMs, float fs)
    : ceiling_(std::pow(10.0f, ceilingDb / 20.0f)),
      L_(lookahead < 1 ? 1 : lookahead),
      relCoef_(std::exp(-1.0f / (releaseMs * 0.001f * fs))),
      delay_(L_, 0.0f) {}

void Limiter::process(const float* in, float* out, size_t n) noexcept {
  for (size_t i = 0; i < n; ++i) {
    float x = in[i];
    // Look-ahead peak: the largest magnitude anywhere in the delay window plus the
    // incoming sample. Driving the target from the window peak keeps the reduction
    // in place until the loud sample has fully transited the delay, so the reduction
    // is guaranteed present by the time that sample emerges from the delay line.
    float peak = std::fabs(x);
    for (size_t k = 0; k < L_; ++k) peak = std::max(peak, std::fabs(delay_[k]));
    float target = (peak > ceiling_) ? ceiling_ / peak : 1.0f;
    // Instant attack (worst-case), slow release back toward unity.
    if (target < gain_) {
      gain_ = target;
    } else {
      gain_ = relCoef_ * gain_ + (1.0f - relCoef_) * 1.0f;
    }
    float delayed = delay_[widx_];
    delay_[widx_] = x;
    widx_ = (widx_ + 1) % L_;
    out[i] = delayed * gain_;
  }
}

void Limiter::reset() noexcept {
  std::fill(delay_.begin(), delay_.end(), 0.0f);
  widx_ = 0;
  gain_ = 1.0f;
}

}  // namespace vc
