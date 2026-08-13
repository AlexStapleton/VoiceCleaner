#include <gtest/gtest.h>
#include <filesystem>
#include <cmath>
#include "wav.h"
using vc::WavData; using vc::wav_read; using vc::wav_write;

static std::string tmp(const char* name){ return (std::filesystem::temp_directory_path()/name).string(); }

TEST(Wav, RoundTripFloat32) {
  WavData w; w.sampleRate=48000; w.channels=1;
  for (int i=0;i<500;++i) w.samples.push_back(std::sin(0.05f*i));
  auto p = tmp("vc_wav_f32.wav");
  ASSERT_TRUE(wav_write(p, w, true));
  WavData r; ASSERT_TRUE(wav_read(p, r));
  EXPECT_EQ(r.sampleRate,48000u); EXPECT_EQ(r.channels,1u);
  ASSERT_EQ(r.samples.size(), w.samples.size());
  for (size_t i=0;i<w.samples.size();++i) EXPECT_NEAR(r.samples[i], w.samples[i], 1e-6f);
}

TEST(Wav, RoundTripPcm16) {
  WavData w; w.sampleRate=48000; w.channels=1;
  for (int i=0;i<500;++i) w.samples.push_back(0.5f*std::sin(0.05f*i));
  auto p = tmp("vc_wav_pcm16.wav");
  ASSERT_TRUE(wav_write(p, w, false));
  WavData r; ASSERT_TRUE(wav_read(p, r));
  ASSERT_EQ(r.samples.size(), w.samples.size());
  for (size_t i=0;i<w.samples.size();++i) EXPECT_NEAR(r.samples[i], w.samples[i], 1.0f/32767.0f + 1e-4f);
}

TEST(Wav, ReadMissingFileFails) {
  WavData r; EXPECT_FALSE(wav_read(tmp("vc_wav_does_not_exist_zzz.wav"), r));
}
