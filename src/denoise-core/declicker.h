#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// Voice-safe transient/click suppressor (design §3). Time-domain: HF-emphasis -> fast/slow envelopes ->
// crest detection -> look-ahead gain duck whose depth scales with (1 - vad). RT-safe: no allocation in
// process(); fixed internal buffers. out must NOT alias in (there is a lookahead delay).
class Declicker {
 public:
  struct Config {
    float  fs = 48000.0f;
    size_t lookahead = 144;   // 3 ms delay so the duck is fully down before the click emerges
    float  hpHz = 3000.0f;    // detection HF-emphasis (RC high-pass) cutoff
    float  fastMs = 0.5f;     // fast peak-envelope decay
    float  slowMs = 50.0f;    // slow baseline envelope
    float  holdMs = 2.0f;     // hold the duck this long past detection (covers the pipe + click width)
    float  releaseMs = 6.0f;  // gain recovery after the hold
    float  crestK = 6.0f;     // env_fast must exceed crestK * env_slow to flag a click
    float  voiceFloor = 0.0f; // fraction of the duck kept during full voice (vad=1). 0 = fully voice-safe
                              // (clicks leak under speech); 1 = ignore vad (duck every detected click).
  };
  explicit Declicker(const Config& c);
  // strength in [0,1] (0 = bypass, output = input delayed by lookahead). vad in [0,1] scales duck depth down.
  void  process(const float* in, float* out, size_t n, float strength, float vad) noexcept;
  void  setStrength(float s) noexcept;
  float currentStrength() const noexcept { return strength_; }
  void  setVoiceFloor(float f) noexcept;                              // live: duck fraction kept under voice
  float currentVoiceFloor() const noexcept { return voiceFloor_; }
  void  reset() noexcept;
 private:
  size_t L_;
  float  hpA_;                          // RC high-pass coefficient
  float  fastCoef_, slowCoef_, relCoef_;
  size_t holdSamples_;
  float  crestK_ = 6.0f;
  float  voiceFloor_ = 0.0f;
  float  hpPrevIn_ = 0.0f, hpPrevOut_ = 0.0f;
  float  envF_ = 0.0f, envS_ = 1e-6f;
  float  gain_ = 1.0f;
  int    holdLeft_ = 0;
  float  strength_ = 0.0f;
  std::vector<float> delay_;
  size_t widx_ = 0;
};
}  // namespace vc
