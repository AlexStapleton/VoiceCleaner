#include "config.h"
#include "settings.h"
#include <toml++/toml.hpp>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
namespace vc {
Config Config::load(const std::string& path) {
  // Parse once here so a missing/corrupt file is an error for the worker (the UI's Settings::load is
  // deliberately tolerant and would silently yield empty settings).
  toml::table t;
  try { t = toml::parse_file(path); }
  catch (const toml::parse_error& e) { throw std::runtime_error(std::string("config parse: ") + e.description().data()); }
  if (!t.contains("global"))
    throw std::runtime_error("config: no [global] table (the old flat config format is no longer supported)");
  Config c = Settings::load(path).resolveActive();
  c.validate();
  return c;
}
void Config::apply_cli(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--capture") && i + 1 < argc) captureDeviceId = argv[++i];
    else if (!std::strcmp(argv[i], "--render") && i + 1 < argc) renderDeviceId = argv[++i];
    else if (!std::strcmp(argv[i], "--csv") && i + 1 < argc) csvPath = argv[++i];
    else if (!std::strcmp(argv[i], "--ring") && i + 1 < argc) ringFrames = clampRingFrames(std::atoll(argv[++i]));
    // Any other token (valueless flags like --dev-keys/--list-devices, or flags handled in main
    // such as --config/--seconds/--record and their values) is skipped without desyncing.
  }
  validate();
}
void Config::validate() {
  if (ringFrames < kRingFramesMin) ringFrames = kRingFramesMin;
  if (ringFrames > kRingFramesMax) ringFrames = kRingFramesMax;
}
}
