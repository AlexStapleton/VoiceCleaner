#include "settings.h"
#include <toml++/toml.hpp>
#include <fstream>
namespace vc {

Config Settings::resolveActive() const {
  Config c;
  c.captureDeviceId = activeCaptureId;
  c.renderDeviceId  = renderId;
  c.ringFrames      = ringFrames;
  for (const auto& m : mics) {
    if (m.id == activeCaptureId) {
      c.engine      = m.engine;
      c.warmStandby = m.warmStandby;
      c.dspChain    = m.dspChain;
      break;
    }
  }
  return c;   // Config::load validates
}

Settings Settings::load(const std::string& path) {
  Settings s;
  toml::table t;
  try { t = toml::parse_file(path); }
  catch (const toml::parse_error&) { return s; }   // UI-tolerant: missing/bad => empty settings

  if (auto g = t["global"].as_table()) {
    s.activeCaptureId = (*g)["active_capture_id"].value_or(std::string(""));
    s.renderId        = (*g)["render_id"].value_or(std::string(""));
    s.ringFrames      = (size_t)(*g)["ring_frames"].value_or((int64_t)16);
    if (auto mics = t["mic"].as_table()) {
      for (auto&& [key, val] : *mics) {
        if (auto p = val.as_table()) {
          MicProfile m;
          m.id          = std::string(key.str());
          m.name        = (*p)["name"].value_or(std::string(""));
          m.engine      = (*p)["engine"].value_or(std::string("rnnoise"));
          m.warmStandby = (*p)["warm_standby"].value_or(false);
          m.dspChain    = (*p)["dsp_chain"].value_or(true);
          s.mics.push_back(std::move(m));
        }
      }
    }
  } else {
    // Flat back-compat: synthesize [global] + one profile from Phase-3 top-level keys.
    s.activeCaptureId = t["capture_device_id"].value_or(std::string(""));
    s.renderId        = t["render_device_id"].value_or(std::string(""));
    s.ringFrames      = (size_t)t["ring_frames"].value_or((int64_t)16);
    if (!s.activeCaptureId.empty()) {
      MicProfile m;
      m.id          = s.activeCaptureId;
      m.engine      = t["engine"].value_or(std::string("rnnoise"));
      m.warmStandby = t["warm_standby"].value_or(false);
      m.dspChain    = t["dsp_chain"].value_or(true);
      s.mics.push_back(std::move(m));
    }
  }
  return s;
}

}  // namespace vc
