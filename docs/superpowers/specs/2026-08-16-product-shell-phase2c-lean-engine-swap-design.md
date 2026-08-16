# Product Shell — Phase 2c: On-Demand Lean Engine Swap — Design

_Date: 2026-08-16 · Status: draft for review (pass 2 — gap-hardened)_

Parent design: [`2026-08-15-product-shell-design.md`](2026-08-15-product-shell-design.md). Sits between the merged
Phase 2b (live DFN3 aggression) and Phase 3 (UI skeleton) — it is a **worker-only** capability the UI skeleton
depends on. Motivated during Phase 3 brainstorming: a user who rarely switches engines should be able to run
**lean** (one engine resident) yet still switch **gap-free**, without paying warm standby's continuous ~3× compute.

## 1. Problem & scope

Today the worker has two engine modes, chosen once at startup:

- **Warm standby** (`warm_standby = true`): both RNNoise and DFN3 are constructed at start and run **every frame**;
  `SwitchableEngine` equal-power-crossfades between them on `request_engine`. Switching is instant and gap-free, but
  both engines run forever (measured ~2.07 ms/hop vs ~0.71 ms for DFN3 alone) plus a second model in RAM.
- **Lean** (`warm_standby = false`): a single engine is constructed; `request_engine` **no-ops** — no switch path.

Measured DFN3 streaming session creation is **~16 ms warm / ~39 ms cold** for all four graphs (RNNoise is
effectively instant), so loading an engine on demand is cheap. This phase adds a **dynamic engine container** that
starts lean, loads the second engine on demand (off the RT thread), crossfades it in gap-free, and **eagerly
retires** (frees) the outgoing engine after the fade — returning to lean at rest. Warm standby becomes the same
container in a "both resident, run both, never retire" mode.

**In scope:** the dynamic container (`denoise-core`), its RT-safe load/crossfade/retire protocol, the non-RT reclaim
path, bridge integration, and headless tests. **Out of scope:** any UI (Phase 3), the live `warm_standby` toggle
checkbox (Phase 4), latency profiles, and switching to/from passthrough (`engine = none` stays a static dev mode).

## 2. Locked decisions

| Decision | Choice |
|---|---|
| Steady state after a lean switch | **True lean with eager retire** — the outgoing engine is freed after the crossfade; at rest exactly one engine is resident. |
| Retire safety model | The **RT thread hands the retired pointer to a lock-free SPSC queue** and nulls the live slot; a **non-RT reclaim** frees it. A re-switch to a just-retired index does a **fresh load** — the container never resurrects a zombie pointer (§4). No "cancel" path, no TOCTOU. |
| Reclaim trigger | Non-RT, **independent of command arrival** — a small reclaim step the bridge runs periodically / when signalled, never on the RT thread and never only-on-command. |
| Switch smoothness | **Gap-free, click-free** — reuse the equal-power cos/sin crossfade (`fadeFrames = 15`, ~150 ms) and delay-alignment. The load happens off the RT thread before the fade begins. |
| Modes | **warm** = both preloaded, **run both every frame**, retirement **off**. **lean** = active-only when not fading, on-demand load, eager retire. A single mode flag; warm behavior stays byte-identical to today (§7 golden). |
| Output delay | **Pinned constant** = `max(RNNoise, DFN3)` algorithmic delay for the whole session (even when lean), so a switch never shifts output timing (the drift controller/downstream never see a step). The per-engine delays are **known constants** — RNNoise 0, DFN3 480 (`AlgorithmicDelaySamples()` in their headers) — so no runtime probe is needed; the bridge passes them into the container's `Config`. |
| Index convention | **0 = RNNoise, 1 = DFN3** everywhere (matches `Params.engine`, `telemetry.activeEngine`, today's leafA/leafB). |
| Regression guard | The warm-standby crossfade output must not change. Capture a golden output vector from today's `SwitchableEngine` **before** refactoring and assert the generalized container reproduces it bit-for-bit. |
| Trigger for headless test | Reuse `--dev-keys` (keys `1`/`2` → `request_engine`), which now drives the container in lean mode too. |

## 3. The dynamic engine container (`denoise-core`)

Generalize `SwitchableEngine` into a container (implements `INoiseFilter`, so it drops into `engine_`/`DspChain`
unchanged) with a **mode** (warm | lean) and these states:

- **Lean, at rest**: exactly one leaf resident (`eng_[cur_]`); the other slot null. `ProcessFrame` runs **only** the
  resident leaf, delay-padded to the pinned max so output timing is engine-independent.
- **Switching** (either mode): both slots resident; `ProcessFrame` runs **both** and crossfades `from_ → cur_`.
- **Warm, at rest**: both resident; `ProcessFrame` runs **both every frame** (keeps the inactive engine warm — this
  is byte-identical to today's `SwitchableEngine` and is what the golden guards).
- **Retiring → lean** (lean mode only): the fade completed; the RT moves the outgoing leaf to the reclaim queue and
  nulls its slot; the non-RT reclaim frees it.

**Ownership:** the container **owns** installed leaves (via `unique_ptr`); it does not construct them — a **loader**
seam supplied by the bridge maps `idx → unique_ptr<INoiseFilter>`, keeping `denoise-core` free of `dfn3`/`rnnoise`
specifics and letting tests inject a fake engine. `INoiseFilter` has a virtual destructor, so the reclaim frees via
the base pointer.

**Aggression:** the container stores the last `SetAggression` value; `SetAggression` applies it to every **resident**
leaf (null-checked) and records it, so a freshly installed leaf is set to the current aggression on install (no
one-frame default-aggression blip). `LastVadProbability`/delay read from `eng_[cur_]`.

## 4. Concurrency protocol (the load-bearing part)

Threads: the **RT relay thread** (runs `ProcessFrame`, advances the fade, retires) and **non-RT command/reclaim**
work (loads/installs, frees). No locks on the RT path.

**Shared state:**
- `std::atomic<INoiseFilter*> eng_[2]` — a slot's leaf, or `nullptr` when not resident. Non-RT stores **release**;
  RT loads **acquire**.
- `std::atomic<int> want_` — target index (RT-visible switch request).
- `retiredQ_` — a small fixed-capacity **lock-free SPSC queue** of `INoiseFilter*` (RT = producer, reclaim =
  consumer). Capacity ≥ 2; retirements are ≥ `fadeFrames` apart and the reclaim runs promptly, so it never fills.
- RT-owned, no other writer: `cur_`, `from_`, `fadeLeft_`. Per-slot alignment (`delaySamples_[2]`, `dline_[2]`) is
  written by the installer before publication and read by the RT only while that slot participates.

**Install a leaf into slot `t` (non-RT, serialized by a command mutex):**
1. Construct via the loader (slow, off-RT; may throw — on failure abort the switch, leave the running engine intact,
   report; **no half-published slot**).
2. `SetAggression(stored)` on it; set `delaySamples_[t] = maxDelay_ − leaf->AlgorithmicDelaySamples()`; **zero+resize
   `dline_[t]`**.
3. `eng_[t].store(p, release)`. (All of step 2's plain writes are published by this release.)

**Switch to `t` (non-RT, under the command mutex):** if `eng_[t] == nullptr` install it (above); then
`want_.store(t, release)`.

**Per frame (RT thread):**
1. `want = want_.load(acquire)`. If `want != cur_ && fadeLeft_ == 0`: `from_ = cur_; cur_ = want; fadeLeft_ =
   fadeFrames_`. Both `eng_[from_]` and `eng_[cur_]` are non-null here — the target's release-store is visible
   because this thread observed `want_ == t` via the acquire that synchronizes-with the installer's release.
2. If `fadeLeft_ > 0` **or** mode is warm: run **both** leaves; crossfade when `fadeLeft_ > 0`, else emit `cur_`.
   If lean and `fadeLeft_ == 0`: run **only** `eng_[cur_]`.
3. On the frame `fadeLeft_` reaches 0 **and** mode is lean: **unless `want_` has meanwhile re-targeted `from_`** (a
   switch-back that arrived mid-fade — then keep it resident and let the next frame fade `cur_ → from_`), retire it:
   `p = eng_[from_].exchange(nullptr)`; `retiredQ_.push(p)` (if the push fails, re-store `p` — never leak). This
   retire-or-keep decision is made **entirely on the RT thread**, so there is no cross-thread free race. From a
   retirement onward the RT will not touch that pointer again — a later switch back finds `eng_[idx] == nullptr` and
   triggers a **fresh install**, so the queued pointer is never reused.

**Reclaim (non-RT, command/reclaim context — not the RT thread):** drain `retiredQ_`, `delete` each leaf. Runs
independently of command arrival (a periodic tick or a signalled step), so a single switch followed by silence still
frees the outgoing engine.

**Why this is safe (the tests must confirm):** the RT never dereferences a null/freed pointer — a target is
published (release) before it is requested, and the RT observes the request (acquire) before touching the target; a
leaf leaves RT reachability atomically (`exchange(nullptr)`) *before* it enters the reclaim queue, so the reclaimer
and the RT never hold the same pointer; a re-switch never resurrects a queued pointer (it installs a fresh one). At
most two leaves are resident (transient, during a switch).

## 5. Bridge integration (`voicecleaner-worker`)

- **`Bridge::start`** builds the container for any real-engine config (`rnnoise`/`dfn3`), passing
  `Config.delaySamples = {0, 480}` (RNNoise, DFN3 — known constants, no probe); `warm_standby = true` → mode warm
  (loader builds both); `false` → mode lean (loader builds only the configured engine). This replaces the current
  `switch_`-only-under-warm-standby branch and the single-engine branches for rnnoise/dfn3. `engine = none` stays a
  static `PassthroughFilter` with no container (no switching).
- **`request_engine(int idx)`** routes to the container: under the command mutex, install `idx` if absent, then
  `requestActive(idx)`. **No longer `noexcept`** — it catches loader failures, logs, and leaves the current engine
  running. Callable from the pipe handler and the `--dev-keys` thread; the mutex serializes them.
- **Reclaim** is invoked from a non-RT context the bridge already owns — the worker main loop's ~100 ms tick calling
  `bridge.reclaimRetiredEngines()` (bounded free latency ≤ one tick; the ~8 MB leaf lingering that long is
  negligible). Never on the relay/RT thread.
- **`dfn3Adapter_` observer fix (gap E):** the raw DFN3 pointer the shutdown path reads for `failedHops()` must not
  dangle. On installing a DFN3 leaf, set it; **before** reclaim frees a DFN3 leaf, fold its `failedHops()` into a
  persistent `dfn3FailedHopsRetired_` counter and clear the observer. Shutdown reports the persistent counter plus
  the live adapter's count if one is resident.
- **Telemetry `activeEngine`** reads `container->activeIndex()` in both modes (the `cfg_.engine` fallback is gone).
- **Shutdown ordering (gap J):** unchanged `stop()` sequence — `capture_.stop()` → `running_ = false` →
  `relay_.join()` (RT gone) — **then** a final `reclaimRetiredEngines()` drain; the container destructor frees any
  still-resident leaves. Nothing is freed while the RT could touch it.

## 6. Passthrough / `engine = none`

The container switches between the two real engines only. `engine = none` remains a **static passthrough** dev mode
with no container and no switching (`request_engine` no-ops), exactly as today. The Phase 3 UI offers only
RNNoise/DFN3; a passthrough worker is a dev configuration, not a product state.

## 7. Testing

**Unit — `denoise-core`, headless, no audio, no ONNX** (a fake `INoiseFilter` that emits a known per-engine signal,
records `ProcessFrame`/`SetAggression` calls, and flags its own destruction so use-after-free/double-free are
detectable; a controllable frame clock):
- **Warm golden (regression):** the generalized container in warm mode reproduces today's `SwitchableEngine` output
  **bit-for-bit** for a scripted switch sequence (vector captured from the current implementation before refactor).
  The two existing `test_switchable_engine.cpp` cases stay green (or are ported unchanged in behavior).
- Lean at rest runs **only** the active engine (the fake records the inactive engine is not called).
- Lean switch: load-on-demand → both run during the fade → outgoing pushed to the reclaim queue → freed by the
  reclaim step → one resident; continuity (first fade frame equals the pre-switch sample, no click); output delay
  constant across the switch; a freshly installed leaf sees the stored aggression, not the default.
- **Rapid re-switch A→B→A before reclaim runs:** no double-free, no use-after-free (fake liveness), ends resident on
  a fresh A, lean; the reclaim frees exactly the retired instances.
- Request mid-fade: coalesced to the latest target; no fade restart glitch; no lost retirement.
- Reclaim independence: a single switch with **no further commands** still frees the outgoing engine on the next
  reclaim tick.
- SPSC queue never overflows for back-to-back switches spaced ≥ `fadeFrames` with prompt reclaim; `SetAggression`
  null-checks a non-resident slot.
- Loader failure: `install` throws → switch abandoned, current engine still running, no half-published slot, no leak.

**Integration (real hardware, headless via `--dev-keys`):**
- Lean worker (`warm_standby = false`, `engine = rnnoise`): press `2` → DFN3 loads on demand, crossfades in
  gap-free/click-free, RNNoise retired; RSS returns to one model within a tick; `activeEngine` telemetry reads DFN3.
- Press `1` → back to RNNoise, DFN3 retired. Rapid `1 2 1 2` → no crash, no dropout, no stuck engine.
- Warm-standby worker: behavior unchanged (instant switches, both resident).
- Shutdown after a lean switch: no crash reading `failedHops` (gap E), clean exit.

**Regression (must stay green):** all existing tests (100) incl. `test_switchable_engine.cpp`, the DFN3 offline
golden, and the streaming Gates. Warm-standby crossfade output is unchanged.

## 8. Deliverables

- Dynamic engine container in `denoise-core` (generalized `SwitchableEngine` + loader seam + mode + SPSC reclaim
  queue) with the §7 unit tests, incl. the warm golden.
- `Bridge` builds and drives it in both modes; `request_engine` (non-noexcept, mutex-serialized, on-demand install);
  `reclaimRetiredEngines()` on the main-loop tick + final drain in `stop()`; `dfn3Adapter_` dangle fix;
  `activeEngine` telemetry via the container; pinned constant output delay via a startup probe.
- No regression in the existing 100 tests, the DFN3 golden, the streaming Gates, or the warm-standby crossfade.

## 9. Assumptions

- On-demand DFN3 construction (~16-39 ms measured) is acceptable latency for a user-initiated switch, happens off the
  RT thread, and the crossfade masks the freshly-loaded engine's cold recurrent state.
- Transient double-memory (two models) during a switch is affordable (it is the warm-standby steady state, already
  deemed affordable).
- Pinning output delay to `max(RNNoise, DFN3)` when lean adds a fixed, small latency to the lower-latency engine —
  accepted in exchange for switch-invariant timing.
- Reclaim latency of up to one main-loop tick (~100 ms) before an ~8 MB leaf is freed is acceptable.
