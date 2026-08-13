#include <gtest/gtest.h>
#include <fstream>
#include "config.h"
using vc::Config;
static std::string write_tmp(const char* body) {
  std::string p = "tmp_cfg.toml";
  std::ofstream(p) << body;
  return p;
}
TEST(Config, LoadsToml) {
  auto p = write_tmp(
    "capture_device_id = \"cap-1\"\n"
    "render_device_id  = \"ren-1\"\n"
    "ring_frames = 1024\n"
    "csv_path = \"drift.csv\"\n");
  Config c = Config::load(p);
  EXPECT_EQ(c.captureDeviceId, "cap-1");
  EXPECT_EQ(c.renderDeviceId, "ren-1");
  EXPECT_EQ(c.ringFrames, 1024u);
  EXPECT_EQ(c.csvPath, "drift.csv");
}
TEST(Config, CliOverridesTakePrecedence) {
  Config c;
  c.captureDeviceId = "from-file";
  const char* argv[] = {"app", "--capture", "from-cli", "--csv", "x.csv"};
  c.apply_cli(5, const_cast<char**>(argv));
  EXPECT_EQ(c.captureDeviceId, "from-cli");
  EXPECT_EQ(c.csvPath, "x.csv");
}
TEST(Config, EnforcesRingFloor) {
  Config c; c.ringFrames = 2;
  c.validate();
  EXPECT_GE(c.ringFrames, 3u);
}
