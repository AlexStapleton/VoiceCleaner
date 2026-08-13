#include "framer.h"
namespace vc {
void FrameAccumulator::write(const float* in, size_t n) {
  buf_.insert(buf_.end(), in, in + n);
}
bool FrameAccumulator::pop_frame(float* out480) {
  if (buf_.size() < FRAME_SAMPLES) return false;
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) { out480[i] = buf_.front(); buf_.pop_front(); }
  return true;
}
void FrameDistributor::push_frame(const float* in480) {
  buf_.insert(buf_.end(), in480, in480 + FRAME_SAMPLES);
}
size_t FrameDistributor::read(float* out, size_t n) {
  size_t k = 0;
  while (k < n && !buf_.empty()) { out[k++] = buf_.front(); buf_.pop_front(); }
  return k;
}
}
