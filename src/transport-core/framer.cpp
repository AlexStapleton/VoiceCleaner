#include "framer.h"
namespace vc {
void FrameAccumulator::write(const float* in, size_t n) {
  fifo_.write(in, n);
}
bool FrameAccumulator::pop_frame(float* out480) {
  if (fifo_.buffered() < FRAME_SAMPLES) return false;
  fifo_.read(out480, FRAME_SAMPLES);
  return true;
}
void FrameDistributor::push_frame(const float* in480) {
  fifo_.write(in480, FRAME_SAMPLES);
}
size_t FrameDistributor::read(float* out, size_t n) {
  return fifo_.read(out, n);
}
}
