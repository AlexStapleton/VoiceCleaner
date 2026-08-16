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
| Params baseline | UI seeds `current_` from `Params::defaults()` (which equals the worker's actual runtime values); continuously syncs `current_.engine`/`current_.bypass` from telemetry (§5). |
| Lifecycle | **Spawn + attach + detect.** Single-instance UI; probe for a live worker → attach or spawn; **readiness gated on the first telemetry `seq` advance** (§10); detect worker death and surface it with a manual **Relaunch**. Auto-restart/backoff deferred to Phase 5. |
| Telemetry additions | **Add `uint32_t bypass` + capability flags (`warmStandby`, `dspChain`) to the `Telemetry` POD now** so the attach path reflects true bypass state and can disable controls the worker cannot honor. |

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

**Critical: `current_.engine` must stay synced to `telemetry.activeEngine` at all times** (except the instant the
user clicks the engine control). The worker's pipe handler calls `request_engine(c.params.engine)` on *every*
`SetParams`, not just engine changes ([main.cpp](../../../src/voicecleaner-worker/main.cpp) `SetParams` case). If
`current_.engine` ever diverged from the live engine, a **bypass-only** toggle would carry a stale `engine` and
silently switch engines as a side effect. Keeping them equal makes that embedded `request_engine` a no-op (switch
to the already-active engine). Likewise `current_.bypass` is reconciled from `telemetry.bypass` on attach.

A `WM_TIMER` at ~10 Hz reads `TelemetryReader` and, each tick:
- `activeEngine` → sets both the engine display **and** `current_.engine` (authoritative; see above).
- `bypass` (new field, §6) → sets the bypass display and `current_.bypass`, notably on **attach**.
- `warmStandby`/`dspChain` (new capability flags, §6) → enable/disable the engine and bypass controls; a disabled
  control shows why (e.g. "engine switch unavailable — worker not in warm standby").
- `seq` → liveness/readiness (see §10; advancing = alive, first advance = ready).
- `workerState` → reflects `DeviceLost`/`Stopping` as "Worker not running".

## 6. Telemetry additions (`bypass` + capability flags)

Widen the shared `vc::ipc::Telemetry` POD (`src/vc-ipc/telemetry.h`) with three `uint32_t` fields:

- `bypass` — current bypass state. The relay writes `sp.bypass` on each ~1 Hz publish
  (`src/voicecleaner-worker/bridge.cpp`). On **attach** to an already-running worker (whose bypass state the UI did
  not set and otherwise could not know), this lets the toggle show the true state instead of defaulting to Active.
- `warmStandby`, `dspChain` — **capability flags**: whether the worker was built with the switchable engine and the
  DSP chain. The relay writes `cfg_.warmStandby ? 1 : 0` and `chain_ != nullptr ? 1 : 0`. On attach, the UI uses
  these to **disable the controls the worker cannot honor** — engine switch requires `warmStandby`, bypass requires
  `dspChain` (§3). Without them, a worker started headless (no flags) would present live-looking but inert controls.

Because the UI's own spawn always passes both flags, these read as `1/1` in the normal path; they matter only when
attaching to a differently-launched worker. Update the two `win-ipc` telemetry tests for the widened struct.

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
   Disabled (greyed, with a reason) when the attached worker reports `warmStandby == 0` (§6).
3. **Bypass toggle** — prominent Enabled/Bypass button; click flips `current_.bypass` and sends `SetParams`.
   Disabled when the attached worker reports `dspChain == 0` (§6).
4. **Relaunch worker** button — visible only when the worker is down; click re-runs the spawn path.

**First-run/behavior:** if `settings.toml` is absent or lacks capture/render IDs (checked via `Config::load`), the
window opens showing "No microphone configured — the mic picker arrives in Phase 4" and does not spawn. This is
distinct from a **launch failure** (§11), where the config was valid but the worker never became ready.

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
- **Present vs running (telemetry cadence matters):** the shared-memory block is created partway through
  `Bridge::start()` — *after* both warm-standby models load, and *before* the pipe server comes up and the relay
  begins publishing. `seq` then advances only at the relay's **~1 Hz** publish cadence. So the two signals are
  distinct: **shm opens = worker "present"**; **first `seq` advance = worker "running/ready"**, and by then
  `start()` has returned, so the **pipe server is guaranteed up**. The UI enables controls and declares spawn
  success on the **first `seq` advance**, never merely on shm existence — this also avoids sending a command into a
  pipe server that isn't listening yet.
- **Worker probe (attach vs spawn):** `TelemetryReader::open` succeeds ⇒ a worker is present → **attach**, then wait
  for a `seq` advance to confirm it is running before enabling controls (no process handle; supervise via telemetry
  + pipe results). `open` fails ⇒ **spawn**. Reuses the existing shm — no worker change for the probe itself.
- **Spawn:** resolve `<moduleDir>` via `GetModuleFileName(NULL)`. `CreateProcess` with `CREATE_NO_WINDOW` on
  `<moduleDir>\voicecleaner-worker.exe --config "<moduleDir>\settings.toml" --warm-standby --dsp-chain`. Keep the
  process handle. Bounded-wait **~10 s** for the first `seq` advance (dual-model warm-standby load is the slow
  path); on timeout, report a launch failure and offer Relaunch.
- **Death detection:** spawned → `WaitForSingleObject(handle, 0) == WAIT_OBJECT_0` (immediate, authoritative);
  attached → shm gone, or `seq` stalled across **~3 publish intervals (~3 s at 1 Hz)**, or `workerState` ≠ `Running`.
  A failed `PipeClient::send` only **corroborates** — never the sole trigger, since it also fails transiently during
  a not-yet-ready pipe server. On death: status → "Worker not running", show **Relaunch**, update the tray. No
  auto-restart/backoff (Phase 5).
- **Minimize / Close window** → hide to tray; the worker keeps running (audio uninterrupted). Restore from the tray.
- **Quit** (tray or window menu) → send `Shutdown`, bounded-wait for the worker to exit (handle if spawned, else
  shm disappearance), remove the tray icon, exit. Only path that stops the worker.
- **UI crash:** worker survives (separate process); relaunch re-attaches via the probe — never double-spawns.

## 11. Error handling

- **No config / no devices:** detected pre-spawn via `Config::load`; window shows a clear message, does not spawn.
- **Launch failure (no `seq` advance within ~10 s):** the worker was spawned but never became ready — most likely a
  device that no longer resolves, or missing models. Distinct from "no config"; surfaces its own message + Relaunch.
- **Pipe send fails:** **not** treated as death on its own (it also fails transiently before the worker's pipe
  server is up). Only a corroborating signal (§10); do not crash the UI.
- **Torn telemetry reads:** tolerated by design (relaxed atomics, `seq`), unchanged from the existing reader.
- **Worker `DeviceLost`:** reflected as "Worker not running" via `workerState`; full recovery UX is Phase 5.
- **Bypass discontinuity (cosmetic):** while bypassed, `DspChain` copies input→output and does not tick the engine,
  so DFN3's streaming state is momentarily stale on un-bypass. A brief audible transient, acceptable this phase.

## 12. Testing

**Pure units — new `tests/voicecleaner` gtest target (no Windows, no audio):**
- `ui_state`: given telemetry + events, computes the status string, control **enabled/disabled** state (from the
  `warmStandby`/`dspChain` capability flags), the engine display, the bypass display, and alive/dead — including the
  attach case where `bypass` and the capability flags come from telemetry.
- **`current_.engine` sync invariant:** a bypass-only toggle emits a `SetParams` whose `engine` equals the last
  `telemetry.activeEngine` (never a stale value) — the guard against gap-#1's spurious engine switch.
- Spawn-arg construction: `(moduleDir, configPath)` → the exact worker command line.
- Liveness/readiness policy: a sequence of `(shmOpen, seq)` samples → present / ready (first `seq` advance) /
  dead (`seq` stalled ~3 intervals or shm gone) decisions.

**Worker unit tests:** `Config::apply_cli` cases for `--warm-standby`, `--dsp-chain`, `--engine <name>`; widened
`Telemetry` (bypass + capability flags) round-trips in the two `win-ipc` telemetry tests.

**Regression (must stay green):** all existing tests (100) and the DFN3 offline golden — the worker changes are
config-flag parsing and three telemetry fields, none of which touches DSP output.

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
- `Telemetry` widened with `bypass` + `warmStandby` + `dspChain`; worker relay writes them; updated `win-ipc`
  telemetry tests.
- `Config::apply_cli` three flags + `transport-core` tests.
- No regression in the existing 100 tests or the DFN3 golden.

## 14. Forward note for Phase 4 (not built here)

When Phase 4 adds the tuning sliders (aggression/HPF/gain/limiter), the attach path must **not** keep seeding those
fields from `Params::defaults()` — that would stomp a running worker's live tuning on every UI relaunch. Phase 4
should carry those values in telemetry (or read them from `settings.toml`, its owner-of-record) and seed `current_`
from the worker's actual state, exactly as this phase already does for `engine` and `bypass`. Harmless in Phase 3
(no slider exists to have moved them off the defaults), but a correctness trap the moment sliders land.
