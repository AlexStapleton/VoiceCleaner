#pragma once
#include "inoise_filter.h"
#include "dc_blocker.h"
#include "biquad.h"
#include "agc.h"
#include "limiter.h"
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
  bool  bypass_ = false;
  static constexpr float kRampCoef = 0.6065f;   // e^{-10ms/20ms}
};
}
