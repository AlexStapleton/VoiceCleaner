# Phase 2c — On-Demand Lean Engine Swap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a lean worker (`warm_standby = false`) switch engines gap-free — loading the target engine on demand, crossfading it in, then freeing the outgoing engine — while warm standby keeps today's instant switch.

**Architecture:** A new `DynamicEngine` container (`denoise-core`) generalizes today's `SwitchableEngine`: two atomic engine slots, an equal-power crossfade, and two modes — **warm** (both resident, both run every frame, never retire) and **lean** (active-only when idle, on-demand load, eager retire). The RT relay thread hands retired engines to a lock-free SPSC queue; a non-RT reclaim step (the worker's ~100 ms main-loop tick) frees them. All non-RT container mutation is serialized by a bridge-owned command mutex; the RT path is lock-free.

**Tech Stack:** C++20, GoogleTest, existing `vc::SpscRing<T>` (`transport-core/spsc_ring.h`), `vc::INoiseFilter` (virtual dtor), `vc::FRAME_SAMPLES = 480`.

**Spec:** [`docs/superpowers/specs/2026-08-16-product-shell-phase2c-lean-engine-swap-design.md`](../specs/2026-08-16-product-shell-phase2c-lean-engine-swap-design.md)

**Build environment (this machine):** CMake is **not on PATH** — use the Espressif-bundled toolchain, and the
generator is **Visual Studio 17 2022** (multi-config → pass `--config Debug` / `-C Debug`). The `build/` dir is
already configured. Every `cmake`/`ctest` command below maps to:

```bash
"C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe" --build build --target <target> --config Debug
"C:/Espressif/tools/cmake/4.0.3/bin/ctest.exe"  --test-dir build -C Debug -R <regex> --output-on-failure
```

---

## File Structure

| File | Responsibility |
|---|---|
| `src/denoise-core/dynamic_engine.h` (create) | `DynamicEngine` interface: modes, `Loader` seam, `Config`, RT `ProcessFrame`, non-RT `requestActive`/`reclaim`. |
| `src/denoise-core/dynamic_engine.cpp` (create) | Implementation: crossfade + delay-align (ported from `SwitchableEngine`), on-demand install, RT-local retire, SPSC reclaim. |
| `src/denoise-core/CMakeLists.txt` (modify) | Add `dynamic_engine.cpp` to `denoise-core`. |
| `tests/denoise-core/test_dynamic_engine.cpp` (create) | Full state-machine coverage with a lifetime-tracking fake engine, single-threaded deterministic call sequences. |
| `tests/denoise-core/CMakeLists.txt` (modify) | Add the new test file. |
| `src/voicecleaner-worker/bridge.h` (modify) | Replace `leafA_/leafB_/switch_` with `engine_` + `DynamicEngine* dyn_`; add command mutex + retired-DFN3 failedHops counter; `request_engine` non-noexcept; `reclaimRetiredEngines()`. |
| `src/voicecleaner-worker/bridge.cpp` (modify) | Build `DynamicEngine` in `start()` (loader lambda, `{0,480}` delays); route `request_engine`/`reclaim`; `dfn3Adapter_` dangle fix; telemetry via `dyn_`; final drain in `stop()`. |
| `src/voicecleaner-worker/main.cpp` (modify) | Call `bridge.reclaimRetiredEngines()` on the existing ~100 ms loop. |
| `src/denoise-core/switchable_engine.{h,cpp}` (delete, final task) | Superseded by `DynamicEngine`. |
| `tests/denoise-core/test_switchable_engine.cpp` (delete, final task) | Behavior ported into `test_dynamic_engine.cpp`. |

**Index convention (everywhere):** `0 = RNNoise`, `1 = DFN3`.

---

## Task 1: `DynamicEngine` header + warm-mode steady state

**Files:**
- Create: `src/denoise-core/dynamic_engine.h`
- Create: `src/denoise-core/dynamic_engine.cpp`
- Modify: `src/denoise-core/CMakeLists.txt:1`
- Create: `tests/denoise-core/test_dynamic_engine.cpp`
- Modify: `tests/denoise-core/CMakeLists.txt:1`

- [ ] **Step 1: Write the header**

Create `src/denoise-core/dynamic_engine.h`:

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "inoise_filter.h"
#include "spsc_ring.h"
#include "frame.h"   // FRAME_SAMPLES
namespace vc {

// Two-slot engine container (index 0 = RNNoise, 1 = DFN3) with an equal-power crossfade.
//  Warm: both leaves resident, BOTH run every frame (inactive stays warm), never retire.
//  Lean: only the active leaf resident; a switch loads the target on demand (off the RT thread),
//        crossfades it in, then retires (frees) the outgoing leaf -> back to lean.
// Output algorithmic delay is pinned to max(delaySamples[0], delaySamples[1]) in BOTH modes so a
// switch never shifts output timing.
//
// Threading: ProcessFrame() runs on the RT thread. requestActive()/reclaim() run on non-RT threads
// and MUST be serialized by the caller (one command mutex). The RT path takes no locks.
class DynamicEngine : public INoiseFilter {
public:
  enum class Mode { Warm, Lean };
  using Loader = std::function<std::unique_ptr<INoiseFilter>(int idx)>;  // idx 0/1 -> engine; may throw

  struct Config {
    Mode     mode = Mode::Lean;
    int      initialActive = 0;           // 0 = RNNoise, 1 = DFN3
    int      fadeFrames = 15;
    uint32_t delaySamples[2] = {0, 0};    // each engine's own algorithmic delay
  };

  DynamicEngine(Loader loader, const Config& cfg);   // builds initial leaf(s); may throw from loader
  ~DynamicEngine() override;

  // INoiseFilter (RT thread):
  void     ProcessFrame(const float* in, float* out) noexcept override;
  float    LastVadProbability() const noexcept override;
  void     SetAggression(float z) noexcept override;
  uint32_t AlgorithmicDelaySamples() const noexcept override { return maxDelay_; }

  // Non-RT control (serialize externally):
  void requestActive(int idx);                          // installs idx on demand if absent, then requests the fade
  int  activeIndex() const noexcept { return cur_; }
  bool resident(int idx) const noexcept { return eng_[clamp01(idx)].load(std::memory_order_acquire) != nullptr; }

  // Non-RT reclaim (serialize externally; never on the RT thread). Drains retired leaves; for each,
  // calls onRetire(ptr, idx) BEFORE deleting, then deletes.
  void reclaim(const std::function<void(INoiseFilter*, int)>& onRetire = {}) noexcept;

private:
  struct Retired { INoiseFilter* ptr; int idx; };
  static int clamp01(int i) noexcept { return i < 0 ? 0 : (i > 1 ? 1 : i); }
  float pushDelay(int slot, float x) noexcept;
  void  installLeaf(int idx);   // loader + aggression + delay-line reset + publish (non-RT)

  Loader   loader_;
  Mode     mode_;
  int      fadeFrames_;
  uint32_t maxDelay_ = 0;
  int      alignDelay_[2] = {0, 0};      // maxDelay_ - delaySamples[idx]
  std::vector<float> dline_[2];          // per-slot alignment buffers (size alignDelay_+1)
  int      dpos_[2] = {0, 0};

  std::atomic<INoiseFilter*> eng_[2]{nullptr, nullptr};  // owning raw pointers; nullptr = not resident
  std::atomic<int>   want_{0};
  std::atomic<float> lastAggression_{0.0f};
  int cur_ = 0, from_ = 0, fadeLeft_ = 0;                // RT-owned
  SpscRing<Retired>  retiredQ_{4};                       // RT producer, reclaim consumer
};
}  // namespace vc
```

- [ ] **Step 2: Write the implementation (ctor, dtor, delay, aggression, ProcessFrame)**

Create `src/denoise-core/dynamic_engine.cpp`:

```cpp
#include "dynamic_engine.h"
#include <algorithm>
#include <cmath>
namespace vc {

DynamicEngine::DynamicEngine(Loader loader, const Config& cfg)
    : loader_(std::move(loader)), mode_(cfg.mode), fadeFrames_(cfg.fadeFrames) {
  maxDelay_ = std::max(cfg.delaySamples[0], cfg.delaySamples[1]);
  for (int i = 0; i < 2; ++i) {
    alignDelay_[i] = static_cast<int>(maxDelay_ - cfg.delaySamples[i]);
    dline_[i].assign(static_cast<size_t>(alignDelay_[i]) + 1, 0.0f);
  }
  const int init = clamp01(cfg.initialActive);
  cur_ = from_ = init;
  want_.store(init, std::memory_order_relaxed);
  // Build the initial resident leaf(s). Warm: both; Lean: only the active. (May throw -> propagate.)
  if (mode_ == Mode::Warm) { installLeaf(0); installLeaf(1); }
  else                     { installLeaf(init); }
}

DynamicEngine::~DynamicEngine() {
  for (int i = 0; i < 2; ++i) delete eng_[i].exchange(nullptr, std::memory_order_acq_rel);
  Retired r;
  while (retiredQ_.try_pop(r)) delete r.ptr;
}

float DynamicEngine::pushDelay(int slot, float x) noexcept {
  if (alignDelay_[slot] == 0) return x;
  float y = dline_[slot][static_cast<size_t>(dpos_[slot])];
  dline_[slot][static_cast<size_t>(dpos_[slot])] = x;
  dpos_[slot] = (dpos_[slot] + 1) % static_cast<int>(dline_[slot].size());
  return y;
}

float DynamicEngine::LastVadProbability() const noexcept {
  INoiseFilter* e = eng_[cur_].load(std::memory_order_acquire);
  return e ? e->LastVadProbability() : 0.0f;
}

void DynamicEngine::SetAggression(float z) noexcept {
  lastAggression_.store(z, std::memory_order_relaxed);
  for (int i = 0; i < 2; ++i) {
    INoiseFilter* e = eng_[i].load(std::memory_order_acquire);
    if (e) e->SetAggression(z);
  }
}

void DynamicEngine::installLeaf(int idx) {
  std::unique_ptr<INoiseFilter> leaf = loader_(idx);                 // may throw -> caller aborts, no state mutated
  leaf->SetAggression(lastAggression_.load(std::memory_order_relaxed));
  std::fill(dline_[idx].begin(), dline_[idx].end(), 0.0f);           // fresh alignment buffer
  dpos_[idx] = 0;
  eng_[idx].store(leaf.release(), std::memory_order_release);        // publish (all writes above are ordered-before)
}

void DynamicEngine::ProcessFrame(const float* in, float* out) noexcept {
  const int want = want_.load(std::memory_order_acquire);
  if (want != cur_ && fadeLeft_ == 0) { from_ = cur_; cur_ = want; fadeLeft_ = fadeFrames_; }

  INoiseFilter* e0 = eng_[0].load(std::memory_order_acquire);
  INoiseFilter* e1 = eng_[1].load(std::memory_order_acquire);
  const bool fading  = fadeLeft_ > 0;
  const bool runBoth = fading || mode_ == Mode::Warm;

  if (runBoth) {
    float o0[FRAME_SAMPLES], o1[FRAME_SAMPLES];
    if (e0) e0->ProcessFrame(in, o0); else std::fill(o0, o0 + FRAME_SAMPLES, 0.0f);
    if (e1) e1->ProcessFrame(in, o1); else std::fill(o1, o1 + FRAME_SAMPLES, 0.0f);
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
      const float d0 = pushDelay(0, o0[i]);
      const float d1 = pushDelay(1, o1[i]);
      const float target = (cur_ == 0) ? d0 : d1;
      if (fadeLeft_ > 0) {
        const int frameIndex = fadeFrames_ - fadeLeft_;
        const float p = (fadeFrames_ <= 1) ? 1.0f
                        : static_cast<float>(frameIndex) / static_cast<float>(fadeFrames_ - 1);
        const float th = p * 1.5707963f;                 // 0..pi/2
        const float src = (from_ == 0) ? d0 : d1;
        out[i] = std::cos(th) * src + std::sin(th) * target;
      } else {
        out[i] = target;
      }
    }
  } else {
    // Lean, not fading: run ONLY the active leaf; keep its delay line advancing.
    INoiseFilter* ec = (cur_ == 0) ? e0 : e1;
    float tmp[FRAME_SAMPLES];
    if (ec) ec->ProcessFrame(in, tmp); else std::fill(tmp, tmp + FRAME_SAMPLES, 0.0f);
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) out[i] = pushDelay(cur_, tmp[i]);
  }

  if (fadeLeft_ > 0 && --fadeLeft_ == 0 && mode_ == Mode::Lean) {
    // Retire the outgoing leaf UNLESS a switch back to it arrived mid-fade (RT-local decision -> no race).
    if (want_.load(std::memory_order_acquire) != from_) {
      INoiseFilter* p = eng_[from_].exchange(nullptr, std::memory_order_acq_rel);
      if (p && !retiredQ_.try_push(Retired{p, from_}))
        eng_[from_].store(p, std::memory_order_release);   // queue full (should never happen): keep resident, no leak
    }
  }
}

void DynamicEngine::requestActive(int idx) {
  idx = clamp01(idx);
  if (eng_[idx].load(std::memory_order_acquire) == nullptr) installLeaf(idx);  // may throw -> want_ untouched
  want_.store(idx, std::memory_order_release);
}

void DynamicEngine::reclaim(const std::function<void(INoiseFilter*, int)>& onRetire) noexcept {
  Retired r;
  while (retiredQ_.try_pop(r)) {
    if (onRetire) onRetire(r.ptr, r.idx);
    delete r.ptr;
  }
}

}  // namespace vc
```

- [ ] **Step 3: Wire into CMake**

In `src/denoise-core/CMakeLists.txt:1`, append `dynamic_engine.cpp` to the `add_library(denoise-core ...)` source list (before the closing paren):

```cmake
add_library(denoise-core passthrough_filter.cpp dc_blocker.cpp biquad.cpp aggression.cpp limiter.cpp agc.cpp dsp_chain.cpp rnnoise_filter.cpp stft.cpp erb.cpp dfn3_features.cpp dfn3_postdsp.cpp switchable_engine.cpp dynamic_engine.cpp)
```

- [ ] **Step 4: Write the failing test (warm steady state + shared fake)**

Create `tests/denoise-core/test_dynamic_engine.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include <vector>
#include "dynamic_engine.h"
using namespace vc;

// Lifetime- and call-tracking fake. All instances of one logical index share an EngineProbe, so a
// test can assert liveness (ctor/dtor balance -> detects leak/double-free/UAF), call counts, and the
// last aggression pushed in.
struct EngineProbe {
  float gain = 1.0f;
  uint32_t delay = 0;
  int ctorCount = 0, dtorCount = 0, processCount = 0;
  float lastAggression = -1.0f;
  int live() const { return ctorCount - dtorCount; }
};
struct ProbeEngine : INoiseFilter {
  EngineProbe* p;
  explicit ProbeEngine(EngineProbe* probe) : p(probe) { ++p->ctorCount; }
  ~ProbeEngine() override { ++p->dtorCount; }
  void ProcessFrame(const float* in, float* out) noexcept override {
    ++p->processCount; for (size_t i = 0; i < FRAME_SAMPLES; ++i) out[i] = p->gain * in[i];
  }
  float LastVadProbability() const noexcept override { return 0.0f; }
  void SetAggression(float z) noexcept override { p->lastAggression = z; }
  uint32_t AlgorithmicDelaySamples() const noexcept override { return p->delay; }
};

// Builds a loader over a caller-owned probes[2] and the matching Config.delaySamples.
static DynamicEngine::Loader probeLoader(EngineProbe* probes) {
  return [probes](int idx) -> std::unique_ptr<INoiseFilter> {
    return std::make_unique<ProbeEngine>(&probes[idx]);
  };
}

TEST(DynamicEngine, WarmSteadyStateEqualsActiveEngineDelayAligned) {
  EngineProbe pr[2]; pr[0] = {2.0f, 0}; pr[1] = {3.0f, 960};   // idx1 reports 2 frames extra latency
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Warm; c.initialActive = 0;
  c.fadeFrames = 1; c.delaySamples[0] = 0; c.delaySamples[1] = 960;
  DynamicEngine eng(probeLoader(pr), c);
  EXPECT_EQ(eng.AlgorithmicDelaySamples(), 960u);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);
  for (int f = 0; f < 4; ++f) eng.ProcessFrame(in.data(), out.data());   // prime the 960-sample delay line
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], 2.0f, 1e-4f);
  EXPECT_EQ(pr[0].live(), 1);
  EXPECT_EQ(pr[1].live(), 1);   // warm: both resident
  EXPECT_GT(pr[1].processCount, 0);   // warm: inactive engine still runs every frame
}
```

- [ ] **Step 5: Wire the test into CMake**

In `tests/denoise-core/CMakeLists.txt:1`, add `test_dynamic_engine.cpp` to the `add_executable(denoise-core-tests ...)` list.

- [ ] **Step 6: Configure + build + run; verify PASS**

Run (using the project's configured CMake build dir `build/`):

```bash
cmake --build build --target denoise-core-tests && ctest --test-dir build -R DynamicEngine --output-on-failure
```

Expected: `DynamicEngine.WarmSteadyStateEqualsActiveEngineDelayAligned` PASSES; both probes live; idx1 processed.

- [ ] **Step 7: Commit**

```bash
git add src/denoise-core/dynamic_engine.h src/denoise-core/dynamic_engine.cpp src/denoise-core/CMakeLists.txt tests/denoise-core/test_dynamic_engine.cpp tests/denoise-core/CMakeLists.txt
git commit -m "feat(denoise-core): DynamicEngine container — warm-mode steady state"
```

---

## Task 2: Warm-mode crossfade golden (regression parity with SwitchableEngine)

**Files:**
- Modify: `tests/denoise-core/test_dynamic_engine.cpp`

- [ ] **Step 1: Write the failing test**

This reproduces `test_switchable_engine.cpp`'s `EqualPowerGainLawOnSwitch` against `DynamicEngine` in warm mode — the golden that pins the crossfade unchanged. Append to `test_dynamic_engine.cpp`:

```cpp
TEST(DynamicEngine, WarmEqualPowerGainLawOnSwitch) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {0.0f, 0};   // "from" passes input; "to" outputs 0
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Warm; c.initialActive = 0;
  c.fadeFrames = 4; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);
  eng.ProcessFrame(in.data(), out.data());
  EXPECT_NEAR(out[0], 1.0f, 1e-4f);                          // steady on engine 0 before the switch
  eng.requestActive(1);
  const float expect[4] = {1.0f, 0.86603f, 0.5f, 0.0f};     // cos(0, pi/6, pi/3, pi/2)
  for (int f = 0; f < 4; ++f) {
    eng.ProcessFrame(in.data(), out.data());
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], expect[f], 1e-4f) << "fade frame " << f;
  }
  eng.ProcessFrame(in.data(), out.data());
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], 0.0f, 1e-4f);   // settled on engine 1
  EXPECT_EQ(eng.activeIndex(), 1);
  EXPECT_EQ(pr[0].live(), 1);   // warm: nothing retired
  EXPECT_EQ(pr[1].live(), 1);
}
```

- [ ] **Step 2: Build + run; verify PASS**

```bash
cmake --build build --target denoise-core-tests && ctest --test-dir build -R "DynamicEngine.Warm" --output-on-failure
```

Expected: both warm tests PASS (crossfade values match the pre-existing `SwitchableEngine` golden; nothing retired in warm mode).

- [ ] **Step 3: Commit**

```bash
git add tests/denoise-core/test_dynamic_engine.cpp
git commit -m "test(denoise-core): DynamicEngine warm crossfade golden (SwitchableEngine parity)"
```

---

## Task 3: Lean-mode steady state runs only the active engine

**Files:**
- Modify: `tests/denoise-core/test_dynamic_engine.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST(DynamicEngine, LeanSteadyStateRunsOnlyActive) {
  EngineProbe pr[2]; pr[0] = {2.0f, 0}; pr[1] = {3.0f, 480};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 15; c.delaySamples[0] = 0; c.delaySamples[1] = 480;
  DynamicEngine eng(probeLoader(pr), c);
  EXPECT_EQ(eng.AlgorithmicDelaySamples(), 480u);
  EXPECT_TRUE(eng.resident(0));
  EXPECT_FALSE(eng.resident(1));            // lean: only the active engine is loaded
  EXPECT_EQ(pr[1].live(), 0);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);
  for (int f = 0; f < 4; ++f) eng.ProcessFrame(in.data(), out.data());
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], 2.0f, 1e-4f);  // engine 0, delay-aligned
  EXPECT_EQ(pr[1].processCount, 0);         // idx1 never constructed, never run
}
```

- [ ] **Step 2: Build + run; verify PASS**

```bash
cmake --build build --target denoise-core-tests && ctest --test-dir build -R "DynamicEngine.LeanSteadyState" --output-on-failure
```

Expected: PASS — idx1 is never loaded or run when lean and idle. (Implementation already supports this from Task 1; this test locks the behavior.)

- [ ] **Step 3: Commit**

```bash
git add tests/denoise-core/test_dynamic_engine.cpp
git commit -m "test(denoise-core): DynamicEngine lean steady state runs only the active engine"
```

---

## Task 4: Lean switch — on-demand load, crossfade, retire to the reclaim queue

**Files:**
- Modify: `tests/denoise-core/test_dynamic_engine.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST(DynamicEngine, LeanSwitchLoadsCrossfadesRetires) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {0.0f, 0};   // from passes input, to outputs 0
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 4; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  eng.requestActive(1);                     // loads idx1 on demand (off-RT here: same thread, serialized)
  EXPECT_TRUE(eng.resident(1));
  EXPECT_EQ(pr[1].lastAggression, 0.0f);    // stored aggression (default 0) applied on install

  const float expect[4] = {1.0f, 0.86603f, 0.5f, 0.0f};
  for (int f = 0; f < 4; ++f) {
    eng.ProcessFrame(in.data(), out.data());
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], expect[f], 1e-4f) << "fade frame " << f;
  }
  EXPECT_EQ(eng.activeIndex(), 1);
  EXPECT_FALSE(eng.resident(0));            // idx0 retired out of the live slot at fade end
  EXPECT_EQ(pr[0].live(), 1);              // ...but not yet freed — sitting in the reclaim queue

  eng.reclaim();                            // non-RT drain
  EXPECT_EQ(pr[0].live(), 0);              // now freed
  EXPECT_EQ(pr[0].dtorCount, 1);           // exactly once — no double free
}
```

- [ ] **Step 2: Build + run; verify PASS**

```bash
cmake --build build --target denoise-core-tests && ctest --test-dir build -R "DynamicEngine.LeanSwitch" --output-on-failure
```

Expected: PASS — the target loads on demand with the stored aggression, crossfades identically to warm, and the outgoing engine leaves the live slot at fade end and is freed exactly once by `reclaim()`.

- [ ] **Step 3: Commit**

```bash
git add tests/denoise-core/test_dynamic_engine.cpp
git commit -m "test(denoise-core): DynamicEngine lean switch loads/crossfades/retires"
```

---

## Task 5: Reclaim independence — a lone switch still frees the outgoing engine

**Files:**
- Modify: `tests/denoise-core/test_dynamic_engine.cpp`

- [ ] **Step 1: Write the failing test**

Confirms retirement does not depend on further `requestActive` calls (gap A): after one switch and no more commands, a later `reclaim()` tick frees the outgoing engine.

```cpp
TEST(DynamicEngine, ReclaimIsIndependentOfFurtherCommands) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {2.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 2; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  eng.requestActive(1);
  for (int f = 0; f < 2; ++f) eng.ProcessFrame(in.data(), out.data());   // fade completes, idx0 retired
  // No further requestActive. Keep processing frames (steady on idx1).
  for (int f = 0; f < 50; ++f) eng.ProcessFrame(in.data(), out.data());
  EXPECT_EQ(pr[0].live(), 1);   // still queued (reclaim not yet called)

  int freed = 0;
  eng.reclaim([&](INoiseFilter*, int idx) { if (idx == 0) ++freed; });
  EXPECT_EQ(freed, 1);
  EXPECT_EQ(pr[0].live(), 0);
}
```

- [ ] **Step 2: Build + run; verify PASS**

```bash
cmake --build build --target denoise-core-tests && ctest --test-dir build -R "DynamicEngine.ReclaimIsIndependent" --output-on-failure
```

Expected: PASS — the retired engine waits in the queue and is freed by the reclaim tick regardless of command activity; `onRetire` sees `idx == 0`.

- [ ] **Step 3: Commit**

```bash
git add tests/denoise-core/test_dynamic_engine.cpp
git commit -m "test(denoise-core): DynamicEngine reclaim is independent of command arrival"
```

---

## Task 6: Rapid re-switch safety (fresh load; no double-free / UAF)

**Files:**
- Modify: `tests/denoise-core/test_dynamic_engine.cpp`

- [ ] **Step 1: Write the failing test**

Covers two hazards: (a) a completed switch, then a switch back BEFORE reclaim — must fresh-load, never resurrect the queued pointer; (b) a switch-back arriving mid-fade — must keep the outgoing engine resident (RT-local cancel), not retire-then-null.

```cpp
TEST(DynamicEngine, RapidReswitchAfterRetireFreshLoadsNoDoubleFree) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {2.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 2; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  eng.requestActive(1);
  for (int f = 0; f < 2; ++f) eng.ProcessFrame(in.data(), out.data());   // idx0 retired to queue (not yet freed)
  eng.requestActive(0);                                                  // switch back BEFORE reclaim
  EXPECT_EQ(pr[0].ctorCount, 2);                                         // fresh idx0 built (queued one not reused)
  for (int f = 0; f < 2; ++f) eng.ProcessFrame(in.data(), out.data());   // fade to fresh idx0; idx1 retired
  EXPECT_EQ(eng.activeIndex(), 0);

  eng.reclaim();
  EXPECT_EQ(pr[0].dtorCount, 1);   // only the FIRST idx0 instance was freed
  EXPECT_EQ(pr[0].live(), 1);      // the fresh idx0 is still resident
  EXPECT_EQ(pr[1].live(), 0);      // idx1 retired and freed
}

TEST(DynamicEngine, ReswitchMidFadeKeepsOutgoingResident) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {2.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 6; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  eng.requestActive(1);
  for (int f = 0; f < 3; ++f) eng.ProcessFrame(in.data(), out.data());   // partway through the 0->1 fade
  eng.requestActive(0);                                                  // switch back mid-fade
  for (int f = 0; f < 20; ++f) eng.ProcessFrame(in.data(), out.data());  // finish, then fade 1->0
  EXPECT_EQ(eng.activeIndex(), 0);
  EXPECT_EQ(pr[0].ctorCount, 1);   // idx0 was NEVER retired/reloaded (RT-local cancel kept it resident)
  eng.reclaim();
  EXPECT_EQ(pr[0].live(), 1);      // idx0 still the resident engine
  EXPECT_EQ(pr[1].live(), 0);      // idx1 retired + freed
}
```

- [ ] **Step 2: Build + run; verify PASS**

```bash
cmake --build build --target denoise-core-tests && ctest --test-dir build -R "DynamicEngine.RapidReswitch|DynamicEngine.ReswitchMidFade" --output-on-failure
```

Expected: PASS — no double-free (each `dtorCount` ≤ its `ctorCount`), a post-retire switch-back fresh-loads, and a mid-fade switch-back keeps the outgoing engine resident (never reloaded).

- [ ] **Step 3: Commit**

```bash
git add tests/denoise-core/test_dynamic_engine.cpp
git commit -m "test(denoise-core): DynamicEngine rapid-reswitch safety (no UAF/double-free)"
```

---

## Task 7: Loader failure and null-safe aggression

**Files:**
- Modify: `tests/denoise-core/test_dynamic_engine.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST(DynamicEngine, LoaderFailureAbortsSwitchCleanly) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {2.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 4; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  // Loader throws for idx1 (simulates model-load failure).
  DynamicEngine::Loader loader = [&](int idx) -> std::unique_ptr<INoiseFilter> {
    if (idx == 1) throw std::runtime_error("boom");
    return std::make_unique<ProbeEngine>(&pr[idx]);
  };
  DynamicEngine eng(loader, c);
  std::vector<float> in(FRAME_SAMPLES, 1.0f), out(FRAME_SAMPLES);

  EXPECT_THROW(eng.requestActive(1), std::runtime_error);
  EXPECT_EQ(eng.activeIndex(), 0);          // switch abandoned; still on idx0
  EXPECT_FALSE(eng.resident(1));            // no half-published slot
  eng.ProcessFrame(in.data(), out.data());  // still runs idx0, no crash
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) EXPECT_NEAR(out[i], 1.0f, 1e-4f);
}

TEST(DynamicEngine, SetAggressionIsNullSafeAndAppliedOnInstall) {
  EngineProbe pr[2]; pr[0] = {1.0f, 0}; pr[1] = {1.0f, 0};
  DynamicEngine::Config c; c.mode = DynamicEngine::Mode::Lean; c.initialActive = 0;
  c.fadeFrames = 2; c.delaySamples[0] = 0; c.delaySamples[1] = 0;
  DynamicEngine eng(probeLoader(pr), c);
  eng.SetAggression(0.7f);                  // idx1 not resident -> must not crash
  EXPECT_FLOAT_EQ(pr[0].lastAggression, 0.7f);
  eng.requestActive(1);
  EXPECT_FLOAT_EQ(pr[1].lastAggression, 0.7f);   // freshly installed leaf gets the stored value
}
```

- [ ] **Step 2: Build + run; verify PASS**

```bash
cmake --build build --target denoise-core-tests && ctest --test-dir build -R "DynamicEngine.Loader|DynamicEngine.SetAggression" --output-on-failure
```

Expected: PASS — a throwing loader leaves the engine untouched and running; `SetAggression` skips the empty slot and is applied to a later-installed leaf.

- [ ] **Step 3: Run the FULL denoise-core suite (nothing regressed)**

```bash
ctest --test-dir build -R denoise-core --output-on-failure
```

Expected: all denoise-core tests PASS, including the still-present `test_switchable_engine.cpp`.

- [ ] **Step 4: Commit**

```bash
git add tests/denoise-core/test_dynamic_engine.cpp
git commit -m "test(denoise-core): DynamicEngine loader-failure + null-safe aggression"
```

---

## Task 8: Bridge builds `DynamicEngine` in both modes

**Files:**
- Modify: `src/voicecleaner-worker/bridge.h:22` (include), `:40` (`request_engine`), `:54-57` (members)
- Modify: `src/voicecleaner-worker/bridge.cpp:43-62` (engine construction), `:86-96` (startup log)

- [ ] **Step 1: Update `bridge.h`**

Replace the `#include "switchable_engine.h"` at `bridge.h:22` with:

```cpp
#include "dynamic_engine.h"
#include <mutex>
```

Replace the members at `bridge.h:54-57`:

```cpp
  std::unique_ptr<INoiseFilter> leafA_, leafB_;  // warm-standby leaves (owned; borrowed by switch_)
  std::unique_ptr<INoiseFilter> engine_;   // constructed in start() (non-RT)
  SwitchableEngine*             switch_ = nullptr;  // non-owning; points into engine_ when warm-standby
  Dfn3StreamAdapter*            dfn3Adapter_ = nullptr;  // non-owning; set whenever a DFN3 adapter is live (observability)
```

with:

```cpp
  std::unique_ptr<INoiseFilter> engine_;   // constructed in start() (non-RT): DynamicEngine or PassthroughFilter
  DynamicEngine*                dyn_ = nullptr;  // non-owning; == engine_.get() when engine is rnnoise/dfn3, else null
  Dfn3StreamAdapter*            dfn3Adapter_ = nullptr;  // non-owning; the live DFN3 leaf, or null (observability)
  std::mutex                    engMutex_;         // serializes non-RT container mutation: request_engine + reclaim
  uint64_t                      dfn3FailedHopsRetired_ = 0;  // failedHops folded in before a DFN3 leaf is freed
```

Change `request_engine` at `bridge.h:40` from the inline no-op to a declaration (defined in the .cpp, no longer `noexcept`):

```cpp
  // Non-RT engine switch (idx 0 = rnnoise, 1 = dfn3). Installs the target on demand when lean, then
  // requests the crossfade. Serialized with reclaim by engMutex_. Catches loader failure internally.
  void request_engine(int idx);
  // Non-RT: free engines retired by the RT relay. Call from a non-RT loop (worker main tick).
  void reclaimRetiredEngines();
```

- [ ] **Step 2: Replace engine construction in `bridge.cpp`**

Replace the engine-construction block at `bridge.cpp:42-62` — the comment line, the whole `if (cfg_.warmStandby) {...} else if ... else ... engine_ = PassthroughFilter`, **and** the trailing `if (cfg_.dspChain) chain_ = ...` line — with:

```cpp
  // Engine: RNNoise (idx 0) / DFN3 (idx 1) live behind a DynamicEngine container that supports
  // warm standby (both resident) and lean on-demand switching. Passthrough ("none") stays static.
  // Per-engine algorithmic delays are known constants: RNNoise 0 (rnnoise_filter.h), DFN3 480
  // (dfn3_stream_adapter.h) — pinned so a switch never shifts output timing.
  auto loader = [this](int idx) -> std::unique_ptr<INoiseFilter> {
    if (idx == 0) return std::make_unique<RnnoiseFilter>();
    auto a = std::make_unique<Dfn3StreamAdapter>("models/dfn3");   // throws on model load failure
    dfn3Adapter_ = a.get();
    return a;
  };
  if (cfg_.engine == "rnnoise" || cfg_.engine == "dfn3") {
    DynamicEngine::Config dc;
    dc.mode = cfg_.warmStandby ? DynamicEngine::Mode::Warm : DynamicEngine::Mode::Lean;
    dc.initialActive = (cfg_.engine == "dfn3") ? 1 : 0;
    dc.fadeFrames = 15;
    dc.delaySamples[0] = 0; dc.delaySamples[1] = 480;
    auto de = std::make_unique<DynamicEngine>(loader, dc);   // builds initial leaf(s); may throw
    dyn_ = de.get();
    engine_ = std::move(de);
  } else {
    engine_ = std::make_unique<PassthroughFilter>();   // "none": static passthrough, no switching
  }
  if (cfg_.dspChain) chain_ = std::make_unique<DspChain>(engine_.get(), DspChain::Config{});
```

- [ ] **Step 3: Fix the startup log**

Replace the `if (cfg_.warmStandby) { std::printf(...switch_->activeIndex()...) } else {...}` block at `bridge.cpp:86-96` with a single log that works in both modes:

```cpp
  std::printf("bridge running: ring=%zu frames (primed %zu), engine=%s, warm_standby=%s, dsp_chain=%s, csv=%s\n",
              ringB_.capacity(), ringB_.capacity()/2, cfg_.engine.c_str(),
              cfg_.warmStandby ? "on" : "off", cfg_.dspChain ? "on" : "off", cfg_.csvPath.c_str());
```

- [ ] **Step 4: Build the worker; verify it compiles and runs headless**

```bash
cmake --build build --target voicecleaner-worker
```

Expected: compiles. (Full behavioral check comes in Task 10's integration run; here just confirm the build and that `request_engine`/`reclaimRetiredEngines` are declared — they're defined next task, so a link error naming them is expected until Task 9. If linking the worker now, temporarily expect unresolved `Bridge::request_engine`/`Bridge::reclaimRetiredEngines`.)

- [ ] **Step 5: Commit**

```bash
git add src/voicecleaner-worker/bridge.h src/voicecleaner-worker/bridge.cpp
git commit -m "feat(worker): build DynamicEngine (warm+lean) in Bridge::start"
```

---

## Task 9: Wire `request_engine` + telemetry through the container

**Files:**
- Modify: `src/voicecleaner-worker/bridge.cpp` (add methods; telemetry line `:152-153`)

- [ ] **Step 1: Implement `request_engine` and telemetry routing**

Add these method definitions to `bridge.cpp` (near the other `Bridge::` methods):

```cpp
void Bridge::request_engine(int idx) {
  std::lock_guard<std::mutex> lk(engMutex_);
  if (!dyn_) return;   // passthrough: no switching
  try {
    dyn_->requestActive(idx);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "engine switch to %d failed (kept current engine): %s\n", idx, e.what());
  }
}
```

Replace the telemetry `activeEngine` line at `bridge.cpp:152-153`:

```cpp
          t.activeEngine = switch_ ? (uint32_t)switch_->activeIndex()
                                   : (cfg_.engine == "dfn3" ? 1u : 0u);
```

with:

```cpp
          t.activeEngine = dyn_ ? (uint32_t)dyn_->activeIndex()
                                : (cfg_.engine == "dfn3" ? 1u : 0u);
```

- [ ] **Step 2: Build the worker**

```bash
cmake --build build --target voicecleaner-worker
```

Expected: compiles; `Bridge::reclaimRetiredEngines` is still unresolved if linked (defined in Task 10) — that's expected; do not run yet.

- [ ] **Step 3: Commit**

```bash
git add src/voicecleaner-worker/bridge.cpp
git commit -m "feat(worker): route request_engine + activeEngine telemetry through DynamicEngine"
```

---

## Task 10: Reclaim on the main-loop tick + `dfn3Adapter_` dangle fix + stop-order drain

**Files:**
- Modify: `src/voicecleaner-worker/bridge.cpp` (add `reclaimRetiredEngines`; `stop()` drain; shutdown `failedHops` read)
- Modify: `src/voicecleaner-worker/main.cpp:180-192` (call reclaim in the loop)

- [ ] **Step 1: Implement `reclaimRetiredEngines` and update the shutdown failedHops read**

Add to `bridge.cpp`:

```cpp
void Bridge::reclaimRetiredEngines() {
  std::lock_guard<std::mutex> lk(engMutex_);
  if (!dyn_) return;
  dyn_->reclaim([this](INoiseFilter* p, int idx) {
    if (idx == 1) {   // a DFN3 leaf is about to be freed: preserve its failedHops, drop the dangling observer
      dfn3FailedHopsRetired_ += static_cast<Dfn3StreamAdapter*>(p)->failedHops();
      if (dfn3Adapter_ == p) dfn3Adapter_ = nullptr;
    }
  });
}
```

Find the shutdown `failedHops` reporting at `bridge.cpp:202-205`:

```cpp
    if (dfn3Adapter_ && dfn3Adapter_->failedHops() > 0) {
      std::printf("dfn3 engine failures (silenced hops): %llu\n",
                  (unsigned long long)dfn3Adapter_->failedHops());
    }
```

Replace it so it sums the persistent counter with any still-resident adapter (and never dereferences a freed one):

```cpp
    const uint64_t dfn3Failed = dfn3FailedHopsRetired_ + (dfn3Adapter_ ? dfn3Adapter_->failedHops() : 0);
    if (dfn3Failed > 0) {
      std::printf("dfn3 engine failures (silenced hops): %llu\n", (unsigned long long)dfn3Failed);
    }
```

- [ ] **Step 2: Add the final drain to `stop()`**

In `Bridge::stop()`, AFTER `relay_.join()` (the RT thread is gone; `bridge.cpp:191-193`) and before returning, drain anything the last fade retired:

```cpp
  reclaimRetiredEngines();   // RT is joined; free any leaf the final fade retired
```

The `DynamicEngine` destructor (runs when `engine_` is reset/destroyed) frees still-resident leaves and drains its queue, so nothing leaks even if a retirement is pending.

- [ ] **Step 3: Call reclaim from the worker main loop**

In `main.cpp`, the run loop (`bridge.cpp` analog at `main.cpp:180-192`) sleeps ~100 ms per iteration. Add a reclaim call inside that loop, right after the sleep:

```cpp
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      bridge.reclaimRetiredEngines();   // free engines the RT relay retired after a lean switch
      if (bridge.device_lost()) {
```

- [ ] **Step 4: Build the full worker + link**

```bash
cmake --build build --target voicecleaner-worker
```

Expected: compiles and links cleanly (all `Bridge` methods now defined).

- [ ] **Step 5: Headless integration check (real hardware, dev keys)**

With a lean config (`warm_standby = false`, `engine = rnnoise`, `dsp_chain = true`, valid device IDs), run:

```bash
build/src/voicecleaner-worker/voicecleaner-worker --config drift.toml --dev-keys
```

Verify manually: audio flows; press `2` → DFN3 engages (gap-free, no dropout), RSS drops back toward one model within ~1 s; press `1` → back to RNNoise; rapid `1 2 1 2` → no crash, no stuck engine; Ctrl+C exits cleanly with no `failedHops` crash. Also smoke a warm config (`warm_standby = true`) → instant switches, unchanged behavior.

- [ ] **Step 6: Commit**

```bash
git add src/voicecleaner-worker/bridge.cpp src/voicecleaner-worker/main.cpp
git commit -m "feat(worker): reclaim retired engines on main tick; fix dfn3Adapter_ dangle; drain on stop"
```

---

## Task 11: Retire `SwitchableEngine`

**Files:**
- Delete: `src/denoise-core/switchable_engine.h`, `src/denoise-core/switchable_engine.cpp`
- Delete: `tests/denoise-core/test_switchable_engine.cpp`
- Modify: `src/denoise-core/CMakeLists.txt:1`, `tests/denoise-core/CMakeLists.txt:1`

- [ ] **Step 1: Confirm no remaining references**

Run: `grep -rn "switchable_engine\|SwitchableEngine" src/ tests/`
Expected: no matches outside the files being deleted. (The bridge now uses `DynamicEngine`; the warm golden lives in `test_dynamic_engine.cpp`.) If anything else references it, stop and reconcile before deleting.

- [ ] **Step 2: Delete the files and drop them from CMake**

```bash
git rm src/denoise-core/switchable_engine.h src/denoise-core/switchable_engine.cpp tests/denoise-core/test_switchable_engine.cpp
```

In `src/denoise-core/CMakeLists.txt:1`, remove `switchable_engine.cpp` from the `add_library` list. In `tests/denoise-core/CMakeLists.txt:1`, remove `test_switchable_engine.cpp` from the `add_executable` list.

- [ ] **Step 3: Full build + full test suite (nothing regressed)**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: everything builds; all tests PASS (the original 100 minus the 2 removed `Switchable` cases, plus the new `DynamicEngine` cases). Confirm the DFN3 offline golden and streaming Gates are green.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor(denoise-core): remove SwitchableEngine (superseded by DynamicEngine)"
```

---

## Self-Review Notes (coverage vs spec)

- Spec §2 modes (warm/lean, retire policy, pinned delay, 0/1 index, fadeFrames=15) → Tasks 1-4, 8.
- Spec §3 states (lean-at-rest active-only, switching runs both, warm runs both always) → Tasks 1-4.
- Spec §4 protocol (atomic slots, publication barrier, SPSC retire, RT-local cancel guard, push-fail re-store) → Tasks 1, 4, 6.
- Spec §5 bridge integration (loader, `{0,480}`, mutex, non-noexcept `request_engine`, reclaim on tick, `dfn3Adapter_` fix, telemetry, stop-order drain) → Tasks 8-10.
- Spec §6 passthrough static → Task 8 (`else` branch).
- Spec §7 tests (warm golden, lean steady, load/crossfade/retire, rapid re-switch, reclaim independence, loader failure, null-safe aggression, integration) → Tasks 2-7, 10.
- Spec §8 deliverables + no regression → Tasks 1-11; final suite in Task 11.
