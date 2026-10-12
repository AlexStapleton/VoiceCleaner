#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>
#include "pre_gain_stage.h"
using namespace vc;

namespace {
// Identity model with a fixed delay of D samples (like the DFN3 streaming engine, 1440).
struct DelayModel {
  DelayLine dl;
  explicit DelayModel(size_t d) : dl(d) {}
  void operator()(const float* in, float* out) { for (size_t i = 0; i < FRAME_SAMPLES; ++i) out[i] = dl.push(in[i]); }
};
}  // namespace

TEST(PreGainStage, IdentityModelUnityGainIsDelayedInput) {
  constexpr size_t D = 1440;
  PreGainStage pg(D);
  DelayModel m(D);
  std::mt19937 rng(1);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  std::vector<float> in, out;
  for (int k = 0; k < 20; ++k) {
    float a[480], b[480];
    for (auto& s : a) s = u(rng);
    pg.process(a, b, 1.0f, [&](const float* x, float* y) { m(x, y); });
    in.insert(in.end(), a, a + 480); out.insert(out.end(), b, b + 480);
  }
  for (size_t i = D; i < out.size(); ++i) EXPECT_NEAR(out[i], in[i - D], 1e-7f);
}

TEST(PreGainStage, GainChangingEveryFrameIsUndoneExactly) {
  constexpr size_t D = 1440;
  PreGainStage pg(D);
  DelayModel m(D);
  std::mt19937 rng(2);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  std::uniform_real_distribution<float> gdb(0.0f, 30.0f);
  std::vector<float> in, out, modelIn;
  float maxModelIn = 0;
  for (int k = 0; k < 60; ++k) {
    float a[480], b[480];
    for (auto& s : a) s = u(rng);
    const float g = std::pow(10.0f, gdb(rng) / 20.0f);
    pg.process(a, b, g, [&](const float* x, float* y) {
      for (int i = 0; i < 480; ++i) maxModelIn = std::max(maxModelIn, std::fabs(x[i]));
      m(x, y);
    });
    in.insert(in.end(), a, a + 480); out.insert(out.end(), b, b + 480);
  }
  EXPECT_GT(maxModelIn, 2.0f);   // the model really did see boosted input
  for (size_t i = D; i < out.size(); ++i)
    EXPECT_NEAR(out[i], in[i - D], 1e-5f) << "i=" << i;
}

TEST(PreGainStage, ModelSeesPerSampleRampBetweenFrameGains) {
  PreGainStage pg(0);
  float ones[480], out[480];
  for (auto& s : ones) s = 1.0f;
  pg.process(ones, out, 1.0f, [](const float* x, float* y) { for (int i = 0; i < 480; ++i) y[i] = x[i]; });
  std::vector<float> seen;
  pg.process(ones, out, 4.0f, [&](const float* x, float* y) {
    seen.assign(x, x + 480);
    for (int i = 0; i < 480; ++i) y[i] = x[i];
  });
  EXPECT_NEAR(seen[0], 1.0f + 3.0f / 480.0f, 1e-5f);
  EXPECT_NEAR(seen[479], 4.0f, 1e-5f);
  for (int i = 1; i < 480; ++i) EXPECT_GT(seen[i], seen[i - 1]);
  for (int i = 0; i < 480; ++i) EXPECT_NEAR(out[i], 1.0f, 1e-5f);   // undone
}
