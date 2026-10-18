#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// Look-ahead brickwall limiter. Delays the signal by `lookahead` samples and reduces gain ahead of
// peaks so no output sample exceeds the ceiling. Fast attack (reaches target within the lookahead),
// slow release.
class Limiter {
public:
  // ceilingDb e.g. -1.0; lookahead e.g. 144 samples (3 ms @48k); releaseMs e.g. 50; fs 48000.
  Limiter(float ceilingDb = -1.0f, size_t lookahead = 144, float releaseMs = 50.0f, float fs = 48000.0f);
  void process(const float* in, float* out, size_t n) noexcept;
  void setCeilingDb(float db) noexcept;
  void reset() noexcept;
private:
  float ceiling_; size_t L_; float relCoef_;
  std::vector<float> delay_; size_t widx_ = 0;
  float gain_ = 1.0f;
  // Sliding-window peak over the last L_+1 input magnitudes (the delay line plus the incoming
  // sample), kept as a monotonic queue: magnitudes strictly decrease from front to back, so the
  // front is the window peak. Each sample is pushed and popped at most once -> O(1) amortized
  // per sample instead of an O(L_) scan. Fixed ring of L_+1 slots, allocated in the constructor.
  std::vector<float>  qMag_;
  std::vector<size_t> qPos_;      // stream position of each queued magnitude (for expiry)
  size_t qHead_ = 0, qCount_ = 0;
  size_t pos_ = 0;                // stream position of the next input sample
};
}
