#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include "config.h"
namespace vc {

// One [mic."<id>"] profile: every tuning slider for that microphone.
struct MicProfile {
  std::string id;                 // WASAPI device id (the section key)
  std::string name;               // cached friendly label (UTF-8)
  bool dspChain = true;
  float hpfHz          = 80.0f;   // .hpf_hz
  float outputGainDbfs = -16.0f;  // .output_gain_dbfs  (== agcTargetDbfs)
  float maxBoostDb = 18.0f;       // .max_boost_db      (== agcMaxGainDb)
  float limiterDbfs    = -1.0f;   // .limiter_dbfs
  float declickStrength = 0.0f;   // .declick_strength
  float declickVoiceFloor = 0.0f; // .declick_voice_floor
  float gateStrength = 0.0f;      // .gate_strength
  float mixStrength = 1.0f;       // .mix_strength (1 = full denoise)
};

// The full settings.toml as the UI owns it: a [global] block + every [mic."<id>"] profile.
// Shared by the worker (via Config::load -> resolveActive) and the UI (full CRUD + save).
struct Settings {
  std::string activeCaptureId;    // [global].active_capture_id  ("" => none)
  std::string renderId;           // [global].render_id
  size_t      ringFrames = 8;     // [global].ring_frames, clamped to [kRingFramesMin, kRingFramesMax] (8 ~= 40 ms)
  unsigned    collapsedSections = 0;  // [global].collapsed_sections — UI slider-group collapse bitmask
  std::vector<MicProfile> mics;   // all [mic."<id>"] sections

  static Settings load(const std::string& path);   // UTF-8 path; missing/bad file => empty (first run)
  bool save(const std::string& path) const;        // UTF-8 path; atomic (temp + rename); false => target left untouched
  MicProfile* activeProfile();                      // profile of activeCaptureId, or nullptr
  MicProfile& ensureProfile(const std::string& id, const std::string& name);  // find or create (defaults)
  Config resolveActive() const;                     // flatten active profile -> the worker's flat Config
};

}  // namespace vc
