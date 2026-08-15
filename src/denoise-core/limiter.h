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
};
}
