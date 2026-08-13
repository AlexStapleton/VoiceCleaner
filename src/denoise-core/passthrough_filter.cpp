#include "passthrough_filter.h"
#include "frame.h"

#include <algorithm>

namespace vc {

void PassthroughFilter::ProcessFrame(const float* in, float* out) noexcept {
  std::copy(in, in + FRAME_SAMPLES, out);
}

}  // namespace vc
