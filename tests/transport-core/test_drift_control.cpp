#include <gtest/gtest.h>
#include "drift_control.h"
using vc::LeakyIntegrator; using vc::DriftController;

TEST(LeakyIntegrator, ConvergesToConstant) {
  LeakyIntegrator f(0.1);
  double y=0; for(int i=0;i<200;++i) y=f.update(10.0);
  EXPECT_NEAR(y, 10.0, 0.01);
}
TEST(DriftController, DrainingRaisesRatioAboveOne) {
  DriftController c({512, 1e-6, 1e-8, 150, 0.2});
  double ratio=1.0; for (int i=0;i<2000;++i) ratio = c.update(480.0);
  EXPECT_GT(ratio, 1.0);
  EXPECT_LE(ratio, 1.0 + 150e-6 + 1e-9);
}
TEST(DriftController, AtSetpointRatioNearOne) {
  DriftController c({512, 1e-6, 1e-8, 150, 0.2});
  double ratio=1.0; for (int i=0;i<5000;++i) ratio = c.update(512.0);
  EXPECT_NEAR(ratio, 1.0, 5e-6);
}
TEST(DriftController, RatioClampedToMaxPpm) {
  DriftController c({512, 1e-2, 1e-3, 100, 0.5});
  double ratio=1.0; for (int i=0;i<100;++i) ratio = c.update(0.0);
  EXPECT_LE(ratio, 1.0 + 100e-6 + 1e-9);
}
