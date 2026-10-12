#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include "input_level_normalizer.h"
using namespace vc;

namespace {
constexpr float kPi = 3.14159265358979f;
// A 480-sample frame of a sine with the given RMS in dBFS.
void sineFrame(float rmsDbfs, float* f, int phase) {
  const float amp = std::pow(10.0f, rmsDbfs / 20.0f) * std::sqrt(2.0f);
  for (int i = 0; i < 480; ++i) f[i] = amp * std::sin(2.0f * kPi * 1000.0f * (phase * 480 + i) / 48000.0f);
}
float runSteady(InputLevelNormalizer& n, float rmsDbfs, int frames, int startFrame = 0) {
  float f[480], g = 0;
  for (int k = 0; k < frames; ++k) { sineFrame(rmsDbfs, f, startFrame + k); g = n.process(f); }
  return g;
}
}  // namespace

TEST(InputLevelNormalizer, SteadyInputConvergesToTargetMinusLevel) {
  InputLevelNormalizer n;
  const float T = n.config().targetDbfs;
  const float g = runSteady(n, T - 12.0f, 1000);
  EXPECT_NEAR(g, 12.0f, 0.5f);
}

TEST(InputLevelNormalizer, NeverAttenuates) {
  InputLevelNormalizer n;
  const float T = n.config().targetDbfs;
  float f[480];
  for (int k = 0; k < 600; ++k) {
    sineFrame(T + 15.0f, f, k);
    EXPECT_GE(n.process(f), 0.0f);
  }
}

TEST(InputLevelNormalizer, CappedAtMaxGain) {
  InputLevelNormalizer n;
  const float g = runSteady(n, -75.0f, 3000);   // far below T - 30, still above the silence gate
  EXPECT_NEAR(g, n.config().maxGainDb, 0.01f);
}

TEST(InputLevelNormalizer, SparseClicksDoNotMoveGain) {
  InputLevelNormalizer n;
  const float T = n.config().targetDbfs;
  const float quiet = T - 20.0f;
  float f[480];
  for (int k = 0; k < 500; ++k) { sineFrame(quiet, f, k); n.process(f); }
  const float before = n.process(f);
  float lo = 1e9f, hi = -1e9f;
  for (int k = 0; k < 600; ++k) {
    const bool click = (k % 6) == 0;                 // ~17% of frames, 35 dB louder
    sineFrame(click ? quiet + 35.0f : quiet, f, k);
    const float g = n.process(f);
    lo = std::min(lo, g); hi = std::max(hi, g);
  }
  EXPECT_LT(std::fabs(hi - before), 1.0f);
  EXPECT_LT(std::fabs(lo - before), 1.0f);
}

TEST(InputLevelNormalizer, DigitalSilenceHoldsGain) {
  InputLevelNormalizer n;
  const float T = n.config().targetDbfs;
  const float g = runSteady(n, T - 10.0f, 1000);
  float zero[480] = {0};
  for (int k = 0; k < 2000; ++k) EXPECT_NEAR(n.process(zero), g, 1e-4f);
  // Sub-gate noise (-90 dBFS) is also ignored.
  float f[480];
  for (int k = 0; k < 500; ++k) { sineFrame(-90.0f, f, k); EXPECT_NEAR(n.process(f), g, 1e-4f); }
}

TEST(InputLevelNormalizer, SlewFastThenSlow) {
  InputLevelNormalizer n;
  const float T = n.config().targetDbfs;
  const float perFrameFast = n.config().fastSlewDbPerSec * 0.01f;
  const float perFrameSlow = n.config().slewDbPerSec * 0.01f;
  float f[480], prev = 0;
  bool reachedFast = false;
  for (int k = 0; k < 200; ++k) {            // first 2 s: <= 30 dB/s, and actually uses it
    sineFrame(T - 25.0f, f, k);
    const float g = n.process(f);
    EXPECT_LE(g - prev, perFrameFast + 1e-4f);
    if (g - prev > perFrameSlow * 2) reachedFast = true;
    prev = g;
  }
  EXPECT_TRUE(reachedFast);
  EXPECT_NEAR(prev, 25.0f, 0.5f);
  // After 2 s a level drop of 15 dB must be tracked at <= 3 dB/s.
  for (int k = 200; k < 800; ++k) {
    sineFrame(T - 40.0f, f, k);
    const float g = n.process(f);
    EXPECT_LE(g - prev, perFrameSlow + 1e-4f);
    EXPECT_GE(g - prev, -perFrameSlow - 1e-4f);
    prev = g;
  }
  EXPECT_GT(prev, 26.0f);                     // is moving up
}

TEST(InputLevelNormalizer, SlowPhaseTakesAboutFiveSecondsFor15dB) {
  InputLevelNormalizer n;
  const float T = n.config().targetDbfs;
  runSteady(n, T - 5.0f, 300);                        // settle at +5 dB after the fast phase
  float f[480], g = 0;
  int frames = 0;
  while (g < 19.5f && frames < 3000) { sineFrame(T - 20.0f, f, frames); g = n.process(f); ++frames; }
  EXPECT_GE(frames, 450);                             // 15 dB at 3 dB/s ~= 5 s, not instant
}
