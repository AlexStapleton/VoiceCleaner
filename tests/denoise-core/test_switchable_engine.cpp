#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "switchable_engine.h"
using namespace vc;

// Test double: fixed-gain "engine" with configurable reported latency.
struct GainEngine : INoiseFilter {
  float g; uint32_t d;
  GainEngine(float gain, uint32_t delay):g(gain),d(delay){}
  void ProcessFrame(const float* in, float* out) noexcept override { for(int i=0;i<480;++i) out[i]=g*in[i]; }
  float LastVadProbability() const noexcept override { return 0.0f; }
  void SetAggression(float) noexcept override {}
  uint32_t AlgorithmicDelaySamples() const noexcept override { return d; }
};

TEST(Switchable, SteadyStateEqualsActiveEngineDelayAligned){
  GainEngine a(2.0f,0), b(3.0f,960);          // b reports 2 frames extra latency
  SwitchableEngine sw(&a,&b,/*fadeFrames=*/1);
  EXPECT_EQ(sw.AlgorithmicDelaySamples(), 960u);
  std::vector<float> in(480,1.0f), out(480);
  for(int f=0; f<4; ++f) sw.ProcessFrame(in.data(), out.data());   // prime the 960-sample delay line
  for(int i=0;i<480;++i) EXPECT_NEAR(out[i], 2.0f, 1e-4f);         // engine a, gain 2, delay-aligned
}

TEST(Switchable, EqualPowerGainLawOnSwitch){
  // a passes the input through (the "from" stream); b outputs 0 (the "to" stream). With
  // delay 0 on both, the crossfade output equals the equal-power gain cos(theta) each frame,
  // theta stepping 0->pi/2 across fadeFrames=4 with p spanning 0..1 INCLUSIVE. Frame 0 -> p=0
  // -> cos(0)=1 == the pre-switch value (continuous); last fade frame -> p=1 -> cos(pi/2)=0.
  GainEngine a(1.0f,0), b(0.0f,0);
  SwitchableEngine sw(&a,&b,/*fadeFrames=*/4);
  std::vector<float> in(480,1.0f), out(480);
  sw.ProcessFrame(in.data(), out.data());
  EXPECT_NEAR(out[0], 1.0f, 1e-4f);                          // steady on engine 0 before switch
  sw.requestActive(1);
  const float expect[4] = {1.0f, 0.86603f, 0.5f, 0.0f};   // cos(0, pi/6, pi/3, pi/2)
  for(int f=0; f<4; ++f){
    sw.ProcessFrame(in.data(), out.data());
    for(int i=0;i<480;++i) EXPECT_NEAR(out[i], expect[f], 1e-4f) << "fade frame " << f;
  }
  sw.ProcessFrame(in.data(), out.data());
  for(int i=0;i<480;++i) EXPECT_NEAR(out[i], 0.0f, 1e-4f);   // settled on engine 1
  EXPECT_EQ(sw.activeIndex(), 1);
}
