#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "dfn3_postdsp.h"
#include "erb.h"
using namespace vc;
TEST(Dfn3Post, MaskUnityGainPreservesSpectrum){
  ErbFilterBank fb(481,32,48000.0f,2);
  std::vector<float> re(481), im(481), m(32,1.0f);
  for(int i=0;i<481;++i){ re[i]=0.5f*std::cos(0.03f*i); im[i]=0.5f*std::sin(0.03f*i); }
  auto re0=re, im0=im;
  apply_erb_mask(fb, m.data(), re.data(), im.data());
  for(int i=0;i<481;++i){ EXPECT_NEAR(re[i],re0[i],1e-5f); EXPECT_NEAR(im[i],im0[i],1e-5f); }  // gain 1 = identity
}
TEST(Dfn3Post, MaskZeroSilencesBand){
  ErbFilterBank fb(481,32,48000.0f,2);
  std::vector<float> re(481,1.0f), im(481,1.0f), m(32,1.0f);
  m[0]=0.0f;                                  // zero the lowest ERB band
  apply_erb_mask(fb, m.data(), re.data(), im.data());
  EXPECT_NEAR(re[0],0.0f,1e-6f); EXPECT_NEAR(im[0],0.0f,1e-6f);   // bin 0 is in band 0
}
TEST(Dfn3Post, DeepFilterZeroCoefsZerosLowBins){
  const int T=8,nbDf=96,order=5,la=2;
  std::vector<float> re(T*481,1.0f), im(T*481,1.0f);
  std::vector<float> coefs(T*nbDf*2*order, 0.0f);           // all-zero coefs
  apply_deep_filter(re.data(), im.data(), coefs.data(), T, nbDf, order, la);
  // low bins (<nbDf) become the zero-coef result; high bins untouched
  for(int t=0;t<T;++t){ EXPECT_NEAR(re[t*481+0],0.0f,1e-6f); EXPECT_NEAR(re[t*481+200],1.0f,1e-6f); }
}
TEST(Dfn3Post, DeepFilterLeavesHighBinsUntouched){
  const int T=4,nbDf=96,order=5,la=2;
  std::vector<float> re(T*481), im(T*481,0.0f);
  for(int i=0;i<T*481;++i) re[i]=(float)(i%7);
  auto re0=re;
  std::vector<float> coefs(T*nbDf*2*order, 0.3f);
  apply_deep_filter(re.data(), im.data(), coefs.data(), T, nbDf, order, la);
  for(int t=0;t<T;++t) for(int f=nbDf;f<481;++f) EXPECT_NEAR(re[t*481+f], re0[t*481+f], 1e-6f);  // f>=96 unchanged
}
