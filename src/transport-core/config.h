#pragma once
#include <string>
#include <cstddef>
namespace vc {
// LATENCY (ring_frames) bounds, shared by the settings loader, the worker and the UI's LATENCY box.
// Buffered delay ~= ring_frames x 5 ms (the ring is primed half full). Below 4 the half-full reserve
// is 1 frame or less and ordinary scheduling jitter causes dropouts; 512 is ~2.5 s.
inline constexpr size_t kRingFramesMin = 4;
inline constexpr size_t kRingFramesMax = 512;
inline size_t clampRingFrames(long long v) {
  return v < (long long)kRingFramesMin ? kRingFramesMin
       : v > (long long)kRingFramesMax ? kRingFramesMax : (size_t)v;
}

// The worker's runtime configuration: the active mic's profile flattened out of settings.toml
// (Settings::resolveActive), plus developer command-line overrides (apply_cli).
struct Config {
  std::string captureDeviceId;
  std::string renderDeviceId;
  size_t      ringFrames = 8;
  std::string csvPath;              // drift/occupancy CSV log; empty = off (developer: --csv <path>)
  // Drift controller tuning (fixed; see DriftController).
  bool   driftCompensation = true;
  double kp             = 2e-7;
  double ki             = 2e-9;
  double driftMaxPpm    = 150.0;
  double filterAlpha    = 0.02;
  std::string engine   = "rnnoise";   // "rnnoise" (the only engine) | "none" = developer passthrough (--engine none)
  bool        dspChain = false;     // wrap the engine in the DSP chain (see DspChain)
  static Config load(const std::string& tomlPath);   // settings.toml ([global] + [mic."<id>"]); throws on a bad file
  void apply_cli(int argc, char** argv);
  void validate();
};
}
