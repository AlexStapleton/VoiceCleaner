#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include "config.h"
namespace vc {

// One [mic."<id>"] profile. 4a fields only; tuning fields (aggression/hpf/…/drift_ppm) arrive in 4c/4d.
struct MicProfile {
  std::string id;                 // WASAPI device id (the section key)
  std::string name;               // cached friendly label (UTF-8)
  std::string engine = "rnnoise"; // "none" | "rnnoise" | "dfn3"
  bool warmStandby = false;
  bool dspChain = true;
};

// The full settings.toml as the UI owns it: a [global] block + every [mic."<id>"] profile.
// Shared by the worker (via Config::load -> resolveActive) and the UI (full CRUD + save).
struct Settings {
  std::string activeCaptureId;    // [global].active_capture_id  ("" => none)
  std::string renderId;           // [global].render_id
  size_t      ringFrames = 16;    // [global].ring_frames
  std::vector<MicProfile> mics;   // all [mic."<id>"] sections

  static Settings load(const std::string& path);   // new schema OR flat back-compat; missing/bad file => empty
  void save(const std::string& path) const;        // Task 2
  MicProfile* activeProfile();                      // Task 2
  MicProfile& ensureProfile(const std::string& id, const std::string& name);  // Task 2
  Config resolveActive() const;                     // flatten active profile -> the worker's flat Config
};

}  // namespace vc
