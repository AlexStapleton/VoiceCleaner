# RNNoise & Performance Improvements — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve the RNNoise path across quality, latency, and stability — without touching DFN3 — by adding a wet/dry "STRENGTH" control, an opt-in low-latency mode that stops RNNoise paying DFN3's 30 ms delay tax, and unattended device-loss recovery, plus the measurement infrastructure to tune the ring depth from real numbers.

**Architecture:** Additive. The STRENGTH control is a new pure `DryWetMix` unit inside `DspChain`, delay-aligned to the engine (mirrors the Declicker/NoiseGate slider machinery end-to-end: `Params` → relay → chain → per-mic `Settings` → `UiState` → D2D slider). Low-latency mode is a `Config`/`Settings` flag that tells the bridge not to pin the engine delay. Stability is supervisor auto-respawn plus `reset()`-able counters and the deferred telemetry finally populated. DFN3's default behavior is unchanged (STRENGTH defaults to 1.0 = full wet).

**Tech Stack:** C++20, `denoise-core` DSP primitives, `vc-ipc` Params/Telemetry, `transport-core` Config/Settings, `voicecleaner-core` UiState, Win32 + Direct2D, GoogleTest. CMake is the ESP-IDF-bundled one (NOT on PATH).

**Findings doc:** [`docs/superpowers/notes/2026-08-21-rnnoise-performance-findings.md`](../notes/2026-08-21-rnnoise-performance-findings.md) — update its "Measured numbers" + "Decisions log" as tasks land.

**Build/test:**
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```

**Order:** Workstream A (quality) → B (latency) → C (stability + measurement infra) → Phase V (one hardware validation + record numbers). A and C1/C2 are pure/testable and land without hardware; B2 and C3's tuning need the rig.

---

# Workstream A — RNNoise "STRENGTH" (engine-delay-aligned wet/dry mix)

## Task A1: The `DryWetMix` DSP unit (pure, RT-safe)

**Files:**
- Create: `src/denoise-core/dry_wet_mix.h`, `src/denoise-core/dry_wet_mix.cpp`
- Modify: `src/denoise-core/CMakeLists.txt`
- Create: `tests/denoise-core/test_dry_wet_mix.cpp`; Modify: `tests/denoise-core/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `src/denoise-core/dry_wet_mix.h`:
```cpp
#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// Blends an engine's WET (processed) output with the DRY (pre-engine) signal:
//   out[i] = s * wet[i] + (1 - s) * dryDelayed[i]
// The engine delays its output by `engineDelaySamples`, so the dry path is delayed by the same amount
// to keep the mix phase-aligned (no comb filtering). s in [0,1]; s=1 => pure wet (engine unchanged),
// s=0 => pure (delayed) dry. RT-safe: fixed buffer, no allocation in process(). out may alias wet.
class DryWetMix {
 public:
  explicit DryWetMix(size_t engineDelaySamples);
  // dry = pre-engine samples for THIS hop; wet = engine output for this hop (already delayed by the engine).
  void  process(const float* dry, const float* wet, float* out, size_t n, float strength) noexcept;
  void  reset() noexcept;
  size_t delaySamples() const noexcept { return L_; }
 private:
  size_t L_;
  std::vector<float> delay_;   // dry delay line (size L_, or 1 when L_==0 to keep indexing valid)
  size_t widx_ = 0;
};
}  // namespace vc
```

- [ ] **Step 2: Write the failing tests**

Create `tests/denoise-core/test_dry_wet_mix.cpp`:
```cpp
#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "dry_wet_mix.h"
using vc::DryWetMix;

TEST(DryWetMix, FullWetReturnsWetExactly) {
  DryWetMix m(0);
  const size_t n = 256;
  std::vector<float> dry(n), wet(n), out(n);
  for (size_t i = 0; i < n; ++i) { dry[i] = (float)i; wet[i] = -(float)i; }
  m.process(dry.data(), wet.data(), out.data(), n, 1.0f);
  for (size_t i = 0; i < n; ++i) EXPECT_FLOAT_EQ(out[i], wet[i]);
}

TEST(DryWetMix, ZeroDelayZeroStrengthReturnsDry) {
  DryWetMix m(0);
  const size_t n = 256;
  std::vector<float> dry(n), wet(n, 0.0f), out(n);
  for (size_t i = 0; i < n; ++i) dry[i] = std::sin(0.05f * i);
  m.process(dry.data(), wet.data(), out.data(), n, 0.0f);
  for (size_t i = 0; i < n; ++i) EXPECT_FLOAT_EQ(out[i], dry[i]);
}

TEST(DryWetMix, ZeroDelayHalfStrengthAverages) {
  DryWetMix m(0);
  const size_t n = 128;
  std::vector<float> dry(n), wet(n), out(n);
  for (size_t i = 0; i < n; ++i) { dry[i] = 1.0f; wet[i] = 3.0f; }
  m.process(dry.data(), wet.data(), out.data(), n, 0.5f);
  for (size_t i = 0; i < n; ++i) EXPECT_FLOAT_EQ(out[i], 2.0f);
}

TEST(DryWetMix, DryPathIsDelayedByEngineDelay) {
  const size_t L = 4, n = 32;
  DryWetMix m(L);
  std::vector<float> dry(n, 0.0f), wet(n, 0.0f), out(n);
  dry[0] = 1.0f;                                   // impulse enters dry at t=0
  m.process(dry.data(), wet.data(), out.data(), n, 0.0f);  // pure dry
  EXPECT_FLOAT_EQ(out[L], 1.0f);                   // emerges L samples later
  for (size_t i = 0; i < n; ++i) if (i != L) EXPECT_FLOAT_EQ(out[i], 0.0f);
}
```

- [ ] **Step 3: Wire CMake**

In `src/denoise-core/CMakeLists.txt`, add `dry_wet_mix.cpp` to the `denoise-core` sources.
In `tests/denoise-core/CMakeLists.txt`, add `test_dry_wet_mix.cpp` to the `denoise-core-tests` sources.

- [ ] **Step 4: Run to verify failure**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64 && "C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target denoise-core-tests`
Expected: FAIL — unresolved `DryWetMix`.

- [ ] **Step 5: Implement**

Create `src/denoise-core/dry_wet_mix.cpp`:
```cpp
#include "dry_wet_mix.h"
#include <algorithm>
namespace vc {

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

DryWetMix::DryWetMix(size_t engineDelaySamples) : L_(engineDelaySamples) {
  delay_.assign(L_ > 0 ? L_ : 1, 0.0f);
}

void DryWetMix::reset() noexcept { std::fill(delay_.begin(), delay_.end(), 0.0f); widx_ = 0; }

void DryWetMix::process(const float* dry, const float* wet, float* out, size_t n, float strength) noexcept {
  const float s = clamp01(strength);
  const float ds = 1.0f - s;
  if (L_ == 0) {                                   // no engine delay: align is trivial
    for (size_t i = 0; i < n; ++i) out[i] = s * wet[i] + ds * dry[i];
    return;
  }
  const size_t sz = delay_.size();
  for (size_t i = 0; i < n; ++i) {
    const float delayedDry = delay_[widx_];        // read-before-write => L_-sample delay
    delay_[widx_] = dry[i];
    widx_ = (widx_ + 1 == sz) ? 0 : widx_ + 1;
    out[i] = s * wet[i] + ds * delayedDry;
  }
}

}  // namespace vc
```

- [ ] **Step 6: Run — verify pass**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target denoise-core-tests && "C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R DryWetMix --output-on-failure`
Expected: PASS (all four `DryWetMix.*`).

- [ ] **Step 7: Commit**

```bash
git checkout -b feat/rnnoise-strength
git add src/denoise-core/dry_wet_mix.h src/denoise-core/dry_wet_mix.cpp src/denoise-core/CMakeLists.txt tests/denoise-core/test_dry_wet_mix.cpp tests/denoise-core/CMakeLists.txt
git commit -m "feat(dsp): DryWetMix — engine-delay-aligned wet/dry mix unit"
```

## Task A2: Insert `DryWetMix` into `DspChain` (after engine, before HPF)

**Files:**
- Modify: `src/denoise-core/dsp_chain.h`, `src/denoise-core/dsp_chain.cpp`
- Test: `tests/denoise-core/test_dsp_chain.cpp`

- [ ] **Step 1: Add failing chain tests**

Append to `tests/denoise-core/test_dsp_chain.cpp`:
```cpp
TEST(DspChain, MixDefaultIsFullWet) {
  EXPECT_FLOAT_EQ(vc::DspChain::Config{}.mixStrength, 1.0f);
}
TEST(DspChain, MixZeroPassesDrySignalThroughEngineStage) {
  // With a passthrough engine, wet == dry, so mix strength shouldn't change a steady tone's level;
  // this guards that the mix stage is wired without corrupting the buffer plumbing.
  vc::PassthroughFilter eng;
  vc::DspChain::Config cfg; cfg.mixStrength = 0.0f;
  vc::DspChain chain(&eng, cfg);
  std::vector<float> in(vc::FRAME_SAMPLES), out(vc::FRAME_SAMPLES);
  for (size_t i = 0; i < vc::FRAME_SAMPLES; ++i) in[i] = 0.2f * std::sin(0.03f * i);
  for (int f = 0; f < 4; ++f) chain.process_frame(in.data(), out.data());
  double r = 0; for (float v : out) r += (double)v * v;
  EXPECT_GT(std::sqrt(r / vc::FRAME_SAMPLES), 0.05);   // signal survives (not silenced/NaN)
}
```
(Ensure the file includes `<cmath>`/`<vector>`.)

- [ ] **Step 2: Run to verify failure**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target denoise-core-tests`
Expected: FAIL — `DspChain::Config` has no `mixStrength`.

- [ ] **Step 3: Add the stage**

In `src/denoise-core/dsp_chain.h`:
- add `#include "dry_wet_mix.h"`;
- in `struct Config`, after `limiterCeilingDb`, add `float mixStrength = 1.0f;   // 0..1 engine wet/dry (1 = full denoise)`;
- add member after `NoiseGate gate_;`: `DryWetMix mix_;`
- add ramp members after `gateCur_, gateTarget_;`: `float mixCur_, mixTarget_;`
- add setter/getter near the others:
  ```cpp
  void  setMixStrength(float s) noexcept   { mixTarget_ = s; }
  float currentMixStrength() const noexcept { return mixCur_; }
  ```

In `src/denoise-core/dsp_chain.cpp`:
- construct `mix_` in the initializer list — it must know the engine delay, so use the engine pointer:
  ```cpp
        gate_(NoiseGate::Config{c.fs}),
        mix_(engine ? engine->AlgorithmicDelaySamples() : 0),
  ```
  (place `mix_(...)` after `gate_(...)`; `engine` is the ctor param, already captured as `engine_`).
- add to the initializer list end: `, mixCur_(c.mixStrength), mixTarget_(c.mixStrength)`.
- in `process_frame`, right after the engine call + `vad` read (currently lines 39–40), insert the mix.
  The engine wrote `t2_` (wet); `t1_` still holds the DC-blocked dry input to the engine. Ramp + mix
  in place, leaving the denoised result in `t2_` for the HPF:
  ```cpp
    // 2. Engine.
    engine_->ProcessFrame(t1_, t2_);
    float vad = engine_->LastVadProbability();
    // 2b. Wet/dry mix (ramped). t1_ = dry (pre-engine), t2_ = wet (engine out, engine-delayed).
    mixCur_ = kRampCoef * mixCur_ + (1.0f - kRampCoef) * mixTarget_;
    mix_.process(t1_, t2_, t2_, FRAME_SAMPLES, mixCur_);   // out aliases wet (t2_) — allowed
    // 3. High-pass.
    hpf_.process(t2_, t1_, FRAME_SAMPLES);
  ```
- in `reset()`, add `mix_.reset(); mixCur_ = mixTarget_;`.

- [ ] **Step 4: Run — verify pass (no regression)**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target denoise-core-tests && "C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "DspChain|DryWetMix" --output-on-failure`
Expected: PASS — new tests + all existing `DspChain.*` (default `mixStrength=1.0` = full wet, and with a 0-delay passthrough engine the mix is a no-op, so existing timing assertions are unaffected).

- [ ] **Step 5: Commit**

```bash
git add src/denoise-core/dsp_chain.h src/denoise-core/dsp_chain.cpp tests/denoise-core/test_dsp_chain.cpp
git commit -m "feat(dsp): wet/dry STRENGTH mix in DspChain (after engine, before HPF)"
```

## Task A3: `Params.mixStrength` + relay

**Files:** Modify `src/vc-ipc/params.h`, `src/voicecleaner-worker/bridge.cpp`; Test `tests/vc-ipc/test_params.cpp`.

- [ ] **Step 1: Failing default test** — append to `tests/vc-ipc/test_params.cpp`:
```cpp
TEST(Params, MixStrengthDefaultsFullWet) {
  EXPECT_FLOAT_EQ(vc::ipc::Params::defaults().mixStrength, 1.0f);
}
```

- [ ] **Step 2: Run to verify failure**
Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target vc-ipc-tests`
Expected: FAIL — no `mixStrength`.

- [ ] **Step 3: Add the field + relay**
In `src/vc-ipc/params.h`, add after `gateStrength;`:
```cpp
  float    mixStrength;         // 0..1 engine wet/dry (1 = full denoise; RNNoise "STRENGTH")
```
and at the end of `defaults()` (after `/*gateStrength*/ 0.0f,`):
```cpp
        /*mixStrength*/ 1.0f,
```
In `src/voicecleaner-worker/bridge.cpp`, in the relay's `if (chain_) { ... }` block (after `setGateStrength`):
```cpp
        chain_->setMixStrength(sp.mixStrength);
```

- [ ] **Step 4: Run — verify pass**
Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target vc-ipc-tests voicecleaner-worker && "C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "Params|Command" --output-on-failure`
Expected: PASS — the whole-`Params` memcpy codec round-trips the new field with no codec change.

- [ ] **Step 5: Commit**
```bash
git add src/vc-ipc/params.h src/voicecleaner-worker/bridge.cpp tests/vc-ipc/test_params.cpp
git commit -m "feat(ipc): Params.mixStrength + relay applies it live"
```

## Task A4: Per-mic `mix_strength` in `Settings`

**Files:** Modify `src/transport-core/settings.h`, `.cpp`; Test `tests/transport-core/test_settings.cpp`.

- [ ] **Step 1: Failing round-trip test** — append to `tests/transport-core/test_settings.cpp`:
```cpp
TEST(Settings, MixStrengthRoundTrips) {
  Settings s; s.activeCaptureId = "cap-1"; s.renderId = "ren-1";
  vc::MicProfile& m = s.ensureProfile("cap-1", "K688");
  m.mixStrength = 0.65f;
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings_mix.toml").string();
  s.save(p);
  Settings r = Settings::load(p);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_FLOAT_EQ(r.mics[0].mixStrength, 0.65f);
}
TEST(Settings, MixStrengthDefaultsFullWet) {
  EXPECT_FLOAT_EQ(vc::MicProfile{}.mixStrength, 1.0f);
}
```

- [ ] **Step 2: Run to verify failure**
Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests`
Expected: FAIL — no `mixStrength`.

- [ ] **Step 3: Add the field + load/save**
In `src/transport-core/settings.h`, in `struct MicProfile` after `gateStrength`:
```cpp
  float mixStrength = 1.0f;       // .mix_strength (1 = full denoise)
```
In `src/transport-core/settings.cpp` `load` (in the `[mic]` loop, after the `gate_strength` read):
```cpp
          m.mixStrength = (float)(*p)["mix_strength"].value_or(1.0);
```
In `save` (after the `gate_strength` insert):
```cpp
    p.insert_or_assign("mix_strength", (double)m.mixStrength);
```

- [ ] **Step 4: Run — verify pass**
Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests && "C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R Settings --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**
```bash
git add src/transport-core/settings.h src/transport-core/settings.cpp tests/transport-core/test_settings.cpp
git commit -m "feat(settings): per-mic mix_strength (default full wet)"
```

## Task A5: `UiState` — `onSetMix` + `seedTuning` 8th arg

**Files:** Modify `src/voicecleaner/ui_state.h`, `.cpp`; Test `tests/voicecleaner/test_ui_state.cpp`.

Note: `seedTuning` currently takes 7 args (aggr, hpf, gain, lim, declick, voiceFloor, gate). Adding an 8th (`mix`) updates its one call site in `app_window.cpp` (Task A6).

- [ ] **Step 1: Failing tests** — append to `tests/voicecleaner/test_ui_state.cpp`:
```cpp
TEST(UiState, OnSetMixMutatesCurrent) {
  UiState ui;
  EXPECT_FLOAT_EQ(ui.onSetMix(0.4f).mixStrength, 0.4f);
  EXPECT_FLOAT_EQ(ui.current().mixStrength, 0.4f);
}
TEST(UiState, SeedTuningCarriesMix) {
  UiState ui;
  ui.seedTuning(/*aggr*/0.0f, /*hpf*/80.0f, /*gain*/-20.0f, /*lim*/-1.0f,
                /*declick*/0.0f, /*voiceFloor*/0.0f, /*gate*/0.0f, /*mix*/0.7f);
  EXPECT_FLOAT_EQ(ui.current().mixStrength, 0.7f);
}
```

- [ ] **Step 2: Run to verify failure**
Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests`
Expected: FAIL — `onSetMix` missing, `seedTuning` takes 7 args.

- [ ] **Step 3: Update header + impl**
In `src/voicecleaner/ui_state.h`, extend `seedTuning` and add the setter:
```cpp
  void seedTuning(float aggression, float hpfHz, float gainDbfs, float limiterDbfs,
                  float declick, float declickVoiceFloor, float gate, float mix);
  vc::ipc::Params onSetMix(float v);
```
In `src/voicecleaner/ui_state.cpp`, add `current_.mixStrength = mix;` as the last line of `seedTuning`'s body, update its signature to match, and add:
```cpp
vc::ipc::Params UiState::onSetMix(float v) { current_.mixStrength = v; return current_; }
```

- [ ] **Step 4: Run — verify pass**
Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests && "C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R UiState --output-on-failure`
Expected: PASS (the two new + all prior `UiState.*`; any other `seedTuning` call in the test file needs the 8th arg `, 1.0f`).

- [ ] **Step 5: Commit**
```bash
git add src/voicecleaner/ui_state.h src/voicecleaner/ui_state.cpp tests/voicecleaner/test_ui_state.cpp
git commit -m "feat(ui): UiState onSetMix + seedTuning carries mix"
```

## Task A6: Eighth "STRENGTH" slider row (window glue)

**Files:** Modify `src/voicecleaner/app_window.h`, `.cpp`. Manually verified (D2D); mirrors the existing slider rows exactly.

- [ ] **Step 1: Header** — add next to the other sliders in `app_window.h`:
```cpp
  Slider sldMix_{0.0f, 1.0f, 1.0f, SliderFmt::Ratio2};
```
and add `rcMix_{}` to the track-rect line (the `rcGain_, ... rcGate_` list).

- [ ] **Step 2: Extend the slider helpers to the new index** in `app_window.cpp`.
The current top slider index is gate (index 6). Add index 7 = mix:
- `sliderByIndex`: add `case 6: return sldGate_; default: return sldMix_;` (make the current gate `default` an explicit `case 6`, mix becomes `default`).
- `trackByIndex`: likewise add `case 6: return rcGate_; default: return rcMix_;`.
- `trackHit`: extend the array + loop bound to 8:
  ```cpp
  const D2D1_RECT_F rc[8] = { rcGain_, rcAggr_, rcHpf_, rcLim_, rcDck_, rcVfl_, rcGate_, rcMix_ };
  for (int i = 0; i < 8; ++i) {
  ```
- `applySlider`: make the current gate `default` an explicit `case 6: return state_->onSetGate(s.value());` and add `default: return state_->onSetMix(s.value());`.

- [ ] **Step 3: Layout — add the row, grow the window.** In `create()`, increase the client height by 44 px (one row). In `layout()`, add `rcMix_` one row below `rcGate_` (mirror the +44 px spacing used between the other track rects) and push the buttons below it down by the same amount. Match the existing row math exactly rather than hard-coding new constants where a stride is already computed.

- [ ] **Step 4: Paint the row.** In `paint()`, after the gate `drawSlider(...)` call, add `drawSlider(L"STRENGTH", 7, true);`.

- [ ] **Step 5: Seed + persist.** In `seedFromActiveProfile()` (and the `refreshDevices()` seeding block), set `sldMix_.setValue(p->mixStrength);` and pass the 8th `seedTuning` arg `p->mixStrength`. In `persistTuning()`, add `p->mixStrength = sldMix_.value();`.

- [ ] **Step 6: Help text.** In `helpText()`, add a case for the STRENGTH control's id: "STRENGTH — how hard the denoiser works. Lower blends your natural voice back in to reduce musical-noise/'laser' artifacts (RNNoise); 100% = full denoise." (Match the id scheme the other sliders use for their help ids.)

- [ ] **Step 7: Build (do NOT run — needs hardware).**
Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner`
Expected: builds clean.

- [ ] **Step 8: Commit**
```bash
git add src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp
git commit -m "feat(ui): eighth STRENGTH slider — live, seeded, persisted"
```

## Task A7: Regression + merge Workstream A

- [ ] Full Debug + Release build; `ctest` 100% in both. Record the new count.
- [ ] DFN3 golden untouched: `git diff --stat master -- src/dfn3 src/offline-cli` is empty; `src/denoise-core` shows only `dry_wet_mix.*` + `dsp_chain.*`.
- [ ] Merge `feat/rnnoise-strength` to master via superpowers:finishing-a-development-branch.

---

# Workstream B — Latency

## Task B1: Opt-in low-latency mode (stop pinning the engine delay)

**Goal:** a `low_latency` flag that makes the bridge NOT pin RNNoise to DFN3's 1440-sample delay, reclaiming ~30 ms on the RNNoise path. Cost: a one-time ~30 ms timing shift the first time the user switches to DFN3 (documented; RNNoise is default).

**Files:**
- `src/transport-core/config.h` / `.cpp` — add `bool lowLatency = false;` + TOML load.
- `src/transport-core/settings.h` / `.cpp` — `Settings.lowLatency` (global, like `ringFrames`) + `resolveActive()` copies it into `Config`. Test the round-trip.
- `src/voicecleaner-worker/bridge.cpp` — at the `dc.delaySamples[1] = kDfn3DelaySamples;` line, use `cfg_.lowLatency ? 0 : kDfn3DelaySamples`. (RNNoise stays 0 either way.)
- (Optional, later) UI checkbox; for now it's a settings.toml flag applied on (re)spawn, exactly like `ring_frames`.

- [ ] **Step 1 (test):** `Settings.LowLatencyRoundTrips` — `low_latency=true` survives save→load and `resolveActive()` sets `Config.lowLatency`.
- [ ] **Step 2:** add the `Config`/`Settings` field + TOML load/save; run the test to fail then pass.
- [ ] **Step 3:** apply it in the bridge (`delaySamples[1]`), build worker, full ctest green.
- [ ] **Step 4 (unit):** a `DynamicEngine` test — with `delaySamples = {0,0}`, `AlgorithmicDelaySamples()==0` and RNNoise output is not delay-shifted (add if not already covered).
- [ ] **Step 5:** commit `feat(latency): opt-in low_latency mode skips the DFN3 delay pin`.

**Interaction note:** in low-latency mode `AlgorithmicDelaySamples()` reports 0, so `DryWetMix` (Task A1) sizes its dry line to 0 — correct for RNNoise. If the user is on DFN3 *and* low-latency *and* lowers STRENGTH, the dry/wet will be misaligned (DFN3's real 1440 delay is uncompensated). That triple-combo is off the RNNoise happy path; document it in the findings doc rather than complicating the mix.

## Task B2: Measure end-to-end latency + tune the ring default (hardware)

**Goal:** replace the estimated latency budget with real numbers and set the shipped `ring_frames` default from data. Requires the rig (K688 → VB-Cable).

- [ ] Measure end-to-end latency (e.g. loopback click-to-output, or the existing `--record`/analysis tooling) at `ring_frames = 16`, pin ON — baseline into the findings doc.
- [ ] Measure again with `low_latency = true` — confirm ~30 ms reduction.
- [ ] Sweep `ring_frames` down (16 → 12 → 8 → 6 …) in low-latency mode; find the lowest dropout-free value over a few minutes of talk + typing. Record each step's dropout count (`ResetCounters` from C1 gives clean windows).
- [ ] Set the shipped `settings.toml` `ring_frames` default to the chosen value; update the LATENCY box help/`≈ ms` if the mapping shifts.
- [ ] Record all numbers in the findings doc "Measured numbers" table; commit any default change.

---

# Workstream C — Stability + measurement infrastructure

## Task C1: `reset()`-able counters + wire `ResetCounters`

**Files:** `src/transport-core/dropout.h`, `src/transport-core/frame_time_stats.h` (+ `.cpp` if needed), `src/voicecleaner-worker/bridge.h`/`.cpp`, `src/voicecleaner-worker/main.cpp`; tests in `tests/transport-core/`.

- [ ] **Step 1 (tests):**
  - `DropoutCounter.ResetZeroesCounts` — after recording, `reset()` returns `underruns()==0 && overruns()==0` and re-enables accounting.
  - `FrameTimeStats.ResetClearsHistogram` — after `record()`, `reset()` gives `count()==0`, `maxMs()==0`, `meanMs()==0`.
- [ ] **Step 2:** add `void reset() noexcept;` to both:
  - `DropoutCounter::reset()` — store 0 to `under_`/`over_` and `on_=true`.
  - `FrameTimeStats::reset()` — zero `buckets_`, `count_`, `sumMs_`, `maxMs_`.
- [ ] **Step 3:** wire the command. In `bridge.h`/`.cpp` add `void reset_counters()` that (RT-safely — call it from the main tick, not the relay) resets `dropouts_` + `frameStats_` + the ring high-water (C2). In `main.cpp` `ResetCounters` case, call `bridge.reset_counters();` (replace the no-op + stale comment).
- [ ] **Step 4:** build + `ctest` green; commit `feat(worker): reset()-able counters + wired ResetCounters`.

## Task C2: Populate deferred telemetry (`uptimeMs`, `ringBHigh`, `ringAOcc`)

**Files:** `src/voicecleaner-worker/bridge.h`/`.cpp` (relay publish path + a start timestamp + a ring-B high-water tracker). Optionally surface in the UI status line (`app_window.cpp`).

- [ ] **Step 1:** in `Bridge`, capture a `std::chrono::steady_clock::time_point start_` in `start()`; track `ringBHigh_` as the running max of `ringB_.size()` each hop (reset in `reset_counters()`).
- [ ] **Step 2:** in the ~1 Hz telemetry publish block, set:
  ```cpp
  t.ringAOcc  = (uint32_t)ringA_.size();
  t.ringBHigh = ringBHigh_;
  t.uptimeMs  = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_).count();
  ```
- [ ] **Step 3:** (optional) show uptime + ring high-water in the status line/help so headroom is watchable.
- [ ] **Step 4:** build + `ctest` green (telemetry POD already round-trips); commit `feat(worker): populate ringAOcc/ringBHigh/uptimeMs telemetry`.

## Task C3: Supervisor auto-respawn with backoff

**Goal:** when the worker dies **after having been ready** (e.g. device-loss exit), the UI auto-respawns it with bounded backoff instead of waiting for a manual Relaunch. A crash *before* ready (launch failure) does NOT auto-loop.

**Approach:** add a small pure policy (testable, like `LivenessTracker`) that decides, from `(alive, launchFailure, everReady, attempts, nowMs)`, whether to respawn now, wait, or give up after a cap. `app_window.cpp`'s `tick()` consults it when `poll()` reports death and calls `sup_->startOrAttach()`. Backoff (e.g. 0.5s → 1s → 2s → 4s, cap ~5 attempts) prevents a spin loop when the device is truly gone; surface "reconnecting… (n/5)" in the status line, and a terminal "worker stopped — click Relaunch" when the cap is hit.

**Files:** new pure `RespawnPolicy` in `voicecleaner-core` (`ui_state.*` or its own file) + tests; `app_window.cpp` `tick()` wiring; `worker_supervisor` already exposes `startOrAttach()` and `poll()` with `launchFailure`/`everReady`.

- [ ] **Detailed plan:** write `docs/superpowers/plans/2026-08-21-supervisor-auto-respawn.md` (full TDD for `RespawnPolicy` + the exact `tick()` wiring) before building — the backoff schedule, the ready-vs-launch-failure distinction, and the cap deserve their own test matrix. Branch: `feat/auto-respawn`.
- [ ] Execute it (policy tests → tick wiring → build clean).
- [ ] Commit + merge.

---

# Phase V — One hardware validation pass + record numbers

Run once, on the final build, with the K688 → VB-Cable rig. Record everything in the findings doc.

- [ ] **STRENGTH knob:** at 100% behavior is unchanged; lowering audibly reduces musical noise / "quiet lasers" while voice stays natural; persists per-mic across restart.
- [ ] **Low-latency mode:** on → audibly/measurably lower latency; switching to DFN3 once shows the expected one-time timing shift but no ongoing glitch; RNNoise↔RNNoise stable.
- [ ] **Ring sweep (Task B2):** find + set the lowest dropout-free default; numbers into the findings table.
- [ ] **Auto-respawn:** unplug/replug the mic (or disable/enable the endpoint) → audio recovers unattended within the backoff window; pulling it for good hits the cap and shows the Relaunch prompt.
- [ ] **Telemetry:** uptime climbs, ring high-water is plausible, `ResetCounters` (if surfaced) zeroes the readout.
- [ ] **CPU headroom:** frame-time mean/p99/max recorded; confirm ample margin under the 10 ms budget.
- [ ] Apply + commit any tuning fixes found; update the findings doc + STATUS.md; consider tagging.

---

## Self-Review

**Scope coverage:** quality → Workstream A (STRENGTH). latency → B1 (unpin) + B2 (measure/tune). CPU → C2 (visibility; findings say no blind optimization warranted). stability → C3 (auto-respawn) + C1 (reset for clean measurement). DFN3 untouched: STRENGTH default 1.0, `git diff` gate in A7. Deferred items (spectral R&D, IAudioClient3, model swap, endurance) are named in the findings doc, not silently dropped. ✓

**Placeholder scan:** Workstream A and Tasks B1/C1/C2 carry complete code. C3 defers to its own detailed plan (a genuinely separate subsystem with its own test matrix — a decomposition decision per writing-plans' scope-check, not a placeholder). B2/Phase V are inherently manual measurement checklists. ✓

**Type/name consistency:** `DryWetMix(engineDelaySamples)` / `process(dry,wet,out,n,strength)` (A1) used in A2; `DspChain::{Config::mixStrength, setMixStrength, currentMixStrength, mix_, mixCur_, mixTarget_}` (A2); `Params.mixStrength` default 1.0 (A3); `MicProfile.mixStrength` / `.mix_strength` (A4); `UiState.onSetMix` + 8-arg `seedTuning` (A5) called in A6; slider index 7 `sldMix_`/`rcMix_` consistent across helpers/paint/seed/persist (A6); `Config.lowLatency`/`Settings.lowLatency`/`low_latency` (B1); `DropoutCounter::reset`/`FrameTimeStats::reset`/`Bridge::reset_counters` (C1); `Telemetry.{ringAOcc,ringBHigh,uptimeMs}` (C2) match `telemetry.h`. ✓
