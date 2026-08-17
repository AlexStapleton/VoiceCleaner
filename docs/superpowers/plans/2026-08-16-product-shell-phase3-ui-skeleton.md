# Product Shell — Phase 3: UI Skeleton — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `voicecleaner.exe` — one Direct2D window + tray icon that spawns/attaches/supervises `voicecleaner-worker.exe` and drives engine switch and bypass over the existing named-pipe/shm IPC.

**Architecture:** All decision logic lives in a pure, Windows-free static library (`voicecleaner-core`: `ui_state`) that is unit-tested with gtest. The Win32/Direct2D/tray glue (`main`, `worker_supervisor`, `app_window`, `tray`) is thin and links the core. The only worker-side change is two new telemetry fields (`bypass`, `dspChain`); no RT-path or DSP-output change.

**Tech Stack:** C++20, Direct2D + DirectWrite, Win32 (`user32`/`shell32`/`ole32`), the existing `vc-ipc` (Params/Command codec), `win-ipc` (`PipeClient`/`TelemetryReader`), and `transport-core` (`Config::load`). Build via the ESP-IDF-bundled CMake (see below). GoogleTest for units.

**Design spec:** [`docs/superpowers/specs/2026-08-16-product-shell-phase3-ui-skeleton-design.md`](../specs/2026-08-16-product-shell-phase3-ui-skeleton-design.md)

**Build/test commands (CMake is NOT on PATH):**
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```

---

## File Structure

**New — pure core (Windows-free, unit-tested):**
- `src/voicecleaner/ui_state.h` / `ui_state.cpp` — `vc::ui::UiState` (status/displays/control-enablement/engine-sync), `vc::ui::LivenessTracker` (present/ready/dead policy), `vc::ui::buildWorkerCommandLine` (spawn-arg builder). Depends only on `vc-ipc` (`Params`, `Telemetry`) + the C++ standard library.

**New — Win32/D2D/tray glue (built, manually verified; not unit-tested):**
- `src/voicecleaner/worker_supervisor.h` / `.cpp` — spawn (`CreateProcess`), attach probe (`TelemetryReader::open`), per-tick `poll()` (feeds `LivenessTracker` + `WaitForSingleObject`), `send()` (`PipeClient::send`), `shutdown()`.
- `src/voicecleaner/app_window.h` / `.cpp` — window class, `ID2D1HwndRenderTarget` lifetime, layout/hit-test/paint, `WM_*` handlers, `WM_TIMER` (~10 Hz) telemetry poll.
- `src/voicecleaner/tray.h` / `.cpp` — `Shell_NotifyIcon` add/modify/delete, `TrackPopupMenu`, `WM_APP` callback.
- `src/voicecleaner/main.cpp` — `WinMain`; single-instance mutex + restore broadcast; pre-flight `Config::load`; creates window/tray/supervisor; message pump.

**New — config + build glue:**
- `src/voicecleaner/settings.toml` — shipped template with **`ring_frames = 16`** (real-time), device IDs blank.
- `cmake/copy_if_absent.cmake` — copies the template next to the exe only if absent (never clobbers a user's file).
- `src/voicecleaner/CMakeLists.txt` — `voicecleaner-core` STATIC lib + `voicecleaner` WIN32 exe.
- `tests/voicecleaner/CMakeLists.txt` + `tests/voicecleaner/test_ui_state.cpp` — gtest target.

**Modified:**
- `src/vc-ipc/telemetry.h` — add `uint32_t bypass` + `uint32_t dspChain` to the `Telemetry` POD.
- `src/voicecleaner-worker/bridge.cpp` — relay writes the two new fields.
- `tests/win-ipc/test_telemetry_shm.cpp` — round-trip the widened struct.
- `CMakeLists.txt` (root) — `add_subdirectory(src/voicecleaner)` + `add_subdirectory(tests/voicecleaner)`.

---

## Task 1: Widen the `Telemetry` POD (bypass + dspChain)

**Files:**
- Modify: `src/vc-ipc/telemetry.h`
- Test: `tests/win-ipc/test_telemetry_shm.cpp`

- [ ] **Step 1: Extend the round-trip test to exercise the new fields**

In `tests/win-ipc/test_telemetry_shm.cpp`, replace the body of `TEST(TelemetryShm, WriterToReaderRoundTrip)` with:

```cpp
TEST(TelemetryShm, WriterToReaderRoundTrip) {
  const std::wstring name = L"Local\\voicecleaner-test-telemetry";
  TelemetryWriter w; ASSERT_TRUE(w.create(name));
  TelemetryReader r; ASSERT_TRUE(r.open(name));
  Telemetry t{}; t.inputRms = 0.25f; t.dropouts = 3; t.activeEngine = 1;
  t.driftRatioPpm = -26.0; t.seq = 7; t.bypass = 1; t.dspChain = 1;
  w.write(t);
  Telemetry got = r.read();
  EXPECT_FLOAT_EQ(got.inputRms, 0.25f);
  EXPECT_EQ(got.dropouts, 3u);
  EXPECT_EQ(got.activeEngine, 1u);
  EXPECT_DOUBLE_EQ(got.driftRatioPpm, -26.0);
  EXPECT_EQ(got.seq, 7u);
  EXPECT_EQ(got.bypass, 1u);
  EXPECT_EQ(got.dspChain, 1u);
}
```

- [ ] **Step 2: Build the test target and verify it fails to compile**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target win-ipc-tests
```
Expected: FAIL — `error: 'struct vc::ipc::Telemetry' has no member named 'bypass'`.

- [ ] **Step 3: Add the two fields to the POD**

In `src/vc-ipc/telemetry.h`, append two fields to the `Telemetry` struct immediately after `uint32_t seq;`:

```cpp
  uint32_t seq;            // increments each aggregate publish
  uint32_t bypass;         // current bypass state (0/1); lets the UI reflect true bypass on attach
  uint32_t dspChain;       // capability: worker has a DSP chain (0/1); UI disables bypass when 0
};
```

- [ ] **Step 4: Build and run the win-ipc tests — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target win-ipc-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R TelemetryShm --output-on-failure
```
Expected: PASS (both `TelemetryShm.*` tests).

- [ ] **Step 5: Commit**

```
git add src/vc-ipc/telemetry.h tests/win-ipc/test_telemetry_shm.cpp
git commit -m "feat(ipc): widen Telemetry POD with bypass + dspChain fields"
```

---

## Task 2: Worker relay writes `bypass` + `dspChain`

**Files:**
- Modify: `src/voicecleaner-worker/bridge.cpp:174-188` (the telemetry-publish block inside `relayLoop`)

The relay already has `sp` (the `Params` snapshot) and `chain_` in scope at the publish site.

- [ ] **Step 1: Write the two new fields at publish time**

In `src/voicecleaner-worker/bridge.cpp`, inside the `if (++hopsSincePublish_ >= 100)` block, add the two writes right after `t.workerState = ...;` and before `t.seq = ++telemetrySeq_;`:

```cpp
          t.workerState = (uint32_t)vc::ipc::WorkerState::Running;
          t.bypass   = (chain_ && sp.bypass != 0) ? 1u : 0u;   // true bypass only meaningful with a chain
          t.dspChain = chain_ ? 1u : 0u;                        // capability flag for the UI
          t.seq = ++telemetrySeq_;
```

Rationale: `bypass` is applied via `chain_->setBypass(...)`; with no chain there is nothing bypassed, so report 0. `dspChain` is the capability the UI gates the bypass control on.

- [ ] **Step 2: Build the worker — verify it compiles**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-worker
```
Expected: builds clean (pre-existing benign warnings only: `LNK4098`, `C4324`, `C4996`).

- [ ] **Step 3: Run the full test suite — verify no regression**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```
Expected: PASS — all existing tests green (the change touches only two telemetry fields, no DSP output).

- [ ] **Step 4: Commit**

```
git add src/voicecleaner-worker/bridge.cpp
git commit -m "feat(worker): publish bypass + dspChain in telemetry relay"
```

---

## Task 3: Scaffold `voicecleaner-core` lib + `tests/voicecleaner` target; first `UiState` test (Active status)

**Files:**
- Create: `src/voicecleaner/ui_state.h`
- Create: `src/voicecleaner/ui_state.cpp`
- Create: `src/voicecleaner/CMakeLists.txt`
- Create: `tests/voicecleaner/CMakeLists.txt`
- Create: `tests/voicecleaner/test_ui_state.cpp`
- Modify: `CMakeLists.txt` (root)

- [ ] **Step 1: Write the header with the full pure-core API**

Create `src/voicecleaner/ui_state.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>
#include "params.h"      // vc::ipc::Params
#include "telemetry.h"   // vc::ipc::Telemetry

namespace vc::ui {

// What the status line is showing. All ASCII so it is trivially testable; the painter maps
// the kind to a colored dot glyph and statusLine() builds the full display string.
enum class StatusKind : uint32_t { Active, Bypassed, Down, NoConfig };

// Pure UI state model (blueprint §5). Fed telemetry + user events; computes the display and the
// control-enablement state. Owns one Params `current_` seeded from Params::defaults() (which equals
// the worker's runtime values), and keeps current_.engine / current_.bypass synced to telemetry so
// a bypass-only toggle never carries a stale engine (design §5, gap-#1 guard).
class UiState {
 public:
  UiState();

  void setRenderLabel(std::string label);   // e.g. "CABLE Input"; used in the status line

  // ---- events ----
  void onNoConfig();                                        // pre-flight failed: no config / no devices
  void onTelemetry(const vc::ipc::Telemetry& t, bool ready);  // ready == LivenessTracker reports Ready
  void onWorkerDown();                                      // supervisor declared death / launch failure

  // ---- user actions: mutate current_, return the Params the caller sends via SetParams ----
  vc::ipc::Params onSelectEngine(uint32_t engine);   // 0 = RNNoise, 1 = DFN3
  vc::ipc::Params onToggleBypass();                   // flips current_.bypass; engine stays synced

  // ---- queries (paint) ----
  StatusKind statusKind() const { return kind_; }
  std::string statusLine() const;
  uint32_t activeEngine() const { return activeEngine_; }   // segmented-control highlight (authoritative)
  bool bypassed() const { return bypass_; }
  bool bypassEnabled() const { return dspChain_; }
  std::string bypassDisabledReason() const;
  const vc::ipc::Params& current() const { return current_; }

 private:
  static const char* engineName(uint32_t engine);   // "RNNoise" / "DeepFilterNet3"

  vc::ipc::Params current_ = vc::ipc::Params::defaults();
  StatusKind kind_ = StatusKind::Down;   // pre-ready shows Down until the first ready telemetry
  std::string renderLabel_ = "CABLE Input";
  uint32_t activeEngine_ = 0;
  bool bypass_ = false;
  bool dspChain_ = true;
};

}  // namespace vc::ui
```

- [ ] **Step 2: Write the implementation (only what this task needs; later tasks extend it)**

Create `src/voicecleaner/ui_state.cpp`:

```cpp
#include "ui_state.h"

namespace vc::ui {

UiState::UiState() = default;

void UiState::setRenderLabel(std::string label) { renderLabel_ = std::move(label); }

const char* UiState::engineName(uint32_t engine) {
  return engine == 1 ? "DeepFilterNet3" : "RNNoise";
}

void UiState::onNoConfig() { kind_ = StatusKind::NoConfig; }

void UiState::onWorkerDown() { kind_ = StatusKind::Down; }

void UiState::onTelemetry(const vc::ipc::Telemetry& t, bool ready) {
  activeEngine_ = t.activeEngine;
  bypass_       = (t.bypass != 0);
  dspChain_     = (t.dspChain != 0);
  // Keep the outgoing snapshot synced to live state (design §5): a later bypass-only toggle then
  // carries the live engine, and the embedded request_engine on the worker becomes a no-op.
  current_.engine = t.activeEngine;
  current_.bypass = t.bypass;
  if (ready) kind_ = bypass_ ? StatusKind::Bypassed : StatusKind::Active;
}

vc::ipc::Params UiState::onSelectEngine(uint32_t engine) {
  current_.engine = engine;
  return current_;
}

vc::ipc::Params UiState::onToggleBypass() {
  current_.bypass = current_.bypass ? 0u : 1u;
  return current_;
}

std::string UiState::bypassDisabledReason() const {
  return "no DSP chain \xE2\x80\x94 bypass unavailable";   // U+2014 em dash, UTF-8
}

std::string UiState::statusLine() const {
  switch (kind_) {
    case StatusKind::Active:
      return "\xE2\x97\x8F Active \xC2\xB7 " + std::string(engineName(activeEngine_)) +
             " \xC2\xB7 \xE2\x86\x92 " + renderLabel_;                       // ● … · … · → …
    case StatusKind::Bypassed:
      return "\xE2\x97\x8B Bypassed \xC2\xB7 " + std::string(engineName(activeEngine_)) +
             " \xC2\xB7 \xE2\x86\x92 " + renderLabel_;                       // ○ …
    case StatusKind::NoConfig:
      return "No microphone configured \xE2\x80\x94 the mic picker arrives in Phase 4";
    case StatusKind::Down:
    default:
      return "\xE2\x9C\x95 Worker not running";                             // ✕ …
  }
}

}  // namespace vc::ui
```

Note: non-ASCII glyphs are written as explicit UTF-8 byte escapes so the build is encoding-independent; the target still sets `/utf-8` (Step 3) for safety and for the painter's conversions.

- [ ] **Step 3: Add the CMake targets**

Create `src/voicecleaner/CMakeLists.txt` (core lib only for now; the exe is added in Task 14):

```cmake
# Pure, Windows-free decision core — the unit-tested heart of the UI (design §4).
add_library(voicecleaner-core STATIC ui_state.cpp)
target_include_directories(voicecleaner-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voicecleaner-core PUBLIC vc-ipc)
if (MSVC)
  target_compile_options(voicecleaner-core PUBLIC /utf-8)
endif()
```

Create `tests/voicecleaner/CMakeLists.txt`:

```cmake
add_executable(voicecleaner-tests test_ui_state.cpp)
target_link_libraries(voicecleaner-tests PRIVATE voicecleaner-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(voicecleaner-tests)
```

In the root `CMakeLists.txt`, add after `add_subdirectory(src/dfn3-probe)`:

```cmake
add_subdirectory(src/voicecleaner)
```

and after `add_subdirectory(tests/win-ipc)`:

```cmake
add_subdirectory(tests/voicecleaner)
```

- [ ] **Step 4: Write the first failing test**

Create `tests/voicecleaner/test_ui_state.cpp`:

```cpp
#include <gtest/gtest.h>
#include "ui_state.h"
using namespace vc::ui;
using vc::ipc::Telemetry;

static Telemetry tel(uint32_t engine, uint32_t bypass, uint32_t dspChain, uint32_t seq) {
  Telemetry t{}; t.activeEngine = engine; t.bypass = bypass; t.dspChain = dspChain; t.seq = seq;
  t.workerState = (uint32_t)vc::ipc::WorkerState::Running;
  return t;
}

TEST(UiState, ActiveStatusLineFromReadyTelemetry) {
  UiState ui; ui.setRenderLabel("CABLE Input");
  ui.onTelemetry(tel(/*engine*/1, /*bypass*/0, /*dspChain*/1, /*seq*/1), /*ready*/true);
  EXPECT_EQ(ui.statusKind(), StatusKind::Active);
  EXPECT_EQ(ui.statusLine(),
            "\xE2\x97\x8F Active \xC2\xB7 DeepFilterNet3 \xC2\xB7 \xE2\x86\x92 CABLE Input");
  EXPECT_EQ(ui.activeEngine(), 1u);
}
```

- [ ] **Step 5: Regenerate the build (new subdirs) and run the test**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R UiState --output-on-failure
```
Expected: PASS (`UiState.ActiveStatusLineFromReadyTelemetry`).

- [ ] **Step 6: Commit**

```
git add src/voicecleaner/ui_state.h src/voicecleaner/ui_state.cpp src/voicecleaner/CMakeLists.txt tests/voicecleaner/CMakeLists.txt tests/voicecleaner/test_ui_state.cpp CMakeLists.txt
git commit -m "feat(ui): pure UiState core + gtest target (active status line)"
```

---

## Task 4: `UiState` — Bypassed status + displays from telemetry

**Files:**
- Test: `tests/voicecleaner/test_ui_state.cpp`

The implementation from Task 3 already handles Bypassed; this task locks the behavior with tests.

- [ ] **Step 1: Add the failing tests**

Append to `tests/voicecleaner/test_ui_state.cpp`:

```cpp
TEST(UiState, BypassedStatusLineAndDisplay) {
  UiState ui; ui.setRenderLabel("CABLE Input");
  ui.onTelemetry(tel(/*engine*/0, /*bypass*/1, /*dspChain*/1, /*seq*/1), /*ready*/true);
  EXPECT_EQ(ui.statusKind(), StatusKind::Bypassed);
  EXPECT_TRUE(ui.bypassed());
  EXPECT_EQ(ui.statusLine(),
            "\xE2\x97\x8B Bypassed \xC2\xB7 RNNoise \xC2\xB7 \xE2\x86\x92 CABLE Input");
}

TEST(UiState, PreReadyTelemetryDoesNotFlipToActive) {
  UiState ui;
  ui.onTelemetry(tel(1, 0, 1, 1), /*ready*/false);   // present but not yet ready
  EXPECT_EQ(ui.statusKind(), StatusKind::Down);       // stays Down until ready
  EXPECT_EQ(ui.activeEngine(), 1u);                   // display fields still track telemetry
}
```

- [ ] **Step 2: Run the tests — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R UiState --output-on-failure
```
Expected: PASS (all `UiState.*`).

- [ ] **Step 3: Commit**

```
git add tests/voicecleaner/test_ui_state.cpp
git commit -m "test(ui): lock bypassed + pre-ready status behavior"
```

---

## Task 5: `UiState` — bypass control enablement from `dspChain` (incl. attach)

**Files:**
- Test: `tests/voicecleaner/test_ui_state.cpp`

- [ ] **Step 1: Add the failing tests**

Append to `tests/voicecleaner/test_ui_state.cpp`:

```cpp
TEST(UiState, BypassControlDisabledWhenNoChain) {
  UiState ui;
  ui.onTelemetry(tel(/*engine*/0, /*bypass*/0, /*dspChain*/0, /*seq*/1), /*ready*/true);
  EXPECT_FALSE(ui.bypassEnabled());
  EXPECT_EQ(ui.bypassDisabledReason(), "no DSP chain \xE2\x80\x94 bypass unavailable");
}

TEST(UiState, BypassControlEnabledWithChain) {
  UiState ui;
  ui.onTelemetry(tel(0, 0, /*dspChain*/1, 1), /*ready*/true);
  EXPECT_TRUE(ui.bypassEnabled());
}

TEST(UiState, AttachReflectsTrueBypassAndCapability) {
  // Attach to a worker already bypassed, with a chain: the very first telemetry must show it.
  UiState ui;
  ui.onTelemetry(tel(/*engine*/1, /*bypass*/1, /*dspChain*/1, /*seq*/42), /*ready*/true);
  EXPECT_TRUE(ui.bypassed());
  EXPECT_TRUE(ui.bypassEnabled());
  EXPECT_EQ(ui.statusKind(), StatusKind::Bypassed);
}
```

- [ ] **Step 2: Run the tests — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R UiState --output-on-failure
```
Expected: PASS.

- [ ] **Step 3: Commit**

```
git add tests/voicecleaner/test_ui_state.cpp
git commit -m "test(ui): bypass control gating on dspChain incl. attach case"
```

---

## Task 6: `UiState` — Down + NoConfig transitions

**Files:**
- Test: `tests/voicecleaner/test_ui_state.cpp`

- [ ] **Step 1: Add the failing tests**

Append to `tests/voicecleaner/test_ui_state.cpp`:

```cpp
TEST(UiState, WorkerDownStatus) {
  UiState ui;
  ui.onTelemetry(tel(1, 0, 1, 1), true);   // was Active
  ui.onWorkerDown();
  EXPECT_EQ(ui.statusKind(), StatusKind::Down);
  EXPECT_EQ(ui.statusLine(), "\xE2\x9C\x95 Worker not running");
}

TEST(UiState, NoConfigStatus) {
  UiState ui;
  ui.onNoConfig();
  EXPECT_EQ(ui.statusKind(), StatusKind::NoConfig);
  EXPECT_EQ(ui.statusLine(),
            "No microphone configured \xE2\x80\x94 the mic picker arrives in Phase 4");
}
```

- [ ] **Step 2: Run the tests — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R UiState --output-on-failure
```
Expected: PASS.

- [ ] **Step 3: Commit**

```
git add tests/voicecleaner/test_ui_state.cpp
git commit -m "test(ui): worker-down + no-config status transitions"
```

---

## Task 7: `UiState` — engine-sync invariant (the gap-#1 guard)

**Files:**
- Test: `tests/voicecleaner/test_ui_state.cpp`

This is the core correctness invariant from design §5: a bypass-only toggle must emit a `SetParams` whose `engine` equals the last `telemetry.activeEngine`, never a stale value — otherwise the worker's per-`SetParams` `request_engine` would spuriously switch engines.

- [ ] **Step 1: Add the failing tests**

Append to `tests/voicecleaner/test_ui_state.cpp`:

```cpp
TEST(UiState, BypassToggleCarriesLiveEngineNotStaleDefault) {
  // Params::defaults().engine == 1 (DFN3). Worker is live on engine 0 (RNNoise).
  UiState ui;
  ui.onTelemetry(tel(/*engine*/0, /*bypass*/0, /*dspChain*/1, /*seq*/1), /*ready*/true);
  vc::ipc::Params sent = ui.onToggleBypass();
  EXPECT_EQ(sent.engine, 0u);   // synced to live engine, NOT the stale default 1
  EXPECT_EQ(sent.bypass, 1u);
}

TEST(UiState, SelectEngineSetsOutgoingEngine) {
  UiState ui;
  ui.onTelemetry(tel(/*engine*/1, 0, 1, 1), true);
  vc::ipc::Params sent = ui.onSelectEngine(0u);
  EXPECT_EQ(sent.engine, 0u);
}

TEST(UiState, BypassToggleFlipsBackAndForth) {
  UiState ui;
  ui.onTelemetry(tel(1, /*bypass*/0, 1, 1), true);
  EXPECT_EQ(ui.onToggleBypass().bypass, 1u);   // off -> on
  ui.onTelemetry(tel(1, /*bypass*/1, 1, 2), true);   // worker confirms bypassed
  EXPECT_EQ(ui.onToggleBypass().bypass, 0u);   // on -> off
}
```

- [ ] **Step 2: Run the tests — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R UiState --output-on-failure
```
Expected: PASS.

- [ ] **Step 3: Commit**

```
git add tests/voicecleaner/test_ui_state.cpp
git commit -m "test(ui): engine-sync invariant guards bypass-only SetParams"
```

---

## Task 8: `LivenessTracker` — present / ready / dead policy

**Files:**
- Modify: `src/voicecleaner/ui_state.h`, `src/voicecleaner/ui_state.cpp`
- Test: `tests/voicecleaner/test_ui_state.cpp`

Pure policy over `(shmOpen, seq, nowMs)` samples (design §10/§12). Readiness = first `seq` advance. Dead = shm lost after ready, OR `seq` stalled past `stallTimeoutMs` while Ready, OR never became ready within `readyTimeoutMs` (launch failure).

- [ ] **Step 1: Write the failing tests**

Append to `tests/voicecleaner/test_ui_state.cpp`:

```cpp
using S = LivenessTracker::Status;

TEST(LivenessTracker, PresentThenReadyOnFirstSeqAdvance) {
  LivenessTracker lt;   // defaults: readyTimeout 5000, stallTimeout 3000
  EXPECT_EQ(lt.onSample(/*shmOpen*/false, /*seq*/0, /*now*/0),   S::Connecting);  // shm not up yet
  EXPECT_EQ(lt.onSample(/*shmOpen*/true,  /*seq*/5, /*now*/900), S::Connecting);  // baseline
  EXPECT_EQ(lt.onSample(/*shmOpen*/true,  /*seq*/6, /*now*/1900), S::Ready);      // first advance
}

TEST(LivenessTracker, LaunchFailureWhenNeverReady) {
  LivenessTracker lt;
  EXPECT_EQ(lt.onSample(true, 5, 0),    S::Connecting);   // baseline, no advance
  EXPECT_EQ(lt.onSample(true, 5, 4000), S::Connecting);   // still within readyTimeout
  EXPECT_EQ(lt.onSample(true, 5, 5001), S::Down);         // exceeded readyTimeout -> launch failure
}

TEST(LivenessTracker, DiesWhenSeqStallsAfterReady) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);          // baseline
  lt.onSample(true, 6, 1000);       // Ready (lastAdvance = 1000)
  EXPECT_EQ(lt.onSample(true, 6, 2000), S::Ready);   // stalled but within stallTimeout
  EXPECT_EQ(lt.onSample(true, 6, 4001), S::Down);    // > 3000 since last advance -> dead
}

TEST(LivenessTracker, DiesWhenShmLostAfterReady) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);
  lt.onSample(true, 6, 1000);       // Ready
  EXPECT_EQ(lt.onSample(/*shmOpen*/false, 0, 1100), S::Down);
}

TEST(LivenessTracker, StaysReadyWhileSeqKeepsAdvancing) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);
  lt.onSample(true, 6, 1000);
  EXPECT_EQ(lt.onSample(true, 7, 2000), S::Ready);
  EXPECT_EQ(lt.onSample(true, 8, 6000), S::Ready);   // advancing resets the stall clock
}
```

- [ ] **Step 2: Run to verify it fails to compile**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
```
Expected: FAIL — `LivenessTracker` undeclared.

- [ ] **Step 3: Declare `LivenessTracker` in the header**

In `src/voicecleaner/ui_state.h`, add before the closing `}  // namespace vc::ui`:

```cpp
// Pure liveness/readiness policy over (shmOpen, seq, nowMs) samples (design §10). Terminal at Down.
class LivenessTracker {
 public:
  enum class Status : uint32_t { Connecting, Ready, Down };
  explicit LivenessTracker(uint64_t readyTimeoutMs = 5000, uint64_t stallTimeoutMs = 3000);
  Status onSample(bool shmOpen, uint32_t seq, uint64_t nowMs);
  Status status() const { return status_; }
  bool everReady() const { return everReady_; }   // distinguishes launch failure from later death

 private:
  uint64_t readyTimeoutMs_;
  uint64_t stallTimeoutMs_;
  Status status_ = Status::Connecting;
  bool started_ = false;        // startMs_ initialized
  bool haveBaseline_ = false;   // baseline seq captured
  bool everReady_ = false;
  uint64_t startMs_ = 0;
  uint64_t lastAdvanceMs_ = 0;
  uint32_t lastSeq_ = 0;
};
```

- [ ] **Step 4: Implement in the .cpp**

In `src/voicecleaner/ui_state.cpp`, add before the closing `}  // namespace vc::ui`:

```cpp
LivenessTracker::LivenessTracker(uint64_t readyTimeoutMs, uint64_t stallTimeoutMs)
    : readyTimeoutMs_(readyTimeoutMs), stallTimeoutMs_(stallTimeoutMs) {}

LivenessTracker::Status LivenessTracker::onSample(bool shmOpen, uint32_t seq, uint64_t nowMs) {
  if (!started_) { started_ = true; startMs_ = nowMs; }
  if (status_ == Status::Down) return status_;   // terminal

  if (status_ == Status::Ready) {
    if (!shmOpen) { status_ = Status::Down; return status_; }        // shm lost
    if (seq != lastSeq_) { lastSeq_ = seq; lastAdvanceMs_ = nowMs; } // still advancing
    else if (nowMs - lastAdvanceMs_ > stallTimeoutMs_) status_ = Status::Down;  // stalled
    return status_;
  }

  // Connecting.
  if (shmOpen) {
    if (!haveBaseline_) { haveBaseline_ = true; lastSeq_ = seq; }
    else if (seq != lastSeq_) {
      lastSeq_ = seq; lastAdvanceMs_ = nowMs; everReady_ = true;
      status_ = Status::Ready; return status_;
    }
  }
  if (nowMs - startMs_ > readyTimeoutMs_) status_ = Status::Down;    // never became ready
  return status_;
}
```

- [ ] **Step 5: Run the tests — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R LivenessTracker --output-on-failure
```
Expected: PASS (all `LivenessTracker.*`).

- [ ] **Step 6: Commit**

```
git add src/voicecleaner/ui_state.h src/voicecleaner/ui_state.cpp tests/voicecleaner/test_ui_state.cpp
git commit -m "feat(ui): LivenessTracker present/ready/dead policy with tests"
```

---

## Task 9: `buildWorkerCommandLine` — spawn-arg builder

**Files:**
- Modify: `src/voicecleaner/ui_state.h`, `src/voicecleaner/ui_state.cpp`
- Test: `tests/voicecleaner/test_ui_state.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/voicecleaner/test_ui_state.cpp`:

```cpp
TEST(SpawnArgs, ExactWorkerCommandLine) {
  std::wstring cmd = buildWorkerCommandLine(L"C:\\app", L"C:\\app\\settings.toml");
  EXPECT_EQ(cmd, L"\"C:\\app\\voicecleaner-worker.exe\" --config \"C:\\app\\settings.toml\"");
}
```

- [ ] **Step 2: Run to verify it fails to compile**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
```
Expected: FAIL — `buildWorkerCommandLine` undeclared.

- [ ] **Step 3: Declare + implement**

In `src/voicecleaner/ui_state.h`, add before the closing namespace brace:

```cpp
// Exact worker command line (design §10): "<dir>\voicecleaner-worker.exe" --config "<configPath>".
// No forced flags — the worker honors settings.toml (warm_standby / dsp_chain / engine / ring_frames).
std::wstring buildWorkerCommandLine(const std::wstring& moduleDir, const std::wstring& configPath);
```

In `src/voicecleaner/ui_state.cpp`, add before the closing namespace brace:

```cpp
std::wstring buildWorkerCommandLine(const std::wstring& moduleDir, const std::wstring& configPath) {
  return L"\"" + moduleDir + L"\\voicecleaner-worker.exe\" --config \"" + configPath + L"\"";
}
```

- [ ] **Step 4: Run the test — verify pass**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner-tests
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug -R SpawnArgs --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```
git add src/voicecleaner/ui_state.h src/voicecleaner/ui_state.cpp tests/voicecleaner/test_ui_state.cpp
git commit -m "feat(ui): exact worker command-line builder with test"
```

---

## Task 10: Shipped `settings.toml` (ring_frames = 16) + copy-if-absent CMake

**Files:**
- Create: `src/voicecleaner/settings.toml`
- Create: `cmake/copy_if_absent.cmake`

This is the product-blocking Phase 2c finding: the drift-test default `ring_frames = 1024` primes ~5 s of latency. The shipped config MUST set `ring_frames = 16`.

- [ ] **Step 1: Create the shipped template**

Create `src/voicecleaner/settings.toml`:

```toml
# VoiceCleaner settings — shipped default.
#
# REAL-TIME latency: ring_frames MUST stay small (~16 = ~15 ms buffered). The repo's drift*.toml
# files use ring_frames = 1024 for multi-hour drift TESTS, which primes ~5 s of latency — never
# use that here. (Phase 2c hardware-validation finding, 2026-08-16.)
ring_frames  = 16

# Engine + chain. The worker honors these; the UI never forces overrides (design §2).
engine       = "rnnoise"   # "none" | "rnnoise" | "dfn3"
dsp_chain    = true        # DC->engine->HPF->AGC->limiter; required for the bypass control
warm_standby = false       # lean by default; engine switch is gap-free either way (Phase 2c)

# Device IDs are filled by the Phase 4 mic picker. Left blank here, the UI shows
# "No microphone configured" and does NOT spawn the worker (design §8). For manual hardware
# testing, paste your 48 kHz capture/render IDs (from `voicecleaner-worker --list-devices`):
capture_device_id = ""
render_device_id  = ""
```

- [ ] **Step 2: Create the copy-if-absent helper**

Create `cmake/copy_if_absent.cmake`:

```cmake
# Copy ${SRC} to ${DST} only when ${DST} does not already exist — never clobbers a user's edited
# settings.toml on rebuild. Invoked via `cmake -DSRC=.. -DDST=.. -P copy_if_absent.cmake`.
if(NOT EXISTS "${DST}")
  configure_file("${SRC}" "${DST}" COPYONLY)
  message(STATUS "copy_if_absent: wrote ${DST}")
else()
  message(STATUS "copy_if_absent: kept existing ${DST}")
endif()
```

- [ ] **Step 3: Commit (wiring into the exe POST_BUILD happens in Task 14)**

```
git add src/voicecleaner/settings.toml cmake/copy_if_absent.cmake
git commit -m "feat(ui): shipped settings.toml (ring_frames=16) + copy-if-absent helper"
```

---

## Task 11: `worker_supervisor` — spawn / attach / poll / send / shutdown (Win32 glue)

**Files:**
- Create: `src/voicecleaner/worker_supervisor.h`
- Create: `src/voicecleaner/worker_supervisor.cpp`

Windows glue — verified by build + the manual checklist (Task 15), not unit-tested. Uses the pure `LivenessTracker` + `buildWorkerCommandLine` for all policy.

- [ ] **Step 1: Write the header**

Create `src/voicecleaner/worker_supervisor.h`:

```cpp
#pragma once
#include <string>
#include "telemetry.h"       // vc::ipc::Telemetry
#include "telemetry_shm.h"   // vc::ipc::TelemetryReader
#include "command.h"         // vc::ipc::Command
#include "ui_state.h"        // vc::ui::LivenessTracker

namespace vc::ui {

// Outcome of one poll tick, consumed by the window to drive UiState + repaint.
struct PollResult {
  bool alive = false;              // false -> UiState::onWorkerDown()
  bool ready = false;              // true once the worker is running (first seq advance)
  bool launchFailure = false;      // died before ever becoming ready (bad device / not 48 kHz / no model)
  vc::ipc::Telemetry telemetry{};  // last sample (valid when a reader is open)
};

// Owns the worker lifecycle: spawn or attach, liveness supervision, command send, shutdown.
class WorkerSupervisor {
 public:
  WorkerSupervisor(std::wstring moduleDir, std::wstring configPath);
  ~WorkerSupervisor();

  // Probe for a live worker (TelemetryReader::open). If present -> attach; else CreateProcess.
  // Returns false only if spawn itself failed (CreateProcess error); attach/spawn-then-wait
  // readiness is reported via poll() over subsequent ticks.
  bool startOrAttach();

  PollResult poll(uint64_t nowMs);           // call from WM_TIMER (~10 Hz)
  bool send(const vc::ipc::Command& c);       // PipeClient::send to the worker pipe
  void shutdown(uint64_t waitMs);             // send Shutdown, bounded-wait for exit
  bool workerDown() const { return down_; }

 private:
  bool openReader();                          // (re)open the telemetry mapping

  std::wstring moduleDir_;
  std::wstring configPath_;
  const std::wstring pipeName_ = L"\\\\.\\pipe\\voicecleaner";
  const std::wstring shmName_  = L"Local\\voicecleaner-telemetry";

  vc::ipc::TelemetryReader reader_;
  bool readerOpen_ = false;
  void* process_ = nullptr;                   // HANDLE (spawned only; null when attached)
  LivenessTracker liveness_;
  bool down_ = false;
};

}  // namespace vc::ui
```

- [ ] **Step 2: Write the implementation**

Create `src/voicecleaner/worker_supervisor.cpp`:

```cpp
#include "worker_supervisor.h"
#include <windows.h>
#include "pipe.h"   // vc::ipc::PipeClient

namespace vc::ui {

WorkerSupervisor::WorkerSupervisor(std::wstring moduleDir, std::wstring configPath)
    : moduleDir_(std::move(moduleDir)), configPath_(std::move(configPath)) {}

WorkerSupervisor::~WorkerSupervisor() {
  if (process_) { CloseHandle(static_cast<HANDLE>(process_)); process_ = nullptr; }
}

bool WorkerSupervisor::openReader() {
  if (readerOpen_) return true;
  readerOpen_ = reader_.open(shmName_);
  return readerOpen_;
}

bool WorkerSupervisor::startOrAttach() {
  if (openReader()) return true;   // a worker is already present -> attach (no process handle)

  // Spawn. CreateProcessW requires a mutable command-line buffer.
  std::wstring cmd = buildWorkerCommandLine(moduleDir_, configPath_);
  std::wstring mutableCmd = cmd;   // CreateProcessW may modify in place
  STARTUPINFOW si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, moduleDir_.c_str(), &si, &pi);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  return true;   // readiness confirmed later via poll() (first seq advance)
}

PollResult WorkerSupervisor::poll(uint64_t nowMs) {
  PollResult r;
  if (down_) { r.alive = false; return r; }

  // A spawned process that has exited is authoritative death.
  if (process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0) {
    down_ = true;
    r.alive = false;
    r.launchFailure = !liveness_.everReady();
    return r;
  }

  const bool shm = openReader();
  uint32_t seq = 0;
  if (shm) { r.telemetry = reader_.read(); seq = r.telemetry.seq; }

  auto st = liveness_.onSample(shm, seq, nowMs);
  r.ready = (st == LivenessTracker::Status::Ready);
  if (st == LivenessTracker::Status::Down) {
    down_ = true;
    r.alive = false;
    r.launchFailure = !liveness_.everReady();
  } else {
    r.alive = true;
  }
  return r;
}

bool WorkerSupervisor::send(const vc::ipc::Command& c) {
  return vc::ipc::PipeClient::send(pipeName_, c);   // failure only corroborates death (design §10)
}

void WorkerSupervisor::shutdown(uint64_t waitMs) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::Shutdown;
  vc::ipc::PipeClient::send(pipeName_, c);
  if (process_) {
    WaitForSingleObject(static_cast<HANDLE>(process_), (DWORD)waitMs);
  } else {
    // Attached: wait for the shm mapping to disappear (best-effort bounded wait).
    const uint64_t step = 100;
    for (uint64_t waited = 0; waited < waitMs; waited += step) {
      vc::ipc::TelemetryReader probe;
      if (!probe.open(shmName_)) break;
      Sleep((DWORD)step);
    }
  }
}

}  // namespace vc::ui
```

- [ ] **Step 3: (temporary) add the supervisor to the core lib so it compiles now**

The supervisor uses `windows.h`, so it cannot live in the Windows-free `voicecleaner-core`. It will be compiled into the `voicecleaner` exe in Task 14. To verify it compiles standalone now, temporarily build via the exe target created in Task 14 — **skip building in this task** and defer the compile check to Task 14, Step 4. (Committing the source now keeps tasks bite-sized.)

- [ ] **Step 4: Commit**

```
git add src/voicecleaner/worker_supervisor.h src/voicecleaner/worker_supervisor.cpp
git commit -m "feat(ui): WorkerSupervisor spawn/attach/poll/send/shutdown"
```

---

## Task 12: `app_window` — Direct2D window, layout, paint, hit-test, timer

**Files:**
- Create: `src/voicecleaner/app_window.h`
- Create: `src/voicecleaner/app_window.cpp`

Windows/D2D glue — verified by build + manual checklist. Custom-drawn controls; layout computed from client size; hit-tested in `WM_LBUTTONDOWN`; `WM_TIMER` at 100 ms polls the supervisor and updates `UiState`.

- [ ] **Step 1: Write the header**

Create `src/voicecleaner/app_window.h`:

```cpp
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include "ui_state.h"
#include "worker_supervisor.h"

namespace vc::ui {

class Tray;   // fwd

// The single Direct2D window. Owns the UiState; drives the supervisor from a 100 ms timer.
class AppWindow {
 public:
  AppWindow(HINSTANCE hInst, WorkerSupervisor* sup, UiState* state);
  ~AppWindow();

  bool create(const wchar_t* title);
  HWND hwnd() const { return hwnd_; }
  void setTray(Tray* t) { tray_ = t; }

  void showRestore();          // bring back from tray (single-instance restore + tray click)
  void onCommandBypass();      // shared path for window + tray "Enabled" toggle
  void onCommandEngine(uint32_t engine);
  void requestRelaunch();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT msg, WPARAM w, LPARAM l);
  void ensureRenderTarget();
  void discardRenderTarget();
  void paint();
  void tick();                 // WM_TIMER: poll supervisor -> UiState -> InvalidateRect
  void layout(D2D1_SIZE_F sz); // recompute control rects
  void sendCurrent(const vc::ipc::Params& p);

  HINSTANCE hInst_;
  HWND hwnd_ = nullptr;
  WorkerSupervisor* sup_;
  UiState* state_;
  Tray* tray_ = nullptr;

  ID2D1Factory* d2dFactory_ = nullptr;
  IDWriteFactory* dwFactory_ = nullptr;
  IDWriteTextFormat* textFmt_ = nullptr;
  ID2D1HwndRenderTarget* rt_ = nullptr;

  // control rects (client coords), recomputed in layout()
  D2D1_RECT_F rcEngineRnn_{}, rcEngineDfn_{}, rcBypass_{}, rcRelaunch_{};
};

}  // namespace vc::ui
```

- [ ] **Step 2: Write the implementation**

Create `src/voicecleaner/app_window.cpp`:

```cpp
#include "app_window.h"
#include "tray.h"
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace vc::ui {

static const wchar_t* kClass = L"VoiceCleanerWindow";
static const UINT kTimerId = 1;

static std::wstring toW(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

AppWindow::AppWindow(HINSTANCE hInst, WorkerSupervisor* sup, UiState* state)
    : hInst_(hInst), sup_(sup), state_(state) {}

AppWindow::~AppWindow() {
  discardRenderTarget();
  if (textFmt_) textFmt_->Release();
  if (dwFactory_) dwFactory_->Release();
  if (d2dFactory_) d2dFactory_->Release();
}

bool AppWindow::create(const wchar_t* title) {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&dwFactory_)))) return false;
  dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               16.0f, L"en-us", &textFmt_);

  WNDCLASSW wc{};
  wc.lpfnWndProc = &AppWindow::WndProc;
  wc.hInstance = hInst_;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassW(&wc);

  hwnd_ = CreateWindowExW(0, kClass, title, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                          CW_USEDEFAULT, CW_USEDEFAULT, 460, 260, nullptr, nullptr, hInst_, this);
  if (!hwnd_) return false;
  SetTimer(hwnd_, kTimerId, 100, nullptr);   // ~10 Hz telemetry poll
  ShowWindow(hwnd_, SW_SHOW);
  return true;
}

LRESULT CALLBACK AppWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCT*>(l);
    SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    reinterpret_cast<AppWindow*>(cs->lpCreateParams)->hwnd_ = h;
  }
  auto self = reinterpret_cast<AppWindow*>(GetWindowLongPtr(h, GWLP_USERDATA));
  if (self) return self->handle(m, w, l);
  return DefWindowProc(h, m, w, l);
}

void AppWindow::ensureRenderTarget() {
  if (rt_) return;
  RECT rc; GetClientRect(hwnd_, &rc);
  D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
  d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(hwnd_, size), &rt_);
}

void AppWindow::discardRenderTarget() { if (rt_) { rt_->Release(); rt_ = nullptr; } }

void AppWindow::layout(D2D1_SIZE_F sz) {
  const float pad = 16.0f;
  rcEngineRnn_ = D2D1::RectF(pad,        56, sz.width/2, 92);
  rcEngineDfn_ = D2D1::RectF(sz.width/2, 56, sz.width - pad, 92);
  rcBypass_    = D2D1::RectF(pad, 110, sz.width - pad, 150);
  rcRelaunch_  = D2D1::RectF(pad, 170, sz.width - pad, 206);
}

void AppWindow::paint() {
  ensureRenderTarget();
  if (!rt_) return;
  rt_->BeginDraw();
  rt_->Clear(D2D1::ColorF(0.12f, 0.12f, 0.14f));
  D2D1_SIZE_F sz = rt_->GetSize();
  layout(sz);

  ID2D1SolidColorBrush* text = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.92f,0.92f,0.94f), &text);
  ID2D1SolidColorBrush* accent = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.20f,0.55f,0.95f), &accent);
  ID2D1SolidColorBrush* panel = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.22f,0.22f,0.26f), &panel);
  ID2D1SolidColorBrush* dim = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.45f,0.45f,0.48f), &dim);

  auto drawText = [&](const std::wstring& s, D2D1_RECT_F r, ID2D1Brush* b) {
    rt_->DrawTextW(s.c_str(), (UINT32)s.size(), textFmt_, r, b);
  };
  auto fillBox = [&](D2D1_RECT_F r, ID2D1Brush* b) {
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), b);
  };

  // 1. status line
  drawText(toW(state_->statusLine()), D2D1::RectF(16, 16, sz.width-16, 48), text);

  // 2. engine segmented control (highlight the active engine)
  fillBox(rcEngineRnn_, state_->activeEngine()==0 ? accent : panel);
  fillBox(rcEngineDfn_, state_->activeEngine()==1 ? accent : panel);
  drawText(L"  RNNoise", rcEngineRnn_, text);
  drawText(L"  DeepFilterNet3", rcEngineDfn_, text);

  // 3. bypass toggle (disabled + reason when no chain)
  const bool bEn = state_->bypassEnabled();
  fillBox(rcBypass_, bEn ? panel : D2D1::ColorF(0.18f,0.18f,0.20f));
  std::wstring bLabel = !bEn ? toW(state_->bypassDisabledReason())
                             : (state_->bypassed() ? L"  Bypassed \xE2\x80\x94 click to enable"
                                                   : L"  Enabled \xE2\x80\x94 click to bypass");
  drawText(bLabel, rcBypass_, bEn ? text : dim);

  // 4. Relaunch — only when the worker is down
  if (state_->statusKind()==StatusKind::Down) {
    fillBox(rcRelaunch_, accent);
    drawText(L"  Relaunch worker", rcRelaunch_, text);
  }

  text->Release(); accent->Release(); panel->Release(); dim->Release();
  if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) discardRenderTarget();
}

static bool hit(D2D1_RECT_F r, int x, int y) {
  return x>=r.left && x<=r.right && y>=r.top && y<=r.bottom;
}

void AppWindow::sendCurrent(const vc::ipc::Params& p) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::SetParams; c.params = p;
  sup_->send(c);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::onCommandEngine(uint32_t engine) { sendCurrent(state_->onSelectEngine(engine)); }
void AppWindow::onCommandBypass() { if (state_->bypassEnabled()) sendCurrent(state_->onToggleBypass()); }

void AppWindow::requestRelaunch() {
  sup_->startOrAttach();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::showRestore() {
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void AppWindow::tick() {
  PollResult r = sup_->poll(GetTickCount64());
  if (!r.alive) state_->onWorkerDown();
  else          state_->onTelemetry(r.telemetry, r.ready);
  if (tray_) tray_->update(*state_);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT AppWindow::handle(UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_TIMER: if (w==kTimerId) tick(); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd_, &ps); paint(); EndPaint(hwnd_, &ps); return 0; }
    case WM_SIZE:
      if (rt_) { RECT rc; GetClientRect(hwnd_, &rc); rt_->Resize(D2D1::SizeU(rc.right, rc.bottom)); }
      return 0;
    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
      if (hit(rcEngineRnn_, x, y)) onCommandEngine(0);
      else if (hit(rcEngineDfn_, x, y)) onCommandEngine(1);
      else if (hit(rcBypass_, x, y)) onCommandBypass();
      else if (state_->statusKind()==StatusKind::Down && hit(rcRelaunch_, x, y)) requestRelaunch();
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((w & 0xFFF0) == SC_MINIMIZE) { ShowWindow(hwnd_, SW_HIDE); return 0; }  // minimize -> tray
      break;
    case WM_CLOSE: ShowWindow(hwnd_, SW_HIDE); return 0;  // close -> tray (worker keeps running)
    case WM_DESTROY: KillTimer(hwnd_, kTimerId); PostQuitMessage(0); return 0;
  }
  return DefWindowProc(hwnd_, msg, w, l);
}

}  // namespace vc::ui
```

Note: `WM_LBUTTONDOWN` uses `GET_X_LPARAM`/`GET_Y_LPARAM` — add `#include <windowsx.h>` at the top of `app_window.cpp`.

- [ ] **Step 3: Add the windowsx.h include**

At the top of `src/voicecleaner/app_window.cpp`, add after `#include "tray.h"`:

```cpp
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
```

- [ ] **Step 4: Commit (compile verified in Task 14)**

```
git add src/voicecleaner/app_window.h src/voicecleaner/app_window.cpp
git commit -m "feat(ui): Direct2D AppWindow — layout, paint, hit-test, timer poll"
```

---

## Task 13: `tray` — Shell_NotifyIcon + context menu

**Files:**
- Create: `src/voicecleaner/tray.h`
- Create: `src/voicecleaner/tray.cpp`

- [ ] **Step 1: Write the header**

Create `src/voicecleaner/tray.h`:

```cpp
#pragma once
#include <windows.h>
#include <shellapi.h>
#include "ui_state.h"

namespace vc::ui {

// Tray command IDs routed via WM_COMMAND.
enum : UINT {
  kTrayCmdEnabled = 100,   // toggle bypass
  kTrayCmdEngineRnn = 101,
  kTrayCmdEngineDfn = 102,
  kTrayCmdOpen = 103,
  kTrayCmdQuit = 104,
};
static const UINT WM_TRAY = WM_APP + 1;   // tray callback message

class Tray {
 public:
  Tray(HINSTANCE hInst, HWND hwnd);
  ~Tray();
  bool add();
  void update(const UiState& s);   // icon tooltip reflects active/bypassed + engine
  void remove();
  void showMenu(const UiState& s); // right-click context menu

 private:
  HINSTANCE hInst_;
  HWND hwnd_;
  NOTIFYICONDATAW nid_{};
  bool added_ = false;
};

}  // namespace vc::ui
```

- [ ] **Step 2: Write the implementation**

Create `src/voicecleaner/tray.cpp`:

```cpp
#include "tray.h"
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace vc::ui {

static std::wstring toW(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

Tray::Tray(HINSTANCE hInst, HWND hwnd) : hInst_(hInst), hwnd_(hwnd) {}
Tray::~Tray() { remove(); }

bool Tray::add() {
  nid_.cbSize = sizeof(nid_);
  nid_.hWnd = hwnd_;
  nid_.uID = 1;
  nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid_.uCallbackMessage = WM_TRAY;
  nid_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wcscpy_s(nid_.szTip, L"VoiceCleaner");
  added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
  return added_;
}

void Tray::update(const UiState& s) {
  if (!added_) return;
  std::wstring tip = L"VoiceCleaner \xE2\x80\x94 " + toW(s.statusLine());
  if (tip.size() > 127) tip.resize(127);
  wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
  nid_.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::remove() {
  if (added_) { Shell_NotifyIconW(NIM_DELETE, &nid_); added_ = false; }
}

void Tray::showMenu(const UiState& s) {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | (s.bypassed() ? 0 : MF_CHECKED) |
                 (s.bypassEnabled() ? 0 : MF_GRAYED), kTrayCmdEnabled, L"Enabled");
  HMENU eng = CreatePopupMenu();
  AppendMenuW(eng, MF_STRING | (s.activeEngine()==0 ? MF_CHECKED : 0), kTrayCmdEngineRnn, L"RNNoise");
  AppendMenuW(eng, MF_STRING | (s.activeEngine()==1 ? MF_CHECKED : 0), kTrayCmdEngineDfn, L"DeepFilterNet3");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)eng, L"Engine");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kTrayCmdOpen, L"Open VoiceCleaner");
  AppendMenuW(m, MF_STRING, kTrayCmdQuit, L"Quit");

  POINT pt; GetCursorPos(&pt);
  SetForegroundWindow(hwnd_);   // required so the menu dismisses on click-away
  TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
  DestroyMenu(m);
}

}  // namespace vc::ui
```

- [ ] **Step 3: Commit (compile verified in Task 14)**

```
git add src/voicecleaner/tray.h src/voicecleaner/tray.cpp
git commit -m "feat(ui): tray icon + context menu (bypass / engine / open / quit)"
```

---

## Task 14: `main.cpp` + exe wiring (single-instance, pre-flight, message pump)

**Files:**
- Create: `src/voicecleaner/main.cpp`
- Modify: `src/voicecleaner/CMakeLists.txt` (add the exe)

- [ ] **Step 1: Write `WinMain`**

Create `src/voicecleaner/main.cpp`:

```cpp
#include <windows.h>
#include <string>
#include "config.h"          // vc::Config
#include "app_window.h"
#include "tray.h"
#include "worker_supervisor.h"
#include "ui_state.h"

using namespace vc::ui;

static const wchar_t* kMutex = L"Local\\voicecleaner-ui";
static UINT g_restoreMsg = 0;
static AppWindow* g_win = nullptr;

// Tray + single-instance-restore routing lives in a subclass-free hook: the window's WndProc handles
// WM_TIMER/WM_PAINT/etc.; here we route WM_COMMAND, WM_TRAY, and the restore broadcast via a thin
// message filter installed as the window's user message handler.
static LRESULT CALLBACK topProc(HWND h, UINT m, WPARAM w, LPARAM l);

static std::wstring moduleDir() {
  wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf);
  size_t slash = p.find_last_of(L'\\');
  return slash == std::wstring::npos ? p : p.substr(0, slash);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
  g_restoreMsg = RegisterWindowMessageW(L"VoiceCleanerRestore");

  // Single instance: if one already runs, broadcast restore and exit.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutex);
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    PostMessageW(HWND_BROADCAST, g_restoreMsg, 0, 0);
    if (mutex) CloseHandle(mutex);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const std::wstring dir = moduleDir();
  const std::wstring cfgPath = dir + L"\\settings.toml";

  UiState state; state.setRenderLabel("CABLE Input");
  WorkerSupervisor sup(dir, cfgPath);
  AppWindow win(hInst, &sup, &state);
  g_win = &win;
  if (!win.create(L"VoiceCleaner")) { CoUninitialize(); return 1; }

  Tray tray(hInst, win.hwnd());
  tray.add();
  win.setTray(&tray);

  // Route WM_COMMAND / WM_TRAY / restore through topProc by chaining the window proc.
  SetWindowLongPtrW(win.hwnd(), GWLP_WNDPROC, 0);   // placeholder; see Step 2 note

  // Pre-flight: config must exist and name both devices, else show NoConfig and do NOT spawn.
  bool spawn = false;
  std::string cfgUtf8(cfgPath.begin(), cfgPath.end());
  try {
    vc::Config cfg = vc::Config::load(cfgUtf8);
    if (cfg.captureDeviceId.empty() || cfg.renderDeviceId.empty()) state.onNoConfig();
    else spawn = true;
  } catch (...) {
    state.onNoConfig();   // missing/malformed settings.toml
  }
  if (spawn) sup.startOrAttach();

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

  tray.remove();
  CoUninitialize();
  if (mutex) CloseHandle(mutex);
  return 0;
}
```

**Design note for the implementer (resolve during Step 1):** the cleanest wiring is to move `WM_COMMAND`, `WM_TRAY`, and the `g_restoreMsg` handling *into* `AppWindow::handle` rather than chaining a second proc. Prefer that: delete the `topProc`/`SetWindowLongPtrW` placeholder above and instead extend `AppWindow::handle` (Task 12) with these cases:

```cpp
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case kTrayCmdEnabled:  onCommandBypass(); break;
        case kTrayCmdEngineRnn: onCommandEngine(0); break;
        case kTrayCmdEngineDfn: onCommandEngine(1); break;
        case kTrayCmdOpen:     showRestore(); break;
        case kTrayCmdQuit:     sup_->shutdown(3000); DestroyWindow(hwnd_); break;
      }
      return 0;
    case WM_TRAY:
      if (LOWORD(l)==WM_LBUTTONUP || LOWORD(l)==WM_LBUTTONDBLCLK) showRestore();
      else if (LOWORD(l)==WM_RBUTTONUP && tray_) tray_->showMenu(*state_);
      return 0;
    default:
      if (msg == RegisterWindowMessageW(L"VoiceCleanerRestore")) { showRestore(); return 0; }
      break;
```

Add `#include "tray.h"` (already present) so the `kTrayCmd*` IDs and `WM_TRAY` are visible in `app_window.cpp`. With this, `main.cpp` needs neither `topProc` nor the `SetWindowLongPtrW` line — remove both. (`WM_CLOSE`/`WM_SYSCOMMAND` already hide to tray; `Quit` is the only path that calls `sup_->shutdown` + `DestroyWindow`.)

- [ ] **Step 2: Apply the design-note wiring**

Remove the `topProc` forward declaration, the `SetWindowLongPtrW(win.hwnd(), GWLP_WNDPROC, 0);` line, and the (unused) `g_win`/`topProc` scaffolding from `main.cpp`. Add the `WM_COMMAND` / `WM_TRAY` / restore-message cases to `AppWindow::handle` in `src/voicecleaner/app_window.cpp` exactly as shown in Step 1's note. Cache the restore message id once (e.g. a static `UINT` in `app_window.cpp` set on first use) rather than calling `RegisterWindowMessageW` per message.

- [ ] **Step 3: Wire the exe into CMake**

Append to `src/voicecleaner/CMakeLists.txt`:

```cmake
# The GUI executable: thin Win32/D2D/tray glue over voicecleaner-core.
add_executable(voicecleaner WIN32
  main.cpp worker_supervisor.cpp app_window.cpp tray.cpp)
target_link_libraries(voicecleaner PRIVATE
  voicecleaner-core win-ipc transport-core
  d2d1 dwrite user32 shell32 ole32)
if (MSVC)
  target_compile_options(voicecleaner PRIVATE /utf-8)
endif()

# Ship settings.toml next to the exe on first build only (never clobber a user-edited file).
add_custom_command(TARGET voicecleaner POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/settings.toml
    -DDST=$<TARGET_FILE_DIR:voicecleaner>/settings.toml
    -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_absent.cmake)

# The UI spawns voicecleaner-worker.exe from its own directory — make sure it is built first
# and its ORT dependency is available alongside.
add_dependencies(voicecleaner voicecleaner-worker)
```

- [ ] **Step 4: Regenerate, build the exe, and verify the glue compiles**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner
```
Expected: `voicecleaner.exe` builds; `settings.toml` appears in `build\src\voicecleaner\Debug\`. Fix any compile errors surfaced now in `worker_supervisor.cpp` / `app_window.cpp` / `tray.cpp` / `main.cpp` (these files were committed unbuilt in Tasks 11-13).

- [ ] **Step 5: Commit**

```
git add src/voicecleaner/main.cpp src/voicecleaner/app_window.cpp src/voicecleaner/CMakeLists.txt
git commit -m "feat(ui): WinMain, single-instance, pre-flight config, tray/command wiring"
```

---

## Task 15: Full regression + manual hardware validation + STATUS update

**Files:**
- Modify: `docs/STATUS.md`

- [ ] **Step 1: Full build + full test suite (regression gate)**

Run:
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```
Expected: PASS — all existing tests (100 + Phase 2c's), the widened `win-ipc` telemetry tests, and the new `voicecleaner-tests` (`UiState.*`, `LivenessTracker.*`, `SpawnArgs.*`). No regressions.

- [ ] **Step 2: DFN3 offline golden (unchanged output gate)**

Confirm the DFN3 offline golden still matches (the only worker change was two telemetry fields). Run the project's DFN3 validation as documented in `docs/dfn3-validation.md`:
```
build\src\offline-cli\Release\offline-cli.exe --engine dfn3 <golden_in.wav> <out.wav>
```
Expected: sample-matches the reference as before (r ≈ 0.9946). If your golden harness is a script, run that instead.

- [ ] **Step 3: Manual hardware checklist (Release build, real devices)**

Build Release, copy your real 48 kHz capture/render IDs into `build\src\voicecleaner\Release\settings.toml` (keep `ring_frames = 16`, `dsp_chain = true`), then launch `voicecleaner.exe` and verify (design §12):

- [ ] UI launches → worker spawns with **no console window** → audio flows to CABLE Input.
- [ ] **Latency is low (tens of ms), NOT ~5 s** — confirms the spawned config uses `ring_frames = 16`, not 1024.
- [ ] Engine switch RNNoise ↔ DeepFilterNet3 via the **window** and the **tray**: instant under `warm_standby = true`, gap-free on-demand under `warm_standby = false`; status reflects it.
- [ ] Bypass toggles cleanly from the window and the tray.
- [ ] With `dsp_chain = false`: the bypass control is disabled with its reason; the engine control still works.
- [ ] Minimize/close hides to tray with audio still flowing; tray left-click restores.
- [ ] **Isolation:** kill the UI in Task Manager → audio keeps flowing; relaunch re-attaches (no second worker) and shows true engine + bypass.
- [ ] Kill the worker → status shows "Worker not running"; **Relaunch** respawns and reconnects.
- [ ] **Quit** (tray/menu) shuts down both processes cleanly; tray icon removed.
- [ ] With blank device IDs: window shows "No microphone configured …" and does **not** spawn.
- [ ] **Headless-correctness:** the worker still runs standalone (`voicecleaner-worker --config <filled>.toml --ring 16`) with no UI.

- [ ] **Step 4: Update STATUS.md**

In `docs/STATUS.md`, update the top "Active work" line to record Phase 3 complete: `voicecleaner.exe` (Direct2D window + tray) spawns/attaches/supervises the worker, drives engine switch + bypass over the pipe, reflects true bypass/dspChain from telemetry; shipped `settings.toml` uses `ring_frames = 16`. Note the new `voicecleaner-tests` target and the widened `Telemetry` POD. Bump `_Last updated_` to 2026-08-16.

- [ ] **Step 5: Commit**

```
git add docs/STATUS.md
git commit -m "docs(status): Phase 3 UI skeleton complete"
```

---

## Self-Review

**Spec coverage** (design §-by-§):
- §2 locked decisions — Direct2D from start (Task 12); reads `settings.toml`, no forced overrides (Task 14 spawn path + Task 9 builder); `ring_frames = 16` (Task 10); warm standby respected not forced (worker unchanged; Task 14 passes no flags); params seeded from `Params::defaults()` and synced from telemetry (Tasks 3, 7); lifecycle spawn+attach+detect gated on first `seq` advance (Tasks 8, 11); telemetry additions (Task 1). ✓
- §3 control availability — engine always enabled (no gating in `UiState`/paint); bypass gated by `dspChain` (Tasks 5, 12). ✓
- §4 new target + file responsibilities — all 5 units + core split (Tasks 3, 11-14). ✓
- §5 state & IPC wiring — optimistic `current_` mutate + `SetParams`; `current_.engine` synced to `activeEngine` (Task 7); WM_TIMER 10 Hz (Task 12). ✓
- §6 telemetry additions — `bypass` + `dspChain` (Tasks 1, 2). ✓
- §7 worker change — only the two telemetry writes (Task 2). ✓
- §8 window skeleton — status line, engine segmented, bypass toggle, relaunch, first-run message (Tasks 3-6, 12, 14). ✓
- §9 tray — icon/tooltip, left-click restore, right-click menu with bypass/engine/open/quit (Tasks 13, 14). ✓
- §10 lifecycle — single-instance + restore broadcast (Task 14); present-vs-ready on first `seq` advance, probe attach/spawn, bounded ~5 s wait (readyTimeout), death detection (stall/shm/process), quit/shutdown (Tasks 8, 11, 14). ✓
- §11 error handling — no config/devices → NoConfig (Tasks 6, 14); launch failure via readyTimeout `everReady()` (Tasks 8, 11); pipe-send failure not fatal (Task 11 `send` comment); torn reads tolerated (existing reader); DeviceLost via `workerState` → down. ✓ *(Note: the launch-failure message hint about the 48 kHz requirement (§11) is surfaced by the supervisor's `launchFailure` flag; the window shows the generic Down/Relaunch state — the explicit 48 kHz hint text is a nicety the implementer may add to the status area when `PollResult::launchFailure` is set. Recorded here so it is not silently dropped.)*
- §12 testing — pure `ui_state` units, engine-sync invariant, spawn-arg, liveness/readiness (Tasks 3-9); widened telemetry round-trip (Task 1); regression + golden + manual (Task 15). ✓
- §13 deliverables — new src + tests targets, widened Telemetry + relay writes + updated tests, `settings.toml` ring_frames=16, no regression (all tasks). ✓
- §14 forward notes — Phase 4 only; not built. ✓

**Placeholder scan:** no TBD/TODO; every code step shows complete code. The one deferred item (48 kHz hint text) is explicitly called out as optional above, not left as a silent placeholder.

**Type consistency:** `UiState`/`LivenessTracker`/`buildWorkerCommandLine` signatures match across Tasks 3/7/8/9 and their consumers in `worker_supervisor.cpp` (Task 11) and `app_window.cpp` (Task 12). `PollResult` fields (`alive`/`ready`/`launchFailure`/`telemetry`) are defined once (Task 11) and consumed in `tick()` (Task 12). Telemetry field names `bypass`/`dspChain` are identical in Tasks 1, 2, 3, 5. Command/Params usage matches the existing `vc::ipc` API (`CmdType::SetParams`, `Command::params`).
