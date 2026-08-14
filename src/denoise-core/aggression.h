#pragma once
#include <cstddef>
namespace vc {
// Map a 0..1 slider to a gain-shaping exponent gamma: 0.0->0.4, 0.5->1.0, 1.0->2.5
// (piecewise-linear between those three anchor points). Clamps slider to [0,1].
float aggression_gamma(float slider) noexcept;
// Shape suppression band gains in place: g' = max(gMin, g^gamma). gMin ~ 10^(-20/20)=0.1 default.
void  apply_aggression(float* gains, size_t n, float gamma, float gMin) noexcept;
}
