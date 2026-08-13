#pragma once
#include <cstddef>
namespace vc {
// Variable-rate resampler (ratio ~1.0). ratio = output_samples / input_samples.
// Maintains fractional read position + a small input history for continuity across process() calls.
// RT-safe: no allocation. Catmull-Rom cubic interpolation.
class Resampler {
public:
  void   set_ratio(double ratio) noexcept;
  double ratio() const noexcept;
  // Consume n_in input samples; write resampled output into out (capacity out_cap).
  // Returns number of output samples produced. Never writes past out_cap.
  size_t process(const float* in, size_t n_in, float* out, size_t out_cap) noexcept;
  void   reset() noexcept;
private:
  double ratio_ = 1.0;
  double pos_   = 0.0;      // fractional position in the incoming stream, relative to start of current `in`
  float  h_[3]  = {0,0,0};  // last 3 input samples of the previous block: x[-3], x[-2], x[-1]
  bool   primed_ = false;
};
}
