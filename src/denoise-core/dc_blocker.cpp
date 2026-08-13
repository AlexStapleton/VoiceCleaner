#include "dc_blocker.h"

namespace vc {

void DcBlocker::process(const float* in, float* out, size_t n) noexcept {
  for (size_t i = 0; i < n; ++i) {
    float x = in[i];
    float y = x - x1_ + R_ * y1_;
    x1_ = x;
    y1_ = y;
    out[i] = y;
  }
}

}  // namespace vc
