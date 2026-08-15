#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "agc.h"
using vc::Agc;
static float rmsdb(const std::vector<float>& v,size_t from){double s=0;size_t n=0;for(size_t i=from;i<v.size();++i){s+=v[i]*v[i];++n;}return 20.0f*std::log10(std::sqrt(s/n)+1e-12);}
TEST(Agc, RaisesQuietSignalTowardTarget){
  Agc::Config c; c.targetRmsDb=-15; Agc a(c);
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
