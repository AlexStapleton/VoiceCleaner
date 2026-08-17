# Product Shell — Phase 4a: Device picker + settings persistence — Design

_Date: 2026-08-17 · Status: draft for review_

Parent design: [`2026-08-15-product-shell-design.md`](2026-08-15-product-shell-design.md) (§4.3 mic picker, §5.1
select-device command, §7/§7.1 settings persistence + per-mic profiles). Builds directly on the merged Phase 3 UI
skeleton ([`2026-08-16-product-shell-phase3-ui-skeleton-design.md`](2026-08-16-product-shell-phase3-ui-skeleton-design.md)).

## 1. Problem & scope

Phase 3 shipped `voicecleaner.exe` (Direct2D window + tray) that spawns/attaches/supervises the worker and drives
engine switch + bypass — but it **cannot select a microphone**. With blank device IDs it shows "No microphone
configured" and does not spawn; to run at all, a user must hand-edit `settings.toml`. This phase makes the app
**usable without editing TOML**: enumerate audio devices, pick capture + render in the UI, persist the choice, and
apply it live.

**In scope:** capture + render device **enumeration**, a **custom-drawn dark dropdown** picker for each, persistence
to a new `[global]` + `[mic."<id>"]` `settings.toml` schema, the **first-run** flow (pick → spawn), **live mic/render
switching** while the worker runs, and **device-gone / non-48 kHz** handling.

**Out of scope (later increments, named only to fix the boundary):** the Direct2D level meter (4b); tuning sliders
aggression/HPF/gain/limiter (4c); the live warm-standby toggle, drift-ppm persistence, and per-mic *tuning* fields
(4d); the full first-run wizard checklist / VB-Cable install guidance (5); live hot-plug auto-refresh (4a
re-enumerates when a dropdown opens — that is enough).

## 2. Locked decisions (from brainstorming)

| Decision | Choice |
|---|---|
| Schema | **Full per-mic structure now** — `[global]` (active capture id, render id, ring_frames) + one `[mic."<id>"]` profile per microphone. Only the fields that exist today are populated (`name`, `engine`, `warm_standby`, `dsp_chain`); tuning fields land in 4c/4d. |
| Schema owner | **One `Settings` model in `transport-core`** (already links `tomlplusplus`), shared by worker and UI. `Config::load` becomes a thin resolver over it. Not a second toml reader in the UI. |
| Back-compat | If `[global]` is absent, fall back to Phase-3's **flat top-level keys** (`capture_device_id`/`render_device_id`/`ring_frames`/`engine`/`dsp_chain`/`warm_standby`), so an existing file still loads; the UI migrates to the new layout on its next write. |
| Apply-on-switch | **Live pipeline rebuild.** Switching a mic while the worker runs sends `SelectCaptureDevice(id, driftSeed)` (→ `Bridge::reselectCapture`: stop→rebuild→start) + `SetParams(engine)`; switching render sends `SelectRenderDevice(id)`. The worker process stays alive (no respawn). warm_standby-per-mic fully applies once 4d wires the live toggle. |
| Dropdown | **Custom-drawn** dark list overlay on the D2D target (matches the theme). Device lists are short — no scrolling machinery. |
| 48 kHz | Devices are enumerated with their `sampleRate`; non-48 kHz devices are shown **greyed with a "48 kHz only" note** and are not selectable (the worker requires 48 kHz endpoints). |
| Render picker | Included in 4a (both capture and render must be chosen for audio to flow), defaulting the render selection to a device named like "CABLE Input" when present. |

## 3. `settings.toml` schema (new)

```toml
[global]
active_capture_id = "{0.0.1.00000000}.{5fbcc184-...}"   # selected mic (WASAPI id)
render_id         = "{0.0.0.00000000}.{59cf8297-...}"   # CABLE Input
ring_frames       = 16                                   # RT latency policy (global; stays 16)

[mic."{0.0.1.00000000}.{5fbcc184-...}"]
name         = "fifine Microphone (K688)"   # cached friendly label for the dropdown (UTF-8)
engine       = "rnnoise"                     # "none" | "rnnoise" | "dfn3"
warm_standby = false
dsp_chain    = true
# aggression / hpf_hz / output_gain_dbfs / limiter_dbfs / drift_ppm are added in 4c/4d
```

- **Keys are WASAPI device IDs** (ASCII). The active mic is `[global].active_capture_id`; its profile is
  `[mic."<that id>"]`. `render_id` is global (not per-mic) in 4a.
- **Blank IDs** (`active_capture_id = ""`) → first-run/NoConfig (§7).
- The shipped template (`src/voicecleaner/settings.toml`) is rewritten to this schema with blank IDs and
  `ring_frames = 16`.
- **Unknown/future keys are not preserved** across a UI rewrite (the UI is owner-of-record and, within a version,
  models every field it ships). Acceptable this phase; revisit only if external editing of unmodeled keys becomes a
  goal.

## 4. `Settings` model — new unit in `transport-core`

New `src/transport-core/settings.{h,cpp}` (Windows-free; uses `toml++`, already a `transport-core` dependency):

```cpp
namespace vc {
struct MicProfile {                 // one [mic."<id>"] section (4a fields only)
  std::string id;                   // WASAPI id (the section key)
  std::string name;                 // cached friendly label (UTF-8)
  std::string engine = "rnnoise";   // "none" | "rnnoise" | "dfn3"
  bool warmStandby = false;
  bool dspChain = true;
};
struct Settings {
  std::string activeCaptureId;      // [global].active_capture_id  ("" => none)
  std::string renderId;             // [global].render_id
  size_t      ringFrames = 16;      // [global].ring_frames
  std::vector<MicProfile> mics;     // all [mic."<id>"] sections

  static Settings load(const std::string& path);   // tolerant: flat-file back-compat; missing file => empty
  void save(const std::string& path) const;        // serialize [global] + every [mic."<id>"]
  MicProfile* activeProfile();                      // profile for activeCaptureId, or nullptr
  MicProfile& ensureProfile(const std::string& id, const std::string& name);  // create default if unseen
  Config resolveActive() const;                     // flatten active profile → the worker's flat Config
};
}
```

- **`resolveActive()`** produces the existing flat `Config` (captureDeviceId/renderDeviceId/engine/warmStandby/
  dspChain/ringFrames) the worker already consumes — RT/Bridge code is untouched.
- **`Config::load` is reimplemented** as `return Settings::load(path).resolveActive();`, preserving its current
  signature and its `validate()` call. Flat files (no `[global]`) resolve exactly as before → existing behavior and
  the shipped `drift*.toml` configs keep working unchanged.
- `save()` writes `[global]` + one `[mic."<id>"]` table per profile. The UI calls it; the worker never writes.

## 5. Device enumeration (UI side, Win32)

The UI links **`win-audio`** and uses the existing `vc::DeviceEnumerator`:
- `list(eCapture)` / `list(eRender)` → `vector<DeviceInfo{ id(wstring), name(wstring), sampleRate, channels, kind }>`.
- Enumerated on **window open** and **each time a dropdown is opened** (so a just-plugged device appears; no
  background hot-plug listener this phase). COM is already initialized (`main.cpp` `CoInitializeEx`).
- The Win32 layer converts `DeviceInfo` (wstring id/name) into the pure picker's input rows: `id` as ASCII
  `std::string` (device IDs are ASCII in this codebase), `name` as UTF-8, plus `sampleRate` and an `is48k` flag.

## 6. `DevicePicker` — new pure unit in `voicecleaner-core`

New `src/voicecleaner/device_picker.{h,cpp}` (Windows-free, unit-tested — the decision core of the picker):

```cpp
namespace vc::ui {
struct DeviceRow { std::string id, name; uint32_t sampleRate; bool is48k; };
enum class PickerKind { Capture, Render };

class DevicePicker {
 public:
  void setDevices(const std::vector<DeviceRow>& capture, const std::vector<DeviceRow>& render);
  void setActive(const std::string& captureId, const std::string& renderId);

  void open(PickerKind k);          // click the field
  void close();                     // click-away / Esc / after a pick
  bool isOpen() const; PickerKind openKind() const;

  const std::vector<DeviceRow>& rows(PickerKind k) const;
  // The active row's display label, or "(not connected)" when the saved id is absent from `rows`.
  std::string fieldLabel(PickerKind k) const;
  bool selectable(const DeviceRow& r) const { return r.is48k; }   // non-48k rows are inert
  // Result of clicking a list row: the chosen id if selectable+changed, else empty (no-op).
  std::string pick(PickerKind k, size_t rowIndex);
  bool deviceGone(PickerKind k) const;   // active id set but not present in `rows`
};
}
```

The Win32 side owns painting the field + overlay and hit-testing; **all decisions** (label, selectable, active
check, device-gone, open/close/pick) live here so they are testable without a display or COM.

## 7. The window (4a additions)

Two new controls above the existing engine/bypass rows, both drawn by the same custom dropdown:

1. **Microphone** — a field showing the active capture device's name + a `▾`; clicking opens a **dark overlay list**
   of capture devices (active row check-marked; non-48 kHz rows greyed with a "48 kHz only" tag; hover highlight;
   click a row to select; click-away or `Esc` to dismiss).
2. **Output** — same control for render devices (defaulting to a "CABLE Input"-named device when first seen).

**First-run / NoConfig:** when `active_capture_id`/`render_id` are blank (or the saved capture id is not present),
the window opens with the two pickers as the primary content and **does not spawn** the worker. This replaces Phase
3's static "No microphone configured" text — the pickers are now the call to action. `tick()` continues to not poll
in this state (design §10 of Phase 3), but a **selection** transitions out of it (§8).

The engine segmented control and bypass toggle from Phase 3 remain; they operate on the active mic.

3. **Quit** — a small labeled control in the window footer that runs the **full shutdown path** (the same one the
   tray "Quit" uses: `sup_->shutdown(3000); DestroyWindow(hwnd_)` → stop the worker, remove the tray icon, exit
   both processes). It is the first visible in-window way to fully stop everything. The window **X** and
   **minimize** still hide to the tray (audio keeps running) — unchanged; only a *new, explicit* Quit affordance is
   added. Reuses the existing `kTrayCmdQuit` handler.

## 8. Flows

- **First selection (from NoConfig):** user picks capture (and render) →
  `settings.ensureProfile(id, name)`, `settings.activeCaptureId = id`, `settings.renderId = renderId`,
  `settings.save(path)` → `WorkerSupervisor::startOrAttach()` spawns the worker against the file → normal running UI.
- **Switch mic while running:** `settings.activeCaptureId = id` (+ `ensureProfile` if unseen) → `save` →
  pipe `SelectCaptureDevice(id, driftSeed=0)` (no cached `drift_ppm` until 4d) → pipe `SetParams(engine = new
  profile's engine)`. `Bridge::reselectCapture` rebuilds the pipeline on the new device (brief, expected gap); the
  worker process stays alive.
- **Switch render while running:** `settings.renderId = id` → `save` → pipe `SelectRenderDevice(id)`
  (→ `Bridge::reselectRender`).
- **Persistence:** the UI writes `settings.toml` on each committed device change (device picks are discrete and
  infrequent — the debounced-write machinery the parent design mentions is for high-rate *slider* edits and lands
  in 4c).

## 9. Error handling

- **Saved device gone at launch:** enumeration lacks `active_capture_id` → the field shows "(not connected)", the
  app stays in the pick state and does **not** spawn (avoids a guaranteed launch failure); the user picks a present
  device.
- **Non-48 kHz device:** greyed and non-selectable in the list with a "48 kHz only" note (`DeviceInfo.sampleRate !=
  48000`). If a stale `settings.toml` names a non-48 kHz active device, spawning still fails fast; the UI surfaces
  the worker's launch failure (Phase 3 §11) and points at the picker.
- **`settings.toml` read-only / write fails:** surface a clear, non-fatal warning; keep running with in-memory
  settings (parent §7 caveat).
- **Enumeration returns nothing / COM error:** show an empty picker with a message and a way to retry
  (re-enumerate); never crash the UI.

## 10. Components / files

**New:**
- `src/transport-core/settings.{h,cpp}` — the `Settings` model (§4). Wired into `transport-core/CMakeLists.txt`.
- `src/voicecleaner/device_picker.{h,cpp}` — the pure `DevicePicker` (§6). Added to `voicecleaner-core`.
- `tests/transport-core` additions — `Settings` load/save/resolve tests.
- `tests/voicecleaner/test_device_picker.cpp` — `DevicePicker` tests (new file in the existing target).

**Modified:**
- `src/transport-core/config.cpp` — `Config::load` → `Settings::load(path).resolveActive()` (keeps signature +
  `validate()`; preserves flat back-compat).
- `src/voicecleaner/app_window.{h,cpp}` — the two picker fields + overlay paint/hit-test; enumerate on open; wire
  picks to `Settings` save + pipe commands / spawn; a **window Quit control** (§7.3) reusing the existing
  `kTrayCmdQuit` shutdown path.
- `src/voicecleaner/worker_supervisor.{h,cpp}` — expose typed helpers to send `SelectCaptureDevice`/
  `SelectRenderDevice` (thin wrappers over `PipeClient::send`), alongside the existing `send`.
- `src/voicecleaner/main.cpp` — first-run flow: load `Settings`, resolve devices, decide spawn vs. pick state.
- `src/voicecleaner/CMakeLists.txt` — link **`win-audio`** into the `voicecleaner` exe (for `DeviceEnumerator`).
- `src/voicecleaner/settings.toml` — rewritten to the new `[global]`+`[mic]` schema (blank IDs, `ring_frames = 16`).

## 11. Testing

**Pure units (no audio, no display):**
- **`Settings` (`tests/transport-core`):** `load → mutate → save → load` is identity; an unseen mic via
  `ensureProfile` creates a default profile; `resolveActive()` flattens the active profile into `Config`
  correctly; a **flat** Phase-3 file (no `[global]`) resolves to the same `Config` as before (back-compat);
  missing file → empty settings; missing active profile → resolver defaults.
- **`Config::load` regression (`tests/transport-core`):** the existing config tests still pass unchanged (flat
  files resolve identically), guarding the reimplementation.
- **`DevicePicker` (`tests/voicecleaner`):** field label for present vs. absent active id ("(not connected)");
  non-48 kHz rows are non-selectable; `pick` returns the id only on a selectable, changed row (else no-op);
  open/close/openKind transitions; `deviceGone` detection; active-row identification for the checkmark.

**Regression (must stay green):** all existing tests (130) and the DFN3 offline golden — 4a changes config
*parsing* and adds UI code; it must not alter resolved `Config` values for existing files or touch DSP output.

**Manual / integration (real hardware):**
- First run (blank IDs) → both pickers shown, no worker → pick K688 + CABLE Input → worker spawns → audio flows.
- Switch mic live → pipeline rebuilds on the new device (brief gap), audio continues; engine reflects the new
  profile; no worker respawn.
- Switch output (render) live.
- A non-48 kHz device is greyed and cannot be selected.
- Unplug the saved mic, relaunch → field shows "(not connected)", app waits for a pick, does not crash.
- Selection persists across a UI relaunch (re-attaches to the running worker and shows the right devices).
- Headless-correctness: the worker still runs standalone against a `settings.toml` (new or flat schema).
- The window **Quit** control stops the worker and exits both processes cleanly (tray icon removed) — matching the
  tray Quit; window X / minimize still hide to the tray with audio running.

## 12. Forward notes (not built here)

- **4b (meter):** reads the already-published `Telemetry.inputRms/inputPeak/vadActive`.
- **4c (sliders):** adds `aggression/hpf_hz/output_gain_dbfs/limiter_dbfs` to `MicProfile` + the per-mic profile
  sections; introduces the debounced settings write for high-rate edits; must seed slider values from the active
  profile on attach (the seeding-on-attach trap named in Phase 3 §14).
- **4d (warm-standby + drift):** wires the live `SetWarmStandby` command (today a no-op stub) so a per-mic
  `warm_standby` applies without a rebuild, and persists the worker's reported `driftRatioPpm` into a per-mic
  `drift_ppm` for the warm-start seed carried by `SelectCaptureDevice`.
