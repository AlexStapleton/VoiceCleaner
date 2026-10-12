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

- [ ] **Step 1: Failing tests — measured lag equals reported delay.** In `test_rnnoise.cpp`:
  (a) deterministic: calls 0 and 1 output exact zeros, and a noise burst confined to input frame k
  first produces output energy at call k+2; (b) cross-correlate output vs input over lags 0..2400 for
  an **aperiodic** pitch glide (100 -> 300 Hz harmonic sweep at -20 dBFS; a fixed-pitch probe has equal
  peaks every period and 960 is a multiple of common periods) and assert `argmax == 960`.
  Change the existing `EXPECT_EQ(f.AlgorithmicDelaySamples(), 0u)` (test_rnnoise.cpp:30) to 960.
- [ ] **Step 2: Failing test — STRENGTH alignment.** Test `DryWetMix(f.AlgorithmicDelaySamples())` +
  `RnnoiseFilter` directly (not the full DspChain — its HPF/AGC/gate would mask the effect): glide
  input, strength 0.5; the output/input cross-correlation has a single peak (no second peak 960
  samples from the first). Fails while the reported delay is 0.
- [ ] **Step 3: Implement.** `constexpr uint32_t kRnnoiseDelaySamples = 960;` (comment: input frame k
  leaves at call k+2 — `denoise.c` synthesizes `delayed_X`) returned by `AlgorithmicDelaySamples()`.
  `bridge.cpp` pins: default `{960, 1440}`, LOW LATENCY **`{960, 960}`** (all pins equal => nothing
  padded, reported 960; NOT `{960, 0}`, which would pad DFN3 by 960 and misalign STRENGTH by 1440).
  Add a `test_dynamic_engine` case: equal pins pad nothing and report the pin. Update `latency.h`
  comment and `test_latency` (RNNoise 960 = 20 ms).
- [ ] **Step 4: Run all tests, MinGW syntax-check `bridge.cpp`.**
- [ ] **Step 5: Commit** `fix(rnnoise): report the real 960-sample delay (STRENGTH alignment, -20 ms in default mode)`.

## Task 2: DFN3 automatic input level — **Sonnet 5.5**

Spec Part 1.

**Files:** create `src/denoise-core/input_level_normalizer.{h,cpp}`,
`src/denoise-core/pre_gain_stage.{h,cpp}`, `src/denoise-core/delay_line.h` (pulled forward from Task 5),
modify `src/denoise-core/dfn3_stream_adapter.{h,cpp}`, `src/denoise-core/CMakeLists.txt`; tests
`tests/denoise-core/test_input_level_normalizer.cpp`, `test_pre_gain_stage.cpp`, `test_delay_line.cpp`
(+ `tests/denoise-core/CMakeLists.txt`). The DFN3 adapter itself needs ONNX Runtime and can't be
unit-tested on Linux, so all logic lives in the two pure units.

- [ ] **Step 1: Failing tests for `DelayLine`** (fixed delay N incl. 0; reset zeroes; per-sample push).
- [ ] **Step 2: Failing tests for `InputLevelNormalizer`** (pure; `Config{targetDbfs, maxGainDb=30,
  windowFrames=300, percentile=0.75, silenceDbfs=-80, slewDbPerSec=3, fastSlewDbPerSec=30,
  fastPhaseSec=2}`; `float process(const float* frame480)` returns this frame's target gain in dB):
  - steady input at level L -> gain converges to `T - L` (±0.5 dB); never negative; capped at +30;
  - sparse loud clicks (≤ 20% of frames) on quiet input don't move the gain by more than 1 dB
    (75th-percentile robustness — the failure mode of a peak-hold level);
  - digital silence after speech -> gain holds (silent frames are ignored);
  - slew: ≤ 30 dB/s during the first 2 s, ≤ 3 dB/s after.
- [ ] **Step 3: Failing tests for `PreGainStage`** (wraps a `model(const float* in, float* out)` with a
  known delay D=1440): ramps the input gain per sample from the previous to the new frame gain;
  multiplies the model output by `1/g` taken from a D-sample per-sample gain delay line. With an
  identity model of delay D and a gain that changes every frame, output == input delayed by D to
  float precision.
- [ ] **Step 4: Implement** the three units (no allocation after construction, no per-sample
  transcendental calls — dB<->linear once per frame, linear ramp per sample) and wire `PreGainStage`
  + `InputLevelNormalizer` into `Dfn3StreamAdapter::ProcessFrame` (warm-up hops at unity). Calibrate
  `targetDbfs` so the -45 dBFS-peak harness clip receives about +20 dB.
- [ ] **Step 5: Harness validation (Linux, `scratchpad/dfn3exp`):** rebuild against the modified
  adapter; run the -55/-45/-35/-25/-15 clips; acceptance (spec Part 1): click RMS reduction within
  3 dB of the -25 result at every level, -15 no worse than before. Record the table in
  `docs/superpowers/notes/2026-10-12-dual-engine-eval.md` (create).
- [ ] **Step 6: Commit** `feat(dfn3): automatic input level so DFN3 works on quiet microphones`.

> **Task 2 outcome (2026-10-12):** units built and tested, but the ungated level failed acceptance
> (see spec "Part 1 revision" and the eval note). `Dfn3StreamAdapter` keeps the option, default
> **off**; v1.5.1 ships DFN3 unchanged. The speech-gated level moves into Task 8 (Dual).

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

## Task 5: `INoiseFilter::failedHops` + `DelayLine` reuse — **Sonnet 5.5**

**Files:** `src/denoise-core/inoise_filter.h`, `dfn3_stream_adapter.{h,cpp}`,
`dynamic_engine.{h,cpp}`, `src/voicecleaner-worker/bridge.{h,cpp}`; test `test_dynamic_engine.cpp`.

- [ ] Refactor `DynamicEngine::pushDelay` onto `DelayLine` (from Task 2; existing tests stay green).
- [ ] Add `virtual uint64_t failedHops() const noexcept { return 0; }` to `INoiseFilter` (the DFN3
  adapter's existing `failedHops()` becomes the override — one name, no case-only twin).
  Bridge reclaim + stop summary use `failedHops()` (remove the `static_cast` and the `idx == 1`
  special case; keep the live counter via a `DynamicEngine::failedHops()` that sums resident leaves).
- [ ] Commit `refactor(engine): DynamicEngine on DelayLine; uniform failedHops() counter`.

## Task 6: `DynamicEngine` with three slots — **Sonnet 5.5**

Spec Part 2.

**Files:** `src/denoise-core/dynamic_engine.{h,cpp}`; `tests/denoise-core/test_dynamic_engine.cpp`.

- [ ] Failing tests (extend the `ProbeEngine` fake to 3 probes): Lean `0 -> 2 -> 1` loads/crossfades/
  retires correctly; a fade involving slot 2 is **linear** (constant-1.0 input, equal-gain probes ->
  output stays 1.0 through the fade) while 0<->1 keeps the existing equal-power law; Warm keeps 0 and 1
  resident, loads 2 on demand, **does not run 0/1 while 2 is active and not fading** (process counts),
  retires 2 when leaving it, never retires 0/1; `AlgorithmicDelaySamples() == max` of three pins; equal
  pins pad nothing; `requestActive(3)` clamps to 2; mid-fade reswitch with slot 2.
- [ ] Implement (`kSlots = 3`, arrays, clamp, fade law by slot pair, Warm residency/run rule for slot 2).
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
  - `dfn3Failed` **snaps** the weight to 0 for that same frame (`wStart = wEnd = 0`: the failing hop's
    DFN3 output is silence) and holds 0 for 1 s;
  - otherwise `|wEnd - wStart|` per frame never exceeds the attack slope × 10 ms;
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
    `failedHops()` == DFN3 fake's; a failed-hop increment makes that frame's output == the RNNoise leg;
  - `currentWeight()` (backed by `std::atomic<float>`, read from the telemetry thread) matches the ramp end.
- [ ] Implement (loader `std::function<std::unique_ptr<INoiseFilter>(int which)>` for the two leaves;
  two `DelayLine`s; per-sample linear ramp; scratch buffers sized at construction).
- [ ] Speech-gated DFN3 boost (spec "Part 1 revision"): add `process(frame, bool speech)` to
  `InputLevelNormalizer` (level = median of the last ~3 s of *speech* frames' RMS; no boost until 0.5 s
  of speech; T ≈ -24 dBFS; 0..+30 dB) with failing tests first (click-only input -> 0 dB; speech at
  -46 dBFS -> ~+22 dB; speech at -41.5 -> ~+17.5). `DualEngine` wraps its DFN3 leg in `PreGainStage`
  driven by that normalizer, gated by the RNNoise leg's VAD for the same frame. Re-run the real-recording
  evaluation (`scratchpad/eval_real`) with the in-app logic; acceptance per spec Part 6.3.
- [ ] Commit `feat(engine): DualEngine — RNNoise in gaps, DFN3 during speech`.

---

# Phase D — Wiring

## Task 9: Worker — **Sonnet 5.5**

Spec Part 4.

**Files:** `src/voicecleaner-worker/bridge.{h,cpp}`, `main.cpp`; `src/vc-ipc/telemetry.h`;
`tests/win-ipc/test_telemetry_shm.cpp`; `src/offline-cli/main.cpp`.

- [ ] Telemetry: add `float dualWeight;` after `agcGainDb` (it occupies existing tail padding:
  `static_assert(sizeof(Telemetry) == 104)`); test round trip with `activeEngine = 2`, `dualWeight = 0.75`.
- [ ] Bridge: Dynamic branch for `engineIndex(cfg.engine)` (rnnoise/dfn3/dual); loader `idx 2` builds
  `DualEngine` (RNNoise + `Dfn3StreamAdapter("models/dfn3")`); pins default `{960, 1440, 1440}`,
  LOW LATENCY `{960, 960, 960}`;
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
  shift on switch), as DFN3 does today; STRENGTH in that combination is 480 samples off (was 1440).
- **Review (Sonnet, 2026-10-12) folded in:** LOW LATENCY pins all equal (not `{960, 0}`), linear fade
  for slot-2 transitions, Warm doesn't run 0/1 under Dual, exact per-sample pre-gain undo via a delay
  line in a Linux-testable `PreGainStage`, percentile (not peak-hold) level with a fast start, failure
  snap, `failedHops` naming, atomic weight, telemetry `sizeof` unchanged, aperiodic test probes.
- **Model split:** Sonnet 5.5 for Tasks 1, 2, 5–9, 11 and reviews; Haiku 5.5 for Tasks 4, 10, docs;
  Opus coordinates, reviews each diff, and owns release and user-facing decisions.
