#include <gtest/gtest.h>
#include "latency.h"
using vc::LatencyInputs; using vc::LatencyBreakdown; using vc::compute_latency;

TEST(Latency, FramerIsAlwaysOneHop) {
  LatencyInputs in;   // all buffers zero
  LatencyBreakdown b = compute_latency(in);
  EXPECT_NEAR(b.framerMs, 10.0, 1e-6);   // one 480-sample hop @ 48 kHz
  EXPECT_NEAR(b.totalMs, 10.0, 1e-6);
}

TEST(Latency, RnnoiseBudget) {
  LatencyInputs in;
  in.captureBufferFrames = 480;   // 10 ms
  in.renderBufferFrames  = 480;   // 10 ms
  in.ringPrimedFrames    = 8;     // 8 * 10 ms = 80 ms (shipped default ring_frames=16 -> primes 8)
  in.engineDelaySamples  = 960;   // RNNoise real delay (20 ms)
  LatencyBreakdown b = compute_latency(in);
  EXPECT_NEAR(b.captureMs, 10.0, 1e-6);
  EXPECT_NEAR(b.ringMs,    80.0, 1e-6);
  EXPECT_NEAR(b.engineMs,  20.0, 1e-6);
  EXPECT_NEAR(b.renderMs,  10.0, 1e-6);
  EXPECT_NEAR(b.totalMs,   130.0, 1e-6);   // 10+10+80+20+10
}

TEST(Latency, EngineDelayConvertsSamplesToMs) {
  LatencyInputs a; a.sampleRate = 48000.0; a.engineDelaySamples = 960;
  EXPECT_NEAR(compute_latency(a).engineMs, 20.0, 1e-6);
}

TEST(Latency, SampleRateGuardFallsBackTo48k) {
  LatencyInputs in; in.sampleRate = 0.0; in.captureBufferFrames = 480;
  LatencyBreakdown b = compute_latency(in);
  EXPECT_NEAR(b.captureMs, 10.0, 1e-6);   // guarded to 48 kHz, not a divide-by-zero
}
