# Whole-Program Performance Deep Dive

_Author: performance pass, 2026-08-21. Code-grounded review of the entire program's
performance management — real-time audio path, latency, CPU, memory, the two-process
shell, and observability. Complements the narrower
[RNNoise & Performance findings](2026-08-21-rnnoise-performance-findings.md); that doc
tracks the shipped RNNoise/latency/stability workstreams, this one audits the machine as
a whole and calls out what is genuinely well-built vs. what is a latent risk._

---

## 0. Executive summary

The program is, on the whole, **carefully engineered for real time** — SPSC rings passed by
value, VirtualLock'd + page-touched buffers, MMCSS "Pro Audio / CRITICAL" on every audio
thread, an allocation-free engine/DSP inner loop, lean on-demand engine loading, and a
crash-isolated two-process split. Frame-time is instrumented with p50/p99/max and has ~5×
tail headroom on RNNoise. Most of what a real-time audio app can get wrong, this one got
right.

There is **one real defect** that undercuts the rest, and a handful of smaller gaps:

| # | Finding | Severity | Where |
|---|---|---|---|
| **F1** | ~~`std::deque<float>` reframing buffers allocate/free on all three audio threads~~ **FIXED 2026-08-21** | **High (latent)** | `framer.h` / `bridge.cpp` |
| F2 | ~~Frame-time budget only times engine+chain — excludes resample/ring/reframe/WASAPI~~ **FIXED 2026-08-22** (whole-hop stats added) | Medium | `bridge.cpp` |
| F3 | ~~End-to-end latency is still estimated, never measured~~ **FIXED 2026-08-22** (per-device budget computed + reported) | Medium | `bridge.cpp`, `latency.h` |
| F4 | ~~~34 MB RNNoise weights resident even in passthrough / DFN3 mode~~ **INVESTIGATED 2026-08-22 → not acting** (premise false: demand-paged, not resident) | Low | `rnnoise_data.c` |
| F5 | ~~UI repaints at a fixed 10 Hz whenever visible, not on state change~~ **FIXED 2026-08-22** (repaint gated on `seq`/status change) | Low | `app_window.cpp` |
| F6 | Warm-standby runs both engines every frame (2× engine CPU) | Low (not default) | `dynamic_engine.cpp:73` |
| F7 | No CPU% / RSS / total-hop time in telemetry; long-run headroom is inferred | Low | `telemetry.h` |

Only **F1** is worth treating as a bug. The rest are accuracy/observability gaps and
deferred-by-design trade-offs, listed so they are named rather than forgotten.

---

## 1. The real-time audio path

### 1.1 What's right (keep it)

The worker's hot path is well disciplined and worth stating explicitly so it isn't
accidentally regressed:

- **SPSC rings hold `Frame` (`std::array<float,480>`) by value** (`spsc_ring.h`) — enqueue/
  dequeue is a fixed-size copy, no per-item allocation, single-producer/single-consumer with
  correct acquire/release ordering and 128-byte-aligned head/tail to avoid false sharing.
- **Ring memory is VirtualLock'd and page-touched at start** (`bridge.cpp:36-37`,
  `rt_lock_and_touch`) so the audio threads never take a page fault on the ring.
- **MMCSS on every audio thread** — capture, render, and the relay each construct an
  `MmcssThread` registering as "Pro Audio" at `AVRT_PRIORITY_CRITICAL`
  (`wasapi_capture.cpp:54`, `wasapi_render.cpp:59`, `bridge.cpp:150`).
- **The engine + DSP inner loop is allocation-free.** `DynamicEngine::ProcessFrame` uses
  stack arrays (`dynamic_engine.cpp:74`), `DspChain` uses fixed `t1_/t2_[FRAME_SAMPLES]`
  members (`dsp_chain.h:64-65`), `RnnoiseFilter` uses fixed scratch, the resampler output
  scratch is pre-sized in the ctor (`bridge.cpp:22`), and `proc_` is a `Frame`.
- **Lean engine loading keeps the idle cost off the RT thread** — the model load and the
  eventual free of a retired engine both happen on non-RT threads
  (`request_engine`, `reclaimRetiredEngines` on the 100 ms main tick); the RT thread only
  hands a retired pointer to a 4-slot SPSC queue (`dynamic_engine.cpp:100-107`).
- **Telemetry does not stall the RT thread** — the relay accumulates peak/RMS cheaply and
  only publishes a POD to shared memory once every ~100 hops (~1 s); the observer-side
  `size()` uses a bounded retry to avoid torn occupancy reads (`spsc_ring.h:31-46`).

> **RESOLVED 2026-08-21.** `FrameAccumulator` / `FrameDistributor` (and the shared new
> `FloatFifo`) are now backed by a fixed-capacity circular buffer preallocated at
> construction — no allocation, no heap lock, hard-bounded, on any of the three audio
> threads. Method signatures were unchanged, so the `bridge.cpp` hot path was untouched. New
> tests pin wraparound over 1000 rounds and the overflow guard; full suite **202/202** green
> in Debug + Release. The description below is retained as the rationale.
>
> **Page-fault refinement also DONE 2026-08-22.** The three reframing FIFOs are now
> `VirtualLock`'d + page-touched by the bridge in `start()` / unlocked in `stop()`, exactly
> like the SPSC rings (`bridge.cpp`, `rt_lock_and_touch` via new `data()`/`byte_size()`
> accessors on the framer classes). The default FIFO capacity was right-sized 48000 → 16384
> (≈341 ms, 64 KiB each) so all three locks fit inside the process working-set quota and
> actually succeed rather than falling back to touch-only, while staying >10x the real peak
> occupancy. So the audio threads now take **neither an allocation nor a soft page fault** on
> these buffers. 202/202 still green.

### 1.2 F1 — `std::deque<float>` on the audio threads (the one real defect)

`FrameAccumulator` and `FrameDistributor` are backed by `std::deque<float>`
(`framer.h:12,20`). They sit directly on all three real-time threads:

- `accum_` — written on the **WASAPI capture thread** every callback
  (`bridge.cpp:141` → `buf_.insert(buf_.end(), …)`), drained via `pop_frame`
  (`framer.cpp:6-10`, `pop_front` per sample).
- `dist_` — written and read on the **WASAPI render thread** inside `onFill`
  (`bridge.cpp:233`).
- `reframe_` — written and drained on the **MMCSS relay thread** after the resampler
  (`bridge.cpp:220-222`).

`std::deque` is **not a fixed-capacity structure**: it allocates and frees block nodes as
the buffer grows and shrinks, and it returns emptied blocks to the allocator rather than
pooling them. MSVC's deque uses a very small block size for small elements (4 floats per
block for a 4-byte type), so a single 480-sample write crosses ~120 block boundaries.

In steady state each ~10 ms hop pushes ~480 samples and pops ~480, so the buffer cycles
0 → ~480 → 0 and churns on the order of a hundred `malloc`/`free` pairs **per hop, per
thread** — roughly 10–20 k allocations/second on both the capture and render threads. Each
of those calls can take the process heap lock. On a priority-CRITICAL audio thread that is
exactly the kind of unbounded, lock-taking, non-deterministic operation the rest of the
design goes out of its way to avoid — a latent glitch / priority-inversion source that
directly contradicts the "allocation-free hot path" property claimed elsewhere.

**Why it hasn't bitten yet:** the Windows heap is fast, RNNoise leaves ~8 ms of slack in a
10 ms budget, and testing has been on a fast Ryzen 9. So it hides. But it is the single
biggest gap between the design's intent (hard-real-time) and its implementation, and it will
be the first thing to surface under load, on a slower machine, or during an 8-hour run.

**Fix (recommended, low risk):** replace the three deques with a fixed-capacity
preallocated ring/scratch. The building blocks already exist — a `Frame`-granular SPSC ring,
or a simple pre-`reserve`d `std::vector<float>` used as a circular buffer with head/tail
indices, sized once at construction from `ring_frames` + the max resampler output. The
reframing math is unchanged; only the container changes. This is a self-contained, testable
swap (the existing `test_framer.cpp` already pins the behavior) and would make the "no
allocation on the RT path" guarantee actually true end-to-end.

### 1.3 F6 — warm-standby doubles engine CPU (not on the default path)

In `Mode::Warm`, `DynamicEngine::ProcessFrame` runs **both** engines every frame
(`dynamic_engine.cpp:71-91`), and during any crossfade both run regardless of mode. v1 ships
**lean-first** (only the active engine runs), so the default RNNoise session pays this only
for the ~15-frame fade on a switch — negligible. Worth remembering that the future
warm-standby checkbox (Phase 4) turns on a permanent 2× engine cost (RNNoise + DFN3 + ONNX
every hop); the earlier live measurement (mean 2.07 ms, p99.9 4.61 ms, 0% over budget) says
there's headroom for it, but it is a real, standing CPU cost, not free.

---

## 2. Latency

The latency budget is already well characterized in the RNNoise findings doc; the whole-
program view adds context, not new numbers.

| Source | Cost | Status |
|---|---|---|
| WASAPI capture, shared-mode default period | ~10 ms | Deferred (`IAudioClient3` low-latency mode) |
| Framer accumulation to a 480-sample hop | 0–10 ms | Inherent |
| **Ring B primed half-full** | **`ring_frames`/2 × 10 ms** | **Dominant, user-tunable** (default 8 → ~40 ms) |
| DynamicEngine delay pin (DFN3 1440) | ~30 ms | Reclaimed by `low_latency` mode (RNNoise → 0) |
| WASAPI render pre-roll + distributor | ~10–20 ms | Deferred |

Two observations:

- **The dominant lever is already exposed.** Ring priming is the biggest single chunk and is
  a runtime setting (LATENCY box + `ring_frames` default lowered 16 → 8). The user has run
  `ring_frames = 4` (~20 ms) dropout-free on their rig. Good.
- **F3 — the estimate is now computed per-device at runtime. FIXED 2026-08-22.** The numbers
  above were hand-estimated from code. The worker now computes the **app-controlled** latency
  budget from the *actual* queried WASAPI buffer sizes (`WasapiCapture/Render::bufferFrames()` via
  `GetBufferSize`) plus this session's ring priming and engine delay — a pure, unit-tested
  `compute_latency()` in `latency.h`. It's printed at start (`latency (app-controlled, ms):
  capture=… + framer=… + ring=… + engine=… + render=… = … total`) and republished in telemetry as
  `estLatencyMs`, so it updates live with `ring_frames` / `low_latency` / engine. This is a real
  per-device figure, not a guess. It **excludes** analog/driver latency outside the process (small,
  not app-controlled). A true acoustic loopback measurement (impulse → render, timestamp at a
  reference) is still the only way to capture that external tail and independently confirm the
  computed budget — that remains a Phase V hardware task — but the dominant, tunable stages are now
  reported from real values instead of estimated.

---

## 3. CPU & efficiency

- **RNNoise is not CPU-bound.** Instrumented engine+chain time runs ~0.7–2 ms against a 10 ms
  budget with 0% over budget and ~5× tail headroom. There is nothing to blindly optimize on
  the compute side; the honest CPU work is visibility (already largely done via telemetry),
  not micro-optimization.
- **F2 — the budget accounting boundary is narrower than it reads. FIXED 2026-08-22.**
  Frame-time was measured around **engine + chain only** (`bridge.cpp`, "time ONLY engine+chain
  compute"), excluding the resampler, the reframe buffers, the ring push/pop, telemetry
  aggregation, and the WASAPI callback overhead — so "0% over 10 ms" described DSP compute, not
  the whole hop. **Now a second `FrameTimeStats hopStats_` times the entire relay hop** (ringA
  pop → params snapshot → engine/chain → telemetry → drift/resample/reframe → ringB push). It
  is reset by `ResetCounters` alongside the frame stats, published in telemetry as
  `hopP50Ms`/`hopP99Ms`/`hopMaxMs`, and printed in the worker's `stop()` summary as a
  `hop-time (whole relay hop, ms)` line beside the existing engine+chain line. The whole-hop
  cost — including what was previously invisible — is now observable. (The narrower
  engine+chain window is kept too, so the DSP slice can still be isolated from the glue.)
- **Resampler cost** is a per-sample cubic Hermite (`resampler.cpp:31-34`) — a handful of
  flops per output sample, trivial, and only active when `drift_compensation` is on (default
  on). Fine.

---

## 4. Memory footprint

- **F4 — the RNNoise weights. INVESTIGATED 2026-08-22 → not worth acting on; premise corrected.**
  The original claim ("~34 MB resident even in passthrough / DFN3") does not hold up:
  - `rnnoise_data.c` is **75 MB of C *source text*** (`{0.12f, -0.34f, …}`), which compiles to a
    modest fraction of that — the **entire** Release worker exe is only ~15 MB.
  - The weights are `const` data in a **memory-mapped, demand-paged** exe. They fault into physical
    RAM only when RNNoise actually touches them. A passthrough- or DFN3-only session that never
    constructs `RnnoiseFilter` **never makes them resident** — so the runtime-memory cost the finding
    warned about is already ~zero.
  - RNNoise is also the **default** engine, so in the overwhelming majority of sessions the weights
    are legitimately needed and loaded.

  What remains is only **binary/download size**, and the two ways to shrink it both have a downside
  that isn't justified for a personal tool: (a) on-demand file load (build rnnoise without the
  built-in model, ship a weights file, lazy-load) is invasive build surgery whose payoff applies only
  to the rare non-RNNoise session; (b) the "little" model is smaller but **changes output quality** —
  contraindicated for a v1 whose whole thesis is RNNoise quality, and requires a by-ear A/B that
  hasn't been done. **Conclusion: leave it.** Runtime cost is already negligible; the residual
  binary-size cost isn't worth a quality regression or build risk. Revisit only if download size
  becomes a real product constraint, and then only via (a).
- **DFN3 / ONNX are demand-loaded.** In lean mode a pure-RNNoise start never touches
  `onnxruntime.dll` or the DFN3 models; they load only on switch. Good — the heavy engine's
  memory cost is opt-in.
- **Locked ring memory scales with `ring_frames`** but stays small: at the 512 max it's
  ~512 × 480 × 4 bytes × 2 rings ≈ 2 MB non-pageable. Negligible.

---

## 5. Two-process shell & UI

The `voicecleaner.exe` (UI) / `voicecleaner-worker.exe` (audio) split is a deliberate
performance/robustness trade: a UI crash, hang, or GPU stall can never touch the audio
thread. That isolation is the single most important product-level performance decision and
it's sound. Costs and small gaps:

- **IPC is cheap and decoupled.** Worker → UI is a ~1 Hz shared-memory telemetry POD; UI →
  worker is a named pipe touched only on a param/engine/device change. No busy channel. The
  UI polls telemetry at 10 Hz (`app_window.cpp:89`), the worker's main tick is 100 ms
  (`main.cpp:182`) for reclaim + device-loss — both negligible.
- **F5 — the UI repainted on a fixed 10 Hz timer whenever visible. FIXED 2026-08-22.** `tick()`
  used to call `InvalidateRect` unconditionally, repainting the Direct2D scene 10×/second even
  when nothing changed — wasteful because the worker only publishes telemetry (meter + all
  aggregates) ~1 Hz, so 9 of every 10 repaints drew identical pixels. The tick now invalidates
  **only when a paint-relevant input changed**: telemetry `seq` advanced, the status kind changed,
  or the respawn attempt count changed (tracked via `lastPaintSeq_`/`lastPaintKind_`/
  `lastPaintAttempts_`). Interaction (hover/drag/clicks/pickers) still invalidates on its own WM
  events, so responsiveness is unchanged. Verified there is no UI-side animation (no blink/decay/
  phase timer) that needed the free-running repaint, and the meter data is itself 1 Hz, so nothing
  is visibly lost. Net: idle open-window repaints drop ~10×.
- **Crash-isolation recovery has a cost.** Supervisor auto-respawn (RespawnPolicy, backoff
  0.5→8 s) recovers audio unattended but **loses the session** — the new worker re-primes the
  ring (latency returns to full) and the drift controller re-adapts from the per-mic seed.
  That's the accepted v1 trade vs. worker-level in-place device reconnect (deferred). Not yet
  exercised on hardware.

---

## 6. Observability — what you can and can't see

**Strong.** Telemetry already surfaces frame p50/p99/max, ring A/B occupancy + monotonic
ring-B high-water, uptime, dropouts, drift ppm, AGC gain, VAD, active engine, and DFN3 failed
hops; `ResetCounters` gives clean A/B windows. This is more than most apps of this size have.

**Gaps (all low priority):**
- **F7** — no process CPU% or RSS in telemetry, and no whole-hop time (only the engine+chain
  slice, per F2). Long-run headroom is therefore *inferred* from the DSP slice, not measured
  at the process level. Adding a coarse CPU%/RSS sample to the ~1 Hz telemetry POD would make
  an 8-hour endurance run self-documenting.
- **End-to-end latency** is not a telemetry field (it can't be self-measured without a
  loopback reference) — see F3.

---

## 7. Recommended priority order

1. **F1 — replace the RT-thread deques with fixed-capacity buffers. ✅ DONE 2026-08-21.**
   The only change that fixed a genuine real-time correctness gap. `FloatFifo` circular
   buffer; `bridge.cpp` untouched; 202/202 tests green (Debug + Release).
2. **F3 — report a real per-device latency budget. ✅ DONE 2026-08-22.** Computed from queried
   WASAPI buffer sizes + config (`latency.h`, unit-tested); printed at start + telemetry
   `estLatencyMs`. A true acoustic-loopback measurement to capture the external analog/driver tail
   and independently confirm the budget remains a Phase V hardware task.
3. **F2 — publish whole-hop time. ✅ DONE 2026-08-22.** `hopStats_` times the full relay hop;
   telemetry `hopP50/P99/MaxMs` + a `hop-time` summary line. **F7 (still open)** — add a coarse
   CPU%/RSS sample so long-run headroom is observed, not inferred; cheap, makes the endurance run
   trustworthy.
4. **F5 — event-driven UI repaint. ✅ DONE 2026-08-22.** Repaint gated on `seq`/status/attempts
   change; idle open-window repaints drop ~10×.
5. **F4 — investigated, not acting** (runtime cost already ~zero via demand paging; residual
   binary-size cost not worth a quality regression or build risk). **F6 — leave deferred**;
   remember warm-standby's 2× engine cost when that checkbox lands. **F7 — still open** (coarse
   CPU%/RSS telemetry for endurance runs).

None of items 2–5 block anything. Item 1 — the one worth doing before an endurance run or
before shipping to a slower machine — is **done**: the audio threads went from "empirically
hasn't glitched yet" to "provably won't allocate."

---

## Appendix A — Telemetry window (design spec)

_Brainstormed + agreed 2026-08-22. An opt-in diagnostics window that surfaces the F2 hop-time
and the rest of the performance/health telemetry, which the app otherwise never displays.
Design constraint from the user: **spend no extra CPU unless the window is open.**_

### Key fact that shapes the design
The main window **already reads the entire `Telemetry` POD every 10 Hz tick**
(`WorkerSupervisor::poll` → `reader_.read()`), because worker **liveness/readiness/death** is
detected by watching `telemetry.seq` advance — that read is load-bearing and cannot be gated
off. The worker also always computes the frame/hop stats (a histogram increment per hop,
negligible). So the *reading* and *computing* are unavoidable and effectively free; the only
gate-able cost is **formatting + painting** a second window. The design gates exactly that.

### Trigger
A **"TELEMETRY" button in the main-window footer** (rect + hit-test + paint like the existing
footer controls). Click toggles the window open/closed.

### Components
- **`TelemetryWindow` (`src/voicecleaner/telemetry_window.{h,cpp}`)** — a second top-level
  window with its own Direct2D `HwndRenderTarget`, created lazily on first open. API:
  `toggle()` / `show()` / `hide()` / `isVisible()`, `update(const vc::ipc::Telemetry&)`, and an
  `onReset` callback. Closing **hides** it (`SW_HIDE`) rather than destroying, so reopening is
  instant. Thin Win32/D2D glue — not unit-tested, per the repo convention.
- **Pure `formatTelemetryRows(const vc::ipc::Telemetry&)` in `voicecleaner-core`** — returns the
  ordered `label : value` rows to draw (units, precision, and empty-state handling live here).
  Windows-free, **unit-tested** — this is the only real logic. The window just paints the rows
  it's handed.
- **`AppWindow`** — owns one `TelemetryWindow`; adds the footer button; wires the button to
  `toggle()` and the window's `onReset` to `sup_->send({CmdType::ResetCounters})` (already
  handled end-to-end by the worker).

### Lifecycle & CPU-gating (the requirement)
The existing 10 Hz `tick()` already has the telemetry snapshot in hand. On each tick, **only if
the telemetry window is visible *and* `telemetry.seq` changed** since its last paint, it hands
the snapshot to the window and invalidates it. Consequences:
- **Closed:** nothing — no format, no paint, no second timer.
- **Open:** repaints at the true data rate (~1 Hz, when `seq` advances), **not** 10 Hz — so even
  open it is nearly idle. (Same `seq`-changed trick as the F5 main-window optimization.)

### Content (Perf + Reset)
Rows: frame-time p50/p99/max (engine+chain) · hop-time p50/p99/max (whole hop) · dropouts ·
ring A occ · ring B occ · ring B high-water · drift ppm · uptime · active engine. Plus a
**Reset counters** button → fires the existing `ResetCounters` command (zeroes dropouts +
frame/hop stats + ring high-water for a clean measurement window).

### Appearance
Small fixed-size window, dark to match the app, monospaced `label : value` rows via the existing
D2D text setup. No resize logic.

### Testing
Unit tests for `formatTelemetryRows` (formatting, units, zero-count → "—"). Window/button glue
follows the existing untested-Win32-glue convention.

**Status: specced, not yet built** (this appendix is the spec; implementation is a later task).
