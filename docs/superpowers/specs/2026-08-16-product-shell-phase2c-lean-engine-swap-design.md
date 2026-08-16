# Product Shell — Phase 2c: On-Demand Lean Engine Swap — Design

_Date: 2026-08-16 · Status: draft for review_

Parent design: [`2026-08-15-product-shell-design.md`](2026-08-15-product-shell-design.md). Sits between the merged
Phase 2b (live DFN3 aggression) and Phase 3 (UI skeleton) — it is a **worker-only** capability the UI skeleton
depends on. Motivated during Phase 3 brainstorming: a user who rarely switches engines should be able to run
**lean** (one engine resident) yet still switch **gap-free**, without paying warm standby's continuous ~3× compute.

## 1. Problem & scope

Today the worker has two engine modes, chosen once at startup:

- **Warm standby** (`warm_standby = true`): both RNNoise and DFN3 are constructed at start and run **every frame**;
  `SwitchableEngine` equal-power-crossfades between them on `request_engine`. Switching is instant and gap-free, but
  both engines run forever (measured ~2.07 ms/hop vs ~0.71 ms for DFN3 alone) plus a second model in RAM.
- **Lean** (`warm_standby = false`): a single engine is constructed; `request_engine` **no-ops** — there is no
  engine-switch path at all.

Measured DFN3 streaming session creation is **~16 ms warm / ~39 ms cold** for all four graphs (RNNoise is
effectively instant), so loading an engine on demand is cheap. This phase adds a **dynamic engine container** that
starts lean, loads the second engine on demand (~tens of ms, off the RT thread), crossfades it in gap-free, and
**eagerly retires** (frees) the outgoing engine after the fade — returning to lean at rest. Warm standby becomes the
same container with retirement disabled and both leaves preloaded.

**In scope:** the dynamic container (`denoise-core`), its RT-safe load/crossfade/retire protocol, bridge
integration, and headless tests. **Out of scope:** any UI (Phase 3), the live `warm_standby` toggle checkbox
(Phase 4), latency profiles, and switching to/from passthrough (`engine = none` stays a static dev mode, §6).

## 2. Locked decisions

| Decision | Choice |
|---|---|
| Steady state after a lean switch | **True lean with eager retire** — free the just-faded-out engine a safe margin after the crossfade completes; at rest exactly one engine is resident. |
| Switch smoothness | **Gap-free, click-free** — reuse the existing equal-power cos/sin crossfade (`fadeFrames = 15`, ~150 ms) and delay-alignment. The load happens on the non-RT command thread before the fade begins. |
| Warm standby | Same container, **retirement disabled + both leaves preloaded**. `warm_standby` becomes the retirement/preload **policy**, not a structurally different pipeline. |
| Output delay | **Pinned constant** = `max(RNNoise, DFN3)` algorithmic delay for the whole session, even when lean, so a switch never shifts output timing (the drift controller and downstream never see a step). |
| Regression guard | The existing warm-standby crossfade **output must not change** (goldens/Gates stay green). Prefer generalizing `SwitchableEngine` such that warm standby is its "both resident, never retire" special case, re-verified. |
| Trigger for headless test | Reuse `--dev-keys` (keys `1`/`2` → `request_engine`), which now drives the container in lean mode too. No UI needed to exercise the swap. |

## 3. The dynamic engine container (`denoise-core`)

Generalize `SwitchableEngine` (or introduce a sibling it collapses into) with these states:

- **Lean**: exactly one leaf resident (`eng_[cur_]`); the other slot is null. `ProcessFrame` runs **only** the
  resident leaf, delay-padded to the pinned max so output timing is engine-independent.
- **Switching**: both slots resident; `ProcessFrame` runs **both** and crossfades `from_ → cur_` over `fadeFrames_`,
  exactly as today.
- **Retiring → Lean**: fade complete; the container signals the outgoing leaf is no longer touched; the command
  thread frees it; back to one resident.

Interface (unchanged shape): `ProcessFrame`, `SetAggression` (applied to resident leaves; the command thread sets it
on a freshly loaded leaf **before** the fade so levels match), `LastVadProbability`/delay from `eng_[cur_]`,
`requestActive(idx)` (non-RT), `activeIndex()` (returns `cur_`, i.e. the fade target, promptly on fade start).

The container does **not** construct engines itself. A small **loader** callback (owned by the bridge) maps
`idx → std::unique_ptr<INoiseFilter>` so `denoise-core` stays free of `dfn3`/`rnnoise` construction specifics and the
container is testable with a fake engine.

## 4. Concurrency protocol (the load-bearing part)

Single-writer discipline, two threads: the **command thread** (non-RT; loads, publishes, frees) and the **RT relay
thread** (runs `ProcessFrame`, advances the fade). No locks on the RT path.

**Shared state:**
- `std::atomic<INoiseFilter*> eng_[2]` — a slot's pointer, or `nullptr` when not resident. Command thread stores
  with **release**; RT loads with **acquire**.
- `std::atomic<int> want_` — target index. RT-visible switch request.
- `std::atomic<int> retired_` — index the RT has finished fading away from and will not touch again (`-1` = none).
- `std::atomic<uint32_t> reqSeq_` / `ackSeq_` — request generation, to resolve rapid re-switches (ABA guard).
- RT-owned (no other writer): `cur_`, `from_`, `fadeLeft_`.

**Switch to `t` (command thread):**
1. If `eng_[t] == nullptr`: construct the leaf (slow, off-RT), `SetAggression(current)`, then `eng_[t].store(p, release)`.
2. `want_.store(t, release)` and bump `reqSeq_`. (The pointer is published *before* the request that references it,
   so the RT never sees `want_ == t` with a null `eng_[t]`.)

**Per frame (RT thread):**
1. `want = want_.load(acquire)`. If `want != cur_ && fadeLeft_ == 0`: begin a fade — `from_ = cur_; cur_ = want;
   fadeLeft_ = fadeFrames_`. (Both `eng_[from_]` and `eng_[cur_]` are guaranteed non-null here.)
2. If `fadeLeft_ > 0`: run **both** leaves, crossfade (existing math); else run **only** `eng_[cur_]`.
3. When `fadeLeft_` reaches 0 at the end of a fade: if retirement is enabled, `retired_.store(from_, release)` and
   `ackSeq_.store(reqSeq_at_fade_start, release)`.

**Retire (command thread), only when `warm_standby == false`:**
- Observe `retired_ >= 0`. Before freeing, check for a **pending re-switch**: if `want_ == retired_` (a request to
  bring the retiring leaf back has arrived), **cancel** — leave it resident, clear `retired_`, let the RT fade back
  to the still-live leaf. Otherwise: `eng_[retired_].store(nullptr, release)`, then free the object, clear `retired_`.
- The free is safe because the RT only reaches a slot it holds non-null: after a completed fade the RT runs solely
  `eng_[cur_]`, and it only re-touches `from_` on a *new* fade, which requires a new `want_` the command thread
  serializes against here.

**Why this is safe (the argument the tests must confirm):** the RT never dereferences a null or freed pointer
because (a) a target is published before it is requested, and (b) a leaf is freed only after the RT has signalled it
left it *and* no newer request re-targets it. Rapid `A→B→A` before the free lands on the cancel branch (B stays
resident, no free/reload). At most two leaves are ever resident (transient, during a switch).

## 5. Bridge integration (`voicecleaner-worker`)

- `Bridge::start` builds the **dynamic container** for any real-engine config (`rnnoise`/`dfn3`), passing the loader
  and the pinned max delay. `warm_standby = true` → preload both leaves + retirement disabled; `false` → load only
  the configured engine + retirement enabled. This **replaces** the current `switch_`-only-under-warm-standby branch.
- `request_engine(idx)` always routes to `container->requestActive(idx)`; the retire step runs on the existing
  command thread (the pipe handler / a small serviced step), never on the RT relay.
- Telemetry `activeEngine` reads `container->activeIndex()` in both modes (today it falls back to `cfg_.engine` when
  lean — no longer needed).
- **Pinned max delay:** obtain both engines' `AlgorithmicDelaySamples()` without keeping both resident — either a
  per-engine static constant or a one-time probe at startup (construct both to read delays, free the non-active in
  lean). Chosen in the plan; the **requirement** is a session-constant output delay.

## 6. Passthrough / `engine = none`

The container switches between the two real engines only. `engine = none` remains a **static passthrough** dev mode
with no container and no switching (`request_engine` no-ops), exactly as today. The Phase 3 UI offers only
RNNoise/DFN3; a passthrough worker is a dev configuration, not a product state.

## 7. Testing

**Unit — `denoise-core`, headless, no audio, no ONNX** (fake `INoiseFilter` that records calls + emits a known
signal; a controllable frame clock):
- Lean → switch → both run during fade → outgoing freed → one resident on the new engine.
- Crossfade output is **bit-identical** to the current `SwitchableEngine` for the both-resident case (regression).
- Continuity: first fade frame equals the pre-switch sample (no click at the join); output delay constant across a switch.
- **Rapid re-switch** `A→B→A` before the free: no double-free, no use-after-free (checked via the fake's liveness),
  ends resident on A, lean.
- Request mid-fade: coalesced to the latest target; no fade restart glitch.
- Warm-standby policy: retirement disabled keeps both resident indefinitely; switches stay instant.
- Loader failure (engine construction throws): the switch is abandoned cleanly, the current engine keeps running,
  and the failure is surfaced (no half-published slot).

**Integration (real hardware, headless via `--dev-keys`):**
- Lean worker (`warm_standby = false`, `engine = rnnoise`): press `2` → DFN3 loads on demand, crossfades in
  gap-free/click-free, RNNoise is retired; RAM returns to one model; `activeEngine` telemetry reads DFN3.
- Press `1` → back to RNNoise, DFN3 retired. Rapid `1 2 1 2` produces no crash, no dropout, no stuck engine.
- Warm-standby worker: behavior unchanged from today (instant switches, both resident).

**Regression (must stay green):** all existing tests (100) and the DFN3 offline golden + streaming Gates; the
warm-standby crossfade output is unchanged.

## 8. Deliverables

- Dynamic engine container in `denoise-core` (generalized `SwitchableEngine`) + loader seam.
- `Bridge` builds and drives it in both modes; `request_engine` + retire wired; `activeEngine` telemetry via the
  container; pinned constant output delay.
- New `denoise-core` unit tests for the state machine (fake engine + clock) per §7.
- No regression in the existing 100 tests, the DFN3 golden, or the warm-standby crossfade.

## 9. Assumptions

- On-demand DFN3 construction (~16-39 ms measured) is acceptable latency for a user-initiated switch and happens off
  the RT thread; the crossfade masks the freshly-loaded engine's cold recurrent state.
- Transient double-memory (two models) during a switch is affordable (it is the warm-standby steady state, already
  deemed affordable).
- Pinning output delay to `max(RNNoise, DFN3)` when lean adds a fixed, small latency to the lower-latency engine —
  accepted in exchange for switch-invariant timing (no drift-controller disturbance on switch).
