#include <gtest/gtest.h>
#include "occupancy.h"
using vc::OccupancyStats;
TEST(Occupancy, TracksHighWater) {
  OccupancyStats s;
  s.sample(3); s.sample(7); s.sample(2);
  EXPECT_EQ(s.high_water(), 7u);
}
TEST(Occupancy, ResetClears) {
  OccupancyStats s;
  s.sample(5); s.reset();
  EXPECT_EQ(s.high_water(), 0u);
}
