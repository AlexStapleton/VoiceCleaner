#pragma once
#include "inoise_filter.h"
#include "dc_blocker.h"
#include "biquad.h"
#include "agc.h"
#include "limiter.h"
#include "declicker.h"
#include "noise_gate.h"
#include "dry_wet_mix.h"
#include "frame.h"
namespace vc {
// Composes the DSP leaves around the engine, per 480-sample frame:
//   DC blocker -> ENGINE -> wet/dry mix -> high-pass -> declicker -> VAD-gated AGC -> noise gate -> limiter.
// Only the DC blocker precedes the engine. Slider params ramp (~20 ms) toward their targets.
class DspChain {
public:
  struct Config {
    float fs = 48000.0f;
    float hpfCutoffHz = 80.0f;
    float agcTargetDb = -16.0f;
    float agcMaxGainDb = 18.0f;     // AGC max boost ("MAX BOOST"); quiet mics need more than the old fixed +9 dB
    float limiterCeilingDb = -1.0f;
    float mixStrength = 1.0f;       // 0..1 engine wet/dry (1 = full denoise; RNNoise "STRENGTH")
    float declickStrength = 0.0f;   // 0..1 transient/click ducker (0 = off)
    float declickVoiceFloor = 0.0f; // 0..1 fraction of the duck kept during voice (0 = fully voice-safe)
    float gateStrength = 0.0f;      // 0..1 noise gate depth (0 = off; silences non-speech gaps)
    float vadOpen = 0.6f;      // hysteresis open threshold
    float vadClose = 0.3f;     // hysteresis close threshold
    float vadHoldMs = 200.0f;  // keep AGC active this long after VAD drops
  };
  DspChain(INoiseFilter* engine, const Config& c);
  void process_frame(const float* in, float* out) noexcept;   // FRAME_SAMPLES in/out
  void reset() noexcept;
  bool vadActive() const noexcept { return vadActive_; }
  float currentAgcGainDb() const noexcept { return agc_.currentGainDb(); }   // live AGC gain (readout)
  void setHpfCutoffHz(float hz) noexcept          { hpfTarget_ = hz; }
  void setAgcTargetDb(float db) noexcept          { agcTarget_ = db; }
  void setAgcMaxGainDb(float db) noexcept         { agc_.setMaxGainDb(db); }
  float currentAgcMaxGainDb() const noexcept      { return agc_.maxGainDb(); }
  void setLimiterCeilingDb(float db) noexcept     { limTarget_ = db; }
  void setMixStrength(float s) noexcept           { mixTarget_ = s; }
  void setDeclickStrength(float s) noexcept       { dckTarget_ = s; }
  void setDeclickVoiceFloor(float f) noexcept     { declick_.setVoiceFloor(f); }
  void setGateStrength(float s) noexcept          { gateTarget_ = s; }
  void setBypass(bool on) noexcept                { bypass_ = on; }
  float currentHpfCutoffHz() const noexcept       { return hpfCur_; }
  float currentAgcTargetDb() const noexcept       { return agcCur_; }
  float currentLimiterCeilingDb() const noexcept  { return limCur_; }
private:
  INoiseFilter* engine_;
  DcBlocker dc_;
  Biquad    hpf_;
  Agc       agc_;
  Limiter   lim_;
  Declicker declick_;
  NoiseGate gate_;
  DryWetMix mix_;
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
  float gateCur_, gateTarget_;
  float mixCur_, mixTarget_;
  bool  bypass_ = false;
  static constexpr float kRampCoef = 0.6065f;   // e^{-10ms/20ms}
};
}
