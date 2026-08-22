#pragma once
#include <cstddef>
#include <cstdint>
namespace vc {
// Fixed-bucket histogram of per-hop compute times, for RT-safe frame-time
// instrumentation on the live relay thread. record() is O(1) and allocation-
// free (a plain array of counts), so it is safe to call from the MMCSS relay
// thread. The relay thread is the SOLE writer; the readers (count/max/mean/
// percentile/fractionOver) must run ONLY after the writer has stopped — i.e.
// after the relay thread is joined — so no atomics or locks are needed.
//
// Buckets are BUCKET_US wide (25 us) from 0 up to CEILING_US (20 ms), giving
// NUM_BUCKETS = 800 buckets, plus one overflow bucket for samples >= ceiling.
// Exact running max and sum are tracked separately (doubles) so maxMs() and
// meanMs() are exact, not bucket-quantized. percentileMs() is nearest-rank on
// the bucketed CDF and is therefore accurate to +/- one bucket width (25 us);
// the overflow bucket is reported at the ceiling (a lower bound). fractionOver
// is exact when the threshold is a multiple of the bucket width, otherwise
// accurate to +/- one bucket width.
class FrameTimeStats {
public:
  static constexpr double BUCKET_US   = 25.0;
  static constexpr double CEILING_US  = 20000.0;             // 20 ms
  static constexpr size_t NUM_BUCKETS = 800;                 // CEILING_US / BUCKET_US
  static constexpr size_t OVERFLOW_IDX = NUM_BUCKETS;        // index of overflow bucket

  // RT hot path: inline, allocation-free, no branches into anything heavy.
  void record(double ms) noexcept {
    const double us = ms * 1000.0;
    size_t idx = OVERFLOW_IDX;
    if (us < CEILING_US) {
      const long b = static_cast<long>(us / BUCKET_US);
      idx = (b < 0) ? 0 : static_cast<size_t>(b);           // clamp stray negatives to bucket 0
    }
    ++buckets_[idx];
    ++count_;
    sumMs_ += ms;
    if (ms > maxMs_) maxMs_ = ms;
  }

  void reset() noexcept {   // clear the histogram (call from the writer thread only)
    for (size_t i = 0; i <= NUM_BUCKETS; ++i) buckets_[i] = 0;
    count_ = 0; sumMs_ = 0.0; maxMs_ = 0.0;
  }

  uint64_t count()  const noexcept { return count_; }
  double   maxMs()  const noexcept { return count_ ? maxMs_ : 0.0; }
  double   meanMs() const noexcept { return count_ ? sumMs_ / static_cast<double>(count_) : 0.0; }

  double percentileMs(double p) const noexcept;      // p in [0,100], nearest-rank on bucketed CDF
  double fractionOverMs(double thresholdMs) const noexcept;  // share of samples >= threshold

private:
  static double bucketCenterMs(size_t i) noexcept {
    if (i >= NUM_BUCKETS) return CEILING_US / 1000.0;        // overflow -> ceiling (lower bound)
    return (static_cast<double>(i) * BUCKET_US + BUCKET_US * 0.5) / 1000.0;
  }
  uint64_t buckets_[NUM_BUCKETS + 1] = {};                  // +1 for the overflow bucket
  uint64_t count_ = 0;
  double   sumMs_ = 0.0;
  double   maxMs_ = 0.0;
};
}
