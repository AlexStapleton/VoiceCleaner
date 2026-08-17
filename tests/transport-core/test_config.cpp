#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include "config.h"
using vc::Config;
static std::string write_tmp(const char* body) {
  auto dir = std::filesystem::temp_directory_path();
  std::string p = (dir / "vc_tmp_cfg.toml").string();
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
TEST(Config, ThrowsOnMalformedToml) {
  auto p = write_tmp("this is = = not valid ][ toml\n");
  EXPECT_THROW(Config::load(p), std::runtime_error);
}
TEST(Config, ClampsNegativeRingFromCli) {
  Config c;
  const char* argv[] = {"app", "--ring", "-5"};
  c.apply_cli(3, const_cast<char**>(argv));
  EXPECT_GE(c.ringFrames, 3u);
  EXPECT_LE(c.ringFrames, 1048576u);
}
TEST(Config, ApplyCliHandlesValuelessFlagBeforeDevicePairs) {
  const char* argv[] = {"exe", "--dev-keys", "--capture", "CAP", "--render", "REN"};
  vc::Config c;
  c.apply_cli(6, const_cast<char**>(argv));
  EXPECT_EQ(c.captureDeviceId, "CAP");
  EXPECT_EQ(c.renderDeviceId, "REN");
}
TEST(Config, LoadsNewSchemaViaSettings) {
  auto p = write_tmp(
    "[global]\n"
    "active_capture_id = \"cap-1\"\n"
    "render_id = \"ren-1\"\n"
    "ring_frames = 16\n"
    "[mic.\"cap-1\"]\n"
    "engine = \"dfn3\"\n"
    "dsp_chain = true\n");
  Config c = Config::load(p);
  EXPECT_EQ(c.captureDeviceId, "cap-1");
  EXPECT_EQ(c.renderDeviceId, "ren-1");
  EXPECT_EQ(c.ringFrames, 16u);
  EXPECT_EQ(c.engine, "dfn3");
  EXPECT_TRUE(c.dspChain);
}
