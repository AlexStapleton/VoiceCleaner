#pragma once
#include <cstdint>
namespace vc::ipc {
// POD parameter snapshot, UI -> worker. Fixed size, no pointers/strings, safe to memcpy into a
// double-buffered slot and read wait-free on the RT thread. Changing the layout: bump the command
// magic in command.cpp.
struct Params {
  float    hpfCutoffHz;         // high-pass cutoff
  float    agcTargetDbfs;       // AGC target = "output gain"
  float    limiterCeilingDbfs;  // brickwall ceiling
  uint32_t bypass;              // 0 / 1
  float    declickStrength;     // 0..1 transient/click ducker (0 = off)
  float    declickVoiceFloor;   // 0..1 duck fraction kept during voice (0 = fully voice-safe)
  float    gateStrength;        // 0..1 noise gate depth (0 = off; silences non-speech gaps)
  float    mixStrength;         // 0..1 engine wet/dry (1 = full denoise; RNNoise "STRENGTH")
  float    agcMaxGainDb;        // AGC max boost in dB ("MAX BOOST"); caps how far a quiet mic is raised

  static constexpr Params defaults() {
    return Params{
        /*hpfCutoffHz*/ 80.0f,
        /*agcTargetDbfs*/ -16.0f,
        /*limiterCeilingDbfs*/ -1.0f,
        /*bypass*/ 0u,
        /*declickStrength*/ 0.0f,
        /*declickVoiceFloor*/ 0.0f,
        /*gateStrength*/ 0.0f,
        /*mixStrength*/ 1.0f,
        /*agcMaxGainDb*/ 18.0f,
    };
  }
};
}  // namespace vc::ipc
