#include <gtest/gtest.h>
#include "dropout.h"
using vc::DropoutCounter;
TEST(Dropout, CountsWhenAccounting) {
  DropoutCounter c;
  c.record_underrun(); c.record_underrun(); c.record_overrun();
  EXPECT_EQ(c.underruns(), 2u);
  EXPECT_EQ(c.overruns(), 1u);
}
TEST(Dropout, StopsCountingAfterShutdown) {
  DropoutCounter c;
  c.stop_accounting();
  c.record_underrun(); c.record_overrun();
  EXPECT_EQ(c.underruns(), 0u);
  EXPECT_EQ(c.overruns(), 0u);
}
TEST(Dropout, ResetZeroesCountsAndReenablesAccounting) {
  DropoutCounter c;
  c.record_underrun(); c.record_overrun(); c.record_overrun();
  c.reset();
  EXPECT_EQ(c.underruns(), 0u);
  EXPECT_EQ(c.overruns(), 0u);
  c.record_underrun();                 // accounting is live again after reset
  EXPECT_EQ(c.underruns(), 1u);
}
