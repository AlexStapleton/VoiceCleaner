#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include <vector>
#include "dynamic_engine.h"
using namespace vc;

// Lifetime- and call-tracking fake. All instances of one logical index share an EngineProbe, so a
// test can assert liveness (ctor/dtor balance -> detects leak/double-free/UAF), call counts, and the
// last aggression pushed in.
struct EngineProbe {
  float gain = 1.0f;
  uint32_t delay = 0;
  int ctorCount = 0, dtorCount = 0, processCount = 0;
  float lastAggression = -1.0f;
  int live() const { return ctorCount - dtorCount; }
};
struct ProbeEngine : INoiseFilter {
  EngineProbe* p;
  explicit ProbeEngine(EngineProbe* probe) : p(probe) { ++p->ctorCount; }
  ~ProbeEngine() override { ++p->dtorCount; }
  void ProcessFrame(const float* in, float* out) noexcept override {
    ++p->processCount; for (size_t i = 0; i < FRAME_SAMPLES; ++i) out[i] = p->gain * in[i];
  }
  float LastVadProbability() const noexcept override { return 0.0f; }
  void SetAggression(float z) noexcept override { p->lastAggression = z; }
  uint32_t AlgorithmicDelaySamples() const noexcept override { return p->delay; }
};

// Builds a loader over a caller-owned probes[2] and the matching Config.delaySamples.
static DynamicEngine::Loader probeLoader(EngineProbe* probes) {
  return [probes](int idx) -> std::unique_ptr<INoiseFilter> {
    return std::make_unique<ProbeEngine>(&probes[idx]);
  };
}

TEST(DynamicEngine, WarmSteadyStateEqualsActiveEngineDelayAligned) {
  EngineProbe pr[2]; pr[0] = {2.0f, 0}; pr[1] = {3.0f, 960};   // idx1 reports 2 frames extra latency
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Warm; c.initialActive = 0;
  c.fadeFrames = 1; c.delaySamples[0] = 0; c.delaySamples[1] = 960;
  DynamicEngine eng(probeLoader(pr), c);
  EXPECT_EQ(eng.AlgorithmicDelaySamples(), 960u);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);
  for (int f = 0; f < 4; ++f) eng.ProcessFrame(in.data(), out.data());   // prime the 960-sample delay line
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], 2.0f, 1e-4f);
  EXPECT_EQ(pr[0].live(), 1);
  EXPECT_EQ(pr[1].live(), 1);   // warm: both resident
  EXPECT_GT(pr[1].processCount, 0);   // warm: inactive engine still runs every frame
}
