#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include <utility>
#include "agc.h"
using vc::Agc;
static float rmsdb(const std::vector<float>& v,size_t from){double s=0;size_t n=0;for(size_t i=from;i<v.size();++i){s+=v[i]*v[i];++n;}return 20.0f*std::log10(std::sqrt(s/n)+1e-12);}
TEST(Agc, RaisesQuietSignalTowardTarget){
  Agc::Config c; c.targetRmsDb=-15; c.maxGainDb=30; Agc a(c);   // pin cap: this mechanics test needs +22 dB
  std::vector<float> x(96000), y(96000);
  for(size_t i=0;i<x.size();++i) x[i]=0.02f*std::sin(2*3.14159f*300.0f*i/48000.0f); // ~-34 dBFS
  a.process(x.data(),y.data(),x.size(),true);
  EXPECT_NEAR(rmsdb(y,72000), -15.0f, 2.0f);   // pulled up to ~target
}
TEST(Agc, FrozenWhenVadInactive){
  Agc::Config c; Agc a(c);
  std::vector<float> x(48000,0.0f), y(48000);
  a.process(x.data(),y.data(),x.size(),false);  // silence + no vad -> gain must not hunt up
  EXPECT_NEAR(a.currentGainDb(), 0.0f, 1e-3f);
}

TEST(Agc, SetTargetDbChangesGainTargetLive) {
  vc::Agc a({-30.0f, 5.0f, 5.0f, 30.0f, -10.0f, 48000.0f});
  std::vector<float> in(4800, 0.1f), out(4800);
  a.process(in.data(), out.data(), in.size(), true);
  a.setTargetDb(-6.0f);                      // raise the target
  a.process(in.data(), out.data(), in.size(), true);
  float rmsAfter = 0; for (float v : out) rmsAfter += v * v;
  rmsAfter = std::sqrt(rmsAfter / out.size());
  const float targetLin = std::pow(10.0f, -6.0f / 20.0f);
  EXPECT_NEAR(rmsAfter, targetLin, 0.05f);   // converged near the NEW target
}

// The reported problem: a quiet mic (~-40 dBFS speech) with the old fixed +9 dB cap never reached
// the target, and raising the target did nothing ("AGC +9.0 dB" pinned). The cap is now adjustable.
TEST(Agc, MaxGainCapsHowFarAQuietMicIsRaised) {
  std::vector<float> x(96000), y(96000);
  for (size_t i = 0; i < x.size(); ++i) x[i] = 0.0141f * std::sin(2 * 3.14159f * 300.0f * i / 48000.0f); // ~-40 dBFS RMS
  auto run = [&](float maxGainDb) {
    Agc::Config c; c.targetRmsDb = -16.0f; c.maxGainDb = maxGainDb; Agc a(c);
    a.process(x.data(), y.data(), x.size(), true);
    return std::make_pair(a.currentGainDb(), rmsdb(y, 72000));
  };
  auto old9  = run(9.0f);
  auto now18 = run(18.0f);
  auto big30 = run(30.0f);
  EXPECT_NEAR(old9.first, 9.0f, 0.5f);       // pinned at the cap (approached via the 500 ms release) ...
  EXPECT_NEAR(old9.second, -31.0f, 1.0f);    // ... so output stays ~15 dB under the -16 target
  EXPECT_NEAR(now18.first, 18.0f, 0.5f);
  EXPECT_NEAR(now18.second, -22.0f, 1.0f);   // 9 dB louder than before
  EXPECT_LT(big30.first, 30.0f);             // enough headroom: gain settles below the cap
  EXPECT_NEAR(big30.second, -16.0f, 1.5f);   // and the target is actually reached
}
TEST(Agc, LoweringMaxGainClampsTheHeldGainImmediately) {
  Agc::Config c; c.targetRmsDb = -16.0f; c.maxGainDb = 30.0f; Agc a(c);
  std::vector<float> x(48000, 0.003f), y(48000);   // very quiet -> gain climbs high
  a.process(x.data(), y.data(), x.size(), true);
  ASSERT_GT(a.currentGainDb(), 20.0f);
  a.setMaxGainDb(12.0f);                           // user drags MAX BOOST down while silent (VAD off)
  EXPECT_FLOAT_EQ(a.currentGainDb(), 12.0f);       // not left frozen above the new cap
  EXPECT_FLOAT_EQ(a.maxGainDb(), 12.0f);
}
