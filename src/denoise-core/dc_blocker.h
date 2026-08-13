#pragma once
#include <cstddef>
namespace vc {
// One-pole DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1]. Default R=0.998 (~15 Hz corner @ 48 kHz).
class DcBlocker {
public:
  explicit DcBlocker(float R = 0.998f) : R_(R) {}
  void process(const float* in, float* out, size_t n) noexcept;  // may alias (in==out) safely
  void reset() noexcept { x1_ = 0.0f; y1_ = 0.0f; }
private:
  float R_; float x1_ = 0.0f; float y1_ = 0.0f;
};
}
