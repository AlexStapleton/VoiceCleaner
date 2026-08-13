#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "resampler.h"
using vc::Resampler;

TEST(Resampler, UnityRatioApproxIdentity) {
  Resampler r; r.set_ratio(1.0);
  std::vector<float> in(480), out(600);
  for (size_t i=0;i<480;++i) in[i]=std::sin(0.05f*i);
  size_t n = r.process(in.data(), in.size(), out.data(), out.size());
  EXPECT_GE(n, 470u); EXPECT_LE(n, 481u);
}
TEST(Resampler, RatioChangesOutputLength) {
  Resampler r; r.set_ratio(1.10);
  std::vector<float> in(1000, 1.0f), out(2000);
  size_t total_in=0, total_out=0;
  for (int b=0;b<10;++b){ total_out += r.process(in.data(),1000,out.data(),out.size()); total_in+=1000; }
  double measured = (double)total_out/total_in;
  EXPECT_NEAR(measured, 1.10, 0.02);
}
TEST(Resampler, LinearRampStaysLinear) {
  Resampler r; r.set_ratio(1.0);
  std::vector<float> in(480), out(600);
  for (size_t i=0;i<480;++i) in[i]=(float)i;
  r.process(in.data(),480,out.data(),out.size());
  for (size_t i=0;i<480;++i) in[i]=(float)(480+i);
  size_t n = r.process(in.data(),480,out.data(),out.size());
  ASSERT_GT(n, 10u);
  for (size_t i=1;i<n;++i) EXPECT_NEAR(out[i]-out[i-1], 1.0f, 1e-3f);
}
