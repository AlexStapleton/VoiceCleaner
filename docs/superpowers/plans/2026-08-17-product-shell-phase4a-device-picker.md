# Product Shell — Phase 4a: Device picker + settings persistence — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user pick capture + render devices in `voicecleaner.exe`, persist the choice to a `[global]`+`[mic."<id>"]` `settings.toml`, spawn/switch the worker from it, and add a visible window Quit — so the app is usable without hand-editing TOML.

**Architecture:** A shared `Settings` model in `transport-core` owns the new schema (worker's `Config::load` resolves the active profile through it; flat files stay back-compatible). A pure `DevicePicker` in `voicecleaner-core` holds all picker decisions and is unit-tested. The Win32/D2D glue (`voicecleaner` exe) enumerates devices via `win-audio`'s `DeviceEnumerator`, draws two custom dark dropdowns, and wires picks to `Settings` save + the existing `SelectCaptureDevice`/`SelectRenderDevice`/`SetParams` pipe commands.

**Tech Stack:** C++20, `toml++` (already in `transport-core`), Direct2D/DirectWrite, Win32, `win-audio` (WASAPI `DeviceEnumerator`), GoogleTest.

**Design spec:** [`docs/superpowers/specs/2026-08-17-product-shell-phase4a-device-picker-design.md`](../specs/2026-08-17-product-shell-phase4a-device-picker-design.md)

**Build/test commands (CMake is NOT on PATH):**
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```

---

## File Structure

**New — pure, unit-tested:**
- `src/transport-core/settings.h` / `settings.cpp` — `vc::Settings` + `vc::MicProfile`: load (new schema + flat back-compat), save, `ensureProfile`/`activeProfile`, `resolveActive()→Config`.
- `src/voicecleaner/device_picker.h` / `device_picker.cpp` — pure `vc::ui::DevicePicker` + `DeviceRow` (added to `voicecleaner-core`).

**New — Win32 glue (built, manually verified):**
- `src/voicecleaner/device_enum.h` / `device_enum.cpp` — enumerate WASAPI devices into `DeviceRow`s (links `win-audio`).

**Modified:**
- `src/transport-core/config.cpp` — `Config::load` branches: `[global]` → `Settings::load().resolveActive()`; else the existing flat parse (unchanged).
- `src/transport-core/CMakeLists.txt` — add `settings.cpp`.
- `src/voicecleaner/worker_supervisor.h` / `.cpp` — `sendSelectCapture`/`sendSelectRender` helpers.
- `src/voicecleaner/app_window.h` / `.cpp` — two picker fields + dark overlay + hit-test; enumerate on open; apply picks; window Quit control.
- `src/voicecleaner/main.cpp` — first-run flow: load `Settings`, decide spawn vs. pick state; own the `Settings` + config path and hand them to the window.
- `src/voicecleaner/CMakeLists.txt` — add `device_picker.cpp` to `voicecleaner-core`; add `device_enum.cpp` + link `win-audio` to the exe.
- `src/voicecleaner/settings.toml` — rewritten to the new `[global]`+`[mic]` schema (blank IDs, `ring_frames = 16`).
- `tests/transport-core/CMakeLists.txt` + `tests/transport-core/test_settings.cpp` — Settings tests.
- `tests/voicecleaner/CMakeLists.txt` + `tests/voicecleaner/test_device_picker.cpp` — DevicePicker tests.

---

## Task 1: `Settings` model — load (new schema + flat) + `resolveActive`

**Files:**
- Create: `src/transport-core/settings.h`, `src/transport-core/settings.cpp`
- Modify: `src/transport-core/CMakeLists.txt`
- Create: `tests/transport-core/test_settings.cpp`; Modify: `tests/transport-core/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `src/transport-core/settings.h`:

```cpp
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
// Shared by the worker (via Config::load → resolveActive) and the UI (full CRUD + save).
struct Settings {
  std::string activeCaptureId;    // [global].active_capture_id  ("" => none)
  std::string renderId;           // [global].render_id
  size_t      ringFrames = 16;    // [global].ring_frames
  std::vector<MicProfile> mics;   // all [mic."<id>"] sections

  static Settings load(const std::string& path);   // new schema OR flat back-compat; missing/bad file => empty
  void save(const std::string& path) const;        // Task 2
  MicProfile* activeProfile();                      // Task 2
  MicProfile& ensureProfile(const std::string& id, const std::string& name);  // Task 2
  Config resolveActive() const;                     // flatten active profile → the worker's flat Config
};

}  // namespace vc
```

- [ ] **Step 2: Write the failing tests**

Create `tests/transport-core/test_settings.cpp`:

```cpp
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include "settings.h"
using vc::Settings;

static std::string writeTmp(const char* body) {
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings.toml").string();
  std::ofstream(p) << body;
  return p;
}

TEST(Settings, LoadsNewSchemaAndResolvesActive) {
  auto p = writeTmp(
    "[global]\n"
    "active_capture_id = \"cap-1\"\n"
    "render_id = \"ren-1\"\n"
    "ring_frames = 16\n"
    "[mic.\"cap-1\"]\n"
    "name = \"K688\"\n"
    "engine = \"dfn3\"\n"
    "warm_standby = true\n"
    "dsp_chain = true\n");
  Settings s = Settings::load(p);
  EXPECT_EQ(s.activeCaptureId, "cap-1");
  EXPECT_EQ(s.renderId, "ren-1");
  EXPECT_EQ(s.ringFrames, 16u);
  ASSERT_EQ(s.mics.size(), 1u);
  EXPECT_EQ(s.mics[0].name, "K688");
  vc::Config c = s.resolveActive();
  EXPECT_EQ(c.captureDeviceId, "cap-1");
  EXPECT_EQ(c.renderDeviceId, "ren-1");
  EXPECT_EQ(c.ringFrames, 16u);
  EXPECT_EQ(c.engine, "dfn3");
  EXPECT_TRUE(c.warmStandby);
  EXPECT_TRUE(c.dspChain);
}

TEST(Settings, FlatFileBackCompatResolves) {
  auto p = writeTmp(
    "capture_device_id = \"cap-9\"\n"
    "render_device_id = \"ren-9\"\n"
    "ring_frames = 16\n"
    "engine = \"rnnoise\"\n"
    "dsp_chain = true\n");
  Settings s = Settings::load(p);
  EXPECT_EQ(s.activeCaptureId, "cap-9");
  EXPECT_EQ(s.renderId, "ren-9");
  ASSERT_EQ(s.mics.size(), 1u);
  EXPECT_EQ(s.mics[0].id, "cap-9");
  EXPECT_EQ(s.mics[0].engine, "rnnoise");
  vc::Config c = s.resolveActive();
  EXPECT_EQ(c.captureDeviceId, "cap-9");
  EXPECT_EQ(c.engine, "rnnoise");
}

TEST(Settings, MissingFileIsEmpty) {
  Settings s = Settings::load("Z:/does/not/exist_vc.toml");
  EXPECT_TRUE(s.activeCaptureId.empty());
  EXPECT_TRUE(s.mics.empty());
}

TEST(Settings, ResolveWithNoActiveProfileUsesDefaults) {
  Settings s;  // empty
  vc::Config c = s.resolveActive();
  EXPECT_TRUE(c.captureDeviceId.empty());
  EXPECT_EQ(c.ringFrames, 16u);   // Settings default ring
}
```

- [ ] **Step 3: Wire the test + lib sources into CMake**

In `src/transport-core/CMakeLists.txt`, add `settings.cpp` to the `add_library(transport-core ...)` source list (after `config.cpp`).

In `tests/transport-core/CMakeLists.txt`, add `test_settings.cpp` to the `add_executable(transport-core-tests ...)` source list.

- [ ] **Step 4: Run to verify failure (no `settings.cpp` yet)**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
```
Expected: FAIL — unresolved `Settings::load` / `resolveActive`.

- [ ] **Step 5: Implement `load` + `resolveActive`**

Create `src/transport-core/settings.cpp`:

```cpp
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
```

(Note: a flat file that omits `engine` synthesizes `"rnnoise"` — the product default a new mic gets — rather than `Config`'s legacy `"none"`. Only affects migrating a flat file that never named an engine; harmless.)

- [ ] **Step 6: Run tests to verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R Settings --output-on-failure
```
Expected: PASS (all four `Settings.*`, `resolveActive` cases).

- [ ] **Step 7: Commit**

```
git add src/transport-core/settings.h src/transport-core/settings.cpp src/transport-core/CMakeLists.txt tests/transport-core/test_settings.cpp tests/transport-core/CMakeLists.txt
git commit -m "feat(settings): Settings model — load (new+flat) + resolveActive"
```

---

## Task 2: `Settings` — `save`, `ensureProfile`, `activeProfile` (round-trip)

**Files:**
- Modify: `src/transport-core/settings.cpp`
- Test: `tests/transport-core/test_settings.cpp`

- [ ] **Step 1: Add failing tests**

Append to `tests/transport-core/test_settings.cpp`:

```cpp
TEST(Settings, EnsureProfileCreatesDefaultThenReuses) {
  Settings s;
  vc::MicProfile& a = s.ensureProfile("cap-1", "K688");
  EXPECT_EQ(a.engine, "rnnoise");
  EXPECT_TRUE(a.dspChain);
  EXPECT_EQ(s.mics.size(), 1u);
  vc::MicProfile& b = s.ensureProfile("cap-1", "K688");  // same id -> no new entry
  EXPECT_EQ(s.mics.size(), 1u);
  EXPECT_EQ(&a, &b);
}

TEST(Settings, ActiveProfileFollowsActiveId) {
  Settings s;
  s.ensureProfile("cap-1", "A");
  s.ensureProfile("cap-2", "B");
  s.activeCaptureId = "cap-2";
  ASSERT_NE(s.activeProfile(), nullptr);
  EXPECT_EQ(s.activeProfile()->name, "B");
  s.activeCaptureId = "cap-none";
  EXPECT_EQ(s.activeProfile(), nullptr);
}

TEST(Settings, SaveLoadRoundTripIsIdentity) {
  Settings s;
  s.activeCaptureId = "cap-1"; s.renderId = "ren-1"; s.ringFrames = 16;
  vc::MicProfile& m = s.ensureProfile("cap-1", "K688");
  m.engine = "dfn3"; m.warmStandby = true; m.dspChain = true;
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings_rt.toml").string();
  s.save(p);
  Settings r = Settings::load(p);
  EXPECT_EQ(r.activeCaptureId, "cap-1");
  EXPECT_EQ(r.renderId, "ren-1");
  EXPECT_EQ(r.ringFrames, 16u);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_EQ(r.mics[0].id, "cap-1");
  EXPECT_EQ(r.mics[0].name, "K688");
  EXPECT_EQ(r.mics[0].engine, "dfn3");
  EXPECT_TRUE(r.mics[0].warmStandby);
}
```

- [ ] **Step 2: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
```
Expected: FAIL — unresolved `save` / `ensureProfile` / `activeProfile`.

- [ ] **Step 3: Implement them**

In `src/transport-core/settings.cpp`, add before the closing `}  // namespace vc`:

```cpp
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
    micTbl.insert_or_assign(m.id, std::move(p));
  }

  toml::table root;
  root.insert_or_assign("global", std::move(g));
  root.insert_or_assign("mic", std::move(micTbl));

  std::ofstream out(path);
  out << root;   // toml++ serializes [global] + [mic."<id>"] sections
}
```

- [ ] **Step 4: Run tests to verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R Settings --output-on-failure
```
Expected: PASS (all `Settings.*`).

- [ ] **Step 5: Commit**

```
git add src/transport-core/settings.cpp tests/transport-core/test_settings.cpp
git commit -m "feat(settings): save + ensureProfile/activeProfile with round-trip test"
```

---

## Task 3: `Config::load` resolves the new schema (flat stays unchanged)

**Files:**
- Modify: `src/transport-core/config.cpp`
- Test: `tests/transport-core/test_config.cpp`

- [ ] **Step 1: Add a failing test for the new schema path**

Append to `tests/transport-core/test_config.cpp`:

```cpp
TEST(Config, LoadsNewSchemaViaSettings) {
  auto p = write_tmp(
    "[global]\n"
    "active_capture_id = \"cap-1\"\n"
    "render_id = \"ren-1\"\n"
    "ring_frames = 16\n"
    "[mic.\"cap-1\"]\n"
    "engine = \"dfn3\"\n"
    "dsp_chain = true\n");
  Config c = Config::load(p);
  EXPECT_EQ(c.captureDeviceId, "cap-1");
  EXPECT_EQ(c.renderDeviceId, "ren-1");
  EXPECT_EQ(c.ringFrames, 16u);
  EXPECT_EQ(c.engine, "dfn3");
  EXPECT_TRUE(c.dspChain);
}
```

Add `#include "settings.h"` is NOT needed in the test (it only uses `Config`). The existing flat `Config.LoadsToml`, `ThrowsOnMalformedToml`, etc. must remain unchanged and keep passing.

- [ ] **Step 2: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "Config.LoadsNewSchemaViaSettings" --output-on-failure
```
Expected: FAIL — `Config::load` ignores `[global]`, so `captureDeviceId` is empty.

- [ ] **Step 3: Branch `Config::load` on `[global]`**

In `src/transport-core/config.cpp`, add `#include "settings.h"` at the top, and change `Config::load` so that after the malformed-throw parse it branches. Replace the body between the `toml::parse_file` try/catch and `c.validate()` with:

```cpp
Config Config::load(const std::string& path) {
  toml::table t;
  try { t = toml::parse_file(path); }
  catch (const toml::parse_error& e) { throw std::runtime_error(std::string("config parse: ") + e.description().data()); }
  Config c;
  if (t.contains("global")) {
    // New schema: resolve the active mic's profile (Settings owns the layout).
    c = Settings::load(path).resolveActive();
  } else {
    // Flat (Phase-3 + drift*.toml) — unchanged parsing.
    c.captureDeviceId = t["capture_device_id"].value_or(std::string(""));
    c.renderDeviceId  = t["render_device_id"].value_or(std::string(""));
    int64_t rawRing = t["ring_frames"].value_or((int64_t)1024);
    c.ringFrames = rawRing < 0 ? 0 : (size_t)rawRing;
    c.csvPath         = t["csv_path"].value_or(std::string("drift.csv"));
    c.logLevel        = t["log_level"].value_or(1);
    c.driftCompensation = t["drift_compensation"].value_or(true);
    c.kp                = t["drift_kp"].value_or(2e-7);
    c.ki                = t["drift_ki"].value_or(2e-9);
    c.driftMaxPpm       = t["drift_max_ppm"].value_or(150.0);
    c.filterAlpha       = t["drift_filter_alpha"].value_or(0.02);
    c.engine            = t["engine"].value_or(std::string("none"));
    c.dspChain          = t["dsp_chain"].value_or(false);
    c.warmStandby       = t["warm_standby"].value_or(false);
  }
  c.validate();
  return c;
}
```

(This keeps the flat branch byte-identical to the previous implementation, so `drift*.toml` and every existing `Config.*` test behave exactly as before. `settings.cpp` and `config.cpp` are in the same `transport-core` lib, so `settings.h` is directly includable.)

- [ ] **Step 4: Run the full transport-core suite — new + regression**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "Config|Settings" --output-on-failure
```
Expected: PASS — the new `Config.LoadsNewSchemaViaSettings` **and** all pre-existing `Config.*` (flat) tests.

- [ ] **Step 5: Commit**

```
git add src/transport-core/config.cpp tests/transport-core/test_config.cpp
git commit -m "feat(config): Config::load resolves [global] schema via Settings; flat unchanged"
```

---

## Task 4: `DevicePicker` — devices, active, label, selectable, device-gone

**Files:**
- Create: `src/voicecleaner/device_picker.h`, `src/voicecleaner/device_picker.cpp`
- Modify: `src/voicecleaner/CMakeLists.txt` (add `device_picker.cpp` to `voicecleaner-core`)
- Create: `tests/voicecleaner/test_device_picker.cpp`; Modify: `tests/voicecleaner/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `src/voicecleaner/device_picker.h`:

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace vc::ui {

struct DeviceRow { std::string id; std::string name; uint32_t sampleRate = 0; bool is48k = false; };
enum class PickerKind { Capture, Render };

// Pure picker state (design §6). Fed enumerated device rows + the active ids; computes the field
// label, which rows are selectable, device-gone, and the open/close/pick transitions. No Windows.
class DevicePicker {
 public:
  void setDevices(std::vector<DeviceRow> capture, std::vector<DeviceRow> render);
  void setActive(std::string captureId, std::string renderId);

  void open(PickerKind k) { open_ = true; openKind_ = k; }
  void close() { open_ = false; }
  bool isOpen() const { return open_; }
  PickerKind openKind() const { return openKind_; }

  const std::vector<DeviceRow>& rows(PickerKind k) const;
  const std::string& activeId(PickerKind k) const;
  std::string fieldLabel(PickerKind k) const;                 // name / "(not connected)" / "Select a device..."
  static bool selectable(const DeviceRow& r) { return r.is48k; }
  bool deviceGone(PickerKind k) const;                        // active id set but absent from rows
  // Click a list row: returns the chosen id (and updates active + closes) iff selectable & changed; else "".
  std::string pick(PickerKind k, size_t rowIndex);

 private:
  std::vector<DeviceRow> capture_, render_;
  std::string activeCapture_, activeRender_;
  bool open_ = false;
  PickerKind openKind_ = PickerKind::Capture;
};

}  // namespace vc::ui
```

- [ ] **Step 2: Write the failing tests**

Create `tests/voicecleaner/test_device_picker.cpp`:

```cpp
#include <gtest/gtest.h>
#include "device_picker.h"
using namespace vc::ui;

static DeviceRow row(const char* id, const char* name, uint32_t sr) {
  DeviceRow r; r.id = id; r.name = name; r.sampleRate = sr; r.is48k = (sr == 48000); return r;
}

TEST(DevicePicker, FieldLabelPresentAbsentEmpty) {
  DevicePicker p;
  p.setDevices({row("c1","K688",48000), row("c2","USB Mic",44100)}, {row("r1","CABLE Input",48000)});
  p.setActive("c1", "r1");
  EXPECT_EQ(p.fieldLabel(PickerKind::Capture), "K688");
  EXPECT_EQ(p.fieldLabel(PickerKind::Render), "CABLE Input");
  p.setActive("gone", "r1");
  EXPECT_EQ(p.fieldLabel(PickerKind::Capture), "(not connected)");
  EXPECT_TRUE(p.deviceGone(PickerKind::Capture));
  p.setActive("", "r1");
  EXPECT_EQ(p.fieldLabel(PickerKind::Capture), "Select a device...");
  EXPECT_FALSE(p.deviceGone(PickerKind::Capture));   // empty active is not "gone"
}

TEST(DevicePicker, NonFortyEightKIsNotSelectable) {
  DeviceRow bad = row("c2","USB Mic",44100);
  DeviceRow good = row("c1","K688",48000);
  EXPECT_FALSE(DevicePicker::selectable(bad));
  EXPECT_TRUE(DevicePicker::selectable(good));
}
```

- [ ] **Step 3: Wire CMake**

In `src/voicecleaner/CMakeLists.txt`, change the `voicecleaner-core` library to compile `ui_state.cpp device_picker.cpp`.

In `tests/voicecleaner/CMakeLists.txt`, add `test_device_picker.cpp` to the `add_executable(voicecleaner-tests ...)` sources.

- [ ] **Step 4: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
```
Expected: FAIL — unresolved `DevicePicker` methods.

- [ ] **Step 5: Implement the queries**

Create `src/voicecleaner/device_picker.cpp`:

```cpp
#include "device_picker.h"
namespace vc::ui {

void DevicePicker::setDevices(std::vector<DeviceRow> capture, std::vector<DeviceRow> render) {
  capture_ = std::move(capture); render_ = std::move(render);
}
void DevicePicker::setActive(std::string captureId, std::string renderId) {
  activeCapture_ = std::move(captureId); activeRender_ = std::move(renderId);
}
const std::vector<DeviceRow>& DevicePicker::rows(PickerKind k) const {
  return k == PickerKind::Capture ? capture_ : render_;
}
const std::string& DevicePicker::activeId(PickerKind k) const {
  return k == PickerKind::Capture ? activeCapture_ : activeRender_;
}
std::string DevicePicker::fieldLabel(PickerKind k) const {
  const std::string& id = activeId(k);
  if (id.empty()) return "Select a device...";
  for (const auto& r : rows(k)) if (r.id == id) return r.name;
  return "(not connected)";
}
bool DevicePicker::deviceGone(PickerKind k) const {
  const std::string& id = activeId(k);
  if (id.empty()) return false;
  for (const auto& r : rows(k)) if (r.id == id) return false;
  return true;
}
std::string DevicePicker::pick(PickerKind k, size_t rowIndex) {
  const auto& rs = rows(k);
  if (rowIndex >= rs.size()) return "";
  const DeviceRow& r = rs[rowIndex];
  if (!selectable(r)) return "";
  if (r.id == activeId(k)) return "";
  (k == PickerKind::Capture ? activeCapture_ : activeRender_) = r.id;
  open_ = false;
  return r.id;
}

}  // namespace vc::ui
```

- [ ] **Step 6: Run tests to verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R DevicePicker --output-on-failure
```
Expected: PASS (both `DevicePicker.*`).

- [ ] **Step 7: Commit**

```
git add src/voicecleaner/device_picker.h src/voicecleaner/device_picker.cpp src/voicecleaner/CMakeLists.txt tests/voicecleaner/test_device_picker.cpp tests/voicecleaner/CMakeLists.txt
git commit -m "feat(ui): pure DevicePicker — label, selectable, device-gone"
```

---

## Task 5: `DevicePicker` — open/close/pick transitions

**Files:**
- Test: `tests/voicecleaner/test_device_picker.cpp`

The implementation from Task 4 already handles these; this task locks the transitions.

- [ ] **Step 1: Add the failing tests**

Append to `tests/voicecleaner/test_device_picker.cpp`:

```cpp
TEST(DevicePicker, OpenCloseTracksKind) {
  DevicePicker p;
  EXPECT_FALSE(p.isOpen());
  p.open(PickerKind::Render);
  EXPECT_TRUE(p.isOpen());
  EXPECT_EQ(p.openKind(), PickerKind::Render);
  p.close();
  EXPECT_FALSE(p.isOpen());
}

TEST(DevicePicker, PickSelectableChangedReturnsIdAndCloses) {
  DevicePicker p;
  p.setDevices({row("c1","K688",48000), row("c2","USB",44100)}, {});
  p.setActive("c1", "");
  p.open(PickerKind::Capture);
  EXPECT_EQ(p.pick(PickerKind::Capture, 1), "");   // c2 is 44.1k -> not selectable, no-op
  EXPECT_TRUE(p.isOpen());
  EXPECT_EQ(p.pick(PickerKind::Capture, 0), "");   // c1 already active -> no-op
  // add a second 48k device and pick it
  p.setDevices({row("c1","K688",48000), row("c3","Yeti",48000)}, {});
  p.open(PickerKind::Capture);
  EXPECT_EQ(p.pick(PickerKind::Capture, 1), "c3"); // selectable + changed
  EXPECT_FALSE(p.isOpen());                         // pick closes
  EXPECT_EQ(p.activeId(PickerKind::Capture), "c3");
  EXPECT_EQ(p.pick(PickerKind::Capture, 9), "");   // out of range -> no-op
}
```

- [ ] **Step 2: Run tests to verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R DevicePicker --output-on-failure
```
Expected: PASS.

- [ ] **Step 3: Commit**

```
git add tests/voicecleaner/test_device_picker.cpp
git commit -m "test(ui): lock DevicePicker open/close/pick transitions"
```

---

## Task 6: Device enumeration helper (Win32) + link `win-audio`

**Files:**
- Create: `src/voicecleaner/device_enum.h`, `src/voicecleaner/device_enum.cpp`
- Modify: `src/voicecleaner/CMakeLists.txt`

Win32 glue — verified by build (COM enumeration can't run headless in unit tests).

- [ ] **Step 1: Write the header**

Create `src/voicecleaner/device_enum.h`:

```cpp
#pragma once
#include <vector>
#include "device_picker.h"   // vc::ui::DeviceRow
namespace vc::ui {
// Enumerate WASAPI capture + render devices into DeviceRows (id ASCII, name UTF-8, is48k flag).
// COM must already be initialized (main.cpp CoInitializeEx). Fills empty vectors on failure.
void enumerateDevices(std::vector<DeviceRow>& capture, std::vector<DeviceRow>& render);
}  // namespace vc::ui
```

- [ ] **Step 2: Write the implementation**

Create `src/voicecleaner/device_enum.cpp`:

```cpp
#include "device_enum.h"
#include "device_enumerator.h"   // vc::DeviceEnumerator (win-audio)
#include <windows.h>
namespace vc::ui {

static std::string toUtf8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
static std::string toAscii(const std::wstring& w) { return std::string(w.begin(), w.end()); }  // device IDs are ASCII

static std::vector<DeviceRow> rowsFor(vc::DeviceEnumerator& e, EDataFlow flow) {
  std::vector<DeviceRow> out;
  for (const auto& d : e.list(flow)) {
    DeviceRow r;
    r.id = toAscii(d.id);
    r.name = toUtf8(d.name);
    r.sampleRate = d.sampleRate;
    r.is48k = (d.sampleRate == 48000);
    out.push_back(std::move(r));
  }
  return out;
}

void enumerateDevices(std::vector<DeviceRow>& capture, std::vector<DeviceRow>& render) {
  vc::DeviceEnumerator e;
  capture = rowsFor(e, eCapture);
  render  = rowsFor(e, eRender);
}

}  // namespace vc::ui
```

- [ ] **Step 3: Add source + link `win-audio` to the exe**

In `src/voicecleaner/CMakeLists.txt`:
- add `device_enum.cpp` to the `add_executable(voicecleaner WIN32 ...)` source list;
- add `win-audio` to the exe's `target_link_libraries(voicecleaner PRIVATE ...)` (it publicly exposes `device_enumerator.h`).

- [ ] **Step 4: Build the exe to verify it compiles + links**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner
```
Expected: builds (pre-existing benign warnings only). `device_enum.cpp` compiles; `win-audio` links.

- [ ] **Step 5: Commit**

```
git add src/voicecleaner/device_enum.h src/voicecleaner/device_enum.cpp src/voicecleaner/CMakeLists.txt
git commit -m "feat(ui): WASAPI device enumeration helper (win-audio)"
```

---

## Task 7: Worker-supervisor select helpers + `Settings` ownership in the window

**Files:**
- Modify: `src/voicecleaner/worker_supervisor.h`, `src/voicecleaner/worker_supervisor.cpp`
- Modify: `src/voicecleaner/app_window.h`, `src/voicecleaner/app_window.cpp`, `src/voicecleaner/main.cpp`

This task adds the pipe helpers and threads the `Settings` model + config path into `AppWindow`, without drawing the picker yet (that's Task 8) — so it stays compilable.

- [ ] **Step 1: Add typed pipe helpers to the supervisor**

In `src/voicecleaner/worker_supervisor.h`, add to the public section of `WorkerSupervisor` (near `send`):

```cpp
  bool sendSelectCapture(const std::string& id, double driftSeedPpm);  // pipe: SelectCaptureDevice
  bool sendSelectRender(const std::string& id);                        // pipe: SelectRenderDevice
```

In `src/voicecleaner/worker_supervisor.cpp`, add (before the closing namespace brace):

```cpp
bool WorkerSupervisor::sendSelectCapture(const std::string& id, double driftSeedPpm) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::SelectCaptureDevice;
  c.deviceId = id; c.driftSeedPpm = driftSeedPpm;
  return vc::ipc::PipeClient::send(pipeName_, c);
}
bool WorkerSupervisor::sendSelectRender(const std::string& id) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::SelectRenderDevice;
  c.deviceId = id;
  return vc::ipc::PipeClient::send(pipeName_, c);
}
```

- [ ] **Step 2: Give `AppWindow` the settings + config path + a picker**

In `src/voicecleaner/app_window.h`:
- add `#include "device_picker.h"` and `#include "settings.h"` at the top (after the existing includes);
- add setters + members. In the public section:

```cpp
  void setSettings(vc::Settings* s, std::string configPath) { settings_ = s; configPath_ = std::move(configPath); }
```
- in the private members:

```cpp
  vc::Settings* settings_ = nullptr;
  std::string configPath_;
  DevicePicker picker_;
```

- [ ] **Step 3: Seed the picker from settings + enumeration on create**

In `src/voicecleaner/app_window.cpp`, add `#include "device_enum.h"` at the top. At the end of `AppWindow::create` (just before `return true;`), add:

```cpp
  refreshDevices();   // initial enumeration + active seed
```

And add the method (near `tick()`):

```cpp
void AppWindow::refreshDevices() {
  std::vector<DeviceRow> cap, ren;
  enumerateDevices(cap, ren);
  picker_.setDevices(std::move(cap), std::move(ren));
  if (settings_) picker_.setActive(settings_->activeCaptureId, settings_->renderId);
}
```

Declare it in `app_window.h` private section: `void refreshDevices();`.

- [ ] **Step 4: Own the `Settings` in `main.cpp` and hand it to the window**

In `src/voicecleaner/main.cpp`, add `#include "settings.h"`. Replace the pre-flight block so `main` owns a `Settings` and passes it in. After `AppWindow win(...)` / `win.create(...)` and before the message pump, change the pre-flight to:

```cpp
  std::string cfgUtf8;
  { std::wstring w = cfgPath; cfgUtf8.reserve(w.size());
    for (wchar_t ch : w) cfgUtf8.push_back(static_cast<char>(ch)); }   // ASCII module path

  vc::Settings settings = vc::Settings::load(cfgUtf8);
  win.setSettings(&settings, cfgUtf8);

  bool spawn = !settings.activeCaptureId.empty() && !settings.renderId.empty();
  if (spawn) sup.startOrAttach();
  else       state.onNoConfig();
```

Remove the old `vc::Config::load` try/catch pre-flight (the `Settings` load replaces it; `Settings::load` is tolerant and never throws). Keep `state.setRenderLabel("CABLE Input")`.

- [ ] **Step 5: Build to verify it compiles**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner
```
Expected: builds clean. (No behavior change yet beyond seeding the picker; the picker isn't drawn until Task 8.)

- [ ] **Step 6: Commit**

```
git add src/voicecleaner/worker_supervisor.h src/voicecleaner/worker_supervisor.cpp src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp src/voicecleaner/main.cpp
git commit -m "feat(ui): supervisor select helpers + Settings/picker wired into AppWindow"
```

---

## Task 8: Draw the two dropdowns (fields + overlay + hit-test)

**Files:**
- Modify: `src/voicecleaner/app_window.h`, `src/voicecleaner/app_window.cpp`

Custom-drawn dark dropdowns driven by `picker_`. Manually verified (D2D paint).

- [ ] **Step 1: Add field/overlay rects + row hit-testing to the header**

In `src/voicecleaner/app_window.h` private members, add:

```cpp
  D2D1_RECT_F rcMicField_{}, rcOutField_{};   // the two collapsed picker fields
  // Overlay list geometry is recomputed in paint when picker_.isOpen().
  int overlayHit(int x, int y) const;         // row index under (x,y) in the open overlay, or -1
  D2D1_RECT_F overlayRect() const;            // the open overlay's bounding rect
```

- [ ] **Step 2: Lay out the two fields; shift existing controls down**

In `src/voicecleaner/app_window.cpp` `layout()`, prepend the two picker fields and push the engine/bypass/relaunch rows down by a fixed offset. Replace the body of `layout()` with:

```cpp
void AppWindow::layout(D2D1_SIZE_F sz) {
  const float pad = 18.0f, gap = 8.0f, h = 40.0f;
  rcMicField_ = D2D1::RectF(pad, 92,  sz.width - pad, 92 + h);
  rcOutField_ = D2D1::RectF(pad, 160, sz.width - pad, 160 + h);
  const float mid = sz.width * 0.5f;
  rcEngineRnn_ = D2D1::RectF(pad,            232, mid - gap * 0.5f, 232 + h);
  rcEngineDfn_ = D2D1::RectF(mid + gap*0.5f, 232, sz.width - pad,   232 + h);
  rcBypass_    = D2D1::RectF(pad, 300, sz.width - pad, 300 + h);
  rcRelaunch_  = D2D1::RectF(pad, 354, sz.width - pad, 354 + h);
}
```

And enlarge the window client in `create()` — change the `RECT wr{0, 0, 440, 300};` to `RECT wr{0, 0, 460, 420};` so all rows fit.

- [ ] **Step 3: Add overlay geometry + hit-test helpers**

In `src/voicecleaner/app_window.cpp`, add near `hitControl()`:

```cpp
static const float kRowH = 34.0f;

D2D1_RECT_F AppWindow::overlayRect() const {
  const D2D1_RECT_F& field = (picker_.openKind()==PickerKind::Capture) ? rcMicField_ : rcOutField_;
  const auto& rows = picker_.rows(picker_.openKind());
  float h = kRowH * (rows.empty() ? 1 : (float)rows.size());
  return D2D1::RectF(field.left, field.bottom + 2, field.right, field.bottom + 2 + h);
}

int AppWindow::overlayHit(int x, int y) const {
  if (!picker_.isOpen()) return -1;
  D2D1_RECT_F o = overlayRect();
  if (x < o.left || x > o.right || y < o.top || y > o.bottom) return -1;
  int idx = (int)((y - o.top) / kRowH);
  const auto& rows = picker_.rows(picker_.openKind());
  return (idx >= 0 && idx < (int)rows.size()) ? idx : -1;
}
```

- [ ] **Step 4: Paint the fields (always) and the overlay (when open)**

In `paint()`, after the status line and before the engine control, draw the two fields; and at the very end of `paint()` (after Relaunch, before releasing brushes) draw the overlay so it sits on top. Insert after the status-line `label(...)` call:

```cpp
  // Device pickers: Microphone + Output fields
  auto drawField = [&](const wchar_t* cap, D2D1_RECT_F r, PickerKind k) {
    label(cap, D2D1::RectF(r.left, r.top - 16, r.right, r.top), dim, capFmt_);
    box(r, panel);
    std::wstring name = toW(picker_.fieldLabel(k)) + L"    \u25be";   // name + ▾
    D2D1_RECT_F tr = D2D1::RectF(r.left + 12, r.top, r.right - 12, r.bottom);
    rt_->DrawText(name.c_str(), (UINT32)name.size(), textFmt_, tr, text);
  };
  drawField(L"MICROPHONE", rcMicField_, PickerKind::Capture);
  drawField(L"OUTPUT",     rcOutField_, PickerKind::Render);
```

And insert just before the `text->Release();` line at the end of `paint()`:

```cpp
  // Open dropdown overlay (drawn last, on top of everything)
  if (picker_.isOpen()) {
    PickerKind k = picker_.openKind();
    D2D1_RECT_F o = overlayRect();
    box(o, panelHot);
    const auto& rows = picker_.rows(k);
    for (size_t i = 0; i < rows.size(); ++i) {
      D2D1_RECT_F rr = D2D1::RectF(o.left, o.top + kRowH*i, o.right, o.top + kRowH*(i+1));
      const DeviceRow& d = rows[i];
      const bool sel = DevicePicker::selectable(d);
      const bool active = (d.id == picker_.activeId(k));
      std::wstring line = (active ? L"\u2713 " : L"   ") + toW(d.name);
      if (!sel) line += L"   \u2014 48 kHz only";
      D2D1_RECT_F tr = D2D1::RectF(rr.left + 10, rr.top, rr.right - 10, rr.bottom);
      rt_->DrawText(line.c_str(), (UINT32)line.size(), textFmt_, tr, sel ? text : dim);
    }
  }
```

- [ ] **Step 5: Hit-test clicks — open a field, or pick/close from the overlay**

In `handle()` `WM_LBUTTONDOWN`, handle the pickers first (overlay takes priority, then fields), before the existing `hitControl` switch. Replace the `WM_LBUTTONDOWN` case with:

```cpp
    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
      if (picker_.isOpen()) {
        int row = overlayHit(x, y);
        if (row >= 0) { applyPick(picker_.openKind(), (size_t)row); }
        else { picker_.close(); }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (hit(rcMicField_, x, y)) { refreshDevices(); picker_.open(PickerKind::Capture); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
      if (hit(rcOutField_, x, y)) { refreshDevices(); picker_.open(PickerKind::Render);  InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
      switch (hitControl(x, y)) {
        case 0: onCommandEngine(0); break;
        case 1: onCommandEngine(1); break;
        case 2: onCommandBypass(); break;
        case 3: if (state_->statusKind()==StatusKind::Down) requestRelaunch(); break;
      }
      return 0;
    }
```

Add an `applyPick` stub for now (Task 9 fills it) so it compiles — in `app_window.cpp`:

```cpp
void AppWindow::applyPick(PickerKind, size_t) { picker_.close(); }   // Task 9 implements
```
Declare `void applyPick(PickerKind k, size_t rowIndex);` in the header private section.

Also dismiss the overlay on `Esc`: in `handle()` add a case:
```cpp
    case WM_KEYDOWN:
      if (w == VK_ESCAPE && picker_.isOpen()) { picker_.close(); InvalidateRect(hwnd_, nullptr, FALSE); }
      return 0;
```

- [ ] **Step 6: Build to verify it compiles**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner
```
Expected: builds clean. (The dropdowns now draw and open/close; picking is a no-op until Task 9.)

- [ ] **Step 7: Commit**

```
git add src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp
git commit -m "feat(ui): custom-drawn device dropdowns — fields, overlay, hit-test"
```

---

## Task 9: Apply a pick — persist + spawn (first run) or live switch

**Files:**
- Modify: `src/voicecleaner/app_window.cpp`

- [ ] **Step 1: Implement `applyPick`**

Replace the `applyPick` stub in `src/voicecleaner/app_window.cpp` with:

```cpp
void AppWindow::applyPick(PickerKind k, size_t rowIndex) {
  const std::vector<DeviceRow>& rows = picker_.rows(k);
  if (rowIndex >= rows.size()) { picker_.close(); return; }
  const DeviceRow chosen = rows[rowIndex];          // copy before pick() may mutate state
  std::string id = picker_.pick(k, rowIndex);       // "" if non-selectable / unchanged
  if (id.empty()) { InvalidateRect(hwnd_, nullptr, FALSE); return; }
  if (!settings_) { picker_.close(); return; }

  const bool wasRunning = (state_->statusKind() != StatusKind::NoConfig);

  if (k == PickerKind::Capture) {
    settings_->ensureProfile(id, chosen.name);
    settings_->activeCaptureId = id;
  } else {
    settings_->renderId = id;
  }
  settings_->save(configPath_);

  if (!wasRunning) {
    // First-run: spawn once BOTH devices are chosen.
    if (!settings_->activeCaptureId.empty() && !settings_->renderId.empty()) {
      sup_->startOrAttach();
    }
  } else if (k == PickerKind::Capture) {
    sup_->sendSelectCapture(id, /*driftSeed*/0.0);
    const vc::MicProfile* p = settings_->activeProfile();
    uint32_t eng = (p && p->engine == "dfn3") ? 1u : 0u;
    sendCurrent(state_->onSelectEngine(eng));       // apply the mic's engine
  } else {
    sup_->sendSelectRender(id);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}
```

- [ ] **Step 2: Build**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner
```
Expected: builds clean.

- [ ] **Step 3: Full test suite (no regression from the transport-core/config change)**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```
Expected: PASS — all prior tests + the new `Settings.*` / `DevicePicker.*` / `Config.LoadsNewSchemaViaSettings`.

- [ ] **Step 4: Commit**

```
git add src/voicecleaner/app_window.cpp
git commit -m "feat(ui): apply device pick — persist + spawn (first run) or live switch"
```

---

## Task 10: Window Quit control

**Files:**
- Modify: `src/voicecleaner/app_window.h`, `src/voicecleaner/app_window.cpp`

- [ ] **Step 1: Add a Quit rect + paint it**

In `app_window.h` private members add: `D2D1_RECT_F rcQuit_{};`

In `layout()`, add a small footer Quit rect (right-aligned):

```cpp
  rcQuit_ = D2D1::RectF(sz.width - pad - 90, sz.height - 34, sz.width - pad, sz.height - 8);
```

In `paint()`, before releasing brushes, draw it:

```cpp
  box(rcQuit_, hot_==4 ? panelHot : panel);
  label(L"Quit", rcQuit_, text, centerFmt_);
```

- [ ] **Step 2: Hover + click**

In `hitControl()`, add Quit as index 4 (append before `return -1;`):

```cpp
  if (hit(rcQuit_, x, y)) return 4;
```

In `WM_LBUTTONDOWN` (the `hitControl` switch added in Task 8), add:

```cpp
        case 4: sup_->shutdown(3000); DestroyWindow(hwnd_); break;
```

(Hover already works: `WM_MOUSEMOVE` sets `hot_` from `hitControl`, so index 4 lightens the Quit box; the hand cursor already keys off `hot_ != -1`.)

- [ ] **Step 3: Build**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner
```
Expected: builds clean.

- [ ] **Step 4: Commit**

```
git add src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp
git commit -m "feat(ui): visible window Quit control (same shutdown as tray Quit)"
```

---

## Task 11: Shipped `settings.toml` (new schema) + full validation + STATUS

**Files:**
- Modify: `src/voicecleaner/settings.toml`
- Modify: `docs/STATUS.md`

- [ ] **Step 1: Rewrite the shipped template to the new schema**

Replace `src/voicecleaner/settings.toml` with:

```toml
# VoiceCleaner settings — shipped default (Phase 4a schema).
#
# REAL-TIME latency: ring_frames MUST stay small (~16 = ~15 ms buffered). 1024 = ~5 s latency.
# The mic picker in the app fills active_capture_id / render_id and writes the per-mic profile;
# with them blank, the app opens to the pickers and does not spawn the worker.

[global]
active_capture_id = ""
render_id         = ""
ring_frames       = 16

# One [mic."<wasapi-id>"] profile per microphone is written by the app, e.g.:
# [mic."{0.0.1.00000000}.{...}"]
# name         = "fifine Microphone (K688)"
# engine       = "rnnoise"   # "none" | "rnnoise" | "dfn3"
# warm_standby = false
# dsp_chain    = true
```

- [ ] **Step 2: Full Debug + Release build + test suite**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Release
```
Expected: full build (both configs); ctest 100% (prior 130 + new `Settings.*`, `DevicePicker.*`, `Config.LoadsNewSchemaViaSettings`). Confirm the Release `settings.toml` next to `voicecleaner.exe` is the new-schema template (delete a stale copy in `build\src\voicecleaner\Release\` first if the copy-if-absent kept an old one).

- [ ] **Step 3: DFN3 golden gate (unchanged DSP)**

Confirm no DSP source changed: `git diff --stat main -- src/dfn3 src/denoise-core src/offline-cli` is empty (4a touches config parsing + UI only). The offline DFN3 component tests remain green in the suite above.

- [ ] **Step 4: Manual hardware checklist (Release, real devices)**

Launch `build\src\voicecleaner\Release\voicecleaner.exe` (blank IDs) and verify:
- [ ] Opens to the **Microphone** + **Output** dropdowns; worker not spawned.
- [ ] Clicking a field shows a dark overlay of devices; non-48 kHz devices are greyed "48 kHz only" and cannot be selected.
- [ ] Picking capture (K688) + output (CABLE Input) → worker spawns → **audio flows** to CABLE Input, low latency.
- [ ] Switch to a different mic while running → pipeline rebuilds on the new device (brief gap), audio continues, no worker respawn; the active engine reflects the new mic's profile.
- [ ] Switch output while running.
- [ ] Unplug the saved mic, relaunch → field shows "(not connected)", app waits for a pick, no crash.
- [ ] Selection persists across a UI relaunch (re-attaches to the running worker; correct devices shown).
- [ ] Window **Quit** stops the worker and exits both processes (tray icon removed); X/minimize still hide to tray.
- [ ] Headless-correctness: the worker still runs standalone against a filled new-schema `settings.toml` (and against a legacy flat `drift-dfn3.toml`).

- [ ] **Step 5: Update STATUS.md**

In `docs/STATUS.md`, update the top "Active work" line: Phase 4a complete — in-app device picker (custom-drawn dropdowns) + `[global]`/`[mic."<id>"]` `settings.toml` persistence via a shared `Settings` model (`Config::load` resolves it; flat files stay back-compatible), live mic/render switching over the pipe, non-48 kHz greying, device-gone handling, and a visible window Quit. Note the new `Settings`/`DevicePicker` units + tests; total test count updated. Bump `_Last updated_` to 2026-08-17.

- [ ] **Step 6: Commit**

```
git add src/voicecleaner/settings.toml docs/STATUS.md
git commit -m "feat(ui): ship new-schema settings.toml; docs: Phase 4a complete"
```

---

## Self-Review

**Spec coverage** (design §-by-§):
- §2 locked decisions — full per-mic schema (Tasks 1–2); `Settings` in transport-core + `Config::load` resolver (Tasks 1,3); flat back-compat (Tasks 1,3); live rebuild on switch via `SelectCaptureDevice`+`SetParams` / `SelectRenderDevice` (Tasks 7,9); custom dark dropdown (Task 8); 48 kHz greying (Tasks 4,8); render picker (Tasks 8,9). ✓
- §3 schema — Tasks 1,2,11. ✓
- §4 Settings model + resolveActive + Config::load — Tasks 1,2,3. ✓
- §5 enumeration via win-audio — Task 6. ✓
- §6 pure DevicePicker — Tasks 4,5. ✓
- §7 window: two pickers, first-run/NoConfig, Quit (§7.3) — Tasks 8,9,10; `main.cpp` NoConfig-vs-spawn (Task 7). ✓
- §8 flows (first selection spawn; live capture/render switch; persist on change) — Task 9. ✓
- §9 error handling: device-gone "(not connected)" (Tasks 4,8); non-48 kHz non-selectable (Tasks 4,8); tolerant Settings load / no crash (Task 1). ✓ *(settings.toml read-only write-failure surfacing is minimal here — `save()` failure is silent this phase; noted as a follow-up, not a blocker.)*
- §10 components/files — all tasks; win-audio link (Task 6). ✓
- §11 testing — Settings round-trip/resolve/back-compat (Tasks 1,2,3), DevicePicker pure (Tasks 4,5), regression + DFN3 golden + manual (Task 11). ✓

**Placeholder scan:** no TBD/TODO; every code step shows complete code. The two deliberately-deferred items (read-only `save()` surfacing; the `applyPick` stub in Task 8) are explicitly staged — the stub is replaced with full code in Task 9, not left dangling.

**Type consistency:** `Settings`/`MicProfile` fields (`activeCaptureId`/`renderId`/`ringFrames`/`mics`; `id`/`name`/`engine`/`warmStandby`/`dspChain`) are identical across Tasks 1–3, 7, 9. `DevicePicker` API (`setDevices`/`setActive`/`rows`/`activeId`/`fieldLabel`/`selectable`/`deviceGone`/`open`/`close`/`pick`/`isOpen`/`openKind`) is defined in Task 4 and used consistently in Tasks 8–9. `DeviceRow{id,name,sampleRate,is48k}` matches between `device_picker.h` (Task 4) and `device_enum.cpp` (Task 6). `WorkerSupervisor::sendSelectCapture/sendSelectRender` (Task 7) are called in Task 9. `CmdType::SelectCaptureDevice/SelectRenderDevice` + `Command{deviceId,driftSeedPpm}` match `command.h`/`command.cpp`. `hitControl` indices 0–4 (engine×2, bypass, relaunch, quit) are consistent between Tasks 8 and 10.
