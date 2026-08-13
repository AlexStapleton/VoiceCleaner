#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "biquad.h"
using vc::Biquad;
static float rms(const std::vector<float>& v, size_t from){ double s=0; size_t n=0; for(size_t i=from;i<v.size();++i){s+=v[i]*v[i];++n;} return (float)std::sqrt(s/n); }

TEST(Biquad, AttenuatesDc) {
  Biquad hp = Biquad::highpass(48000, 80.0);
  std::vector<float> x(4000, 1.0f), y(4000);
  hp.process(x.data(), y.data(), x.size());
  EXPECT_LT(std::fabs(y.back()), 0.01f);
}
TEST(Biquad, PassesHighFrequencyNearUnity) {
  Biquad hp = Biquad::highpass(48000, 80.0);
  std::vector<float> x(9600), y(9600);          // 1 kHz tone, well above 80 Hz
  for (size_t i=0;i<x.size();++i) x[i]=std::sin(2*3.14159265*1000.0*i/48000.0);
  hp.process(x.data(), y.data(), x.size());
  float ri = rms(x, 4800), ro = rms(y, 4800);   // settled halves
  EXPECT_NEAR(ro/ri, 1.0f, 0.05f);              // passband ~unity
}
TEST(Biquad, AttenuatesLowFrequency) {
  Biquad hp = Biquad::highpass(48000, 80.0);
  std::vector<float> x(48000), y(48000);        // 20 Hz tone, well below cutoff
  for (size_t i=0;i<x.size();++i) x[i]=std::sin(2*3.14159265*20.0*i/48000.0);
  hp.process(x.data(), y.data(), x.size());
  float ri = rms(x, 24000), ro = rms(y, 24000);
  EXPECT_LT(ro/ri, 0.3f);                        // clearly attenuated below cutoff
}
