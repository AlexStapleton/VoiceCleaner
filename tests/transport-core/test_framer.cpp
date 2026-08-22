#include <gtest/gtest.h>
#include <vector>
#include "framer.h"
using vc::FrameAccumulator; using vc::FrameDistributor; using vc::FRAME_SAMPLES;
using vc::FloatFifo;

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

TEST(FloatFifo, WriteReadRoundTrip) {
  FloatFifo f(16);
  float in[5] = {1, 2, 3, 4, 5};
  f.write(in, 5);
  EXPECT_EQ(f.buffered(), 5u);
  float out[5] = {0};
  EXPECT_EQ(f.read(out, 5), 5u);
  for (int i = 0; i < 5; ++i) EXPECT_FLOAT_EQ(out[i], (float)(i + 1));
  EXPECT_EQ(f.buffered(), 0u);
}

TEST(FloatFifo, ReadShortOnUnderrun) {
  FloatFifo f(16);
  float in[3] = {7, 8, 9};
  f.write(in, 3);
  float out[8] = {0};
  EXPECT_EQ(f.read(out, 8), 3u);   // only 3 available
  EXPECT_FLOAT_EQ(out[0], 7.0f);
  EXPECT_FLOAT_EQ(out[2], 9.0f);
}

TEST(FloatFifo, WrapsAroundCapacityManyTimes) {
  // Small capacity, drained each round: the read/write indices must wrap without corruption.
  FloatFifo f(8);
  float phase = 0.0f;
  for (int round = 0; round < 1000; ++round) {
    float in[6];
    for (int i = 0; i < 6; ++i) in[i] = phase + (float)i;
    f.write(in, 6);
    EXPECT_EQ(f.buffered(), 6u);
    float out[6] = {0};
    EXPECT_EQ(f.read(out, 6), 6u);
    for (int i = 0; i < 6; ++i) EXPECT_FLOAT_EQ(out[i], phase + (float)i);
    EXPECT_EQ(f.buffered(), 0u);
    phase += 6.0f;
  }
}

TEST(FloatFifo, OverflowGuardDropsNewestAndStaysBounded) {
  FloatFifo f(8);   // usable capacity 8
  std::vector<float> big(100, 1.0f);
  f.write(big.data(), big.size());       // far exceeds capacity
  EXPECT_EQ(f.buffered(), 8u);           // bounded, never grows past capacity
  float out[8] = {0};
  EXPECT_EQ(f.read(out, 8), 8u);
  for (int i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(out[i], 1.0f);   // the samples it kept are intact
}
