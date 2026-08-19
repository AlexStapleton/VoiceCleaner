# Per-Mic Profile Reload + Editable Latency Control — Design

Date: 2026-08-18
Status: Approved (design), pending implementation plan

## Problem

Two defects and one missing control in the VoiceCleaner GUI (`src/voicecleaner`):

1. **Switching microphones keeps the previous mic's settings.** When the user
   picks a different capture device, the worker is told to switch devices and the
   new mic's *engine* is applied, but the on-screen sliders and `UiState` are
   never re-seeded from the new mic's profile. The pipeline therefore runs the
   *previous* mic's tuning on the new mic. Worse: the next slider drag calls
   `persistTuning()`, which writes all the stale on-screen values into the new
   mic's profile — silently overwriting whatever that mic had saved (data loss).

2. **`ring_frames` (audio latency) is not editable and hand-edits don't stick.**
   The value lives in `[global].ring_frames` and is read only once, at worker
   process startup. There is no GUI control for it. Hand-editing the file
   appeared to do nothing because the edit landed on the wrong copy (the `src/`
   template rather than the live `settings.toml` next to the exe) and/or was made
   while the app was running and later overwritten by a GUI `save()`.

## Goals

- Each microphone loads and applies its own saved settings the instant it is
  selected, with no worker respawn.
- `ring_frames` is editable from the GUI as a free-entry numeric box (clamped
  `[4, 512]`), applied via an explicit button that respawns the worker.
- Hand-edits to `ring_frames` made while the app is closed survive; the GUI is
  the source of truth once running.

## Non-Goals

- Per-microphone `ring_frames`. It stays a single global value (a pipeline
  latency/stability tradeoff, not a per-mic taste).
- Live resizing of the ring buffers without a respawn (the worker sizes them
  once in the bridge constructor; runtime resize is out of scope).
- Any change to the drift controller, engines, or DSP leaves.

## Relevant current behavior (as-is)

- `AppWindow::refreshDevices()` (`src/voicecleaner/app_window.cpp:99-108`) seeds
  the 7 sliders and `state_->seedTuning(...)` from `settings_->activeProfile()`.
  This is the only place profile → UI seeding happens today.
- `AppWindow::applyPick()` Capture branch (`app_window.cpp:412-416`) calls
  `sendSelectCapture(id, 0.0)` (live pipeline rebuild) then applies the profile's
  engine — but does **not** re-seed sliders/state.
- `persistTuning()` (`app_window.cpp:351-363`) writes the 7 slider values into
  `settings_->activeProfile()` and calls `settings_->save()`. Called on slider
  drag-end (`WM_LBUTTONUP`).
- `Settings::load` (`src/transport-core/settings.cpp:31`) reads
  `[global].ring_frames` into `settings_.ringFrames`; `Settings::save`
  (`settings.cpp:87`) writes it back. The worker reads it via `Config::load →
  resolveActive` and sizes `ringA_/ringB_` once in the bridge constructor
  (`src/voicecleaner-worker/bridge.cpp:17`).
- `WorkerSupervisor::startOrAttach()` attaches to a live worker if one exists,
  else spawns; `shutdown()` stops the current worker (waits on the process
  handle for a spawned worker). `ring_frames` is only read at spawn.
- `FRAME_SAMPLES = 480` (10 ms @ 48 kHz). The drift controller targets a
  half-full ring, so buffered latency ≈ `ring_frames × 5 ms`
  (16 → ~80 ms, 512 → ~2.5 s, 1024 → ~5 s).
- The window is a fixed, non-resizable 460×804 client; controls are laid out at
  absolute Y coordinates in `layout()`, packed down to the Quit footer.

## Part 1 — Per-mic settings load on input switch

**Extract** the profile → UI seeding block from `refreshDevices()` into a private
helper:

```cpp
void AppWindow::seedFromActiveProfile();   // sliders sldGain_..sldGate_ + state_->seedTuning(...)
```

`refreshDevices()` calls it (behavior unchanged). `applyPick()`'s Capture branch
gains it. New Capture-switch sequence:

1. `ensureProfile(id, name); activeCaptureId = id`  — unchanged
2. `settings_->save(configPath_)`                    — unchanged
3. `sup_->sendSelectCapture(id, 0.0)`                — unchanged (live rebuild)
4. `seedFromActiveProfile()`                         — **NEW**
5. `const MicProfile* p = settings_->activeProfile();`
   `uint32_t eng = (p && p->engine == "dfn3") ? 1u : 0u;`
   `sendCurrent(state_->onSelectEngine(eng));`       — unchanged call, now carries
   the freshly-seeded tuning because step 4 refreshed `current_` via `seedTuning`.

`onSelectEngine` returns `current_`, and `SetParams` carries the full `Params`
snapshot (aggression, hpf, agcTarget, limiter, declick, voiceFloor, gate,
engine, bypass). So a single `sendCurrent` pushes the new mic's engine and tuning
to the worker live — no respawn.

**Why this fixes the data loss:** after step 4 the sliders match the active
profile, so a later drag's `persistTuning()` writes the correct mic's values.

**Ordering note:** the old mic's profile is already current before the switch
(every slider drag persists on `WM_LBUTTONUP`), so no explicit "flush old
profile" step is needed.

## Part 2 — `ring_frames` load-preserving

No new clobber-guard is required. Guarantees:

- Startup seeds the GUI latency box from `settings_.ringFrames`
  (already loaded from `[global].ring_frames`).
- `save()` writes exactly the GUI-owned value; it never invents or resets it.
- Therefore a hand-edit made while the app is **closed** survives, and while the
  app is **running** the GUI (seeded from disk at startup) is the source of
  truth; changing the value through the box + Apply persists it.

Also update the stale comment in `src/voicecleaner/settings.toml` (currently
claims "16 = ~15 ms"; actual buffered latency ≈ `ring_frames × 5 ms`, so
16 ≈ 80 ms).

## Part 3 — Editable latency control + respawn

### UI

A new row below the NOISE GATE slider:

```
LATENCY (ring frames)
[  64  ]  ≈320 ms          [ Apply ]
```

- **Editable numeric box** (`rcRingBox_`), hand-rolled in the D2D dark theme:
  - Click to focus → blinking caret; a focus/caret flag on `AppWindow`.
  - `WM_CHAR`: accept digits `0-9` (cap the buffer at 4 chars); Backspace
    deletes; Enter commits (same as Apply). Non-digits ignored.
  - The edit buffer is a `std::wstring ringEdit_`. The "≈NNN ms" readout
    (`value × 5`) updates live as the buffer changes.
  - Losing focus (click elsewhere) commits the buffer (clamp, but no respawn on
    blur — respawn only on Apply/Enter).
- **Clamp on commit:** parse `ringEdit_` → integer, clamp to `[4, 512]`, write
  back the clamped text. Empty/garbage → revert to the running value.
- **`[Apply]` button** (`rcRingApply_`): enabled only when the committed pending
  value ≠ `settings_.ringFrames`. Action:
  1. `settings_->ringFrames = pending;`
  2. `settings_->save(configPath_);`
  3. `respawnWorker();`
- Hover help (new `helpTargetAt` id + `helpText` case): "LATENCY — audio buffer
  depth in frames (1 frame = 10 ms audio; buffered delay ≈ frames × 5 ms).
  Higher tolerates glitches better but adds delay. Applying restarts the audio
  engine (brief dropout)."

### Respawn helper

```cpp
void AppWindow::respawnWorker() {
  sup_->shutdown(3000);      // fully stop the current worker process
  pushedTuning_ = false;     // re-push current tuning to the fresh worker
  sup_->startOrAttach();     // spawns fresh -> reads the new ring_frames
  InvalidateRect(hwnd_, nullptr, FALSE);
}
```

After respawn, the existing `tick()` readiness path re-pushes `state_->current()`
once (guarded by `pushedTuning_`), so the fresh worker receives the active mic's
tuning/engine. `spawned_` stays true. Expected brief (<1 s) audio dropout.

Guard: if `!spawned_` (first-run, no worker yet), Apply just persists the value
and does not respawn (there is nothing to restart; the value takes effect at
first spawn).

### Layout

The window is fixed-size; grow the client height from 804 to ~864 (+60 px) and
insert the latency row between the NOISE GATE track (`rcGate_`, y≈636) and the
help strip. Shift `rcHelp_`, `rcRelaunch_`, and `rcQuit_` down by the same
amount. New rects: `rcRingBox_`, `rcRingApply_` (the "≈ms" text draws next to the
box, no rect needed). Add them to `hitControl`/click routing and `helpTargetAt`.

### State added to `AppWindow`

- `std::wstring ringEdit_;` — the edit buffer (seeded from `settings_.ringFrames`
  at startup / in `setSettings` or `create`).
- `bool ringFocused_ = false;` — box has keyboard focus (caret shown).
- `D2D1_RECT_F rcRingBox_{}, rcRingApply_{};`

## Testing

- **Settings unit test** (`tests/transport-core/test_settings.cpp`): a settings
  table with two `[mic."…"]` profiles carrying distinct tuning; assert
  `activeProfile()` returns the correct profile's values for each
  `activeCaptureId`, and that `ring_frames` round-trips unchanged through
  `load → save → load`.
- **Manual verification** (the window + a real worker process, not unit-testable):
  - Switch between two mics with different tuning → sliders + audio update to the
    selected mic's saved values; dragging a slider then persists to the correct
    mic.
  - Type a latency value, Apply → worker respawns and the new buffering takes
    effect; typing >512 clamps to 512; the file's `[global].ring_frames` reflects
    the applied value; relaunching preserves it.

## Files touched

- `src/voicecleaner/app_window.h` — new members, method decls, rects.
- `src/voicecleaner/app_window.cpp` — `seedFromActiveProfile`, `applyPick`
  Capture branch, `respawnWorker`, latency box paint/hit/edit, layout shift,
  help text.
- `src/voicecleaner/settings.toml` — corrected latency comment.
- `tests/transport-core/test_settings.cpp` — per-mic + ring round-trip test.

No changes to the worker, DSP core, or IPC schema — per-mic reload rides existing
`SelectCaptureDevice` + `SetParams` commands; latency change rides an existing
process respawn.
