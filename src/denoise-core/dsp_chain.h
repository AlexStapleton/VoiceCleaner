#pragma once
#include "inoise_filter.h"
#include "dc_blocker.h"
#include "biquad.h"
#include "agc.h"
#include "limiter.h"
#include "declicker.h"
#include "frame.h"
namespace vc {
// Composes the DSP leaves per blueprint §7.1:
//   DC blocker -> ENGINE -> high-pass -> VAD-gated AGC -> limiter.
// Only the DC blocker precedes the engine.
class DspChain {
public:
  struct Config {
    float fs = 48000.0f;
    float hpfCutoffHz = 80.0f;
    float agcTargetDb = -20.0f;
    float limiterCeilingDb = -1.0f;
    float declickStrength = 0.0f;   // 0..1 transient/click ducker (0 = off)
    float declickVoiceFloor = 0.0f; // 0..1 fraction of the duck kept during voice (0 = fully voice-safe)
    float vadOpen = 0.6f;      // hysteresis open threshold
    float vadClose = 0.3f;     // hysteresis close threshold
    float vadHoldMs = 200.0f;  // keep AGC active this long after VAD drops
  };
  DspChain(INoiseFilter* engine, const Config& c);
  void process_frame(const float* in, float* out) noexcept;   // FRAME_SAMPLES in/out
  void reset() noexcept;
  bool vadActive() const noexcept { return vadActive_; }
  void setHpfCutoffHz(float hz) noexcept          { hpfTarget_ = hz; }
  void setAgcTargetDb(float db) noexcept          { agcTarget_ = db; }
  void setLimiterCeilingDb(float db) noexcept     { limTarget_ = db; }
  void setDeclickStrength(float s) noexcept       { dckTarget_ = s; }
  void setDeclickVoiceFloor(float f) noexcept     { declick_.setVoiceFloor(f); }
  void setBypass(bool on) noexcept                { bypass_ = on; }
  float currentHpfCutoffHz() const noexcept       { return hpfCur_; }
  float currentAgcTargetDb() const noexcept       { return agcCur_; }
  float currentLimiterCeilingDb() const noexcept  { return limCur_; }
  float currentDeclickStrength() const noexcept   { return dckCur_; }
  float currentDeclickVoiceFloor() const noexcept { return declick_.currentVoiceFloor(); }
private:
  INoiseFilter* engine_;
  DcBlocker dc_;
  Biquad    hpf_;
  Agc       agc_;
  Limiter   lim_;
  Declicker declick_;
  float open_;
  float close_;
  bool  vadActive_ = false;
  int   holdLeft_  = 0;
  int   holdSamples_;
  float t1_[FRAME_SAMPLES];
  float t2_[FRAME_SAMPLES];
  float fs_;
  float hpfCur_, hpfTarget_;
  float agcCur_, agcTarget_;
  float limCur_, limTarget_;
  float dckCur_, dckTarget_;
  bool  bypass_ = false;
  static constexpr float kRampCoef = 0.6065f;   // e^{-10ms/20ms}
};
}
