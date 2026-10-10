# Performance & Fix Review — 2026-10-10

_Code-grounded review of the whole program (worker RT path, DSP chain, engines, IPC, UI).
Follows up the [2026-08-21 whole-program deep dive](2026-08-21-whole-program-performance-deep-dive.md):
F1–F5 there are done and were **not** re-reported. Everything below is new. Nothing has been
implemented yet; this doc records the findings and recommended fixes._

## Method

- Read every first-party file on the audio path: `bridge.cpp`, `framer.h`, `spsc_ring.h`,
  `resampler.cpp`, `drift_control.cpp`, `dsp_chain.cpp` and its leaves, `dynamic_engine.cpp`,
  `rnnoise_filter.cpp`, `dfn3_streaming.cpp`, `stft.cpp`, `dfn3_features.cpp`, `dfn3_postdsp.cpp`,
  the WASAPI threads, `logger.cpp`, the worker `main.cpp`, `pipe.cpp`, `telemetry_shm.cpp`,
  `worker_supervisor.cpp`, and the `app_window.cpp` input and paint paths.
- Micro-benchmarked the Windows-free DSP leaves on Linux (g++ -O2, 20k hops of 480 samples).
  The absolute numbers will differ under MSVC on the target machine, but the ratios between
  stages hold. Bench source: session scratchpad (not committed).

| Stage (one 480-sample hop) | µs/hop | Notes |
|---|---|---|
| `Limiter::process` | **60.5** | O(n·L) look-ahead scan, see P2 |
| `Agc::process` (VAD on) | 11.3 | per-sample `pow` + `log10` + `sqrt` |
| DC blocker + HPF, **digital silence, no FTZ/DAZ** | **119.1** | IIR state decays into denormals, see P1 |
| DC blocker + HPF, digital silence, FTZ/DAZ on | 4.8 | **~25× faster** |

Context: the UI's default profile is `engine = "rnnoise"` with `dsp_chain = true`
(`settings.cpp:41-43`), so the limiter, AGC and IIR stages run on the **default** path.

---

## Summary (priority order)

| # | Finding | Kind | Severity | Effort |
|---|---|---|---|---|
| **B1** | ~~Slider changes can be silently dropped. The last drag position may never reach the worker~~ **FIXED 2026-10-10** | Bug | **High** | S |
| **B2** | ~~Device re-select overfills ring B, adding about 40 ms of latency for about 4.5 min plus overruns~~ **FIXED 2026-10-10** | Bug | **High** | S |
| **P1** | ~~No flush-to-zero / denormals-are-zero (FTZ/DAZ) on any audio thread. Measured 25× slowdown in silence~~ **FIXED 2026-10-10** | Perf | **High** | XS |
| **P2** | Limiter look-ahead is O(n·L), the most expensive non-engine stage (60 µs/hop) | Perf | Medium | S |
| **P3** | RNNoise is built scalar-only. The vendored SSE4.1/AVX2 kernels are unused | Perf | Medium | M |
| **P4** | Worker logger wakes at 100 Hz all session and writes `drift.csv` without limit | Perf/hygiene | Medium | S |
| P5 | AGC does transcendental math on every sample | Perf | Low | S |
| P6 | DFN3 windows may be larger than the standard model needs. Investigate per-model W | Perf | Low (DFN3 only) | M |
| C1 | Telemetry shared memory is a plain `memcpy` with no seqlock, so the UI can read torn snapshots | Bug | Low | XS |
| C2 | A DFN3 model load holds `engMutex_` and stalls the worker main tick, including device-loss detection | Bug | Low | S |
| P7 | Small per-sample and per-hop waste (trig, `%`, brushes). Fold into neighbouring fixes | Perf | Trivial | XS |

---

## Bugs

### B1 — Slider updates can be silently dropped

> **FIXED 2026-10-10.** New pure `ParamOutbox` (`voicecleaner/param_outbox.h`, 5 unit tests)
> holds the newest undelivered `Params` and clears it only on a successful send. Drag moves are
> throttled to at most one send per 30 ms. Drag end (WM_LBUTTONUP) and discrete clicks force a
> send. `tick()` retries anything still pending once the worker is ready, and the ready-path
> tuning push also goes through the outbox. `PipeClient::send` now retries the pipe-recycle gap:
> `ERROR_PIPE_BUSY` → `WaitNamedPipeW`, up to 100 ms; `ERROR_FILE_NOT_FOUND` → up to 20 ms when
> no server has been seen, so a dead worker still fails fast. A Windows regression test sends
> 200 commands back to back and requires all of them, in order
> (`PipeClient.BackToBackSendsAreAllDelivered`). This also covers the device-pick path, where the
> engine/tuning push right after `SelectCaptureDevice` used to hit a worker busy rebuilding.

**Where:** `app_window.cpp:706-713` (WM_MOUSEMOVE → `sendCurrent`), `app_window.cpp:726`
(WM_LBUTTONUP), `pipe.cpp:19-22` (server), `pipe.cpp:85-93` (client).

- Each WM_MOUSEMOVE during a drag makes a **synchronous, connect-per-message** pipe send on
  the UI thread. These come at mouse rate, often more than 100 per second.
- The server creates the pipe with `nMaxInstances = 1`. After each message it disconnects,
  closes, and calls `CreateNamedPipeW` again.
- The client calls `CreateFileW` with no `WaitNamedPipe` and no retry. If a send lands in that
  gap it fails with `ERROR_PIPE_BUSY` or `ERROR_FILE_NOT_FOUND`, and `send()` returns false.
  Nothing checks the return value.
- WM_LBUTTONUP only calls `persistTuning()` (writes to disk). It does **not** re-send.

**Effect:** if the *last* move event of a drag hits the gap, the UI and `settings.toml` show
one value while the worker keeps processing with an older one. Nothing corrects this until the
next param change or a worker restart. The high publish rate also weakens the "UI-cadence
publish" assumption behind the two-slot `ParamStore` (`param_store.h:13-18`).

**Fix:**
1. On WM_LBUTTONUP, always send the final `state_->current()`. This alone fixes the
   user-visible bug.
2. Coalesce drag sends. Either send on a short timer (for example at most every 30 ms, with a
   "dirty" flag), or only when the quantized value changes.
3. Make `PipeClient::send` call `WaitNamedPipeW(name, ~50 ms)` once on `ERROR_PIPE_BUSY` and
   retry.

Optional: keep one persistent client connection instead of connecting per message.

### B2 — Device re-select overfills ring B (latency bump plus overruns)

> **FIXED 2026-10-10.** `Bridge::start()` now clears `ringA_`, `ringB_`, `accum_`, `reframe_`
> and `dist_`, and resets the resampler, before priming. New `clear()` on `SpscRing` (documented
> as only valid with no producer or consumer running) and on the FIFOs. 4 unit tests, including
> the stop→start residue scenario (`SpscRing.ClearThenPrimeRestoresHalfFullAfterReselect`).

**Where:** `bridge.cpp:45-46` (priming), `bridge.cpp:272-325` (`stop` / `reselect*`).

- `stop()` stops the capture, relay and render threads but **never drains `ringA_`, `ringB_`,
  `accum_`, `reframe_` or `dist_`**.
- At steady state `ringB_` holds about `ring_frames/2` (the drift setpoint).
- `start()` then pushes **another** `capacity/2` silent frames, so ring B begins nearly full.
  Pushes that fail count as overruns.
- The drift controller can only pull occupancy back at `driftMaxPpm = 150` ppm. With the
  default `ring_frames = 8`, the 4-frame (1920-sample) excess takes
  1920 / (48000 × 150e-6) ≈ **267 s** to drain.
- So after every mic or output change there is about **+40 ms of extra latency for roughly
  4.5 minutes**, plus stale audio from the previous mic still sitting in ring A.

No test covers the reselect path (only command codec and pipe round-trip tests exist).

**Fix:** in `start()`, before priming, drain everything. Pop `ringA_` and `ringB_` until empty
(single-threaded at that point, because all threads are joined). Add a `clear()` to
`FloatFifo` and the framer classes, and call `resampler_.reset()`. A unit test can pin the
behavior: prime → simulate a stop with residual frames → start → assert
occupancy == capacity/2.

---

## Performance

### P1 — No FTZ/DAZ on any audio thread (do this first)

> **FIXED 2026-10-10.** Set in the `MmcssThread` constructor, which runs first on the capture,
> render and relay threads. Not restored on destruction. Only those worker threads are affected:
> the offline tools (`offline-cli`, the DFN3 parity scripts) don't use `MmcssThread`, so their
> numbers are unchanged.

**Where:** a repo-wide search finds `_MM_SET_FLUSH_ZERO_MODE` / `_MM_SET_DENORMALS_ZERO_MODE`
**only in docs**. The Phase 0 spec deferred it "to Phase 1" and it was never added. Blueprint §9.5
calls it "the single highest-value change in this section."

**Why it matters:** during silence the IIR state decays into the denormal range. Silence here
means a muted mic, `AUDCLNT_BUFFERFLAGS_SILENT` (which the capture thread turns into exact
zeros), or a gated or suppressed signal. Affected state includes the DC blocker `y1_`, biquad
state, declicker `envF_ *= fastCoef_`, AGC `envRms_`, and RNNoise GRU state. Measured cost:
**DC blocker + HPF alone went from 4.8 to 119 µs/hop (25×)**. The same penalty applies across
the whole chain and the RNNoise network, and it hits exactly when suppression is working hardest.

**Fix:** set both flags in the `MmcssThread` constructor (`mmcss_thread.cpp`). It already runs
first on all three RT threads: capture, render and relay.
`#include <xmmintrin.h>` / `<pmmintrin.h>`, then:

```cpp
_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
```

ORT runs on the calling relay thread (`SetIntraOpNumThreads(1)`), so DFN3 inherits the flags.
ORT also has a `session.set_denormal_as_zero` option if needed.

**Check after the change:** FTZ changes numerics in the ~1e-38 range. Re-run the DFN3 parity
scripts (`tools/dfn3_stream_check.py`, `dfn3_compare.py`) and the unit suite. Expect no audible
or measurable difference.

### P2 — Limiter look-ahead scan is O(n·L)

**Where:** `limiter.cpp:20-21`. For every sample, `peak = max |delay_[k]|` over all 144
look-ahead taps, which is about 69k `fabs`/`max` per hop. Measured **60 µs/hop**, about 5× the
AGC and the most expensive stage outside the engine.

**Fix:** keep a sliding-window maximum with a monotonic deque. It can be a fixed-size index ring
of `L_+1` entries, preallocated so nothing allocates on the RT thread. That makes it O(1)
amortized per sample, and the output is **bit-identical** because it computes the same max.
Also replace `widx_ = (widx_+1) % L_` with a compare. The existing limiter tests pin the
behavior. Add one that compares old and new output on random input.

### P3 — RNNoise compiled scalar-only

**Where:** `third_party/rnnoise/CMakeLists.txt` ("Scalar (C) build only: no x86 RTCD/SIMD
variants"). There are also **no `/arch` or `/O` overrides** in the root `CMakeLists.txt`, so
everything gets the MSVC x64 default (SSE2) and the compiler cannot auto-vectorize to AVX2.
Upstream RNNoise ships run-time CPU detection (RTCD) with SSE4.1 and AVX2 dense/GRU kernels.
They are already vendored here: `src/x86/nnet_avx2.c`, `nnet_sse4_1.c`, `x86cpu.c` and
`x86_dnn_map.c`.

**Fix:**
- Add those files with `RNN_ENABLE_X86_RTCD`, `CPU_INFO_BY_ASM` (or the MSVC `__cpuid` path),
  and per-file `/arch:AVX2` on `nnet_avx2.c`. This keeps one binary that still runs on any
  x64 CPU.
- Expect a several-fold cut in RNNoise compute. That means lower CPU and battery use, and more
  headroom for warm-standby (F6 in the earlier doc).

**Check after the change:** float sum order changes, so output is not bit-exact. Do an offline
A/B on the existing quality WAVs with `offline-cli` and diff the outputs; expect differences
around 1e-6. Medium effort because it is build surgery on vendored code.

### P4 — Worker logger: 100 Hz wakeups and an unbounded CSV

**Where:** `logger.cpp:48-75`, `config.h:9` (`csvPath = "drift.csv"` default, never
overridden by `settings.cpp`).

- The logger thread sleeps 10 ms in a loop **for the entire session**. That is 100
  wakeups/s, which keeps the core out of deep C-states, all for a ring high-water figure the
  relay already tracks (`ringBHigh_`).
- Once a second it appends a CSV row (≈90 B/s ≈ **8 MB/day**, no rotation) and calls
  `printf` + `fflush(stdout)`. The UI launches the worker with `CREATE_NO_WINDOW` and no
  redirected handles, so nobody reads stdout.
- `Logger::start()` runs on every `Bridge::start()`, so a device reselect truncates the file
  (opened in `"w"` mode) and loses the history.
- The CSV lands in the exe directory. If that is under Program Files, the open just fails
  silently.

**Fix:**
- Make the CSV opt-in: an empty `csv_path` (the new default for the UI-launched worker)
  disables the logger thread entirely. Keep it on for the CLI/dev workflow.
- When it is enabled, poll at 1 Hz and take the high-water mark from the relay's counter.

### P5 — AGC transcendental math per sample

**Where:** `agc.cpp:15-30`. Each sample does `sqrt`, `log10` (when VAD is active) and
`pow(10, gainDb/20)`. Measured 11 µs/hop.

**Fix (optional):**
- Run the gain in the linear domain, or compute `desiredDb` once per sub-block (for example
  every 16 samples) and interpolate the linear gain.
- Use `exp2`-based conversion.

Low priority. Do it only if you are already touching the AGC, and use the AGC tests to pin
the envelope behavior.

### P6 — DFN3 windowed graphs: per-model window size (investigate)

**Where:** `dfn3_streaming.cpp:474-494`. The encoder and decoders run on a rolling window of
`W` frames every hop and only keep the last frame. W is the **union** of both models' needs
(enc_conv 6, erb_dec 3, df_dec 5). The code comment says the standard model needs
"5/1/5 + lookahead", which suggests erb_dec (and maybe enc_conv) recompute frames that the
standard model, the default DFN3 path, never uses.

**Fix:** read W per model at load time, as hidden size and lookahead already are. Validate
that output is bit-exact with `tools/dfn3_stream_check.py` before adopting it. This only
matters when DFN3 is active, and the gain depends on how much of the hop time each graph
takes. Profile first: `hopStats_` already isolates it.

### P7 — Trivia (fold into a neighbouring change; not worth their own PR)

- `dynamic_engine.cpp:83-87`: the crossfade computes `cos`/`sin` per sample, but `th` is
  constant across the frame. Hoist it. This only runs during the 15-frame fade.
- `dsp_chain.cpp:35`: `hpf_.setHighpass` runs `cos`/`sin` every hop even after the ramp has
  converged. Skip it when `|hpfCur_ - hpfTarget_| < 1e-3`. Likewise the limiter
  `setCeilingDb` `pow`.
- `dynamic_engine.cpp:38`: `pushDelay` uses `%` per sample. Use a compare-wrap.
- `framer.h:28-46`: `FloatFifo` copies sample by sample with a branch. Two `memcpy` spans
  would do.
- `app_window.cpp:234-242`: nine D2D brushes are created and released on every paint.
  Create them once with the render target.

---

## Other correctness notes

### C1 — Torn telemetry reads

`TelemetryWriter::write` and `TelemetryReader::read` (`telemetry_shm.cpp:22-44`) are plain
`memcpy` calls with no sequence protocol. The UI can read a struct where `seq` is new and the
other fields are half-old. Impact is low (display only, ~1 Hz, small POD).

**Fix:** a seqlock. The writer bumps `seq` to odd, copies, then bumps to even with release
ordering. The reader retries while `seq` is odd or changed. Liveness detection already keys
on `seq`, so the semantics are preserved.

### C2 — DFN3 load holds `engMutex_`

`Bridge::request_engine` (`bridge.cpp:135-143`) calls `requestActive` → `installLeaf` → the
loader, which builds the ORT sessions and runs 16 warm-up hops, while holding `engMutex_`.
The worker main tick's `reclaimRetiredEngines()` (`main.cpp:183`) takes the same mutex, so
**device-loss detection stalls for the length of the model load**. Each `SetParams` also calls
`request_engine`, so pipe commands queue behind the load too.

**Fix:** build the leaf outside the lock, then lock only to publish. This is a small
`DynamicEngine` API split: `prepareLeaf` / `publishLeaf`.

---

## Addendum: settings and UI shell (second pass)

These came from a second pass over the non-RT files: `settings.cpp`, `config.cpp`,
`voicecleaner/main.cpp`, `tray.cpp`, `ui_state.cpp`, `device_picker.cpp`,
`respawn_policy.cpp`, `device_enumerator.cpp` and `rt_memory.cpp`.

### S1 — `settings.toml` is rewritten in place (risk of losing all profiles)

> **FIXED 2026-10-10.** `Settings::save` now returns `bool`. It writes `settings.toml.tmp`, checks
> the stream, then calls `std::filesystem::rename` over the target (MSVC: `MoveFileExW` with
> replace-existing). It retries for up to ~200 ms if the worker has the file open (sharing
> violation). On failure it removes the temp file and leaves the old file intact. 2 unit tests.
> The optional `.bak` fallback was not added.

**Where:** `settings.cpp:118-119`, which does `std::ofstream out(path); out << root;`.

- The file is saved on every slider release, device pick, ring change and low-latency toggle.
- The write truncates the file first and is not atomic. A crash, kill or power loss mid-write
  leaves a partial file.
- On the next start, `Settings::load` catches the parse error and **silently returns empty
  settings** (`settings.cpp:27`). All per-mic profiles are lost and the app falls back to
  first-run.
- The worker reads the same file through `Config::load`, which **throws** on a parse error.
  If a respawn races a UI save, the worker exits with "config parse". Because it never became
  ready, `RespawnPolicy` treats it as a launch failure and gives up instead of retrying.

**Fix:** write to `settings.toml.tmp`, then
`MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`. Optionally keep
a `.bak` copy and fall back to it when parsing fails.

### S2 — Settings path is narrowed character by character and lives next to the exe

**Where:** `voicecleaner/main.cpp:14-19` and `:37-41`.

- The wide path is narrowed one character at a time (`static_cast<char>(c)`, marked "ASCII
  path"). If the install directory contains any non-ASCII character (for example
  `C:\Users\José\…`), the UI loads and saves a **mangled path**. Settings never persist, while
  the worker gets the correct wide path on its command line, so the two read different files.
- The settings live in the exe directory. If the app is installed under Program Files, saves
  fail silently because nobody checks the `ofstream` result.

**Fix:**
- Convert with `WideCharToMultiByte(CP_UTF8, …)` (`tray.cpp` already has the reverse helper).
  Note that toml++ / `std::ofstream` on MSVC need a `std::filesystem::path` built from the wide
  string to open non-ASCII paths, not a narrow `char*`.
- Longer term, move settings to `%LOCALAPPDATA%\VoiceCleaner\`.

### S3 — The tray icon is updated 10 times a second whether or not anything changed

**Where:** `tray.cpp:54-63`, called from every `AppWindow::tick()`. Each call is a
`Shell_NotifyIconW(NIM_MODIFY)` round-trip to Explorer, even when the tooltip and icon are
unchanged.

**Fix:** cache the last tooltip string and skip the call when neither the tooltip nor the icon
changed. Low priority.

### S4 — Stale first-run text

`ui_state.cpp` `statusLine()` for `NoConfig` still says "the mic picker arrives in Phase 4",
but the picker exists now. Trivial copy fix.

---

## Explicitly checked and fine (no action)

- SPSC ring ordering, the `size()` observer retry, and the VirtualLock + touch of rings and
  FIFOs.
- The relay hot path makes no heap allocation in steady state (`mono_` and `scratch_` are
  reserved; the WASAPI render `mono_` never exceeds `bufferFrames_`).
- `FrameTimeStats` percentile scans (800 buckets, about 1 Hz on the relay) cost nothing
  significant.
- The resampler history indexing stays within `h_[0..2]` (`pos_ ≥ -2` after carry).
- The DFN3 steady state is allocation-free in our code, except for ORT's internal arena.
- UI repaint is gated on `seq` (F5) and the telemetry poll is 10 Hz. Both are fine.

## Verification status of the 2026-10-10 fixes

- **Run on Linux:** transport-core 75/75 and vc-ipc 13/13 pass. voicecleaner-core passes 45/45
  without `respawn_policy`, which GCC can't compile (a GCC limitation; MSVC builds it). These
  include 11 new tests.
- **Syntax-checked only (MinGW cross-compiler, `-Wall -Wextra`):** `mmcss_thread.cpp`,
  `pipe.cpp`, `bridge.cpp`, `app_window.cpp` and `test_pipe.cpp`. No new warnings.
- **Not run (needs Windows):** the full MSVC build, `win-ipc-tests` (including the new burst
  test), and on-device checks:
  1. Drag a slider fast and release. The audio should match the final position.
  2. Switch mic, then output. The latency readout should return to the normal value right away,
     not about 40 ms higher, and there should be no new dropouts.
  3. Mute the mic for a minute with DFN3 and with RNNoise. The hop p99 should stay flat.
  4. Kill the UI while you drag a slider. `settings.toml` should still load.

## Suggested order

1. ~~**P1**, **B1**, **B2**~~ and ~~**S1**~~: done 2026-10-10.
2. **P2**, **P4** — small, self-contained, testable.
3. **C1**, **C2** — cheap robustness.
4. **P3**, then **P6** — bigger CPU wins that need numerical A/B validation.
5. P5 and P7 opportunistically.
