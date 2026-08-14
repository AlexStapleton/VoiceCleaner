#include <gtest/gtest.h>

#include <cmath>

#include "frame.h"
#include "rnnoise_filter.h"

using vc::RnnoiseFilter;

// RNNoise is data-heavy; this is a smoke test: it constructs, processes a
// frame, produces finite output, and reports a VAD probability in [0,1].
TEST(Rnnoise, ProcessesFrameFiniteAndVadInRange) {
  RnnoiseFilter f;
  float in[vc::FRAME_SAMPLES], out[vc::FRAME_SAMPLES];
  for (size_t i = 0; i < vc::FRAME_SAMPLES; ++i) {
    in[i] = 0.25f * std::sin(2.0f * 3.14159265f * 300.0f * i / 48000.0f);
    out[i] = -123.0f;
  }
  f.ProcessFrame(in, out);

  for (size_t i = 0; i < vc::FRAME_SAMPLES; ++i) {
    EXPECT_TRUE(std::isfinite(out[i])) << "sample " << i;
  }
  const float vad = f.LastVadProbability();
  EXPECT_GE(vad, 0.0f);
  EXPECT_LE(vad, 1.0f);
  EXPECT_EQ(f.AlgorithmicDelaySamples(), 0u);
}
