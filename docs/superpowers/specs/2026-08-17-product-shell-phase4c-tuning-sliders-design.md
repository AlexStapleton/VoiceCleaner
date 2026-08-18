# Product Shell — Phase 4c: Tuning sliders — Design

_Date: 2026-08-17 · Status: draft for review_

Parent design: [`2026-08-15-product-shell-design.md`](2026-08-15-product-shell-design.md) (§4.5 tuning panel).
Forward-notes from [`2026-08-17-product-shell-phase4a-device-picker-design.md`](2026-08-17-product-shell-phase4a-device-picker-design.md) §12
(per-mic tuning fields + debounced write + seeding-on-attach). Motivated by the first real-hardware listening
test (voice too loud from the AGC; DFN3 needs a tunable suppression control).

## 1. Problem & scope

Phase 4a made the app usable (pick a mic → audio flows), but the DSP is fixed: the AGC blasts the voice, and
there is no way to dial DFN3's suppression. The worker **already applies** `aggression`/`hpfCutoffHz`/
`agcTargetDbfs`/`limiterCeilingDbfs` live from the `Params` snapshot every hop
([bridge.cpp](../../../src/voicecleaner-worker/bridge.cpp): `chain_->setHpfCutoffHz/setAgcTargetDb/
setLimiterCeilingDb`, `engine_->SetAggression`) — Phase 2/2b wired the DSP side. So this phase is **UI +
persistence**, plus one small default change to tame the AGC.

**In scope:** four live sliders (output gain, aggression, HPF, limiter), per-mic persistence, seeding-on-attach,
and softening the AGC default. **Out of scope:** an AGC on/off toggle, dry-tap, the Advanced section, latency
profiles, and any change to the live-param plumbing (already done).

## 2. Locked decisions (from brainstorming)

| Decision | Choice |
|---|---|
| Controls | **Four sliders** — Output gain (`agcTargetDbfs`), Aggression (`aggression`, **DFN3-only**), HPF (`hpfCutoffHz`), Limiter ceiling (`limiterCeilingDbfs`). |
| AGC default | Ship **quieter**: target `-15 → -20 dBFS`, and cap makeup `+30 → +18 dB` so quiet input can't be blasted. |
| Aggression default | Stays **0.0** (neutral) — the user slides up to cancel more; avoids over-suppressing voice out of the box. |
| Persistence | Values stored **per-mic** in the profile; written on **drag-end** (not continuously); **seeded on attach** so a UI relaunch never stomps live tuning. |
| Decision logic | A pure, unit-tested **`Slider`** (value↔fraction, clamp, format) + `UiState` tuning setters in `voicecleaner-core`; the Win32 layer does only pixel↔rect math + drag. |

## 3. Slider ranges & defaults

| Slider | Param field | Range | Default | Format |
|---|---|---|---|---|
| Output gain | `agcTargetDbfs` | −30 … −6 dBFS | **−20** | `"-20 dBFS"` |
| Aggression | `aggression` | 0.00 … 1.00 | 0.00 | `"0.50"` |
| HPF cutoff | `hpfCutoffHz` | 40 … 200 Hz | 80 | `"80 Hz"` |
| Limiter ceiling | `limiterCeilingDbfs` | −6 … 0 dB | −1 | `"-1 dB"` |

## 4. Worker change — soften the AGC default (the only DSP edit)

- `src/denoise-core/dsp_chain.cpp:8` — the `Agc` ctor's makeup cap literal `30.0f → 18.0f`.
- `src/denoise-core/dsp_chain.h:17` — `Config::agcTargetDb -15.0f → -20.0f`.
- `src/denoise-core/agc.h` — `Config::maxGainDb 30.0f → 18.0f` (keep the header default consistent with the ctor).
- `src/vc-ipc/params.h` — `Params::defaults().agcTargetDbfs -15.0f → -20.0f`, so the worker's `ParamStore` init,
  the UI's seed, and the slider default all agree.

No other worker change: the relay already pushes every `Params` field into the chain each hop. The DFN3 **offline
golden is engine-only** (`offline-cli --engine dfn3`, no chain), so AGC changes cannot alter it. `denoise-core`
AGC/DspChain unit tests that assert the old `-15`/`+30` values are updated to the new defaults.

## 5. Settings — per-mic tuning fields

`MicProfile` (`src/transport-core/settings.h`) gains four fields, defaulting to the `Params::defaults()` values so
an older profile without them loads sanely:

```cpp
float aggression      = 0.0f;    // [mic."<id>"].aggression
float hpfHz           = 80.0f;   // .hpf_hz
float outputGainDbfs  = -20.0f;  // .output_gain_dbfs   (== agcTargetDbfs)
float limiterDbfs     = -1.0f;   // .limiter_dbfs
```

`Settings::load`/`save` read/write them under each `[mic."<id>"]`. `resolveActive()` is **unchanged** — tuning is
UI-driven live params, not worker-startup `Config`; a headless worker uses the `Params::defaults()` tuning, which
is correct for the headless test.

## 6. Pure core (`voicecleaner-core`)

- **`Slider`** (new `slider.{h,cpp}`): `{min, max, value}`; `float fraction() const` (→ 0..1, clamped),
  `void setFraction(float t)` (value = min + t·(max−min)), `void setValue(float v)` (clamped), and a
  `std::string format(Fmt) const` with `Fmt ∈ {Dbfs, Ratio2, Hz, Db}`. No pixels — the window maps a click x to a
  fraction against the track rect and calls `setFraction`.
- **`UiState`** gains: `seedTuning(float aggression, float hpfHz, float gainDbfs, float limiterDbfs)` (sets
  `current_`'s four fields, for attach), and four setters `onSetAggression`/`onSetHpf`/`onSetOutputGain`/
  `onSetLimiter(float)` that mutate `current_` and return the `Params` to send. `onTelemetry` continues to sync
  **only** `engine`/`bypass` — tuning in `current_` is UI-owned and never clobbered (the same invariant that
  protects engine/bypass, extended to tuning).

## 7. Window & interaction

Four slider rows below the engine/bypass controls (the window grows again; existing controls keep their custom-draw
style). Each row: a caption, a track, a thumb, and the formatted value. Custom-drawn on the D2D target.

- `WM_LBUTTONDOWN` on a track → grab that slider, set its value from the click x, send live.
- `WM_MOUSEMOVE` while a slider is grabbed → update value from x, send `SetParams` live (cheap synchronous pipe).
- `WM_LBUTTONUP` → release the grab and **persist**: write the value into the active `MicProfile` and
  `Settings::save`.
- The **Aggression** row is greyed with a "DFN3 only" note when `telemetry.activeEngine == RNNoise`
  (`engine_->SetAggression` is a no-op on RNNoise); it is interactive again when DFN3 is active.

Slider drag and the device dropdowns are mutually exclusive interaction modes (a dropdown open swallows clicks
first, as today); nothing else changes in the existing hit-testing.

## 8. Persistence & seeding (the 4a trap, handled)

- **On attach/spawn** (first-run pick, startup with devices, relaunch): after seeding, call
  `UiState::seedTuning(...)` from the active `MicProfile`, seed the four `Slider`s to the same values, and — once
  the worker reports **ready** (first telemetry) — send one `SetParams` carrying `current_` so the saved tuning is
  applied. This is the seeding-on-attach guard the 4a spec named: a relaunch shows and re-applies the saved tuning
  instead of stomping it back to defaults.
- **Live edits** send `SetParams` continuously during a drag; the file is written once on drag-end (the "debounced
  write" — device picks and slider drags both persist on their terminal event, never mid-gesture).

## 9. Error handling

- A profile missing the tuning fields (older 4a file) loads them at their defaults — no error.
- Aggression edits while RNNoise is active are prevented at the UI (greyed), so no stale-engine surprise; if a
  `SetParams` still carried aggression, the worker's RNNoise `SetAggression` is a harmless no-op.
- Slider values are clamped to range in the pure `Slider`, so an out-of-range click can never send a bad param.

## 10. Components / files

**New:** `src/voicecleaner/slider.{h,cpp}` (pure `Slider`, added to `voicecleaner-core`);
`tests/voicecleaner/test_slider.cpp`.

**Modified:**
- `src/denoise-core/dsp_chain.cpp`, `dsp_chain.h`, `agc.h`, `src/vc-ipc/params.h` — AGC default softening (§4).
- `src/transport-core/settings.{h,cpp}` — `MicProfile` tuning fields + load/save; `tests/transport-core/test_settings.cpp`.
- `src/voicecleaner/ui_state.{h,cpp}` — `seedTuning` + four setters; `tests/voicecleaner/test_ui_state.cpp`.
- `src/voicecleaner/app_window.{h,cpp}` — four slider rows, drag handling, aggression-greying, seed + persist wiring.
- `tests/denoise-core` — AGC/DspChain tests updated for the new defaults.

## 11. Testing

**Pure units:** `Slider` (fraction↔value, clamp at both ends, format per unit); `UiState` (each setter mutates the
right `current_` field and returns it; `seedTuning` sets all four; a bypass/engine toggle after seeding still
carries the seeded tuning); `Settings` (tuning fields round-trip; an older profile without them loads defaults).
**Worker:** updated `denoise-core` AGC/DspChain tests assert the new `-20`/`+18` defaults; the DFN3 offline golden
stays green (engine-only).
**Manual / hardware:** each slider audibly changes level/suppression/tone; the default output is clearly quieter
than before; Aggression greys out on RNNoise and works on DFN3; tuning persists across a UI relaunch and is
re-applied (not reset) on attach.

## 12. Forward notes (not built here)

- **4d** — live warm-standby toggle + per-mic `drift_ppm` persistence (wires the currently-stubbed `SetWarmStandby`
  and the drift warm-start seed carried by `SelectCaptureDevice`).
- An **AGC on/off** (fixed-gain) toggle and the **Advanced** section remain available to add later if auto-leveling
  itself is unwanted.
