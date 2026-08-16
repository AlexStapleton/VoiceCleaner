# Product Shell — Phase 3: UI Skeleton — Design

_Date: 2026-08-16 · Status: draft for review_

Parent design: [`2026-08-15-product-shell-design.md`](2026-08-15-product-shell-design.md) (§4 window, §5 IPC,
§10 phase 3). This spec realizes that plan's **Phase 3 (UI skeleton)** and nothing beyond it.

## 1. Problem & scope

The worker (`voicecleaner-worker.exe`) is UI-ready: it serves a named-pipe command channel and publishes a
shared-memory telemetry block. What is missing is the thing you click. This phase builds `voicecleaner.exe`:
**one Direct2D window plus a tray icon that spawns/attaches/supervises the worker and drives engine switch and
bypass over the pipe.** It proves the end-to-end control loop and the crash-isolation guarantee.

**In scope:** the window shell, the tray, the spawn/attach/supervise lifecycle, engine switch, bypass, and one
small worker-side config change to make those controls functional.

**Out of scope (Phase 4+):** the Direct2D level meter, the microphone picker, tuning sliders (aggression/HPF/
gain/limiter), the Advanced section, `settings.toml` per-mic profiles and drift-ppm persistence, and rich
error/reconnect UX (auto-restart with backoff, device-lost recovery). These are named here only to fix the
boundary; they are not built.

## 2. Locked decisions (this phase)

| Decision | Choice |
|---|---|
| Increment | Parent spec's **Phase 3 exactly** — skeleton only. No meter, sliders, mic picker, or persistence. |
| UI toolkit | **Direct2D from the start** — `ID2D1HwndRenderTarget` + `WM_PAINT`/`InvalidateRect`, custom-drawn controls. Not a DXGI swap chain. |
| Launch config | UI reads `settings.toml` next to its module and spawns the worker with **forced flags** `--warm-standby --dsp-chain`. Missing/incomplete config → clear message, no spawn. |
| Params baseline | UI seeds `current_` from `Params::defaults()` (which equals the worker's actual runtime values); reads `activeEngine`/`bypass` back from telemetry. |
| Lifecycle | **Spawn + attach + detect.** Single-instance UI; probe for a live worker → attach or spawn; detect worker death and surface it with a manual **Relaunch**. Auto-restart/backoff deferred to Phase 5. |
| Telemetry bypass | **Add `uint32_t bypass` to the `Telemetry` POD now** so the attach path reflects true bypass state. |

## 3. Why the controls depend on the launch flags

The worker's relay applies `ParamStore::snapshot()` values each hop. Two facts constrain the skeleton:

- **Engine switch** is instant only under **warm standby** — `Bridge::request_engine` is a no-op unless the
  both-engines-warm `switch_` object exists, which is built only when the worker starts with `warm_standby=true`.
- **Bypass** is applied via `chain_->setBypass(...)`; with no DSP chain there is nothing to bypass. The chain
  exists only when the worker starts with `dsp_chain=true`.

Therefore the UI must launch the worker with **both** enabled, or its two controls are inert. `Config::apply_cli`
does not currently accept these overrides (only `--capture/--render/--csv/--ring`), so this phase adds them (§7).

Because `ParamStore` initializes to `Params::defaults()`, the worker's effective runtime values for
aggression/HPF/AGC/limiter/bypass already equal `Params::defaults()` from the first hop. The UI seeding its
`current_` snapshot from `Params::defaults()` is therefore consistent — sending a full `SetParams` never stomps a
divergent config value.

## 4. New target: `src/voicecleaner/`

A `WIN32` (no-console) executable. Links: `vc-ipc` (Params, Command codec), `win-ipc` (`PipeClient`,
`TelemetryReader`), `transport-core` (`Config::load` for pre-flight validation), and the system libs
`d2d1`/`dwrite`/`user32`/`shell32`/`ole32`.

| File | Responsibility |
|---|---|
| `main.cpp` | `WinMain`; single-instance mutex + restore-broadcast; creates the window, tray, supervisor; runs the message pump. |
| `worker_supervisor.{h,cpp}` | Spawn (`CreateProcess`), attach probe, liveness/death detection, `Shutdown`. Wraps `TelemetryReader` + `PipeClient` + the process handle. |
| `app_window.{h,cpp}` | Window class, `ID2D1HwndRenderTarget` lifetime, layout + hit-test + paint, `WM_*` handlers, `WM_TIMER` telemetry poll. |
| `tray.{h,cpp}` | `Shell_NotifyIcon` add/modify/delete, `TrackPopupMenu`, `WM_APP` tray callback. |
| `ui_state.{h,cpp}` | **Pure, Windows-free** state model — the unit-testable core (§8). |

Design intent: the Win32/D2D/tray glue is kept thin; all decision logic lives in `ui_state` so it can be tested
without a display or an audio device.

## 5. State & IPC wiring

The UI owns one `vc::ipc::Params current_`, seeded from `Params::defaults()`. This phase mutates only two fields —
`engine` and `bypass`; the rest ride the defaults that match the worker's runtime. Each control action:

1. mutates `current_` (optimistic local update + `InvalidateRect`), then
2. calls `PipeClient::send(SetParams, current_)` — synchronous connect-send-close, at UI cadence, on the UI thread.

A `WM_TIMER` at ~10 Hz reads `TelemetryReader`:
- `activeEngine` → corrects the engine display (authoritative; the worker may switch on its own under warm standby).
- `bypass` (new field, §6) → corrects the bypass display, notably on **attach**.
- `seq` → liveness (advancing = alive).
- `workerState` → reflects `DeviceLost`/`Stopping` as "Worker not running".

## 6. Telemetry `bypass` field

Add `uint32_t bypass;` to the shared `vc::ipc::Telemetry` POD (`src/vc-ipc/telemetry.h`). The worker's relay writes
`sp.bypass` into it on each ~1 Hz publish (`src/voicecleaner-worker/bridge.cpp`). The UI reads it in the telemetry
poll so that, when it **attaches** to an already-running worker (which it did not spawn and whose bypass state it
otherwise could not know), the toggle shows the true state instead of defaulting to Active. Update the two
`win-ipc` telemetry tests for the widened struct.

## 7. Worker-side change (stays headless-correct)

Extend `Config::apply_cli` (`src/transport-core/config.cpp`) with three override flags:
`--warm-standby`, `--dsp-chain`, and `--engine <name>`. They set `warmStandby`/`dspChain`/`engine` respectively,
overriding the loaded TOML. Valueless flags already parse without desyncing the argv scan. Add `transport-core`
config unit tests for each. No RT-path or telemetry change beyond §6; the worker still runs standalone headless.

## 8. The window (skeleton)

Custom-drawn on the D2D render target; control rects computed from client size, hit-tested in `WM_LBUTTONDOWN`,
repainted on state change.

1. **Status line** — `● Active · <engine> · → CABLE Input`, `○ Bypassed · <engine> · → CABLE Input`, or
   `✕ Worker not running`. Colored status dot.
2. **Engine segmented control** — `[ RNNoise │ DeepFilterNet3 ]`; click sends `SetParams` with the chosen `engine`.
3. **Bypass toggle** — prominent Enabled/Bypass button; click flips `current_.bypass` and sends `SetParams`.
4. **Relaunch worker** button — visible only when the worker is down; click re-runs the spawn path.

**First-run/behavior:** if `settings.toml` is absent or lacks capture/render IDs (checked via `Config::load`), the
window opens showing "No microphone configured — the mic picker arrives in Phase 4" and does not spawn.

## 9. Tray icon

`Shell_NotifyIcon` with a `WM_APP` callback; icon/tooltip reflect active/bypassed + engine.
- **Left-click / double-click** → restore the window.
- **Right-click** → `TrackPopupMenu`, routed through `WM_COMMAND`:
  - `☑ Enabled` (toggles bypass — same path as the window toggle)
  - `Engine ▸` submenu (`RNNoise` / `DeepFilterNet3`)
  - `Open VoiceCleaner`
  - `Quit`

## 10. Lifecycle (spawn + attach + detect)

- **Single-instance UI:** `CreateMutex(Local\voicecleaner-ui)`. On `ERROR_ALREADY_EXISTS`, broadcast a registered
  "restore" window message (`RegisterWindowMessage`) so the running instance shows itself, then exit.
- **Worker probe (attach vs spawn):** `TelemetryReader::open` succeeds **and** `seq` advances across two quick
  reads ⇒ a live worker exists → **attach** (no process handle; supervise via telemetry + pipe-send results).
  Otherwise → **spawn**. Reuses the existing shm — no worker change for the probe itself.
- **Spawn:** resolve `<moduleDir>` via `GetModuleFileName(NULL)`. `CreateProcess` with `CREATE_NO_WINDOW` on
  `<moduleDir>\voicecleaner-worker.exe --config "<moduleDir>\settings.toml" --warm-standby --dsp-chain`. Keep the
  process handle. Bounded-wait (~3 s) for the shm to appear before enabling controls; on timeout, report a launch
  failure.
- **Death detection:** spawned → `WaitForSingleObject(handle, 0) == WAIT_OBJECT_0`; attached → shm gone or `seq`
  stalled ~2 s; a failed `PipeClient::send` corroborates either. On death: status → "Worker not running", show
  **Relaunch**, update the tray. No auto-restart/backoff (Phase 5).
- **Minimize / Close window** → hide to tray; the worker keeps running (audio uninterrupted). Restore from the tray.
- **Quit** (tray or window menu) → send `Shutdown`, bounded-wait for the worker to exit (handle if spawned, else
  shm disappearance), remove the tray icon, exit. Only path that stops the worker.
- **UI crash:** worker survives (separate process); relaunch re-attaches via the probe — never double-spawns.

## 11. Error handling

- **No config / no devices:** detected pre-spawn via `Config::load`; window shows a clear message, does not spawn.
- **Spawn timeout (shm never appears):** report a launch failure; offer Relaunch.
- **Pipe send fails:** treat as a death signal (§10); do not crash the UI.
- **Torn telemetry reads:** tolerated by design (relaxed atomics, `seq`), unchanged from the existing reader.
- **Worker `DeviceLost`:** reflected as "Worker not running" via `workerState`; full recovery UX is Phase 5.

## 12. Testing

**Pure units — new `tests/voicecleaner` gtest target (no Windows, no audio):**
- `ui_state`: given telemetry + events, computes the status string, which controls are enabled, the engine
  display, the bypass display, and alive/dead — including the attach case where `bypass` comes from telemetry.
- Spawn-arg construction: `(moduleDir, configPath)` → the exact worker command line.
- Liveness policy: a sequence of `(shmOpen, seq)` samples → alive/dead decision (including the ~2 s stall).

**Worker unit tests:** `Config::apply_cli` cases for `--warm-standby`, `--dsp-chain`, `--engine <name>`; widened
`Telemetry` round-trips in the two `win-ipc` telemetry tests.

**Regression (must stay green):** all existing tests (100) and the DFN3 offline golden — the worker changes are
config-flag parsing and one telemetry field, neither of which touches DSP output.

**Manual / integration (real hardware):**
- UI launches → worker spawns (no console window) → audio flows to CABLE Input.
- Engine switch (RNNoise ↔ DeepFilterNet3) via the window **and** the tray is instant and clean; status reflects it.
- Bypass toggles cleanly from the window and the tray.
- Minimize/close hides to the tray with audio still flowing; tray left-click restores the window.
- **Isolation:** killing the UI in Task Manager leaves audio flowing; relaunching re-attaches (no second worker)
  and shows the true engine and bypass state.
- Killing the worker shows "Worker not running"; **Relaunch** respawns and reconnects.
- **Quit** shuts down both processes cleanly; the tray icon is removed.
- **Headless-correctness:** the worker still runs and cleans audio with no UI started at all.

## 13. Deliverables

- New `src/voicecleaner/` target (5 source units above) wired into `CMakeLists.txt`.
- New `tests/voicecleaner/` gtest target wired into `CMakeLists.txt`.
- `Telemetry.bypass` field + worker relay write + updated `win-ipc` telemetry tests.
- `Config::apply_cli` three flags + `transport-core` tests.
- No regression in the existing 100 tests or the DFN3 golden.
