#include "biquad.h"
#include <cmath>

namespace vc {

Biquad Biquad::highpass(double fs, double fc, double Q) {
  constexpr double kPi = 3.14159265358979323846;
  const double w0 = 2.0 * kPi * fc / fs;
  const double c = std::cos(w0);
  const double s = std::sin(w0);
  const double alpha = s / (2.0 * Q);

  const double b0 = (1.0 + c) / 2.0;
  const double b1 = -(1.0 + c);
  const double b2 = (1.0 + c) / 2.0;
  const double a0 = 1.0 + alpha;
  const double a1 = -2.0 * c;
  const double a2 = 1.0 - alpha;

  Biquad bq;
  bq.b0_ = static_cast<float>(b0 / a0);
  bq.b1_ = static_cast<float>(b1 / a0);
  bq.b2_ = static_cast<float>(b2 / a0);
  bq.a1_ = static_cast<float>(a1 / a0);
  bq.a2_ = static_cast<float>(a2 / a0);
  return bq;
}

void Biquad::setHighpass(double fs, double fc, double Q) noexcept {
  constexpr double kPi = 3.14159265358979323846;
  const double w0 = 2.0 * kPi * fc / fs;
  const double c = std::cos(w0);
  const double s = std::sin(w0);
  const double alpha = s / (2.0 * Q);
  const double b0 = (1.0 + c) / 2.0, b1 = -(1.0 + c), b2 = (1.0 + c) / 2.0;
  const double a0 = 1.0 + alpha, a1 = -2.0 * c, a2 = 1.0 - alpha;
  b0_ = static_cast<float>(b0 / a0);
  b1_ = static_cast<float>(b1 / a0);
  b2_ = static_cast<float>(b2 / a0);
  a1_ = static_cast<float>(a1 / a0);
  a2_ = static_cast<float>(a2 / a0);
}

void Biquad::process(const float* in, float* out, size_t n) noexcept {
  for (size_t i = 0; i < n; ++i) {
    float x = in[i];
    float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    out[i] = y;
  }
}

}  // namespace vc
