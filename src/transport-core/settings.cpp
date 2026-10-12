#include "settings.h"
#include <toml++/toml.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>
namespace vc {

Config Settings::resolveActive() const {
  Config c;
  c.captureDeviceId = activeCaptureId;
  c.renderDeviceId  = renderId;
  c.ringFrames      = ringFrames;
  for (const auto& m : mics) {
    if (m.id == activeCaptureId) {
      c.engine      = "rnnoise";
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
    s.ringFrames      = clampRingFrames((*g)["ring_frames"].value_or((int64_t)8));   // hand-edited -1 must not wrap
    s.collapsedSections = (unsigned)(*g)["collapsed_sections"].value_or((int64_t)0);
    if (auto mics = t["mic"].as_table()) {
      for (auto&& [key, val] : *mics) {
        if (auto p = val.as_table()) {
          MicProfile m;
          m.id          = std::string(key.str());
          m.name        = (*p)["name"].value_or(std::string(""));
          m.dspChain    = (*p)["dsp_chain"].value_or(true);
          m.hpfHz          = (float)(*p)["hpf_hz"].value_or(80.0);
          m.outputGainDbfs = (float)(*p)["output_gain_dbfs"].value_or(-16.0);
          m.maxBoostDb     = (float)(*p)["max_boost_db"].value_or(18.0);   // absent in pre-v1.4 files -> 18
          m.limiterDbfs    = (float)(*p)["limiter_dbfs"].value_or(-1.0);
          m.declickStrength = (float)(*p)["declick_strength"].value_or(0.0);
          m.declickVoiceFloor = (float)(*p)["declick_voice_floor"].value_or(0.0);
          m.gateStrength = (float)(*p)["gate_strength"].value_or(0.0);
          m.mixStrength = (float)(*p)["mix_strength"].value_or(1.0);
          s.mics.push_back(std::move(m));
        }
      }
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

bool Settings::save(const std::string& path) const {
  toml::table g;
  g.insert_or_assign("active_capture_id", activeCaptureId);
  g.insert_or_assign("render_id", renderId);
  g.insert_or_assign("ring_frames", (int64_t)ringFrames);
  g.insert_or_assign("collapsed_sections", (int64_t)collapsedSections);

  toml::table micTbl;
  for (const auto& m : mics) {
    toml::table p;
    p.insert_or_assign("name", m.name);
    p.insert_or_assign("dsp_chain", m.dspChain);
    p.insert_or_assign("hpf_hz", (double)m.hpfHz);
    p.insert_or_assign("output_gain_dbfs", (double)m.outputGainDbfs);
    p.insert_or_assign("limiter_dbfs", (double)m.limiterDbfs);
    p.insert_or_assign("declick_strength", (double)m.declickStrength);
    p.insert_or_assign("declick_voice_floor", (double)m.declickVoiceFloor);
    p.insert_or_assign("gate_strength", (double)m.gateStrength);
    p.insert_or_assign("mix_strength", (double)m.mixStrength);
    p.insert_or_assign("max_boost_db", (double)m.maxBoostDb);
    micTbl.insert_or_assign(m.id, std::move(p));
  }

  toml::table root;
  root.insert_or_assign("global", std::move(g));
  root.insert_or_assign("mic", std::move(micTbl));

  // Crash-safe save: write a sibling temp file, then atomically rename it over the target. Writing the
  // target in place would truncate it first, so a crash / kill / power loss mid-write left a partial
  // file that load() rejects as a parse error -> silently empty settings (every mic profile lost),
  // and a worker (re)starting mid-write failed Config::load. A rename leaves either the old or the
  // new file, never a torn one.
  namespace fs = std::filesystem;
  // `path` is UTF-8; a plain fs::path(std::string) would decode it with the ANSI code page on Windows.
  const fs::path target(std::u8string(path.begin(), path.end()));
  fs::path tmp = target;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream out(tmp, std::ios::out | std::ios::trunc);
    if (!out) return false;
    out << root;   // toml++ serializes [global] + [mic."<id>"] sections
    out.flush();
    if (!out) { out.close(); fs::remove(tmp, ec); return false; }
  }
  // std::filesystem::rename replaces an existing file (MoveFileExW + MOVEFILE_REPLACE_EXISTING on
  // MSVC). On Windows it fails with a sharing violation while another process has the target open,
  // e.g. the worker reading it in Config::load, so retry briefly before giving up.
  for (int attempt = 0; attempt < 10; ++attempt) {
    fs::rename(tmp, target, ec);
    if (!ec) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fs::remove(tmp, ec);   // give up: keep the previous (intact) settings file, drop the temp
  return false;
}

}  // namespace vc
