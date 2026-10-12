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
TEST(Config, RejectsOldFlatFormat) {
  auto p = write_tmp(
    "capture_device_id = \"cap-1\"\n"
    "render_device_id  = \"ren-1\"\n");
  EXPECT_THROW(Config::load(p), std::runtime_error);
}
TEST(Config, CsvLogIsOffByDefault) {
  EXPECT_TRUE(Config{}.csvPath.empty());
}
TEST(Config, CliOverridesTakePrecedence) {
  Config c;
  c.captureDeviceId = "from-file";
  const char* argv[] = {"app", "--capture", "from-cli", "--csv", "x.csv"};
  c.apply_cli(5, const_cast<char**>(argv));
  EXPECT_EQ(c.captureDeviceId, "from-cli");
  EXPECT_EQ(c.csvPath, "x.csv");
}
TEST(Config, ClampsRingToLatencyBounds) {
  Config c; c.ringFrames = 2;
  c.validate();
  EXPECT_EQ(c.ringFrames, vc::kRingFramesMin);
  c.ringFrames = 100000;
  c.validate();
  EXPECT_EQ(c.ringFrames, vc::kRingFramesMax);
}
TEST(Config, ThrowsOnMalformedToml) {
  auto p = write_tmp("this is = = not valid ][ toml\n");
  EXPECT_THROW(Config::load(p), std::runtime_error);
}
TEST(Config, ClampsNegativeRingFromCli) {
  Config c;
  const char* argv[] = {"app", "--ring", "-5"};
  c.apply_cli(3, const_cast<char**>(argv));
  EXPECT_EQ(c.ringFrames, vc::kRingFramesMin);
}
TEST(Config, EngineCliFlagSelectsRnnoiseOrNone) {
  vc::Config c;
  const char* a1[] = {"app", "--engine", "rnnoise"};
  c.apply_cli(3, const_cast<char**>(a1));
  EXPECT_EQ(c.engine, "rnnoise");
  const char* a2[] = {"app", "--engine", "none"};
  c.apply_cli(3, const_cast<char**>(a2));
  EXPECT_EQ(c.engine, "none");
  const char* a3[] = {"app", "--engine", "bogus"};   // unknown -> RNNoise
  c.apply_cli(3, const_cast<char**>(a3));
  EXPECT_EQ(c.engine, "rnnoise");
}
TEST(Config, ApplyCliHandlesValuelessFlagBeforeDevicePairs) {
  const char* argv[] = {"exe", "--list-devices", "--capture", "CAP", "--render", "REN"};
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
    "engine = \"dfn3\"\n"   // legacy key from an old engine: ignored
    "dsp_chain = true\n");
  Config c = Config::load(p);
  EXPECT_EQ(c.captureDeviceId, "cap-1");
  EXPECT_EQ(c.renderDeviceId, "ren-1");
  EXPECT_EQ(c.ringFrames, 16u);
  EXPECT_EQ(c.engine, "rnnoise");   // profile resolves to RNNoise regardless of legacy keys
  EXPECT_TRUE(c.dspChain);
}
