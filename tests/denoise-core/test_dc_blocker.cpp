#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "dc_blocker.h"
using vc::DcBlocker;
TEST(DcBlocker, RemovesConstantDc) {
  DcBlocker d;                     // R=0.998
  std::vector<float> x(20000, 1.0f), y(20000);
  d.process(x.data(), y.data(), x.size());
  EXPECT_LT(std::fabs(y.back()), 0.02f);       // DC driven toward 0
}
TEST(DcBlocker, PassesAlternatingSignal) {
  DcBlocker d;
  std::vector<float> x(1000), y(1000);
  for (size_t i=0;i<x.size();++i) x[i] = (i%2? 1.0f : -1.0f);   // Nyquist-ish, well above corner
  d.process(x.data(), y.data(), x.size());
  // after settling, magnitude preserved to within a few percent
  EXPECT_GT(std::fabs(y[900]), 0.9f);
}
TEST(DcBlocker, AliasingInPlaceSafe) {
  DcBlocker a, b;
  std::vector<float> x(500), y(500), z(500);
  for (size_t i=0;i<500;++i){ x[i]=std::sin(0.1f*i); z[i]=x[i]; }
  a.process(x.data(), y.data(), 500);          // out-of-place
  b.process(z.data(), z.data(), 500);          // in-place
  for (size_t i=0;i<500;++i) EXPECT_NEAR(y[i], z[i], 1e-6f);
}
