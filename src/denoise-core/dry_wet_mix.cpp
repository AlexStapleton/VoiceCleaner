#include "dry_wet_mix.h"
#include <algorithm>
namespace vc {

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

DryWetMix::DryWetMix(size_t engineDelaySamples) : L_(engineDelaySamples) {
  delay_.assign(L_ > 0 ? L_ : 1, 0.0f);
}

void DryWetMix::reset() noexcept { std::fill(delay_.begin(), delay_.end(), 0.0f); widx_ = 0; }

void DryWetMix::process(const float* dry, const float* wet, float* out, size_t n, float strength) noexcept {
  const float s = clamp01(strength);
  const float ds = 1.0f - s;
  if (L_ == 0) {                                   // no engine delay: align is trivial
    for (size_t i = 0; i < n; ++i) out[i] = s * wet[i] + ds * dry[i];
    return;
  }
  const size_t sz = delay_.size();
  for (size_t i = 0; i < n; ++i) {
    const float delayedDry = delay_[widx_];        // read-before-write => L_-sample delay
    delay_[widx_] = dry[i];
    widx_ = (widx_ + 1 == sz) ? 0 : widx_ + 1;
    out[i] = s * wet[i] + ds * delayedDry;
  }
}

}  // namespace vc
