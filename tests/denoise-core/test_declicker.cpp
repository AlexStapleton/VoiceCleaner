#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "declicker.h"
using vc::Declicker;

static Declicker make() { return Declicker(Declicker::Config{}); }

TEST(Declicker, ZeroStrengthIsPureDelay) {
  Declicker d = make();
  const size_t L = 144, n = 1000;
  std::vector<float> in(n), out(n);
  for (size_t i = 0; i < n; ++i) in[i] = std::sin(2*3.14159f*220.0f*i/48000.0f);
  d.process(in.data(), out.data(), n, /*strength*/0.0f, /*vad*/0.0f);
  for (size_t i = 0; i < L; ++i) EXPECT_FLOAT_EQ(out[i], 0.0f);           // pre-fill
  for (size_t i = L; i < n; ++i) EXPECT_NEAR(out[i], in[i - L], 1e-6f);   // exact delayed copy
}

TEST(Declicker, RemovesImpulseInSilence) {
  Declicker d = make();
  const size_t L = 144, n = 2000, P = 500;
  std::vector<float> in(n, 0.0f), out(n);
  in[P] = 1.0f;                                                            // a lone click
  d.process(in.data(), out.data(), n, /*strength*/1.0f, /*vad*/0.0f);
  EXPECT_LT(std::fabs(out[P + L]), 0.15f);                                 // click emerges heavily ducked
}

TEST(Declicker, PreservesSteadyToneAtFullStrength) {
  Declicker d = make();
  const size_t n = 6000;
  std::vector<float> in(n), out(n);
  for (size_t i = 0; i < n; ++i) in[i] = 0.3f * std::sin(2*3.14159f*300.0f*i/48000.0f);
  d.process(in.data(), out.data(), n, 1.0f, 0.0f);
  double rin = 0, rout = 0;                                               // steady-state RMS (skip onset)
  for (size_t i = 2000; i < n; ++i) { rin += in[i]*in[i]; rout += out[i]*out[i]; }
  EXPECT_NEAR(std::sqrt(rout), std::sqrt(rin), 0.1 * std::sqrt(rin));      // tone not falsely ducked
}

TEST(Declicker, VoiceFloorStillDucksImpulseWhenVoiceActive) {
  // Default config carries a voice floor (~0.6): during full voice the duck is softened but NOT
  // disabled, so clicks that land on top of speech are still substantially reduced (DFN3 path).
  const size_t L = 144, n = 2000, P = 500;
  std::vector<float> in(n, 0.0f), out0(n), out1(n);
  in[P] = 1.0f;
  Declicker a = make(); a.process(in.data(), out0.data(), n, 1.0f, /*vad*/0.0f);
  Declicker b = make(); b.process(in.data(), out1.data(), n, 1.0f, /*vad*/1.0f);
  EXPECT_LT(std::fabs(out0[P + L]), 0.15f);   // vad=0 -> fully ducked
  EXPECT_LT(std::fabs(out1[P + L]), 0.6f);    // vad=1 -> voice floor still ducks it well below unity
}

TEST(Declicker, ZeroVoiceFloorIsFullyVoiceSafe) {
  // voiceFloor=0 restores the original behavior: the duck fully backs off under voice.
  Declicker::Config c; c.voiceFloor = 0.0f;
  const size_t L = 144, n = 2000, P = 500;
  std::vector<float> in(n, 0.0f), out(n);
  in[P] = 1.0f;
  Declicker d(c); d.process(in.data(), out.data(), n, 1.0f, /*vad*/1.0f);
  EXPECT_GT(std::fabs(out[P + L]), 0.8f);     // vad=1 -> passes (fully voice-safe)
}
