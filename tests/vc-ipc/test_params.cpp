#include <gtest/gtest.h>
#include <type_traits>
#include "params.h"
#include "telemetry.h"
using vc::ipc::Params; using vc::ipc::Telemetry;

TEST(Params, IsTriviallyCopyableStandardLayout) {
  static_assert(std::is_trivially_copyable_v<Params>);
  static_assert(std::is_standard_layout_v<Params>);
  SUCCEED();
}
TEST(Telemetry, IsTriviallyCopyableStandardLayout) {
  static_assert(std::is_trivially_copyable_v<Telemetry>);
  static_assert(std::is_standard_layout_v<Telemetry>);
  SUCCEED();
}
TEST(Params, DefaultsMatchTodaysDspChain) {
  Params p = Params::defaults();
  EXPECT_FLOAT_EQ(p.hpfCutoffHz, 80.0f);
  EXPECT_FLOAT_EQ(p.agcTargetDbfs, -15.0f);
  EXPECT_FLOAT_EQ(p.limiterCeilingDbfs, -1.0f);
  EXPECT_EQ(p.engine, 1u);
  EXPECT_EQ(p.bypass, 0u);
  EXPECT_EQ(p.dspChain, 1u);
  EXPECT_EQ(p.driftComp, 1u);
}
