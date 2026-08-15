# Product Shell — Phase 1 (Foundational Core) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the shared IPC data types and make the DSP chain + drift controller live-tunable, all unit-tested, with no UI and no behavior change at default settings.

**Architecture:** A new `vc-ipc` static lib holds the POD `Params`/`Telemetry` structs (blueprint §8.3/§8.4) and a binary command codec that the worker and UI will later exchange over a named pipe. `denoise-core`'s `DspChain` gains RT-safe setters that ramp HPF cutoff / AGC target / limiter ceiling toward new values (and a bypass), driven per-frame; `transport-core`'s `DriftController` gains a `seedPpm` warm-start. `denoise-core` stays free of `vc-ipc` — the worker (a later phase) maps `Params` fields to `DspChain` setters.

**Tech Stack:** C++20, MSVC, CMake (ESP-IDF-bundled), GoogleTest. Windows-free libraries (`vc-ipc`, `denoise-core`, `transport-core`).

**Build/test commands** (CMake is not on PATH — use the ESP-IDF copy):
- Configure: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64`
- Build (Debug): `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
- Test (filter): `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R <regex>`

**Scope note — aggression:** A live *aggression* slider is intentionally **out of scope for Phase 1**. Aggression is applied inside the engine's ERB-mask path, not in `DspChain`; both live engines (`RnnoiseFilter`, the DFN3 stream adapter) currently no-op `SetAggression`. Wiring live aggression means mask-shaping in the DFN3 streaming adapter (mirroring `src/dfn3/dfn3_offline.cpp:217-225`) and is validated by A/B listening, not a pure unit test — it belongs to the worker-wiring phase where the engine is already in focus. Phase 1 delivers live HPF cutoff, output gain (AGC target), and limiter ceiling.

---

## File Structure

- **Create** `src/vc-ipc/params.h` — `vc::ipc::Params` POD (§8.3), with `defaults()`.
- **Create** `src/vc-ipc/telemetry.h` — `vc::ipc::Telemetry` POD (§8.4).
- **Create** `src/vc-ipc/command.h` + `src/vc-ipc/command.cpp` — `CmdType`, `Command`, `encode`/`decode`.
- **Create** `src/vc-ipc/CMakeLists.txt` — the `vc-ipc` static lib.
- **Create** `tests/vc-ipc/CMakeLists.txt`, `tests/vc-ipc/test_params.cpp`, `tests/vc-ipc/test_command.cpp`.
- **Modify** `CMakeLists.txt` (root) — add the two subdirs.
- **Modify** `src/denoise-core/agc.h` — add `setTargetDb`.
- **Modify** `src/denoise-core/limiter.h` — add `setCeilingDb`.
- **Modify** `src/denoise-core/biquad.h` + `biquad.cpp` — add `setHighpass` (recompute coeffs, keep filter state).
- **Modify** `src/denoise-core/dsp_chain.h` + `dsp_chain.cpp` — ramped setters, bypass, getters.
- **Modify** `tests/denoise-core/test_dsp_chain.cpp`, `tests/denoise-core/test_agc.cpp`, `tests/denoise-core/test_limiter.cpp`, `tests/denoise-core/test_biquad.cpp`.
- **Modify** `src/transport-core/drift_control.h` + `drift_control.cpp` — `seedPpm`.
- **Modify** `tests/transport-core/test_drift_control.cpp`.

---

## Task 1: `vc-ipc` lib with `Params` and `Telemetry` PODs

**Files:**
- Create: `src/vc-ipc/params.h`, `src/vc-ipc/telemetry.h`, `src/vc-ipc/CMakeLists.txt`
- Create: `tests/vc-ipc/test_params.cpp`, `tests/vc-ipc/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root)

- [ ] **Step 1: Create the `Params` header**

Create `src/vc-ipc/params.h`:

```cpp
#pragma once
#include <cstdint>
namespace vc::ipc {
// POD parameter snapshot, UI -> worker (blueprint §8.3). Fixed size, no pointers/strings,
// safe to memcpy into a double-buffered slot and read wait-free on the RT thread.
struct Params {
  float    aggression;          // 0..1 (engine mask shaping; wired in a later phase)
  float    hpfCutoffHz;         // high-pass cutoff
  float    agcTargetDbfs;       // AGC target = "output gain"
  float    limiterCeilingDbfs;  // brickwall ceiling
  uint32_t engine;              // 0 = RNNoise, 1 = DFN3
  uint32_t bypass;              // 0 / 1
  uint32_t dspChain;            // 0 / 1
  uint32_t driftComp;           // 0 / 1
  uint32_t dryTap;              // 0 / 1 (placeholder; behavior deferred)

  static constexpr Params defaults() {
    return Params{
        /*aggression*/ 0.0f,
        /*hpfCutoffHz*/ 80.0f,
        /*agcTargetDbfs*/ -15.0f,
        /*limiterCeilingDbfs*/ -1.0f,
        /*engine*/ 1u,
        /*bypass*/ 0u,
        /*dspChain*/ 1u,
        /*driftComp*/ 1u,
        /*dryTap*/ 0u,
    };
  }
};
}  // namespace vc::ipc
```

- [ ] **Step 2: Create the `Telemetry` header**

Create `src/vc-ipc/telemetry.h`:

```cpp
#pragma once
#include <cstdint>
namespace vc::ipc {
// POD telemetry block, worker -> UI (blueprint §8.4). Written with relaxed atomic stores by the
// worker; the UI tolerates a torn read. `seq` bumps on each aggregate update for torn-read detection.
enum class WorkerState : uint32_t { Starting = 0, Running = 1, DeviceLost = 2, Stopping = 3 };
struct Telemetry {
  // Per-frame (cheap): meter fields.
  float    inputRms;
  float    inputPeak;
  uint32_t vadActive;      // 0 / 1
  // ~1 Hz aggregates.
  float    frameP50Ms;
  float    frameP99Ms;
  float    frameMaxMs;
  uint32_t ringAOcc;
  uint32_t ringBOcc;
  uint32_t ringBHigh;
  uint32_t dropouts;
  uint32_t activeEngine;   // 0 = RNNoise, 1 = DFN3
  double   driftRatioPpm;
  uint64_t uptimeMs;
  uint32_t workerState;    // WorkerState
  uint32_t seq;            // increments each aggregate publish
};
}  // namespace vc::ipc
```

- [ ] **Step 3: Write the failing POD test**

Create `tests/vc-ipc/test_params.cpp`:

```cpp
#include <gtest/gtest.h>
#include <type_traits>
#include "params.h"
#include "telemetry.h"
using vc::ipc::Params; using vc::ipc::Telemetry;

TEST(Params, IsTriviallyCopyableStandardLayout) {
  static_assert(std::is_trivially_copyable_v<Params>);
  static_assert(std::is_standard_layout_v<Params>);
  SUCCEED();
}
TEST(Telemetry, IsTriviallyCopyableStandardLayout) {
  static_assert(std::is_trivially_copyable_v<Telemetry>);
  static_assert(std::is_standard_layout_v<Telemetry>);
  SUCCEED();
}
TEST(Params, DefaultsMatchTodaysDspChain) {
  Params p = Params::defaults();
  EXPECT_FLOAT_EQ(p.hpfCutoffHz, 80.0f);
  EXPECT_FLOAT_EQ(p.agcTargetDbfs, -15.0f);
  EXPECT_FLOAT_EQ(p.limiterCeilingDbfs, -1.0f);
  EXPECT_EQ(p.engine, 1u);
  EXPECT_EQ(p.bypass, 0u);
  EXPECT_EQ(p.dspChain, 1u);
  EXPECT_EQ(p.driftComp, 1u);
}
```

- [ ] **Step 4: Create the lib + test CMake and wire into the build**

`vc-ipc` starts as a **header-only INTERFACE lib** (Task 1 has no `.cpp`); Task 2 upgrades it to a
STATIC lib when it adds `command.cpp`. Create `src/vc-ipc/CMakeLists.txt`:

```cmake
add_library(vc-ipc INTERFACE)
target_include_directories(vc-ipc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# GUARD: vc-ipc must never depend on windows.h — POD structs + a pure codec only.
```

Create `tests/vc-ipc/CMakeLists.txt` (Task 2 adds `test_command.cpp` here):

```cmake
add_executable(vc-ipc-tests test_params.cpp)
target_link_libraries(vc-ipc-tests PRIVATE vc-ipc GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(vc-ipc-tests)
```

In the root `CMakeLists.txt`, add these lines next to the other `add_subdirectory` calls (place the
`src/vc-ipc` line immediately after `add_subdirectory(src/transport-core)`, and the test line after
`add_subdirectory(tests/denoise-core)`):

```cmake
add_subdirectory(src/vc-ipc)
add_subdirectory(tests/vc-ipc)
```

- [ ] **Step 5: Configure, build, run, commit**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64`
Then: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Then: `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R "Params|Telemetry"`
Expected: PASS.

```bash
git add src/vc-ipc tests/vc-ipc CMakeLists.txt
git commit -m "feat(vc-ipc): Params/Telemetry POD structs (header-only)"
```

---

## Task 2: `vc-ipc` command codec

**Files:**
- Create: `src/vc-ipc/command.h`, `src/vc-ipc/command.cpp`
- Create: `tests/vc-ipc/test_command.cpp`

- [ ] **Step 1: Write the codec header**

Create `src/vc-ipc/command.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "params.h"
namespace vc::ipc {
enum class CmdType : uint32_t {
  SetParams           = 1,   // uses `params`
  SelectCaptureDevice = 2,   // uses `deviceId` + `driftSeedPpm`
  SelectRenderDevice  = 3,   // uses `deviceId`
  SetWarmStandby      = 4,   // uses `flag` (0/1)
  ResetCounters       = 5,
  Shutdown            = 6,
};
// Host-side command (UI -> worker). Richer than the shared POD: carries a device-id string.
struct Command {
  CmdType     type{};
  Params      params      = Params::defaults();
  std::string deviceId;
  double      driftSeedPpm = 0.0;
  uint32_t    flag         = 0;
};
// Length-checked binary framing: magic "VCC1" + uint32 type + per-type payload.
std::vector<uint8_t> encode(const Command& c);
bool decode(const uint8_t* data, size_t len, Command& out);  // false on malformed/short/unknown
}  // namespace vc::ipc
```

- [ ] **Step 2: Write the failing codec test**

Create `tests/vc-ipc/test_command.cpp`:

```cpp
#include <gtest/gtest.h>
#include "command.h"
using namespace vc::ipc;

static std::vector<uint8_t> roundtrip_ok(const Command& in, Command& out) {
  auto bytes = encode(in);
  EXPECT_TRUE(decode(bytes.data(), bytes.size(), out));
  return bytes;
}

TEST(Codec, SetParamsRoundTripsExactly) {
  Command in; in.type = CmdType::SetParams;
  in.params.hpfCutoffHz = 123.5f; in.params.agcTargetDbfs = -9.25f;
  in.params.engine = 0; in.params.bypass = 1;
  Command out; roundtrip_ok(in, out);
  EXPECT_EQ(out.type, CmdType::SetParams);
  EXPECT_FLOAT_EQ(out.params.hpfCutoffHz, 123.5f);
  EXPECT_FLOAT_EQ(out.params.agcTargetDbfs, -9.25f);
  EXPECT_EQ(out.params.engine, 0u);
  EXPECT_EQ(out.params.bypass, 1u);
}
TEST(Codec, SelectCaptureDeviceRoundTripsIdAndSeed) {
  Command in; in.type = CmdType::SelectCaptureDevice;
  in.deviceId = "{0.0.1.00000000}.{5fbcc184}"; in.driftSeedPpm = -26.0;
  Command out; roundtrip_ok(in, out);
  EXPECT_EQ(out.type, CmdType::SelectCaptureDevice);
  EXPECT_EQ(out.deviceId, in.deviceId);
  EXPECT_DOUBLE_EQ(out.driftSeedPpm, -26.0);
}
TEST(Codec, ShutdownRoundTrips) {
  Command in; in.type = CmdType::Shutdown;
  Command out; roundtrip_ok(in, out);
  EXPECT_EQ(out.type, CmdType::Shutdown);
}
TEST(Codec, RejectsTooShort) {
  Command out; uint8_t junk[3] = {1,2,3};
  EXPECT_FALSE(decode(junk, sizeof(junk), out));
}
TEST(Codec, RejectsBadMagic) {
  Command out; uint8_t buf[8] = {'X','X','X','X', 1,0,0,0};
  EXPECT_FALSE(decode(buf, sizeof(buf), out));
}
TEST(Codec, RejectsTruncatedPayload) {
  Command in; in.type = CmdType::SetParams;
  auto bytes = encode(in);
  bytes.resize(bytes.size() - 4);   // chop the params tail
  Command out;
  EXPECT_FALSE(decode(bytes.data(), bytes.size(), out));
}
```

- [ ] **Step 3: Implement the codec**

Create `src/vc-ipc/command.cpp`:

```cpp
#include "command.h"
#include <cstring>
namespace vc::ipc {
namespace {
constexpr uint8_t kMagic[4] = {'V','C','C','1'};
template <class T> void put(std::vector<uint8_t>& b, const T& v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(T));
}
template <class T> bool get(const uint8_t*& p, const uint8_t* end, T& v) {
  if (static_cast<size_t>(end - p) < sizeof(T)) return false;
  std::memcpy(&v, p, sizeof(T)); p += sizeof(T); return true;
}
}  // namespace

std::vector<uint8_t> encode(const Command& c) {
  std::vector<uint8_t> b;
  b.insert(b.end(), kMagic, kMagic + 4);
  put(b, static_cast<uint32_t>(c.type));
  switch (c.type) {
    case CmdType::SetParams: put(b, c.params); break;
    case CmdType::SelectCaptureDevice: {
      put(b, c.driftSeedPpm);
      put(b, static_cast<uint32_t>(c.deviceId.size()));
      b.insert(b.end(), c.deviceId.begin(), c.deviceId.end());
      break;
    }
    case CmdType::SelectRenderDevice: {
      put(b, static_cast<uint32_t>(c.deviceId.size()));
      b.insert(b.end(), c.deviceId.begin(), c.deviceId.end());
      break;
    }
    case CmdType::SetWarmStandby: put(b, c.flag); break;
    case CmdType::ResetCounters:
    case CmdType::Shutdown: break;
  }
  return b;
}

bool decode(const uint8_t* data, size_t len, Command& out) {
  if (len < 8) return false;
  if (std::memcmp(data, kMagic, 4) != 0) return false;
  const uint8_t* p = data + 4; const uint8_t* end = data + len;
  uint32_t type = 0; if (!get(p, end, type)) return false;
  out = Command{};
  out.type = static_cast<CmdType>(type);
  switch (out.type) {
    case CmdType::SetParams: return get(p, end, out.params);
    case CmdType::SelectCaptureDevice: {
      uint32_t n = 0;
      if (!get(p, end, out.driftSeedPpm)) return false;
      if (!get(p, end, n)) return false;
      if (static_cast<size_t>(end - p) < n) return false;
      out.deviceId.assign(reinterpret_cast<const char*>(p), n); p += n; return true;
    }
    case CmdType::SelectRenderDevice: {
      uint32_t n = 0;
      if (!get(p, end, n)) return false;
      if (static_cast<size_t>(end - p) < n) return false;
      out.deviceId.assign(reinterpret_cast<const char*>(p), n); p += n; return true;
    }
    case CmdType::SetWarmStandby: return get(p, end, out.flag);
    case CmdType::ResetCounters:
    case CmdType::Shutdown: return true;
    default: return false;
  }
}
}  // namespace vc::ipc
```

- [ ] **Step 4: Upgrade the CMake (INTERFACE → STATIC) and register the new files**

Replace `src/vc-ipc/CMakeLists.txt` with:

```cmake
add_library(vc-ipc STATIC command.cpp)
target_include_directories(vc-ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# GUARD: vc-ipc must never depend on windows.h — POD structs + a pure codec only.
```

Edit `tests/vc-ipc/CMakeLists.txt` — add `test_command.cpp` to the test exe:

```cmake
add_executable(vc-ipc-tests test_params.cpp test_command.cpp)
target_link_libraries(vc-ipc-tests PRIVATE vc-ipc GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(vc-ipc-tests)
```

- [ ] **Step 5: Re-configure, build, run the vc-ipc tests**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64`
Then: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Then: `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R "Params|Telemetry|Codec"`
Expected: PASS (Task 1 + Task 2 tests).

- [ ] **Step 6: Commit**

```bash
git add src/vc-ipc tests/vc-ipc CMakeLists.txt
git commit -m "feat(vc-ipc): Params/Telemetry PODs + binary command codec"
```

---

## Task 3: DSP-leaf live setters

**Files:**
- Modify: `src/denoise-core/agc.h`, `src/denoise-core/limiter.h`, `src/denoise-core/biquad.h`, `src/denoise-core/biquad.cpp`
- Modify: `tests/denoise-core/test_agc.cpp`, `tests/denoise-core/test_limiter.cpp`, `tests/denoise-core/test_biquad.cpp`

- [ ] **Step 1: Write the failing setter tests**

Append to `tests/denoise-core/test_agc.cpp`:

```cpp
TEST(Agc, SetTargetDbChangesGainTargetLive) {
  vc::Agc a({-30.0f, 5.0f, 5.0f, 30.0f, -10.0f, 48000.0f});
  std::vector<float> in(4800, 0.1f), out(4800);
  a.process(in.data(), out.data(), in.size(), true);
  a.setTargetDb(-6.0f);                      // raise the target
  a.process(in.data(), out.data(), in.size(), true);
  float rmsAfter = 0; for (float v : out) rmsAfter += v * v;
  rmsAfter = std::sqrt(rmsAfter / out.size());
  const float targetLin = std::pow(10.0f, -6.0f / 20.0f);
  EXPECT_NEAR(rmsAfter, targetLin, 0.05f);   // converged near the NEW target
}
```
(Ensure `#include <cmath>` and `#include <vector>` are present at the top of the file.)

Append to `tests/denoise-core/test_limiter.cpp`:

```cpp
TEST(Limiter, SetCeilingDbLowersTheCeilingLive) {
  vc::Limiter lim(-1.0f, 16, 50.0f, 48000.0f);
  lim.setCeilingDb(-12.0f);
  std::vector<float> in(2048, 0.9f), out(2048);
  lim.process(in.data(), out.data(), in.size());
  float peak = 0; for (float v : out) peak = std::max(peak, std::fabs(v));
  const float ceilLin = std::pow(10.0f, -12.0f / 20.0f);
  EXPECT_LE(peak, ceilLin + 1e-3f);
}
```
(Ensure `#include <cmath>`, `#include <vector>`, `#include <algorithm>` at the top.)

Append to `tests/denoise-core/test_biquad.cpp`:

```cpp
TEST(Biquad, SetHighpassRetunesWithoutResettingState) {
  vc::Biquad bq = vc::Biquad::highpass(48000.0, 80.0);
  std::vector<float> in(64, 1.0f), out(64);
  bq.process(in.data(), out.data(), in.size());   // build up filter state
  bq.setHighpass(48000.0, 200.0);                  // retune, keep state
  // DC input through a high-pass should trend toward 0; no NaN/blowup after retune.
  bq.process(in.data(), out.data(), in.size());
  for (float v : out) EXPECT_TRUE(std::isfinite(v));
  EXPECT_LT(std::fabs(out.back()), 1.0f);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Expected: compile error — `setTargetDb` / `setCeilingDb` / `setHighpass` not members.

- [ ] **Step 3: Add the setters**

In `src/denoise-core/agc.h`, add a public method (near `reset`):

```cpp
  void setTargetDb(float db) noexcept { targetLin_ = std::pow(10.0f, db / 20.0f); }
```
Add `#include <cmath>` to `agc.h` if not present.

In `src/denoise-core/limiter.h`, add a public method:

```cpp
  void setCeilingDb(float db) noexcept;
```
In `src/denoise-core/limiter.cpp`, implement it (needs `<cmath>`, already included):

```cpp
void Limiter::setCeilingDb(float db) noexcept { ceiling_ = std::pow(10.0f, db / 20.0f); }
```

In `src/denoise-core/biquad.h`, add a public method:

```cpp
  void setHighpass(double fs, double fc, double Q = 0.70710678) noexcept;  // retune, keep state
```
In `src/denoise-core/biquad.cpp`, implement it by recomputing the coefficients in place (do NOT touch
`x1_..y2_`):

```cpp
void Biquad::setHighpass(double fs, double fc, double Q) noexcept {
  constexpr double kPi = 3.14159265358979323846;
  const double w0 = 2.0 * kPi * fc / fs;
  const double c = std::cos(w0);
  const double s = std::sin(w0);
  const double alpha = s / (2.0 * Q);
  const double b0 = (1.0 + c) / 2.0, b1 = -(1.0 + c), b2 = (1.0 + c) / 2.0;
  const double a0 = 1.0 + alpha, a1 = -2.0 * c, a2 = 1.0 - alpha;
  b0_ = static_cast<float>(b0 / a0);
  b1_ = static_cast<float>(b1 / a0);
  b2_ = static_cast<float>(b2 / a0);
  a1_ = static_cast<float>(a1 / a0);
  a2_ = static_cast<float>(a2 / a0);
}
```
(Optional DRY: `highpass` may delegate to a fresh `Biquad` then `setHighpass`; leave as-is to avoid
touching validated code.)

- [ ] **Step 4: Build + run the leaf tests**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Then: `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R "Agc|Limiter|Biquad"`
Expected: PASS (new + existing leaf tests).

- [ ] **Step 5: Commit**

```bash
git add src/denoise-core/agc.h src/denoise-core/limiter.h src/denoise-core/limiter.cpp src/denoise-core/biquad.h src/denoise-core/biquad.cpp tests/denoise-core/test_agc.cpp tests/denoise-core/test_limiter.cpp tests/denoise-core/test_biquad.cpp
git commit -m "feat(denoise-core): live setters on Agc/Limiter/Biquad (retune without state reset)"
```

---

## Task 4: `DspChain` ramped params + bypass

**Files:**
- Modify: `src/denoise-core/dsp_chain.h`, `src/denoise-core/dsp_chain.cpp`
- Modify: `tests/denoise-core/test_dsp_chain.cpp`

**Design:** `DspChain` keeps a *current* and *target* for `hpfCutoffHz`, `agcTargetDb`, `limiterCeilingDb`.
Setters update the target. Each `process_frame`, a one-pole step (`coef = e^{-Δt/τ}`, Δt = 10 ms, τ = 20 ms
→ ~0.6065) moves current toward target, then the current value is pushed into the leaf. Currents
**initialize to the constructor config** (not 0) so default behavior is unchanged before any setter call.
`setBypass(true)` makes `process_frame` copy input to output.

- [ ] **Step 1: Write the failing tests**

Append to `tests/denoise-core/test_dsp_chain.cpp` (ensure `#include "passthrough_filter.h"`, `<cmath>`,
`<vector>` are present):

```cpp
TEST(DspChain, CurrentsInitializeToConfigNotZero) {
  vc::PassthroughFilter eng;
  vc::DspChain::Config cfg;              // hpf 80, agc -15, lim -1
  vc::DspChain chain(&eng, cfg);
  EXPECT_FLOAT_EQ(chain.currentHpfCutoffHz(), 80.0f);
  EXPECT_FLOAT_EQ(chain.currentAgcTargetDb(), -15.0f);
  EXPECT_FLOAT_EQ(chain.currentLimiterCeilingDb(), -1.0f);
}
TEST(DspChain, HpfCutoffRampsMonotonicallyToTarget) {
  vc::PassthroughFilter eng;
  vc::DspChain chain(&eng, vc::DspChain::Config{});
  chain.setHpfCutoffHz(200.0f);
  std::vector<float> in(vc::FRAME_SAMPLES, 0.0f), out(vc::FRAME_SAMPLES);
  float prev = chain.currentHpfCutoffHz();
  bool monotonic = true;
  for (int f = 0; f < 20; ++f) {
    chain.process_frame(in.data(), out.data());
    float cur = chain.currentHpfCutoffHz();
    if (cur < prev - 1e-3f) monotonic = false;   // never decreases toward a higher target
    prev = cur;
  }
  EXPECT_TRUE(monotonic);
  EXPECT_NEAR(chain.currentHpfCutoffHz(), 200.0f, 1.0f);  // arrived
}
TEST(DspChain, NoSingleFrameJumpOnParamChange) {
  vc::PassthroughFilter eng;
  vc::DspChain chain(&eng, vc::DspChain::Config{});
  chain.setAgcTargetDb(0.0f);            // big change from -15
  float prev = chain.currentAgcTargetDb();
  std::vector<float> in(vc::FRAME_SAMPLES, 0.0f), out(vc::FRAME_SAMPLES);
  for (int f = 0; f < 5; ++f) {
    chain.process_frame(in.data(), out.data());
    float step = std::fabs(chain.currentAgcTargetDb() - prev);
    EXPECT_LT(step, 10.0f);              // no full-jump in one frame (full jump would be 15 dB)
    prev = chain.currentAgcTargetDb();
  }
}
TEST(DspChain, BypassCopiesInputToOutput) {
  vc::PassthroughFilter eng;
  vc::DspChain chain(&eng, vc::DspChain::Config{});
  chain.setBypass(true);
  std::vector<float> in(vc::FRAME_SAMPLES), out(vc::FRAME_SAMPLES);
  for (size_t i = 0; i < in.size(); ++i) in[i] = 0.01f * static_cast<float>(i % 50);
  chain.process_frame(in.data(), out.data());
  for (size_t i = 0; i < in.size(); ++i) EXPECT_FLOAT_EQ(out[i], in[i]);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Expected: compile error — `setHpfCutoffHz` / `currentHpfCutoffHz` / `setBypass` etc. not members.

- [ ] **Step 3: Extend the `DspChain` header**

In `src/denoise-core/dsp_chain.h`, add public methods and private ramp state. Replace the class body's
public/private sections to include:

Public (after `bool vadActive() const noexcept ...`):
```cpp
  void setHpfCutoffHz(float hz) noexcept       { hpfTarget_ = hz; }
  void setAgcTargetDb(float db) noexcept       { agcTarget_ = db; }
  void setLimiterCeilingDb(float db) noexcept  { limTarget_ = db; }
  void setBypass(bool on) noexcept             { bypass_ = on; }
  float currentHpfCutoffHz() const noexcept    { return hpfCur_; }
  float currentAgcTargetDb() const noexcept    { return agcCur_; }
  float currentLimiterCeilingDb() const noexcept { return limCur_; }
```
Private — add these at the **end** of the private section, after `float t2_[FRAME_SAMPLES];` (so the
member declaration order matches the constructor initializer-list order in Step 4, avoiding MSVC
reorder warnings):
```cpp
  float fs_;
  float hpfCur_, hpfTarget_;
  float agcCur_, agcTarget_;
  float limCur_, limTarget_;
  bool  bypass_ = false;
  static constexpr float kRampCoef = 0.6065f;   // e^{-10ms/20ms}
```

- [ ] **Step 4: Update the constructor + `process_frame` + `reset`**

In `src/denoise-core/dsp_chain.cpp`, extend the constructor initializer list (after `holdSamples_(...)`):
```cpp
      ,
      fs_(c.fs),
      hpfCur_(c.hpfCutoffHz), hpfTarget_(c.hpfCutoffHz),
      agcCur_(c.agcTargetDb), agcTarget_(c.agcTargetDb),
      limCur_(c.limiterCeilingDb), limTarget_(c.limiterCeilingDb) {}
```

At the very top of `process_frame`, add the bypass short-circuit and the per-frame ramp:
```cpp
void DspChain::process_frame(const float* in, float* out) noexcept {
  if (bypass_) {
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) out[i] = in[i];
    return;
  }
  // Ramp params toward targets (one-pole, ~20 ms) and push into the leaves.
  hpfCur_ = kRampCoef * hpfCur_ + (1.0f - kRampCoef) * hpfTarget_;
  agcCur_ = kRampCoef * agcCur_ + (1.0f - kRampCoef) * agcTarget_;
  limCur_ = kRampCoef * limCur_ + (1.0f - kRampCoef) * limTarget_;
  hpf_.setHighpass(fs_, hpfCur_);
  agc_.setTargetDb(agcCur_);
  lim_.setCeilingDb(limCur_);
  // ... existing body unchanged from here (DC blocker, engine, hpf, VAD, AGC, limiter) ...
```
Leave the rest of the existing `process_frame` body as-is.

In `reset()`, snap currents to targets so a reset does not re-ramp (add before the closing brace):
```cpp
  hpfCur_ = hpfTarget_; agcCur_ = agcTarget_; limCur_ = limTarget_;
```

- [ ] **Step 5: Build + run the DspChain tests**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Then: `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R "DspChain"`
Expected: PASS (new + existing DspChain tests). If an existing DspChain test compares against fixed
leaves, it still holds because currents initialize to the config values (frame 1 uses config values).

- [ ] **Step 6: Guard the DFN3 golden did not move**

Run the full test suite to confirm no regression:
`"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure`
Expected: all previously-green tests still pass (the offline DFN3 golden is an integration check via
`offline-cli`, not run here; the unit suite must be fully green).

- [ ] **Step 7: Commit**

```bash
git add src/denoise-core/dsp_chain.h src/denoise-core/dsp_chain.cpp tests/denoise-core/test_dsp_chain.cpp
git commit -m "feat(denoise-core): DspChain ramped HPF/AGC/limiter params + bypass"
```

---

## Task 5: `DriftController::seedPpm` warm-start

**Files:**
- Modify: `src/transport-core/drift_control.h`, `src/transport-core/drift_control.cpp`
- Modify: `tests/transport-core/test_drift_control.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/transport-core/test_drift_control.cpp`:

```cpp
TEST(DriftController, SeedPpmWarmStartsTheRatio) {
  DriftController c({512, 1e-6, 1e-8, 150, 0.2});
  c.seedPpm(-26.0);
  double ratio = c.update(512.0);          // occupancy exactly at setpoint => err ~ 0
  EXPECT_NEAR((ratio - 1.0) * 1e6, -26.0, 1.0);   // first output reflects the seed
}
TEST(DriftController, SeedIsClampedToMaxPpm) {
  DriftController c({512, 1e-6, 1e-8, 100, 0.2});
  c.seedPpm(-9999.0);
  double ratio = c.update(512.0);
  EXPECT_GE((ratio - 1.0) * 1e6, -100.0 - 1e-6);  // not below -maxPpm
}
TEST(DriftController, NoSeedReproducesColdBehavior) {
  DriftController c({512, 1e-6, 1e-8, 150, 0.2});   // seedPpm NOT called
  double ratio = 1.0; for (int i = 0; i < 5000; ++i) ratio = c.update(512.0);
  EXPECT_NEAR(ratio, 1.0, 5e-6);                     // same as AtSetpointRatioNearOne
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Expected: compile error — `seedPpm` not a member.

- [ ] **Step 3: Declare + implement `seedPpm`**

In `src/transport-core/drift_control.h`, add to the `DriftController` public section (after `update`):

```cpp
  void seedPpm(double ppm) noexcept;   // warm-start the integrator/ratio from a cached drift ppm
```

In `src/transport-core/drift_control.cpp`, add (after `update`):

```cpp
void DriftController::seedPpm(double ppm) noexcept {
  const double lo = 1.0 - cfg_.maxPpm * 1e-6;
  const double hi = 1.0 + cfg_.maxPpm * 1e-6;
  double r = 1.0 + ppm * 1e-6;
  ratio_ = r < lo ? lo : (r > hi ? hi : r);
  // Place the bulk of the seed on the integral term so the very first update reproduces `ratio_`
  // (err starts ~0 because we prime the occupancy LPF at the setpoint).
  integ_ = (cfg_.ki != 0.0) ? (ratio_ - 1.0) / cfg_.ki : 0.0;
  lpf_.set(cfg_.setpoint);
}
```
(`ratio_`, `integ_`, `lpf_`, `cfg_` are existing private members; no header layout change beyond the
new method.)

- [ ] **Step 4: Build + run the drift tests**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug`
Then: `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure -R "DriftController|LeakyIntegrator"`
Expected: PASS (new + all existing drift tests, including the untouched cold-start cases).

- [ ] **Step 5: Commit**

```bash
git add src/transport-core/drift_control.h src/transport-core/drift_control.cpp tests/transport-core/test_drift_control.cpp
git commit -m "feat(transport-core): DriftController::seedPpm drift warm-start"
```

---

## Final verification

- [ ] **Run the entire suite**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure`
Expected: every test passes (the original ~55 plus the new `vc-ipc`, leaf, DspChain, and drift tests).

- [ ] **Confirm the windows.h guards held**

`vc-ipc`, `denoise-core`, and `transport-core` must still not include `<windows.h>`. Quick check:
`grep -rl "windows.h" src/vc-ipc src/denoise-core src/transport-core` should print nothing.

---

## What Phase 1 delivers / what comes next

**Delivered:** the shared `Params`/`Telemetry` types + a tested command codec; a live-tunable `DspChain`
(HPF/output-gain/limiter, ramped, plus bypass); a seedable `DriftController`. No UI, no IPC transport,
no behavior change at defaults.

**Next phases (own plans, written against these concrete interfaces):**
- **Phase 2 — Worker IPC:** named-pipe server + command thread (maps `Command`→`DspChain` setters /
  rebuild / `seedPpm`), shared-memory `Telemetry` writer, `Params` double-buffer + `PublishParams`,
  device re-select/rebuild path; replace the stdin `1`/`2` key reader. Rename `phase0-app` →
  `voicecleaner-worker`. Includes the **live aggression** task (mask-shaping in the DFN3 stream adapter).
- **Phase 3 — UI skeleton:** `voicecleaner.exe` single window + tray icon; spawn/attach/supervise;
  engine switch + bypass over the pipe.
- **Phase 4 — UI settings surface:** Direct2D meter, mic picker, tuning sliders, Advanced section,
  `settings.toml` + per-mic profiles + drift-ppm persistence.
- **Phase 5 — First-run stub + polish.**
