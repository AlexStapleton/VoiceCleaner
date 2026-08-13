# Phase 0 — Instrumented Pass-Through Bridge — Design Spec

**Date:** 2026-08-12
**Project:** VoiceCleaner (real-time mic noise suppression, Windows 11)
**Parent blueprint:** `Noise_Suppression_Blueprint_v5.md`
**Scope of this spec:** Phase 0 only — the measurement gate that precedes all DSP/engine work.

---

## 1. Objective

Build the bare audio transport — WASAPI capture from the FIFINE K688 → lock-free ring(s)
→ WASAPI render into VB-Cable "CABLE Input" — with **no DSP**, and instrument it well enough to:

1. Prove **zero dropouts over a full 8-hour session** (the blueprint's real quality bar, §1.2).
2. **Measure clock drift** between the mic's ADC clock and VB-Cable's software clock (§5.8) by
   logging Ring B occupancy once per second and recovering the slope → the ppm mismatch of *this*
   hardware pair. This is the number Phase 1.5's adaptive resampler will compensate.

Phase 0 exists to de-risk the **production transport topology** and to measure the hardware, before
any inference work is layered on. What is tested at hour 8 must be the shape we ship.

## 2. Scope

**In scope**
- Headless C++20 / CMake / MSVC console application.
- Faithful two-ring topology (Approach A): capture → Ring A → MMCSS relay thread (pass-through)
  → Ring B → render.
- 480-sample (10 ms) framing throughout, matching the production frame size.
- Instrumentation: drift CSV (Ring B occupancy/sec), dropout counter with timestamps, ~1 Hz live
  console telemetry.
- `VirtualLock` + pre-touch of RT buffers (pulled forward from Phase 1 — protects the measurement).
- Format guard (§5.7) and device selection by ID.

**Out of scope (deferred, with the phase that owns each)**
- All DSP / engines, `denoise-core` proper — Phase 2.
- Drift *compensation* (adaptive resampler + PI loop) — Phase 1.5. Phase 0 only *measures*.
- Latency-profile switching + startup calibration guard — Phase 1.
- In-app impulse/cross-correlation latency measurement — Phase 1.
- Full device lifecycle (`IMMNotificationClient`, hot-unplug rebuild) — Phase 1.
- Denormal flush-to-zero flags — Phase 1 (no DSP means no denormals to flush; zero effect now).
- CCD pinning — dropped (per §5.5 it is expected to show nothing).
- Tray UI, IPC, parameter snapshot, config hot-reload — Phase 3 / 3.5.
- External measurements (LatencyMon DPC baseline, PresentMon frame-time) — manual runbook steps,
  no application code.

## 3. Module structure

Three CMake targets, establishing the §2.1 algorithm/delivery boundary from day one and, more
concretely, keeping the bug-prone logic unit-testable without hardware.

- **`transport-core`** — platform-agnostic static library, **must not `#include <windows.h>`**.
  Enforced mechanically by keeping it a separate target with no Windows link dependencies.
  - `SpscRing<T>` — lock-free single-producer/single-consumer ring, fixed capacity set at
    construction, **cache-line-padded (128 B, Zen 5) head/tail indices** in separate structs (§9.5).
  - `Framer` — **bidirectional**: accumulates arbitrary-size sample runs into exact 480-sample
    frames (capture side) and doles frames back out into arbitrary-size render buffers with a
    carried remainder (render side).
  - `ChannelExtractor` — extract channel 0 from an interleaved N-channel buffer; mono→stereo
    duplication for the render side.
  - `OccupancyStats` — occupancy (with wraparound) and high-water tracking.
  - `DropoutCounter` — atomic under/overrun counters + event timestamps.
  - `Config` — parse TOML + apply CLI overrides.
- **`win-audio`** — Windows/WASAPI layer (validated by the run, not unit tests).
  - `WasapiCapture`, `WasapiRender` — shared-mode, **event-driven** `IAudioClient` streams.
  - `MmcssThread` — real `AvSetMmThreadCharacteristicsW(L"Pro Audio")` +
    `AvSetMmThreadPriority(AVRT_PRIORITY_CRITICAL)`, reverted on teardown (§5.4).
  - `RtMemory` — `VirtualLock` + pre-touch helper for ring buffers.
  - `DeviceEnumerator` — list endpoints (ID, friendly name, mix format), resolve by ID, format guard.
- **`phase0-app`** — the console executable wiring config → devices → rings → threads → logger,
  plus a Ctrl+C clean-shutdown handler and a `--list-devices` command.

`denoise-core` does not exist yet (no DSP in Phase 0). At Phase 2 the relay's copy becomes
`denoise-core::ProcessFrame`, and the pure pieces here move under it as a rename, not surgery.

## 4. Topology, threads, and driving model

```
K688 ─► [Capture thread] ─► Ring A ─► [Relay thread] ─► Ring B ─► [Render thread] ─► CABLE Input
         WASAPI event                  MMCSS Pro Audio             WASAPI event
                                       (pass-through copy)

                         [Logger thread, normal priority] ──► drift CSV + ~1 Hz console
```

- **Capture thread** — waits on the WASAPI capture event; `GetBuffer` → extract ch0 → `Framer`
  accumulates 480-sample frames → push to Ring A → signal the relay's frame-ready event → `ReleaseBuffer`.
- **Relay thread (the future DSP thread)** — MMCSS "Pro Audio", `AVRT_PRIORITY_CRITICAL`. Blocks on
  an **auto-reset event** (not a mutex, not a condvar-with-lock — no lock to invert) signalled by
  capture; on wake, drains Ring A, copies each frame verbatim, pushes to Ring B. In Phase 2 the copy
  is replaced by inference; the wait/push shape is unchanged.
- **Render thread** — waits on the WASAPI render event; pulls frames from Ring B via `Framer` to fill
  the exact render buffer size. On Ring B empty → writes silence for the missing samples **and counts
  an underrun** (this is precisely the dropout we are hunting; there is no dry frame to pass in Phase 0).
- **Logger thread** — normal priority. Once/sec reads atomic snapshots of occupancy and counters,
  appends a CSV row, and prints a console status line. **RT threads never touch the file** (§12).

Four worker threads + main, matching the production thread inventory (capture, DSP, render, plus a
non-RT writer — here the logger stands in for it).

## 5. Framing, ring sizing, and priming *(incorporates the key gap fix — see §12)*

- **Frame size:** 480 samples / 10 ms, fixed. Keeps Ring B occupancy in the same units Phase 1.5's
  PI controller will observe, and exercises the exact framing path we ship.
- **Ring sizing is configurable and profile-independent in Phase 0.** The §5.2 profile depths
  (Balanced = 16 frames, etc.) are *not* used here — see §12 for why they cannot survive an 8-hour
  uncompensated drift test.
  - **Drift-measurement run:** deep ring, default **1024 frames (~10.7 s)**. Sized so that expected
    ordinary drift (±100 ppm per §5.8) cannot reach a rail within 8 hours, letting occupancy trace a
    clean monotonic slope while achieving zero dropouts.
  - **Bring-up run:** shallow ring (e.g. 16 frames) for the quick "talk and hear yourself" sanity
    check, where multi-second priming latency would be annoying.
- **Priming:** before the render stream starts, pre-fill Ring B to its **target occupancy
  (half capacity)** with silence. This is the drift setpoint (§5.8 step 2 — "half full"), gives the
  occupancy room to move in either direction, and prevents a burst of startup underruns from
  polluting the dropout count.

## 6. Device and format handling

- `--list-devices` enumerates render + capture endpoints (ID, friendly name, mix format) and exits —
  the way you discover the K688 and CABLE Input device IDs for the config.
- Both devices addressed **by ID**, never "default" (§4.1 step 3 — so game audio never leaks into
  CABLE Input).
- **Format guard (§5.7):** `GetMixFormat()` on both endpoints. If `nSamplesPerSec != 48000`, refuse
  with a message pointing at the endpoint's Advanced setting rather than proceeding into silent
  resampling. Sample format: handle IEEE float32 (the WASAPI shared-mode engine default) directly;
  convert 16-bit PCM if encountered; refuse anything else with a clear message. Channel count:
  the mix format is often 2ch even for a mono mic — take channel 0 (§5.7).
- **Minimal lifecycle only:** on `AUDCLNT_E_DEVICE_INVALIDATED` (or any fatal stream error), the
  affected WASAPI thread surfaces it via `onError`; the bridge then **fails fast visibly** — clean
  stop of the whole bridge, a clear stderr message, and a non-zero process exit — so an unattended
  run ends *visibly* rather than zombie-ing with one dead stream. Automatic rebuild/reconnect is
  deliberately deferred to Phase 1, where the full `IMMNotificationClient` device-lifecycle lives
  and where the Logger's monotonic `elapsed_ms` clock and CSV must be preserved across a reconnect
  (a partial rebuild here would reset the drift time-axis and corrupt the measurement).

## 7. Instrumentation and the drift measurement

- **Drift CSV**, one row/sec: `elapsed_ms, ringA_frames, ringB_frames, ringA_hi, ringB_hi,
  underruns, overruns`. `elapsed_ms` from `QueryPerformanceCounter` (monotonic); a wall-clock start
  stamp goes in the CSV header for correlating with the gaming session.
- **Drift math:** fit a line to `ringB_frames` vs time. `drift_ppm = (slope_frames_per_s × 480 /
  48000) × 1e6`. A monotonic trend is drift; its sign tells you which clock is faster.
- **Dropout events log:** each under/overrun timestamped on its own line, so clicks can be correlated
  with events.
- **Console telemetry:** ~1 Hz — elapsed, Ring A/B occupancy, high-water, underruns, overruns.

## 8. Config and CLI

- **Config (TOML), read once at startup** (hot-reload is Phase 3.5): capture device ID, render
  device ID, ring depth (frames), CSV path, log level.
- **CLI:** `--list-devices`; `--config <path>`; overrides for device IDs and CSV path.
- TOML parsing via `toml++` (header-only, `FetchContent`).

## 9. Error handling and shutdown

- COM init/teardown; every HRESULT checked with an actionable message.
- Format guard refuses mismatched sample rate rather than degrading silently.
- **Clean shutdown:** Ctrl+C → set `running=false` → **stop dropout accounting** → `IAudioClient::Stop`
  on both streams → signal all events to wake threads → join capture, relay, render, logger → flush
  CSV → `AvRevertMmThreadCharacteristics` → `CoUninitialize`. Leaking MMCSS registration degrades the
  whole system (§12), so teardown is explicit and covered by a test of the shutdown path.

## 10. Hardening pulled forward

- **`VirtualLock` + pre-touch** the ring buffers at init (write a byte to every page before the RT
  threads start). Rationale: Phase 0's job is a clean zero-dropout / drift measurement; a paged-out
  buffer causing a fault-induced underrun would be **mis-attributed** to drift or scheduling. Locking
  removes paging as a confound. (Denormal flags deliberately *not* pulled forward — see §2.)

## 11. Testing strategy

- **Unit tests (GoogleTest via `FetchContent`, no hardware):**
  - `SpscRing` — fill/empty/wrap, and a concurrent producer/consumer stress test.
  - `Framer` — arbitrary run sizes → exact 480-frames and back, remainder carried correctly across
    calls, both directions.
  - `ChannelExtractor` — N-channel → ch0, mono → stereo.
  - `OccupancyStats` — occupancy with wraparound, high-water.
  - `DropoutCounter` — atomic increments, event timestamps.
  - `Config` — TOML parse + CLI override precedence.
- **Integration / manual:**
  - `--list-devices` shows K688 + CABLE Input.
  - Short bring-up (shallow ring): talk → monitor CABLE Output → hear yourself.
  - **8-hour drift run (deep ring):** the real acceptance test — zero dropouts + a clean occupancy CSV.

## 12. Logic-gap review — findings folded into the design above

A deliberate pass over the design surfaced these; each is resolved above.

1. **"Zero dropouts over 8 h" vs. uncompensated drift (the significant one).** With no drift
   compensation in Phase 0, any real clock mismatch drives a ring monotonically to a rail. At 20 ppm
   a §5.2 Balanced 16-frame ring rails in ~66 min; even the deepest profile fails inside the 8 h
   window. So the blueprint's own §5.2 depths cannot satisfy its Phase 0 "zero dropouts over 8 h"
   criterion. **Resolution:** Phase 0's measurement rig uses a special **deep ring (~1024 frames)**
   sized so ordinary drift can't rail it within 8 h — giving both a clean slope *and* zero dropouts.
   This makes the coherent story explicit: Phase 0 measures the slope with a deep ring; Phase 1.5
   builds compensation so the *shallow* production rings (§5.2) survive 8 h. If even the deep ring
   rails, drift is pathological (usually a format mismatch, §5.7) and you want to see it, not hear it.
2. **Startup underrun burst.** An empty Ring B at render start underruns until capture fills it,
   polluting the dropout count. **Resolution:** prime Ring B to half capacity with silence before
   starting render (§5) — which also lands occupancy on the drift setpoint.
3. **Bidirectional framing.** WASAPI packet sizes are not multiples of 480 on either side.
   **Resolution:** `Framer` carries a remainder in both directions (§3, §5).
4. **Dropout accounting during shutdown.** Stopping streams naturally produces underruns.
   **Resolution:** stop accounting the moment shutdown is signalled (§9).
5. **Sample-format assumption.** Shared-mode is usually float32 but not guaranteed.
   **Resolution:** handle float32 directly, convert PCM16, refuse others with a clear message (§6).
6. **Relay wait primitive & RT discipline.** A blocking wait in the DSP thread must not be a lock.
   **Resolution:** auto-reset event signalled by the RT capture thread — no lock to invert (§4).

## 13. Exit criteria

- **Zero dropouts over a full 8-hour session** (deep ring), including a long gaming session.
- Ring B occupancy CSV plotted; the slope recorded as this hardware pair's ppm drift figure.
- *(Manual, no app code:* LatencyMon DPC delta app-off vs app-on within variance; PresentMon
  frame-time delta in one game within run-to-run variance.*)*
