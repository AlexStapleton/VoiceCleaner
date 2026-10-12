#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// Slowly varying input-level estimator for DFN3's pre-gain (spec Part 1). Per 480-sample frame it
// computes the frame RMS in dB, keeps the last `windowFrames` non-silent values and takes the
// `percentile` of them as the level L (robust to sparse loud clicks, unlike a peak hold).
// Gain = clamp(targetDbfs - L, 0, maxGainDb), slew-limited: fastSlewDbPerSec for the first
// fastPhaseSec after construction, slewDbPerSec afterwards. Never attenuates.
// No allocation after construction; one log10 per frame.
class InputLevelNormalizer {
public:
  struct Config {
    float targetDbfs = -40.0f;      // calibrated: the -45 dBFS-peak synthetic clip settles at about +20 dB (eval note)
    float maxGainDb = 30.0f;
    int   windowFrames = 300;       // ~3 s
    float percentile = 0.75f;
    float silenceDbfs = -80.0f;     // frames quieter than this are ignored
    float slewDbPerSec = 3.0f;
    float fastSlewDbPerSec = 30.0f;
    float fastPhaseSec = 2.0f;
  };
  InputLevelNormalizer() : InputLevelNormalizer(Config{}) {}
  explicit InputLevelNormalizer(const Config& c);
  // `frame` holds exactly 480 samples. Returns the gain in dB to apply from this frame on.
  float process(const float* frame) noexcept;
  float gainDb() const noexcept { return gain_; }
  const Config& config() const noexcept { return cfg_; }
private:
  Config cfg_;
  std::vector<float> ring_;     // insertion order, size windowFrames
  std::vector<float> sorted_;   // ascending, first count_ valid
  size_t head_ = 0, count_ = 0;
  float gain_ = 0.0f;
  long frames_ = 0;
};
}
