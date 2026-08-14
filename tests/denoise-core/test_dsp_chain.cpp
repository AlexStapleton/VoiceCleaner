#include <gtest/gtest.h>
#include <vector>
#include <cmath>
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
