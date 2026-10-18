# VoiceCleaner — Project Status & Resume Guide

_Last updated: 2026-10-18_

## Current state (v1.6.1)

Shipping app: `voicecleaner.exe` (UI + tray) supervising `voicecleaner-worker.exe` (audio). RNNoise is
the only engine (DeepFilterNet3 was removed in v1.6). Releases are built, tested and
published by GitHub Actions (`.github/workflows/release.yml`); see the root `README.md` for install,
build and layout.

Recent releases:
- **v1.2** — performance/robustness review fixes (denormal flushing, clean restarts, crash-safe settings
  save, throttled slider sends, pipe message-loss fix). Review: `docs/superpowers/notes/2026-10-10-performance-review.md`.
- **v1.3** — RNNoise built like upstream (int8 weights + SSE4.1/AVX2 dispatch), ~6x faster per frame.
  Notes: `docs/superpowers/notes/2026-10-11-rnnoise-build-performance.md`.
- **v1.4** — MAX BOOST slider (AGC max gain, default +18 dB) and a -16 dBFS target default.
- **v1.5** — settings moved to `%LOCALAPPDATA%\VoiceCleaner` (updates no longer reset them), engine
  choice persisted, status line names the real output, LATENCY clamped to 4..512 everywhere, plus a
  cleanup pass (dead code, retired `models/dfn3-ll`, repo layout, `app_window` refactor). Review:
  `docs/superpowers/notes/2026-10-12-cleanup-review.md`.
  Also ONNX Runtime 1.28.0 → 1.28.3 (patch release: memory-safety fixes; DFN3 only; ONNX Runtime was
  later removed in v1.6).
- **v1.6** — DeepFilterNet3 removed (RNNoise only: engine buttons, AGGRESSION slider and LOW LATENCY
  checkbox gone; zip ~3 MB without ONNX Runtime/models); RNNoise reports its real 960-sample (20 ms)
  delay, so STRENGTH is phase-aligned and engine delay is 20 ms (was 50 ms in the default mode); help
  strip fits its longest text. Dual mode designed, evaluated on the user's recordings and shelved
  (spec/plan/eval note in `docs/superpowers`). The v1.5.1 help-strip fix is folded in here (there was no
  v1.5.1 release).
- **v1.6.1** — performance pass: the limiter's look-ahead peak is now a sliding-window maximum (O(1)
  per sample instead of a 144-sample scan), ~6x faster (60 → 10 µs per frame) with bit-identical output;
  whole chain ~245 → ~203 µs per 10 ms frame. Analysis: `docs/superpowers/notes/2026-10-18-performance-analysis.md`.

Worker developer flags (console run): `--list-devices`, `--config <settings.toml>`, `--capture/--render
<id>`, `--ring <4..512>`, `--csv <path>` (drift CSV for `tools/analyze_drift.py`), `--record <wav>
--seconds N`. The old flat `drift.toml` config format and `phase0-app.exe` referenced in
the history below no longer exist.

## History

**Active work:** COMPLETE — **RNNoise quality + performance pass (DFN3 frozen).** Project pivoted to
RNNoise-path + performance only; DFN3 stays shipped/selectable but untouched. Three workstreams merged to
`master` (198 tests, Debug+Release): **A** — RNNoise **STRENGTH** control (a delay-aligned wet/dry `DryWetMix`
in `DspChain`; `mixStrength`/`mix_strength` default 1.0 = full wet, so DFN3 default is unchanged; 8th slider).
**B** — **low-latency mode** (`[global].low_latency` skips the DFN3 delay pin → RNNoise ~0 ms; in-app **LOW
LATENCY** checkbox) + shipped **`ring_frames` default 16→8** (~40 ms). **C** — `reset()`-able counters + wired
`ResetCounters`, populated `ringAOcc`/`ringBHigh`/`uptimeMs` telemetry, and **supervisor auto-respawn** (pure
`RespawnPolicy`: backoff 0.5→8 s, cap 5, reconnect latch; "Reconnecting… (n/5)"). **Hardware-validated
(2026-08-21):** STRENGTH + latency confirmed good on the K688→VB-Cable rig. **STILL OWED:** by-ear check of
auto-respawn (unplug/replug); optional deferred R&D (spectral declicker, `IAudioClient3`, RNNoise model swap,
8-hr endurance, worker-level in-place reconnect). Plan: `docs/superpowers/plans/2026-08-21-rnnoise-performance-plan.md`;
findings: `docs/superpowers/notes/2026-08-21-rnnoise-performance-findings.md`.

---
_(historical)_ **OUTPUT GAIN diagnosis → TARGET LOUDNESS + noise gate.**
Hardware testing found the "OUTPUT GAIN slider does nothing" + "static when typing" issues. Root cause (by
elimination — DE-CLICK proves live param delivery; a new chain test proves the AGC follows its target under
active VAD): the slider drives the **AGC target**, which only acts while VAD is open and auto-normalizes — so it
felt dead — and with a quiet mic it **rails to max gain**, freezing high gain over the gaps and amplifying the
noise floor / typing residue → static. Fixes shipped: (1) a live **AGC gain + VAD readout** in the status line
(`agcGainDb` telemetry) so auto-leveling is visible; (2) relabel **OUTPUT GAIN → TARGET LOUDNESS** + corrected
help; (3) AGC **max gain 18 → 9 dB** to cap noise over-amplification; (4) a new **`vc::NoiseGate`** (denoise-core,
VAD-keyed downward gate, fast attack / smooth release, depth = `gateStrength`) inserted **AGC → gate → limiter**,
exposed as a live per-mic **NOISE GATE** slider (7th) that silences the non-speech gaps. All `gateStrength` flows
live over the pipe/relay + persists per-mic (`.gate_strength`); default 0 (off). Total tests **173 passing**
(Debug + Release). Branch `fix/output-gain-agc-readout`. **NEXT — hardware A/B:** with a quiet mic, raise NOISE
GATE until the between-keystroke static drops out, and raise the Windows mic level so the AGC stops railing.


**Active work:** COMPLETE (software; hardware-informed) — **transient/click Declicker + voice-floor knob.** A
voice-safe, RT-safe look-ahead **transient ducker** (`vc::Declicker`, denoise-core) is inserted into `DspChain`
**`HPF → declick → AGC`**: HF-emphasis (RC high-pass) → fast/slow envelopes → crest detection → a 3 ms
look-ahead gain duck. Two live, per-mic knobs drive it: **DE-CLICK** (`declickStrength`, overall duck; 0 = off)
and **SPEECH DECLICK** (`declickVoiceFloor`, 0..1 — the fraction of the duck kept *during voice*). Depth =
`strength * ((1 − vad) + vad*voiceFloor)`, so at **voiceFloor 0 (default)** the duck fully backs off under
speech (only clicks in the gaps are removed — fully voice-safe), and higher values also fight clicks that ride
on top of speech, at a clarity cost. Both flow live as `Params.declick*` over the pipe/relay, persist per-mic
(`.declick_strength` / `.declick_voice_floor`), and are two D2D sliders (fifth + sixth) seeded/pushed/persisted
via the Phase 4c machinery. **Default off; no engine change — DFN3 offline golden unaffected** (`src/dfn3`/
`src/offline-cli` untouched; only `declicker.*` + `dsp_chain.*` in denoise-core). Total tests **164 passing**
(Debug + Release). Branch `feat/declicker-voice-floor`.

**Hardware finding (2026-08-18):** a post-engine *broadband* ducker cannot cleanly remove a click that overlaps
speech — killing the click dips the voice with it (that's the clarity loss on DFN3 at high SPEECH DECLICK).
**RNNoise removes clicks at the engine stage, before they mix with voice, so it wins for clicky environments —
that's the intended click-suppression path.** DFN3 (spectral mask) passes transients; the declicker helps only
in the gaps for that engine. **NEXT options:** (a) accept RNNoise as the click engine (done — declicker defaults
voice-safe so it never hurts clarity); (b) if DFN3-with-clicks is worth it, build a **spectral (STFT-domain)
declicker** that separates the click's flat broadband signature from voice harmonics (real R&D; reuses the
DFN3 STFT/ERB infra). See [[realworld-audio-quality]].

_(historical)_ **Phase 4c tuning sliders + AGC softening (hardware-tested, merged 2026-08-17).** `voicecleaner.exe`
has four live tuning sliders — **output gain** (`agcTargetDbfs`), **aggression** (DFN3-only, greyed on RNNoise),
**HPF**, **limiter** — custom-drawn D2D with a draggable thumb; dragging sends `SetParams` live, values are
**seeded per-mic from the profile on attach**, **pushed once the worker is ready**, and **persisted on drag-end**.
The **AGC default was softened** (target `-15 → -20 dBFS`, makeup cap `+30 → +18 dB`). **Hardware finding
(2026-08-17 A/B):** on real clicky input, **RNNoise clearly beats DFN3 at mouse/keyboard suppression** — DFN3
(spectral mask) passes short broadband transients; RNNoise gates them better; aggression deepens the mask, not
transient timing, so it doesn't fix clicks. See [[realworld-audio-quality]] — the motivation for the Declicker above.

_(historical)_ **Phase 4a device picker + settings persistence (merged 2026-08-17).** In-app capture/render
pickers (custom D2D dropdowns; non-48 kHz greyed; device-gone "(not connected)"), `[global]`+`[mic."<id>"]`
`settings.toml` via a shared `Settings` model (`Config::load` resolves it; flat files back-compatible), live
mic/render switching over the pipe, window Quit. New pure units `Settings` + `DevicePicker`.

_(historical)_ **Phase 3 product-shell UI skeleton (merged 2026-08-16).** `voicecleaner.exe` (Direct2D window +
tray) spawns/attaches/supervises the worker and drives engine switch + bypass over the named-pipe/shm IPC;
crash isolation is by design + code-verified. `Telemetry` gained `bypass` + `dspChain`; new `voicecleaner-core`
lib (`UiState`, `LivenessTracker`, `buildWorkerCommandLine`); shipped `settings.toml` uses `ring_frames = 16`.

---
_(historical)_ **COMPLETE — live real-time DeepFilterNet3 (STANDARD model), competitive with RNNoise.**
`engine="dfn3"` now runs the standard streaming model (Gates A/B/C all green: streaming==offline
r=0.999999, live mean 0.71 ms / p99.9 1.06 ms / 0% over budget). On real K688 keyboard audio it
suppresses **−30.4 dB** (vs `_ll` −5.6, RNNoise −32.1) and **retains voice better than RNNoise**
(0.891 vs 0.876) — the blueprint D9 premise realized. The `_ll` model was the wrong choice (0-lookahead
can't bracket transients) and is retired from the live path (kept offline). ~30 ms latency vs RNNoise
~10 ms; RNNoise still edges peak-click suppression.

---
_(historical — superseded by the line above)_ On branch `feat/live-dfn3-streaming`: streaming ONNX surgery + orchestrator done
and validated (Gate A frame-by-frame == whole-utterance 1.2e-5; Gate B streaming C++ vs whole-utterance
`_ll` r=1.000000), RT-hardened (allocation-free hot path), and wired live as `engine="dfn3"`. **Remaining:
Gate C** — a live K688 run to confirm the `_ll` model fits the 10 ms real-time budget. **Gate C PASSED (2026-08-14, live K688):** frame-time
mean=1.56 ms, p99.9=2.76 ms, max=4.19 ms, **0.00% over 10 ms**, zero dropouts, Ring B flat — ~3.6×
tail headroom, so warm-standby (both engines) is viable. **Live DFN3 is complete and validated
end-to-end.** **Phase 4 DONE** (warm-standby RNNoise↔DFN3 crossfade): `SwitchableEngine` runs both
engines warm, equal-power delay-aligned crossfade, live switch via keys 1/2 (`warm_standby=true`,
e.g. `drift-warmstandby.toml`). Smoke (live K688, both engines): mean 2.07 ms, p99.9 4.61 ms, 0% over
budget, zero dropouts, clean switches. (Crossfade *audible* smoothness not yet ear-checked; equal-power
may add a brief ~+3 dB bump — swap to linear if audible.) **Next: the product shell** (tray UI + §8.5
command channel + first-run wizard) — the remaining "make it a real program" work, still unbuilt;
warrants its own design pass. Approach details: spec §11
in `docs/superpowers/specs/2026-08-14-live-dfn3-streaming-design.md`.

## TL;DR

VoiceCleaner is a **working real-time microphone noise suppressor** for Windows 11. The live app
(`phase0-app`) captures the FIFINE K688, denoises with **RNNoise + a full DSP chain**, compensates
capture/render clock drift, and renders into VB-Cable "CABLE Input" (which Discord/games select as a
mic). A second, larger engine — **DeepFilterNet3** — has its full foundation built and de-risked but
is **not yet wired end-to-end** (the inference/DSP "middle" remains).

**55 unit tests pass.** Two executables build: `phase0-app` (live suppressor) and `offline-cli`
(file-based §11 harness), plus `dfn3-probe` (ONNX model loader).

## How to build & run

CMake/CTest are **not on PATH** — use the ESP-IDF-bundled one:
```bash
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Release
```
Run the live suppressor (needs `drift.toml` with your device IDs — get them via `--list-devices`):
```bash
build\src\phase0-app\Release\phase0-app.exe --config drift.toml
```
Enable denoising in the config: `engine = "rnnoise"`, `dsp_chain = true`. Then select **CABLE Output**
as your mic in Discord. `--seconds N` runs a fixed duration; Ctrl+C stops cleanly.
Tests: `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure`.

## What's done

### Phase 0 — transport (certified)
Instrumented pass-through bridge (WASAPI capture → SPSC rings → MMCSS relay → WASAPI render). Measured
**−26 ppm** clock drift on this K688↔VB-Cable pair, zero dropouts. `transport-core` (platform-agnostic:
`SpscRing`, `Framer`, `ChannelExtractor`, `OccupancyStats`, `DropoutCounter`, `Config`, `wav`),
`win-audio` (WASAPI capture/render, MMCSS, VirtualLock, device enumeration), `phase0-app`.
Spec: `docs/superpowers/specs/2026-08-12-phase0-passthrough-bridge-design.md`.

### Phase 1.5 — adaptive drift compensation (validated)
`Resampler` (cubic Hermite) + `DriftController` (PI, leaky-integrator anti-windup) hold Ring B occupancy
flat with zero dropouts (validated 30 min). Config flag `drift_compensation` (default on); telemetry
columns `ratio_ppm`, `filt_occ`. Analysis: `tools/analyze_drift.py`.
Plan: `docs/superpowers/plans/2026-08-13-phase1.5-drift-compensation.md`.

### Phase 2 — engine framework + DSP chain
- `denoise-core`: `INoiseFilter` interface, `PassthroughFilter`, **`RnnoiseFilter`** (working; −38.9 dB
  noise-floor reduction offline; runs live within the 10 ms deadline, zero dropouts).
- Full §7.1 DSP chain: `DcBlocker`, `Biquad` (RBJ HPF), `Agc` (VAD-gated), `Limiter` (look-ahead
  brickwall), `aggression` mapping, and `DspChain` composer (DC→engine→HPF→AGC→limiter).
- `offline-cli`: `--engine rnnoise --chain` — the §11 offline harness.
- Live path wired: `engine`/`dsp_chain` config selects the engine + chain in the RT relay.
Plan: `docs/superpowers/plans/2026-08-13-phase2-denoise-core-offline-cli.md`.

### DeepFilterNet3 — OFFLINE ENGINE COMPLETE & VALIDATED
Full offline pipeline works and **sample-matches the official `deep-filter` reference (Pearson r =
0.9946)** on a synthetic noisy clip — see `docs/archive/dfn3/dfn3-validation.md`. Run it:
`offline-cli --engine dfn3 <in.wav> <out.wav>`.
- **STFT/ISTFT** (`stft.h`): Vorbis window + libDF `wnorm` analysis normalization, perfect
  reconstruction; `third_party/kissfft/` (real FFT).
- **ERB filterbank** (`erb.h`): 32 bands, exact libDF construction.
- **Feature extraction** (`dfn3_features.h`): `feat_erb`/`feat_spec` with libDF exp-mean normalization.
- **Post-inference DSP** (`dfn3_postdsp.h`): ERB mask broadcast + order-5 deep filter.
- **ONNX Runtime 1.28.3** (1.28.0 before v1.5) vendored (FetchContent, pinned SHA256); models in `models/dfn3/`.
- **Orchestrator** `src/dfn3/dfn3_offline.{h,cpp}`: STFT → features → enc/erb_dec/df_dec over full T →
  mask (high bins) + deep filter (noisy low bins) → ISTFT. `src/dfn3-probe/` loads/checks the models.
Reference details (params, model I/O shapes): `docs/archive/dfn3/dfn3-progress.md`.

## What remains (DFN3)

**Live/real-time streaming.** The ONNX export has **no recurrent-state I/O**, so it must run over a
whole sequence at once — fine offline, but frame-by-frame live playback would reset the GRU context
every 10 ms. A live DFN3 needs a state-carrying solution: a state-exposing re-export (Python), or a
bounded sliding-window/chunked approach (latency/compute tradeoff). Until then **RNNoise is the live
engine; DFN3 is offline-only** (great for the dry-tap / archival path, §13).

## Notes / gotchas
- `rnnoise_data.c` is a 75 MB vendored weights file (~34 MB in git) — could switch to the "little" model
  or fetch at build time if repo size matters.
- Pre-existing benign warnings: `LNK4098 LIBCMT` (from vendored C libs), `C4324` (ring alignment padding),
  `C4996` (fopen). None affect correctness.
- Device-invalidation policy is fail-fast-visibly (Phase 0); auto-reconnect is deferred to Phase 1.
- Not built: DFN3 end-to-end, Phase 1 hardening (latency profiles, device lifecycle, first-run wizard),
  tray UI (Phase 3), regression corpus (§11). The full 8-hour endurance run was declined by the user.
