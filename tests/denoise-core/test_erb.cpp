#include <gtest/gtest.h>
#include <numeric>
#include <vector>
#include "erb.h"
using vc::ErbFilterBank;
TEST(Erb, BandWidthsSumToFreq){
  ErbFilterBank fb(481,32,48000.0f,2);
  const auto& w = fb.bandWidths();
  EXPECT_EQ((int)w.size(), 32);
  EXPECT_EQ(std::accumulate(w.begin(),w.end(),0), 481);
  for (int x : w) EXPECT_GE(x, 2);          // min_nb_erb_freqs honored
}
TEST(Erb, LowBandsNarrowerThanHigh){
  ErbFilterBank fb(481,32,48000.0f,2);
  const auto& w = fb.bandWidths();
  EXPECT_LE(w.front(), w.back());           // ERB bands widen with frequency
}
TEST(Erb, FlatSpectrumCompressesFlatAndExpandsFlat){
  ErbFilterBank fb(481,32,48000.0f,2);
  std::vector<float> spec(481,1.0f), bands(32), back(481);
  fb.compress(spec.data(), bands.data());
  for (float b : bands) EXPECT_NEAR(b, 1.0f, 1e-5f);   // mean of flat = flat
  fb.expand(bands.data(), back.data());
  for (float v : back) EXPECT_NEAR(v, 1.0f, 1e-5f);
}
TEST(Erb, ExpandBroadcastsBandValue){
  ErbFilterBank fb(481,32,48000.0f,2);
  std::vector<float> bands(32,0.0f), back(481);
  bands[0]=0.5f;                          // only lowest band set
  fb.expand(bands.data(), back.data());
  EXPECT_NEAR(back[0], 0.5f, 1e-6f);       // first bin is in band 0
}
