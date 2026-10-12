#include <gtest/gtest.h>
#include "slider_table.h"
using namespace vc::ui;

TEST(SliderTable, IndexedById) {
  for (int i = 0; i < kSliderCount; ++i) EXPECT_EQ((int)kSliderSpecs[i].id, i);
}

TEST(SliderTable, EverySliderIsInExactlyOneGroup) {
  int seen[kSliderCount] = {};
  for (const auto& g : kSliderGroups) {
    ASSERT_LE(g.count, (int)g.ids.size());
    for (int k = 0; k < g.count; ++k) ++seen[(int)g.ids[k]];
  }
  for (int i = 0; i < kSliderCount; ++i) EXPECT_EQ(seen[i], 1) << "slider " << i;
}

TEST(SliderTable, DefaultsMatchProfileAndWorkerDefaults) {
  // A fresh mic profile, the worker's startup Params and the slider default must agree, or a new
  // mic would show one value and run another.
  const vc::MicProfile m{};
  const vc::ipc::Params p = vc::ipc::Params::defaults();
  for (const auto& s : kSliderSpecs) {
    SCOPED_TRACE((int)s.id);
    EXPECT_LT(s.lo, s.hi);
    EXPECT_GE(s.def, s.lo);
    EXPECT_LE(s.def, s.hi);
    EXPECT_FLOAT_EQ(m.*s.profileField, s.def);
    EXPECT_FLOAT_EQ(p.*s.paramField, s.def);
  }
}

TEST(SliderTable, ApplySetsTheMatchingParam) {
  for (const auto& s : kSliderSpecs) {
    SCOPED_TRACE((int)s.id);
    UiState ui;
    const float v = s.lo + 0.37f * (s.hi - s.lo);
    const vc::ipc::Params out = (ui.*s.apply)(v);
    EXPECT_FLOAT_EQ(out.*s.paramField, v);
  }
}

TEST(SliderTable, OnlyAggressionIsDfn3Only) {
  for (const auto& s : kSliderSpecs) EXPECT_EQ(s.dfn3Only, s.id == SliderId::Aggression);
}
