#include "frame_time_stats.h"
#include <cmath>
namespace vc {

double FrameTimeStats::percentileMs(double p) const noexcept {
  if (count_ == 0) return 0.0;
  if (p < 0.0)   p = 0.0;
  if (p > 100.0) p = 100.0;
  // Nearest-rank: smallest 1-indexed rank r with cumulative count >= r.
  uint64_t rank = static_cast<uint64_t>(std::ceil((p / 100.0) * static_cast<double>(count_)));
  if (rank < 1)      rank = 1;
  if (rank > count_) rank = count_;
  uint64_t cum = 0;
  for (size_t i = 0; i <= OVERFLOW_IDX; ++i) {
    cum += buckets_[i];
    if (cum >= rank) return bucketCenterMs(i);
  }
  return maxMs();   // unreachable while count_ > 0
}

double FrameTimeStats::fractionOverMs(double thresholdMs) const noexcept {
  if (count_ == 0) return 0.0;
  const double us = thresholdMs * 1000.0;
  size_t first = OVERFLOW_IDX;
  if (us < CEILING_US) {
    const long b = static_cast<long>(us / BUCKET_US);
    first = (b < 0) ? 0 : static_cast<size_t>(b);
  }
  uint64_t over = 0;
  for (size_t i = first; i <= OVERFLOW_IDX; ++i) over += buckets_[i];
  return static_cast<double>(over) / static_cast<double>(count_);
}

}
