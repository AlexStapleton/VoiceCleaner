#include "rnnoise_filter.h"

#include <rnnoise.h>

namespace vc {

namespace {
constexpr int kFrameSize = 480;
constexpr float kInt16Scale = 32768.0f;
}  // namespace

RnnoiseFilter::RnnoiseFilter() {
  st_ = rnnoise_create(nullptr);  // built-in model
}

RnnoiseFilter::~RnnoiseFilter() {
  if (st_) {
    rnnoise_destroy(st_);
    st_ = nullptr;
  }
}

void RnnoiseFilter::ProcessFrame(const float* in, float* out) noexcept {
  // RNNoise operates on int16-range samples, not [-1,1].
  for (int i = 0; i < kFrameSize; ++i) {
    scratchIn_[i] = in[i] * kInt16Scale;
  }
  vad_ = rnnoise_process_frame(st_, scratchOut_, scratchIn_);
  for (int i = 0; i < kFrameSize; ++i) {
    out[i] = scratchOut_[i] / kInt16Scale;
  }
}

}  // namespace vc
