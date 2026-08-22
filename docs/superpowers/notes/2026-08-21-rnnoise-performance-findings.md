# RNNoise & Performance — Findings / Tracking Doc

_Living document. Started 2026-08-21. Records the code-grounded discovery behind the
[RNNoise & Performance Improvements plan](../plans/2026-08-21-rnnoise-performance-plan.md).
Update the "Measured numbers" and "Decisions log" sections as work lands._

**Scope decisions (2026-08-21):** No DFN3 *feature* work. DFN3 stays shipped and selectable
**untouched** (gap-free switch preserved). All effort goes to the **RNNoise path + overall performance**
(quality, latency, CPU, stability). Spectral musical-noise R&D, `IAudioClient3` low-latency mode, RNNoise
model swap, and the 8-hour endurance run are **deferred** (see "Deferred" below).

---

## 1. Latency

### Measured budget (RNNoise path, `ring_frames = 16`, estimated from code)

| Source | Cost | Location |
|---|---|---|
| WASAPI capture, shared-mode default buffer | ~10 ms | `wasapi_capture.cpp:30` — `Initialize(...,0,0,mix,...)` |
| Framer accumulation to a 480-sample hop | 0–10 ms | `bridge.cpp:136` |
| **Ring B primed half-full** | **~80 ms** | `bridge.cpp:40` — primes `capacity()/2` = 8 frames × 10 ms |
| **DynamicEngine delay pin (1440 samples)** | **~30 ms** | `bridge.cpp:71`, `dynamic_engine.h:40` |
| WASAPI render pre-roll + distributor | ~10–20 ms | `wasapi_render.cpp:45` |
| **Estimated total** | **~140–150 ms** | (to be confirmed by measurement — Plan Task B2) |

### Key finding — RNNoise pays DFN3's 30 ms even when DFN3 never loads
`bridge.cpp:71` sets `dc.delaySamples[1] = 1440` **unconditionally**. `DynamicEngine` pins output delay to
`max(delaySamples[0], delaySamples[1]) = 1440` in *both* Warm and Lean modes (`dynamic_engine.h:40`,
`alignDelay_[0] = maxDelay_ - 0 = 1440`). So a pure-RNNoise lean session — the shipped default — delays
output by 30 ms purely to keep engine-switch timing invariant with DFN3. **Reclaimable** via a low-latency
mode that skips the pin (Plan Task B1); the cost is a one-time ~30 ms timing shift the first time the user
switches to DFN3 (acceptable — RNNoise is default, switching is rare).

### Ring priming is the dominant chunk
80 ms of the ~145 ms is Ring B priming (8 frames). Already user-tunable via the LATENCY box, but the
**default of 16** is conservative. How low it can go before dropouts is a **measurement** question (Task B2),
not a guess. This is the single biggest lever.

### Deferred
`IAudioClient3` / `InitializeSharedAudioStream` low-latency shared mode could shave the WASAPI buffers, but
it's higher-risk and a smaller slice than the two above. Not in this plan.

---

## 2. RNNoise output quality

### Finding — no suppression-depth control exists
- `RnnoiseFilter::SetAggression` is a hard no-op (`rnnoise_filter.h:21`).
- `DspChain` has no wet/dry mix: the engine call at `dsp_chain.cpp:39` is all-or-nothing.
- The declicker + noise gate already handle the **gaps**; there is nothing to control the RNNoise sound
  **during speech** (over/under-suppression, "quiet lasers" musical noise).

### Chosen fix — engine-delay-aligned wet/dry mix ("STRENGTH")
A wet/dry mix right after the engine (`dsp_chain.cpp:39`, before the HPF). `out = s·wet + (1−s)·dryDelayed`,
default `s = 1.0` (full wet = today's behavior, so DFN3 is untouched at default). Lower `s` blends the
natural signal back → controllable suppression depth **and** masks musical noise.

**Subtlety (drives the design):** the engine output is delayed by the engine's algorithmic delay (0 for
RNNoise in low-latency mode, else 1440 via the DynamicEngine pin). A naive mix of undelayed dry with delayed
wet combs. So the dry path must be delayed by `engine->AlgorithmicDelaySamples()` — handled by a dedicated
pure unit `DryWetMix` sized at construction. See Plan Task A1.

### Deferred
Spectral (STFT-domain) musical-noise smoothing — real R&D, uncertain payoff.

---

## 3. CPU / efficiency

### Finding — RNNoise is not CPU-bound; there is nothing to blindly optimize
Frame-time is already instrumented (engine+chain only) with p50/p99/max telemetry
(`bridge.cpp:161-165`, `frame_time_stats.h`). Prior live runs: mean ~0.7–2 ms against a 10 ms budget,
0% over budget. Lean mode loads only the active engine, so a pure-RNNoise start never touches
`onnxruntime.dll` / DFN3 model.

**So the honest CPU work is visibility, not optimization:** wire the deferred `uptimeMs` / ring-high-water
telemetry (Plan Task C2) so headroom is watchable over long runs, and add `ResetCounters` (Task C1) for clean
A/B measurement windows.

### Deferred
RNNoise model swap (built-in ~75 MB weights at `rnnoise_filter.cpp:13`) for memory/CPU — needs a side-by-side
quality test first; not worth it until measurement shows a memory/CPU problem.

---

## 4. Stability / robustness

### Finding — no auto-recovery from device loss
On a mic/USB glitch the WASAPI thread sets `deviceLost_` (`wasapi_capture.cpp:68`), the worker prints
"re-run once the device is back" and **exits** rc=2 (`main.cpp:183-188`). The supervisor detects the dead
process (`worker_supervisor.cpp:48`) but only re-spawns on a **manual** Relaunch — `startOrAttach()` is never
called automatically. Net: a transient glitch kills audio until the user intervenes.

**Chosen fix (v1):** supervisor **auto-respawn with backoff** (Plan Task C3). Simpler than worker-level
in-place device reconnect, and it composes with the existing "worker exits on device loss" behavior. It loses
the session (re-primes latency, drift re-adapts) but recovers audio unattended. Worker-level in-place
reconnect is the better-UX upgrade — deferred.

### Finding — `ResetCounters` can't work yet
No-op because `DropoutCounter` / `FrameTimeStats` have no `reset()` (`main.cpp:148-149`, `dropout.h`,
`frame_time_stats.h`). Adding `reset()` unblocks it (Plan Task C1) — useful for A/B measurement windows.

### Deferred
Worker-level in-place device reconnect; the 8-hour endurance run.

---

## Status
- **Workstream A (RNNoise STRENGTH) — DONE, merged to master 2026-08-21.** New pure `DryWetMix`
  (engine-delay-aligned) in `DspChain` after the engine; `Params.mixStrength` / `MicProfile.mix_strength`
  default **1.0 = full wet** (DFN3 default unchanged); 8th "STRENGTH" D2D slider (live/seeded/persisted).
  **186 tests** green (Debug + Release); DFN3 golden untouched (`src/dfn3`/`src/offline-cli` diff empty).
  **STILL OWED:** by-ear hardware validation of the knob (Phase V) — no GUI run happened this session.
- **Workstream B1 (low-latency no-pin mode) — DONE, merged 2026-08-21.** Global `low_latency` flag
  (`Config`/`Settings`, TOML `[global].low_latency`, default off) → bridge passes `delaySamples[1]=0` so
  RNNoise runs at native ~0 latency (reclaims ~30 ms); switching to DFN3 then costs a one-time ~30 ms shift.
  Enable for testing: add `low_latency = true` under `[global]` in settings.toml. **189 tests** green.
  **B2 (measure end-to-end latency + tune the ring default) is a HARDWARE task → Phase V.** UI checkbox for
  the flag deferred until B2 quantifies the win.
- Workstream C (stability/measurement), Phase V (hardware) — not started.

## Decisions log
- **2026-08-21** — DFN3 stays shipped untouched; work is RNNoise + performance only. (User.)
- **2026-08-21** — Wet/dry "STRENGTH" mix chosen for RNNoise quality; default 1.0 so DFN3 default behavior
  is unchanged; must delay-align the dry path.
- **2026-08-21** — Latency: add an opt-in low-latency (no-pin) mode + measure/tune the ring default; defer
  `IAudioClient3`.
- **2026-08-21** — Stability: supervisor auto-respawn with backoff for v1; defer worker-level reconnect.

## Measured numbers (fill in during Plan Task B2 / Phase V)
| Metric | Config | Value | Date | Notes |
|---|---|---|---|---|
| End-to-end latency | ring=16, pin on | _TBD_ | | baseline |
| End-to-end latency | ring=16, low-latency (no pin) | _TBD_ | | expect ~30 ms less |
| Lowest dropout-free ring | low-latency | _TBD_ | | sets new default |
| Frame-time mean / p99 / max | RNNoise | _TBD_ | | headroom check |
| Dropouts over N min | best low-latency config | _TBD_ | | stability |
