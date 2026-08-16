# Product Shell — Phase 2b (Live DFN3 Aggression) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the aggression parameter actually affect the live (streaming) DFN3 engine — un-stubbing what is today a no-op — so the UI's aggression slider shapes real-time noise suppression, matching the already-validated offline behavior.

**Architecture:** DFN3 aggression shapes the ERB gain mask before it's applied: `g' = max(gMin, g^gamma)`, `gamma = aggression_gamma(slider)`. This exists and is validated in the OFFLINE path (`src/dfn3/dfn3_offline.cpp:222-226`) but is a no-op on the live path. We apply the identical transform per-hop in the streaming orchestrator (`Dfn3Streaming`), forward it through the adapter's `SetAggression`, expose it via offline-cli for a fidelity check, and feed it from the Bridge relay's `Params` snapshot. Aggression is DFN3-only (RNNoise/passthrough `SetAggression` are no-ops and stay that way).

**Tech Stack:** C++20, MSVC, CMake (ESP-IDF-bundled), GoogleTest for the unchanged unit suite; Python (`tools/dfn3_compare.py`) for the DSP fidelity check (same as the existing DFN3 validation — not a ctest).

**Build/test commands** (CMake not on PATH):
- Configure: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64`
- Build (Debug): `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
- Full unit suite: `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure`

**Key facts (verified in the code):**
- Streaming per-hop ERB mask: fetched at `src/dfn3/dfn3_streaming.cpp:575` (`const float* mask = erbDec.outData(erbM);  // [32]`), applied at `:591` (`apply_erb_mask(fb, mask, outRe.data(), outIm.data());`). Aggression must shape it in between.
- ERB bands = **32** (`ErbFilterBank` default `nBands=32`); the streaming mask is exactly 32 floats (one frame).
- `apply_aggression(float* gains, size_t n, float gamma, float gMin)` and `float aggression_gamma(float slider)` live in `src/denoise-core/aggression.h` (already unit-tested).
- Offline gate: aggression is applied only when `slider > 0` (mapping gives `gamma(0)=0.4`, not identity), keeping the default path byte-identical. Replicate that gate.
- `Dfn3StreamAdapter::SetAggression(float)` is a no-op stub (`src/denoise-core/dfn3_stream_adapter.h:14`).
- `SwitchableEngine::SetAggression(z)` already forwards to BOTH leaves (`switchable_engine.cpp:27`), so feeding the engine under warm-standby reaches the DFN3 leaf.
- `Params::defaults().aggression == 0.0f`, so wiring `SetAggression(snapshot.aggression)` is a no-op at defaults → the 100-test suite and live default behavior stay unchanged.

---

## File Structure
- **Modify** `src/dfn3/dfn3_streaming.h` — add `void setAggression(float) noexcept;`.
- **Modify** `src/dfn3/dfn3_streaming.cpp` — Impl state (`aggressionSlider_`, scratch `aggMask_[32]`), the setter, and the per-hop mask shaping; include `aggression.h`.
- **Modify** `src/denoise-core/dfn3_stream_adapter.h` + (new impl in) `dfn3_stream_adapter.cpp` — un-stub `SetAggression` to forward to `Dfn3Streaming::setAggression`.
- **Modify** `src/offline-cli/main.cpp` — pass `--aggression` into the `dfn3-stream`/`dfn3-std-stream` path (enables the fidelity check).
- **Modify** `src/voicecleaner-worker/bridge.cpp` — relay reads `sp.aggression` and calls `engine_->SetAggression(...)` per hop.

---

## Task 1: Live aggression in the streaming DFN3 path (+ offline-cli wiring + fidelity check)

**Files:** `src/dfn3/dfn3_streaming.{h,cpp}`, `src/denoise-core/dfn3_stream_adapter.{h,cpp}`, `src/offline-cli/main.cpp`

- [ ] **Step 1: Read the code first**

Read `src/dfn3/dfn3_streaming.cpp` around the `struct Impl` definition (member layout) and around lines
560-600 (the per-hop decode + mask application), and `src/dfn3/dfn3_offline.cpp:216-227` (the reference
aggression application). Confirm the mask is 32 floats per hop.

- [ ] **Step 2: Add the setter to the header**

In `src/dfn3/dfn3_streaming.h`, add a public method (next to `reset()`):
```cpp
  void setAggression(float slider) noexcept;   // 0..1; shapes the ERB mask (DFN3-only). 0 = off (identity).
```

- [ ] **Step 3: Implement state + setter + per-hop shaping**

In `src/dfn3/dfn3_streaming.cpp`:
- Add `#include "aggression.h"` with the other denoise-core includes.
- In `struct Impl`, add two members:
```cpp
    float aggressionSlider_ = 0.0f;   // 0 = off; >0 shapes the ERB mask like the offline path
    float aggMask_[32];               // scratch: shaped copy of the 32-band mask (RT-safe, no alloc)
```
- Implement the setter (after the class's other out-of-line methods; adapt to how Impl is accessed —
  the public methods delegate to `p_`):
```cpp
void Dfn3Streaming::setAggression(float slider) noexcept {
  p_->aggressionSlider_ = slider < 0.0f ? 0.0f : (slider > 1.0f ? 1.0f : slider);
}
```
- In the per-hop `process()` (Impl), immediately AFTER `const float* mask = erbDec.outData(erbM);`
  (line ~575) and BEFORE `apply_erb_mask(...)` (line ~591), shape the mask when aggression is on. Since
  `mask` is `const float*` into the runner's buffer, copy into the scratch and repoint:
```cpp
    // Optional aggression: shape the ERB gain mask harder toward attenuation (g' = max(gMin, g^gamma)),
    // identical to the offline path (dfn3_offline.cpp). Gate on slider>0 so aggression 0 is byte-identical
    // to the model's native mask. Only affects ERB bins; deep-filtered low bins (<96) are untouched.
    if (aggressionSlider_ > 0.0f) {
      std::copy(mask, mask + 32, aggMask_);
      apply_aggression(aggMask_, 32, aggression_gamma(aggressionSlider_), 0.1f);  // 0.1 = -20 dB floor
      mask = aggMask_;
    }
```
  (Use the correct member-access form for wherever `process()` lives — inside `Impl` the members are
  `aggressionSlider_`/`aggMask_` directly; if `process()` is a free-standing `Dfn3Streaming::process`
  delegating to `p_`, use `p_->...`. Match the file's existing style. `<algorithm>` is needed for
  `std::copy` — add the include if not present.)

- [ ] **Step 4: Un-stub the adapter**

In `src/denoise-core/dfn3_stream_adapter.h`, change the inline no-op to a declaration:
```cpp
  void     SetAggression(float z) noexcept override;   // forwards to Dfn3Streaming (DFN3 mask shaping)
```
In `src/denoise-core/dfn3_stream_adapter.cpp`, implement it:
```cpp
void Dfn3StreamAdapter::SetAggression(float z) noexcept { eng_->setAggression(z); }
```

- [ ] **Step 5: Wire --aggression into the streaming offline path (for the fidelity check)**

In `src/offline-cli/main.cpp`, inside the `if (useDfn3Stream || useDfn3StdStream)` block, right after
`vc::Dfn3Streaming stream(modelDir);`, add:
```cpp
      stream.setAggression(aggression);
```
Also update the `--aggression` comment at line ~24/27 to note it now also affects the streaming engines.

- [ ] **Step 6: Build**

Configure + build (at least targets `dfn3`, `dfn3-engine`, `offline-cli`, and the full `denoise-core-tests`):
`"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Expected: clean build (only the pre-existing C4324/LNK4098 warnings).

- [ ] **Step 7: Regression — unit suite unchanged**

`"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure`
Expected: 100/100 (this task changes no unit tests; `apply_aggression`'s own tests still pass).

- [ ] **Step 8: DSP fidelity check (script; report the numbers)**

Use `val_in.wav` (in the repo root) and the standard model both ways. Run from the repo root; the
`offline-cli.exe` is at `build/src/offline-cli/Debug/offline-cli.exe` and needs `models/dfn3` present
(it is). For each aggression A in {0.0, 0.5, 1.0}:
```
build/src/offline-cli/Debug/offline-cli.exe --engine dfn3          --aggression A val_in.wav wholeA.wav
build/src/offline-cli/Debug/offline-cli.exe --engine dfn3-std-stream --aggression A val_in.wav streamA.wav
python tools/dfn3_compare.py wholeA.wav streamA.wav
```
(If `dfn3_compare.py`'s argument form differs, adapt — it computes Pearson r between two WAVs; read its
`--help`/top comment.) 

**Acceptance:**
- Streaming matches whole-utterance at every A: Pearson **r ≥ 0.999** for A = 0.0, 0.5, 1.0 (this
  reproduces the existing Gate-B agreement — aggression is an element-wise mask op, so framing is the
  only difference, exactly as at A=0).
- Aggression actually does something: `dfn3_compare.py stream0.0.wav stream1.0.wav` shows r **noticeably
  below 1** (e.g. < 0.99) — proof the slider changes the output.

Report all r values in the task report. If the A≥0.5 streaming-vs-whole r drops well below 0.999, STOP
and report BLOCKED — that means the per-hop shaping isn't equivalent to whole-utterance and needs
investigation (do not proceed to Task 2).

- [ ] **Step 9: Commit**
```bash
git add src/dfn3/dfn3_streaming.h src/dfn3/dfn3_streaming.cpp src/denoise-core/dfn3_stream_adapter.h src/denoise-core/dfn3_stream_adapter.cpp src/offline-cli/main.cpp
git commit -m "feat(dfn3): live streaming aggression (ERB-mask shaping); offline-cli --aggression for streaming"
```

---

## Task 2: Feed aggression from the Bridge relay

**Files:** `src/voicecleaner-worker/bridge.cpp`

- [ ] **Step 1: Read the relay**

In `src/voicecleaner-worker/bridge.cpp` `relayLoop()`, find the block (added in Phase 2) that reads the
`Params` snapshot and pushes to the chain:
```cpp
      const vc::ipc::Params sp = params_.snapshot();
      if (chain_) {
        chain_->setHpfCutoffHz(sp.hpfCutoffHz);
        chain_->setAgcTargetDb(sp.agcTargetDbfs);
        chain_->setLimiterCeilingDb(sp.limiterCeilingDbfs);
        chain_->setBypass(sp.bypass != 0);
      }
```

- [ ] **Step 2: Push aggression to the engine**

Aggression lives on the ENGINE (the DFN3 leaf), not the DspChain. `engine_` is the `INoiseFilter`
(the `SwitchableEngine`, `Dfn3StreamAdapter`, `RnnoiseFilter`, or `PassthroughFilter`); the DspChain,
when present, wraps this same `engine_` object. So set aggression directly on `engine_` — it reaches the
DFN3 leaf even under the chain and under warm-standby (SwitchableEngine forwards to both leaves). Add,
right after the `if (chain_) { ... }` block above:
```cpp
      if (engine_) engine_->SetAggression(sp.aggression);   // DFN3-only; RNNoise/passthrough no-op
```
(This is a cheap per-hop scalar store into the engine; RT-safe. At the default `aggression == 0` it's a
no-op, so live behavior is unchanged until the UI sends a non-zero value.)

- [ ] **Step 3: Build + full suite**

`"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug` then
`"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure`
Expected: builds clean; **100/100** (no behavior change at default aggression 0).

- [ ] **Step 4: Commit**
```bash
git add src/voicecleaner-worker/bridge.cpp
git commit -m "feat(worker): relay feeds live aggression to the engine (Params snapshot)"
```

---

## Final verification
- [ ] Full unit suite: **100/100**.
- [ ] Task 1 fidelity r-values recorded (streaming≈whole at A=0/0.5/1.0; output changes with A).
- [ ] `git grep -n "SetAggression" src/denoise-core/dfn3_stream_adapter.*` shows the forward (no stub).

## What Phase 2b delivers / what's next
**Delivered:** the aggression slider is now live on DFN3 (streaming), validated to match the offline
reference; fed from the IPC `Params` snapshot; DFN3-only (RNNoise unchanged). **Manual K688 ear-check**
(does the slider feel right at 0.25/0.5/0.75/1.0) is the user's, ideally against the Phase 3 UI meter.
**Next:** Phase 3 — the single-window + tray UI that drives this whole control plane.
