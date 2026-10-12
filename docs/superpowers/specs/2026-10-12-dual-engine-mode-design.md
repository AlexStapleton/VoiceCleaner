# Dual Engine Mode (RNNoise in the gaps, DeepFilterNet3 while speaking) — Design

Date: 2026-10-12
Status: SHELVED 2026-10-12 — user listening test: Dual lets typing through under speech (DFN3 owns speech segments); DFN3 then removed from the product in v1.6. Kept for the record; Part 0 (RNNoise's real 960-sample delay) shipped.

## Problem

VoiceCleaner offers two engines and each wins at something different:

- **RNNoise** nearly erases mouse/keyboard clicks in the gaps between words, is cheap and has low
  latency, but thins the voice a little (91.8% speech-band retention, 2.44 dB LSD on the user's real
  recordings, `docs/archive/dfn3/dfn3-realtest-report.md`).
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

**Fix.** `RnnoiseFilter::AlgorithmicDelaySamples()` returns `960` (named constant: input frame `k`
leaves at call `k+2`; calls 0 and 1 output exact zeros). Bridge pins:

- Default: `{960, 1440}` (and `{960, 1440, 1440}` once Dual exists).
- LOW LATENCY: **every pin = 960** (`{960, 960}` / `{960, 960, 960}`), *not* `{960, 0}`.
  `DynamicEngine` pads each slot by `max(pins) - ownPin`; with all pins equal nothing is padded (the
  same "no padding" behavior LOW LATENCY has today with `{0, 0}`), while the reported delay becomes
  RNNoise's real 960. `{960, 0}` would pad DFN3 by 960 (+20 ms) and misalign STRENGTH by 1440.

Resulting alignment (reported delay is what `DryWetMix` and the latency readout use):

| Mode | Reported | RNNoise real | DFN3 real | Dual real |
|---|---|---|---|---|
| Default | 1440 | 960+480 pad = 1440 ✓ (was 2400) | 1440 ✓ | 1440 ✓ |
| LOW LATENCY | 960 | 960 ✓ (STRENGTH now aligned) | 1440 (STRENGTH 480 off; was 1440 off) | 1440 (same) |

Effects: default-mode RNNoise drops 50 -> 30 ms; LOW LATENCY RNNoise stays 20 ms with STRENGTH aligned;
latency readout correct for RNNoise. DFN3/Dual in LOW LATENCY keep the documented one-time timing shift
on switch.

## Part 1 — DFN3 automatic input level (pre-gain)

**Defect.** DFN3's ERB features are `10*log10(bandPower + 1e-10)` (libDF-identical,
`dfn3_features.cpp:96`). Input much below about -45 dBFS is squashed against that floor; the model then
reports high LSNR ("clean speech") and passes clicks. Measured (synthetic clicks, 2026-10-12):
click reduction -46 dB at -25 dBFS peaks, -20 dB at -45, -10 dB at -55; with +20 dB pre-gain the -45
clip behaves exactly like the -25 clip. The user's mic needs ~+18 dB of AGC, i.e. it sits in the bad
range. (Lowering the epsilon also works but moves features off the training distribution — rejected.)

**Fix.** `Dfn3StreamAdapter` applies a slowly varying gain `g` before the model and exactly `1/g`
after. The output level is unchanged; only the model's view of the input moves into its working range.

- New pure unit `InputLevelNormalizer` (denoise-core): per frame, compute frame RMS in dB and keep the
  last ~3 s (300 frames) of non-silent values (frames below -80 dBFS are ignored, so silence never
  pumps the gain); level `L` = the **75th percentile** of that window. A percentile of RMS is robust to
  sparse loud clicks — a peak-hold envelope would latch onto a mouse click and drop the gain for
  seconds, bringing the failure back. Gain `G_dB = clamp(T - L, 0, +30)` with `T` calibrated in Plan
  Task 2 so the -45 dBFS-peak harness clip gets about +20 dB. Slew ±3 dB/s in steady state (DFN3's
  feature normalization has a ~1 s time constant), but **±30 dB/s for the first 2 s** after
  construction so a freshly loaded engine (Lean switch) reaches its gain in well under a second.
- New pure unit `PreGainStage` (denoise-core, no ONNX Runtime): wraps a per-frame "model" callable;
  ramps the input gain per sample, and undoes it with a **1440-sample per-sample gain delay line**
  (`out[i] *= 1/gDelayed[i]`), which is exact for any linear-in-amplitude delay and needs no
  frame-history interpolation. Unit-testable on Linux with an identity model of 1440 samples delay
  (output must equal the delayed input to float precision).
- Applies in plain DFN3 mode and inside Dual (Part 3 uses the same adapter).
- Acceptance: on the harness clips at -55/-45/-35/-25/-15 dBFS, DFN3 click RMS reduction is within
  3 dB of its -25 dBFS result at every level, and the -15 dBFS result is no worse than today's.

### Part 1 revision (2026-10-12, after implementation and real-recording evaluation)

- The ungated 75th-percentile level failed: on synthetic clips (mostly noise floor) it measured the
  floor, so the estimate was non-monotonic in clip level and overdrove louder clips; on the user's
  recordings it gave typing-only audio +15.6 dB and speech only +0.8..2.9 dB (T = -40) — the opposite
  of what helps. Real-recording data: a +20 dB boost cuts DFN3's voice distortion (LSD 2.14 -> 0.89)
  but *worsens* its click suppression on click-only audio (-42.5 -> -28.9 dB).
- **Decision:** the boost is used **only inside Dual**, and the level is **speech-gated by RNNoise's
  VAD** (only frames with RNNoise VAD >= 0.6 update the level). Click-only audio has almost no such
  frames (9 of 1500 on `typing.wav`), so it gets no boost — which is right, since Dual hands gaps to
  RNNoise anyway. Measured speech-frame RMS medians: -46.3 dBFS (`speech.wav`), -41.5 (`both.wav`);
  the evaluation's best gain was about +20 dB, so the target is **T ≈ -24 dBFS speech RMS** (median of
  the speech-frame window), still clamped to 0..+30 dB.
- No boost until 0.5 s of speech frames has been observed (then the fast-slew phase applies), so a
  fresh engine never boosts on a guess.
- Plain DFN3 mode keeps `autoInputLevel = false` (bit-identical to v1.5). Its own LSNR-based VAD can't
  gate the level reliably at low input (chicken-and-egg).
- `InputLevelNormalizer` gains a `speech` flag per frame (`process(frame, bool speech)`); the ungated
  mode stays for tests only.

## Part 2 — Engine container: three slots + a uniform failure counter

- `INoiseFilter` gains `virtual uint64_t failedHops() const noexcept { return 0; }` (same name the DFN3
  adapter already exposes, now an override). `DualEngine` returns its DFN3 leaf's. The bridge stops
  `static_cast`-ing retired leaves to `Dfn3StreamAdapter` and sums `failedHops()` instead.
- `DynamicEngine` generalizes from 2 to 3 slots (`kSlots = 3`; arrays, `clamp` to 0..2,
  `delaySamples[3]`, loader `idx 0..2`). Semantics:
  - Lean (default): unchanged — only the active slot resident; others load on demand, crossfade, retire.
  - Warm (hidden `warm_standby` setting): slots 0 and 1 resident as today; slot 2 (Dual) is loaded on
    demand and retired when left. **While Dual is the active, non-fading slot, slots 0 and 1 are not
    run** (Dual already runs both engines; running all three would double the CPU). On switching back,
    the resumed leaf starts from stale state, which the crossfade covers.
  - Crossfade law: equal-power for 0<->1 (unchanged), **linear whenever slot 2 is involved** — Dual's
    output shares RNNoise's/DFN3's content, so the signals are correlated and equal-power would bump
    the level up to +3 dB during the 150 ms fade.
  - Pins per Part 0: default `{960, 1440, 1440}`, LOW LATENCY `{960, 960, 960}`.

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
   RNNoise mode). `SetAggression` is forwarded to the DFN3 leaf only. `failedHops()` = DFN3 leaf's.
   The current weight is published through a `std::atomic<float>` (read by the telemetry path).

**`DualBlendController` (pure, header + .cpp in denoise-core, fully unit-tested on Linux):**
- Weight `w` in [0, 1]: 0 = RNNoise, 1 = DFN3. Inputs per frame: `vad` (RNNoise, describes the newest
  input frame) and `dfn3Failed` (DFN3 silenced a hop). Output: `wStart`, `wEnd` for the frame being
  emitted; `DualEngine` ramps linearly between them across the 480 samples (no steps).
- **Look-ahead:** the frame leaving `DualEngine` at call `n` is input frame `n-3` (1440 samples), but
  RNNoise's VAD already describes the newest analysis window `[n-1 | n]` (centered on the n-1/n
  boundary, so ~2.5 frames ≈ 25 ms ahead of the audio leaving). The controller keeps the last
  `lookaheadFrames + 1 = 4` VADs and decides on their max, so DFN3 fades in **before** a word starts
  (no clipped onsets).
- **Hysteresis + hold:** speech opens at `>= 0.6`, closes below `0.3`, and stays open for a
  `hold = 150 ms` after the last open frame (bridges short pauses inside words/sentences).
- **Slew:** attack 0 -> 1 in `20 ms`; release 1 -> 0 in `80 ms` after the hold expires (releasing
  slowly hides the handover at word ends, where RNNoise's gating would otherwise be abrupt).
- **Safety:** if DFN3 silences a hop (`failedHops()` increased), the silenced audio is the frame
  leaving *now*, so the weight **snaps to 0 for that whole frame** (no slew — a slewed release would
  mix the silent hop in and drop out) and stays 0 for 1 s, then resumes normally. A broken DFN3 must
  never mute speech in Dual.
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
  default `{960, 1440, 1440}`, LOW LATENCY `{960, 960, 960}` (Part 0); reclaim sums `failedHops()`;
  DFN3 load failure keeps the current engine (existing behavior).
- Telemetry: `activeEngine` can be 2; append `float dualWeight` (0..1, 0 when not in Dual) after
  `agcGainDb`. It lands in existing tail padding, so `sizeof(Telemetry)` stays 104 — add
  `static_assert(sizeof(Telemetry) == 104)`; an old worker with a new UI reads it as 0.
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
  - `RnnoiseFilter` real lag equals `AlgorithmicDelaySamples()`: deterministic (calls 0–1 output exact
    zeros; a burst in frame k first appears at call k+2) plus cross-correlation of an **aperiodic**
    pitch-glide (a fixed-pitch probe has equal correlation peaks every period, and 960 is a multiple of
    common periods). Update the existing `test_rnnoise.cpp` assertion of `0`.
  - `DryWetMix(rnnoise.AlgorithmicDelaySamples())` + `RnnoiseFilter` directly (not the full DspChain,
    whose HPF/AGC/gate would mask the effect), aperiodic glide input: at strength 0.5 the output's
    cross-correlation with the input has one peak, no second peak 960 samples away.
  - `InputLevelNormalizer`: converges to `T - L`, 75th-percentile robustness (sparse loud clicks don't
    move it), silence ignored, never attenuates, +30 dB clamp, slow and fast slew phases.
  - `PreGainStage` with an identity 1440-delay model: output == delayed input (float precision) while
    the gain changes.
  - `DualBlendController`: VAD 0 -> w 0; sustained VAD 1 -> w 1 within attack; look-ahead opens before
    the delayed onset; hold bridges a short dip; release slope; failure snaps to 0 for the failing
    frame and holds 1 s; per-frame step bound (except the failure snap).
  - `DualEngine` with fake leaves (settable gain/delay/VAD/failures): alignment (both legs line up),
    `AlgorithmicDelaySamples() == 1440`, output == RNNoise leg at w=0 and == DFN3 leg at w=1, linear
    mix in between, VAD/aggression/failedHops forwarding.
  - `DynamicEngine`: 3-slot switching 0<->2, 1<->2 with a linear fade, 0<->1 still equal-power, Warm
    keeps 0/1 resident but idle while 2 is active and loads/retires 2, LOW LATENCY pins pad nothing.
  - `engineIndex/engineKey`, Settings validation of unknown engine strings, `aggressionApplies`,
    `UiState::engineName(2)` and the Dual readout, telemetry round trip with `activeEngine = 2`.
- Windows CI: full build + ctest.
- Manual: Part 6 evaluation and listening test; UI check of the three-button row, tray item, persist.

## Files touched

- `src/denoise-core/`: `rnnoise_filter.{h,cpp}`, `inoise_filter.h`, `dynamic_engine.{h,cpp}`,
  new `delay_line.h`, new `input_level_normalizer.{h,cpp}`, new `pre_gain_stage.{h,cpp}`,
  `dfn3_stream_adapter.{h,cpp}`,
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
