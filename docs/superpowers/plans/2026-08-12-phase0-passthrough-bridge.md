# Phase 0 — Instrumented Pass-Through Bridge — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a headless Windows audio bridge (WASAPI capture → lock-free rings → WASAPI render into VB-Cable) with no DSP, instrumented to prove zero dropouts over 8 hours and measure capture/render clock drift.

**Architecture:** Three CMake targets. `transport-core` holds platform-agnostic, unit-tested logic (lock-free ring, bidirectional framer, channel extraction, occupancy/dropout stats, config). `win-audio` wraps WASAPI (capture, render, device enumeration, MMCSS, RT memory locking). `phase0-app` wires them into the four-thread topology (capture → Ring A → MMCSS relay → Ring B → render) with a non-RT logger. See the spec: `docs/superpowers/specs/2026-08-12-phase0-passthrough-bridge-design.md`.

**Tech Stack:** C++20, MSVC, CMake, WASAPI (shared-mode event-driven), MMCSS ("Pro Audio"), GoogleTest, toml++.

---

## File Structure

```
VoiceCleaner/
├── CMakeLists.txt                         # top-level: fetches deps, adds targets
├── .gitignore
├── cmake/                                 # (reserved)
├── src/
│   ├── transport-core/
│   │   ├── CMakeLists.txt
│   │   ├── frame.h                        # FRAME_SAMPLES, Frame alias
│   │   ├── spsc_ring.h                    # SpscRing<T> (header-only, templated)
│   │   ├── framer.h / framer.cpp          # FrameAccumulator, FrameDistributor
│   │   ├── channel.h / channel.cpp        # extract_channel, mono_to_interleaved
│   │   ├── occupancy.h / occupancy.cpp    # OccupancyStats
│   │   ├── dropout.h / dropout.cpp        # DropoutCounter
│   │   └── config.h / config.cpp          # Config load + CLI override
│   ├── win-audio/
│   │   ├── CMakeLists.txt
│   │   ├── com_guard.h                     # CoInitialize RAII
│   │   ├── device_enumerator.h/.cpp        # list + resolve endpoints, format guard
│   │   ├── mmcss_thread.h/.cpp             # AvSetMmThreadCharacteristics RAII
│   │   ├── rt_memory.h/.cpp                # VirtualLock + pre-touch
│   │   ├── wasapi_capture.h/.cpp           # event-driven capture
│   │   └── wasapi_render.h/.cpp            # event-driven render
│   └── phase0-app/
│       ├── CMakeLists.txt
│       ├── main.cpp                        # CLI dispatch, wiring, shutdown
│       ├── bridge.h/.cpp                   # thread orchestration + priming
│       └── logger.h/.cpp                   # non-RT CSV + console telemetry
└── tests/
    └── transport-core/
        ├── CMakeLists.txt
        ├── test_spsc_ring.cpp
        ├── test_framer.cpp
        ├── test_channel.cpp
        ├── test_occupancy.cpp
        ├── test_dropout.cpp
        └── test_config.cpp
```

**Interface contracts (defined once here, referenced by every task):**

```cpp
// frame.h
namespace vc {
inline constexpr size_t FRAME_SAMPLES = 480;   // 10 ms @ 48 kHz, mono
using Frame = std::array<float, FRAME_SAMPLES>;
}
```

---

## Task 0: Project scaffolding

**Files:**
- Create: `.gitignore`, `CMakeLists.txt`, `src/transport-core/CMakeLists.txt`,
  `src/win-audio/CMakeLists.txt`, `src/phase0-app/CMakeLists.txt`,
  `tests/transport-core/CMakeLists.txt`, `src/transport-core/frame.h`

- [ ] **Step 1: Initialize git and .gitignore**

```bash
cd "I:/Coding-Projects/VoiceCleaner"
git init
```

`.gitignore`:
```
/build/
*.user
.vs/
/out/
*.csv
```

- [ ] **Step 2: Top-level `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(VoiceCleaner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (MSVC)
  add_compile_options(/W4 /permissive-)
endif()

include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0)
FetchContent_MakeAvailable(tomlplusplus)

enable_testing()
add_subdirectory(src/transport-core)
add_subdirectory(src/win-audio)
add_subdirectory(src/phase0-app)
add_subdirectory(tests/transport-core)
```

- [ ] **Step 3: `src/transport-core/frame.h`** — exactly the `frame.h` contract above (wrap with `#pragma once` and `#include <array>`, `#include <cstddef>`).

- [ ] **Step 4: `src/transport-core/CMakeLists.txt`**

```cmake
add_library(transport-core
  framer.cpp channel.cpp occupancy.cpp dropout.cpp config.cpp)
target_include_directories(transport-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(transport-core PUBLIC tomlplusplus::tomlplusplus)
# GUARD: transport-core must never depend on windows.h — do not link any Win32 lib here.
```

Create empty stub `.cpp`/`.h` pairs for framer/channel/occupancy/dropout/config so this configures.

- [ ] **Step 5: `tests/transport-core/CMakeLists.txt`**

```cmake
add_executable(transport-core-tests
  test_spsc_ring.cpp test_framer.cpp test_channel.cpp
  test_occupancy.cpp test_dropout.cpp test_config.cpp)
target_link_libraries(transport-core-tests PRIVATE transport-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(transport-core-tests)
```

Create each test file with a single `TEST(Placeholder, Compiles){ SUCCEED(); }` so it builds.

- [ ] **Step 6: `src/win-audio/CMakeLists.txt` and `src/phase0-app/CMakeLists.txt`** — stub targets

```cmake
# win-audio/CMakeLists.txt
add_library(win-audio
  device_enumerator.cpp mmcss_thread.cpp rt_memory.cpp
  wasapi_capture.cpp wasapi_render.cpp)
target_include_directories(win-audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(win-audio PUBLIC transport-core PRIVATE avrt ole32 ksuser)
```
```cmake
# phase0-app/CMakeLists.txt
add_executable(phase0-app main.cpp bridge.cpp logger.cpp)
target_link_libraries(phase0-app PRIVATE transport-core win-audio)
```
Create empty stubs for all listed sources so configuration succeeds.

- [ ] **Step 7: Configure and build**

Run:
```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```
Expected: configures, fetches gtest + toml++, builds all (empty) targets with no errors.

- [ ] **Step 8: Run the (placeholder) tests**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: all placeholder tests PASS.

- [ ] **Step 9: Commit**

```bash
git add -A && git commit -m "chore: scaffold CMake project, targets, gtest + toml++"
```

---

## Task 1: `SpscRing<T>` — lock-free single-producer/single-consumer ring

**Files:**
- Create: `src/transport-core/spsc_ring.h`
- Test: `tests/transport-core/test_spsc_ring.cpp`

Design: fixed capacity fixed at construction; internally allocates `capacity + 1` slots (one empty
slot distinguishes full from empty). `head`/`tail` are `std::atomic<size_t>` in **separate
128-byte-aligned structs** to avoid false sharing (§9.5). Producer calls `try_push`, consumer calls
`try_pop`. `size()` is an approximate occupancy safe to read from a third (logger) thread.

- [ ] **Step 1: Write failing tests**

```cpp
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "spsc_ring.h"
using vc::SpscRing;

TEST(SpscRing, PushPopFifo) {
  SpscRing<int> r(4);
  EXPECT_EQ(r.size(), 0u);
  EXPECT_TRUE(r.try_push(1));
  EXPECT_TRUE(r.try_push(2));
  EXPECT_EQ(r.size(), 2u);
  int v = 0;
  EXPECT_TRUE(r.try_pop(v)); EXPECT_EQ(v, 1);
  EXPECT_TRUE(r.try_pop(v)); EXPECT_EQ(v, 2);
  EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, RejectsWhenFull) {
  SpscRing<int> r(2);            // capacity 2
  EXPECT_TRUE(r.try_push(10));
  EXPECT_TRUE(r.try_push(20));
  EXPECT_FALSE(r.try_push(30));  // full
  EXPECT_EQ(r.size(), 2u);
}

TEST(SpscRing, WrapsAround) {
  SpscRing<int> r(2);
  int v;
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(r.try_push(i));
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_EQ(r.size(), 0u);
}

TEST(SpscRing, ConcurrentProducerConsumer) {
  SpscRing<int> r(64);
  constexpr int N = 200000;
  std::thread prod([&]{
    for (int i = 0; i < N; ) { if (r.try_push(i)) ++i; }
  });
  std::vector<int> got; got.reserve(N);
  int v;
  while ((int)got.size() < N) { if (r.try_pop(v)) got.push_back(v); }
  prod.join();
  for (int i = 0; i < N; ++i) EXPECT_EQ(got[i], i);  // FIFO, no loss, no dup
}
```

- [ ] **Step 2: Run to verify failure** — `ctest --test-dir build -C Debug -R SpscRing` → FAIL (no `spsc_ring.h`). Add `test_spsc_ring.cpp` to the tests CMakeLists if not already present (it is, from Task 0).

- [ ] **Step 3: Implement `spsc_ring.h`**

```cpp
#pragma once
#include <atomic>
#include <cstddef>
#include <new>
#include <vector>

namespace vc {
template <typename T>
class SpscRing {
public:
  explicit SpscRing(size_t capacity)
      : slots_(capacity + 1), cap_(capacity + 1) {}

  bool try_push(const T& item) noexcept {
    const size_t h = head_.idx.load(std::memory_order_relaxed);
    const size_t n = (h + 1) % cap_;
    if (n == tail_.idx.load(std::memory_order_acquire)) return false; // full
    slots_[h] = item;
    head_.idx.store(n, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const size_t t = tail_.idx.load(std::memory_order_relaxed);
    if (t == head_.idx.load(std::memory_order_acquire)) return false; // empty
    out = slots_[t];
    tail_.idx.store((t + 1) % cap_, std::memory_order_release);
    return true;
  }

  size_t size() const noexcept {
    const size_t h = head_.idx.load(std::memory_order_acquire);
    const size_t t = tail_.idx.load(std::memory_order_acquire);
    return (h + cap_ - t) % cap_;
  }
  size_t capacity() const noexcept { return cap_ - 1; }
  T* data() noexcept { return slots_.data(); }
  size_t byte_size() const noexcept { return slots_.size() * sizeof(T); }

private:
  struct alignas(128) Index { std::atomic<size_t> idx{0}; };
  std::vector<T> slots_;
  size_t cap_;
  Index head_;
  Index tail_;
};
}  // namespace vc
```

- [ ] **Step 4: Run to verify pass** — `ctest --test-dir build -C Debug -R SpscRing` → all PASS.

- [ ] **Step 5: Commit** — `git add -A && git commit -m "feat(core): lock-free SPSC ring with padded indices"`

---

## Task 2: `Framer` — bidirectional 480-sample framing

**Files:**
- Create: `src/transport-core/framer.h`, `src/transport-core/framer.cpp`
- Test: `tests/transport-core/test_framer.cpp`

`FrameAccumulator` (capture side): `write(const float*, size_t)` buffers samples; `pop_frame(float* out480)`
returns true and fills a frame when ≥480 buffered, carrying the remainder. `FrameDistributor` (render
side): `push_frame(const float* in480)`; `read(float* out, size_t n)` fills up to n samples from queued
frames, returning the count actually written (short on underrun).

- [ ] **Step 1: Write failing tests**

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "framer.h"
using vc::FrameAccumulator; using vc::FrameDistributor; using vc::FRAME_SAMPLES;

TEST(FrameAccumulator, ExactFrame) {
  FrameAccumulator a;
  std::vector<float> in(FRAME_SAMPLES);
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) in[i] = (float)i;
  a.write(in.data(), in.size());
  float out[FRAME_SAMPLES];
  ASSERT_TRUE(a.pop_frame(out));
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_FLOAT_EQ(out[i], (float)i);
  EXPECT_FALSE(a.pop_frame(out));
}

TEST(FrameAccumulator, SmallChunksCarryRemainder) {
  FrameAccumulator a;
  float out[FRAME_SAMPLES];
  // feed 128 samples at a time (a realistic WASAPI packet); frame ready after 4 (=512>=480)
  float chunk[128];
  for (int i = 0; i < 128; ++i) chunk[i] = 1.0f;
  a.write(chunk, 128); EXPECT_FALSE(a.pop_frame(out));
  a.write(chunk, 128); EXPECT_FALSE(a.pop_frame(out));
  a.write(chunk, 128); EXPECT_FALSE(a.pop_frame(out));
  a.write(chunk, 128); EXPECT_TRUE(a.pop_frame(out));   // 512 buffered → 1 frame, 32 remain
  EXPECT_FALSE(a.pop_frame(out));
}

TEST(FrameAccumulator, MultipleFramesFromLargeWrite) {
  FrameAccumulator a;
  std::vector<float> big(FRAME_SAMPLES * 3 + 17, 0.5f);
  a.write(big.data(), big.size());
  float out[FRAME_SAMPLES];
  EXPECT_TRUE(a.pop_frame(out));
  EXPECT_TRUE(a.pop_frame(out));
  EXPECT_TRUE(a.pop_frame(out));
  EXPECT_FALSE(a.pop_frame(out));   // 17 remain
}

TEST(FrameDistributor, ReadShortOnUnderrun) {
  FrameDistributor d;
  float frame[FRAME_SAMPLES];
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) frame[i] = 2.0f;
  d.push_frame(frame);
  std::vector<float> out(FRAME_SAMPLES + 10, -1.0f);
  size_t got = d.read(out.data(), out.size());
  EXPECT_EQ(got, FRAME_SAMPLES);              // only one frame available
  EXPECT_FLOAT_EQ(out[0], 2.0f);
}

TEST(FrameDistributor, ReadAcrossFrameBoundary) {
  FrameDistributor d;
  float f1[FRAME_SAMPLES], f2[FRAME_SAMPLES];
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) { f1[i] = 1.0f; f2[i] = 3.0f; }
  d.push_frame(f1); d.push_frame(f2);
  std::vector<float> out(FRAME_SAMPLES + 5);
  EXPECT_EQ(d.read(out.data(), out.size()), FRAME_SAMPLES + 5);
  EXPECT_FLOAT_EQ(out[FRAME_SAMPLES - 1], 1.0f);
  EXPECT_FLOAT_EQ(out[FRAME_SAMPLES], 3.0f);
}
```

- [ ] **Step 2: Run to verify failure** — `ctest --test-dir build -C Debug -R Frame` → FAIL.

- [ ] **Step 3: Implement `framer.h` / `framer.cpp`**

```cpp
// framer.h
#pragma once
#include <cstddef>
#include <deque>
#include "frame.h"
namespace vc {
class FrameAccumulator {
public:
  void write(const float* in, size_t n);
  bool pop_frame(float* out480);   // true if a full frame was written
private:
  std::deque<float> buf_;
};
class FrameDistributor {
public:
  void push_frame(const float* in480);
  size_t read(float* out, size_t n);   // returns samples written (< n on underrun)
private:
  std::deque<float> buf_;
};
}
```
```cpp
// framer.cpp
#include "framer.h"
namespace vc {
void FrameAccumulator::write(const float* in, size_t n) {
  buf_.insert(buf_.end(), in, in + n);
}
bool FrameAccumulator::pop_frame(float* out480) {
  if (buf_.size() < FRAME_SAMPLES) return false;
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) { out480[i] = buf_.front(); buf_.pop_front(); }
  return true;
}
void FrameDistributor::push_frame(const float* in480) {
  buf_.insert(buf_.end(), in480, in480 + FRAME_SAMPLES);
}
size_t FrameDistributor::read(float* out, size_t n) {
  size_t k = 0;
  while (k < n && !buf_.empty()) { out[k++] = buf_.front(); buf_.pop_front(); }
  return k;
}
}
```
> NOTE: `std::deque` allocates — acceptable in `transport-core` unit context and in Phase 0 where
> the relay is not yet the hard-RT DSP stage. Phase 2 replaces these with fixed ring-backed buffers.
> Recorded as a known Phase 2 follow-up, not a Phase 0 defect.

- [ ] **Step 4: Run to verify pass** — `ctest --test-dir build -C Debug -R Frame` → PASS.

- [ ] **Step 5: Commit** — `git commit -am "feat(core): bidirectional 480-sample framer"`

---

## Task 3: `ChannelExtractor` — interleaved ch0 extract + mono→stereo

**Files:**
- Create: `src/transport-core/channel.h`, `src/transport-core/channel.cpp`
- Test: `tests/transport-core/test_channel.cpp`

- [ ] **Step 1: Write failing tests**

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "channel.h"
using vc::extract_channel; using vc::mono_to_interleaved;

TEST(Channel, ExtractCh0FromStereo) {
  std::vector<float> inter = {1,2, 3,4, 5,6};   // 3 frames, 2ch: L=1,3,5 R=2,4,6
  std::vector<float> out(3);
  extract_channel(inter.data(), 3, 2, 0, out.data());
  EXPECT_FLOAT_EQ(out[0], 1); EXPECT_FLOAT_EQ(out[1], 3); EXPECT_FLOAT_EQ(out[2], 5);
}

TEST(Channel, ExtractPassthroughMono) {
  std::vector<float> inter = {7,8,9};
  std::vector<float> out(3);
  extract_channel(inter.data(), 3, 1, 0, out.data());
  EXPECT_FLOAT_EQ(out[0], 7); EXPECT_FLOAT_EQ(out[2], 9);
}

TEST(Channel, MonoToStereoDuplicates) {
  std::vector<float> mono = {4, 5};
  std::vector<float> out(4);
  mono_to_interleaved(mono.data(), 2, 2, out.data());
  EXPECT_FLOAT_EQ(out[0], 4); EXPECT_FLOAT_EQ(out[1], 4);
  EXPECT_FLOAT_EQ(out[2], 5); EXPECT_FLOAT_EQ(out[3], 5);
}
```

- [ ] **Step 2: Run to verify failure** — `-R Channel` → FAIL.

- [ ] **Step 3: Implement**

```cpp
// channel.h
#pragma once
#include <cstddef>
namespace vc {
void extract_channel(const float* interleaved, size_t frames, int numChannels, int ch, float* out);
void mono_to_interleaved(const float* mono, size_t frames, int numChannels, float* out);
}
```
```cpp
// channel.cpp
#include "channel.h"
namespace vc {
void extract_channel(const float* in, size_t frames, int nc, int ch, float* out) {
  for (size_t i = 0; i < frames; ++i) out[i] = in[i * nc + ch];
}
void mono_to_interleaved(const float* mono, size_t frames, int nc, float* out) {
  for (size_t i = 0; i < frames; ++i)
    for (int c = 0; c < nc; ++c) out[i * nc + c] = mono[i];
}
}
```

- [ ] **Step 4: Run to verify pass** — `-R Channel` → PASS.
- [ ] **Step 5: Commit** — `git commit -am "feat(core): channel extract + mono/stereo interleave"`

---

## Task 4: `OccupancyStats` — high-water tracking

**Files:**
- Create: `src/transport-core/occupancy.h`, `src/transport-core/occupancy.cpp`
- Test: `tests/transport-core/test_occupancy.cpp`

- [ ] **Step 1: Failing tests**

```cpp
#include <gtest/gtest.h>
#include "occupancy.h"
using vc::OccupancyStats;

TEST(Occupancy, TracksHighWater) {
  OccupancyStats s;
  s.sample(3); s.sample(7); s.sample(2);
  EXPECT_EQ(s.high_water(), 7u);
}
TEST(Occupancy, ResetClears) {
  OccupancyStats s;
  s.sample(5); s.reset();
  EXPECT_EQ(s.high_water(), 0u);
}
```

- [ ] **Step 2: Verify failure** — `-R Occupancy` → FAIL.
- [ ] **Step 3: Implement**

```cpp
// occupancy.h
#pragma once
#include <cstddef>
namespace vc {
class OccupancyStats {
public:
  void sample(size_t occ) { if (occ > hi_) hi_ = occ; }
  size_t high_water() const { return hi_; }
  void reset() { hi_ = 0; }
private:
  size_t hi_ = 0;
};
}
```
(occupancy.cpp: empty translation unit with `#include "occupancy.h"` to satisfy the source list, or
remove it from the CMake source list and make the class header-only. If removed, drop `occupancy.cpp`
from `src/transport-core/CMakeLists.txt`.)

- [ ] **Step 4: Verify pass** — `-R Occupancy` → PASS.
- [ ] **Step 5: Commit** — `git commit -am "feat(core): occupancy high-water stats"`

---

## Task 5: `DropoutCounter` — atomic under/overrun counters

**Files:**
- Create: `src/transport-core/dropout.h`, `src/transport-core/dropout.cpp`
- Test: `tests/transport-core/test_dropout.cpp`

- [ ] **Step 1: Failing tests**

```cpp
#include <gtest/gtest.h>
#include "dropout.h"
using vc::DropoutCounter;

TEST(Dropout, CountsWhenAccounting) {
  DropoutCounter c;
  c.record_underrun(); c.record_underrun(); c.record_overrun();
  EXPECT_EQ(c.underruns(), 2u);
  EXPECT_EQ(c.overruns(), 1u);
}
TEST(Dropout, StopsCountingAfterShutdown) {
  DropoutCounter c;
  c.stop_accounting();
  c.record_underrun(); c.record_overrun();
  EXPECT_EQ(c.underruns(), 0u);
  EXPECT_EQ(c.overruns(), 0u);
}
```

- [ ] **Step 2: Verify failure** — `-R Dropout` → FAIL.
- [ ] **Step 3: Implement**

```cpp
// dropout.h
#pragma once
#include <atomic>
#include <cstdint>
namespace vc {
class DropoutCounter {
public:
  void record_underrun() noexcept { if (on_.load(std::memory_order_relaxed)) under_.fetch_add(1, std::memory_order_relaxed); }
  void record_overrun()  noexcept { if (on_.load(std::memory_order_relaxed)) over_.fetch_add(1, std::memory_order_relaxed); }
  uint64_t underruns() const noexcept { return under_.load(std::memory_order_relaxed); }
  uint64_t overruns()  const noexcept { return over_.load(std::memory_order_relaxed); }
  void stop_accounting() noexcept { on_.store(false, std::memory_order_relaxed); }
private:
  std::atomic<bool> on_{true};
  std::atomic<uint64_t> under_{0}, over_{0};
};
}
```
(Header-only; drop `dropout.cpp` from the source list, mirroring Task 4.)

- [ ] **Step 4: Verify pass** — `-R Dropout` → PASS.
- [ ] **Step 5: Commit** — `git commit -am "feat(core): dropout counter with shutdown gate"`

---

## Task 6: `Config` — TOML load + CLI override

**Files:**
- Create: `src/transport-core/config.h`, `src/transport-core/config.cpp`
- Test: `tests/transport-core/test_config.cpp`

- [ ] **Step 1: Failing tests**

```cpp
#include <gtest/gtest.h>
#include <fstream>
#include "config.h"
using vc::Config;

static std::string write_tmp(const char* body) {
  std::string p = "tmp_cfg.toml";
  std::ofstream(p) << body;
  return p;
}

TEST(Config, LoadsToml) {
  auto p = write_tmp(
    "capture_device_id = \"cap-1\"\n"
    "render_device_id  = \"ren-1\"\n"
    "ring_frames = 1024\n"
    "csv_path = \"drift.csv\"\n");
  Config c = Config::load(p);
  EXPECT_EQ(c.captureDeviceId, "cap-1");
  EXPECT_EQ(c.renderDeviceId, "ren-1");
  EXPECT_EQ(c.ringFrames, 1024u);
  EXPECT_EQ(c.csvPath, "drift.csv");
}

TEST(Config, CliOverridesTakePrecedence) {
  Config c;
  c.captureDeviceId = "from-file";
  const char* argv[] = {"app", "--capture", "from-cli", "--csv", "x.csv"};
  c.apply_cli(5, const_cast<char**>(argv));
  EXPECT_EQ(c.captureDeviceId, "from-cli");
  EXPECT_EQ(c.csvPath, "x.csv");
}

TEST(Config, EnforcesRingFloor) {
  Config c; c.ringFrames = 2;   // below the 3-frame floor (§5.3)
  c.validate();
  EXPECT_GE(c.ringFrames, 3u);
}
```

- [ ] **Step 2: Verify failure** — `-R Config` → FAIL.
- [ ] **Step 3: Implement** (`config.h` struct + `config.cpp` using `toml::parse_file`, a small
  `--flag value` scanner for `--capture/--render/--csv/--ring`, and `validate()` clamping
  `ringFrames` to ≥3). Full signatures:

```cpp
// config.h
#pragma once
#include <string>
#include <cstddef>
namespace vc {
struct Config {
  std::string captureDeviceId;
  std::string renderDeviceId;
  size_t      ringFrames = 1024;
  std::string csvPath = "drift.csv";
  int         logLevel = 1;
  static Config load(const std::string& tomlPath);   // throws std::runtime_error on parse error
  void apply_cli(int argc, char** argv);
  void validate();                                    // clamps ringFrames >= 3
};
}
```
```cpp
// config.cpp
#include "config.h"
#include <toml++/toml.hpp>
#include <cstring>
#include <stdexcept>
namespace vc {
Config Config::load(const std::string& path) {
  toml::table t;
  try { t = toml::parse_file(path); }
  catch (const toml::parse_error& e) { throw std::runtime_error(std::string("config parse: ") + e.description().data()); }
  Config c;
  c.captureDeviceId = t["capture_device_id"].value_or("");
  c.renderDeviceId  = t["render_device_id"].value_or("");
  c.ringFrames      = (size_t)t["ring_frames"].value_or((int64_t)1024);
  c.csvPath         = t["csv_path"].value_or("drift.csv");
  c.logLevel        = t["log_level"].value_or(1);
  c.validate();
  return c;
}
void Config::apply_cli(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--capture")) captureDeviceId = argv[i+1];
    else if (!std::strcmp(argv[i], "--render")) renderDeviceId = argv[i+1];
    else if (!std::strcmp(argv[i], "--csv")) csvPath = argv[i+1];
    else if (!std::strcmp(argv[i], "--ring")) ringFrames = (size_t)std::atoi(argv[i+1]);
  }
  validate();
}
void Config::validate() { if (ringFrames < 3) ringFrames = 3; }
}
```

- [ ] **Step 4: Verify pass** — `-R Config` → PASS.
- [ ] **Step 5: Commit** — `git commit -am "feat(core): TOML config + CLI override + ring floor"`

---

## Task 7: `DeviceEnumerator` + `--list-devices`

**Files:**
- Create: `src/win-audio/com_guard.h`, `src/win-audio/device_enumerator.h`, `.cpp`
- Modify: `src/phase0-app/main.cpp` (dispatch `--list-devices`)

Windows-specific; verified by running, not unit tests. `DeviceEnumerator::list()` uses
`IMMDeviceEnumerator` (`eRender`/`eCapture`, `DEVICE_STATE_ACTIVE`) → for each device read the
friendly name (`IPropertyStore` / `PKEY_Device_FriendlyName`), the endpoint ID
(`IMMDevice::GetId`), and the mix format (`IAudioClient::GetMixFormat`). `resolve(id, flow)` returns
an `IMMDevice`. `check_format(mix)` returns an error unless `nSamplesPerSec == 48000` and the format
is IEEE float32 **or** PCM16 (record which, for the sample conversion path).

- [ ] **Step 1: Implement `com_guard.h`**

```cpp
#pragma once
#include <objbase.h>
#include <stdexcept>
namespace vc {
struct ComGuard {
  ComGuard() { HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
               if (FAILED(hr)) throw std::runtime_error("CoInitializeEx failed"); }
  ~ComGuard() { CoUninitialize(); }
};
}
```

- [ ] **Step 2: Implement `device_enumerator.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <mmdeviceapi.h>
#include <audioclient.h>
namespace vc {
enum class SampleKind { Float32, Pcm16, Unsupported };
struct DeviceInfo { std::wstring id; std::wstring name; uint32_t sampleRate; uint16_t channels; SampleKind kind; };
struct FormatCheck { bool ok; SampleKind kind; uint16_t channels; std::string message; };

class DeviceEnumerator {
public:
  std::vector<DeviceInfo> list(EDataFlow flow);              // eRender or eCapture
  IMMDevice* resolve(const std::wstring& id);                // caller Release()s
  static FormatCheck check_format(const WAVEFORMATEX* mix);  // 48k + float32/pcm16
};
}
```

- [ ] **Step 3: Implement `.cpp`** — enumerate, read friendly name + id + mix format, classify
  `SampleKind` (`WAVE_FORMAT_IEEE_FLOAT` / `WAVEFORMATEXTENSIBLE` subformat, or 16-bit PCM), and
  `check_format` returning `ok=false` with a message pointing at the endpoint's Advanced tab when
  `nSamplesPerSec != 48000`.

- [ ] **Step 4: Wire `--list-devices` in `main.cpp`**

```cpp
// pseudODispatch: if argv contains "--list-devices": ComGuard g; DeviceEnumerator e;
// print "RENDER:" then e.list(eRender); "CAPTURE:" then e.list(eCapture); each row:
//   <id>\t<name>\t<rate>Hz\t<channels>ch\t<kind>
// return 0.
```

- [ ] **Step 5: Build + manual verify**

Run:
```bash
cmake --build build --config Debug
build/src/phase0-app/Debug/phase0-app.exe --list-devices
```
Expected: the K688 appears under CAPTURE and "CABLE Input" appears under RENDER, both at 48000 Hz.
Record their IDs for the config file.

- [ ] **Step 6: Commit** — `git commit -am "feat(win): device enumeration + --list-devices + format guard"`

---

## Task 8: `MmcssThread` + `RtMemory`

**Files:**
- Create: `src/win-audio/mmcss_thread.h`/`.cpp`, `src/win-audio/rt_memory.h`/`.cpp`

- [ ] **Step 1: `mmcss_thread.h`/`.cpp`** — RAII wrapper

```cpp
// mmcss_thread.h
#pragma once
namespace vc {
class MmcssThread {                       // call on the thread you want registered
public:
  MmcssThread();                          // AvSetMmThreadCharacteristicsW(L"Pro Audio") + CRITICAL
  ~MmcssThread();                         // AvRevertMmThreadCharacteristics — never leak (§5.4/§12)
  bool ok() const { return handle_ != nullptr; }
private:
  void* handle_ = nullptr;
};
}
```
`.cpp` uses `AvSetMmThreadCharacteristicsW`, `AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL)`,
and reverts in the destructor. Link `avrt`.

- [ ] **Step 2: `rt_memory.h`/`.cpp`** — lock + pre-touch

```cpp
// rt_memory.h
#pragma once
#include <cstddef>
namespace vc {
// VirtualLock the region and write one byte per 4 KiB page (pre-touch). Best-effort:
// logs (via return bool) if VirtualLock fails, does not throw. Unlock in the matching call.
bool rt_lock_and_touch(void* addr, size_t bytes);
void rt_unlock(void* addr, size_t bytes);
}
```
`.cpp` calls `VirtualLock`, then walks the region touching each page, and `VirtualUnlock` in
`rt_unlock`. (May require raising the process working-set via `SetProcessWorkingSetSize` for large
locks — note in a comment; the ~10 s ring is a few MB, within the default minimum working set, so
Phase 0 does not need to raise it.)

- [ ] **Step 3: Build** — `cmake --build build --config Debug` → win-audio compiles.
- [ ] **Step 4: Commit** — `git commit -am "feat(win): MMCSS Pro Audio RAII + VirtualLock/pre-touch"`

---

## Task 9: `WasapiCapture`

**Files:**
- Create: `src/win-audio/wasapi_capture.h`/`.cpp`

Event-driven shared-mode capture. `start(device, onSamples)` initializes `IAudioClient` with
`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, creates the event, gets `IAudioCaptureClient`, and runs a
thread that: registers `MmcssThread`, loops on `WaitForSingleObject(event)`, `GetBuffer` →
deliver interleaved samples + channel count + `SampleKind` via callback → `ReleaseBuffer`, until
stopped. Surfaces `AUDCLNT_E_DEVICE_INVALIDATED` via an error callback.

- [ ] **Step 1: Header**

```cpp
#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "device_enumerator.h"
namespace vc {
class WasapiCapture {
public:
  using SampleCb = std::function<void(const float* interleaved, size_t frames, int channels)>;
  using ErrorCb  = std::function<void(const char* what)>;
  bool start(IMMDevice* dev, SampleCb onSamples, ErrorCb onError);   // false on setup failure
  void stop();
  ~WasapiCapture();
private:
  void run();
  // IAudioClient*, IAudioCaptureClient*, HANDLE event_, std::thread, std::atomic<bool> running_,
  // SampleKind kind_, WAVEFORMATEX mix_, PCM16->float conversion scratch
};
}
```

- [ ] **Step 2: Implement `.cpp`** — full WASAPI init (`Initialize(AUDCLNT_SHAREMODE_SHARED,
  AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr)`, `SetEventHandle`, `GetService`), the
  MMCSS-registered capture loop, PCM16→float conversion when `kind_ == Pcm16`, and clean stop
  (`IAudioClient::Stop`, signal + join, Release).

- [ ] **Step 3: Build** — compiles.
- [ ] **Step 4: Commit** — `git commit -am "feat(win): event-driven WASAPI capture"`

---

## Task 10: `WasapiRender`

**Files:**
- Create: `src/win-audio/wasapi_render.h`/`.cpp`

Event-driven shared-mode render into CABLE Input. `start(device, fillCb)`: init `IAudioClient` +
`IAudioRenderClient`, **pre-fill the engine buffer with silence** (`GetBufferSize` →
`GetBuffer`/`ReleaseBuffer(..., AUDCLNT_BUFFERFLAGS_SILENT)`), then run an MMCSS thread that waits on
the event and, each period, computes `GetCurrentPadding` → frames needed = bufferSize − padding →
asks the fill callback for that many mono samples → `mono_to_interleaved` → writes; if the callback
returns short, zero-fill the remainder (the render-side underrun).

- [ ] **Step 1: Header**

```cpp
#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <mmdeviceapi.h>
#include <audioclient.h>
namespace vc {
class WasapiRender {
public:
  // fill up to n mono samples into out; return count written (< n → underrun, zero-filled by caller)
  using FillCb  = std::function<size_t(float* outMono, size_t n)>;
  using ErrorCb = std::function<void(const char* what)>;
  bool start(IMMDevice* dev, FillCb onFill, ErrorCb onError);
  void stop();
  ~WasapiRender();
private:
  void run();
  // IAudioClient*, IAudioRenderClient*, HANDLE event_, mix format (channels), thread, running flag
};
}
```

- [ ] **Step 2: Implement `.cpp`** — as above, with the initial silent pre-roll and per-period
  fill/convert/zero-fill-on-short, MMCSS registration, clean stop.

- [ ] **Step 3: Build** — compiles.
- [ ] **Step 4: Commit** — `git commit -am "feat(win): event-driven WASAPI render with silent pre-roll"`

---

## Task 11: `Logger` + `Bridge` wiring + `main`

**Files:**
- Create: `src/phase0-app/logger.h`/`.cpp`, `src/phase0-app/bridge.h`/`.cpp`
- Modify: `src/phase0-app/main.cpp`

**Logger** (non-RT thread): once/sec, snapshots Ring A/B `size()`, `OccupancyStats` high-water, and
`DropoutCounter` counts; appends a CSV row (`elapsed_ms,ringA_frames,ringB_frames,ringA_hi,ringB_hi,
underruns,overruns`) and prints a console status line. CSV header carries a wall-clock start stamp.
`elapsed_ms` from `QueryPerformanceCounter`.

**Bridge**: owns `SpscRing<Frame> ringA(cfg.ringFrames)`, `ringB(cfg.ringFrames)`; `rt_lock_and_touch`
both buffers; **primes Ring B to half capacity with silent frames**; starts `WasapiRender` (fill
callback pops a frame from Ring B via `FrameDistributor`, records underrun on short), the relay
thread (MMCSS; waits on frame-ready event; drains Ring A → pushes to Ring B, records overrun if
Ring B full), and `WasapiCapture` (extracts ch0 → `FrameAccumulator` → push Ring A → signal
frame-ready; records overrun if Ring A full). Clean shutdown reverses the order and
`stop_accounting()` first.

- [ ] **Step 1: Implement `logger.h`/`.cpp`** (own thread, `std::this_thread::sleep_for(1s)` loop,
  guarded by a running flag; opens the CSV once, flushes each row).

- [ ] **Step 2: Implement `bridge.h`/`.cpp`** with the exact wiring above. Priming loop:
  `Frame silent{}; for (size_t i=0;i<ringB.capacity()/2;++i) ringB.try_push(silent);` **before**
  `render.start(...)`.

- [ ] **Step 3: `main.cpp`** — parse args; if `--list-devices`, dispatch Task 7 path; else
  `Config::load` + `apply_cli`, resolve devices, construct `Bridge`, install a `SetConsoleCtrlHandler`
  that signals shutdown, run until signalled, then `Bridge::stop()`.

- [ ] **Step 4: Build** — `cmake --build build --config Debug` → phase0-app links.

- [ ] **Step 5: Short bring-up run (shallow ring)**

Create `bringup.toml` with the K688 + CABLE Input IDs and `ring_frames = 16`. In Windows Sound,
set "Listen to this device" on CABLE Output (or select it as a playback monitor). Run:
```bash
build/src/phase0-app/Debug/phase0-app.exe --config bringup.toml
```
Expected: speaking into the K688 is audible through CABLE Output within a fraction of a second; the
console prints ~1 Hz telemetry; Ctrl+C exits cleanly with a final counter line.

- [ ] **Step 6: Commit** — `git commit -am "feat(app): bridge wiring, priming, logger, clean shutdown"`

---

## Task 12: 8-hour drift run — acceptance runbook

**Files:**
- Create: `docs/runbooks/phase0-8hour-drift.md`

- [ ] **Step 1: Write the runbook** documenting: create `drift.toml` with `ring_frames = 1024`
  and the real device IDs; start the app; run a normal 8-hour day including a long gaming session;
  keep LatencyMon running (record DPC app-off vs app-on beforehand) and take one PresentMon capture
  in-game app-off vs app-on. At the end, Ctrl+C.

- [ ] **Step 2: Analysis** — document plotting `ringB_frames` vs `elapsed_ms` (any spreadsheet or a
  short Python snippet), fitting the slope, and computing
  `drift_ppm = slope_frames_per_s * 480 / 48000 * 1e6`.

- [ ] **Step 3: Record exit criteria results** — underruns/overruns total (target **0**), the ppm
  figure, and the LatencyMon/PresentMon deltas (within run-to-run variance).

- [ ] **Step 4: Commit** — `git commit -am "docs: phase 0 8-hour drift acceptance runbook"`

---

## Self-Review notes

- **Spec coverage:** every §-referenced Phase 0 requirement maps to a task — topology (T11),
  framing (T2), rings + padding (T1), priming + deep ring (T11/T6), format guard (T7), MMCSS (T8/T9/T10),
  VirtualLock/pre-touch (T8/T11), drift CSV + math (T11/T12), dropout accounting + shutdown gate
  (T5/T11), clean MMCSS teardown (T8/T11), `--list-devices` + config (T6/T7).
- **Deferred, intentionally out of Phase 0:** denormal flags, drift compensation, latency rig, full
  `IMMNotificationClient`, CCD pinning, tray UI. Not tasks here by design (see spec §2).
- **Type consistency:** `Frame`/`FRAME_SAMPLES` from `frame.h` used everywhere; `SpscRing<Frame>`
  is the ring type in T11; `FrameAccumulator`/`FrameDistributor` names match between T2 and T11;
  `DropoutCounter::record_underrun/record_overrun/stop_accounting` names match between T5 and T11.
- **Known Phase 2 follow-up (recorded, not a Phase 0 defect):** `Framer` uses `std::deque`
  (allocates); replace with fixed ring-backed buffers when the relay becomes the hard-RT DSP stage.
```
