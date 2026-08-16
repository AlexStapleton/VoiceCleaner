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

TEST(DynamicEngine, WarmEqualPowerGainLawOnSwitch) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {0.0f, 0};   // "from" passes input; "to" outputs 0
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Warm; c.initialActive = 0;
  c.fadeFrames = 4; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);
  eng.ProcessFrame(in.data(), out.data());
  EXPECT_NEAR(out[0], 1.0f, 1e-4f);                          // steady on engine 0 before the switch
  eng.requestActive(1);
  const float expect[4] = {1.0f, 0.86603f, 0.5f, 0.0f};     // cos(0, pi/6, pi/3, pi/2)
  for (int f = 0; f < 4; ++f) {
    eng.ProcessFrame(in.data(), out.data());
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], expect[f], 1e-4f) << "fade frame " << f;
  }
  eng.ProcessFrame(in.data(), out.data());
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], 0.0f, 1e-4f);   // settled on engine 1
  EXPECT_EQ(eng.activeIndex(), 1);
  EXPECT_EQ(pr[0].live(), 1);   // warm: nothing retired
  EXPECT_EQ(pr[1].live(), 1);
}

TEST(DynamicEngine, LeanSteadyStateRunsOnlyActive) {
  EngineProbe pr[2]; pr[0] = {2.0f, 0}; pr[1] = {3.0f, 480};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 15; c.delaySamples[0] = 0; c.delaySamples[1] = 480;
  DynamicEngine eng(probeLoader(pr), c);
  EXPECT_EQ(eng.AlgorithmicDelaySamples(), 480u);
  EXPECT_TRUE(eng.resident(0));
  EXPECT_FALSE(eng.resident(1));            // lean: only the active engine is loaded
  EXPECT_EQ(pr[1].live(), 0);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);
  for (int f = 0; f < 4; ++f) eng.ProcessFrame(in.data(), out.data());
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], 2.0f, 1e-4f);  // engine 0, delay-aligned
  EXPECT_EQ(pr[1].processCount, 0);         // idx1 never constructed, never run
}

TEST(DynamicEngine, LeanSwitchLoadsCrossfadesRetires) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {0.0f, 0};   // from passes input, to outputs 0
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 4; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  eng.requestActive(1);                     // loads idx1 on demand (off-RT here: same thread, serialized)
  EXPECT_TRUE(eng.resident(1));
  EXPECT_EQ(pr[1].lastAggression, 0.0f);    // stored aggression (default 0) applied on install

  const float expect[4] = {1.0f, 0.86603f, 0.5f, 0.0f};
  for (int f = 0; f < 4; ++f) {
    eng.ProcessFrame(in.data(), out.data());
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], expect[f], 1e-4f) << "fade frame " << f;
  }
  EXPECT_EQ(eng.activeIndex(), 1);
  EXPECT_FALSE(eng.resident(0));            // idx0 retired out of the live slot at fade end
  EXPECT_EQ(pr[0].live(), 1);              // ...but not yet freed — sitting in the reclaim queue

  eng.reclaim();                            // non-RT drain
  EXPECT_EQ(pr[0].live(), 0);              // now freed
  EXPECT_EQ(pr[0].dtorCount, 1);           // exactly once — no double free
}

TEST(DynamicEngine, ReclaimIsIndependentOfFurtherCommands) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {2.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 2; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  eng.requestActive(1);
  for (int f = 0; f < 2; ++f) eng.ProcessFrame(in.data(), out.data());   // fade completes, idx0 retired
  // No further requestActive. Keep processing frames (steady on idx1).
  for (int f = 0; f < 50; ++f) eng.ProcessFrame(in.data(), out.data());
  EXPECT_EQ(pr[0].live(), 1);   // still queued (reclaim not yet called)

  int freed = 0;
  eng.reclaim([&](INoiseFilter*, int idx) { if (idx == 0) ++freed; });
  EXPECT_EQ(freed, 1);
  EXPECT_EQ(pr[0].live(), 0);
}

TEST(DynamicEngine, RapidReswitchAfterRetireFreshLoadsNoDoubleFree) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {2.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 2; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  eng.requestActive(1);
  for (int f = 0; f < 2; ++f) eng.ProcessFrame(in.data(), out.data());   // idx0 retired to queue (not yet freed)
  eng.requestActive(0);                                                  // switch back BEFORE reclaim
  EXPECT_EQ(pr[0].ctorCount, 2);                                         // fresh idx0 built (queued one not reused)
  for (int f = 0; f < 2; ++f) eng.ProcessFrame(in.data(), out.data());   // fade to fresh idx0; idx1 retired
  EXPECT_EQ(eng.activeIndex(), 0);

  eng.reclaim();
  EXPECT_EQ(pr[0].dtorCount, 1);   // only the FIRST idx0 instance was freed
  EXPECT_EQ(pr[0].live(), 1);      // the fresh idx0 is still resident
  EXPECT_EQ(pr[1].live(), 0);      // idx1 retired and freed
}

TEST(DynamicEngine, ReswitchMidFadeKeepsOutgoingResident) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {2.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 6; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  eng.requestActive(1);
  for (int f = 0; f < 3; ++f) eng.ProcessFrame(in.data(), out.data());   // partway through the 0->1 fade
  eng.requestActive(0);                                                  // switch back mid-fade
  for (int f = 0; f < 20; ++f) eng.ProcessFrame(in.data(), out.data());  // finish, then fade 1->0
  EXPECT_EQ(eng.activeIndex(), 0);
  EXPECT_EQ(pr[0].ctorCount, 1);   // idx0 was NEVER retired/reloaded (RT-local cancel kept it resident)
  eng.reclaim();
  EXPECT_EQ(pr[0].live(), 1);      // idx0 still the resident engine
  EXPECT_EQ(pr[1].live(), 0);      // idx1 retired + freed
}

TEST(DynamicEngine, LoaderFailureAbortsSwitchCleanly) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {2.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 4; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  // Loader throws for idx1 (simulates model-load failure).
  DynamicEngine::Loader loader = [&](int idx) -> std::unique_ptr<INoiseFilter> {
    if (idx == 1) throw std::runtime_error("boom");
    return std::make_unique<ProbeEngine>(&pr[idx]);
  };
  DynamicEngine eng(loader, c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  EXPECT_THROW(eng.requestActive(1), std::runtime_error);
  EXPECT_EQ(eng.activeIndex(), 0);          // switch abandoned; still on idx0
  EXPECT_FALSE(eng.resident(1));            // no half-published slot
  eng.ProcessFrame(in.data(), out.data());  // still runs idx0, no crash
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], 1.0f, 1e-4f);
}

TEST(DynamicEngine, SetAggressionIsNullSafeAndAppliedOnInstall) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {1.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 2; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  eng.SetAggression(0.7f);                  // idx1 not resident -> must not crash
  EXPECT_FLOAT_EQ(pr[0].lastAggression, 0.7f);
  eng.requestActive(1);
  EXPECT_FLOAT_EQ(pr[1].lastAggression, 0.7f);   // freshly installed leaf gets the stored value
}
