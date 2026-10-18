# Performance analysis (v1.6 → v1.6.1)

_2026-10-18. Scope: the whole Windows app after v1.6 (RNNoise only). Outcome: one change, the limiter._

## Method

Each DSP stage was timed in isolation and as the full `DspChain`, per 480-sample (10 ms) frame, on the
user's real recording `both.wav` (typing while talking, 1,506 frames). The build was Linux gcc -O2 with
FTZ/DAZ on, matching the worker's audio threads, on a 2.1 GHz Xeon with AVX2. Each figure is the best of
5 runs. Absolute numbers differ on the user's PC; the proportions are what matter. The recordings stay out
of the repo because they contain the user's voice.

## Before (v1.6)

| Stage | µs / frame | Share |
|---|---|---|
| RNNoise (int8, AVX2 dispatch) | 167 | 68% |
| **Limiter** | **60** | **25%** |
| AGC | 12 | 5% |
| Declicker | 3.8 | |
| High-pass (incl. per-frame coefficient update) | 3.4 | |
| Resampler (drift) | 2.9 | |
| DC blocker | 2.2 | |
| Noise gate | 2.1 | |
| Wet/dry mix | 0.4 | |
| **Full chain + RNNoise** | **245** | ≈2.5% of one core |

## Finding: the limiter scanned its whole look-ahead per sample

`Limiter::process` took the window peak by scanning all 144 delay-line samples for every input sample,
which is 69k `fabs`/`max` per frame. Replacing the scan with a **monotonic-queue sliding-window maximum**
gives exactly the same peak, because both compute the max of the same L+1 magnitudes. Each sample is
pushed and popped at most once, so the cost is O(1) amortized.

Design decisions:
- **Fixed ring of L+1 slots, allocated in the constructor.** Nothing is allocated on the relay thread.
  Leaving the delay line's initial zeros out of the queue is safe, since a magnitude is never below 0.
- **Expire before push.** The window is positions [pos−L, pos]. The front entry must be dropped
  *before* the new sample is pushed. Pushing first lets a strictly falling run of more than L+1 samples
  overflow the ring and overwrite the true peak.
  - The scratch prototype pushed first. It still matched on real audio, because the wrong (smaller) peak
    only changes the output once the gain has released above ceiling/true-peak while that peak is still
    in the window.
  - The test includes that exact case: silence, then a long, shallow, strictly falling ramp. The
    push-first order was checked to fail it.
- **The old scan limiter lives on in the test as the reference.** `SlidingPeakMatchesScanReferenceExactly`
  compares the two sample for sample (`ASSERT_EQ`, not near). It covers L = 1, 2, 16 and 144, with odd
  block sizes so the queue state crosses `process()` calls. Its input mixes loud noise, falling and rising
  ramps longer than L, the shallow ramp, impulses, equal repeated peaks and silence.
  `ResetMatchesAFreshLimiter` covers `reset()`.

Verification:
- **Real recordings.** typing/speech/both.wav were run at drive ×1, ×8 and ×30 through old vs new.
  0 samples differed in all nine runs.
- **Unit tests.** All 146 tests in the Linux build pass.

## After (v1.6.1)

| | v1.6 | v1.6.1 |
|---|---|---|
| Limiter | 60 µs | 10 µs |
| Chain without RNNoise | 77 µs | 27 µs |
| Chain + RNNoise | 245 µs | 203 µs |

## Checked and left alone

- **AGC (12 µs).** It does `sqrt`, `log10` and `pow` per sample. It could be cheaper, but the saving is
  small and any rewrite changes the output slightly, so it was left as is.
- **RNNoise** already uses int8 weights with runtime SSE4.1/AVX2 dispatch (v1.3). It is the irreducible
  bulk of the cost.
- **Real-time hygiene is already in place:**
  - FTZ/DAZ on every audio thread (`MmcssThread`);
  - MMCSS "Pro Audio" priority;
  - no allocation on the audio threads (fixed FIFOs, reserved scratch);
  - rings and FIFOs VirtualLock'd;
  - wait-free parameter snapshot (`ParamStore`);
  - telemetry aggregated once per second.
- **UI:** the repaint is gated on telemetry seq/status changes, so idle ticks cost nothing.
- **Latency** is set by design, not by compute:
  - RNNoise's fixed 20 ms;
  - the LATENCY ring (default 8 frames ≈ 40 ms, minimum 4 ≈ 20 ms);
  - WASAPI/VB-Cable buffers.

  Faster code does not change any of these.
