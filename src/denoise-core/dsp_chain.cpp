#include "dsp_chain.h"

namespace vc {

DspChain::DspChain(INoiseFilter* engine, const Config& c)
    : engine_(engine),
      hpf_(Biquad::highpass(c.fs, c.hpfCutoffHz)),
      agc_({c.agcTargetDb, 50.0f, 500.0f, 18.0f, -10.0f, c.fs}),
      lim_(c.limiterCeilingDb, 144, 50.0f, c.fs),
      declick_(Declicker::Config{c.fs}),
      open_(c.vadOpen),
      close_(c.vadClose),
      holdSamples_((int)(c.vadHoldMs * 0.001f * c.fs)),
      fs_(c.fs),
      hpfCur_(c.hpfCutoffHz), hpfTarget_(c.hpfCutoffHz),
      agcCur_(c.agcTargetDb), agcTarget_(c.agcTargetDb),
      limCur_(c.limiterCeilingDb), limTarget_(c.limiterCeilingDb),
      dckCur_(c.declickStrength), dckTarget_(c.declickStrength) {}

void DspChain::process_frame(const float* in, float* out) noexcept {
  if (bypass_) {
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) out[i] = in[i];
    return;
  }
  // Ramp params toward targets (one-pole, ~20 ms) and push into the leaves.
  hpfCur_ = kRampCoef * hpfCur_ + (1.0f - kRampCoef) * hpfTarget_;
  agcCur_ = kRampCoef * agcCur_ + (1.0f - kRampCoef) * agcTarget_;
  limCur_ = kRampCoef * limCur_ + (1.0f - kRampCoef) * limTarget_;
  hpf_.setHighpass(fs_, hpfCur_);
  agc_.setTargetDb(agcCur_);
  lim_.setCeilingDb(limCur_);
  // 1. DC blocker (only stage before the engine).
  dc_.process(in, t1_, FRAME_SAMPLES);
  // 2. Engine.
  engine_->ProcessFrame(t1_, t2_);
  float vad = engine_->LastVadProbability();
  // 3. High-pass.
  hpf_.process(t2_, t1_, FRAME_SAMPLES);          // t1_ = HPF output
  // 3b. Declicker (ramped strength; VAD scales the duck down).
  dckCur_ = kRampCoef * dckCur_ + (1.0f - kRampCoef) * dckTarget_;
  declick_.process(t1_, t2_, FRAME_SAMPLES, dckCur_, vad);   // t2_ = declicked
  // 4. VAD hysteresis + hold (per-frame).
  if (vad >= open_) {
    vadActive_ = true;
    holdLeft_ = holdSamples_;
  } else if (vad < close_) {
    if (holdLeft_ > 0)
      holdLeft_ -= (int)FRAME_SAMPLES;
    else
      vadActive_ = false;
  }
  // 5. VAD-gated AGC.
  agc_.process(t2_, t1_, FRAME_SAMPLES, vadActive_);   // t2_ (declicked) -> t1_
  // 6. Limiter.
  lim_.process(t1_, out, FRAME_SAMPLES);               // t1_ -> out
}

void DspChain::reset() noexcept {
  dc_.reset();
  hpf_.reset();
  agc_.reset();
  lim_.reset();
  declick_.reset(); dckCur_ = dckTarget_;
  vadActive_ = false;
  holdLeft_ = 0;
  hpfCur_ = hpfTarget_; agcCur_ = agcTarget_; limCur_ = limTarget_;
}

}  // namespace vc
