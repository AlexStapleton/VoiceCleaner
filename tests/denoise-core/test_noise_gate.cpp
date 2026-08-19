#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "noise_gate.h"
using vc::NoiseGate;

static NoiseGate make() { return NoiseGate(NoiseGate::Config{}); }

TEST(NoiseGate, StrengthZeroIsBypass) {
  NoiseGate g = make();
  const size_t n = 1000;
  std::vector<float> in(n), out(n);
  for (size_t i = 0; i < n; ++i) in[i] = 0.2f * std::sin(2*3.14159f*200.0f*i/48000.0f);
  g.process(in.data(), out.data(), n, /*open*/false, /*strength*/0.0f);  // closed but strength 0
  for (size_t i = 0; i < n; ++i) EXPECT_NEAR(out[i], in[i], 1e-6f);       // no attenuation
}

TEST(NoiseGate, ClosedAttenuatesWhenStrengthHigh) {
  NoiseGate g = make();
  const size_t n = 24000;                                                 // 0.5 s to settle
  std::vector<float> in(n, 0.2f), out(n);
  g.process(in.data(), out.data(), n, /*open*/false, /*strength*/1.0f);
  double tail = 0; for (size_t i = n - 1000; i < n; ++i) tail += out[i]*out[i];
  tail = std::sqrt(tail / 1000.0);
  EXPECT_LT(tail, 0.2f * 0.1f);                                           // gap driven well below input
}

TEST(NoiseGate, OpenPassesThroughAtUnity) {
  NoiseGate g = make();
  const size_t n = 24000;
  std::vector<float> in(n, 0.2f), out(n);
  g.process(in.data(), out.data(), n, /*open*/true, /*strength*/1.0f);
  EXPECT_NEAR(out[n - 1], 0.2f, 0.01f);                                   // open -> unity
}

TEST(NoiseGate, ReopensAfterClose) {
  NoiseGate g = make();
  const size_t n = 24000;
  std::vector<float> in(n, 0.2f), out(n);
  g.process(in.data(), out.data(), n, /*open*/false, 1.0f);              // close down
  g.process(in.data(), out.data(), n, /*open*/true, 1.0f);              // then open
  EXPECT_NEAR(out[n - 1], 0.2f, 0.01f);
}
