#include <gtest/gtest.h>
#include <vector>
#include <cmath>
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
