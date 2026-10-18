#include <gtest/gtest.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "limiter.h"
using vc::Limiter;
TEST(Limiter, NeverExceedsCeiling){
  Limiter lim(-1.0f, 144, 50.0f, 48000.0f);
  float ceil = std::pow(10.0f, -1.0f/20.0f);
  std::vector<float> x(4800), y(4800);
  for(size_t i=0;i<x.size();++i) x[i]= 2.0f*std::sin(2*3.14159f*300.0f*i/48000.0f); // peaks at 2.0
  lim.process(x.data(), y.data(), x.size());
  float maxo=0; for(size_t i=200;i<y.size();++i) maxo=std::max(maxo,std::fabs(y[i]));
  EXPECT_LE(maxo, ceil + 0.02f);     // brickwall honored (small tolerance)
}
TEST(Limiter, UnityBelowCeiling){
  Limiter lim(-1.0f, 144, 50.0f, 48000.0f);
  std::vector<float> x(2000), y(2000);
  for(size_t i=0;i<x.size();++i) x[i]=0.3f*std::sin(0.05f*i);   // well below ceiling
  lim.process(x.data(), y.data(), x.size());
  // output equals input delayed by 144, gain ~1
  for(size_t i=500;i<2000;++i) EXPECT_NEAR(y[i], x[i-144], 1e-3f);
}

TEST(Limiter, SetCeilingDbLowersTheCeilingLive) {
  vc::Limiter lim(-1.0f, 16, 50.0f, 48000.0f);
  lim.setCeilingDb(-12.0f);
  std::vector<float> in(2048, 0.9f), out(2048);
  lim.process(in.data(), out.data(), in.size());
  float peak = 0; for (float v : out) { float a = std::fabs(v); if (a > peak) peak = a; }
  const float ceilLin = std::pow(10.0f, -12.0f / 20.0f);
  EXPECT_LE(peak, ceilLin + 1e-3f);
}

namespace {
// The original O(L)-scan limiter, kept verbatim as the reference the sliding-peak version must match.
struct ScanLimiter {
  float ceiling_; size_t L_; float relCoef_; std::vector<float> delay_; size_t widx_ = 0; float gain_ = 1.0f;
  ScanLimiter(float db, size_t L, float rel, float fs)
      : ceiling_(std::pow(10.0f, db / 20.0f)), L_(L < 1 ? 1 : L),
        relCoef_(std::exp(-1.0f / (rel * 0.001f * fs))), delay_(L_, 0.0f) {}
  void process(const float* in, float* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      float x = in[i];
      float peak = std::fabs(x);
      for (size_t k = 0; k < L_; ++k) peak = std::max(peak, std::fabs(delay_[k]));
      float target = (peak > ceiling_) ? ceiling_ / peak : 1.0f;
      if (target < gain_) gain_ = target; else gain_ = relCoef_ * gain_ + (1.0f - relCoef_) * 1.0f;
      float d = delay_[widx_]; delay_[widx_] = x; widx_ = (widx_ + 1) % L_;
      out[i] = d * gain_;
    }
  }
};

// Loud noise, strictly falling and rising ramps longer than the look-ahead, impulses, repeated
// equal peaks and silence: every shape that exercises the sliding-peak queue's push/pop/expiry.
std::vector<float> limiterStress(size_t L) {
  std::vector<float> x;
  uint32_t s = 12345u;
  auto rnd = [&s] { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 32768.0f - 1.0f; };
  for (int i = 0; i < 3000; ++i) x.push_back(2.5f * rnd());                          // noise, often > ceiling
  for (size_t i = 0; i < 3 * L; ++i) x.push_back(3.0f - 2.9f * (float)i / (float)(3 * L)); // strictly falling
  for (size_t i = 0; i < 3 * L; ++i) x.push_back(-0.1f - 2.9f * (float)i / (float)(3 * L)); // strictly rising |x|
  // Shallow strictly falling ramp: the gain releases above ceiling/peak while the oldest (largest)
  // sample is still in the window, so the limiter must keep re-applying that peak. A queue that loses
  // the window's front entry (e.g. overflows its L+1 slots) shows up here as a different output.
  // Silence first, so the gain has recovered to ~1 and the ramp's first sample sets it.
  for (int i = 0; i < 12000; ++i) x.push_back(0.0f);
  for (int i = 0; i < 2000; ++i) x.push_back(2.0f - 0.05f * (float)i / 2000.0f);
  for (int k = 0; k < 20; ++k) { x.push_back(k % 2 ? 4.0f : -4.0f); for (int j = 0; j < 37 * k; ++j) x.push_back(0.05f); }
  for (int i = 0; i < 1000; ++i) x.push_back(i % 50 == 0 ? 1.5f : 0.0f);           // equal peaks
  for (int i = 0; i < 1000; ++i) x.push_back(0.0f);                                 // silence
  return x;
}
}  // namespace

TEST(Limiter, SlidingPeakMatchesScanReferenceExactly) {
  for (size_t L : {1u, 2u, 16u, 144u}) {
    const std::vector<float> x = limiterStress(L);
    Limiter lim(-1.0f, L, 50.0f, 48000.0f);
    ScanLimiter ref(-1.0f, L, 50.0f, 48000.0f);
    std::vector<float> y(x.size()), r(x.size());
    // Odd block sizes so the queue state crosses process() calls at arbitrary points.
    for (size_t off = 0, blk = 1; off < x.size(); off += blk, blk = blk % 509 + 7) {
      const size_t n = std::min(blk, x.size() - off);
      lim.process(x.data() + off, y.data() + off, n);
      ref.process(x.data() + off, r.data() + off, n);
    }
    for (size_t i = 0; i < x.size(); ++i) ASSERT_EQ(y[i], r[i]) << "L=" << L << " i=" << i;
  }
}

TEST(Limiter, ResetMatchesAFreshLimiter) {
  const std::vector<float> x = limiterStress(144);
  Limiter used(-1.0f, 144, 50.0f, 48000.0f), fresh(-1.0f, 144, 50.0f, 48000.0f);
  std::vector<float> y(x.size()), r(x.size());
  used.process(x.data(), y.data(), x.size());   // leave the peak queue mid-state
  used.reset();
  used.process(x.data(), y.data(), x.size());
  fresh.process(x.data(), r.data(), x.size());
  for (size_t i = 0; i < x.size(); ++i) ASSERT_EQ(y[i], r[i]) << "i=" << i;
}
