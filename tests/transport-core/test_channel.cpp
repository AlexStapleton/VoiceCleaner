#include <gtest/gtest.h>
#include <vector>
#include "channel.h"
using vc::extract_channel; using vc::mono_to_interleaved;

TEST(Channel, ExtractCh0FromStereo) {
  std::vector<float> inter = {1,2, 3,4, 5,6};
  std::vector<float> out(3);
  extract_channel(inter.data(), 3, 2, 0, out.data());
  EXPECT_FLOAT_EQ(out[0], 1); EXPECT_FLOAT_EQ(out[1], 3); EXPECT_FLOAT_EQ(out[2], 5);
}
TEST(Channel, ExtractPassthroughMono) {
  std::vector<float> inter = {7,8,9};
  std::vector<float> out(3);
  extract_channel(inter.data(), 3, 1, 0, out.data());
  EXPECT_FLOAT_EQ(out[0], 7); EXPECT_FLOAT_EQ(out[2], 9);
}
TEST(Channel, MonoToStereoDuplicates) {
  std::vector<float> mono = {4, 5};
  std::vector<float> out(4);
  mono_to_interleaved(mono.data(), 2, 2, out.data());
  EXPECT_FLOAT_EQ(out[0], 4); EXPECT_FLOAT_EQ(out[1], 4);
  EXPECT_FLOAT_EQ(out[2], 5); EXPECT_FLOAT_EQ(out[3], 5);
}
