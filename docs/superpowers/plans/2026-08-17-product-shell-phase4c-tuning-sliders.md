# Product Shell — Phase 4c: Tuning sliders — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four live tuning sliders (output gain, aggression, HPF, limiter) to `voicecleaner.exe`, persisted per-mic and seeded on attach, plus soften the AGC default so the voice isn't blasted out of the box.

**Architecture:** The worker already applies all four params live from the `Params` snapshot each hop, so this is UI + persistence: a pure unit-tested `Slider` and `UiState` tuning setters in `voicecleaner-core`, four per-mic fields on the `Settings` model, and custom-drawn D2D slider rows that send `SetParams` on drag and persist on drag-end. One small worker edit softens the AGC default (target −20 dBFS, makeup ≤ +18 dB).

**Tech Stack:** C++20, Direct2D/DirectWrite, Win32, `toml++`, GoogleTest.

**Design spec:** [`docs/superpowers/specs/2026-08-17-product-shell-phase4c-tuning-sliders-design.md`](../specs/2026-08-17-product-shell-phase4c-tuning-sliders-design.md)

**Build/test (CMake NOT on PATH):**
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```

---

## File Structure

**New (pure, unit-tested):** `src/voicecleaner/slider.{h,cpp}` (`vc::ui::Slider`) + `tests/voicecleaner/test_slider.cpp`.

**Modified:**
- `src/denoise-core/dsp_chain.cpp`, `dsp_chain.h`, `agc.h`, `src/vc-ipc/params.h` — AGC default softening.
- `tests/denoise-core/test_dsp_chain.cpp`, `tests/denoise-core/test_agc.cpp`, `tests/vc-ipc/test_params.cpp` — updated for the new defaults.
- `src/transport-core/settings.{h,cpp}` — `MicProfile` tuning fields + load/save; `tests/transport-core/test_settings.cpp`.
- `src/voicecleaner/ui_state.{h,cpp}` — `seedTuning` + four setters; `tests/voicecleaner/test_ui_state.cpp`.
- `src/voicecleaner/app_window.{h,cpp}` — four slider rows, drag, aggression-greying, seed + persist.
- `src/voicecleaner/CMakeLists.txt` — add `slider.cpp` to `voicecleaner-core`; `tests/voicecleaner/CMakeLists.txt` — add `test_slider.cpp`.

---

## Task 1: Soften the AGC default (worker/DSP)

**Files:**
- Modify: `src/vc-ipc/params.h`, `src/denoise-core/dsp_chain.h`, `src/denoise-core/dsp_chain.cpp:8`, `src/denoise-core/agc.h`
- Test: `tests/vc-ipc/test_params.cpp:20`, `tests/denoise-core/test_dsp_chain.cpp`, `tests/denoise-core/test_agc.cpp:8`

TDD: change the tests to assert the new defaults (RED), then change the code (GREEN).

- [ ] **Step 1: Update the tests to the new defaults**

In `tests/vc-ipc/test_params.cpp` line 20, change:
```cpp
  EXPECT_FLOAT_EQ(p.agcTargetDbfs, -20.0f);
```

In `tests/denoise-core/test_dsp_chain.cpp`:
- line 55 comment → `// hpf 80, agc -20, lim -1`
- line 58 → `EXPECT_FLOAT_EQ(chain.currentAgcTargetDb(), -20.0f);`
- line 80 comment → `// big change from -20`
- line 86 comment → `// no full-jump in one frame (full jump would be 20 dB)`

In `tests/denoise-core/test_agc.cpp` line 8, pin the makeup cap so this AGC-mechanics test is independent of the shipped default (its −37 dBFS input needs +22 dB, more than the new +18 cap):
```cpp
  Agc::Config c; c.targetRmsDb=-15; c.maxGainDb=30; Agc a(c);
```

- [ ] **Step 2: Build the two test targets and verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target vc-ipc-tests denoise-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "Params.Defaults|DspChain.CurrentsInitialize" --output-on-failure
```
Expected: FAIL — the code still ships `-15`. (`test_agc` should still PASS after the `maxGainDb=30` pin.)

- [ ] **Step 3: Change the defaults in the code**

In `src/vc-ipc/params.h`, change the `agcTargetDbfs` default in `Params::defaults()` from `-15.0f` to `-20.0f`:
```cpp
        /*agcTargetDbfs*/ -20.0f,
```

In `src/denoise-core/dsp_chain.h` line 17, change:
```cpp
    float agcTargetDb = -20.0f;
```

In `src/denoise-core/dsp_chain.cpp` line 8, change the `Agc` ctor's makeup-cap literal `30.0f → 18.0f`:
```cpp
      agc_({c.agcTargetDb, 50.0f, 500.0f, 18.0f, -10.0f, c.fs}),
```

In `src/denoise-core/agc.h` line 7, keep the header default consistent:
```cpp
  struct Config { float targetRmsDb=-15.0f; float attackMs=50.0f; float releaseMs=500.0f;
                  float maxGainDb=18.0f; float minGainDb=-10.0f; float fs=48000.0f; };
```

- [ ] **Step 4: Build + run — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target vc-ipc-tests denoise-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "Params|DspChain|Agc" --output-on-failure
```
Expected: PASS — all `Params.*`, `DspChain.*`, `Agc.*`.

- [ ] **Step 5: Commit**

```
git add src/vc-ipc/params.h src/denoise-core/dsp_chain.h src/denoise-core/dsp_chain.cpp src/denoise-core/agc.h tests/vc-ipc/test_params.cpp tests/denoise-core/test_dsp_chain.cpp tests/denoise-core/test_agc.cpp
git commit -m "feat(dsp): soften AGC default — target -20 dBFS, makeup cap +18 dB"
```

---

## Task 2: `MicProfile` tuning fields + `Settings` load/save

**Files:**
- Modify: `src/transport-core/settings.h`, `src/transport-core/settings.cpp`
- Test: `tests/transport-core/test_settings.cpp`

- [ ] **Step 1: Add the four fields to `MicProfile`**

In `src/transport-core/settings.h`, add to `struct MicProfile` (after `dspChain`):
```cpp
  float aggression     = 0.0f;    // .aggression
  float hpfHz          = 80.0f;   // .hpf_hz
  float outputGainDbfs = -20.0f;  // .output_gain_dbfs  (== agcTargetDbfs)
  float limiterDbfs    = -1.0f;   // .limiter_dbfs
```

- [ ] **Step 2: Add a failing round-trip test**

Append to `tests/transport-core/test_settings.cpp`:
```cpp
TEST(Settings, TuningFieldsRoundTrip) {
  Settings s;
  s.activeCaptureId = "cap-1"; s.renderId = "ren-1";
  vc::MicProfile& m = s.ensureProfile("cap-1", "K688");
  m.aggression = 0.65f; m.hpfHz = 90.0f; m.outputGainDbfs = -18.0f; m.limiterDbfs = -2.0f;
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings_tuning.toml").string();
  s.save(p);
  Settings r = Settings::load(p);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_FLOAT_EQ(r.mics[0].aggression, 0.65f);
  EXPECT_FLOAT_EQ(r.mics[0].hpfHz, 90.0f);
  EXPECT_FLOAT_EQ(r.mics[0].outputGainDbfs, -18.0f);
  EXPECT_FLOAT_EQ(r.mics[0].limiterDbfs, -2.0f);
}
TEST(Settings, OlderProfileWithoutTuningLoadsDefaults) {
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings_old.toml").string();
  std::ofstream(p) <<
    "[global]\nactive_capture_id=\"c1\"\nrender_id=\"r1\"\nring_frames=16\n"
    "[mic.\"c1\"]\nname=\"Mic\"\nengine=\"dfn3\"\n";
  Settings r = Settings::load(p);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_FLOAT_EQ(r.mics[0].aggression, 0.0f);
  EXPECT_FLOAT_EQ(r.mics[0].hpfHz, 80.0f);
  EXPECT_FLOAT_EQ(r.mics[0].outputGainDbfs, -20.0f);
  EXPECT_FLOAT_EQ(r.mics[0].limiterDbfs, -1.0f);
}
```

- [ ] **Step 3: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "Settings.TuningFieldsRoundTrip|Settings.OlderProfile" --output-on-failure
```
Expected: FAIL (`TuningFieldsRoundTrip` reads back defaults, not the saved values).

- [ ] **Step 4: Load + save the fields**

In `src/transport-core/settings.cpp` `Settings::load`, inside the `[mic]` parse loop (after `m.dspChain = ...`), add:
```cpp
          m.aggression     = (float)(*p)["aggression"].value_or(0.0);
          m.hpfHz          = (float)(*p)["hpf_hz"].value_or(80.0);
          m.outputGainDbfs = (float)(*p)["output_gain_dbfs"].value_or(-20.0);
          m.limiterDbfs    = (float)(*p)["limiter_dbfs"].value_or(-1.0);
```

In `Settings::save`, inside the `for (const auto& m : mics)` loop (after the `dsp_chain` insert), add:
```cpp
    p.insert_or_assign("aggression", (double)m.aggression);
    p.insert_or_assign("hpf_hz", (double)m.hpfHz);
    p.insert_or_assign("output_gain_dbfs", (double)m.outputGainDbfs);
    p.insert_or_assign("limiter_dbfs", (double)m.limiterDbfs);
```

- [ ] **Step 5: Run — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R Settings --output-on-failure
```
Expected: PASS (all `Settings.*`).

- [ ] **Step 6: Commit**

```
git add src/transport-core/settings.h src/transport-core/settings.cpp tests/transport-core/test_settings.cpp
git commit -m "feat(settings): per-mic tuning fields (aggression/hpf/gain/limiter)"
```

---

## Task 3: Pure `Slider`

**Files:**
- Create: `src/voicecleaner/slider.h`, `src/voicecleaner/slider.cpp`
- Modify: `src/voicecleaner/CMakeLists.txt`
- Create: `tests/voicecleaner/test_slider.cpp`; Modify: `tests/voicecleaner/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `src/voicecleaner/slider.h`:
```cpp
#pragma once
#include <string>
namespace vc::ui {

enum class SliderFmt { Dbfs, Ratio2, Hz, Db };

// Pure slider model (design §6): value in [min,max], fraction<->value, clamp, formatted label.
// No pixels — the window maps a click x to a fraction against the track rect and calls setFraction.
class Slider {
 public:
  Slider() = default;
  Slider(float min, float max, float value, SliderFmt fmt) : min_(min), max_(max), fmt_(fmt) { setValue(value); }
  float value() const { return value_; }
  float min() const { return min_; }
  float max() const { return max_; }
  float fraction() const;              // (value-min)/(max-min), clamped to 0..1
  void  setFraction(float t);          // value = min + clamp(t,0,1)*(max-min)
  void  setValue(float v);             // clamped to [min,max]
  std::string format() const;          // per fmt (e.g. "-20 dBFS", "0.50", "80 Hz", "-1 dB")

 private:
  float min_ = 0.0f, max_ = 1.0f, value_ = 0.0f;
  SliderFmt fmt_ = SliderFmt::Ratio2;
};

}  // namespace vc::ui
```

- [ ] **Step 2: Write the failing tests**

Create `tests/voicecleaner/test_slider.cpp`:
```cpp
#include <gtest/gtest.h>
#include "slider.h"
using namespace vc::ui;

TEST(Slider, FractionAndValueRoundTrip) {
  Slider g(-30.0f, -6.0f, -20.0f, SliderFmt::Dbfs);
  EXPECT_NEAR(g.fraction(), ((-20.0f) - (-30.0f)) / ((-6.0f) - (-30.0f)), 1e-5f);  // 10/24
  g.setFraction(0.0f); EXPECT_FLOAT_EQ(g.value(), -30.0f);
  g.setFraction(1.0f); EXPECT_FLOAT_EQ(g.value(), -6.0f);
  g.setFraction(0.5f); EXPECT_FLOAT_EQ(g.value(), -18.0f);
}
TEST(Slider, ClampsOutOfRange) {
  Slider g(-30.0f, -6.0f, -20.0f, SliderFmt::Dbfs);
  g.setValue(-100.0f); EXPECT_FLOAT_EQ(g.value(), -30.0f);
  g.setValue(100.0f);  EXPECT_FLOAT_EQ(g.value(), -6.0f);
  g.setFraction(-1.0f); EXPECT_FLOAT_EQ(g.value(), -30.0f);
  g.setFraction(2.0f);  EXPECT_FLOAT_EQ(g.value(), -6.0f);
}
TEST(Slider, FormatsPerUnit) {
  EXPECT_EQ(Slider(-30,-6,-20,SliderFmt::Dbfs).format(), "-20 dBFS");
  EXPECT_EQ(Slider(0,1,0.5f,SliderFmt::Ratio2).format(), "0.50");
  EXPECT_EQ(Slider(40,200,80,SliderFmt::Hz).format(), "80 Hz");
  EXPECT_EQ(Slider(-6,0,-1,SliderFmt::Db).format(), "-1 dB");
}
```

- [ ] **Step 3: Wire CMake**

In `src/voicecleaner/CMakeLists.txt`, change the core lib line to:
```cmake
add_library(voicecleaner-core STATIC ui_state.cpp device_picker.cpp slider.cpp)
```
In `tests/voicecleaner/CMakeLists.txt`, add `test_slider.cpp` to the `add_executable(voicecleaner-tests ...)` sources.

- [ ] **Step 4: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
```
Expected: FAIL — unresolved `Slider::fraction/setFraction/setValue/format`.

- [ ] **Step 5: Implement**

Create `src/voicecleaner/slider.cpp`:
```cpp
#include "slider.h"
#include <cstdio>
namespace vc::ui {

static float clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

float Slider::fraction() const {
  if (max_ <= min_) return 0.0f;
  return clamp01((value_ - min_) / (max_ - min_));
}
void Slider::setFraction(float t) { value_ = min_ + clamp01(t) * (max_ - min_); }
void Slider::setValue(float v) {
  value_ = v < min_ ? min_ : (v > max_ ? max_ : v);
}
std::string Slider::format() const {
  char buf[32];
  switch (fmt_) {
    case SliderFmt::Dbfs:   std::snprintf(buf, sizeof(buf), "%.0f dBFS", value_); break;
    case SliderFmt::Ratio2: std::snprintf(buf, sizeof(buf), "%.2f", value_); break;
    case SliderFmt::Hz:     std::snprintf(buf, sizeof(buf), "%.0f Hz", value_); break;
    case SliderFmt::Db:     std::snprintf(buf, sizeof(buf), "%.0f dB", value_); break;
  }
  return buf;
}

}  // namespace vc::ui
```

- [ ] **Step 6: Run — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R Slider --output-on-failure
```
Expected: PASS (all `Slider.*`).

- [ ] **Step 7: Commit**

```
git add src/voicecleaner/slider.h src/voicecleaner/slider.cpp src/voicecleaner/CMakeLists.txt tests/voicecleaner/test_slider.cpp tests/voicecleaner/CMakeLists.txt
git commit -m "feat(ui): pure Slider — fraction/value/clamp/format"
```

---

## Task 4: `UiState` — `seedTuning` + four setters

**Files:**
- Modify: `src/voicecleaner/ui_state.h`, `src/voicecleaner/ui_state.cpp`
- Test: `tests/voicecleaner/test_ui_state.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/voicecleaner/test_ui_state.cpp`:
```cpp
TEST(UiState, TuningSettersMutateCurrentAndReturnIt) {
  UiState ui;
  EXPECT_FLOAT_EQ(ui.onSetAggression(0.7f).aggression, 0.7f);
  EXPECT_FLOAT_EQ(ui.onSetHpf(120.0f).hpfCutoffHz, 120.0f);
  EXPECT_FLOAT_EQ(ui.onSetOutputGain(-18.0f).agcTargetDbfs, -18.0f);
  EXPECT_FLOAT_EQ(ui.onSetLimiter(-2.0f).limiterCeilingDbfs, -2.0f);
  EXPECT_FLOAT_EQ(ui.current().aggression, 0.7f);
  EXPECT_FLOAT_EQ(ui.current().agcTargetDbfs, -18.0f);
}
TEST(UiState, SeedTuningSetsAllFourAndSurvivesBypassToggle) {
  UiState ui;
  ui.seedTuning(/*aggr*/0.4f, /*hpf*/90.0f, /*gain*/-16.0f, /*lim*/-3.0f);
  EXPECT_FLOAT_EQ(ui.current().aggression, 0.4f);
  EXPECT_FLOAT_EQ(ui.current().hpfCutoffHz, 90.0f);
  EXPECT_FLOAT_EQ(ui.current().agcTargetDbfs, -16.0f);
  EXPECT_FLOAT_EQ(ui.current().limiterCeilingDbfs, -3.0f);
  // A bypass toggle carries the seeded tuning (tuning is UI-owned, not clobbered by telemetry).
  vc::ipc::Params sent = ui.onToggleBypass();
  EXPECT_FLOAT_EQ(sent.aggression, 0.4f);
  EXPECT_FLOAT_EQ(sent.agcTargetDbfs, -16.0f);
}
```

- [ ] **Step 2: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
```
Expected: FAIL — unresolved `onSetAggression`/`seedTuning`/etc.

- [ ] **Step 3: Declare in the header**

In `src/voicecleaner/ui_state.h`, add to the public `// ---- user actions ...` section (after `onToggleBypass`):
```cpp
  void seedTuning(float aggression, float hpfHz, float gainDbfs, float limiterDbfs);  // attach seed
  vc::ipc::Params onSetAggression(float v);
  vc::ipc::Params onSetHpf(float v);
  vc::ipc::Params onSetOutputGain(float v);
  vc::ipc::Params onSetLimiter(float v);
```

- [ ] **Step 4: Implement in the .cpp**

In `src/voicecleaner/ui_state.cpp`, add after `onToggleBypass`:
```cpp
void UiState::seedTuning(float aggression, float hpfHz, float gainDbfs, float limiterDbfs) {
  current_.aggression         = aggression;
  current_.hpfCutoffHz        = hpfHz;
  current_.agcTargetDbfs      = gainDbfs;
  current_.limiterCeilingDbfs = limiterDbfs;
}
vc::ipc::Params UiState::onSetAggression(float v)  { current_.aggression = v;         return current_; }
vc::ipc::Params UiState::onSetHpf(float v)         { current_.hpfCutoffHz = v;        return current_; }
vc::ipc::Params UiState::onSetOutputGain(float v)  { current_.agcTargetDbfs = v;      return current_; }
vc::ipc::Params UiState::onSetLimiter(float v)     { current_.limiterCeilingDbfs = v; return current_; }
```
(No change to `onTelemetry` — it still syncs only `engine`/`bypass`, so seeded tuning is never clobbered.)

- [ ] **Step 5: Run — verify pass (incl. the existing UiState tests)**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "UiState|Slider|DevicePicker" --output-on-failure
```
Expected: PASS (new tuning tests + all prior `UiState.*`).

- [ ] **Step 6: Commit**

```
git add src/voicecleaner/ui_state.h src/voicecleaner/ui_state.cpp tests/voicecleaner/test_ui_state.cpp
git commit -m "feat(ui): UiState tuning setters + seedTuning (UI-owned, telemetry-safe)"
```

---

## Task 5: Slider rows — layout, paint, drag (window glue)

**Files:**
- Modify: `src/voicecleaner/app_window.h`, `src/voicecleaner/app_window.cpp`

Manually verified (D2D). This task draws + drags the sliders; persistence/seeding is Task 6.

- [ ] **Step 1: Header — includes, slider members, rects, drag state, helpers**

In `src/voicecleaner/app_window.h`, add `#include "slider.h"` near the other includes. Add to the private section:
```cpp
  Slider sldGain_{-30.0f, -6.0f, -20.0f, SliderFmt::Dbfs};
  Slider sldAggr_{0.0f, 1.0f, 0.0f, SliderFmt::Ratio2};
  Slider sldHpf_{40.0f, 200.0f, 80.0f, SliderFmt::Hz};
  Slider sldLim_{-6.0f, 0.0f, -1.0f, SliderFmt::Db};
  D2D1_RECT_F rcGain_{}, rcAggr_{}, rcHpf_{}, rcLim_{};   // track rects
  int grabbed_ = -1;        // slider being dragged: 0=gain 1=aggr 2=hpf 3=lim, -1=none

  Slider& sliderByIndex(int i);
  D2D1_RECT_F& trackByIndex(int i);
  int trackHit(int x, int y) const;             // slider index whose track contains (x,y), or -1
  void setSliderFromX(int i, int x);            // set slider i's value from click/drag x
  bool aggressionEnabled() const;               // false when active engine is RNNoise
```

- [ ] **Step 2: Layout the four track rects; grow the window**

In `create()`, change the client size `RECT wr{0, 0, 460, 420};` to `RECT wr{0, 0, 460, 600};`.

In `layout()`, after the `rcRelaunch_`/`rcQuit_` lines, append the four track rects (a tuning block below bypass):
```cpp
  const float tw = sz.width - pad;
  rcGain_ = D2D1::RectF(pad, 372, tw, 384);
  rcAggr_ = D2D1::RectF(pad, 416, tw, 428);
  rcHpf_  = D2D1::RectF(pad, 460, tw, 472);
  rcLim_  = D2D1::RectF(pad, 504, tw, 516);
```
And move the down-state controls below the sliders — change `rcRelaunch_` and `rcQuit_` in `layout()` to:
```cpp
  rcRelaunch_  = D2D1::RectF(pad, 300, sz.width - pad, 300 + h);   // overlaps bypass row only when Down
  rcQuit_      = D2D1::RectF(sz.width - pad - 90, sz.height - 36, sz.width - pad, sz.height - 10);
```
(`rcRelaunch_` shares the bypass slot — it only paints/hit-tests when `statusKind()==Down`, when the bypass control is not the focus; that mutual exclusivity already holds in paint/hit-test.)

- [ ] **Step 3: Implement the slider helpers**

In `src/voicecleaner/app_window.cpp`, add near `hitControl()`:
```cpp
Slider& AppWindow::sliderByIndex(int i) {
  switch (i) { case 0: return sldGain_; case 1: return sldAggr_; case 2: return sldHpf_; default: return sldLim_; }
}
D2D1_RECT_F& AppWindow::trackByIndex(int i) {
  switch (i) { case 0: return rcGain_; case 1: return rcAggr_; case 2: return rcHpf_; default: return rcLim_; }
}
bool AppWindow::aggressionEnabled() const { return state_->activeEngine() == 1; }  // DFN3 only

int AppWindow::trackHit(int x, int y) const {
  // generous vertical hit band around each 12px track
  const D2D1_RECT_F rc[4] = { rcGain_, rcAggr_, rcHpf_, rcLim_ };
  for (int i = 0; i < 4; ++i) {
    if (i == 1 && !aggressionEnabled()) continue;   // aggression disabled on RNNoise
    if (x >= rc[i].left && x <= rc[i].right && y >= rc[i].top - 12 && y <= rc[i].bottom + 12) return i;
  }
  return -1;
}
void AppWindow::setSliderFromX(int i, int x) {
  D2D1_RECT_F& r = trackByIndex(i);
  float t = (r.right > r.left) ? ((float)x - r.left) / (r.right - r.left) : 0.0f;
  sliderByIndex(i).setFraction(t);
}
```

- [ ] **Step 4: Paint the four rows**

In `paint()`, just before the Quit control (section 6), add a tuning block. Insert:
```cpp
  // Tuning sliders (caption + value on one line, track with thumb below)
  auto drawSlider = [&](const wchar_t* cap, int i, bool enabled) {
    D2D1_RECT_F& tr = trackByIndex(i);
    ID2D1Brush* fg = enabled ? text : dim;
    label(cap, D2D1::RectF(tr.left, tr.top - 20, tr.right, tr.top - 4), dim, capFmt_);
    std::wstring val = enabled ? toW(sliderByIndex(i).format()) : L"DFN3 only";
    label(val, D2D1::RectF(tr.left, tr.top - 20, tr.right, tr.top - 4), fg, capFmtRight_);
    box(tr, panel);                                        // track
    float f = sliderByIndex(i).fraction();
    float cx = tr.left + f * (tr.right - tr.left);
    D2D1_RECT_F thumb = D2D1::RectF(cx - 6, tr.top - 6, cx + 6, tr.bottom + 6);
    box(thumb, enabled ? (grabbed_==i ? accentHot : accent) : disabled);
  };
  drawSlider(L"OUTPUT GAIN", 0, true);
  drawSlider(L"AGGRESSION",  1, aggressionEnabled());
  drawSlider(L"HPF",         2, true);
  drawSlider(L"LIMITER",     3, true);
```
This uses a right-aligned caption format `capFmtRight_`. Add it: in the header private members add `IDWriteTextFormat* capFmtRight_ = nullptr;`; in `create()` after `capFmt_` is set up, add:
```cpp
  dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               11.0f, L"en-us", &capFmtRight_);
  capFmtRight_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  capFmtRight_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
```
and release it in the destructor next to `capFmt_`: `if (capFmtRight_) capFmtRight_->Release();`.

- [ ] **Step 5: Drag handling in `handle()`**

In the `WM_LBUTTONDOWN` case, after the dropdown-overlay block and before the field checks (so a slider grab is caught when no dropdown is open), add:
```cpp
      { int s = trackHit(x, y);
        if (s >= 0) { grabbed_ = s; setSliderFromX(s, x); sendCurrent(applySlider(s)); InvalidateRect(hwnd_, nullptr, FALSE); return 0; } }
```
Add a `WM_MOUSEMOVE` drag branch — at the top of the existing `WM_MOUSEMOVE` case:
```cpp
      if (grabbed_ >= 0) {
        setSliderFromX(grabbed_, GET_X_LPARAM(l));
        sendCurrent(applySlider(grabbed_));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
```
Add a `WM_LBUTTONUP` case (persist on drag-end — Task 6 fills `persistTuning`; a stub for now):
```cpp
    case WM_LBUTTONUP:
      if (grabbed_ >= 0) { persistTuning(); grabbed_ = -1; }
      return 0;
```
Declare and stub the two helpers in the header/`.cpp`:
- header private: `vc::ipc::Params applySlider(int i); void persistTuning();`
- `.cpp` (Task 6 implements `persistTuning`; `applySlider` is real now):
```cpp
vc::ipc::Params AppWindow::applySlider(int i) {
  Slider& s = sliderByIndex(i);
  switch (i) {
    case 0: return state_->onSetOutputGain(s.value());
    case 1: return state_->onSetAggression(s.value());
    case 2: return state_->onSetHpf(s.value());
    default: return state_->onSetLimiter(s.value());
  }
}
void AppWindow::persistTuning() {}   // Task 6 implements
```

- [ ] **Step 6: Build**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner
```
Expected: builds clean. Sliders draw and drag (sending live `SetParams`); aggression greys on RNNoise. Values don't persist yet and aren't seeded from the profile — Task 6.

- [ ] **Step 7: Commit**

```
git add src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp
git commit -m "feat(ui): four tuning slider rows — layout, paint, live drag"
```

---

## Task 6: Seed sliders from the profile + persist on drag-end

**Files:**
- Modify: `src/voicecleaner/app_window.cpp`

- [ ] **Step 1: Seed sliders + `current_` from the active profile in `refreshDevices` (and on attach)**

`refreshDevices()` already runs on `create()` and when a dropdown opens. Extend it to seed the sliders + `UiState` tuning from the active mic's profile. Replace the body of `AppWindow::refreshDevices()` with:
```cpp
void AppWindow::refreshDevices() {
  std::vector<DeviceRow> cap, ren;
  enumerateDevices(cap, ren);
  picker_.setDevices(std::move(cap), std::move(ren));
  if (!settings_) return;
  picker_.setActive(settings_->activeCaptureId, settings_->renderId);
  if (vc::MicProfile* p = settings_->activeProfile()) {
    sldGain_.setValue(p->outputGainDbfs);
    sldAggr_.setValue(p->aggression);
    sldHpf_.setValue(p->hpfHz);
    sldLim_.setValue(p->limiterDbfs);
    state_->seedTuning(p->aggression, p->hpfHz, p->outputGainDbfs, p->limiterDbfs);
  }
}
```

- [ ] **Step 2: Push seeded tuning to the worker once it is ready**

The worker starts at `Params::defaults()`; the UI must send the seeded profile tuning once the worker is running. In `tick()`, after the `onTelemetry` call, add a one-shot push on the first ready transition. Add a `bool pushedTuning_ = false;` member (header), then in `tick()` replace the alive branch:
```cpp
  if (!r.alive) state_->onWorkerDown();
  else {
    state_->onTelemetry(r.telemetry, r.ready);
    if (r.ready && !pushedTuning_) {          // apply the seeded profile tuning once
      pushedTuning_ = true;
      vc::ipc::Command c; c.type = vc::ipc::CmdType::SetParams; c.params = state_->current();
      sup_->send(c);
    }
  }
```

- [ ] **Step 3: Implement `persistTuning` (write the active profile on drag-end)**

Replace the `persistTuning` stub in `app_window.cpp`:
```cpp
void AppWindow::persistTuning() {
  if (!settings_) return;
  vc::MicProfile* p = settings_->activeProfile();
  if (!p) return;
  p->outputGainDbfs = sldGain_.value();
  p->aggression     = sldAggr_.value();
  p->hpfHz          = sldHpf_.value();
  p->limiterDbfs    = sldLim_.value();
  settings_->save(configPath_);
}
```

- [ ] **Step 4: Build + full test suite**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```
Expected: builds; ctest 100% (prior 142 + new `Slider.*`, `Settings` tuning, `UiState` tuning tests).

- [ ] **Step 5: Commit**

```
git add src/voicecleaner/app_window.cpp
git commit -m "feat(ui): seed sliders from profile + push on ready + persist on drag-end"
```

---

## Task 7: Full regression + manual validation + STATUS

**Files:**
- Modify: `docs/STATUS.md`

- [ ] **Step 1: Full Debug + Release build + tests**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Release
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```
Expected: both configs build; ctest 100% in each.

- [ ] **Step 2: DFN3 golden gate**

Confirm no engine DSP source changed beyond the AGC default: `git diff --stat master -- src/dfn3 src/offline-cli` is empty, and `src/denoise-core` shows only `dsp_chain.*`/`agc.h`. The DFN3 offline golden is engine-only (no chain), so the AGC change cannot alter it; the `denoise-core` unit tests (updated in Task 1) are the gate and pass in Step 1.

- [ ] **Step 3: Manual hardware checklist (Release)**

Launch `build\src\voicecleaner\Release\voicecleaner.exe`, pick K688 + CABLE Input, then verify:
- [ ] Default output is clearly **quieter** than before (the −20 dBFS / +18 dB cap).
- [ ] **Output gain** slider changes loudness live; **HPF** changes low-end; **Limiter** changes ceiling.
- [ ] On **DFN3**, the **Aggression** slider is interactive and audibly changes suppression; on **RNNoise** it is greyed "DFN3 only".
- [ ] Tuning **persists** across a UI relaunch, and is **re-applied** on attach (not reset to defaults).
- [ ] Existing controls (device pickers, engine, bypass, Quit) still work.

- [ ] **Step 4: Update STATUS.md**

In `docs/STATUS.md`, update the "Active work" line: Phase 4c complete — four live tuning sliders (output gain / aggression / HPF / limiter), per-mic persistence, seeded + re-applied on attach, aggression greyed on RNNoise; AGC default softened (−20 dBFS, +18 dB cap). Note the new `Slider` unit + updated test count. Bump `_Last updated_` to 2026-08-17.

- [ ] **Step 5: Commit**

```
git add docs/STATUS.md
git commit -m "docs(status): Phase 4c tuning sliders complete"
```

---

## Self-Review

**Spec coverage:** §2 four sliders (Tasks 3,5) + AGC default (Task 1); §3 ranges/defaults (Task 3 Slider ctors, Task 5 members); §4 AGC softening at the exact anchors + test updates (Task 1); §5 MicProfile fields + load/save (Task 2); §6 pure Slider + UiState setters/seed (Tasks 3,4); §7 window rows + drag + aggression-greying (Task 5); §8 seed-on-attach + push-on-ready + persist-on-drag-end (Task 6); §9 older-profile-defaults (Task 2 test) + clamp (Task 3) + RNNoise no-op (Task 5 greying); §11 testing (all tasks). ✓

**Placeholder scan:** none. The Task 5 `persistTuning` stub is explicitly replaced with full code in Task 6 Step 3 (not left dangling).

**Type consistency:** `MicProfile` fields (`aggression`/`hpfHz`/`outputGainDbfs`/`limiterDbfs`) identical across Tasks 2 and 6. `Slider` API (`setValue`/`setFraction`/`fraction`/`value`/`format`) defined in Task 3 and used in Tasks 5–6. `UiState` methods (`seedTuning`, `onSetAggression`/`onSetHpf`/`onSetOutputGain`/`onSetLimiter`) defined in Task 4 and called in Task 5's `applySlider`. `Params` fields (`aggression`/`hpfCutoffHz`/`agcTargetDbfs`/`limiterCeilingDbfs`) match `src/vc-ipc/params.h`. Slider indices 0=gain/1=aggr/2=hpf/3=lim consistent across `sliderByIndex`/`trackByIndex`/`trackHit`/`applySlider` (Task 5).
