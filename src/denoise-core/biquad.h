#pragma once
#include <cstddef>
namespace vc {
// Direct-Form-I biquad. Factory builds an RBJ high-pass (Butterworth default Q).
class Biquad {
public:
  static Biquad highpass(double fs, double fc, double Q = 0.70710678);
  void process(const float* in, float* out, size_t n) noexcept;   // may alias
  void reset() noexcept { x1_=x2_=y1_=y2_=0.0f; }
private:
  float b0_=1,b1_=0,b2_=0,a1_=0,a2_=0;
  float x1_=0,x2_=0,y1_=0,y2_=0;
};
}
