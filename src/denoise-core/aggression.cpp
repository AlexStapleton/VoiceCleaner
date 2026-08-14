#include "aggression.h"
#include <algorithm>
#include <cmath>

namespace vc {

float aggression_gamma(float slider) noexcept {
  slider = std::clamp(slider, 0.0f, 1.0f);
  if (slider <= 0.5f) {
    return 0.4f + (1.0f - 0.4f) * (slider / 0.5f);
  }
  return 1.0f + (2.5f - 1.0f) * ((slider - 0.5f) / 0.5f);
}

void apply_aggression(float* gains, size_t n, float gamma, float gMin) noexcept {
  for (size_t i = 0; i < n; ++i) {
    gains[i] = std::max(gMin, std::pow(gains[i], gamma));
  }
}

}  // namespace vc
