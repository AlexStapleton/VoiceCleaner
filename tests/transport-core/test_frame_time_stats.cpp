#include <gtest/gtest.h>
#include "frame_time_stats.h"
using vc::FrameTimeStats;

// Known sample set: 100 hops @ 2 ms, 5 @ 8 ms, 1 @ 15 ms  (106 total).
// mean = (100*2 + 5*8 + 15) / 106 = 255/106 = 2.40566...
static FrameTimeStats makeKnown() {
  FrameTimeStats s;
  for (int i = 0; i < 100; ++i) s.record(2.0);
  for (int i = 0; i < 5;   ++i) s.record(8.0);
  s.record(15.0);
  return s;
}

TEST(FrameTimeStats, EmptyIsZero) {
  FrameTimeStats s;
  EXPECT_EQ(s.count(), 0u);
  EXPECT_DOUBLE_EQ(s.maxMs(), 0.0);
  EXPECT_DOUBLE_EQ(s.meanMs(), 0.0);
  EXPECT_DOUBLE_EQ(s.percentileMs(99.9), 0.0);
  EXPECT_DOUBLE_EQ(s.fractionOverMs(10.0), 0.0);
}

TEST(FrameTimeStats, Count) {
  auto s = makeKnown();
  EXPECT_EQ(s.count(), 106u);
}

TEST(FrameTimeStats, MeanExact) {
  auto s = makeKnown();
  EXPECT_NEAR(s.meanMs(), 255.0 / 106.0, 1e-9);   // exact running sum, not bucketed
}

TEST(FrameTimeStats, MaxExact) {
  auto s = makeKnown();
  EXPECT_DOUBLE_EQ(s.maxMs(), 15.0);              // exact running max, not bucketed
}

TEST(FrameTimeStats, Median) {
  auto s = makeKnown();
  // 50th percentile lands among the 2 ms samples; nearest-rank on a 25 us
  // bucketed CDF is accurate to within one bucket width (0.025 ms).
  EXPECT_NEAR(s.percentileMs(50.0), 2.0, 0.03);
}

TEST(FrameTimeStats, HighPercentile) {
  auto s = makeKnown();
  // 99.9th (rank 106 of 106) is the single 15 ms outlier.
  EXPECT_NEAR(s.percentileMs(99.9), 15.0, 0.03);
  // 99th (rank ceil(0.99*106)=105) still lands in the 8 ms group.
  EXPECT_NEAR(s.percentileMs(99.0), 8.0, 0.03);
}

TEST(FrameTimeStats, FractionOverBudget) {
  auto s = makeKnown();
  // Only the 15 ms hop is >= 10 ms.
  EXPECT_NEAR(s.fractionOverMs(10.0), 1.0 / 106.0, 1e-9);
  // All 106 are >= 1 ms.
  EXPECT_NEAR(s.fractionOverMs(1.0), 1.0, 1e-9);
  // The 8 ms group and the 15 ms outlier are >= 8 ms: 6 of 106.
  EXPECT_NEAR(s.fractionOverMs(8.0), 6.0 / 106.0, 1e-9);
}

TEST(FrameTimeStats, OverflowBucketCountsAndMaxExact) {
  FrameTimeStats s;
  s.record(2.0);
  s.record(25.0);   // above the 20 ms ceiling -> overflow bucket
  EXPECT_EQ(s.count(), 2u);
  EXPECT_DOUBLE_EQ(s.maxMs(), 25.0);            // exact even for overflow sample
  EXPECT_NEAR(s.fractionOverMs(10.0), 0.5, 1e-9);  // overflow sample counts as over-budget
  // High percentile resolves into the overflow bucket (reported as a lower bound
  // at the ceiling); it must be at least the ceiling and no more than the max.
  const double p = s.percentileMs(100.0);
  EXPECT_GE(p, 20.0);
  EXPECT_LE(p, 25.0);
}
