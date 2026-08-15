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
  std::string engine   = "none";   // "none" (passthrough) | "rnnoise" | "dfn3"
  bool        dspChain = false;     // wrap engine in DC->HPF->AGC->limiter chain
  bool        warmStandby = false;  // run rnnoise+dfn3, live switch
  static Config load(const std::string& tomlPath);
  void apply_cli(int argc, char** argv);
  void validate();
};
}
