# Product Shell — Phase 3: UI Skeleton — Design

_Date: 2026-08-16 · Status: draft for review_

Parent design: [`2026-08-15-product-shell-design.md`](2026-08-15-product-shell-design.md) (§4 window, §5 IPC,
§10 phase 3). This spec realizes that plan's **Phase 3 (UI skeleton)** and nothing beyond it.

**Depends on Phase 2c** ([`2026-08-16-product-shell-phase2c-lean-engine-swap-design.md`](2026-08-16-product-shell-phase2c-lean-engine-swap-design.md)):
the on-demand lean engine swap. With 2c landed, **engine switching works in both warm and lean modes** (gap-free),
so the engine control is always available and needs no capability gating — only bypass does (it requires the DSP
chain). Build 2c first; this phase assumes it.

## 1. Problem & scope

The worker (`voicecleaner-worker.exe`) is UI-ready: it serves a named-pipe command channel and publishes a
shared-memory telemetry block. What is missing is the thing you click. This phase builds `voicecleaner.exe`:
**one Direct2D window plus a tray icon that spawns/attaches/supervises the worker and drives engine switch and
bypass over the pipe.** It proves the end-to-end control loop and the crash-isolation guarantee.

**In scope:** the window shell, the tray, the spawn/attach/supervise lifecycle, engine switch, bypass, and one
small worker-side telemetry addition (`bypass` + `dspChain` capability flag) that lets the UI reflect true bypass
state and disable the bypass control when the worker has no DSP chain.

**Out of scope (Phase 4+):** the Direct2D level meter, the microphone picker, tuning sliders (aggression/HPF/
gain/limiter), the Advanced section, `settings.toml` per-mic profiles and drift-ppm persistence, and rich
error/reconnect UX (auto-restart with backoff, device-lost recovery). These are named here only to fix the
boundary; they are not built.

## 2. Locked decisions (this phase)

| Decision | Choice |
|---|---|
| Increment | Parent spec's **Phase 3 exactly** — skeleton only. No meter, sliders, mic picker, or persistence. |
| UI toolkit | **Direct2D from the start** — `ID2D1HwndRenderTarget` + `WM_PAINT`/`InvalidateRect`, custom-drawn controls. Not a DXGI swap chain. |
| Launch config | UI reads `settings.toml` next to its module and spawns the worker **against it, with no forced overrides** — the worker honors the file's `warm_standby`/`dsp_chain`/`engine`. Missing/incomplete config → clear message, no spawn. |
| Warm standby | **Respected from `settings.toml`, never forced.** Thanks to Phase 2c, engine switch works either way (warm = instant, lean = gap-free on-demand load), so the engine control is **always enabled**. Warm standby is a resource/latency policy, not a gate. The live toggle checkbox + persistence are **Phase 4** (§14). |
| Params baseline | UI seeds `current_` from `Params::defaults()` (which equals the worker's actual runtime values); continuously syncs `current_.engine`/`current_.bypass` from telemetry (§5). |
| Lifecycle | **Spawn + attach + detect.** Single-instance UI; probe for a live worker → attach or spawn; **readiness gated on the first telemetry `seq` advance** (§10); detect worker death and surface it with a manual **Relaunch**. Auto-restart/backoff deferred to Phase 5. |
| Telemetry additions | **Add `uint32_t bypass` + a `dspChain` capability flag to the `Telemetry` POD** so the UI reflects true bypass state and disables the bypass control when the worker has no DSP chain. (Engine switch is never gated — Phase 2c makes it always available.) |

## 3. Control availability

- **Engine switch — always enabled.** Phase 2c makes `request_engine` work in both modes: warm standby switches
  instantly (both leaves resident), lean loads the target on demand (~16-39 ms, off the RT thread) and crossfades it
  in gap-free. So the UI never disables the engine control for warm-standby reasons; warm standby is a resource
  policy the user sets in `settings.toml`, not a gate. (The ~40 ms lean load is imperceptible; the UI need not even
  show a "loading" state.)
- **Bypass — gated by the DSP chain.** Bypass is applied via `chain_->setBypass(...)`; with no chain there is nothing
  to bypass. The chain exists only when `dsp_chain = true`, reported as the `dspChain` telemetry flag (§6); the UI
  disables the bypass control with its reason when the worker has no chain.

Phase 3 spawns the worker against `settings.toml` with **no forced overrides** — it honors the file's
`warm_standby`/`dsp_chain`/`engine`.

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
- `dspChain` (new capability flag, §6) → enables/disables the **bypass** control; a disabled control shows why
  ("no DSP chain — bypass unavailable"). The engine control is never gated (§3).
- `seq` → liveness/readiness (see §10; advancing = alive, first advance = ready).
- `workerState` → reflects `DeviceLost`/`Stopping` as "Worker not running".

## 6. Telemetry additions (`bypass` + `dspChain`)

Widen the shared `vc::ipc::Telemetry` POD (`src/vc-ipc/telemetry.h`) with two `uint32_t` fields:

- `bypass` — current bypass state. The relay writes `sp.bypass` on each ~1 Hz publish
  (`src/voicecleaner-worker/bridge.cpp`). On **attach** to an already-running worker (whose bypass state the UI did
  not set and otherwise could not know), this lets the toggle show the true state instead of defaulting to Active.
- `dspChain` — capability flag: whether the worker has a DSP chain. The relay writes `chain_ != nullptr ? 1 : 0`.
  The UI disables the bypass control when it is 0 (a `dsp_chain = false` worker can't bypass). Engine switch needs no
  such flag — Phase 2c makes switching always available.

Both fields are read every tick (normal path and attach). Update the two `win-ipc` telemetry tests for the widened
struct. (A `warmStandby` flag is deliberately **not** added here — nothing in Phase 3 gates on it; if Phase 4's
warm-standby checkbox wants a lean/warm readout, it can add one then.)

## 7. Worker-side change (stays headless-correct)

**No new CLI flags, and no engine-switch work** — Phase 2c already delivered lean/warm switching, and the worker
reads `warm_standby`/`dsp_chain`/`engine` from `settings.toml` ([config.cpp](../../../src/transport-core/config.cpp)),
which Phase 3 respects as-is. The **only** worker change this phase is the two telemetry writes in §6
(`bypass` + `dspChain`). The worker still runs standalone headless; no RT-path change.

## 8. The window (skeleton)

Custom-drawn on the D2D render target; control rects computed from client size, hit-tested in `WM_LBUTTONDOWN`,
repainted on state change.

1. **Status line** — `● Active · <engine> · → CABLE Input`, `○ Bypassed · <engine> · → CABLE Input`, or
   `✕ Worker not running`. Colored status dot.
2. **Engine segmented control** — `[ RNNoise │ DeepFilterNet3 ]`; click sends `SetParams` with the chosen `engine`.
   Always enabled (Phase 2c makes switching work warm or lean); reflects `telemetry.activeEngine`.
3. **Bypass toggle** — prominent Enabled/Bypass button; click flips `current_.bypass` and sends `SetParams`.
   Disabled (greyed, with a reason) when the worker reports `dspChain == 0` (§6).
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
  `Bridge::start()` — *after* engine construction and device init, and *before* the pipe server comes up and the
  relay begins publishing. `seq` then advances only at the relay's **~1 Hz** publish cadence. So the two signals are
  distinct: **shm opens = worker "present"**; **first `seq` advance = worker "running/ready"**, and by then
  `start()` has returned, so the **pipe server is guaranteed up**. The UI enables controls and declares spawn
  success on the **first `seq` advance**, never merely on shm existence — this also avoids sending a command into a
  pipe server that isn't listening yet.
- **Worker probe (attach vs spawn):** `TelemetryReader::open` succeeds ⇒ a worker is present → **attach**, then wait
  for a `seq` advance to confirm it is running before enabling controls (no process handle; supervise via telemetry
  + pipe results). `open` fails ⇒ **spawn**. Reuses the existing shm — no worker change for the probe itself.
- **Spawn:** resolve `<moduleDir>` via `GetModuleFileName(NULL)`. `CreateProcess` with `CREATE_NO_WINDOW` on
  `<moduleDir>\voicecleaner-worker.exe --config "<moduleDir>\settings.toml"` (no forced flags — the worker honors
  the file). **REQUIRED: the `settings.toml` this ships/writes MUST set a small real-time `ring_frames` (~16), NOT
  the repo's `drift*.toml` default of 1024.** A 1024-frame ring primes ~512 frames of render buffer = **~5 s of
  latency** out of the box (each frame is 10 ms); a ~16-frame ring is ~15 ms. Verified during Phase 2c hardware
  validation — see [`docs/superpowers/notes/2026-08-16-phase2c-hardware-validation.md`](../notes/2026-08-16-phase2c-hardware-validation.md).
  (`ring_frames` = `Config::ringFrames`, CLI `--ring`; if TOML load doesn't yet parse `ring_frames`, wire it.) Keep the process handle. Bounded-wait **~5 s** for the first `seq` advance — startup is dominated by
  device/COM init and the first ~1 s telemetry window, **not** model load (measured ~16-39 ms even for DFN3's four
  graphs); 5 s is generous headroom. On timeout, report a launch failure and offer Relaunch.
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
- **Launch failure (no `seq` advance within ~5 s):** the worker was spawned but never became ready — most likely a
  device that no longer resolves, or missing models. Distinct from "no config"; surfaces its own message + Relaunch.
- **Pipe send fails:** **not** treated as death on its own (it also fails transiently before the worker's pipe
  server is up). Only a corroborating signal (§10); do not crash the UI.
- **Torn telemetry reads:** tolerated by design (relaxed atomics, `seq`), unchanged from the existing reader.
- **Worker `DeviceLost`:** reflected as "Worker not running" via `workerState`; full recovery UX is Phase 5.
- **Bypass discontinuity (cosmetic):** while bypassed, `DspChain` copies input→output and does not tick the engine,
  so DFN3's streaming state is momentarily stale on un-bypass. A brief audible transient, acceptable this phase.

## 12. Testing

**Pure units — new `tests/voicecleaner` gtest target (no Windows, no audio):**
- `ui_state`: given telemetry + events, computes the status string, the **bypass** control's enabled/disabled state
  (from the `dspChain` flag), the engine display, the bypass display, and alive/dead — including the attach case
  where `bypass` and `dspChain` come from telemetry.
- **`current_.engine` sync invariant:** a bypass-only toggle emits a `SetParams` whose `engine` equals the last
  `telemetry.activeEngine` (never a stale value) — the guard against gap-#1's spurious engine switch.
- Spawn-arg construction: `(moduleDir, configPath)` → the exact worker command line.
- Liveness/readiness policy: a sequence of `(shmOpen, seq)` samples → present / ready (first `seq` advance) /
  dead (`seq` stalled ~3 intervals or shm gone) decisions.

**Worker unit tests:** widened `Telemetry` (bypass + dspChain) round-trips in the two `win-ipc` telemetry tests.

**Regression (must stay green):** all existing tests (100 + Phase 2c's) and the DFN3 offline golden — the only
Phase 3 worker change is two telemetry fields, which do not touch DSP output.

**Manual / integration (real hardware):** exercised against a `settings.toml` with `dsp_chain = true` unless noted.
- UI launches → worker spawns (no console window) → audio flows to CABLE Input.
- Engine switch (RNNoise ↔ DeepFilterNet3) via the window **and** the tray: **instant** under `warm_standby = true`,
  **gap-free on-demand** under `warm_standby = false` (Phase 2c) — clean either way; status reflects it.
- Bypass toggles cleanly from the window and the tray.
- **No-chain config:** with `dsp_chain = false`, the bypass control is disabled with its reason; the engine control
  still works. (Bypass gating, §3/§6.)
- Minimize/close hides to the tray with audio still flowing; tray left-click restores the window.
- **Isolation:** killing the UI in Task Manager leaves audio flowing; relaunching re-attaches (no second worker)
  and shows the true engine and bypass state.
- Killing the worker shows "Worker not running"; **Relaunch** respawns and reconnects.
- **Quit** shuts down both processes cleanly; the tray icon is removed.
- **Headless-correctness:** the worker still runs and cleans audio with no UI started at all.

## 13. Deliverables

- New `src/voicecleaner/` target (5 source units above) wired into `CMakeLists.txt`.
- New `tests/voicecleaner/` gtest target wired into `CMakeLists.txt`.
- `Telemetry` widened with `bypass` + `dspChain`; worker relay writes them; updated `win-ipc` telemetry tests.
- No regression in the existing tests (100 + Phase 2c's) or the DFN3 golden.

## 14. Forward notes for Phase 4 (not built here)

- **Live warm-standby toggle.** Add the warm-standby checkbox and make it live. Phase 2c already built the
  mechanism — warm/lean is a retirement+preload policy on the dynamic engine container — so Phase 4 wires the
  currently-deferred `SetWarmStandby` pipe command to flip that policy (and persists it per-mic). No engine-switch
  machinery remains to build; this is the checkbox + persistence + the policy flip. Decide the default (lean vs warm)
  there.
- **Slider seeding on attach.** When Phase 4 adds the tuning sliders (aggression/HPF/gain/limiter), the attach path
  must **not** keep seeding those fields from `Params::defaults()` — that would stomp a running worker's live tuning
  on every UI relaunch. Carry those values in telemetry (or read `settings.toml`, its owner-of-record) and seed
  `current_` from the worker's actual state, exactly as this phase already does for `engine` and `bypass`. Harmless
  in Phase 3 (no slider exists to have moved them off the defaults), but a correctness trap once sliders land.
