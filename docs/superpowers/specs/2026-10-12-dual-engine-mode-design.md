# Dual Engine Mode (RNNoise in the gaps, DeepFilterNet3 while speaking) — Design

Date: 2026-10-12
Status: Draft for review (design), implementation plan in `docs/superpowers/plans/2026-10-12-dual-engine-mode.md`

## Problem

VoiceCleaner offers two engines and each wins at something different:

- **RNNoise** nearly erases mouse/keyboard clicks in the gaps between words, is cheap and has low
  latency, but thins the voice a little (91.8% speech-band retention, 2.44 dB LSD on the user's real
  recordings, `docs/dfn3/dfn3-realtest-report.md`).
- **DeepFilterNet3 (DFN3)** keeps the voice more natural (95.9%, 1.31 dB LSD) and handles steady noise
  (fans, hum) better, but lets clicks through, adds ~30 ms, costs ~3x the CPU, and — found 2026-10-12 —
  barely works on quiet input (see Part 1).

The user wants a third engine choice, **Dual**, that gets RNNoise's click removal between words and
DFN3's natural voice while speaking.

Two pre-existing defects found during the investigation also block a correct Dual mode and are fixed
first (Parts 0 and 1).

## Goals

1. A third engine button **Dual** (UI, tray, settings `engine = "dual"`), switchable live like the
   other two, persisted per mic.
2. In Dual, both engines process every frame; the output is a smooth, voice-activity-driven crossfade:
   RNNoise output in non-speech gaps, DFN3 output during speech, with no audible switching artifacts.
3. DFN3 works on quiet microphones (benefits plain DFN3 mode too).
4. Correct engine delay accounting, so STRENGTH (wet/dry) is phase-aligned and RNNoise isn't padded
   with 20 ms it doesn't need.
5. A repeatable evaluation (harness + the user's own recordings) with acceptance criteria, so Dual
   ships only if it is measurably and audibly better than RNNoise for this user.

## Non-Goals

- Cascading the engines (RNNoise -> DFN3 or the reverse). Rejected: voice damage compounds, the second
  model sees out-of-distribution input, and latency/CPU add up for little gain.
- Per-frequency mask fusion of the two models (would need both engines' internal masks; much larger).
- Removing clicks that overlap speech. Neither engine does this well; DE-CLICK covers it.
- Changing DFN3's model, its training-matched features, or RNNoise's model.
- Making Dual compatible with LOW LATENCY (Dual needs DFN3's 30 ms look-ahead by definition).

## Relevant current behavior (as-is)

- `DynamicEngine` (`src/denoise-core/dynamic_engine.{h,cpp}`) holds two slots (0 = RNNoise,
  1 = DFN3). Warm mode runs both every frame; Lean (the default, `warm_standby = false`) runs only the
  active one and loads the other on demand for a 15-frame (~150 ms) equal-power crossfade, then
  retires it. Output delay is pinned to `max(delaySamples[0], delaySamples[1])` so switches never shift
  timing; each slot's output is padded by `max - own`.
- `bridge.cpp:84-97` builds it for `engine = "rnnoise" | "dfn3"`; anything else becomes passthrough.
  Pins: `delaySamples = {0, lowLatency ? 0 : 1440}`. `request_engine(idx)` comes from
  `SetParams.engine`; `requestActive` clamps to 0..1, so an index 2 would silently become DFN3.
- `DspChain` reads the engine's VAD after each frame (`LastVadProbability()`): raw VAD drives the
  declicker, a hysteresis (`open 0.6 / close 0.3 / hold 200 ms`) drives AGC and the noise gate.
  `DryWetMix` delays the dry path by the engine's *reported* `AlgorithmicDelaySamples()`.
- RNNoise VAD is its native per-frame speech probability. DFN3's adapter VAD is a provisional logistic
  of the model's LSNR (`dfn3_stream_adapter.cpp:116-125`).
- UI: two-button segmented control (`app_window.cpp`, 208 px each), tray Engine submenu with two items,
  `UiState::engineName` maps `1 -> "DeepFilterNet3"`, everything else -> "RNNoise"; the AGGRESSION slider
  is enabled only when the active engine is 1. Settings store `engine` as an unvalidated string.

## Part 0 — Correct RNNoise's reported delay (prerequisite bug fix)

**Defect.** `RnnoiseFilter::AlgorithmicDelaySamples()` returns 0, but RNNoise outputs input frame
`n-2` at call `n` — a real **960-sample (20 ms)** lag. Verified two ways: in `third_party/rnnoise/src/
denoise.c` (`frame_analysis` windows [previous 480 | current 480]; `rnnoise_process_frame` synthesizes
`delayed_X`, the previous frame's spectrum, with 480-sample overlap-add), and by cross-correlation in the
2026-10-12 harness (960-sample lag). Consequences today:

- STRENGTH < 100% on RNNoise blends dry and wet **20 ms apart** (comb filtering / hollow sound).
- Default mode (LOW LATENCY off): RNNoise is padded by 1440 on top of its own 960 -> **50 ms** engine
  delay instead of the 30 ms the pin intends.
- The latency readout under-reports RNNoise by 20 ms.

**Fix.** `RnnoiseFilter::AlgorithmicDelaySamples()` returns `960` (a named constant derived from
RNNoise's frame size: `2 * FRAME_SIZE - FRAME_SIZE` look-back = one analysis hop + one synthesis hop).
Bridge pins become `{960, lowLatency ? 0 : 1440}` (and `{960, ·, 1440}` once Dual exists, Part 3).
Effects: default mode RNNoise drops 50 -> 30 ms; LOW LATENCY RNNoise stays 20 ms but STRENGTH becomes
aligned; latency readout correct. DFN3 in LOW LATENCY keeps its existing, documented one-time timing
shift.

## Part 1 — DFN3 automatic input level (pre-gain)

**Defect.** DFN3's ERB features are `10*log10(bandPower + 1e-10)` (libDF-identical,
`dfn3_features.cpp:96`). Input much below about -45 dBFS is squashed against that floor; the model then
reports high LSNR ("clean speech") and passes clicks. Measured (synthetic clicks, 2026-10-12):
click reduction -46 dB at -25 dBFS peaks, -20 dB at -45, -10 dB at -55; with +20 dB pre-gain the -45
clip behaves exactly like the -25 clip. The user's mic needs ~+18 dB of AGC, i.e. it sits in the bad
range. (Lowering the epsilon also works but moves features off the training distribution — rejected.)

**Fix.** `Dfn3StreamAdapter` applies a slowly varying gain `g` before the model and `1/g` after. The
output level is unchanged; only the model's view of the input moves into its working range.

- New pure unit `InputLevelNormalizer` (denoise-core): per frame, track a level estimate `L` = a
  peak-hold envelope of frame peak (instant attack, release ~6 dB/s, floored at -90 dBFS), target
  `T = -25 dBFS`, gain `G_dB = clamp(T - L, 0, +30)`, slew-limited to ±3 dB/s (DFN3's feature
  normalization has a ~1 s time constant; slow gain keeps it stable). Holds `G` while `L` is below a
  silence floor (-80 dBFS) so silence doesn't pump the gain to max. Constants in a `Config` struct.
- Applies in plain DFN3 mode and inside Dual (Part 3 uses the same adapter).
- Acceptance: on the harness clips at -55/-45/-35/-25/-15 dBFS, DFN3 click RMS reduction is within
  3 dB of its -25 dBFS result at every level, and the -15 dBFS result is no worse than today's.

## Part 2 — Engine container: three slots + a uniform failure counter

- `INoiseFilter` gains `virtual uint64_t FailedHops() const noexcept { return 0; }`.
  `Dfn3StreamAdapter` overrides it; `DualEngine` returns its DFN3 leaf's. The bridge stops
  `static_cast`-ing retired leaves to `Dfn3StreamAdapter` and sums `FailedHops()` instead.
- `DynamicEngine` generalizes from 2 to 3 slots (`kSlots = 3`; arrays, `clamp` to 0..2,
  `delaySamples[3]`, loader `idx 0..2`). Semantics:
  - Lean (default): unchanged — only the active slot resident; others load on demand, crossfade, retire.
  - Warm: slots 0 and 1 resident and running as today; slot 2 (Dual) is always loaded on demand and
    retired when left (Dual already contains both engines; keeping a third pair warm is waste).
  - Output delay pin = max over all three pins, so with Dual present the default-mode pin stays 1440
    (unchanged), and LOW LATENCY passes `delaySamples[2] = 0` like DFN3 (one-time shift on switch,
    same documented behavior as DFN3 in LOW LATENCY).

## Part 3 — `DualEngine` and its blend controller

New `DualEngine : INoiseFilter` (denoise-core) owning one RNNoise leaf and one DFN3 leaf (constructed
by a loader so tests can inject fakes and the bridge injects real engines).

**Per frame (RT-safe, no allocation, no locks):**
1. Run both leaves on the same 480 input samples.
2. Delay-align: each leaf's output passes through a fixed delay of `maxDelay - ownDelay`
   (RNNoise 960 -> +480, DFN3 1440 -> +0). `AlgorithmicDelaySamples()` = `maxDelay` = 1440. The
   alignment delay line is factored out of `DynamicEngine` into a small shared `DelayLine` helper.
3. Feed RNNoise's VAD for this call to the `DualBlendController`, get the weight ramp for the frame
   leaving now, and mix per sample: `out[i] = (1 - w_i) * rnnoise[i] + w_i * dfn3[i]`.
   **Linear** (not equal-power): during speech the two outputs are highly correlated estimates of the
   same voice, so a linear crossfade keeps the level constant; equal-power would bump it ~+3 dB
   mid-fade.
4. `LastVadProbability()` returns RNNoise's VAD (so DspChain's declicker/AGC/gate behave exactly as in
   RNNoise mode). `SetAggression` is forwarded to the DFN3 leaf only. `FailedHops()` = DFN3 leaf's.

**`DualBlendController` (pure, header + .cpp in denoise-core, fully unit-tested on Linux):**
- Weight `w` in [0, 1]: 0 = RNNoise, 1 = DFN3. Inputs per frame: `vad` (RNNoise, describes the newest
  input frame) and `dfn3Failed` (DFN3 silenced a hop). Output: `wStart`, `wEnd` for the frame being
  emitted; `DualEngine` ramps linearly between them across the 480 samples (no steps).
- **Look-ahead:** the frame leaving `DualEngine` at call `n` is input frame `n-3` (1440 samples), but
  RNNoise's VAD already describes frame `n`. The controller keeps the last `lookaheadFrames + 1 = 4`
  VADs and decides on `max(vad[n-3 .. n])`, so DFN3 fades in **before** a word starts (no clipped
  onsets) — 30 ms of free look-ahead.
- **Hysteresis + hold:** speech opens at `>= 0.6`, closes below `0.3`, and stays open for a
  `hold = 150 ms` after the last open frame (bridges short pauses inside words/sentences).
- **Slew:** attack 0 -> 1 in `20 ms`; release 1 -> 0 in `80 ms` after the hold expires (releasing
  slowly hides the handover at word ends, where RNNoise's gating would otherwise be abrupt).
- **Safety:** if DFN3 silences a hop (`FailedHops()` increased), force the target to 0 and keep it
  there for 1 s — a broken DFN3 must never mute speech in Dual.
- All constants live in `DualBlendController::Config` and are tuned in Part 6 within the bounds
  above.
- Exposes the current `w` for telemetry.

**Cost:** both engines every frame ≈ RNNoise + DFN3 (earlier "both engines warm" smoke: mean 2.07 ms
per 10 ms hop, 0% over budget) — acceptable; Part 6 checks p99 on the user's PC.

## Part 4 — Worker wiring

- Engine names and indices in one place: `transport-core` gets `engineIndex(std::string_view)`
  (`"rnnoise"` -> 0, `"dfn3"` -> 1, `"dual"` -> 2, anything else -> 0) and `engineKey(uint32_t)`. Used by
  Settings validation on load (unknown values become `"rnnoise"`), the bridge, and the UI.
- `bridge.cpp`: the Dynamic branch accepts `"dual"`; `initialActive = engineIndex(cfg.engine)`;
  loader `idx 2` builds a `DualEngine` (RNNoise + `Dfn3StreamAdapter("models/dfn3")`); pins
  `{960, ll ? 0 : 1440, ll ? 0 : 1440}`; reclaim sums `FailedHops()`; DFN3 load failure keeps the
  current engine (existing behavior).
- Telemetry: `activeEngine` can be 2; append `float dualWeight` (0..1, 0 when not in Dual) at the end of
  `Telemetry` (UI and worker ship together; shared-memory layout grows by 4 bytes).
- `--dev-keys`: `3` switches to Dual. `offline-cli --engine dual` (dev tool) for A/B on Windows.
- LOW LATENCY + Dual: allowed but Dual still runs with DFN3's look-ahead (its internal alignment is
  correct); only the cross-engine pin is relaxed, as for DFN3. The LOW LATENCY help text says so.

## Part 5 — UI

- Engine row: three equal buttons, `RNNoise | DFN3 | Dual` (each `(460 - 2*18 - 2*8) / 3 = 136 px`;
  x = 18..154, 162..298, 306..442, y = 232..272). `Control` / `HelpTarget` gain `EngineDual`.
- Status line names: `RNNoise`, `DeepFilterNet3`, `Dual`; in Dual the readout adds the blend, e.g.
  `● Active · Dual · → CABLE Input · VAD ● · AGC +9.0 dB · DFN3 82%`.
- Help text: "ENGINE: Dual — runs both engines: RNNoise between words (removes clicks in the gaps) and
  DeepFilterNet3 while you speak (more natural voice, also removes steady noise like fans). ~30 ms
  latency and about 4x RNNoise's CPU. Clicks *during* speech still need DE-CLICK."
- Tray Engine submenu gets `Dual` (`kTrayCmdEngineDual = 105`).
- AGGRESSION is enabled when the engine is DFN3 **or** Dual (pure helper `aggressionApplies(engine)`,
  unit-tested); its caption stays "DFN3 only" when disabled.
- Persisted per mic as `engine = "dual"`; `applyPick` and `onCommandEngine` use `engineIndex/engineKey`.

## Part 6 — Evaluation, tuning, acceptance

1. **Harness:** move the Linux harness used on 2026-10-12 into the repo as `tools/eval/` (CMake project
   that fetches ONNX Runtime 1.28.3 for Linux, builds RNNoise/DFN3/Dual through the real classes, plus
   the Python clip generator and metrics). Engines: `rnnoise | dfn3 | dual`.
2. **The user's real recordings** (48 kHz, their mic, normal speaking level), made with the existing
   recorder: `voicecleaner-worker.exe --list-devices`, then
   `voicecleaner-worker.exe --capture <mic id> --record typing.wav --seconds 15` (clicking + typing,
   no speech), `speech.wav` (talking), `both.wav` (talking while clicking).
3. **Acceptance — Dual ships only if all hold on the real recordings:**
   - Gaps / clicks (`typing.wav`): click RMS and p99 reduction within **2 dB of RNNoise**.
   - Voice (`speech.wav`): speech-band retention within **1 point of DFN3** and LSD **below RNNoise's**.
   - Mixed (`both.wav`): HF transient reduction no worse than RNNoise.
   - No switching artifacts: per-sample weight change never exceeds the attack slope (unit test), and
     the user hears no pumping or word-edge glitches in an A/B listening test (RNNoise vs Dual).
   - CPU: telemetry `hopP99Ms` < 4 ms on the user's PC in Dual.
4. **Tune** `DualBlendController::Config` (thresholds, hold, slews) and `InputLevelNormalizer::Config`
   (target) against these metrics, within the bounds in Parts 1 and 3.

## Testing

- Unit (Linux + Windows CI):
  - `RnnoiseFilter` real lag equals `AlgorithmicDelaySamples()` (cross-correlation of a noise burst).
  - `DryWetMix` + RNNoise: STRENGTH 0.5 produces no comb notch (aligned).
  - `InputLevelNormalizer`: gain converges to `T - L` within bounds, slew limit, silence hold, unity
    for loud input, 0..+30 dB clamp.
  - `DualBlendController`: VAD 0 -> w 0; sustained VAD 1 -> w 1 within attack; look-ahead opens before
    the delayed onset; hold bridges a short dip; release slope; failure forces 0 for 1 s; per-sample
    step bound.
  - `DualEngine` with fake leaves (settable gain/delay/VAD/failures): alignment (both legs line up),
    `AlgorithmicDelaySamples() == 1440`, output == RNNoise leg at w=0 and == DFN3 leg at w=1, linear
    mix in between, VAD/aggression/FailedHops forwarding.
  - `DynamicEngine`: 3-slot switching 0<->2, 1<->2, Warm keeps 0/1 resident and loads/retires 2, pins.
  - `engineIndex/engineKey`, Settings validation of unknown engine strings, `aggressionApplies`,
    `UiState::engineName(2)` and the Dual readout, telemetry round trip with `activeEngine = 2`.
- Windows CI: full build + ctest.
- Manual: Part 6 evaluation and listening test; UI check of the three-button row, tray item, persist.

## Files touched

- `src/denoise-core/`: `rnnoise_filter.{h,cpp}`, `inoise_filter.h`, `dynamic_engine.{h,cpp}`,
  new `delay_line.h`, new `input_level_normalizer.{h,cpp}`, `dfn3_stream_adapter.{h,cpp}`,
  new `dual_blend_controller.{h,cpp}`, new `dual_engine.{h,cpp}`, `CMakeLists.txt`.
- `src/transport-core/`: new `engine_id.{h,cpp}` (or in `settings`), `settings.cpp`, `config.h`,
  `latency.h` (comment).
- `src/vc-ipc/telemetry.h`.
- `src/voicecleaner-worker/`: `bridge.{h,cpp}`, `main.cpp`.
- `src/voicecleaner/`: `ui_state.{h,cpp}`, `app_window.{h,cpp}`, `slider_table.{h,cpp}`, `tray.{h,cpp}`.
- `src/offline-cli/main.cpp`; new `tools/eval/`.
- Tests under `tests/denoise-core`, `tests/transport-core`, `tests/voicecleaner`, `tests/vc-ipc`,
  `tests/win-ipc`.
- Docs: `README.md`, `docs/STATUS.md`, this spec, the plan, an evaluation note in `docs/superpowers/notes/`.

## Open questions for the user

1. Ship Parts 0 + 1 (RNNoise delay fix, DFN3 quiet-mic fix) together with the help-strip fix as
   **v1.5.1** first, and Dual as **v1.6** after evaluation? (Recommended.)
2. Can you record the three 15-second clips (Part 6.2)? Synthetic clicks are a weak proxy for your mouse.
