# VoiceCleaner

Real-time microphone noise suppression for Windows. VoiceCleaner reads your microphone, removes
background noise and keyboard/mouse clicks, levels your voice, and plays the result into
[VB-Cable](https://vb-audio.com/Cable/). In Discord, games or a meeting app, choose
**CABLE Output** as your microphone.

- **Engines:** RNNoise (light, best at clicks; the default) or DeepFilterNet3 (stronger on steady
  background noise, ~30 ms extra latency).
- **Cleanup chain:** strength (wet/dry), high-pass filter, de-click, auto-leveling to a target
  loudness with a max boost, noise gate, and a limiter. Each microphone keeps its own settings.
- **Latency:** about 40 ms buffered in the app at the default LATENCY of 8 (lower it to 4 for ~20 ms), plus Windows' and
  VB-Cable's own buffers.

## Install and update

1. Install [VB-Cable](https://vb-audio.com/Cable/) (one-time; needs a reboot).
2. Download `VoiceCleaner-<version>-win-x64.zip` from
   [Releases](https://github.com/AlexStapleton/VoiceCleaner/releases) and unzip it anywhere.
3. Run `voicecleaner.exe`, choose your **Microphone** and set **Output** to **CABLE Input**.

To update, unzip the new version over the old folder. Settings are kept separately in
`%LOCALAPPDATA%\VoiceCleaner\settings.toml`, so updating never resets them. Versions before 1.5
kept a `settings.toml` next to the exe. 1.5 copies it over once on first run; after that the old
file can be deleted.

The microphone must run at 48 kHz (set it in Windows Sound settings → the device → Advanced).
Closing the window keeps VoiceCleaner running in the tray; use **Quit** to stop it.

## Build

Windows 10/11, Visual Studio 2022 or newer (MSVC, x64), CMake ≥ 3.24. Dependencies (GoogleTest,
toml++, ONNX Runtime) are fetched by CMake.

```
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The app is `build\src\voicecleaner\Release\voicecleaner.exe` (the worker, `onnxruntime.dll` and the
DFN3 models are copied next to it). Add `-DVC_BUILD_DEV_TOOLS=ON` to also build `offline-cli`
(WAV in → engine → WAV out). Pushes and tags build and test on GitHub Actions
(`.github/workflows/release.yml`); a `v*` tag, or a manual run with `release_tag`, publishes the zip.

## Layout

| Path | What |
|---|---|
| `src/voicecleaner` | The UI (Direct2D window + tray). Owns `settings.toml` and supervises the worker. |
| `src/voicecleaner-worker` | The audio process: WASAPI capture → engine + DSP chain → drift-compensated resample → render. |
| `src/denoise-core` | Engines (RNNoise, DFN3 adapter) and the DSP chain. Platform-independent. |
| `src/dfn3` | DeepFilterNet3 streaming runner on ONNX Runtime. |
| `src/transport-core` | Rings, framer, resampler, drift control, settings/config. Platform-independent. |
| `src/vc-ipc`, `src/win-ipc` | UI ↔ worker protocol: commands over a named pipe, telemetry in shared memory. |
| `src/win-audio` | WASAPI, MMCSS, device enumeration. |
| `models/dfn3` | DFN3 weights (the `*_streaming.onnx` split ships; the rest are its export inputs). |
| `tools` | `analyze_drift.py` (for `voicecleaner-worker --csv`); `tools/dfn3` research scripts. |
| `docs` | `STATUS.md` (current state + history), `design/` (the original blueprint), `research/`, `dfn3/`, `superpowers/` (plans, specs and review notes). |

## License

GPL-3.0 — see `LICENSE`. Third-party code in `third_party/` keeps its own license (RNNoise:
BSD-3-Clause, KISS FFT: BSD-3-Clause).
