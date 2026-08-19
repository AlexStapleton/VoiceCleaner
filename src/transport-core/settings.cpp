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
          m.aggression     = (float)(*p)["aggression"].value_or(0.0);
          m.hpfHz          = (float)(*p)["hpf_hz"].value_or(80.0);
          m.outputGainDbfs = (float)(*p)["output_gain_dbfs"].value_or(-20.0);
          m.limiterDbfs    = (float)(*p)["limiter_dbfs"].value_or(-1.0);
          m.declickStrength = (float)(*p)["declick_strength"].value_or(0.0);
          m.declickVoiceFloor = (float)(*p)["declick_voice_floor"].value_or(0.0);
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

MicProfile* Settings::activeProfile() {
  for (auto& m : mics) if (m.id == activeCaptureId) return &m;
  return nullptr;
}

MicProfile& Settings::ensureProfile(const std::string& id, const std::string& name) {
  for (auto& m : mics) {
    if (m.id == id) { if (!name.empty()) m.name = name; return m; }
  }
  MicProfile m; m.id = id; m.name = name;
  mics.push_back(std::move(m));
  return mics.back();
}

void Settings::save(const std::string& path) const {
  toml::table g;
  g.insert_or_assign("active_capture_id", activeCaptureId);
  g.insert_or_assign("render_id", renderId);
  g.insert_or_assign("ring_frames", (int64_t)ringFrames);

  toml::table micTbl;
  for (const auto& m : mics) {
    toml::table p;
    p.insert_or_assign("name", m.name);
    p.insert_or_assign("engine", m.engine);
    p.insert_or_assign("warm_standby", m.warmStandby);
    p.insert_or_assign("dsp_chain", m.dspChain);
    p.insert_or_assign("aggression", (double)m.aggression);
    p.insert_or_assign("hpf_hz", (double)m.hpfHz);
    p.insert_or_assign("output_gain_dbfs", (double)m.outputGainDbfs);
    p.insert_or_assign("limiter_dbfs", (double)m.limiterDbfs);
    p.insert_or_assign("declick_strength", (double)m.declickStrength);
    p.insert_or_assign("declick_voice_floor", (double)m.declickVoiceFloor);
    micTbl.insert_or_assign(m.id, std::move(p));
  }

  toml::table root;
  root.insert_or_assign("global", std::move(g));
  root.insert_or_assign("mic", std::move(micTbl));

  std::ofstream out(path);
  out << root;   // toml++ serializes [global] + [mic."<id>"] sections
}

}  // namespace vc
