#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// Fixed per-sample delay (ring buffer). Allocates only in the constructor; push() is RT-safe.
// push(x) returns the sample pushed N calls ago (the initial fill value for the first N calls).
class DelayLine {
public:
  explicit DelayLine(size_t n = 0, float fill = 0.0f) : buf_(n, fill), fill_(fill) {}
  float push(float x) noexcept {
    if (buf_.empty()) return x;
    const float y = buf_[pos_];
    buf_[pos_] = x;
    if (++pos_ == buf_.size()) pos_ = 0;
    return y;
  }
  void reset() noexcept { for (auto& v : buf_) v = fill_; pos_ = 0; }
  size_t delay() const noexcept { return buf_.size(); }
private:
  std::vector<float> buf_;
  float fill_;
  size_t pos_ = 0;
};
}
