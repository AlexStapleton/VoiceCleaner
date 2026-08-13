#include <gtest/gtest.h>
#include "passthrough_filter.h"
#include "frame.h"
using vc::PassthroughFilter;
TEST(Passthrough, CopiesInputToOutput) {
  PassthroughFilter f;
  float in[vc::FRAME_SAMPLES], out[vc::FRAME_SAMPLES];
  for (size_t i=0;i<vc::FRAME_SAMPLES;++i) { in[i]=(float)i*0.001f; out[i]=-1.0f; }
  f.ProcessFrame(in, out);
  for (size_t i=0;i<vc::FRAME_SAMPLES;++i) EXPECT_FLOAT_EQ(out[i], in[i]);
  EXPECT_EQ(f.AlgorithmicDelaySamples(), 0u);
}
