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
