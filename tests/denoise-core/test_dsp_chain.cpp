#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include "dsp_chain.h"
#include "passthrough_filter.h"
using namespace vc;

// mock engine returning a fixed VAD probability (identity audio)
struct MockEngine : INoiseFilter {
  float v;
  explicit MockEngine(float vad):v(vad){}
  void ProcessFrame(const float* in, float* out) noexcept override { for(size_t i=0;i<FRAME_SAMPLES;++i) out[i]=in[i]; }
  float LastVadProbability() const noexcept override { return v; }
  void SetAggression(float) noexcept override {}
  uint32_t AlgorithmicDelaySamples() const noexcept override { return 0; }
};

TEST(DspChain, LimiterBoundsOutput){
  PassthroughFilter eng; DspChain::Config c; DspChain chain(&eng, c);
  float ceil = std::pow(10.0f, c.limiterCeilingDb/20.0f);
  float in[FRAME_SAMPLES], out[FRAME_SAMPLES];
  float maxo=0;
  for(int f=0; f<50; ++f){
    for(size_t i=0;i<FRAME_SAMPLES;++i) in[i]=1.8f*std::sin(2*3.14159f*300.0f*(f*FRAME_SAMPLES+i)/48000.0f);
    chain.process_frame(in,out);
    if(f>5) for(size_t i=0;i<FRAME_SAMPLES;++i) maxo=std::max(maxo,std::fabs(out[i]));
  }
  EXPECT_LE(maxo, ceil + 0.03f);       // limiter honored through the chain
}

TEST(DspChain, VadHysteresisOpensAndHolds){
  MockEngine eng(0.9f); DspChain::Config c; DspChain chain(&eng, c);
  float in[FRAME_SAMPLES]={0}, out[FRAME_SAMPLES];
  chain.process_frame(in,out);
  EXPECT_TRUE(chain.vadActive());       // 0.9 >= open(0.6)
  // drop VAD below close; should hold active for ~200ms then release
  eng.v = 0.1f;
  chain.process_frame(in,out);
  EXPECT_TRUE(chain.vadActive());       // still within hold window
  for(int i=0;i<60;++i) chain.process_frame(in,out);  // >200ms of frames (60*10ms)
  EXPECT_FALSE(chain.vadActive());      // hold expired -> released
}

TEST(DspChain, RemovesDcOffset){
  MockEngine eng(0.0f); DspChain::Config c; DspChain chain(&eng, c);
  float in[FRAME_SAMPLES], out[FRAME_SAMPLES];
  for(size_t i=0;i<FRAME_SAMPLES;++i) in[i]=1.0f;   // pure DC
  for(int f=0; f<200; ++f) chain.process_frame(in,out);  // settle
  float mean=0; for(size_t i=0;i<FRAME_SAMPLES;++i) mean+=out[i]; mean/=FRAME_SAMPLES;
  EXPECT_LT(std::fabs(mean), 0.05f);    // DC blocker + HPF removed the offset
}

TEST(DspChain, CurrentsInitializeToConfigNotZero) {
  vc::PassthroughFilter eng;
  vc::DspChain::Config cfg;              // hpf 80, agc -20, lim -1
  vc::DspChain chain(&eng, cfg);
  EXPECT_FLOAT_EQ(chain.currentHpfCutoffHz(), 80.0f);
  EXPECT_FLOAT_EQ(chain.currentAgcTargetDb(), -20.0f);
  EXPECT_FLOAT_EQ(chain.currentLimiterCeilingDb(), -1.0f);
}
TEST(DspChain, HpfCutoffRampsMonotonicallyToTarget) {
  vc::PassthroughFilter eng;
  vc::DspChain chain(&eng, vc::DspChain::Config{});
  chain.setHpfCutoffHz(200.0f);
  std::vector<float> in(vc::FRAME_SAMPLES, 0.0f), out(vc::FRAME_SAMPLES);
  float prev = chain.currentHpfCutoffHz();
  bool monotonic = true;
  for (int f = 0; f < 20; ++f) {
    chain.process_frame(in.data(), out.data());
    float cur = chain.currentHpfCutoffHz();
    if (cur < prev - 1e-3f) monotonic = false;   // never decreases toward a higher target
    prev = cur;
  }
  EXPECT_TRUE(monotonic);
  EXPECT_NEAR(chain.currentHpfCutoffHz(), 200.0f, 1.0f);  // arrived
}
TEST(DspChain, NoSingleFrameJumpOnParamChange) {
  vc::PassthroughFilter eng;
  vc::DspChain chain(&eng, vc::DspChain::Config{});
  chain.setAgcTargetDb(0.0f);            // big change from -20
  float prev = chain.currentAgcTargetDb();
  std::vector<float> in(vc::FRAME_SAMPLES, 0.0f), out(vc::FRAME_SAMPLES);
  for (int f = 0; f < 5; ++f) {
    chain.process_frame(in.data(), out.data());
    float step = std::fabs(chain.currentAgcTargetDb() - prev);
    EXPECT_LT(step, 10.0f);              // no full-jump in one frame (full jump would be 20 dB)
    prev = chain.currentAgcTargetDb();
  }
}
TEST(DspChain, BypassCopiesInputToOutput) {
  vc::PassthroughFilter eng;
  vc::DspChain chain(&eng, vc::DspChain::Config{});
  chain.setBypass(true);
  std::vector<float> in(vc::FRAME_SAMPLES), out(vc::FRAME_SAMPLES);
  for (size_t i = 0; i < in.size(); ++i) in[i] = 0.01f * static_cast<float>(i % 50);
  chain.process_frame(in.data(), out.data());
  for (size_t i = 0; i < in.size(); ++i) EXPECT_FLOAT_EQ(out[i], in[i]);
}

TEST(DspChain, DeclickReducesTransientWhenEnabled) {
  vc::PassthroughFilter eng;
  vc::DspChain::Config cfg; cfg.declickStrength = 1.0f;
  vc::DspChain chain(&eng, cfg);
  std::vector<float> in(vc::FRAME_SAMPLES, 0.0f), out(vc::FRAME_SAMPLES);
  in[10] = 1.0f;                              // a click early in the frame
  for (int f = 0; f < 4; ++f) chain.process_frame(in.data(), out.data());  // let the duck engage
  float peak = 0; for (float v : out) peak = std::max(peak, std::fabs(v));
  EXPECT_LT(peak, 0.5f);                      // transient is ducked (default chain would pass ~1.0)
}
TEST(DspChain, DeclickDefaultIsZeroStrength) {
  EXPECT_FLOAT_EQ(vc::DspChain::Config{}.declickStrength, 0.0f);
}
