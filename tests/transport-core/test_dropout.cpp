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
