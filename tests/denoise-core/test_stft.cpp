#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include <random>
#include "stft.h"
using vc::Stft;
TEST(Stft, PerfectReconstruction) {
  const int fft=960, hop=480;
  Stft st(fft, hop);
  std::vector<float> re(st.bins()), im(st.bins());
  // input: sum of a few sinusoids
  const int nHops = 40;
  std::vector<float> in(nHops*hop), out(nHops*hop, 0.0f);
  for (size_t i=0;i<in.size();++i) in[i] = 0.3f*std::sin(0.017f*i) + 0.2f*std::sin(0.11f*i);
  for (int h=0; h<nHops; ++h) {
    st.analyze(&in[h*hop], re.data(), im.data());          // identity spectral processing
    st.synthesize(re.data(), im.data(), &out[h*hop]);
  }
  // reconstructed = input delayed by (fft-hop); compare on the steady-state middle region
  const int delay = fft - hop;
  int errs=0; double maxerr=0;
  for (int i = 5*hop; i < (nHops-2)*hop; ++i) {
    double d = std::fabs(out[i] - in[i - delay]);
    maxerr = std::max(maxerr, d);
    if (d > 1e-3) ++errs;
  }
  EXPECT_EQ(errs, 0) << "max err " << maxerr;
}
TEST(Stft, BinsAndHop){ Stft st(960,480); EXPECT_EQ(st.bins(), 481); EXPECT_EQ(st.hop(), 480); }
