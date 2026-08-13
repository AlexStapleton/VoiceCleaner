#pragma once
#include <cstddef>
#include <deque>
#include "frame.h"
namespace vc {
class FrameAccumulator {
public:
  void write(const float* in, size_t n);
  bool pop_frame(float* out480);   // true if a full frame was written
  size_t buffered() const noexcept { return buf_.size(); }
private:
  std::deque<float> buf_;
};
class FrameDistributor {
public:
  void push_frame(const float* in480);
  size_t read(float* out, size_t n);   // returns samples written (< n on underrun)
  size_t buffered() const noexcept { return buf_.size(); }
private:
  std::deque<float> buf_;
};
}
