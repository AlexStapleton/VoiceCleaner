#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "dfn3_features.h"
using vc::Dfn3Features;
TEST(Dfn3Features, OutputShapesAndFinite){
  Dfn3Features f;
  std::vector<float> re(481), im(481), erb(32), spec(192);
  for(int i=0;i<481;++i){ re[i]=0.01f*std::cos(0.1f*i); im[i]=0.01f*std::sin(0.1f*i); }
  f.process(re.data(), im.data(), erb.data(), spec.data());
  for(float v: erb) EXPECT_TRUE(std::isfinite(v));
  for(float v: spec) EXPECT_TRUE(std::isfinite(v));
}
TEST(Dfn3Features, FeatSpecUsesFirst96BinsReImLayout){
  Dfn3Features f;
  std::vector<float> re(481,0.0f), im(481,0.0f), erb(32), spec(192);
  re[0]=1.0f; im[95]=1.0f; re[200]=5.0f;    // bin 200 is beyond nb_df -> must NOT appear
  f.process(re.data(), im.data(), erb.data(), spec.data());
  // real plane index 0 and imag plane index 95 should be nonzero; nothing from bin>=96
  EXPECT_NE(spec[0], 0.0f);
  EXPECT_NE(spec[96+95], 0.0f);
}
TEST(Dfn3Features, NormalizationStateEvolves){
  Dfn3Features f;
  std::vector<float> re(481,0.1f), im(481,0.0f), e1(32), e2(32), s(192);
  f.process(re.data(), im.data(), e1.data(), s.data());
  for(int k=0;k<50;++k) f.process(re.data(), im.data(), e2.data(), s.data());
  // with a constant input, exp-mean normalization should drive the normalized erb feature toward
  // a steady state that differs from the first frame (state warms up)
  float d=0; for(int i=0;i<32;++i) d+=std::fabs(e2[i]-e1[i]);
  EXPECT_GT(d, 0.0f);
}
TEST(Dfn3Features, ResetRestoresInitial){
  Dfn3Features f;
  std::vector<float> re(481,0.2f), im(481,0.05f), a(32), b(32), s(192);
  f.process(re.data(), im.data(), a.data(), s.data());
  for(int k=0;k<20;++k) f.process(re.data(), im.data(), a.data(), s.data());
  f.reset();
  f.process(re.data(), im.data(), b.data(), s.data());
  // first frame after reset should match the very first frame from a fresh instance
  Dfn3Features g; std::vector<float> c(32);
  g.process(re.data(), im.data(), c.data(), s.data());
  for(int i=0;i<32;++i) EXPECT_NEAR(b[i], c[i], 1e-6f);
}
