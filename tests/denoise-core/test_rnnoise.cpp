#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "dry_wet_mix.h"
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
  EXPECT_EQ(f.AlgorithmicDelaySamples(), 960u);   // real lag: input frame k leaves at call k+2
}

namespace {

// Aperiodic probe: harmonic sweep, fundamental 100 -> 300 Hz (40 harmonics, 1/sqrt(h) amplitudes: broadband, sharp correlation peak) at about
// -20 dBFS peak. A fixed-pitch probe has equal correlation peaks every period and 960 is a multiple
// of common pitch periods, so it could not discriminate the lag; a glide has a single dominant peak.
std::vector<float> glide(size_t n) {
  std::vector<float> x(n);
  const double sr = 48000.0, pi = 3.14159265358979323846;
  double phase = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double f0 = 100.0 + 200.0 * (static_cast<double>(i) / static_cast<double>(n));
    phase += 2.0 * pi * f0 / sr;
    double v = 0.0;
    for (int h = 1; h <= 40; ++h) v += std::sin(h * phase) / std::sqrt(static_cast<double>(h));
    x[i] = static_cast<float>(0.1 * v / 12.0);   // roughly -20 dBFS peak
  }
  return x;
}

std::vector<float> runFilter(RnnoiseFilter& f, const std::vector<float>& in) {
  std::vector<float> out(in.size());
  for (size_t o = 0; o + vc::FRAME_SAMPLES <= in.size(); o += vc::FRAME_SAMPLES)
    f.ProcessFrame(&in[o], &out[o]);
  return out;
}

// Raw (unnormalized) cross-correlation of y against x at lag L: sum x[i] * y[i + L].
double xcorr(const std::vector<float>& x, const std::vector<float>& y, size_t lag) {
  double acc = 0.0;
  for (size_t i = 0; i + lag < y.size() && i < x.size(); ++i) acc += double(x[i]) * double(y[i + lag]);
  return acc;
}

size_t argmaxLag(const std::vector<float>& x, const std::vector<float>& y, size_t maxLag) {
  size_t best = 0; double bv = -1e300;
  for (size_t l = 0; l <= maxLag; ++l) {
    const double c = xcorr(x, y, l);
    if (c > bv) { bv = c; best = l; }
  }
  return best;
}

}  // namespace

TEST(Rnnoise, FirstTwoCallsOutputZeros) {
  RnnoiseFilter f;
  std::vector<float> in(vc::FRAME_SAMPLES, 0.0f), out(vc::FRAME_SAMPLES, -1.0f);
  uint32_t s = 12345;
  for (int call = 0; call < 2; ++call) {
    for (auto& v : in) { s = s * 1664525u + 1013904223u; v = 0.3f * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f); }
    f.ProcessFrame(in.data(), out.data());
    // Call 0 synthesizes the zero-initialized delayed_X (exact zeros). Call 1 emits only the zero
    // "previous" half of frame 0's window, so it is near zero but not bit-exact: spectral gains leak
    // roughly 1e-3 into it for this 0.3-amplitude white noise (about -60 dBFS vs the -15 dBFS input).
    const float tol = (call == 0) ? 0.0f : 1e-2f;
    for (size_t i = 0; i < vc::FRAME_SAMPLES; ++i) ASSERT_NEAR(out[i], 0.0f, tol) << "call " << call << " sample " << i;
  }
}

TEST(Rnnoise, BurstInFrameKFirstAppearsAtCallKPlus2) {
  const auto burst = glide(vc::FRAME_SAMPLES * 4);   // voiced-like burst: RNNoise would gate white noise ~35 dB
  for (int k : {3, 5, 8}) {
    RnnoiseFilter f;
    std::vector<float> in(vc::FRAME_SAMPLES), out(vc::FRAME_SAMPLES);
    std::vector<double> energy;
    for (int call = 0; call < k + 5; ++call) {
      for (size_t i = 0; i < vc::FRAME_SAMPLES; ++i) in[i] = (call == k) ? burst[i + vc::FRAME_SAMPLES] : 0.0f;
      f.ProcessFrame(in.data(), out.data());
      double e = 0.0;
      for (float v : out) e += double(v) * v;
      energy.push_back(e);
    }
    // Before call k+2 only spectral-gain leakage into the window's empty half can appear (about 25 dB
    // below the burst); the burst itself arrives at call k+2.
    ASSERT_GT(energy[k + 2], 1e-4) << "k=" << k;
    for (int call = 0; call < k + 2; ++call)
      EXPECT_LT(energy[call], 1e-2 * energy[k + 2]) << "k=" << k << " call " << call << " must be (near) silent";
  }
}

TEST(Rnnoise, MeasuredLagEqualsReportedDelay) {
  RnnoiseFilter f;
  const auto in = glide(48000 * 3);
  const auto out = runFilter(f, in);
  // The frame-level delay is exactly 960 (see the deterministic tests above). RNNoise's input
  // high-pass (about 40 Hz biquad) has a small phase lead, so the correlation argmax of a real signal
  // lands a sample or two early (959 with this probe); allow +/-2, far below the 480-sample steps a
  // wrong delay would produce.
  const long lag = static_cast<long>(argmaxLag(in, out, 2400));
  EXPECT_NEAR(lag, static_cast<long>(f.AlgorithmicDelaySamples()), 2);
  EXPECT_NEAR(lag, 960, 2);
}

// STRENGTH < 100% must blend wet with a dry path delayed by the engine's REAL delay. Uses DryWetMix +
// RnnoiseFilter directly (the full DspChain's HPF/AGC/gate would mask the effect).
TEST(Rnnoise, DryWetMixAlignedWithReportedDelay) {
  RnnoiseFilter f;
  vc::DryWetMix mix(f.AlgorithmicDelaySamples());
  const auto in = glide(48000 * 3);
  std::vector<float> wet(in.size()), mixed(in.size());
  for (size_t o = 0; o + vc::FRAME_SAMPLES <= in.size(); o += vc::FRAME_SAMPLES) {
    f.ProcessFrame(&in[o], &wet[o]);
    mix.process(&in[o], &wet[o], &mixed[o], vc::FRAME_SAMPLES, 0.5f);
  }
  const size_t lag = argmaxLag(in, mixed, 2400);
  EXPECT_NEAR(static_cast<long>(lag), 960, 2);   // see MeasuredLagEqualsReportedDelay for the tolerance
  const double peak = xcorr(in, mixed, lag);
  // Misaligned (delay reported as 0) leaves a second peak of similar height at lag 0 / 1920; aligned
  // leaves only autocorrelation side lobes of the glide.
  EXPECT_LT(xcorr(in, mixed, 0),    0.5 * peak);
  EXPECT_LT(xcorr(in, mixed, 1920), 0.5 * peak);
}
