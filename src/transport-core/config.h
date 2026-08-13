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
  static Config load(const std::string& tomlPath);
  void apply_cli(int argc, char** argv);
  void validate();
};
}
