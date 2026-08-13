#include <gtest/gtest.h>
#include <vector>
#include "framer.h"
using vc::FrameAccumulator; using vc::FrameDistributor; using vc::FRAME_SAMPLES;

TEST(FrameAccumulator, ExactFrame) {
  FrameAccumulator a;
  std::vector<float> in(FRAME_SAMPLES);
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) in[i] = (float)i;
  a.write(in.data(), in.size());
  float out[FRAME_SAMPLES];
  ASSERT_TRUE(a.pop_frame(out));
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_FLOAT_EQ(out[i], (float)i);
  EXPECT_FALSE(a.pop_frame(out));
}

TEST(FrameAccumulator, SmallChunksCarryRemainder) {
  FrameAccumulator a;
  float out[FRAME_SAMPLES];
  float chunk[128];
  for (int i = 0; i < 128; ++i) chunk[i] = 1.0f;
  a.write(chunk, 128); EXPECT_FALSE(a.pop_frame(out));
  a.write(chunk, 128); EXPECT_FALSE(a.pop_frame(out));
  a.write(chunk, 128); EXPECT_FALSE(a.pop_frame(out));
  a.write(chunk, 128); EXPECT_TRUE(a.pop_frame(out));   // 512 buffered -> 1 frame, 32 remain
  EXPECT_FALSE(a.pop_frame(out));
}

TEST(FrameAccumulator, MultipleFramesFromLargeWrite) {
  FrameAccumulator a;
  std::vector<float> big(FRAME_SAMPLES * 3 + 17, 0.5f);
  a.write(big.data(), big.size());
  float out[FRAME_SAMPLES];
  EXPECT_TRUE(a.pop_frame(out));
  EXPECT_TRUE(a.pop_frame(out));
  EXPECT_TRUE(a.pop_frame(out));
  EXPECT_FALSE(a.pop_frame(out));   // 17 remain
}

TEST(FrameDistributor, ReadShortOnUnderrun) {
  FrameDistributor d;
  float frame[FRAME_SAMPLES];
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) frame[i] = 2.0f;
  d.push_frame(frame);
  std::vector<float> out(FRAME_SAMPLES + 10, -1.0f);
  size_t got = d.read(out.data(), out.size());
  EXPECT_EQ(got, FRAME_SAMPLES);              // only one frame available
  EXPECT_FLOAT_EQ(out[0], 2.0f);
}

TEST(FrameDistributor, ReadAcrossFrameBoundary) {
  FrameDistributor d;
  float f1[FRAME_SAMPLES], f2[FRAME_SAMPLES];
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) { f1[i] = 1.0f; f2[i] = 3.0f; }
  d.push_frame(f1); d.push_frame(f2);
  std::vector<float> out(FRAME_SAMPLES + 5);
  EXPECT_EQ(d.read(out.data(), out.size()), FRAME_SAMPLES + 5);
  EXPECT_FLOAT_EQ(out[FRAME_SAMPLES - 1], 1.0f);
  EXPECT_FLOAT_EQ(out[FRAME_SAMPLES], 3.0f);
}
