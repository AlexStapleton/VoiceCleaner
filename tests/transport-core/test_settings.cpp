#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include "settings.h"
using vc::Settings;

static std::string writeTmp(const char* body) {
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings.toml").string();
  std::ofstream(p) << body;
  return p;
}

TEST(Settings, LoadsNewSchemaAndResolvesActive) {
  auto p = writeTmp(
    "[global]\n"
    "active_capture_id = \"cap-1\"\n"
    "render_id = \"ren-1\"\n"
    "ring_frames = 16\n"
    "[mic.\"cap-1\"]\n"
    "name = \"K688\"\n"
    "engine = \"dfn3\"\n"
    "warm_standby = true\n"
    "dsp_chain = true\n");
  Settings s = Settings::load(p);
  EXPECT_EQ(s.activeCaptureId, "cap-1");
  EXPECT_EQ(s.renderId, "ren-1");
  EXPECT_EQ(s.ringFrames, 16u);
  ASSERT_EQ(s.mics.size(), 1u);
  EXPECT_EQ(s.mics[0].name, "K688");
  vc::Config c = s.resolveActive();
  EXPECT_EQ(c.captureDeviceId, "cap-1");
  EXPECT_EQ(c.renderDeviceId, "ren-1");
  EXPECT_EQ(c.ringFrames, 16u);
  EXPECT_EQ(c.engine, "dfn3");
  EXPECT_TRUE(c.warmStandby);
  EXPECT_TRUE(c.dspChain);
}

TEST(Settings, FlatFileBackCompatResolves) {
  auto p = writeTmp(
    "capture_device_id = \"cap-9\"\n"
    "render_device_id = \"ren-9\"\n"
    "ring_frames = 16\n"
    "engine = \"rnnoise\"\n"
    "dsp_chain = true\n");
  Settings s = Settings::load(p);
  EXPECT_EQ(s.activeCaptureId, "cap-9");
  EXPECT_EQ(s.renderId, "ren-9");
  ASSERT_EQ(s.mics.size(), 1u);
  EXPECT_EQ(s.mics[0].id, "cap-9");
  EXPECT_EQ(s.mics[0].engine, "rnnoise");
  vc::Config c = s.resolveActive();
  EXPECT_EQ(c.captureDeviceId, "cap-9");
  EXPECT_EQ(c.engine, "rnnoise");
}

TEST(Settings, MissingFileIsEmpty) {
  Settings s = Settings::load("Z:/does/not/exist_vc.toml");
  EXPECT_TRUE(s.activeCaptureId.empty());
  EXPECT_TRUE(s.mics.empty());
}

TEST(Settings, ResolveWithNoActiveProfileUsesDefaults) {
  Settings s;  // empty
  vc::Config c = s.resolveActive();
  EXPECT_TRUE(c.captureDeviceId.empty());
  EXPECT_EQ(c.ringFrames, 16u);   // Settings default ring
}

TEST(Settings, EnsureProfileCreatesDefaultThenReuses) {
  Settings s;
  vc::MicProfile& a = s.ensureProfile("cap-1", "K688");
  EXPECT_EQ(a.engine, "rnnoise");
  EXPECT_TRUE(a.dspChain);
  EXPECT_EQ(s.mics.size(), 1u);
  vc::MicProfile& b = s.ensureProfile("cap-1", "K688");  // same id -> no new entry
  EXPECT_EQ(s.mics.size(), 1u);
  EXPECT_EQ(&a, &b);
}

TEST(Settings, ActiveProfileFollowsActiveId) {
  Settings s;
  s.ensureProfile("cap-1", "A");
  s.ensureProfile("cap-2", "B");
  s.activeCaptureId = "cap-2";
  ASSERT_NE(s.activeProfile(), nullptr);
  EXPECT_EQ(s.activeProfile()->name, "B");
  s.activeCaptureId = "cap-none";
  EXPECT_EQ(s.activeProfile(), nullptr);
}

TEST(Settings, TuningFieldsRoundTrip) {
  Settings s;
  s.activeCaptureId = "cap-1"; s.renderId = "ren-1";
  vc::MicProfile& m = s.ensureProfile("cap-1", "K688");
  m.aggression = 0.65f; m.hpfHz = 90.0f; m.outputGainDbfs = -18.0f; m.limiterDbfs = -2.0f;
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings_tuning.toml").string();
  s.save(p);
  Settings r = Settings::load(p);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_FLOAT_EQ(r.mics[0].aggression, 0.65f);
  EXPECT_FLOAT_EQ(r.mics[0].hpfHz, 90.0f);
  EXPECT_FLOAT_EQ(r.mics[0].outputGainDbfs, -18.0f);
  EXPECT_FLOAT_EQ(r.mics[0].limiterDbfs, -2.0f);
}
TEST(Settings, OlderProfileWithoutTuningLoadsDefaults) {
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings_old.toml").string();
  std::ofstream(p) <<
    "[global]\nactive_capture_id=\"c1\"\nrender_id=\"r1\"\nring_frames=16\n"
    "[mic.\"c1\"]\nname=\"Mic\"\nengine=\"dfn3\"\n";
  Settings r = Settings::load(p);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_FLOAT_EQ(r.mics[0].aggression, 0.0f);
  EXPECT_FLOAT_EQ(r.mics[0].hpfHz, 80.0f);
  EXPECT_FLOAT_EQ(r.mics[0].outputGainDbfs, -20.0f);
  EXPECT_FLOAT_EQ(r.mics[0].limiterDbfs, -1.0f);
}
TEST(Settings, SaveLoadRoundTripIsIdentity) {
  Settings s;
  s.activeCaptureId = "cap-1"; s.renderId = "ren-1"; s.ringFrames = 16;
  vc::MicProfile& m = s.ensureProfile("cap-1", "K688");
  m.engine = "dfn3"; m.warmStandby = true; m.dspChain = true;
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings_rt.toml").string();
  s.save(p);
  Settings r = Settings::load(p);
  EXPECT_EQ(r.activeCaptureId, "cap-1");
  EXPECT_EQ(r.renderId, "ren-1");
  EXPECT_EQ(r.ringFrames, 16u);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_EQ(r.mics[0].id, "cap-1");
  EXPECT_EQ(r.mics[0].name, "K688");
  EXPECT_EQ(r.mics[0].engine, "dfn3");
  EXPECT_TRUE(r.mics[0].warmStandby);
}
