#include <gtest/gtest.h>
#include "delay_line.h"
using namespace vc;

TEST(DelayLine, ZeroDelayIsPassthrough) {
  DelayLine d(0);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(d.push(static_cast<float>(i)), static_cast<float>(i));
  EXPECT_EQ(d.delay(), 0u);
}

TEST(DelayLine, DelaysByExactlyN) {
  for (size_t n : {1u, 2u, 7u, 480u}) {
    DelayLine d(n);
    for (size_t i = 0; i < 3 * n + 5; ++i) {
      const float y = d.push(static_cast<float>(i + 1));
      if (i < n) EXPECT_EQ(y, 0.0f) << "n=" << n << " i=" << i;
      else EXPECT_EQ(y, static_cast<float>(i - n + 1)) << "n=" << n << " i=" << i;
    }
  }
}

TEST(DelayLine, InitialFillValue) {
  DelayLine d(3, 1.0f);
  EXPECT_EQ(d.push(5.0f), 1.0f);
  EXPECT_EQ(d.push(6.0f), 1.0f);
  EXPECT_EQ(d.push(7.0f), 1.0f);
  EXPECT_EQ(d.push(8.0f), 5.0f);
}

TEST(DelayLine, ResetRestoresFill) {
  DelayLine d(4);
  for (int i = 0; i < 10; ++i) d.push(9.0f);
  d.reset();
  for (int i = 0; i < 4; ++i) EXPECT_EQ(d.push(3.0f), 0.0f);
  EXPECT_EQ(d.push(3.0f), 3.0f);
}
