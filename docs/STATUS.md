# VoiceCleaner — Project Status & Resume Guide

_Last updated: 2026-08-14_

**Active work:** COMPLETE — **live real-time DeepFilterNet3 (STANDARD model), competitive with RNNoise.**
`engine="dfn3"` now runs the standard streaming model (Gates A/B/C all green: streaming==offline
r=0.999999, live mean 0.71 ms / p99.9 1.06 ms / 0% over budget). On real K688 keyboard audio it
suppresses **−30.4 dB** (vs `_ll` −5.6, RNNoise −32.1) and **retains voice better than RNNoise**
(0.891 vs 0.876) — the blueprint D9 premise realized. The `_ll` model was the wrong choice (0-lookahead
can't bracket transients) and is retired from the live path (kept offline). ~30 ms latency vs RNNoise
~10 ms; RNNoise still edges peak-click suppression. **Open product call:** default engine (RNNoise vs
standard DFN3 vs warm-standby both, switch live). Then: product shell (tray UI + §8.5 command channel +
first-run wizard + level meter). Prior stale line below is history.

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
0.9946)** on a synthetic noisy clip — see `docs/dfn3-validation.md`. Run it:
`offline-cli --engine dfn3 <in.wav> <out.wav>`.
- **STFT/ISTFT** (`stft.h`): Vorbis window + libDF `wnorm` analysis normalization, perfect
  reconstruction; `third_party/kissfft/` (real FFT).
- **ERB filterbank** (`erb.h`): 32 bands, exact libDF construction.
- **Feature extraction** (`dfn3_features.h`): `feat_erb`/`feat_spec` with libDF exp-mean normalization.
- **Post-inference DSP** (`dfn3_postdsp.h`): ERB mask broadcast + order-5 deep filter.
- **ONNX Runtime 1.28.0** vendored (FetchContent, pinned SHA256); models in `models/dfn3/`.
- **Orchestrator** `src/dfn3/dfn3_offline.{h,cpp}`: STFT → features → enc/erb_dec/df_dec over full T →
  mask (high bins) + deep filter (noisy low bins) → ISTFT. `src/dfn3-probe/` loads/checks the models.
Reference details (params, model I/O shapes): `docs/dfn3-progress.md`.

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
