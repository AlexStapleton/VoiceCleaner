# Product Shell — Design

_Date: 2026-08-15 · Status: draft for review_

## 1. Problem

VoiceCleaner is a working real-time mic noise suppressor, but it is still driven by a hand-edited
TOML plus console keypresses (`phase0-app --config drift.toml`, keys `1`/`2` to switch engines). This
design defines the **product shell** — the "make it a real program" layer: a single window where a
user picks their microphone, selects the engine, and adjusts settings, backed by a crash-isolated
background audio process. It realizes blueprint §8 (UI architecture), §8.5 (command channel), and the
§4.2 gain-staging meter, adapted to what is actually built today.

## 2. Locked decisions (from brainstorming)

| Decision | Choice | Rationale |
|---|---|---|
| Audience | Personal now, keep the door open | Build for the author; don't make choices that block a future public release. |
| Process model | **Two processes** (UI + audio worker) | User priority: a UI crash/hang/kill must never stop audio. Only separate processes guarantee this. |
| UI surface | **One window + a tray icon** | Full config lives in one window (mic input, engine, settings, meter). The window minimizes/closes **to the tray**; the tray icon gives quick controls (enable/bypass, switch engine) and restore/quit. No separate settings dialog, no separate wizard window. |
| UI toolkit | **Raw Win32 + Direct2D** | Zero vendored UI deps, smallest binary, native look, best "looks like normal software" story for anti-cheat hygiene (§3). Tray via `Shell_NotifyIcon`, menu via `TrackPopupMenu`. |
| Tuning depth | **Full tuning panel** | Live aggression / HPF / output-gain sliders — requires exposing `DspChain` params through the §8.3 snapshot. |
| First-run | **Deferred stub** | First launch opens the window with the mic picker highlighted and the meter live. The full §4.1/§4.3 checklist is deferred. |
| Packaging | **Portable** | All files (both exes, `onnxruntime.dll`, `models/`, `settings.toml`) live in one self-contained folder; paths resolved relative to the module directory. |

## 3. Architecture

Two executables in one portable folder:

- **`voicecleaner-worker.exe`** — the audio process. Today's `phase0-app` evolved. Runs **standalone
  and headless** (config-driven, Ctrl+C to stop), no UI required — this headless-correctness is the
  design's acceptance test (blueprint §8.1). Adds a non-RT command thread and a telemetry writer; the
  RT hot path (capture → DSP → drift-comp → render) is unchanged.
- **`voicecleaner.exe`** — the UI process. One message-pump thread owning a single config window (the
  mic picker, engine selector, enable/bypass, Direct2D level meter, tuning sliders, an Advanced
  section) **plus a tray icon** for quick controls, the pipe client, and the worker supervisor.

Because they are separate processes sharing no address space, a UI crash, hang, or kill cannot touch
the worker's real-time threads.

```
  ┌─────────────────────────────┐    named pipe (commands →)    ┌──────────────────────────────┐
  │  voicecleaner.exe (UI)      │ ───────────────────────────▶ │  voicecleaner-worker.exe      │
  │  1 thread, Win32 pump       │                               │  (audio, headless-capable)    │
  │  window · D2D meter         │ ◀─────────────────────────── │  capture│RT DSP│render│2nd eng │
  │  supervisor · pipe client   │   shared memory (◀ telemetry) │  cmd(non-RT)│telemetry writer  │
  └─────────────────────────────┘                               │  in-proc Params double-buffer │
                                                                 └──────────────────────────────┘
  🎤 K688 → worker: DC → engine(RNNoise/DFN3) → HPF → AGC → limiter → drift-comp resampler → CABLE Input → Discord
```

## 4. The single window (UI)

Headline controls, top to bottom:

1. **Status + Enable/Bypass** — `● Active · <engine> · → CABLE Input`, and a prominent Enabled/Bypass
   toggle. Bypass for v1 = route the engine to passthrough (delay-matched bypass is deferred; see §11).
2. **Input level meter** — Direct2D, input RMS with peak-hold and decay, target zone −18…−12 dBFS
   marked (§4.2 gain staging). Updated from the worker's telemetry at ~20 Hz.
3. **Microphone input** — dropdown of capture devices (WASAPI enumeration already exists in
   `win-audio` `DeviceEnumerator`). Selecting one is a **structural** change (pipeline rebuild).
4. **Engine** — segmented `RNNoise | DeepFilterNet3` + a `Warm standby` checkbox. With warm standby on
   (both engines loaded), switching is instant (a param update); with it off, switching rebuilds.
5. **Tuning** — three headline sliders: Aggression, HPF cutoff, Output gain (param updates, ramped in
   the RT thread).
6. **Advanced** (collapsible) — output device (defaults to CABLE Input), limiter ceiling, DSP-chain
   toggle, drift-compensation toggle, dry-tap toggle (deferred behavior; see §11), and the telemetry
   readout (frame p50/p99/max, ring occupancy, dropouts, uptime).

**Tray icon (quick controls):** a `Shell_NotifyIcon` icon whose tooltip/icon reflects state
(active/bypassed, current engine). Left-click restores the window; right-click opens a `TrackPopupMenu`:
- `☑ Enabled` (toggle enable/bypass — a param update over the pipe)
- `Engine ▸` submenu (`RNNoise` / `DeepFilterNet3` — instant under warm standby)
- `Open VoiceCleaner` (restore the window)
- `Quit` (shut down the worker and exit both processes)

**First-run:** if `settings.toml` has no capture device, open with the mic picker highlighted and the
meter live; otherwise open normally. No separate wizard window.

**Lifecycle:**
- **Launch:** the UI resolves the worker exe + `settings.toml` next to its own module; if no worker is
  already running (single-instance via a named mutex), it spawns the worker detached with
  `--config settings.toml --ui-pipe <name>`, then connects the pipe. If a worker is already running,
  it attaches instead of spawning a duplicate.
- **Minimize or Close window:** the window **hides to the tray**; the worker keeps running and audio
  keeps flowing. Reopen from the tray (left-click or `Open VoiceCleaner`).
- **Quit (tray menu, or an explicit window menu item):** send the worker a `shutdown` command; both
  processes exit cleanly. This is the only path that stops the worker.
- **UI crash:** the worker keeps running (audio survives the session); relaunching the UI re-attaches
  to the still-running worker via the single-instance/pipe check.

## 5. IPC — two directions, two mechanisms (blueprint §8.3–8.5)

### 5.1 UI → worker: named pipe
A local named pipe (`\\.\pipe\voicecleaner`) carries small, low-rate messages of two kinds, handled
on a **dedicated non-RT thread** in the worker. The RT thread never touches the pipe.

- **Param updates** — aggression, HPF cutoff, output gain, limiter ceiling, active engine (under warm
  standby), bypass, dsp_chain, drift-compensation, dry-tap. The command thread writes these into the
  in-process **double-buffered `Params` snapshot** (§8.3) via `PublishParams` (write inactive slot,
  atomic release-store the index). The RT thread does one acquire-load per frame, wait-free, and
  **ramps** any changed continuous value over ~10–20 ms so parameter jumps never click.
- **Structural commands** — select capture/render device, toggle warm-standby build, reset counters,
  shut down. These stop → rebuild → restart the pipeline on the command thread (never mutate a live
  RT pipeline).

Message schema is small and explicit (a tagged command with a fixed payload); JSON-lines or a compact
binary framing — chosen at implementation time, isolated behind the `vc-ipc` codec.

### 5.2 Worker → UI: shared-memory telemetry (§8.4)
A shared-memory block written only by the worker, read by the UI at ~20 Hz (`WM_TIMER`). Relaxed
atomic stores; the UI tolerates a torn read of a counter. Fields:
- Input RMS, peak, VAD state — **updated every DSP callback** (single relaxed float each, ~free), so
  the meter is responsive.
- Frame-time p50/p99/max, Ring A/B occupancy (current + high-water), dropout count, active engine,
  drift ratio, uptime — aggregated and stored **~1 Hz**.
- A `device_lost` / `worker_state` flag so the UI can react to a fail-fast stop.

This resolves the one tension with §8.4 ("~1 Hz"): the cheap meter fields update per-frame; the
expensive aggregates update per-second. No queue, no strings, no logging on the RT thread.

## 6. Components / code changes

- **`vc-ipc`** (new small static lib, shared by both processes so struct layouts agree): the `Params`
  POD (§8.3), the telemetry struct (§8.4), and the pipe command codec. POD-only, fixed size, no
  pointers/strings in the shared structs.
- **`denoise-core`** — give `DspChain` a `Params`-driven path: the currently-hardcoded aggression /
  HPF / AGC / limiter constants become **ramped targets** with RT-safe setters. This is the "full
  tuning panel" plumbing and the only change to the DSP libraries. Must not regress the DFN3 goldens.
- **worker** (`phase0-app` → renamed target `voicecleaner-worker`): add the named-pipe server + command
  thread, the shared-memory telemetry writer, the in-process `Params` double-buffer + `PublishParams`,
  and a device re-select / pipeline-rebuild path. The interim stdin `1`/`2` key reader is replaced by
  pipe commands (kept only as a dev-only fallback behind a flag).
- **`voicecleaner`** (new UI target): the single Win32 window, Direct2D meter, controls, device
  enumeration/selection (reuse `win-audio` `DeviceEnumerator`), the `Shell_NotifyIcon` tray icon +
  `TrackPopupMenu` quick controls (enable/bypass, engine switch, open, quit), minimize/close-to-tray,
  `settings.toml` read/write, and the spawn/attach/supervise logic.

**Naming to confirm on review:** rename the `phase0-app` target to `voicecleaner-worker`; introduce
the `vc-ipc` lib rather than folding the structs into `transport-core`.

## 7. Settings persistence (portable)

- `settings.toml` lives **next to the executables**, resolved from `GetModuleFileName` → directory.
  Models (`models/dfn3/`) and `onnxruntime.dll` resolve the same way. Nothing under `%APPDATA%`, no
  registry, no installer.
- The UI is the owner of record: it reads `settings.toml` at startup, spawns the worker pointed at it,
  and on any change updates memory + sends the pipe command + **debounced-writes** the file back.
- **Caveat:** a portable app that writes next to itself must not be installed under `Program Files`
  (not user-writable without UAC virtualization). It lives in a user-owned folder — which also
  satisfies the §3 "nothing runs elevated" rule. If the settings write fails (read-only location), the
  UI surfaces a clear warning and keeps running with in-memory settings.
- `settings.toml` supersedes the repo's example TOMLs (`drift.toml`, `drift-warmstandby.toml`), which
  remain as seed/reference configs. Device IDs stay machine-specific and gitignored.

## 8. Error handling

- **Device lost / invalidated:** the worker keeps its Phase-0 fail-fast policy (clean stop, non-zero
  exit) but now also sets the `device_lost` telemetry flag before exiting so the UI can show a clear
  message and offer re-pick / relaunch. (Auto-reconnect remains deferred to Phase 1.)
- **Pipe broken (UI gone):** the worker keeps running headless (audio uninterrupted). On UI relaunch,
  the single-instance check re-attaches to the running worker.
- **Worker exits unexpectedly:** the UI detects pipe disconnect + process exit, surfaces it, and offers
  to relaunch the worker (respawn with backoff; no silent auto-restart loop).
- **UI crash:** worker unaffected (separate process); relaunch re-attaches.
- **Torn telemetry reads:** tolerated by design (relaxed atomics, UI written to accept a stale/torn
  counter).
- **Parameter clicks:** prevented by RT-thread ramping; the UI only publishes targets.
- **Startup races:** UI waits (bounded) for the worker's pipe server to come up before sending
  commands; if it times out, it reports a launch failure.

## 9. Testing

**Unit (host, no audio device — same style as the 55 existing tests):**
- `Params` double-buffer: `PublishParams` / acquire-load never returns a half-written struct; last
  publish wins.
- `DspChain` param path: setters clamp to valid ranges; ramping reaches the target monotonically with
  no discontinuity between frames; a param change mid-stream produces a click-free envelope.
- `settings.toml` round-trip: load → mutate → save → load is identity; missing/partial files fall back
  to defaults.
- `vc-ipc` codec: every command serializes and parses back to the same value; malformed frames are
  rejected, not crashed.

**Regression (must stay green):** the offline DFN3 golden (`offline-cli --engine dfn3`, Pearson
r ≈ 0.9946) and the streaming Gates — the `denoise-core` param plumbing must not alter DSP output when
params equal today's hardcoded defaults.

**Manual / integration (real hardware):**
- UI launches → worker spawns → audio flows to CABLE Input.
- Live knob changes (aggression / HPF / gain) are audibly click-free.
- Engine switch under warm standby is instant and clean; without warm standby, rebuild works.
- Mic-input change rebuilds the pipeline without crashing.
- Bypass toggles cleanly (from both the window and the tray menu).
- Meter tracks real input level and supports gain staging to −18…−12 dBFS.
- **Tray:** minimize/close hides to tray with audio still flowing; tray enable/bypass and engine
  switch work; left-click restores the window; the tray icon/tooltip reflects current state.
- **Headless-correctness:** worker runs and cleans audio with no UI started at all.
- **Isolation:** killing the UI in Task Manager leaves audio flowing; relaunching re-attaches.
- **Quit** (tray menu) shuts down both processes cleanly.

## 10. Phased build order

1. **Shared structs + DSP plumbing** — `vc-ipc` (`Params`, telemetry, codec) + `DspChain` ramped
   param path + unit tests. Drive via config/CLI to validate ramping; no UI yet. Keeps DFN3 goldens
   green.
2. **Worker IPC** — named-pipe server + command thread (replaces stdin keys) + shared-memory telemetry
   writer + device-rebuild path.
3. **UI skeleton** — single window that spawns/attaches/supervises the worker and drives engine switch
   + bypass over the pipe. Proves the end-to-end loop.
4. **UI settings surface** — Direct2D meter, mic picker, tuning sliders, Advanced section, and
   `settings.toml` persistence.
5. **First-run stub + polish** — highlight the mic picker with live meter on first run; error/reconnect
   flows; window state.

Each phase is independently testable and leaves the worker headless-correct.

## 11. Out of scope / deferred

- Full §4.1/§4.3 first-run checklist window (VB-Cable install, 48k endpoints, Discord/Windows settings,
  deep links). Scaffolded conceptually; built when/if this ships to others.
- Richer tray features beyond the quick controls (per-parameter tray sliders, notifications feed,
  jump lists). v1 tray = enable/bypass, engine switch, open, quit.
- Delay-matched bypass and the dry/wet A/B tap as an audible archival feature (§13); v1 bypass is a
  simple passthrough route, dry-tap is a toggle placeholder.
- Latency profiles (Minimum/Low/Balanced/Safe) and an "Auto" engine mode — not built in the engine
  layer yet.
- Installer / code signing / auto-update.
- VST3 or any second front-end (blueprint explicitly not planned).

## 12. Assumptions

- The portable folder is user-writable (not `Program Files`).
- Warm standby (both engines loaded) is affordable as the default, per the measured live headroom
  (DFN3 mean 0.71 ms, p99.9 1.06 ms; both-engines smoke mean 2.07 ms, 0% over budget).
- Output device defaults to CABLE Input; the user routes Discord to CABLE Output (unchanged from today).
- No global hotkeys / input hooks anywhere in the shell (§3, §8.6).
