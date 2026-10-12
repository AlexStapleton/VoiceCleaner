# Dual Engine Mode — Implementation Plan

> **For agentic workers:** Implement task-by-task with subagents. Each task names the model to use:
> **Sonnet 5.5** for algorithmic / real-time / cross-cutting work, **Haiku 5.5** for mechanical
> plumbing, renames, UI strings and test updates. The coordinating session (Opus) reviews every task's
> diff before the next starts, and runs a Sonnet review pass at the end of each phase. Steps use
> checkbox (`- [ ]`) syntax. Strict TDD: failing test first, then the change, then green.

**Goal:** Add a third engine, **Dual** (RNNoise output between words, DeepFilterNet3 output while
speaking, crossfaded by look-ahead VAD), after fixing the two defects that block it: RNNoise's
mis-reported 960-sample delay and DFN3's failure on quiet input.

**Spec:** `docs/superpowers/specs/2026-10-12-dual-engine-mode-design.md` (Parts 0–6 referenced below).

**Architecture:** Additive. New pure, Linux-testable units (`InputLevelNormalizer`,
`DualBlendController`, `DelayLine`) and a `DualEngine : INoiseFilter` that composes the existing
RNNoise and DFN3 leaves. `DynamicEngine` grows from 2 to 3 slots so Dual switches live with the same
crossfade as today. Engine names/indices are centralized in transport-core.

**Tech stack / build-test:**
- Linux (subagents): the scratch test build (`scratchpad/tcbuild`: transport-core, vc-ipc,
  voicecleaner-core, denoise-core tests) and the DFN3 harness (`scratchpad/dfn3exp`, ONNX Runtime
  1.28.3 Linux). Windows-only sources: MinGW `x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only`
  (with the `respawn_policy.h` shim copy for UI files).
- Windows CI (`.github/workflows/release.yml`): full MSVC build + ctest on every push to the branch.

**Releases:** Phase A (Tasks 1–2) + the already-committed help-strip fix -> **v1.5.1**.
Phases B–E -> **v1.6** only if Phase E's acceptance criteria pass. (Pending user confirmation.)

---

# Phase A — Prerequisite fixes (v1.5.1)

## Task 1: RNNoise reports its real 960-sample delay — **Sonnet 5.5**

Spec Part 0.

**Files:** modify `src/denoise-core/rnnoise_filter.{h,cpp}`, `src/voicecleaner-worker/bridge.cpp`,
`src/transport-core/latency.h` (comment); tests `tests/denoise-core/test_rnnoise.cpp`,
`tests/denoise-core/test_dsp_chain.cpp`, `tests/denoise-core/test_dynamic_engine.cpp` (pins only if
they reference RNNoise's 0), `tests/transport-core/test_latency.cpp`.

- [ ] **Step 1: Failing test — measured lag equals reported delay.** In `test_rnnoise.cpp`: feed 2 s of
  deterministic white noise at -20 dBFS through `RnnoiseFilter` frame by frame (RNNoise passes
  broadband noise partially; use a speech-like harmonic signal if the output is gated to zero —
  a 200 Hz sawtooth with vibrato works), cross-correlate output vs input over lags 0..2400, assert
  `argmax == filter.AlgorithmicDelaySamples()`. Fails today (960 vs 0).
- [ ] **Step 2: Failing test — STRENGTH 0.5 on RNNoise has no comb notch.** In `test_dsp_chain.cpp`:
  DspChain with `RnnoiseFilter`, `mixStrength = 0.5`, input = the same harmonic signal. A 20 ms
  dry/wet misalignment comb-filters (notches every 50 Hz), cancelling part of the harmonics; assert
  the steady-state output RMS at 0.5 is ≥ 0.9 × the RMS at `mixStrength = 1` and that the
  cross-correlation of the output with the input peaks at a single lag (no second peak 960 away).
- [ ] **Step 3: Implement.** `constexpr uint32_t kRnnoiseDelaySamples = 960;` (comment: one analysis
  hop of look-back + one synthesis hop, `denoise.c` `delayed_X`) returned by
  `AlgorithmicDelaySamples()`. `bridge.cpp`: `dc.delaySamples[0] = kRnnoiseDelaySamples` (ask the
  loaded leaf instead of hard-coding where possible). Update `latency.h` comment and `test_latency`
  expectations (RNNoise 960 = 20 ms).
- [ ] **Step 4: Run all tests, MinGW syntax-check `bridge.cpp`.**
- [ ] **Step 5: Commit** `fix(rnnoise): report the real 960-sample delay (STRENGTH alignment, -20 ms in default mode)`.

## Task 2: DFN3 automatic input level — **Sonnet 5.5**

Spec Part 1.

**Files:** create `src/denoise-core/input_level_normalizer.{h,cpp}`, modify
`src/denoise-core/dfn3_stream_adapter.{h,cpp}`, `src/denoise-core/CMakeLists.txt`; test
`tests/denoise-core/test_input_level_normalizer.cpp` (+ `tests/denoise-core/CMakeLists.txt`).

- [ ] **Step 1: Failing tests for `InputLevelNormalizer`** (pure; `Config{targetDbfs=-25, maxGainDb=30,
  releaseDbPerSec=6, slewDbPerSec=3, silenceDbfs=-80}`; `float process(const float* frame480)`
  returns the linear gain to apply to this frame):
  - steady -45 dBFS peaks -> gain converges to +20 dB (±0.5) and never exceeds the slew rate per frame;
  - -15 dBFS input -> gain stays 0 dB (never attenuates);
  - -60 dBFS -> capped at +30 dB;
  - digital silence after speech -> gain holds (does not ramp to +30);
  - a single loud click in quiet input -> level jumps (instant attack) and gain drops, then recovers
    no faster than the release rate.
- [ ] **Step 2: Implement** the normalizer (peak-hold envelope in dB, target/clamp, slew limit,
  silence hold; no allocation, no `exp/log` per sample — per-frame `log10` of the frame peak is fine).
- [ ] **Step 3: Wire into `Dfn3StreamAdapter::ProcessFrame`:** `g = norm_.process(in)`; scale a
  scratch copy of the input by `g`; run the model; scale the output by `1/g` using the gain that was
  applied to the *same* audio (the model output lags 1440 samples = 3 frames: keep a 3-entry gain
  history and divide by the gain from 3 frames ago; if the gain is slewing, ramp `1/g` per sample
  between consecutive history entries). Warm-up hops run at unity. Add a unit test with a fake
  pass-through "model" path if the adapter allows injection; otherwise verify in Step 4.
- [ ] **Step 4: Harness validation (Linux, `scratchpad/dfn3exp`):** rebuild against the modified
  adapter; run the -55/-45/-35/-25/-15 clips; acceptance (spec Part 1): click RMS reduction within
  3 dB of the -25 result at every level, -15 no worse than before. Record the table in
  `docs/superpowers/notes/2026-10-12-dual-engine-eval.md` (create).
- [ ] **Step 5: Commit** `feat(dfn3): automatic input level so DFN3 works on quiet microphones`.

## Task 3: Release v1.5.1 — **coordinator (Opus)**

- [ ] Docs (`STATUS.md` v1.5.1 entry, README latency numbers if changed), PR, Windows CI green,
  merge, `workflow_dispatch` with `release_tag = v1.5.1`, verify the asset. Ask the user to re-test
  STRENGTH < 100% and DFN3 on their mic.

---

# Phase B — Engine plumbing

## Task 4: Central engine ids — **Haiku 5.5**

**Files:** create `src/transport-core/engine_id.{h,cpp}` (+ CMake), modify `settings.cpp` (validate on
load: unknown -> `"rnnoise"`), `config.h` comment; test `tests/transport-core/test_engine_id.cpp`.

- [ ] Failing tests: `engineIndex("rnnoise")==0`, `("dfn3")==1`, `("dual")==2`, `("bogus")==0`,
  `("none")==0`; `engineKey(0/1/2)`; Settings with `engine = "bogus"` loads as `"rnnoise"`; `"dual"`
  round-trips.
- [ ] Implement (`enum class EngineId : uint32_t { RNNoise = 0, DFN3 = 1, Dual = 2 }`, constexpr
  string table).
- [ ] Commit `feat(settings): central engine ids; validate engine on load`.

## Task 5: `INoiseFilter::FailedHops` + `DelayLine` extraction — **Sonnet 5.5**

**Files:** `src/denoise-core/inoise_filter.h`, `dfn3_stream_adapter.{h,cpp}`, new `delay_line.h`,
`dynamic_engine.{h,cpp}`, `src/voicecleaner-worker/bridge.{h,cpp}`; tests
`tests/denoise-core/test_delay_line.cpp`, `test_dynamic_engine.cpp`.

- [ ] Failing tests for `DelayLine` (fixed delay N incl. 0; reset zeroes; per-sample `push`).
- [ ] Implement `DelayLine`; refactor `DynamicEngine::pushDelay` onto it (existing tests stay green).
- [ ] Add `virtual uint64_t FailedHops() const noexcept { return 0; }`; override in the DFN3 adapter.
  Bridge reclaim + stop summary use `FailedHops()` (remove the `static_cast` and the
  `idx == 1` special case; keep the live counter via a `DynamicEngine::failedHops()` that sums
  resident leaves).
- [ ] Commit `refactor(engine): DelayLine helper and a uniform FailedHops counter`.

## Task 6: `DynamicEngine` with three slots — **Sonnet 5.5**

Spec Part 2.

**Files:** `src/denoise-core/dynamic_engine.{h,cpp}`; `tests/denoise-core/test_dynamic_engine.cpp`.

- [ ] Failing tests (extend the `ProbeEngine` fake to 3 probes): Lean `0 -> 2 -> 1` loads/crossfades/
  retires correctly; Warm keeps 0 and 1 resident, loads 2 on demand, retires 2 when leaving it, never
  retires 0/1; `AlgorithmicDelaySamples() == max` of three pins; `requestActive(3)` clamps to 2;
  mid-fade reswitch with slot 2.
- [ ] Implement (`kSlots = 3`, arrays, clamp, Warm/Lean residency rule for slot 2).
- [ ] Commit `feat(engine): DynamicEngine supports a third slot`.

---

# Phase C — Dual engine

## Task 7: `DualBlendController` — **Sonnet 5.5**

Spec Part 3 (controller).

**Files:** create `src/denoise-core/dual_blend_controller.{h,cpp}`; test
`tests/denoise-core/test_dual_blend_controller.cpp`.

- [ ] Failing tests (frame = 10 ms; `Config{lookaheadFrames=3, open=0.6, close=0.3, holdMs=150,
  attackMs=20, releaseMs=80, failHoldMs=1000}`; API `Ramp next(float vad, bool dfn3Failed)` returning
  `{wStart, wEnd}`):
  - all-zero VAD -> w stays 0;
  - VAD steps to 1 at call n: the emitted weight starts rising at call n (look-ahead covers the
    3-frame delay) and reaches 1 within `attack`, i.e. **before** the delayed onset frame is emitted;
  - a 100 ms dip below `close` inside speech does not release (hold);
  - after speech ends, release starts after `hold` and falls at the release slope;
  - `dfn3Failed` forces 0 within the attack time and holds 0 for 1 s;
  - `|wEnd - wStart|` per frame never exceeds the attack slope × 10 ms;
  - values between close and open keep the current state.
- [ ] Implement (ring of the last 4 VADs, state machine, slew limiter). No allocation.
- [ ] Commit `feat(dsp): look-ahead VAD blend controller for Dual mode`.

## Task 8: `DualEngine` — **Sonnet 5.5**

Spec Part 3 (engine).

**Files:** create `src/denoise-core/dual_engine.{h,cpp}`, CMake; test
`tests/denoise-core/test_dual_engine.cpp`.

- [ ] Failing tests with fake leaves (settable gain, delay, VAD, failed-hop counter; reuse the
  `ProbeEngine` pattern): constant-1.0 input, RNNoise fake gain 0.5 delay 960, DFN3 fake gain 0.25
  delay 1440:
  - `AlgorithmicDelaySamples() == 1440`; legs aligned (first non-zero output sample at 1440 for both);
  - VAD 0 -> output == 0.5 (RNNoise leg); sustained VAD 1 -> output == 0.25 (DFN3 leg); during the
    ramp the output is the linear mix of the two (per sample, no frame step);
  - `LastVadProbability()` == RNNoise fake's VAD; `SetAggression` reaches only the DFN3 fake;
    `FailedHops()` == DFN3 fake's; a failed-hop increment drives the weight to 0;
  - `currentWeight()` matches the ramp end.
- [ ] Implement (loader `std::function<std::unique_ptr<INoiseFilter>(int which)>` for the two leaves;
  two `DelayLine`s; per-sample linear ramp; scratch buffers sized at construction).
- [ ] Commit `feat(engine): DualEngine — RNNoise in gaps, DFN3 during speech`.

---

# Phase D — Wiring

## Task 9: Worker — **Sonnet 5.5**

Spec Part 4.

**Files:** `src/voicecleaner-worker/bridge.{h,cpp}`, `main.cpp`; `src/vc-ipc/telemetry.h`;
`tests/win-ipc/test_telemetry_shm.cpp`; `src/offline-cli/main.cpp`.

- [ ] Telemetry: append `float dualWeight;` (test round trip with `activeEngine = 2`, `dualWeight = 0.75`).
- [ ] Bridge: Dynamic branch for `engineIndex(cfg.engine)` (rnnoise/dfn3/dual); loader `idx 2` builds
  `DualEngine` (RNNoise + `Dfn3StreamAdapter("models/dfn3")`); pins `{960, ll?0:1440, ll?0:1440}`;
  publish `dualWeight` (via `DynamicEngine` -> active leaf, 0 otherwise).
- [ ] `--dev-keys`: `3` -> Dual. `offline-cli --engine dual`.
- [ ] MinGW syntax-check worker + offline-cli; Windows CI green.
- [ ] Commit `feat(worker): Dual engine selectable at runtime`.

## Task 10: UI — **Haiku 5.5**

Spec Part 5.

**Files:** `src/voicecleaner/ui_state.{h,cpp}`, `app_window.{h,cpp}`, `slider_table.{h,cpp}`,
`tray.{h,cpp}`; tests `tests/voicecleaner/test_ui_state.cpp`, `test_slider_table.cpp`.

- [ ] Failing tests: `engineName(2) == "Dual"`; status line in Dual includes `DFN3 82%` for
  `dualWeight = 0.82`; `onSelectEngine(2)` -> `Params.engine == 2` and the pending-engine latch works
  for 2; `aggressionApplies(0/1/2) == false/true/true`.
- [ ] Implement UiState changes + `aggressionApplies` (used by `AppWindow::sliderEnabled`).
- [ ] AppWindow: three-button engine row (136 px each, rects per spec), `Control::EngineDual`,
  `kHelpEngineDual` + help text, `onCommandEngine` / `applyPick` via `engineKey/engineIndex`,
  LOW LATENCY help mentions Dual. `measureHelpHeight` picks up the new text automatically.
- [ ] Tray: `kTrayCmdEngineDual = 105`, third submenu item with check mark.
- [ ] MinGW syntax-check (shim copy), Linux UI-core tests green.
- [ ] Commit `feat(ui): Dual engine button, tray item and blend readout`.

---

# Phase E — Evaluation and release

## Task 11: Evaluation harness in the repo — **Sonnet 5.5**

**Files:** create `tools/eval/` (CMakeLists fetching ONNX Runtime 1.28.3 Linux by URL + SHA256,
`harness.cpp` driving `RnnoiseFilter`, `Dfn3StreamAdapter`, `DualEngine` hop by hop with latency-aligned
output and a per-hop CSV of VAD/LSNR/weight; `make_clips.py`, `metrics.py` (click RMS / p99 / floor,
speech-band retention, LSD, HF transient reduction), `README.md`).

- [ ] Port from `scratchpad/dfn3exp`, switch to the real adapter/engine classes, add `dual`.
- [ ] Run the synthetic suite; record results in the eval note.
- [ ] Commit `tools(eval): Linux engine evaluation harness (RNNoise / DFN3 / Dual)`.

## Task 12: Real-recording evaluation and tuning — **coordinator (Opus) + Sonnet 5.5**

- [ ] Get the user's three recordings (spec Part 6.2) — the user records on Windows with
  `voicecleaner-worker.exe --capture <id> --record <name>.wav --seconds 15`.
- [ ] Run all three engines; check acceptance (spec Part 6.3); tune `DualBlendController::Config`
  and `InputLevelNormalizer::Config` within spec bounds (Sonnet runs sweeps, Opus decides).
- [ ] Record final numbers and chosen constants in the eval note.
- [ ] Commit `tune(dual): blend constants from real-recording evaluation`.

## Task 13: Docs, review, release v1.6 — **Haiku 5.5 (docs) + Sonnet 5.5 (review) + Opus**

- [ ] Haiku: README (engines section), `STATUS.md`, help texts proof-read.
- [ ] Sonnet: full diff review of Phases B–E against the spec (RT-safety: no allocation/locks in
  `ProcessFrame`; delay alignment; telemetry layout).
- [ ] Opus: user listening test (A/B RNNoise vs Dual) — ship only if the user agrees it's better;
  PR, Windows CI, merge, `workflow_dispatch` `release_tag = v1.6`.

---

## Self-Review

- **Spec coverage:** Part 0 -> Task 1; Part 1 -> Task 2; Part 2 -> Tasks 5–6; Part 3 -> Tasks 7–8;
  Part 4 -> Tasks 4, 9; Part 5 -> Task 10; Part 6 -> Tasks 11–12.
- **Risk ordering:** the two bug fixes ship first and independently (useful even if Dual is dropped).
  Dual's go/no-go is gated on real-recording metrics and the user's ears, not on synthetic clips.
- **RT-safety:** every new per-frame path is allocation- and lock-free; DFN3 loads off the RT thread
  through the existing Lean loader.
- **Known limitation kept:** LOW LATENCY relaxes the cross-engine pin for DFN3 and Dual (one-time timing
  shift on switch), as DFN3 does today; STRENGTH alignment in that combination stays approximate.
- **Model split:** Sonnet 5.5 for Tasks 1, 2, 5–9, 11 and reviews; Haiku 5.5 for Tasks 4, 10, docs;
  Opus coordinates, reviews each diff, and owns release and user-facing decisions.
