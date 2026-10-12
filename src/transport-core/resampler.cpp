#include "resampler.h"
#include <cmath>
namespace vc {

void   Resampler::set_ratio(double ratio) noexcept { ratio_ = ratio; }

size_t Resampler::process(const float* in, size_t n_in, float* out, size_t out_cap) noexcept {
  if (n_in < 3) return 0;

  // Virtual indexable stream: s(i) reads history for i<0, the input block otherwise.
  auto s = [&](long i) -> double {
    if (i < 0) return static_cast<double>(h_[i + 3]);      // i in {-3,-2,-1}
    return static_cast<double>(in[static_cast<size_t>(i)]);
  };

  const double step = 1.0 / ratio_;   // input advance per output sample
  size_t produced = 0;

  // Produce while the highest-indexed tap (k+2) stays within this input block.
  while (produced < out_cap) {
    const long k = static_cast<long>(std::floor(pos_));
    if (k + 2 > static_cast<long>(n_in) - 1) break;
    const double t = pos_ - static_cast<double>(k);

    const double p0 = s(k - 1);
    const double p1 = s(k);
    const double p2 = s(k + 1);
    const double p3 = s(k + 2);

    const double y = 0.5 * ((2.0 * p1)
        + (-p0 + p2) * t
        + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t
        + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);

    out[produced++] = static_cast<float>(y);
    pos_ += step;
  }

  // Carry the last 3 input samples as history for the next block.
  h_[0] = in[n_in - 3];
  h_[1] = in[n_in - 2];
  h_[2] = in[n_in - 1];
  pos_ -= static_cast<double>(n_in);

  return produced;
}

void   Resampler::reset() noexcept {
  ratio_ = 1.0; pos_ = 0.0; h_[0] = h_[1] = h_[2] = 0.0f;
}

}
