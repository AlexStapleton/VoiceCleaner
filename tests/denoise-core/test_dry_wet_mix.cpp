#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "dry_wet_mix.h"
using vc::DryWetMix;

TEST(DryWetMix, FullWetReturnsWetExactly) {
  DryWetMix m(0);
  const size_t n = 256;
  std::vector<float> dry(n), wet(n), out(n);
  for (size_t i = 0; i < n; ++i) { dry[i] = (float)i; wet[i] = -(float)i; }
  m.process(dry.data(), wet.data(), out.data(), n, 1.0f);
  for (size_t i = 0; i < n; ++i) EXPECT_FLOAT_EQ(out[i], wet[i]);
}

TEST(DryWetMix, ZeroDelayZeroStrengthReturnsDry) {
  DryWetMix m(0);
  const size_t n = 256;
  std::vector<float> dry(n), wet(n, 0.0f), out(n);
  for (size_t i = 0; i < n; ++i) dry[i] = std::sin(0.05f * i);
  m.process(dry.data(), wet.data(), out.data(), n, 0.0f);
  for (size_t i = 0; i < n; ++i) EXPECT_FLOAT_EQ(out[i], dry[i]);
}

TEST(DryWetMix, ZeroDelayHalfStrengthAverages) {
  DryWetMix m(0);
  const size_t n = 128;
  std::vector<float> dry(n), wet(n), out(n);
  for (size_t i = 0; i < n; ++i) { dry[i] = 1.0f; wet[i] = 3.0f; }
  m.process(dry.data(), wet.data(), out.data(), n, 0.5f);
  for (size_t i = 0; i < n; ++i) EXPECT_FLOAT_EQ(out[i], 2.0f);
}

TEST(DryWetMix, DryPathIsDelayedByEngineDelay) {
  const size_t L = 4, n = 32;
  DryWetMix m(L);
  std::vector<float> dry(n, 0.0f), wet(n, 0.0f), out(n);
  dry[0] = 1.0f;                                   // impulse enters dry at t=0
  m.process(dry.data(), wet.data(), out.data(), n, 0.0f);  // pure dry
  EXPECT_FLOAT_EQ(out[L], 1.0f);                   // emerges L samples later
  for (size_t i = 0; i < n; ++i) if (i != L) EXPECT_FLOAT_EQ(out[i], 0.0f);
}
