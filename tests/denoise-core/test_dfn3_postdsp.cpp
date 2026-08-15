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
TEST(Dfn3Post, StreamingRingMatchesWholeBufferBothOffsets){
  const int T=12, nbDf=96, order=5, kNFreq=481;
  // Deterministic pseudo-random spectrum and coefs.
  std::vector<float> re(T*kNFreq), im(T*kNFreq);
  for(int i=0;i<T*kNFreq;++i){
    re[i]=std::sin(0.017f*i+0.3f)*std::cos(0.005f*i);
    im[i]=std::cos(0.023f*i+1.1f)*std::sin(0.009f*i+0.4f);
  }
  std::vector<float> coefs(T*nbDf*order*2);
  for(size_t i=0;i<coefs.size();++i) coefs[i]=std::sin(0.013f*(float)i+0.7f)*0.5f;

  for(int leftPad : {2,4}){
    const int future = order-1-leftPad;   // 2 for lp=2, 0 for lp=4
    // Whole-buffer reference on a copy.
    std::vector<float> refRe=re, refIm=im;
    apply_deep_filter(refRe.data(), refIm.data(), coefs.data(), T, nbDf, order, leftPad);

    // Drive the streaming ring; start result as a copy so high bins (>=nbDf)
    // stay = original and can be asserted untouched by the ring.
    std::vector<float> resRe=re, resIm=im;
    DeepFilterRing ring(nbDf, order, leftPad);
    std::vector<float> oRe(nbDf), oIm(nbDf);
    int outT=-1;
    for(int t=0;t<T;++t){
      if(ring.push(&re[t*kNFreq], &im[t*kNFreq], &coefs[(size_t)t*nbDf*order*2],
                   outT, oRe.data(), oIm.data())){
        ASSERT_GE(outT,0); ASSERT_LT(outT,T);
        for(int b=0;b<nbDf;++b){ resRe[outT*kNFreq+b]=oRe[b]; resIm[outT*kNFreq+b]=oIm[b]; }
      }
    }
    while(ring.flush(outT, oRe.data(), oIm.data())){
      ASSERT_GE(outT,0); ASSERT_LT(outT,T);
      for(int b=0;b<nbDf;++b){ resRe[outT*kNFreq+b]=oRe[b]; resIm[outT*kNFreq+b]=oIm[b]; }
    }

    // Every low bin of every frame matches whole-buffer reference.
    for(int t=0;t<T;++t) for(int b=0;b<nbDf;++b){
      EXPECT_NEAR(resRe[t*kNFreq+b], refRe[t*kNFreq+b], 1e-5f) << "lp="<<leftPad<<" t="<<t<<" b="<<b;
      EXPECT_NEAR(resIm[t*kNFreq+b], refIm[t*kNFreq+b], 1e-5f) << "lp="<<leftPad<<" t="<<t<<" b="<<b;
    }
    // High bins (>=nbDf) untouched by the ring (still original spectrum).
    for(int t=0;t<T;++t) for(int f=nbDf;f<kNFreq;++f){
      EXPECT_NEAR(resRe[t*kNFreq+f], re[t*kNFreq+f], 1e-6f) << "lp="<<leftPad<<" hi t="<<t<<" f="<<f;
      EXPECT_NEAR(resIm[t*kNFreq+f], im[t*kNFreq+f], 1e-6f) << "lp="<<leftPad<<" hi t="<<t<<" f="<<f;
    }
    (void)future;
  }
}
