#include "config.h"
#include <toml++/toml.hpp>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
namespace vc {
Config Config::load(const std::string& path) {
  toml::table t;
  try { t = toml::parse_file(path); }
  catch (const toml::parse_error& e) { throw std::runtime_error(std::string("config parse: ") + e.description().data()); }
  Config c;
  c.captureDeviceId = t["capture_device_id"].value_or(std::string(""));
  c.renderDeviceId  = t["render_device_id"].value_or(std::string(""));
  int64_t rawRing = t["ring_frames"].value_or((int64_t)1024);
  c.ringFrames = rawRing < 0 ? 0 : (size_t)rawRing;   // 0 -> validate() floors to 3
  c.csvPath         = t["csv_path"].value_or(std::string("drift.csv"));
  c.logLevel        = t["log_level"].value_or(1);
  c.validate();
  return c;
}
void Config::apply_cli(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--capture")) captureDeviceId = argv[i+1];
    else if (!std::strcmp(argv[i], "--render")) renderDeviceId = argv[i+1];
    else if (!std::strcmp(argv[i], "--csv")) csvPath = argv[i+1];
    else if (!std::strcmp(argv[i], "--ring")) {
      long long v = std::atoll(argv[i+1]);
      ringFrames = v < 0 ? 0 : (size_t)v;   // 0/low -> validate() floors; huge -> validate() caps
    }
  }
  validate();
}
void Config::validate() {
  if (ringFrames < 3) ringFrames = 3;
  if (ringFrames > 1048576) ringFrames = 1048576;   // ~218 s ceiling; guards size_t wrap from negative input
}
}
