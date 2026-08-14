#include <gtest/gtest.h>
#include <vector>
#include "aggression.h"
using vc::aggression_gamma; using vc::apply_aggression;
TEST(Aggression, GammaAnchors){
  EXPECT_NEAR(aggression_gamma(0.0f),0.4f,1e-5f);
  EXPECT_NEAR(aggression_gamma(0.5f),1.0f,1e-5f);
  EXPECT_NEAR(aggression_gamma(1.0f),2.5f,1e-5f);
  EXPECT_NEAR(aggression_gamma(0.25f),0.7f,1e-4f);
}
TEST(Aggression, ClampsSlider){ EXPECT_NEAR(aggression_gamma(-1.0f),0.4f,1e-5f); EXPECT_NEAR(aggression_gamma(2.0f),2.5f,1e-5f); }
TEST(Aggression, AppliesFloorAndExponent){
  std::vector<float> g={0.5f,0.01f,1.0f};
  apply_aggression(g.data(),g.size(),2.0f,0.1f);
  EXPECT_NEAR(g[0],0.25f,1e-4f);   // 0.5^2
  EXPECT_NEAR(g[1],0.1f,1e-6f);    // floored (0.01^2=0.0001 < 0.1)
  EXPECT_NEAR(g[2],1.0f,1e-6f);
}
