# DFN3 `_ll` Quality Validation on Impulsive (Keyboard) Noise

_Run 2026-08-15 on Windows 11 (x86-64), branch `test/dfn3-quality`._

Does VoiceCleaner's **live** DeepFilterNet3 engine (`dfn3-stream`, model
`models/dfn3-ll`) actually remove keyboard-style impulsive noise, and does it
beat RNNoise at it? This is a **ground-truth** test: the noisy clip is built from
KNOWN clean speech + KNOWN noise kept as separate signals, so we measure true
SNR improvement against the clean reference, not just similarity to another tool.

## TL;DR verdict

1. **Streaming == whole-utterance: YES, exactly.** `dfn3-stream` matches the
   whole-utterance `_ll` golden at **Pearson r = 1.000000** (10.0 ms lag) and
   produces byte-for-byte-equal quality metrics — now confirmed on realistic
   voiced+impulsive content, not only the old synthetic sine clip.
2. **Does live DFN3 denoise impulsive noise: YES.** Positive SNR improvement
   (+1.34 dB raw, **+8.31 dB scale-invariant SDR**) and ~40 dB suppression of the
   noise-only lead-in. It is a real denoiser, not a no-op.
3. **Does DFN3 beat RNNoise: NO — this is the headline red flag.** On this clip
   the **bare RNNoise core beats bare DFN3** (+13.81 vs +8.31 dB SI-SDR, with far
   cleaner speech: NRMSE 0.14 vs 1.20). DFN3 only "wins" against the *production*
   RNNoise path (`--chain`, +4.31 dB) because VoiceCleaner's DSP chain applies a
   VAD-gated AGC that boosts this quiet clip ~6.8x and distorts RNNoise's output —
   a **chain artifact, not DFN3 superiority.** This test does **not** support
   DFN3's "better for keyboard clicks" justification; if anything it shows the
   opposite. See caveats — the synthetic clicks may be unrepresentative.

## Clip design (`tools/dfn3/dfn3_make_quality_clip.py`)

Deterministic (seed 20260815), 48 kHz mono, 4.0 s. Written as three aligned
files so metrics have ground truth: `qual_mix.wav` (input to denoise),
`qual_clean.wav` (clean speech), `qual_noise.wav` (noise alone). By construction
`mix == clean + noise` sample-for-sample.

- **Clean speech proxy — harmonic voiced speech (NOT sustained tones).** A
  glottal impulse train with a per-syllable F0 contour (100–140 Hz, downward
  glide + 5 Hz vibrato) through three formant resonators (700/1220/2600 Hz), gated
  into 9 syllables (~31% voiced) with silent gaps. Speech is silent through the
  0.5 s lead-in.
  - **Why this matters (a discovered trap):** the first attempt reused
    `dfn3_make_val_wav.py`'s three non-harmonic sustained sinusoids
    (200/900/2500 Hz). Both DFN3 **and** RNNoise treat those as *tonal noise* and
    strip them 100–1000x (measured: f900/f2500 attenuated to ~1e-6), collapsing
    every engine's ground-truth SNR to ≈0 dB. The old r=0.9946 validation was a
    *similarity* metric — both tools attenuate identically, so they still
    correlate — and was blind to this. A ground-truth SNR metric requires a
    genuinely speech-like (harmonic, syllabic) reference; that fix is baked in.
- **Noise — impulsive keyboard clatter.** 24 short (5 ms) sharp broadband
  transients (~6/s, τ = 1.2 ms fast decay, mid/high emphasis), randomized
  Poisson-spaced (≥40 ms apart), over a faint stationary fan/hum floor
  (low-passed noise + 120 Hz hum, 6% level). ~0.5 s noise-only lead-in.
- **Mix SNR:** 5.0 dB over the voiced region (target). The analyzer's
  speech-frame aggregate reads ~3.4 dB because its frame mask includes
  lower-energy syllable edges.

## Metrics (`tools/dfn3/dfn3_quality.py`)

All aligned to the clean reference by xcorr lag (handles processing latency).
Metrics over speech-active frames (VAD from the clean reference):

- **dSNR (raw):** output − input of `10log10(‖clean‖²/‖x−clean‖²)`. Penalizes any
  level change, so it is unfair to an engine whose output gain differs.
- **dSI-SDR (headline):** same, but the estimate is first projected onto clean to
  remove a benign global gain. This is the robust, fair improvement figure.
- **Segmental SNR:** median per-frame SNR over speech-active frames.
- **Noise suppression:** dB RMS drop in the noise-only lead-in (proc vs mix).
- **Speech distortion:** gain-matched log-spectral distance (LSD) and normalized
  RMS error (NRMSE) in speech-active frames.

## Results

Input SNR (speech-frame aggregate): **3.42 dB**. Clip voiced-region SNR: 5.0 dB.

| Engine | dSNR raw (dB) | **dSI-SDR (dB)** | lead-in suppress (dB) | LSD (dB) | NRMSE |
|---|---:|---:|---:|---:|---:|
| passthrough (sanity) | +0.00 | **+0.00** | 0.00 | 8.87 | 0.674 |
| **dfn3-stream** (live) | +1.34 | **+8.31** | 39.57 | 12.98 | 1.204 |
| dfn3-ll (whole-utt) | +1.34 | **+8.31** | 39.51 | 12.99 | 1.204 |
| rnnoise **`--chain`** (production) | −19.59 | **+4.31** | 60.58 | 27.07 | 49.02 |
| rnnoise (bare engine) | +13.87 | **+13.81** | 62.77 | 14.97 | 0.141 |

Cross-check: `dfn3_compare.py out_dfn3ll out_dfn3stream` → **r = 1.000000** at −480
samples (−10.00 ms).

### Reading the table

- **Passthrough = 0.00 dB / 0.00 suppression** — the metric and alignment are
  sound.
- **`dfn3-stream` ≡ `dfn3-ll`** to 2 decimals on every metric, and r = 1.000000
  directly. Streaming faithfully reproduces the whole-utterance model on
  realistic content. **Solid.**
- **DFN3 is a real denoiser:** +8.31 dB SI-SDR, ~40 dB lead-in suppression, raw
  dSNR still positive (+1.34).
- **The RNNoise rows are the story.** With the `--chain` DSP wrapper (the actual
  live RNNoise configuration), RNNoise scores +4.31 dB and a wrecked NRMSE of 49
  — because the chain's VAD-gated AGC (`agcTargetDb = -15 dB`, `dsp_chain.h`)
  boosts this ~−34 dBFS clip about 6.8x, and that time-varying gate distorts the
  output. Strip the chain and the **bare RNNoise core lands +13.81 dB SI-SDR with
  NRMSE 0.14** — cleaner and stronger than DFN3.
- **Segmental SNR is negative for every denoiser** (DFN out 4.5 dB vs in 26 dB).
  Because clicks are sparse, most speech frames are already ~26 dB SNR; processing
  them can only add artifacts. The aggregate SI-SDR gain comes entirely from
  killing the few high-energy click frames. Honest nuance: DFN3 buys click removal
  at the cost of mild distortion on already-clean frames.
- **LSD caveat:** passthrough's LSD (8.87) is *lower* than DFN3's (12.98). LSD is
  log-spectral and dominated by low-energy spectral valleys, which denoisers push
  toward zero — so LSD is unreliable here and is reported only as secondary.
  SI-SDR is the headline.

## External ground truth (Task 4)

- **`deep-filter` 0.5.6 binary: NOT present.** Searched the repo and
  Downloads/Desktop/tools/bin; not found. So the standard-pipeline re-confirmation
  (expected r ≈ 0.99) and any external `_ll` reference could not be produced this
  run. The prior standard-DFN3 validation (r = 0.9946, `docs/dfn3/dfn3-validation.md`)
  stands as the external anchor for the shared DSP machinery `_ll` reuses.
- **`_ll` model reference:** even if the binary were present, `deep-filter` ships
  the STANDARD DFN3 weights; per `docs/dfn3/dfn3-validation.md` it exposes no
  model/tar flag to load the `_ll` split. So a true external `_ll` reference is
  **not obtainable from that tool** regardless. The `_ll` path remains validated
  only internally (streaming ≡ whole-utterance, r = 1.0) against our own
  whole-utterance `_ll`, which is itself unvalidated against an external `_ll`.
- **`config.ini` lookahead (confirmed by reading `models/dfn3-ll/config.ini`):**
  `[df] df_lookahead = 0` and `[deepfilternet] conv_lookahead = 0`. This confirms
  the assumed **deep-filter FIR offset leftPad = order − lookahead − 1 = 5 − 0 − 1
  = 4** (past-only, causal), matching `Dfn3Offline("models/dfn3-ll", dfLeftPad=4)`
  in `src/offline-cli/main.cpp`.

## Residual assumptions / red flags

1. **DFN3 does not demonstrably beat RNNoise on this impulsive noise.** Bare
   RNNoise wins outright; DFN3's apparent win exists only against the AGC-crippled
   `--chain` path. DFN3's core justification is unsupported by this test.
2. **The synthetic clicks may be unrepresentative.** Short, broadband, Poisson
   transients over a stationary floor with near-silent gaps are arguably *easy*
   for RNNoise: it gates the gaps hard (62 dB) and spectral-subtracts the floor.
   Real keyboard clatter over continuous speech (no gaps, non-stationary) may be
   where DFN3's advantage actually lives. **A real keyboard recording is needed to
   settle the DFN3-vs-RNNoise question.**
3. **Production paths are not apples-to-apples.** `dfn3-stream` is measured raw
   while the live RNNoise runs through the AGC/limiter chain (the CLI's dfn3
   engines ignore `--chain`). Either both should go through the chain, or the AGC
   target should be revisited so it doesn't clobber a quiet clip.
4. **The `_ll` leftPad = 4 offset remains an assumption** validated only by
   config lookahead = 0/0 and internal streaming≡whole-utterance consistency — no
   external `_ll` ground truth exists (see Task 4).

## Reproduce

```
python tools/dfn3/dfn3_make_quality_clip.py qual
build/src/offline-cli/Release/offline-cli.exe --engine dfn3-stream     qual_mix.wav out_dfn3stream.wav
build/src/offline-cli/Release/offline-cli.exe --engine dfn3-ll         qual_mix.wav out_dfn3ll.wav
build/src/offline-cli/Release/offline-cli.exe --chain --engine rnnoise qual_mix.wav out_rnnoise.wav
build/src/offline-cli/Release/offline-cli.exe --engine rnnoise         qual_mix.wav out_rnnoise_nochain.wav
build/src/offline-cli/Release/offline-cli.exe --engine passthrough     qual_mix.wav out_passthrough.wav
for e in dfn3stream dfn3ll rnnoise rnnoise_nochain passthrough; do \
  python tools/dfn3/dfn3_quality.py --clean qual_clean.wav --mix qual_mix.wav --proc out_$e.wav; done
python tools/dfn3/dfn3_compare.py out_dfn3ll.wav out_dfn3stream.wav
```
