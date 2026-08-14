# VoiceCleaner — Project Status & Resume Guide

_Last updated: 2026-08-13_

## TL;DR

VoiceCleaner is a **working real-time microphone noise suppressor** for Windows 11. The live app
(`phase0-app`) captures the FIFINE K688, denoises with **RNNoise + a full DSP chain**, compensates
capture/render clock drift, and renders into VB-Cable "CABLE Input" (which Discord/games select as a
mic). A second, larger engine — **DeepFilterNet3** — has its full foundation built and de-risked but
is **not yet wired end-to-end** (the inference/DSP "middle" remains).

**56 unit tests pass.** Two executables build: `phase0-app` (live suppressor) and `offline-cli`
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

### DeepFilterNet3 — foundation built & de-risked (NOT yet wired)
- **STFT/ISTFT** (`stft.h`): Vorbis window, perfect reconstruction; `third_party/kissfft/` (real FFT).
- **ERB filterbank** (`erb.h`): 32 bands, exact libDF construction.
- **ONNX Runtime 1.28.0** vendored via FetchContent (pinned SHA256).
- **DFN3 models** vendored in `models/dfn3/` (enc/erb_dec/df_dec + config.ini). `src/dfn3-probe/` loads
  all three and runs a dummy inference successfully.
Reference details (params, model I/O shapes, validation): `docs/dfn3-progress.md`.

## What remains (DFN3 "hard middle")

1. **Feature extraction:** `feat_erb` (log-compressed ERB + exponential-mean normalization, `norm_tau=1`)
   and `feat_spec` (first 96 bins complex, normalized), matching libDF bit-closely.
2. **Inference orchestration:** run enc → {erb_dec, df_dec} per frame, carrying recurrent (GRU) state and
   the 2+2-frame lookahead. Exact ONNX I/O shapes are in `docs/dfn3-progress.md`.
3. **Post-inference DSP:** apply the ERB gain mask `m`, apply the deep filter (`coefs`, order 5, 96 DF
   bins), then ISTFT overlap-add.
4. **`DeepFilterFilter : INoiseFilter`** wrapper + wire into `offline-cli`/live relay.
5. **Validation** (blueprint requires sample-for-sample): via golden intermediate vectors from
   DeepFilterNet's `export.py` (`.npz` dumps) and/or the `deep-filter` CLI end-to-end reference.
   **Open decision:** validate-first vs build-then-validate-end-to-end.

## Notes / gotchas
- `rnnoise_data.c` is a 75 MB vendored weights file (~34 MB in git) — could switch to the "little" model
  or fetch at build time if repo size matters.
- Pre-existing benign warnings: `LNK4098 LIBCMT` (from vendored C libs), `C4324` (ring alignment padding),
  `C4996` (fopen). None affect correctness.
- Device-invalidation policy is fail-fast-visibly (Phase 0); auto-reconnect is deferred to Phase 1.
- Not built: DFN3 end-to-end, Phase 1 hardening (latency profiles, device lifecycle, first-run wizard),
  tray UI (Phase 3), regression corpus (§11). The full 8-hour endurance run was declined by the user.
