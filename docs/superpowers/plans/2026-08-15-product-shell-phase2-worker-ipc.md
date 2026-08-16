# Product Shell — Phase 2 (Worker IPC) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the audio worker a control plane — a `Params` double-buffer, a named-pipe command server, and a shared-memory telemetry block — and wire it into the `Bridge`, so a future UI can tune/switch/select-device and read meters over IPC, while the worker still runs headless.

**Architecture:** A new `win-ipc` static lib (Windows-only) holds `PipeServer`/`PipeClient` (message-mode named pipe carrying `vc-ipc` `Command` frames) and `TelemetryShm` (a `CreateFileMapping`-backed `Telemetry` block). `vc-ipc` gains a header-only `ParamStore` (blueprint §8.3 double-buffer). The worker (`phase0-app`, renamed `voicecleaner-worker`) runs a `PipeServer` on a non-RT thread that translates commands into `ParamStore::publish` / `request_engine` / `seedPpm` / device-rebuild / shutdown; the relay reads the `Params` snapshot each hop and pushes changed values into `DspChain`, and publishes `Telemetry` ~1 Hz (meter fields per-frame). The RT thread never touches the pipe or shared memory beyond one relaxed store.

**Tech Stack:** C++20, MSVC, CMake (ESP-IDF-bundled), GoogleTest, Win32 (named pipes, file mapping).

**Build/test commands** (CMake not on PATH):
- Configure: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64`
- Build (Debug): `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
- Test (filter): `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R <regex>`

**Deferred (NOT in this plan):**
- **Live aggression** (mask-shaping inside `Dfn3Streaming`) — own follow-up (Phase 2b); the adapter forwards frames and cannot shape the mask without engine-internal changes.
- The UI process (`voicecleaner.exe`) — Phase 3. This plan only produces IPC that a UI will later drive; verification of Tasks 6–7 is a **manual K688 smoke test** plus the unit suite staying green.

---

## File Structure

- **Create** `src/vc-ipc/param_store.h` — `vc::ipc::ParamStore` (double-buffered `Params`, atomic flip).
- **Create** `src/win-ipc/pipe.h` + `pipe.cpp` — `PipeServer` (non-RT reader thread + callback), `PipeClient`.
- **Create** `src/win-ipc/telemetry_shm.h` + `telemetry_shm.cpp` — `TelemetryWriter`, `TelemetryReader`.
- **Create** `src/win-ipc/CMakeLists.txt`; **Create** `tests/win-ipc/{CMakeLists.txt,test_pipe.cpp,test_telemetry_shm.cpp}`.
- **Create** `tests/vc-ipc/test_param_store.cpp` (added to the existing `tests/vc-ipc` target).
- **Modify** root `CMakeLists.txt` — add `src/win-ipc`, `tests/win-ipc`.
- **Rename** `src/phase0-app/` → `src/voicecleaner-worker/`; target `phase0-app` → `voicecleaner-worker`.
- **Modify** the worker `bridge.{h,cpp}` and `main.cpp` — wire `ParamStore`, `TelemetryWriter`, `PipeServer`, device rebuild.

---

## Task 1: `ParamStore` double-buffer (vc-ipc, header-only)

**Files:**
- Create: `src/vc-ipc/param_store.h`
- Create: `tests/vc-ipc/test_param_store.cpp`
- Modify: `tests/vc-ipc/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/vc-ipc/test_param_store.cpp`:

```cpp
#include <gtest/gtest.h>
#include "param_store.h"
using vc::ipc::ParamStore; using vc::ipc::Params;

TEST(ParamStore, SnapshotReturnsLastPublished) {
  ParamStore s;                                 // starts at Params::defaults()
  EXPECT_FLOAT_EQ(s.snapshot().hpfCutoffHz, 80.0f);
  Params p = Params::defaults(); p.hpfCutoffHz = 150.0f; p.bypass = 1;
  s.publish(p);
  EXPECT_FLOAT_EQ(s.snapshot().hpfCutoffHz, 150.0f);
  EXPECT_EQ(s.snapshot().bypass, 1u);
}
TEST(ParamStore, LastPublishWins) {
  ParamStore s;
  Params p = Params::defaults();
  for (uint32_t i = 1; i <= 100; ++i) { p.engine = i % 2; s.publish(p); }
  EXPECT_EQ(s.snapshot().engine, 0u);           // 100 % 2
}
```

- [ ] **Step 2: Add the test to CMake**

Edit `tests/vc-ipc/CMakeLists.txt` — add `test_param_store.cpp` to the `vc-ipc-tests` sources:

```cmake
add_executable(vc-ipc-tests test_params.cpp test_command.cpp test_param_store.cpp)
target_link_libraries(vc-ipc-tests PRIVATE vc-ipc GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(vc-ipc-tests)
```

- [ ] **Step 3: Run to verify it fails**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64` then build target `vc-ipc-tests`.
Expected: compile error — `param_store.h` not found.

- [ ] **Step 4: Implement `ParamStore`**

Create `src/vc-ipc/param_store.h`:

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include "params.h"
namespace vc::ipc {
// Double-buffered parameter snapshot with an atomic index flip (blueprint §8.3).
// Writer (non-RT command thread) calls publish(); reader (RT thread) calls snapshot() wait-free.
class ParamStore {
public:
  ParamStore() { slot_[0] = Params::defaults(); slot_[1] = Params::defaults(); }
  void publish(const Params& p) noexcept {
    const uint32_t next = 1u - active_.load(std::memory_order_relaxed);
    slot_[next] = p;                                   // plain store into the inactive slot
    active_.store(next, std::memory_order_release);    // publish
  }
  Params snapshot() const noexcept {
    return slot_[active_.load(std::memory_order_acquire)];   // one acquire load, wait-free
  }
private:
  Params slot_[2];
  std::atomic<uint32_t> active_{0};
};
}  // namespace vc::ipc
```

- [ ] **Step 5: Build + test**

Build `vc-ipc-tests`, then:
`"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R "ParamStore"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/vc-ipc/param_store.h tests/vc-ipc/test_param_store.cpp tests/vc-ipc/CMakeLists.txt
git commit -m "feat(vc-ipc): ParamStore double-buffered snapshot (RT-safe publish/snapshot)"
```

---

## Task 2: `win-ipc` lib — `PipeServer` / `PipeClient`

**Files:**
- Create: `src/win-ipc/pipe.h`, `src/win-ipc/pipe.cpp`, `src/win-ipc/CMakeLists.txt`
- Create: `tests/win-ipc/test_pipe.cpp`, `tests/win-ipc/CMakeLists.txt`
- Modify: root `CMakeLists.txt`

**Design:** message-mode inbound named pipe (UI writes, worker reads). Each `WriteFile` from the client
is one message; each `ReadFile` on the server returns one message → the `vc-ipc` codec decodes it. The
server owns a reader thread using **overlapped `ConnectNamedPipe`** plus a stop event so `stop()` is clean.

- [ ] **Step 1: Write the header**

Create `src/win-ipc/pipe.h`:

```cpp
#pragma once
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include "command.h"
namespace vc::ipc {
// Inbound message-mode named-pipe server. Runs a non-RT reader thread; decodes each message with the
// vc-ipc codec and invokes onCommand on that thread. One client at a time (the UI).
class PipeServer {
public:
  using Handler = std::function<void(const Command&)>;
  PipeServer() = default;
  ~PipeServer();
  bool start(const std::wstring& name, Handler onCommand);  // false if the pipe can't be created
  void stop();
private:
  void run();
  std::wstring name_;
  Handler handler_;
  std::thread th_;
  void* stopEvent_ = nullptr;   // HANDLE (manual-reset)
  std::atomic<bool> running_{false};
};
// Client: connect + send one encoded Command per call. Returns false if no server / write fails.
class PipeClient {
public:
  static bool send(const std::wstring& name, const Command& c);
};
}  // namespace vc::ipc
```

- [ ] **Step 2: Write the failing round-trip test**

Create `tests/win-ipc/test_pipe.cpp`:

```cpp
#include <gtest/gtest.h>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>
#include "pipe.h"
using namespace vc::ipc;

TEST(PipeServer, ReceivesEncodedCommandFromClient) {
  const std::wstring name = L"\\\\.\\pipe\\voicecleaner-test-1";
  std::mutex m; std::condition_variable cv; std::vector<Command> got;
  PipeServer srv;
  ASSERT_TRUE(srv.start(name, [&](const Command& c){
    { std::lock_guard<std::mutex> lk(m); got.push_back(c); } cv.notify_all();
  }));

  Command out; out.type = CmdType::SelectCaptureDevice;
  out.deviceId = "{dev-id}"; out.driftSeedPpm = -26.0;
  // The server may need a moment to reach ConnectNamedPipe; retry the client briefly.
  bool sent = false;
  for (int i = 0; i < 50 && !sent; ++i) {
    sent = PipeClient::send(name, out);
    if (!sent) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(sent);

  std::unique_lock<std::mutex> lk(m);
  ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(2), [&]{ return !got.empty(); }));
  EXPECT_EQ(got[0].type, CmdType::SelectCaptureDevice);
  EXPECT_EQ(got[0].deviceId, "{dev-id}");
  EXPECT_DOUBLE_EQ(got[0].driftSeedPpm, -26.0);
  srv.stop();
}
```

- [ ] **Step 3: Create CMake + wire the build**

Create `src/win-ipc/CMakeLists.txt`:

```cmake
add_library(win-ipc pipe.cpp telemetry_shm.cpp)
target_include_directories(win-ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(win-ipc PUBLIC vc-ipc)
```
> Note: `telemetry_shm.cpp` is created in Task 3. For Task 2 alone, temporarily list only `pipe.cpp`
> here, and add `telemetry_shm.cpp` in Task 3 Step 3.

Create `tests/win-ipc/CMakeLists.txt`:

```cmake
add_executable(win-ipc-tests test_pipe.cpp)
target_link_libraries(win-ipc-tests PRIVATE win-ipc GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(win-ipc-tests)
```
> Note: Task 3 adds `test_telemetry_shm.cpp` here.

In root `CMakeLists.txt`, after `add_subdirectory(src/vc-ipc)` add `add_subdirectory(src/win-ipc)`,
and after `add_subdirectory(tests/vc-ipc)` add `add_subdirectory(tests/win-ipc)`.

- [ ] **Step 4: Implement `pipe.cpp`**

Create `src/win-ipc/pipe.cpp`:

```cpp
#include "pipe.h"
#include <windows.h>
#include <vector>
namespace vc::ipc {

PipeServer::~PipeServer() { stop(); }

bool PipeServer::start(const std::wstring& name, Handler onCommand) {
  name_ = name; handler_ = std::move(onCommand);
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual-reset
  if (!stopEvent_) return false;
  running_.store(true);
  th_ = std::thread([this]{ run(); });
  return true;
}

void PipeServer::run() {
  while (running_.load()) {
    HANDLE pipe = CreateNamedPipeW(
        name_.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 0, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;

    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = ConnectNamedPipe(pipe, &ov);
    DWORD err = GetLastError();
    if (!ok && err == ERROR_IO_PENDING) {
      HANDLE hs[2] = { ov.hEvent, static_cast<HANDLE>(stopEvent_) };
      DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
      if (w != WAIT_OBJECT_0) { CloseHandle(ov.hEvent); CloseHandle(pipe); break; }
    } else if (!ok && err != ERROR_PIPE_CONNECTED) {
      CloseHandle(ov.hEvent); CloseHandle(pipe);
      if (!running_.load()) break; else continue;
    }
    CloseHandle(ov.hEvent);

    // Read messages until the client disconnects.
    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
      DWORD n = 0;
      BOOL r = ReadFile(pipe, buf.data(), (DWORD)buf.size(), &n, nullptr);
      if (!r || n == 0) break;
      Command c;
      if (decode(buf.data(), n, c) && handler_) handler_(c);
      if (!running_.load()) break;
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

void PipeServer::stop() {
  if (!running_.exchange(false)) { if (th_.joinable()) th_.join(); return; }
  if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
  if (th_.joinable()) th_.join();
  if (stopEvent_) { CloseHandle(static_cast<HANDLE>(stopEvent_)); stopEvent_ = nullptr; }
}

bool PipeClient::send(const std::wstring& name, const Command& c) {
  HANDLE pipe = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  auto bytes = encode(c);
  DWORD written = 0;
  BOOL ok = WriteFile(pipe, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
  CloseHandle(pipe);
  return ok && written == bytes.size();
}
}  // namespace vc::ipc
```

- [ ] **Step 5: Build + test**

Configure + build `win-ipc-tests`, then:
`"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R "PipeServer"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/win-ipc tests/win-ipc CMakeLists.txt
git commit -m "feat(win-ipc): message-mode named-pipe command server + client"
```

---

## Task 3: `TelemetryWriter` / `TelemetryReader` (shared memory)

**Files:**
- Create: `src/win-ipc/telemetry_shm.h`, `src/win-ipc/telemetry_shm.cpp`
- Create: `tests/win-ipc/test_telemetry_shm.cpp`
- Modify: `src/win-ipc/CMakeLists.txt`, `tests/win-ipc/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `src/win-ipc/telemetry_shm.h`:

```cpp
#pragma once
#include <string>
#include "telemetry.h"
namespace vc::ipc {
// Worker side: owns a named file-mapping and writes the Telemetry block (single writer, relaxed).
class TelemetryWriter {
public:
  ~TelemetryWriter();
  bool create(const std::wstring& name);   // false on failure
  void write(const Telemetry& t) noexcept; // memcpy into the mapped view
private:
  void* handle_ = nullptr;   // HANDLE
  Telemetry* view_ = nullptr;
};
// UI side: opens the same mapping read-only and copies the block out.
class TelemetryReader {
public:
  ~TelemetryReader();
  bool open(const std::wstring& name);     // false if the mapping doesn't exist
  Telemetry read() const noexcept;         // returns a copy (torn reads tolerated)
private:
  void* handle_ = nullptr;
  const Telemetry* view_ = nullptr;
};
}  // namespace vc::ipc
```

- [ ] **Step 2: Write the failing test**

Create `tests/win-ipc/test_telemetry_shm.cpp`:

```cpp
#include <gtest/gtest.h>
#include "telemetry_shm.h"
using namespace vc::ipc;

TEST(TelemetryShm, WriterToReaderRoundTrip) {
  const std::wstring name = L"Local\\voicecleaner-test-telemetry";
  TelemetryWriter w; ASSERT_TRUE(w.create(name));
  TelemetryReader r; ASSERT_TRUE(r.open(name));
  Telemetry t{}; t.inputRms = 0.25f; t.dropouts = 3; t.activeEngine = 1;
  t.driftRatioPpm = -26.0; t.seq = 7;
  w.write(t);
  Telemetry got = r.read();
  EXPECT_FLOAT_EQ(got.inputRms, 0.25f);
  EXPECT_EQ(got.dropouts, 3u);
  EXPECT_EQ(got.activeEngine, 1u);
  EXPECT_DOUBLE_EQ(got.driftRatioPpm, -26.0);
  EXPECT_EQ(got.seq, 7u);
}
TEST(TelemetryShm, ReaderFailsWhenMappingAbsent) {
  TelemetryReader r; EXPECT_FALSE(r.open(L"Local\\voicecleaner-test-absent"));
}
```

- [ ] **Step 3: Register the new files in CMake**

In `src/win-ipc/CMakeLists.txt`, set the sources to `pipe.cpp telemetry_shm.cpp` (if Task 2 listed only
`pipe.cpp`). In `tests/win-ipc/CMakeLists.txt`, set the test sources to
`test_pipe.cpp test_telemetry_shm.cpp`.

- [ ] **Step 4: Implement `telemetry_shm.cpp`**

Create `src/win-ipc/telemetry_shm.cpp`:

```cpp
#include "telemetry_shm.h"
#include <windows.h>
#include <cstring>
namespace vc::ipc {

TelemetryWriter::~TelemetryWriter() {
  if (view_) UnmapViewOfFile(view_);
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}
bool TelemetryWriter::create(const std::wstring& name) {
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                0, sizeof(Telemetry), name.c_str());
  if (!h) return false;
  void* v = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(Telemetry));
  if (!v) { CloseHandle(h); return false; }
  handle_ = h; view_ = static_cast<Telemetry*>(v);
  std::memset(view_, 0, sizeof(Telemetry));
  return true;
}
void TelemetryWriter::write(const Telemetry& t) noexcept {
  if (view_) std::memcpy(view_, &t, sizeof(Telemetry));
}

TelemetryReader::~TelemetryReader() {
  if (view_) UnmapViewOfFile(const_cast<Telemetry*>(view_));
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}
bool TelemetryReader::open(const std::wstring& name) {
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  if (!h) return false;
  void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(Telemetry));
  if (!v) { CloseHandle(h); return false; }
  handle_ = h; view_ = static_cast<const Telemetry*>(v);
  return true;
}
Telemetry TelemetryReader::read() const noexcept {
  Telemetry t{};
  if (view_) std::memcpy(&t, view_, sizeof(Telemetry));
  return t;
}
}  // namespace vc::ipc
```

- [ ] **Step 5: Build + test**

Configure + build `win-ipc-tests`, then:
`"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R "TelemetryShm"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/win-ipc/telemetry_shm.h src/win-ipc/telemetry_shm.cpp tests/win-ipc/test_telemetry_shm.cpp src/win-ipc/CMakeLists.txt tests/win-ipc/CMakeLists.txt
git commit -m "feat(win-ipc): shared-memory Telemetry writer/reader"
```

---

## Task 4: Rename `phase0-app` → `voicecleaner-worker`

**Files:**
- Rename: `src/phase0-app/` → `src/voicecleaner-worker/` (git mv all files)
- Modify: `src/voicecleaner-worker/CMakeLists.txt`, root `CMakeLists.txt`

- [ ] **Step 1: git-mv the directory**

```bash
git mv src/phase0-app src/voicecleaner-worker
```

- [ ] **Step 2: Update the target name**

In `src/voicecleaner-worker/CMakeLists.txt`, replace every `phase0-app` with `voicecleaner-worker`:

```cmake
add_executable(voicecleaner-worker main.cpp bridge.cpp logger.cpp)
target_link_libraries(voicecleaner-worker PRIVATE transport-core win-audio denoise-core dfn3-engine)
add_custom_command(TARGET voicecleaner-worker POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_PROPERTY:onnxruntime,IMPORTED_LOCATION>" "$<TARGET_FILE_DIR:voicecleaner-worker>")
```

- [ ] **Step 3: Update the root CMake subdir**

In root `CMakeLists.txt`, change `add_subdirectory(src/phase0-app)` to
`add_subdirectory(src/voicecleaner-worker)`.

- [ ] **Step 4: Configure, build, confirm the exe**

Run configure + `--build build --config Debug --target voicecleaner-worker`.
Expected: builds `build/src/voicecleaner-worker/Debug/voicecleaner-worker.exe`.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor: rename phase0-app -> voicecleaner-worker"
```

---

## Task 5: Add `win-ipc` to the worker and publish telemetry from `Bridge`

**Files:**
- Modify: `src/voicecleaner-worker/CMakeLists.txt` (link `win-ipc`)
- Modify: `src/voicecleaner-worker/bridge.h`, `bridge.cpp`

**Goal:** `Bridge` owns a `ParamStore` and a `TelemetryWriter`; the relay reads the snapshot each hop
and applies it to `DspChain`, and publishes `Telemetry` (~1 Hz aggregates; meter fields per hop). No
pipe yet — this task is verified by the build + existing suite staying green (no behavior change at
default params, since the snapshot starts at `Params::defaults()`).

- [ ] **Step 1: Link `win-ipc`**

In `src/voicecleaner-worker/CMakeLists.txt`, add `win-ipc` to `target_link_libraries` (which pulls in
`vc-ipc`):
```cmake
target_link_libraries(voicecleaner-worker PRIVATE transport-core win-audio denoise-core dfn3-engine win-ipc)
```

- [ ] **Step 2: Add members to `bridge.h`**

Add includes and members. After `#include "dsp_chain.h"` add:
```cpp
#include "param_store.h"
#include "telemetry_shm.h"
```
Add a public accessor + private members (near the other private members):
```cpp
 public:
  vc::ipc::ParamStore& params() noexcept { return params_; }   // command thread publishes here
 private:
  vc::ipc::ParamStore   params_;
  vc::ipc::TelemetryWriter telemetry_;
  bool                  telemetryOn_ = false;
  uint32_t              telemetrySeq_ = 0;
  int                   hopsSincePublish_ = 0;
```
(Keep the existing `public:`/`private:` layout coherent — merge these into the existing sections.)

- [ ] **Step 3: Open the telemetry mapping in `start()`**

In `bridge.cpp` `start()`, after `logger_.start();`, add:
```cpp
  telemetryOn_ = telemetry_.create(L"Local\\voicecleaner-telemetry");
```

- [ ] **Step 4: Apply params + publish telemetry in `relayLoop()`**

In `relayLoop()`, at the top of the `while (ringA_.try_pop(f))` body (before the compute), read the
snapshot and push to the chain:
```cpp
      const vc::ipc::Params sp = params_.snapshot();
      if (chain_) {
        chain_->setHpfCutoffHz(sp.hpfCutoffHz);
        chain_->setAgcTargetDb(sp.agcTargetDbfs);
        chain_->setLimiterCeilingDb(sp.limiterCeilingDbfs);
        chain_->setBypass(sp.bypass != 0);
      }
```
After the `frameStats_.record(...)` line, add per-hop meter + ~1 Hz aggregate publish:
```cpp
      if (telemetryOn_) {
        float rms = 0.0f, peak = 0.0f;
        for (size_t i = 0; i < FRAME_SAMPLES; ++i) { float a = src[i] < 0 ? -src[i] : src[i];
          rms += src[i]*src[i]; if (a > peak) peak = a; }
        rms = std::sqrt(rms / (float)FRAME_SAMPLES);
        if (++hopsSincePublish_ >= 100) {   // ~1 s at 10 ms/hop
          hopsSincePublish_ = 0;
          vc::ipc::Telemetry t{};
          t.inputRms = rms; t.inputPeak = peak;
          t.vadActive = (chain_ && chain_->vadActive()) ? 1u : 0u;
          t.frameP50Ms = (float)frameStats_.percentileMs(50.0);
          t.frameP99Ms = (float)frameStats_.percentileMs(99.0);
          t.frameMaxMs = (float)frameStats_.maxMs();
          t.ringBOcc = (uint32_t)ringB_.size();
          t.dropouts = (uint32_t)dropouts_.overruns();
          t.activeEngine = switch_ ? (uint32_t)switch_->activeIndex()
                                   : (cfg_.engine == "dfn3" ? 1u : 0u);
          t.driftRatioPpm = ratioPpm_.load(std::memory_order_relaxed);
          t.workerState = (uint32_t)vc::ipc::WorkerState::Running;
          t.seq = ++telemetrySeq_;
          telemetry_.write(t);
        }
      }
```
> The subagent MUST confirm the exact accessor names against the source: `dropouts_.overruns()` (see
> `src/transport-core/dropout.h`) and `switch_->activeIndex()` (see `switchable_engine.h`, already used
> in `bridge.cpp`). If `DropoutCounter` exposes a different getter, use that name. `<cmath>` is already
> included transitively via headers; if `std::sqrt` fails to compile, add `#include <cmath>` to bridge.cpp.

- [ ] **Step 5: Build + full suite (regression guard)**

Configure + build all, then run the entire suite:
`"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure`
Expected: all tests pass (no behavior change at default params).

- [ ] **Step 6: Commit**

```bash
git add src/voicecleaner-worker
git commit -m "feat(worker): Bridge owns ParamStore + publishes Telemetry shm"
```

---

## Task 6: Pipe command server in `main.cpp` (params / engine / seed / shutdown)

**Files:**
- Modify: `src/voicecleaner-worker/main.cpp`

**Goal:** replace the interim stdin `1`/`2` reader with a `PipeServer`. Commands map to: `SetParams`
→ `bridge.params().publish(p)`; `SetWarmStandby`/engine selection → `bridge.request_engine(idx)`;
`Shutdown` → set `g_stop`. Device selection is Task 7. Keep stdin keys behind a `--dev-keys` flag.

- [ ] **Step 1: Include the pipe header + add the flag**

At the top of `main.cpp` add `#include "pipe.h"`. Parse a `--dev-keys` bool flag alongside the existing
flags (default false).

- [ ] **Step 2: Start the pipe server after `bridge.start()`**

Replace the `if (cfg.warmStandby) { switchThread = ... }` stdin block with:
```cpp
    vc::ipc::PipeServer pipe;
    pipe.start(L"\\\\.\\pipe\\voicecleaner", [&bridge](const vc::ipc::Command& c){
      switch (c.type) {
        case vc::ipc::CmdType::SetParams:
          bridge.params().publish(c.params);
          bridge.request_engine((int)c.params.engine);   // no-op unless warm-standby
          break;
        case vc::ipc::CmdType::SetWarmStandby:
          break;   // build-time toggle; handled on next start (Task 7 rebuild)
        case vc::ipc::CmdType::Shutdown:
          g_stop.store(true);
          break;
        default: break;   // Select*Device handled in Task 7
      }
    });
```
Keep the stdin reader, but only spawn it when `devKeys` is set (dev fallback).

- [ ] **Step 3: Stop the pipe on shutdown**

Before `bridge.stop();` (or right after the main loop), add `pipe.stop();`.

- [ ] **Step 4: Build + smoke (headless correctness)**

Build `voicecleaner-worker`. Run the full unit suite (unchanged): all green.
Manual (optional, needs devices): `voicecleaner-worker.exe --config drift-warmstandby.toml` still runs
headless; no UI required (the pipe server simply waits for a client).

- [ ] **Step 5: Commit**

```bash
git add src/voicecleaner-worker/main.cpp
git commit -m "feat(worker): named-pipe command server (params/engine/shutdown); stdin keys behind --dev-keys"
```

---

## Task 7: Live device re-select (pipeline rebuild)

**Files:**
- Modify: `src/voicecleaner-worker/bridge.h`, `bridge.cpp`, `main.cpp`

**Goal:** handle `SelectCaptureDevice` / `SelectRenderDevice` by rebuilding the pipeline on the pipe's
non-RT thread: `stop()` → update `cfg_.captureDeviceId`/`renderDeviceId` (and seed drift for capture)
→ `start()`. The RT threads are torn down and recreated cleanly; never mutated live.

- [ ] **Step 1: Add a rebuild method to `Bridge`**

In `bridge.h`, add public:
```cpp
  bool reselectCapture(const std::string& deviceId, double driftSeedPpm);
  bool reselectRender(const std::string& deviceId);
```
In `bridge.cpp`, implement (they stop, swap the id, seed drift, restart):
```cpp
bool Bridge::reselectCapture(const std::string& deviceId, double driftSeedPpm) {
  stop();
  cfg_.captureDeviceId = deviceId;
  running_.store(false, std::memory_order_release);
  bool ok = start();
  if (ok && cfg_.driftCompensation) controller_.seedPpm(driftSeedPpm);
  return ok;
}
bool Bridge::reselectRender(const std::string& deviceId) {
  stop();
  cfg_.renderDeviceId = deviceId;
  bool ok = start();
  return ok;
}
```
> The subagent MUST verify `controller_` survives across `stop()`/`start()` (it is a `Bridge` member,
> constructed once in the ctor — so `seedPpm` after `start()` is correct and persists into the relay).
> If `start()` reconstructs controller state, seed inside `start()` instead. Confirm against `bridge.cpp`.

- [ ] **Step 2: Route the device commands in `main.cpp`**

Add to the pipe handler switch:
```cpp
        case vc::ipc::CmdType::SelectCaptureDevice:
          bridge.reselectCapture(c.deviceId, c.driftSeedPpm);
          break;
        case vc::ipc::CmdType::SelectRenderDevice:
          bridge.reselectRender(c.deviceId);
          break;
```

- [ ] **Step 3: Build + full suite**

Build all; run the entire unit suite — all green (no unit test drives the pipeline, so this is a
compile + no-regression check).

- [ ] **Step 4: Commit**

```bash
git add src/voicecleaner-worker
git commit -m "feat(worker): live capture/render device re-select (pipeline rebuild + drift seed)"
```

---

## Final verification

- [ ] **Whole suite green**

`"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure`
Expected: original 93 + `ParamStore` + `PipeServer` + `TelemetryShm` all pass.

- [ ] **Manual K688 smoke (user-run, not automatable here)**

1. `voicecleaner-worker.exe --config drift-warmstandby.toml` — runs headless, audio flows to CABLE Input.
2. From a throwaway client (or the future UI), send a `SetParams` with `bypass=1` → audio passes
   through uncleaned; `bypass=0` → cleaned again. Send `engine=0/1` under warm standby → engine switches.
3. Confirm zero dropouts and the frame-time summary on exit is unchanged from Phase 1 baselines.

This manual step validates the live wiring; the automated suite only guarantees the IPC building blocks
and no unit regressions.

---

## What Phase 2 delivers / what's next

**Delivered:** `ParamStore` (§8.3), `win-ipc` (`PipeServer`/`PipeClient`, `TelemetryWriter`/`Reader`),
worker wired for live params + telemetry + engine switch + device re-select over a named pipe, worker
renamed. **Deferred:** live aggression (Phase 2b, engine-internal); the UI process (Phase 3) that
drives this IPC and reads the telemetry block.
