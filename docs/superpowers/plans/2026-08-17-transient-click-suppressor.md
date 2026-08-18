# Transient / Click Suppressor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a voice-safe `Declicker` DSP stage that ducks short broadband transients (keyboard/mouse clicks), controlled by a live per-mic "De-click" slider, on top of the existing engine + chain.

**Architecture:** A new pure, RT-safe `Declicker` (time-domain, look-ahead ducking with VAD back-off) inserted into `DspChain` after the HPF, before the AGC. A new `Params.declickStrength` field flows live over the existing pipe/relay; a fifth tuning slider reuses all the Phase 4c slider + per-mic-persistence machinery. No engine change; the DFN3 offline golden is untouched.

**Tech Stack:** C++20, the existing `denoise-core` DSP primitives (`Agc`/`Limiter` style), `vc-ipc` Params, `transport-core` Settings, Direct2D UI, GoogleTest.

**Design spec:** [`docs/superpowers/specs/2026-08-17-transient-click-suppressor-design.md`](../specs/2026-08-17-transient-click-suppressor-design.md)

**Status / where this fits:** Phases 3, 4a, 4c are merged to `master`. This is the next feature. Start it on a branch: `git checkout -b feat/declicker`.

**Build/test (CMake NOT on PATH):**
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```

---

## File Structure

**New (pure, unit-tested):** `src/denoise-core/declicker.{h,cpp}` (`vc::Declicker`) + `tests/denoise-core/test_declicker.cpp`.

**Modified:**
- `src/denoise-core/dsp_chain.{h,cpp}` — add the `Declicker` stage + live setter; `tests/denoise-core/test_dsp_chain.cpp`.
- `src/denoise-core/CMakeLists.txt` — add `declicker.cpp`; `tests/denoise-core/CMakeLists.txt` — add `test_declicker.cpp`.
- `src/vc-ipc/params.h` — add `declickStrength`; `tests/vc-ipc/test_params.cpp`.
- `src/voicecleaner-worker/bridge.cpp` — relay applies `declickStrength`.
- `src/transport-core/settings.{h,cpp}` — `MicProfile.declickStrength` + load/save; `tests/transport-core/test_settings.cpp`.
- `src/voicecleaner/ui_state.{h,cpp}` — `onSetDeclick` + extend `seedTuning`; `tests/voicecleaner/test_ui_state.cpp`.
- `src/voicecleaner/app_window.{h,cpp}` — fifth "DE-CLICK" slider row + seed/persist; taller window.

---

## Task 1: The `Declicker` DSP unit (pure, RT-safe)

**Files:**
- Create: `src/denoise-core/declicker.h`, `src/denoise-core/declicker.cpp`
- Modify: `src/denoise-core/CMakeLists.txt`
- Create: `tests/denoise-core/test_declicker.cpp`; Modify: `tests/denoise-core/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `src/denoise-core/declicker.h`:
```cpp
#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// Voice-safe transient/click suppressor (design §3). Time-domain: HF-emphasis -> fast/slow envelopes ->
// crest detection -> look-ahead gain duck whose depth scales with (1 - vad). RT-safe: no allocation in
// process(); fixed internal buffers. out must NOT alias in (there is a lookahead delay).
class Declicker {
 public:
  struct Config {
    float  fs = 48000.0f;
    size_t lookahead = 144;   // 3 ms delay so the duck is fully down before the click emerges
    float  hpHz = 3000.0f;    // detection HF-emphasis (RC high-pass) cutoff
    float  fastMs = 0.5f;     // fast peak-envelope decay
    float  slowMs = 50.0f;    // slow baseline envelope
    float  holdMs = 2.0f;     // hold the duck this long past detection (covers the pipe + click width)
    float  releaseMs = 6.0f;  // gain recovery after the hold
    float  crestK = 6.0f;     // env_fast must exceed crestK * env_slow to flag a click
  };
  explicit Declicker(const Config& c);
  // strength in [0,1] (0 = bypass, output = input delayed by lookahead). vad in [0,1] scales duck depth down.
  void  process(const float* in, float* out, size_t n, float strength, float vad) noexcept;
  void  setStrength(float s) noexcept;
  float currentStrength() const noexcept { return strength_; }
  void  reset() noexcept;
 private:
  size_t L_;
  float  hpA_;                          // RC high-pass coefficient
  float  fastCoef_, slowCoef_, relCoef_;
  size_t holdSamples_;
  float  hpPrevIn_ = 0.0f, hpPrevOut_ = 0.0f;
  float  envF_ = 0.0f, envS_ = 1e-6f;
  float  gain_ = 1.0f;
  int    holdLeft_ = 0;
  float  strength_ = 0.0f;
  std::vector<float> delay_;
  size_t widx_ = 0;
};
}  // namespace vc
```

- [ ] **Step 2: Write the failing tests**

Create `tests/denoise-core/test_declicker.cpp`:
```cpp
#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "declicker.h"
using vc::Declicker;

static Declicker make() { return Declicker(Declicker::Config{}); }

TEST(Declicker, ZeroStrengthIsPureDelay) {
  Declicker d = make();
  const size_t L = 144, n = 1000;
  std::vector<float> in(n), out(n);
  for (size_t i = 0; i < n; ++i) in[i] = std::sin(2*3.14159f*220.0f*i/48000.0f);
  d.process(in.data(), out.data(), n, /*strength*/0.0f, /*vad*/0.0f);
  for (size_t i = 0; i < L; ++i) EXPECT_FLOAT_EQ(out[i], 0.0f);           // pre-fill
  for (size_t i = L; i < n; ++i) EXPECT_NEAR(out[i], in[i - L], 1e-6f);   // exact delayed copy
}

TEST(Declicker, RemovesImpulseInSilence) {
  Declicker d = make();
  const size_t L = 144, n = 2000, P = 500;
  std::vector<float> in(n, 0.0f), out(n);
  in[P] = 1.0f;                                                            // a lone click
  d.process(in.data(), out.data(), n, /*strength*/1.0f, /*vad*/0.0f);
  EXPECT_LT(std::fabs(out[P + L]), 0.15f);                                 // click emerges heavily ducked
}

TEST(Declicker, PreservesSteadyToneAtFullStrength) {
  Declicker d = make();
  const size_t n = 6000;
  std::vector<float> in(n), out(n);
  for (size_t i = 0; i < n; ++i) in[i] = 0.3f * std::sin(2*3.14159f*300.0f*i/48000.0f);
  d.process(in.data(), out.data(), n, 1.0f, 0.0f);
  double rin = 0, rout = 0;                                               // steady-state RMS (skip onset)
  for (size_t i = 2000; i < n; ++i) { rin += in[i]*in[i]; rout += out[i]*out[i]; }
  EXPECT_NEAR(std::sqrt(rout), std::sqrt(rin), 0.1 * std::sqrt(rin));      // tone not falsely ducked
}

TEST(Declicker, VadBackoffLeavesImpulseWhenVoiceActive) {
  const size_t L = 144, n = 2000, P = 500;
  std::vector<float> in(n, 0.0f), out0(n), out1(n);
  in[P] = 1.0f;
  Declicker a = make(); a.process(in.data(), out0.data(), n, 1.0f, /*vad*/0.0f);
  Declicker b = make(); b.process(in.data(), out1.data(), n, 1.0f, /*vad*/1.0f);
  EXPECT_LT(std::fabs(out0[P + L]), 0.15f);   // vad=0 -> ducked
  EXPECT_GT(std::fabs(out1[P + L]), 0.8f);    // vad=1 -> passes (voice-safe)
}
```

- [ ] **Step 3: Wire CMake**

In `src/denoise-core/CMakeLists.txt`, add `declicker.cpp` to the `denoise-core` library sources.
In `tests/denoise-core/CMakeLists.txt`, add `test_declicker.cpp` to the `add_executable(denoise-core-tests ...)` sources.

- [ ] **Step 4: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target denoise-core-tests
```
Expected: FAIL — unresolved `Declicker` methods.

- [ ] **Step 5: Implement**

Create `src/denoise-core/declicker.cpp`:
```cpp
#include "declicker.h"
#include <cmath>
#include <algorithm>
namespace vc {

static float onepole(float ms, float fs) { return std::exp(-1.0f / (0.001f * ms * fs)); }
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

Declicker::Declicker(const Config& c) : L_(c.lookahead) {
  const float rc = 1.0f / (2.0f * 3.14159265f * c.hpHz);
  const float dt = 1.0f / c.fs;
  hpA_ = rc / (rc + dt);
  fastCoef_ = onepole(c.fastMs, c.fs);
  slowCoef_ = onepole(c.slowMs, c.fs);
  relCoef_  = onepole(c.releaseMs, c.fs);
  holdSamples_ = L_ + (size_t)(c.holdMs * 0.001f * c.fs);
  crestK_ = c.crestK;
  delay_.assign(L_ > 0 ? L_ : 1, 0.0f);
}

void Declicker::setStrength(float s) noexcept { strength_ = clamp01(s); }

void Declicker::reset() noexcept {
  hpPrevIn_ = hpPrevOut_ = 0.0f; envF_ = 0.0f; envS_ = 1e-6f; gain_ = 1.0f; holdLeft_ = 0;
  std::fill(delay_.begin(), delay_.end(), 0.0f); widx_ = 0;
}

void Declicker::process(const float* in, float* out, size_t n, float strength, float vad) noexcept {
  strength = clamp01(strength); strength_ = strength; vad = clamp01(vad);
  const size_t sz = delay_.size();
  for (size_t i = 0; i < n; ++i) {
    const float x = in[i];
    const float hp = hpA_ * (hpPrevOut_ + x - hpPrevIn_);   // RC high-pass on the detection copy
    hpPrevIn_ = x; hpPrevOut_ = hp;
    const float mag = std::fabs(hp);
    envF_ = mag > envF_ ? mag : envF_ * fastCoef_;          // fast peak envelope
    envS_ = envS_ * slowCoef_ + mag * (1.0f - slowCoef_);   // slow baseline
    if (strength > 0.0f && envF_ > crestK_ * (envS_ + 1e-6f)) holdLeft_ = (int)holdSamples_;
    const float depth  = holdLeft_ > 0 ? strength * (1.0f - vad) : 0.0f;
    if (holdLeft_ > 0) --holdLeft_;
    const float target = 1.0f - depth;
    gain_ = target < gain_ ? target                          // fast attack (down, within lookahead)
                           : gain_ * relCoef_ + target * (1.0f - relCoef_);  // smooth release (up)
    const float delayed = delay_[widx_];                     // read-before-write => L_-sample delay
    delay_[widx_] = x;
    widx_ = (widx_ + 1 == sz) ? 0 : widx_ + 1;
    out[i] = delayed * gain_;
  }
}

}  // namespace vc
```
Add `float crestK_ = 6.0f;` to the private members in `declicker.h` (used above).

- [ ] **Step 6: Run — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target denoise-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R Declicker --output-on-failure
```
Expected: PASS (all four `Declicker.*`). If `RemovesImpulseInSilence` is marginal, the click emerges at `out[P+L]`; the hold (`L_ + holdMs`) must cover that index — it does (hold ≥ L_). Do not weaken the assertion; adjust `holdMs`/`crestK` defaults if needed and note it.

- [ ] **Step 7: Commit**

```
git add src/denoise-core/declicker.h src/denoise-core/declicker.cpp src/denoise-core/CMakeLists.txt tests/denoise-core/test_declicker.cpp tests/denoise-core/CMakeLists.txt
git commit -m "feat(dsp): Declicker — look-ahead transient ducker with VAD back-off"
```

---

## Task 2: Insert `Declicker` into `DspChain`

**Files:**
- Modify: `src/denoise-core/dsp_chain.h`, `src/denoise-core/dsp_chain.cpp`
- Test: `tests/denoise-core/test_dsp_chain.cpp`

- [ ] **Step 1: Add a failing chain test**

Append to `tests/denoise-core/test_dsp_chain.cpp`:
```cpp
TEST(DspChain, DeclickReducesTransientWhenEnabled) {
  vc::PassthroughFilter eng;
  vc::DspChain::Config cfg; cfg.declickStrength = 1.0f;
  vc::DspChain chain(&eng, cfg);
  std::vector<float> in(vc::FRAME_SAMPLES, 0.0f), out(vc::FRAME_SAMPLES);
  in[10] = 1.0f;                              // a click early in the frame
  for (int f = 0; f < 4; ++f) chain.process_frame(in.data(), out.data());  // let the duck engage
  float peak = 0; for (float v : out) peak = std::max(peak, std::fabs(v));
  EXPECT_LT(peak, 0.5f);                      // transient is ducked (default chain would pass ~1.0)
}
TEST(DspChain, DeclickDefaultIsZeroStrength) {
  EXPECT_FLOAT_EQ(vc::DspChain::Config{}.declickStrength, 0.0f);
}
```
(If `test_dsp_chain.cpp` lacks `#include <algorithm>`/`<cmath>`/`<vector>`, add them.)

- [ ] **Step 2: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target denoise-core-tests
```
Expected: FAIL — `DspChain::Config` has no `declickStrength`.

- [ ] **Step 3: Add the stage to the chain**

In `src/denoise-core/dsp_chain.h`:
- add `#include "declicker.h"`;
- add `float declickStrength = 0.0f;` to `struct Config` (after `limiterCeilingDb`);
- add a member `Declicker declick_;` (after `Limiter lim_;`);
- add live control + ramp members: after the limiter ramp members, add
  `float dckCur_, dckTarget_;`
- add public setters/getters near the others:
  `void setDeclickStrength(float s) noexcept { dckTarget_ = s; }`
  `float currentDeclickStrength() const noexcept { return dckCur_; }`

In `src/denoise-core/dsp_chain.cpp`:
- in the constructor initializer list, construct `declick_` and init the ramp:
  ```cpp
        lim_(c.limiterCeilingDb, 144, 50.0f, c.fs),
        declick_(Declicker::Config{c.fs}),
  ```
  and after `limCur_(...), limTarget_(...)` add `, dckCur_(c.declickStrength), dckTarget_(c.declickStrength)`.
- in `process_frame`, ramp declick and insert it **after the HPF, before the AGC**. Change the HPF→AGC section so the buffers ping-pong through the declicker (it needs a non-aliasing out):
  ```cpp
    // 3. High-pass.
    hpf_.process(t2_, t1_, FRAME_SAMPLES);          // t1_ = HPF output
    // 3b. Declicker (ramped strength; VAD scales the duck down).
    dckCur_ = kRampCoef * dckCur_ + (1.0f - kRampCoef) * dckTarget_;
    declick_.process(t1_, t2_, FRAME_SAMPLES, dckCur_, vad);   // t2_ = declicked
    // 4. VAD hysteresis + hold (per-frame).  [unchanged, still uses `vad`]
    ...
    // 5. VAD-gated AGC.
    agc_.process(t2_, t1_, FRAME_SAMPLES, vadActive_);   // was (t1_, t2_); now t2_ -> t1_
    // 6. Limiter.
    lim_.process(t1_, out, FRAME_SAMPLES);               // was (t2_, out); now t1_
  ```
  (Only the buffer names on the AGC/limiter lines change — declick sits between them and the HPF.)
- in `reset()`, add `declick_.reset(); dckCur_ = dckTarget_;`.

- [ ] **Step 4: Run — verify pass (and no regression)**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target denoise-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "DspChain|Declicker" --output-on-failure
```
Expected: PASS — the two new tests plus all existing `DspChain.*` (the default `declickStrength=0` makes declick a pure 144-sample delay, which the existing chain tests tolerate; if any existing test asserts an exact input→output timing that the added 3 ms delay now shifts, update that test's expected index and note it).

- [ ] **Step 5: Commit**

```
git add src/denoise-core/dsp_chain.h src/denoise-core/dsp_chain.cpp tests/denoise-core/test_dsp_chain.cpp
git commit -m "feat(dsp): insert Declicker into DspChain (HPF -> declick -> AGC)"
```

---

## Task 3: `Params.declickStrength` + relay

**Files:**
- Modify: `src/vc-ipc/params.h`, `src/voicecleaner-worker/bridge.cpp`
- Test: `tests/vc-ipc/test_params.cpp`

- [ ] **Step 1: Add a failing default test**

Append to `tests/vc-ipc/test_params.cpp` (inside the existing defaults test or as a new one):
```cpp
TEST(Params, DeclickDefaultsOff) {
  vc::ipc::Params p = vc::ipc::Params::defaults();
  EXPECT_FLOAT_EQ(p.declickStrength, 0.0f);
}
```

- [ ] **Step 2: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target vc-ipc-tests
```
Expected: FAIL — `Params` has no `declickStrength`.

- [ ] **Step 3: Add the field + relay**

In `src/vc-ipc/params.h`, add the field after `dryTap` in the struct **and** a matching `0.0f` at the end of `defaults()`:
```cpp
  uint32_t dryTap;              // 0 / 1 (placeholder; behavior deferred)
  float    declickStrength;     // 0..1 transient/click ducker (0 = off)
```
```cpp
        /*dryTap*/ 0u,
        /*declickStrength*/ 0.0f,
```

In `src/voicecleaner-worker/bridge.cpp`, in the relay's `if (chain_) { ... }` block, add:
```cpp
        chain_->setDeclickStrength(sp.declickStrength);
```

- [ ] **Step 4: Run — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target vc-ipc-tests voicecleaner-worker
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R "Params|Command" --output-on-failure
```
Expected: PASS — the new default test and the existing `Command` round-trip (which memcpy's the whole `Params`, so the new field round-trips with no codec change). Worker builds.

- [ ] **Step 5: Commit**

```
git add src/vc-ipc/params.h src/voicecleaner-worker/bridge.cpp tests/vc-ipc/test_params.cpp
git commit -m "feat(ipc): Params.declickStrength + relay applies it live"
```

---

## Task 4: Per-mic `declick_strength` in `Settings`

**Files:**
- Modify: `src/transport-core/settings.h`, `src/transport-core/settings.cpp`
- Test: `tests/transport-core/test_settings.cpp`

- [ ] **Step 1: Add a failing round-trip test**

Append to `tests/transport-core/test_settings.cpp`:
```cpp
TEST(Settings, DeclickStrengthRoundTrips) {
  Settings s; s.activeCaptureId = "cap-1"; s.renderId = "ren-1";
  vc::MicProfile& m = s.ensureProfile("cap-1", "K688");
  m.declickStrength = 0.55f;
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_settings_declick.toml").string();
  s.save(p);
  Settings r = Settings::load(p);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_FLOAT_EQ(r.mics[0].declickStrength, 0.55f);
}
```

- [ ] **Step 2: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
```
Expected: FAIL — `MicProfile` has no `declickStrength`.

- [ ] **Step 3: Add the field + load/save**

In `src/transport-core/settings.h`, add to `struct MicProfile` (after `limiterDbfs`):
```cpp
  float declickStrength = 0.0f;   // .declick_strength
```
In `src/transport-core/settings.cpp` `load` (in the `[mic]` loop, after `m.limiterDbfs = ...`):
```cpp
          m.declickStrength = (float)(*p)["declick_strength"].value_or(0.0);
```
In `save` (after the `limiter_dbfs` insert):
```cpp
    p.insert_or_assign("declick_strength", (double)m.declickStrength);
```

- [ ] **Step 4: Run — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target transport-core-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R Settings --output-on-failure
```
Expected: PASS (all `Settings.*`).

- [ ] **Step 5: Commit**

```
git add src/transport-core/settings.h src/transport-core/settings.cpp tests/transport-core/test_settings.cpp
git commit -m "feat(settings): per-mic declick_strength"
```

---

## Task 5: `UiState` — `onSetDeclick` + `seedTuning` extension

**Files:**
- Modify: `src/voicecleaner/ui_state.h`, `src/voicecleaner/ui_state.cpp`
- Test: `tests/voicecleaner/test_ui_state.cpp`

Note: `seedTuning` gains a fifth parameter, so its existing call site (`app_window.cpp refreshDevices`) is updated in Task 6.

- [ ] **Step 1: Add failing tests**

Append to `tests/voicecleaner/test_ui_state.cpp`:
```cpp
TEST(UiState, OnSetDeclickMutatesCurrent) {
  UiState ui;
  EXPECT_FLOAT_EQ(ui.onSetDeclick(0.6f).declickStrength, 0.6f);
  EXPECT_FLOAT_EQ(ui.current().declickStrength, 0.6f);
}
TEST(UiState, SeedTuningCarriesDeclick) {
  UiState ui;
  ui.seedTuning(/*aggr*/0.4f, /*hpf*/90.0f, /*gain*/-16.0f, /*lim*/-3.0f, /*declick*/0.7f);
  EXPECT_FLOAT_EQ(ui.current().declickStrength, 0.7f);
  EXPECT_FLOAT_EQ(ui.current().aggression, 0.4f);   // still carries the others
}
```

- [ ] **Step 2: Run to verify failure**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
```
Expected: FAIL — `onSetDeclick` missing and `seedTuning` takes 4 args.

- [ ] **Step 3: Update the header + impl**

In `src/voicecleaner/ui_state.h`, change the `seedTuning` declaration and add the setter:
```cpp
  void seedTuning(float aggression, float hpfHz, float gainDbfs, float limiterDbfs, float declick);
  vc::ipc::Params onSetDeclick(float v);
```
In `src/voicecleaner/ui_state.cpp`, update `seedTuning` and add `onSetDeclick`:
```cpp
void UiState::seedTuning(float aggression, float hpfHz, float gainDbfs, float limiterDbfs, float declick) {
  current_.aggression         = aggression;
  current_.hpfCutoffHz        = hpfHz;
  current_.agcTargetDbfs      = gainDbfs;
  current_.limiterCeilingDbfs = limiterDbfs;
  current_.declickStrength    = declick;
}
vc::ipc::Params UiState::onSetDeclick(float v) { current_.declickStrength = v; return current_; }
```

- [ ] **Step 4: Run — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R UiState --output-on-failure
```
Expected: PASS (the two new + all prior `UiState.*`; the old 4-arg `seedTuning` tests were updated in Step 1's file — if any other test calls `seedTuning` with 4 args, add the `, 0.0f` fifth arg).

- [ ] **Step 5: Commit**

```
git add src/voicecleaner/ui_state.h src/voicecleaner/ui_state.cpp tests/voicecleaner/test_ui_state.cpp
git commit -m "feat(ui): UiState onSetDeclick + seedTuning carries declick"
```

---

## Task 6: Fifth "DE-CLICK" slider row + seed/persist (window glue)

**Files:**
- Modify: `src/voicecleaner/app_window.h`, `src/voicecleaner/app_window.cpp`

Manually verified (D2D). Mirrors the four Phase 4c slider rows exactly.

- [ ] **Step 1: Header — add the slider, track rect, and extend the index range**

In `src/voicecleaner/app_window.h`, add next to the other `Slider` members:
```cpp
  Slider sldDck_{0.0f, 1.0f, 0.0f, SliderFmt::Ratio2};
```
and add `rcDck_{}` to the track-rect line:
```cpp
  D2D1_RECT_F rcGain_{}, rcAggr_{}, rcHpf_{}, rcLim_{}, rcDck_{};
```

- [ ] **Step 2: Extend the slider helpers to index 4**

In `src/voicecleaner/app_window.cpp`:
- `sliderByIndex`: `case 3: return sldLim_; default: return sldDck_;` (change the `default` from `sldLim_` to `sldDck_`, and make `3` explicit).
  ```cpp
  Slider& AppWindow::sliderByIndex(int i) {
    switch (i) { case 0: return sldGain_; case 1: return sldAggr_; case 2: return sldHpf_; case 3: return sldLim_; default: return sldDck_; }
  }
  ```
- `trackByIndex`: likewise add `case 3: return rcLim_; default: return rcDck_;`.
- `trackHit`: extend the array + loop bound to 5:
  ```cpp
  const D2D1_RECT_F rc[5] = { rcGain_, rcAggr_, rcHpf_, rcLim_, rcDck_ };
  for (int i = 0; i < 5; ++i) {
  ```
- `applySlider`: add the declick case:
  ```cpp
    case 3: return state_->onSetLimiter(s.value());
    default: return state_->onSetDeclick(s.value());
  ```
  (change the current `default` (limiter) to an explicit `case 3`, and make declick the new `default`).

- [ ] **Step 3: Layout — add the row, grow the window**

In `create()`, change the window client height `RECT wr{0, 0, 460, 620};` to `RECT wr{0, 0, 460, 664};`.
In `layout()`, add the declick track after `rcLim_` and push Relaunch down:
```cpp
  rcLim_  = D2D1::RectF(pad, 504, tw, 516);
  rcDck_  = D2D1::RectF(pad, 548, tw, 560);
  rcRelaunch_  = D2D1::RectF(pad, 580, sz.width - pad, 580 + h);
```
(`rcQuit_` stays `sz.height - 36 … sz.height - 10` — now 628…654, clear of Relaunch at 580…620.)

- [ ] **Step 4: Paint the row**

In `paint()`, after the four `drawSlider(...)` calls, add:
```cpp
  drawSlider(L"DE-CLICK", 4, true);
```

- [ ] **Step 5: Seed + persist the declick slider**

In `refreshDevices()`, in the `if (vc::MicProfile* p = settings_->activeProfile())` block, seed the declick slider and pass the fifth `seedTuning` arg:
```cpp
    sldDck_.setValue(p->declickStrength);
    state_->seedTuning(p->aggression, p->hpfHz, p->outputGainDbfs, p->limiterDbfs, p->declickStrength);
```
In `persistTuning()`, add:
```cpp
  p->declickStrength = sldDck_.value();
```

- [ ] **Step 6: Build**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner
```
Expected: builds clean. Do NOT run the exe (needs hardware). The fifth slider draws, drags live, seeds, and persists.

- [ ] **Step 7: Commit**

```
git add src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp
git commit -m "feat(ui): fifth DE-CLICK slider row — live, seeded, persisted"
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
Expected: both build; ctest 100% in each (prior + `Declicker.*`, the new chain/params/settings/uistate tests).

- [ ] **Step 2: DFN3 golden gate**

Confirm the engine is untouched: `git diff --stat master -- src/dfn3 src/offline-cli` is empty; `src/denoise-core` shows only `declicker.*` + `dsp_chain.*`. The offline golden (`offline-cli --engine dfn3`, no `--chain`) does not run the declicker, so it is unaffected; the `denoise-core` unit tests are the gate and pass in Step 1.

- [ ] **Step 3: Manual hardware checklist (Release)**

Launch `build\src\voicecleaner\Release\voicecleaner.exe`, pick devices, and verify:
- [ ] With **DE-CLICK at 0**, behavior is unchanged (baseline).
- [ ] Raising **DE-CLICK** audibly reduces keyboard/mouse clicks **in the gaps between words**.
- [ ] At moderate strength, spoken consonants stay reasonably crisp; pushing to 1.0 may soften them (expected).
- [ ] Clicks landing on top of loud speech still partly leak (expected — voice-safe tradeoff).
- [ ] The setting persists across a relaunch and is re-applied on attach.
- [ ] Latency is still low; no dropouts/instability with declick engaged.

_(Note any threshold that felt wrong — `crestK`, `holdMs`, `hpHz` in `Declicker::Config` are the tuning knobs.)_

- [ ] **Step 4: Update STATUS.md**

Update the "Active work" line: transient/click **Declicker** shipped — look-ahead ducker (HF-emphasis + crest detect + VAD back-off) after HPF, live `declickStrength` param + per-mic persistence + fifth "DE-CLICK" slider; default off; DFN3 golden unaffected; new test count. Move Phase 4c to history. Bump `_Last updated_`.

- [ ] **Step 5: Commit + finish**

```
git add docs/STATUS.md
git commit -m "docs(status): transient/click Declicker complete"
```
Then merge to `master` via superpowers:finishing-a-development-branch (tests already green).

---

## Self-Review

**Spec coverage:** §2 conservative/duck/lookahead/placement/one-slider/default-off — Tasks 1 (algorithm), 2 (placement), 6 (slider), 3 (default 0). §3 Declicker unit — Task 1. §4 chain integration + latency note — Task 2. §5 Params + relay + codec (memcpy, no change) — Task 3. §6 Settings + UiState + slider — Tasks 4,5,6. §7 tests (impulse/steady/VAD/RT + wiring + golden) — Tasks 1–7. §8 risks (by-ear tuning via Config knobs) — Task 7 Step 3 note. ✓

**Placeholder scan:** none — every code step is complete. The only "adjust if marginal" notes (Task 1 Step 6, Task 2 Step 4) point at concrete knobs (`holdMs`/`crestK`, a test index), not vague TODOs.

**Type consistency:** `Declicker::process(in,out,n,strength,vad)` and `Config` fields (`lookahead`/`hpHz`/`fastMs`/`slowMs`/`holdMs`/`releaseMs`/`crestK`) are defined in Task 1 and used in Task 2. `DspChain::setDeclickStrength`/`currentDeclickStrength`/`Config::declickStrength` consistent Tasks 2. `Params.declickStrength` consistent Tasks 3,5. `MicProfile.declickStrength` consistent Tasks 4,6. `UiState.onSetDeclick` + 5-arg `seedTuning` defined Task 5, called Task 6. Slider index 4 (`sldDck_`/`rcDck_`) consistent across `sliderByIndex`/`trackByIndex`/`trackHit`/`applySlider`/paint/seed/persist (Task 6).
