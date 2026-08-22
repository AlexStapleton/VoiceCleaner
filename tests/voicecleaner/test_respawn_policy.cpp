#include <gtest/gtest.h>
#include "respawn_policy.h"
using vc::ui::RespawnPolicy;
using Action = vc::ui::RespawnPolicy::Action;

TEST(RespawnPolicy, HealthyReturnsNone) {
  RespawnPolicy p;
  EXPECT_EQ(p.onTick(true, true, true, 1000), Action::None);
  EXPECT_FALSE(p.reconnecting());
}

TEST(RespawnPolicy, LaunchFailureGivesUpNoRespawn) {
  RespawnPolicy p;
  EXPECT_EQ(p.onTick(false, false, /*everReady*/false, 1000), Action::GiveUp);
  // Never respawns on subsequent ticks.
  for (uint64_t t = 1100; t < 20000; t += 100)
    EXPECT_NE(p.onTick(false, false, false, t), Action::Respawn);
}

TEST(RespawnPolicy, DeathAfterReadySchedulesThenRespawns) {
  RespawnPolicy p;
  EXPECT_EQ(p.onTick(false, false, /*everReady*/true, 1000), Action::None);   // schedules +500
  EXPECT_TRUE(p.reconnecting());
  EXPECT_EQ(p.onTick(false, false, true, 1400), Action::None);                // still waiting
  EXPECT_EQ(p.onTick(false, false, true, 1500), Action::Respawn);             // backoff elapsed
  EXPECT_EQ(p.attempts(), 1);
}

TEST(RespawnPolicy, BackoffDoublesAndCapsThenGivesUp) {
  RespawnPolicy p;   // maxAttempts=5, base=500, cap=8000 -> 500,1000,2000,4000,8000
  const uint64_t expected[5] = {500, 1000, 2000, 4000, 8000};
  uint64_t now = 0;
  bool everReady = true;   // first death is after-ready; latch carries the rest
  for (int a = 0; a < 5; ++a) {
    EXPECT_EQ(p.onTick(false, false, everReady, now), Action::None);   // schedules now+expected[a]
    everReady = false;                                                 // respawned worker never got ready
    EXPECT_EQ(p.onTick(false, false, everReady, now + expected[a] - 1), Action::None);  // just before
    EXPECT_EQ(p.onTick(false, false, everReady, now + expected[a]), Action::Respawn);   // fires
    EXPECT_EQ(p.attempts(), a + 1);
    now += expected[a] + 1;
  }
  EXPECT_EQ(p.onTick(false, false, false, now), Action::GiveUp);   // 6th death -> cap hit
  EXPECT_FALSE(p.reconnecting());
}

TEST(RespawnPolicy, ReadyAgainResetsEpisode) {
  RespawnPolicy p;
  p.onTick(false, false, true, 1000);                       // schedule
  EXPECT_EQ(p.onTick(false, false, true, 1500), Action::Respawn);
  EXPECT_EQ(p.attempts(), 1);
  EXPECT_EQ(p.onTick(true, true, true, 1600), Action::None); // respawn became ready
  EXPECT_EQ(p.attempts(), 0);
  EXPECT_FALSE(p.reconnecting());
  // A fresh death restarts backoff at the base delay.
  EXPECT_EQ(p.onTick(false, false, true, 5000), Action::None);
  EXPECT_EQ(p.onTick(false, false, true, 5500), Action::Respawn);
}

TEST(RespawnPolicy, ResetClearsEpisode) {
  RespawnPolicy p;
  p.onTick(false, false, true, 1000);
  EXPECT_TRUE(p.reconnecting());
  p.reset();
  EXPECT_FALSE(p.reconnecting());
  EXPECT_EQ(p.attempts(), 0);
}

TEST(RespawnPolicy, LatchCountsPreReadyDeaths) {
  RespawnPolicy p;
  // First death after-ready enters the episode and fires a respawn.
  p.onTick(false, false, /*everReady*/true, 0);
  EXPECT_EQ(p.onTick(false, false, true, 500), Action::Respawn);
  // The respawned worker dies BEFORE ready (everReady=false) — must keep reconnecting, not GiveUp.
  EXPECT_NE(p.onTick(false, false, /*everReady*/false, 600), Action::GiveUp);
  EXPECT_TRUE(p.reconnecting());
}
