#pragma once
#include <cstdint>
namespace vc::ipc {
// POD parameter snapshot, UI -> worker (blueprint §8.3). Fixed size, no pointers/strings,
// safe to memcpy into a double-buffered slot and read wait-free on the RT thread.
struct Params {
  float    aggression;          // 0..1 (engine mask shaping; wired in a later phase)
  float    hpfCutoffHz;         // high-pass cutoff
  float    agcTargetDbfs;       // AGC target = "output gain"
  float    limiterCeilingDbfs;  // brickwall ceiling
  uint32_t engine;              // 0 = RNNoise, 1 = DFN3
  uint32_t bypass;              // 0 / 1
  uint32_t dspChain;            // 0 / 1 — set at worker start via Config; relay does not consume live yet (deferred)
  uint32_t driftComp;           // 0 / 1 — set at worker start via Config; relay does not consume live yet (deferred)
  uint32_t dryTap;              // 0 / 1 (placeholder; behavior deferred)
  float    declickStrength;     // 0..1 transient/click ducker (0 = off)
  float    declickVoiceFloor;   // 0..1 duck fraction kept during voice (0 = fully voice-safe)
  float    gateStrength;        // 0..1 noise gate depth (0 = off; silences non-speech gaps)
  float    mixStrength;         // 0..1 engine wet/dry (1 = full denoise; RNNoise "STRENGTH")

  static constexpr Params defaults() {
    return Params{
        /*aggression*/ 0.0f,
        /*hpfCutoffHz*/ 80.0f,
        /*agcTargetDbfs*/ -20.0f,
        /*limiterCeilingDbfs*/ -1.0f,
        /*engine*/ 1u,
        /*bypass*/ 0u,
        /*dspChain*/ 1u,
        /*driftComp*/ 1u,
        /*dryTap*/ 0u,
        /*declickStrength*/ 0.0f,
        /*declickVoiceFloor*/ 0.0f,
        /*gateStrength*/ 0.0f,
        /*mixStrength*/ 1.0f,
    };
  }
};
}  // namespace vc::ipc
