#include <gtest/gtest.h>
#include "param_outbox.h"
using vc::ui::ParamOutbox;

static vc::ipc::Params withHpf(float hz) { vc::ipc::Params p = vc::ipc::Params::defaults(); p.hpfCutoffHz = hz; return p; }

TEST(ParamOutbox, NothingDueUntilStaged) {
  ParamOutbox o(30);
  EXPECT_FALSE(o.dirty());
  EXPECT_FALSE(o.due(0, true));
}

TEST(ParamOutbox, FirstStagedValueIsDueImmediately) {
  ParamOutbox o(30);
  o.stage(withHpf(90.0f));
  EXPECT_TRUE(o.due(1000, /*force=*/false));
}

TEST(ParamOutbox, DragUpdatesAreThrottledButForceBypasses) {
  ParamOutbox o(30);
  o.stage(withHpf(90.0f));
  o.onAttempt(1000, true);
  o.stage(withHpf(95.0f));
  EXPECT_FALSE(o.due(1010, false));   // inside the throttle window
  EXPECT_TRUE(o.due(1010, true));     // drag end / click forces delivery
  EXPECT_TRUE(o.due(1030, false));    // window elapsed
}

// The original bug: the last drag value was lost if its send failed. It must stay pending.
TEST(ParamOutbox, FailedSendKeepsLatestValuePendingForRetry) {
  ParamOutbox o(30);
  o.stage(withHpf(90.0f));
  o.onAttempt(1000, true);
  o.stage(withHpf(140.0f));
  o.onAttempt(1040, /*delivered=*/false);
  EXPECT_TRUE(o.dirty());
  EXPECT_FLOAT_EQ(o.pending().hpfCutoffHz, 140.0f);
  EXPECT_TRUE(o.due(1100, false));    // the tick retries it
  o.onAttempt(1100, true);
  EXPECT_FALSE(o.dirty());
  EXPECT_FALSE(o.due(2000, true));    // delivered: nothing left to send
}

TEST(ParamOutbox, ThrottledValueIsSupersededNotQueued) {
  ParamOutbox o(30);
  o.stage(withHpf(90.0f));
  o.onAttempt(1000, true);
  o.stage(withHpf(100.0f));           // throttled, never sent
  o.stage(withHpf(110.0f));
  EXPECT_FLOAT_EQ(o.pending().hpfCutoffHz, 110.0f);   // only the newest snapshot is delivered
}
