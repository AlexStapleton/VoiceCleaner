#pragma once
#include <string>
#include <cstddef>
namespace vc {
struct Config {
  std::string captureDeviceId;
  std::string renderDeviceId;
  size_t      ringFrames = 1024;
  std::string csvPath = "drift.csv";
  int         logLevel = 1;
  bool   driftCompensation = true;
  double kp             = 2e-7;
  double ki             = 2e-9;
  double driftMaxPpm    = 150.0;
  double filterAlpha    = 0.02;
  std::string engine   = "none";   // "none" (passthrough) | "rnnoise" | "dfn3" = standard DeepFilterNet3 (models/dfn3), ~30 ms latency
  bool        dspChain = false;     // wrap engine in DC->HPF->AGC->limiter chain
  bool        warmStandby = false;  // run rnnoise+dfn3 both warm, live switch (keys 1/2). In this mode
                                    // `engine` selects the DEFAULT-active engine at startup: "dfn3" -> DFN3, else RNNoise.
  bool        lowLatency = false;   // skip the DFN3 delay pin so RNNoise runs at native ~0 latency
                                    // (switching to DFN3 then causes a one-time ~30 ms timing shift)
  static Config load(const std::string& tomlPath);
  void apply_cli(int argc, char** argv);
  void validate();
};
}
