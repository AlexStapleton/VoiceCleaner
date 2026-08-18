#include <gtest/gtest.h>
#include "slider.h"
using namespace vc::ui;

TEST(Slider, FractionAndValueRoundTrip) {
  Slider g(-30.0f, -6.0f, -20.0f, SliderFmt::Dbfs);
  EXPECT_NEAR(g.fraction(), ((-20.0f) - (-30.0f)) / ((-6.0f) - (-30.0f)), 1e-5f);  // 10/24
  g.setFraction(0.0f); EXPECT_FLOAT_EQ(g.value(), -30.0f);
  g.setFraction(1.0f); EXPECT_FLOAT_EQ(g.value(), -6.0f);
  g.setFraction(0.5f); EXPECT_FLOAT_EQ(g.value(), -18.0f);
}
TEST(Slider, ClampsOutOfRange) {
  Slider g(-30.0f, -6.0f, -20.0f, SliderFmt::Dbfs);
  g.setValue(-100.0f); EXPECT_FLOAT_EQ(g.value(), -30.0f);
  g.setValue(100.0f);  EXPECT_FLOAT_EQ(g.value(), -6.0f);
  g.setFraction(-1.0f); EXPECT_FLOAT_EQ(g.value(), -30.0f);
  g.setFraction(2.0f);  EXPECT_FLOAT_EQ(g.value(), -6.0f);
}
TEST(Slider, FormatsPerUnit) {
  EXPECT_EQ(Slider(-30,-6,-20,SliderFmt::Dbfs).format(), "-20 dBFS");
  EXPECT_EQ(Slider(0,1,0.5f,SliderFmt::Ratio2).format(), "0.50");
  EXPECT_EQ(Slider(40,200,80,SliderFmt::Hz).format(), "80 Hz");
  EXPECT_EQ(Slider(-6,0,-1,SliderFmt::Db).format(), "-1 dB");
}
