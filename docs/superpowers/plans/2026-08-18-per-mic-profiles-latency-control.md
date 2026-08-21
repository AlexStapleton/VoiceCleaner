# Per-Mic Profile Reload + Editable Latency Control — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make each microphone load and apply its own saved settings the instant it is selected, and add an editable `ring_frames` (latency) box that respawns the worker on Apply.

**Architecture:** The data layer (`vc::Settings` per-mic profiles) and state layer (`vc::ui::UiState`) already behave correctly; the mic-switch bug is a missing re-seed call in the GUI (`AppWindow::applyPick`). Task 1 adds regression guards that lock the two seams the GUI fix relies on. Task 2 wires per-mic reload into the switch path (live, no respawn). Task 3 adds the editable latency box + explicit-Apply respawn. GUI code (Direct2D window, process spawn) is not unit-testable in this repo, so Tasks 2–3 are verified by compile + manual checks, matching the existing pattern (only `UiState`/`Settings`/`Slider` logic is unit-tested).

**Tech Stack:** C++20, CMake (VS 2022 generator), MSVC, Direct2D/DirectWrite, toml++, GoogleTest.

**Build/test note:** CMake/CTest are not on PATH. Use the full path
`C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe`. The project is already configured in `build/`.

---

## File Structure

- `tests/transport-core/test_settings.cpp` — **modify**: add per-mic + ring round-trip guard.
- `tests/voicecleaner/test_ui_state.cpp` — **modify**: add "seedTuning then onSelectEngine carries tuning" guard.
- `src/voicecleaner/app_window.h` — **modify**: new method decls + members (ring box state, rects).
- `src/voicecleaner/app_window.cpp` — **modify**: extract `seedFromActiveProfile`, wire into `applyPick`; add editable latency box (paint/hit/edit), `respawnWorker`, `applyRing`, layout shift, help text.
- `src/voicecleaner/settings.toml` — **modify**: correct the stale latency comment.

No worker/DSP/IPC changes: per-mic reload rides existing `SelectCaptureDevice` + `SetParams`; latency change rides an existing process respawn.

---

## Task 1: Regression guards for the two seams the GUI fix depends on

These tests lock existing correct behavior in the data + state layers. They are expected to **PASS** on current code (the live bug is only the missing GUI wiring); they exist so a future change to `Settings`/`UiState` cannot silently break the per-mic reload.

**Files:**
- Test: `tests/transport-core/test_settings.cpp` (append)
- Test: `tests/voicecleaner/test_ui_state.cpp` (append)

- [ ] **Step 1: Add the settings per-mic + ring round-trip guard**

Append to `tests/transport-core/test_settings.cpp`:

```cpp
TEST(Settings, PerMicProfilesAndRingRoundTrip) {
  Settings s;
  s.activeCaptureId = "cap-A"; s.renderId = "ren-1"; s.ringFrames = 512;
  vc::MicProfile& a = s.ensureProfile("cap-A", "Mic A");
  a.outputGainDbfs = -12.0f; a.hpfHz = 120.0f; a.gateStrength = 0.7f; a.engine = "dfn3";
  vc::MicProfile& b = s.ensureProfile("cap-B", "Mic B");
  b.outputGainDbfs = -24.0f; b.hpfHz = 60.0f;  b.gateStrength = 0.1f; b.engine = "rnnoise";
  auto p = (std::filesystem::temp_directory_path() / "vc_tmp_permic.toml").string();
  s.save(p);

  Settings r = Settings::load(p);
  EXPECT_EQ(r.ringFrames, 512u);                 // global latency preserved across save/load
  ASSERT_EQ(r.mics.size(), 2u);
  r.activeCaptureId = "cap-A";
  ASSERT_NE(r.activeProfile(), nullptr);
  EXPECT_FLOAT_EQ(r.activeProfile()->outputGainDbfs, -12.0f);
  EXPECT_FLOAT_EQ(r.activeProfile()->hpfHz, 120.0f);
  EXPECT_EQ(r.activeProfile()->engine, "dfn3");
  r.activeCaptureId = "cap-B";
  ASSERT_NE(r.activeProfile(), nullptr);
  EXPECT_FLOAT_EQ(r.activeProfile()->outputGainDbfs, -24.0f);
  EXPECT_FLOAT_EQ(r.activeProfile()->hpfHz, 60.0f);
  EXPECT_EQ(r.activeProfile()->engine, "rnnoise");
}
```

- [ ] **Step 2: Add the UiState carries-tuning guard**

Append to `tests/voicecleaner/test_ui_state.cpp`:

```cpp
TEST(UiState, SeedTuningThenSelectEngineCarriesTuning) {
  UiState ui;
  ui.seedTuning(/*aggression*/0.4f, /*hpfHz*/95.0f, /*gainDbfs*/-18.0f, /*limiterDbfs*/-2.0f,
                /*declick*/0.3f, /*declickVoiceFloor*/0.1f, /*gate*/0.5f);
  vc::ipc::Params p = ui.onSelectEngine(1u);   // the exact call the mic-switch path uses
  EXPECT_EQ(p.engine, 1u);
  EXPECT_FLOAT_EQ(p.aggression, 0.4f);
  EXPECT_FLOAT_EQ(p.hpfCutoffHz, 95.0f);
  EXPECT_FLOAT_EQ(p.agcTargetDbfs, -18.0f);
  EXPECT_FLOAT_EQ(p.limiterCeilingDbfs, -2.0f);
  EXPECT_FLOAT_EQ(p.declickStrength, 0.3f);
  EXPECT_FLOAT_EQ(p.declickVoiceFloor, 0.1f);
  EXPECT_FLOAT_EQ(p.gateStrength, 0.5f);
}
```

- [ ] **Step 3: Build the two test targets**

Run:
```bash
C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe --build build --config Debug --target transport-core-tests voicecleaner-tests
```
Expected: builds with no errors.

- [ ] **Step 4: Run the two new tests, expect PASS**

Run:
```bash
build/tests/transport-core/Debug/transport-core-tests.exe --gtest_filter=Settings.PerMicProfilesAndRingRoundTrip
build/tests/voicecleaner/Debug/voicecleaner-tests.exe --gtest_filter=UiState.SeedTuningThenSelectEngineCarriesTuning
```
Expected: both report `[  PASSED  ] 1 test.` (These guard existing behavior — a PASS is the correct, intended result.)

- [ ] **Step 5: Commit**

```bash
git add tests/transport-core/test_settings.cpp tests/voicecleaner/test_ui_state.cpp
git commit -m "test: guard per-mic profile + ring round-trip and UiState carries-tuning invariant"
```

---

## Task 2: Reload the selected mic's profile live on input switch

Extract the profile→UI seeding from `refreshDevices` into a helper, then call it in the capture-switch path so the sliders, `UiState`, and the pushed `Params` all reflect the newly-selected mic.

**Files:**
- Modify: `src/voicecleaner/app_window.h` (add one method decl)
- Modify: `src/voicecleaner/app_window.cpp` (`refreshDevices`, new `seedFromActiveProfile`, `applyPick`)

- [ ] **Step 1: Declare the helper in the header**

In `src/voicecleaner/app_window.h`, in the private methods block, directly after the line
`void persistTuning();                            // write the active profile on drag-end (Task 6)`
add:

```cpp
  void seedFromActiveProfile();                    // load the active mic's tuning into sliders + UiState
```

- [ ] **Step 2: Add the helper definition and call it from `refreshDevices`**

In `src/voicecleaner/app_window.cpp`, replace the seeding block inside `refreshDevices()` (currently):

```cpp
  picker_.setActive(settings_->activeCaptureId, settings_->renderId);
  if (vc::MicProfile* p = settings_->activeProfile()) {   // seed sliders + UiState from the active profile
    sldGain_.setValue(p->outputGainDbfs);
    sldAggr_.setValue(p->aggression);
    sldHpf_.setValue(p->hpfHz);
    sldLim_.setValue(p->limiterDbfs);
    sldDck_.setValue(p->declickStrength);
    sldVfl_.setValue(p->declickVoiceFloor);
    sldGate_.setValue(p->gateStrength);
    state_->seedTuning(p->aggression, p->hpfHz, p->outputGainDbfs, p->limiterDbfs, p->declickStrength, p->declickVoiceFloor, p->gateStrength);
  }
}
```

with:

```cpp
  picker_.setActive(settings_->activeCaptureId, settings_->renderId);
  seedFromActiveProfile();   // seed sliders + UiState from the active profile
}

void AppWindow::seedFromActiveProfile() {
  if (!settings_) return;
  vc::MicProfile* p = settings_->activeProfile();
  if (!p) return;
  sldGain_.setValue(p->outputGainDbfs);
  sldAggr_.setValue(p->aggression);
  sldHpf_.setValue(p->hpfHz);
  sldLim_.setValue(p->limiterDbfs);
  sldDck_.setValue(p->declickStrength);
  sldVfl_.setValue(p->declickVoiceFloor);
  sldGate_.setValue(p->gateStrength);
  state_->seedTuning(p->aggression, p->hpfHz, p->outputGainDbfs, p->limiterDbfs,
                     p->declickStrength, p->declickVoiceFloor, p->gateStrength);
}
```

- [ ] **Step 3: Re-seed on capture switch in `applyPick`**

In `src/voicecleaner/app_window.cpp`, in `applyPick`, replace the capture-switch branch (currently):

```cpp
  } else if (k == PickerKind::Capture) {
    sup_->sendSelectCapture(id, /*driftSeed*/0.0);         // live pipeline rebuild on the new mic
    const vc::MicProfile* p = settings_->activeProfile();
    uint32_t eng = (p && p->engine == "dfn3") ? 1u : 0u;
    sendCurrent(state_->onSelectEngine(eng));              // apply the mic's engine
  } else {
```

with:

```cpp
  } else if (k == PickerKind::Capture) {
    sup_->sendSelectCapture(id, /*driftSeed*/0.0);         // live pipeline rebuild on the new mic
    seedFromActiveProfile();                                // load the new mic's sliders + tuning into state_
    const vc::MicProfile* p = settings_->activeProfile();
    uint32_t eng = (p && p->engine == "dfn3") ? 1u : 0u;
    sendCurrent(state_->onSelectEngine(eng));              // push the new mic's engine + tuning live
  } else {
```

- [ ] **Step 4: Build the GUI**

Run:
```bash
C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe --build build --config Debug --target voicecleaner
```
Expected: builds with no errors.

- [ ] **Step 5: Run the unit tests to confirm no regression**

Run:
```bash
build/tests/voicecleaner/Debug/voicecleaner-tests.exe
```
Expected: all tests PASS (seeding logic still routes through `UiState::seedTuning`, guarded by Task 1).

- [ ] **Step 6: Manual verification (two mics, distinct settings)**

1. Launch `build/src/voicecleaner/Debug/voicecleaner.exe` (or the Release build if that is what you run).
2. Select Mic A, set its sliders to distinctive values (e.g. TARGET LOUDNESS low, NOISE GATE high), let each drag-end persist.
3. Select Mic B, set clearly different slider values.
4. Switch back to Mic A: the sliders must snap to Mic A's saved values, and the audio character must match (not Mic B's).
5. While on Mic A, nudge one slider and confirm on disk that only Mic A's `[mic."…"]` block changed (open the live `settings.toml` next to the exe) — Mic B's block must be untouched.

Expected: each mic keeps its own settings across switches; no cross-contamination.

- [ ] **Step 7: Commit**

```bash
git add src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp
git commit -m "fix(ui): reload each mic's saved tuning/engine live on input switch"
```

---

## Task 3: Editable latency (ring_frames) box with Apply-triggered respawn

Add a numeric box + Apply button below the NOISE GATE slider. Typing edits a buffer; Apply/Enter clamps to `[4, 512]`, persists `ring_frames`, and respawns the worker so it re-reads the value.

**Files:**
- Modify: `src/voicecleaner/app_window.h` (methods + members + rects)
- Modify: `src/voicecleaner/app_window.cpp` (constants, layout, paint, hit-test, input, `respawnWorker`, `applyRing`, ring helpers, help text, window height)
- Modify: `src/voicecleaner/settings.toml` (comment fix)

- [ ] **Step 1: Add declarations, members, and rects to the header**

In `src/voicecleaner/app_window.h`:

(a) In the private methods block, after the `seedFromActiveProfile();` line added in Task 2, add:

```cpp
  void respawnWorker();                            // full stop + fresh spawn so ring_frames is re-read
  void applyRing();                                // commit+clamp the latency box, persist, respawn
  void commitRingEdit();                           // normalize ringEdit_ to the clamped pending value
  int  ringParse() const;                          // ringEdit_ digits -> int (0 if empty/non-digit)
  int  ringPendingClamped() const;                 // ringParse() clamped to [kRingMin, kRingMax]
```

(b) In the members, after the line `int helpHot_ = -1;   // control under the cursor for the help strip (see helpTargetAt/helpText)` add:

```cpp
  std::wstring ringEdit_;      // latency box edit buffer (digits); seeded from settings_->ringFrames
  bool ringFocused_ = false;   // latency box has keyboard focus (caret shown)
```

(c) In the control-rects block, change the line
`D2D1_RECT_F rcEngineRnn_{}, rcEngineDfn_{}, rcBypass_{}, rcRelaunch_{}, rcQuit_{}, rcHelp_{};`
to:
```cpp
  D2D1_RECT_F rcEngineRnn_{}, rcEngineDfn_{}, rcBypass_{}, rcRelaunch_{}, rcQuit_{}, rcHelp_{};
  D2D1_RECT_F rcRingBox_{}, rcRingApply_{};   // editable latency box + Apply button
```

- [ ] **Step 2: Add the clamp constants and ring helpers**

In `src/voicecleaner/app_window.cpp`, after the existing `static const float kRowH = 34.0f;` line near the top of the `namespace vc::ui {` block, add:

```cpp
static const int kRingMin = 4;      // below the worker's floor of 3; keeps the value meaningful
static const int kRingMax = 512;    // ~2.5 s buffered (ring_frames x 5 ms)
```

Then add these definitions (place them right after the `persistTuning()` definition):

```cpp
int AppWindow::ringParse() const {
  if (ringEdit_.empty()) return 0;
  int v = 0;
  for (wchar_t c : ringEdit_) {
    if (c < L'0' || c > L'9') return 0;
    v = v * 10 + (int)(c - L'0');
    if (v > 100000) return 100000;   // guard overflow on absurd input
  }
  return v;
}

int AppWindow::ringPendingClamped() const {
  int v = ringParse();
  if (v < kRingMin) v = kRingMin;
  if (v > kRingMax) v = kRingMax;
  return v;
}

void AppWindow::commitRingEdit() {
  ringEdit_ = std::to_wstring(ringPendingClamped());
}

void AppWindow::respawnWorker() {
  sup_->shutdown(3000);      // fully stop the current worker process
  pushedTuning_ = false;     // re-push the active mic's tuning to the fresh worker (tick() readiness path)
  sup_->startOrAttach();     // spawns fresh -> reads the new ring_frames
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::applyRing() {
  if (!settings_) return;
  commitRingEdit();
  int v = ringPendingClamped();
  if ((size_t)v == settings_->ringFrames) return;   // no change -> nothing to restart
  settings_->ringFrames = (size_t)v;
  settings_->save(configPath_);
  if (spawned_) respawnWorker();   // first-run (no worker yet): value takes effect at first spawn
}
```

- [ ] **Step 3: Seed the edit buffer at startup and grow the window**

In `src/voicecleaner/app_window.cpp`, in `create()`:

(a) Change the window rect line
`  RECT wr{0, 0, 460, 804};`
to:
```cpp
  RECT wr{0, 0, 460, 864};   // +60 px for the LATENCY row
```

(b) Immediately after the `refreshDevices();` call in `create()`, add:
```cpp
  if (settings_) ringEdit_ = std::to_wstring((int)settings_->ringFrames);   // seed the latency box
```

- [ ] **Step 4: Lay out the latency row and shift the lower controls down**

In `src/voicecleaner/app_window.cpp`, in `layout()`, replace the three lines:

```cpp
  rcHelp_      = D2D1::RectF(pad, 670, sz.width - pad, 754);       // hover help strip (what each control does)
  rcRelaunch_  = D2D1::RectF(pad, 670, sz.width - pad, 670 + h);   // only when Down (shares the help band)
  rcQuit_      = D2D1::RectF(sz.width - pad - 90, sz.height - 36, sz.width - pad, sz.height - 10);
```

with:

```cpp
  rcRingBox_   = D2D1::RectF(pad, 684, pad + 120, 684 + h);            // editable latency box
  rcRingApply_ = D2D1::RectF(sz.width - pad - 100, 684, sz.width - pad, 684 + h);
  rcHelp_      = D2D1::RectF(pad, 740, sz.width - pad, 824);           // hover help strip
  rcRelaunch_  = D2D1::RectF(pad, 740, sz.width - pad, 740 + h);       // only when Down (shares the help band)
  rcQuit_      = D2D1::RectF(sz.width - pad - 90, sz.height - 36, sz.width - pad, sz.height - 10);
```

- [ ] **Step 5: Paint the latency row**

In `src/voicecleaner/app_window.cpp`, in `paint()`, directly AFTER the seven `drawSlider(...)` calls (the last is `drawSlider(L"NOISE GATE",  6, true);`) and BEFORE the `// 7. Quit` block, add:

```cpp
  // 6b. Latency (ring frames): editable box + approx-ms readout + Apply (restarts the worker)
  label(L"LATENCY (ring frames)", D2D1::RectF(pad, rcRingBox_.top - 20, sz.width - pad, rcRingBox_.top - 4), dim, capFmt_);
  box(rcRingBox_, ringFocused_ ? panelHot : panel);
  {
    std::wstring shown = ringEdit_;
    if (ringFocused_) shown += L"|";                       // simple caret while focused
    D2D1_RECT_F br = D2D1::RectF(rcRingBox_.left + 12, rcRingBox_.top + 9, rcRingBox_.right - 8, rcRingBox_.bottom);
    rt_->DrawText(shown.c_str(), (UINT32)shown.size(), textFmt_, br, text);
    wchar_t ms[32];
    swprintf(ms, 32, L"\xE2\x89\x88 %d ms", ringParse() * 5);   // buffered delay ~= frames x 5 ms
    D2D1_RECT_F mr = D2D1::RectF(rcRingBox_.right + 12, rcRingBox_.top + 9, rcRingApply_.left - 8, rcRingBox_.bottom);
    rt_->DrawText(ms, (UINT32)wcslen(ms), textFmt_, mr, dim);
  }
  const bool ringDirty = settings_ && ringPendingClamped() != (int)settings_->ringFrames;
  box(rcRingApply_, ringDirty ? (hot_==5 ? accentHot : accent) : disabled);
  label(L"Apply", rcRingApply_, ringDirty ? text : dim, centerFmt_);
```

- [ ] **Step 6: Route hover + clicks to the latency controls**

In `src/voicecleaner/app_window.cpp`:

(a) In `hitControl`, before `return -1;`, add:
```cpp
  if (hit(rcRingApply_, x, y)) return 5;
```

(b) In `helpTargetAt`, before `return -1;`, add:
```cpp
  if (hit(rcRingBox_, x, y) || hit(rcRingApply_, x, y)) return 102;
```

(c) In `helpText`, add a case before `default:`:
```cpp
    case 102: return L"LATENCY — audio buffer depth in frames (1 frame = 10 ms audio; buffered delay \xE2\x89\x88 frames \xC3\x97 5 ms). Higher tolerates glitches better but adds delay. Applying restarts the audio engine (brief dropout).";
```

(d) In `handle`, `WM_LBUTTONDOWN`, immediately after `int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);` (the first line of the case), add:
```cpp
      if (ringFocused_ && !hit(rcRingBox_, x, y)) { commitRingEdit(); ringFocused_ = false; }   // click-away commits
```

(e) In the same `WM_LBUTTONDOWN` case, directly after the two picker-field `if (hit(rcMicField_...` / `if (hit(rcOutField_...` lines, add:
```cpp
      if (hit(rcRingBox_, x, y)) { ringFocused_ = true; InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
```

(f) In the `switch (hitControl(x, y))` block inside `WM_LBUTTONDOWN`, add a case:
```cpp
        case 5: applyRing(); break;
```

- [ ] **Step 7: Handle typing into the box (WM_CHAR)**

In `src/voicecleaner/app_window.cpp`, in `handle`, add a new case before `case WM_MOUSEMOVE:`:

```cpp
    case WM_CHAR:
      if (ringFocused_) {
        wchar_t c = (wchar_t)w;
        if (c >= L'0' && c <= L'9') { if (ringEdit_.size() < 4) ringEdit_.push_back(c); }
        else if (c == L'\b') { if (!ringEdit_.empty()) ringEdit_.pop_back(); }   // Backspace
        else if (c == L'\r') { applyRing(); ringFocused_ = false; }              // Enter commits + applies
        else if (c == 27)    { commitRingEdit(); ringFocused_ = false; }         // Esc cancels edit
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      break;
```

- [ ] **Step 8: Fix the stale latency comment in the shipped template**

In `src/voicecleaner/settings.toml`, replace the line:

```
# REAL-TIME latency: ring_frames MUST stay small (~16 = ~15 ms buffered). 1024 = ~5 s latency.
```

with:

```
# REAL-TIME latency: buffered delay ~= ring_frames x 5 ms (16 ~= 80 ms, 512 ~= 2.5 s, 1024 ~= 5 s).
# Editable in-app: the LATENCY box (clamped 4..512) writes this and restarts the worker.
```

- [ ] **Step 9: Build the GUI**

Run:
```bash
C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe --build build --config Debug --target voicecleaner
```
Expected: builds with no errors.

- [ ] **Step 10: Manual verification (latency box + respawn)**

1. Launch the built `voicecleaner.exe` with a mic already configured (worker running).
2. Hover the LATENCY box/Apply → the help strip shows the latency explanation.
3. Click the box, type `64`, press Enter → audio briefly drops then resumes; the "≈ ms" readout shows ≈320 ms; open the live `settings.toml` and confirm `ring_frames = 64`.
4. Re-open the app later → the LATENCY box shows `64` (load-preserving).
5. Type `9999`, click Apply → value clamps to `512` in the box and on disk.
6. Type `1`, click Apply → clamps to `4`.
7. Confirm the sliders and lower controls (help strip, Quit) are fully visible in the taller window with no overlap.

Expected: latency edits clamp to [4, 512], persist, and restart the worker; the value survives an app restart.

- [ ] **Step 11: Full test sweep + commit**

Run:
```bash
C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe --build build --config Debug --target transport-core-tests voicecleaner-tests
build/tests/transport-core/Debug/transport-core-tests.exe
build/tests/voicecleaner/Debug/voicecleaner-tests.exe
```
Expected: all tests PASS.

```bash
git add src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp src/voicecleaner/settings.toml
git commit -m "feat(ui): editable latency (ring_frames) box with Apply-triggered worker respawn"
```

---

## Self-Review

**Spec coverage:**
- Part 1 (per-mic reload on switch) → Task 2 (`seedFromActiveProfile` + `applyPick`), guarded by Task 1 Step 2. ✓
- Part 1 data-loss fix (drag no longer overwrites new mic) → Task 2 Step 3 (sliders match active profile before any drag). ✓
- Part 2 (load-preserving) → Task 3 Step 3b (seed box from disk) + existing `save()` writing the GUI-owned value; guarded by Task 1 Step 1 (ring round-trip). ✓
- Part 3 (editable box, clamp [4,512], Apply→respawn, help, layout, comment fix) → Task 3 Steps 1–8. ✓
- Testing section (settings round-trip; UiState carries-tuning; manual switch + respawn) → Task 1 + manual Steps in Tasks 2 & 3. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"; every code step shows full code. ✓

**Type consistency:** `seedFromActiveProfile`, `respawnWorker`, `applyRing`, `commitRingEdit`, `ringParse`, `ringPendingClamped`, members `ringEdit_`/`ringFocused_`, rects `rcRingBox_`/`rcRingApply_`, constants `kRingMin`/`kRingMax`, hitControl id `5`, help id `102` — all declared in Task 3 Step 1/2 and used consistently thereafter. `settings_->ringFrames` is `size_t` (`settings.h:29`); casts to/from `int` are explicit. ✓
