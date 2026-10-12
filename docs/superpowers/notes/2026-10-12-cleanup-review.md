# Cleanup review — 2026-10-12 (after v1.4)

Review of the whole tree at `914a3c8` (v1.4). Three parallel passes (UI app; worker/transport/IPC;
DSP/engines/build/repo) plus spot-verification of the high-value claims. All four batches below were
implemented for v1.5; see "Outcome" at the end for what was done, decided, and deliberately left.

Effort S/M/L, risk low/med/high. ✔ = verified by hand beyond the review pass.

## Two user-visible issues that triggered this review

1. ✔ **Updating wipes settings.** The release zip ships `settings.toml` next to the exe
   (`release.yml` Package step), and the app stores the user's settings in that same file
   (`main.cpp:37` → `moduleDir\settings.toml`; saved from `app_window.cpp` on every change).
   Extracting a new zip over the old folder and choosing *Replace* resets device IDs and every
   `[mic."…"]` profile. `cmake/copy_if_absent.cmake` only protects dev rebuilds. The shipped file is
   redundant: its values equal the code defaults and `Settings::load` already tolerates a missing file
   (the first device pick writes it).
   - Fix A (S): stop shipping `settings.toml` in the zip.
   - Fix B (M): store settings in `%APPDATA%\VoiceCleaner\settings.toml` (one-time migrate from the exe
     folder). Survives any update method and allows installing under Program Files.
2. ✔ **"No microphone configured — the mic picker arrives in Phase 4".** `ui_state.cpp:95`, the
   `NoConfig` status (also the tray tooltip). Shown whenever the mic *or* output isn't chosen — which
   is exactly what happens after issue 1 resets settings. The picker has existed since Phase 4; the text
   is stale. Fix (S): "Choose a microphone and an output below to start" + update the test.

## Bugs found along the way

- ✔ **Engine choice is not persisted.** `onCommandEngine` (`app_window.cpp:592`) never writes
  `MicProfile::engine` and `persistTuning` doesn't save it, yet `applyPick` reloads `p->engine` on a mic
  switch (`:621`). A DFN3 pick is lost on restart / mic switch. Low impact for RNNoise-only use (the
  default), but a real bug. S, low.
- ✔ **Status line always says "→ CABLE Input"** — hard-coded in `main.cpp:43`; `setRenderLabel` is never
  called with the picked output. S, low.
- ✔ **Worker always writes `drift.csv`** into the install folder (`config.h:9` default `csvPath`,
  `bridge.cpp:105` unconditional `logger_.start()`) plus a 1 Hz console line nobody sees. Phase-0
  diagnostics left on in the product. Fix: log only when a CSV path is given explicitly. S, low.
- **Negative `ring_frames` in settings.toml → ~4 GB allocation.** `settings.cpp:36` casts
  `int64 → size_t` without a sign check (the old flat parser guards it). Clamp. S, low.
- **`reselectRender` doesn't reset the drift controller**; the UI always sends drift seed 0, so the seed
  plumbing is effectively dead. S–M, low.

## Cleanup

### Packaging / repo size
- ✔ `models/dfn3-ll` (75 MB) is the retired low-latency DFN3 set; the live engine loads `models/dfn3`.
  Only offline-cli `--engine dfn3-ll|dfn3-stream` uses it. Delete (and those CLI modes). M, low.
- Release zip ships unused whole-utterance DFN3 models (`enc/erb_dec/df_dec.onnx`, ~8.6 MB); the
  streaming engine loads only `*_streaming.onnx` + `config.ini`. Copy only those. S, low.
- `dfn3-probe` (one-off ORT probe) has no remaining purpose — delete. Put `offline-cli` behind a
  `VC_BUILD_DEV_TOOLS` option. S, low.
- `third_party/rnnoise` vendors unbuilt files (autotools, examples, training scripts). Prune, keeping
  COPYING/AUTHORS. S, low.

### Dead / duplicated code
- Old flat config schema: two parsers (`config.cpp:42-59`, `settings.cpp:61-75`) with *different*
  defaults; only reached by the root `drift-*.toml` dev files, which contain one machine's device GUIDs
  and stale comments. Remove the flat path from `Settings::load`; move/delete the drift tomls. M, low-med.
- `Dfn3Offline` duplicates `Dfn3Streaming` (widen, OwnedTensor, NamedSession, constants, gating).
  DFN3 is frozen and streaming is validated equal to offline → delete `Dfn3Offline` (~336 LOC). M, low.
- Unused API: `DspChain::current{Mix,DeclickStrength,DeclickVoiceFloor,GateStrength}`,
  `Dfn3Streaming::lastHopFailed`, `WorkerSupervisor::workerDown`, `Resampler::primed_/ratio()`,
  `Wasapi*::channels()`, `Config::logLevel`, empty `dropout.cpp`/`occupancy.cpp`, unneeded `<avrt.h>`
  includes. S, low.
- Duplicated helpers: `toW`/`widen` ×5, `RegisterWindowMessageW(L"VoiceCleanerRestore")` ×2,
  `shutdown(3000)` ×3, Quit path ×2, respawn sequence ×3. S, low.
- Dead IPC surface: `CmdType::SetWarmStandby`, `Params::dspChain/driftComp/dryTap`, most `WorkerState`
  values. Removing changes the wire format (UI + worker ship together; bump magic). M, med.

### Readability
- `app_window`: slider ids 0–8 re-encoded in 5 parallel switches; replace with one `SliderId` table
  driving draw / hit-test / apply / persist / seed. `paint()` ~160 lines → split; brushes created every
  frame → create once per render target. M, med (UI path is not unit-tested; needs a manual smoke test).
- Layout magic numbers, stale member initialisers (`secY_`, `contentH_`). S, low.
- Stale comments: phase/task/blueprint/"design C3"/"F5"/"see MEMORY" tags; `dsp_chain.h` documents the
  wrong stage order; `telemetry.h` says fields are "not yet populated" that are; `params.h` says
  aggression is "wired in a later phase"; worker prints "Automatic reconnect is Phase 1";
  4-slider-era comments in `app_window.h`. S, low.
- Repo layout: no root README; blueprint/reports/research_notes at root → `docs/`; 7 DFN3 Python
  harnesses → `tools/dfn3/` (rename the two engine-agnostic scorers); `docs/STATUS.md` references a
  `phase0-app.exe` that no longer exists. S, low.

### Not worth doing now
- Telemetry fields the UI never reads (keep if a diagnostics view is wanted).
- Delay-loading `onnxruntime.dll` for RNNoise-only users (feature-sized).
- Tests: all live; no obsolete test files.

## Proposed batches
1. **User-facing fixes** — settings no longer wiped on update, NoConfig text, engine persisted, real
   output name in status, no `drift.csv`, ring_frames clamp, render-switch drift reset.
2. **Slim the release & repo** — drop `dfn3-ll`, ship only streaming models, delete `dfn3-probe`,
   dev-tools option, prune vendored rnnoise extras.
3. **Dead code & stale comments** — unused API, duplicated helpers, flat schema + drift tomls,
   `Dfn3Offline`, comment refresh, docs/README layout.
4. **app_window refactor** — SliderId table, split paint/handle, brush caching (higher risk; separate PR).

## Outcome (v1.5)

Decisions (user, 2026-10-12): keep the LATENCY minimum at 4 (1–3 can't hold a reserve frame and
click); do all four batches; settings move to a per-user folder.

Implemented:
- Batch 1 — settings in `%LOCALAPPDATA%\VoiceCleaner\settings.toml` (Local, not Roaming: device IDs
  are per-PC), copied once from the exe folder; UTF-8 paths end to end (UI, `Settings::save` via a
  `u8string` path, worker re-reads `--config` from the UTF-16 command line); NoConfig text; engine
  persisted per mic; real output name in the status line; drift CSV only with `--csv`;
  `clampRingFrames` (4..512) shared by settings load, worker CLI and the LATENCY box; render switch
  resets drift tracking; old flat config format removed (`Config::load` rejects it).
- Batch 2 — `models/dfn3-ll`, `Dfn3Offline`, `dfn3-probe`, shipped `settings.toml`,
  `copy_if_absent.cmake` and unbuilt rnnoise files removed; only DFN3 runtime models copied/zipped;
  `VC_BUILD_DEV_TOOLS` (default OFF) gates offline-cli; DFN3 scripts in `tools/dfn3/`.
- Batch 3 — unused API removed; IPC `SetWarmStandby` + Params `dspChain/driftComp/dryTap` removed,
  command magic VCC1 → VCC2; shared `win-audio/utf8.h`; stale comments fixed; drift tomls deleted;
  README, docs reorganized (`docs/design`, `docs/research`, `docs/dfn3`), STATUS current-state section.
- Batch 4 — `slider_table` + tests; AppWindow paint/input split, cached brushes, named layout
  constants, Control/HelpTarget enums, `quit()`/`startWorker()`.

Deliberately left:
- `driftSeedPpm` stays in the SelectCaptureDevice payload (UI sends 0 = reset); per-device drift
  persistence could use it later, and removing it would rewrite the pipe tests for no user gain.
- Telemetry diagnostics the UI doesn't read; `WorkerState` values other than Running.
- `UiState::onSet*` setters stay (the slider table points at them; the table test checks each).
- Archiving old `docs/superpowers` plans/specs (churn without benefit); trimming the STATUS history.
- Rewriting git history to drop `dfn3-ll` from the pack (would break every clone).

Verification: Linux unit tests (transport-core, vc-ipc, voicecleaner-core, denoise-core) and
MinGW `-fsyntax-only` on the Windows-only sources; full MSVC build + ctest on GitHub Actions.
Needs a by-hand check on Windows: window layout/drag/hover unchanged after the refactor, and the
first-run settings migration from an old install folder.
