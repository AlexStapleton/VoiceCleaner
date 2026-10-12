#include <gtest/gtest.h>
#include "param_store.h"
using vc::ipc::ParamStore; using vc::ipc::Params;

TEST(ParamStore, SnapshotReturnsLastPublished) {
  ParamStore s;                                 // starts at Params::defaults()
  EXPECT_FLOAT_EQ(s.snapshot().hpfCutoffHz, 80.0f);
  Params p = Params::defaults(); p.hpfCutoffHz = 150.0f; p.bypass = 1;
  s.publish(p);
  EXPECT_FLOAT_EQ(s.snapshot().hpfCutoffHz, 150.0f);
  EXPECT_EQ(s.snapshot().bypass, 1u);
}
TEST(ParamStore, LastPublishWins) {
  ParamStore s;
  Params p = Params::defaults();
  for (uint32_t i = 1; i <= 100; ++i) { p.hpfCutoffHz = float(i); s.publish(p); }
  EXPECT_FLOAT_EQ(s.snapshot().hpfCutoffHz, 100.0f);
}
