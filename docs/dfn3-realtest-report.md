# DFN3 vs RNNoise — REAL keyboard-audio comparison

**Date:** 2026-08-15
**Rig:** VoiceCleaner `offline-cli.exe` (Release), both engines **BARE** (engine only, no
`--chain`, so no AGC/limiter to confound the comparison).
**Source:** three real 48 kHz mono PCM16 recordings from the user's K688 mechanical
keyboard — `typing_only.wav` (12 s), `speech_only.wav` (12 s), `speech_typing.wav` (20 s).
These have **no ground-truth separation**, so every output is scored against its own **raw
input** after xcorr-lag alignment (tool: `tools/dfn3_realtest.py`, reusing
`dfn3_compare.py`'s `read_mono` + `best_lag`).

**Engines:** `dfn3-stream` (the live low-latency `_ll` DFN3) and `rnnoise`.
`passthrough` is the 0-dB sanity column.

## Alignment sanity (read this first)

The `passthrough` column lands at exactly **rms_reduction 0.00 dB, speechband_retention
1.0000, lsd 0.000, nrmse 0.0000**. That proves the xcorr alignment introduces **zero**
distortion of its own — every non-zero number below is real engine behaviour, not a
line-up artifact. Measured engine latency (from the alignment lag) is **10 ms for
dfn3-stream, 20 ms for rnnoise** — both sane for real-time.

## Recording health check

| file | peak | RMS | DC | clipping |
|---|---|---|---|---|
| typing_only  | -42.3 dBFS | -66.4 dBFS | -2e-5 | 0.000% |
| speech_only  | -27.3 dBFS | -50.3 dBFS | -2e-5 | 0.000% |
| speech_typing| -30.8 dBFS | -57.1 dBFS | -2e-5 | 0.000% |

No clipping, negligible DC. **But the recordings are quiet** (mic gain low: peaks
-27 to -42 dBFS). Keep that in mind — it appears to matter a lot for DFN3 (see verdict).

---

## A. Keyboard suppression — `typing_only.wav` (ideal output = silence)

| metric | dfn3-stream | rnnoise | passthrough |
|---|---:|---:|---:|
| rms_in (dBFS)          | -66.41 | -66.41 | -66.42 |
| rms_out (dBFS)         | -69.39 | **-93.57** | -66.42 |
| **rms_reduction (dB)** | -2.98  | **-27.16** | 0.00 |
| crest_in               | 16.16  | 16.15  | 16.16 |
| crest_out              | 12.06  | 103.35 | 16.16 |
| crest_reduction        | +4.10  | -87.20 | 0.00 |
| **p99 short-time reduction (dB)** | -2.37 | **-22.58** | 0.00 |
| residual (dBFS)        | -69.39 | **-93.57** | -66.42 |

**RNNoise crushes the keyboard noise; DFN3 barely touches it.** RNNoise pulls the overall
level down **27 dB** and the 99th-percentile short-time level down **22.6 dB**, leaving a
-93 dBFS floor (effectively silent). DFN3-stream removes only **3 dB** overall / 2.4 dB at
the 99th percentile, leaving a -69 dBFS residual.

**About the crest-factor row — it is a trap here, don't be fooled.** DFN3 "wins" crest
(16→12) and RNNoise "loses" badly (16→103), which naively reads as "DFN3 flattens clicks
better." It doesn't. RNNoise's crest explodes only because it crushed the *floor* far more
than the *peaks*, so the ratio blows up. In **absolute** terms RNNoise still wins the peaks
too: residual click peaks land at ~-52 dBFS for RNNoise vs ~-47 dBFS for DFN3 (≈5 dB lower),
on top of a floor that is 24 dB quieter. Absolute residual level (p99 and floor) is the
honest measure, and RNNoise wins it decisively.

**A: RNNoise, by a landslide (~24 dB more suppression).**

---

## B. Voice preservation — `speech_only.wav` (ideal = untouched)

| metric | dfn3-stream | rnnoise | passthrough |
|---|---:|---:|---:|
| **speechband_retention (100–5000 Hz)** | **0.9586** | 0.9183 | 1.0000 |
| **lsd (dB, voiced frames)**            | **1.314**  | 2.436  | 0.000 |
| **nrmse (voiced frames)**              | **0.0288** | 0.1030 | 0.0000 |
| rms_out (dBFS)                         | -50.52 | -50.75 | -50.29 |

**DFN3 preserves the voice slightly better** — higher speech-band retention (95.9% vs
91.8%), ~1.1 dB lower log-spectral distance, and ~3.5× lower normalized spectral error.
That said, **both are well within acceptable** — RNNoise keeps 92% of speech-band energy
and only 2.4 dB LSD; the voice is not damaged by either engine at these levels.

**B: DFN3, narrowly. Real but not dramatic — RNNoise's voice is still fine.**

---

## C. Real use case — `speech_typing.wav` (clicks under live speech)

| metric | dfn3-stream | rnnoise | passthrough |
|---|---:|---:|---:|
| **HF (>3 kHz) transient reduction on speech-active frames (dB)** | -1.68 | **-2.02** | 0.00 |
| rms_out (dBFS) | -58.06 | -59.12 | -57.06 |

Near tie, with RNNoise slightly ahead on removing >3 kHz click energy while speech is
present (-2.0 vs -1.7 dB) and slightly more overall reduction. Both are modest here because
the metric only looks at loud speech-active frames (where the voice masks a lot of the
click energy anyway). **Primary judgment on this clip is the user's ears** — the numbers
just say RNNoise removes marginally more click HF without measurably hurting the voice.

**C: RNNoise, marginally.**

---

## VERDICT

**1. Which suppresses the real keyboard clicks more?**
**RNNoise, decisively.** On pure typing it removes **27 dB** overall and **22.6 dB** at the
99th percentile, versus DFN3-stream's **~3 dB / 2.4 dB**. Its residual is a near-silent
-93 dBFS floor with click peaks ~5 dB below DFN3's. This is not close.

**2. Which preserves the voice better?**
**DFN3, narrowly** (95.9% vs 91.8% speech-band retention; 1.31 vs 2.44 dB LSD). But
RNNoise's voice is still perfectly usable — the difference is audible-detail, not damage.

**3. Net: is DFN3-stream worth ~3× CPU + the whole streaming effort as the DEFAULT?**
**No. RNNoise should be the DEFAULT; DFN3 should be an optional/offline engine.**
The entire point of this product is killing mechanical-keyboard noise, and on the user's
REAL recordings DFN3-stream barely does it (3 dB) while RNNoise nearly erases it (27 dB) —
at roughly one-third the CPU. DFN3's only win is a slightly gentler touch on the voice, and
that edge is small and does not offset losing the core objective by ~24 dB. Shipping
DFN3-stream as default would cost 3× the CPU to denoise **less**. Keep DFN3 available as an
opt-in / offline "max voice fidelity" mode for people who care more about voice timbre than
about noise removal and have CPU to burn, but the default that serves the stated use case
is RNNoise.

### Red flags / caveats
- **Low record levels.** All three clips are quiet (peaks -27 to -42 dBFS, RMS -50 to
  -66 dBFS). This very likely explains DFN3's timidity: the live `_ll` DFN3 appears to apply
  a **conservative gain mask at low input levels**, treating the quiet clicks as maybe-speech
  and passing them. If the user tends to record this quiet, DFN3-stream is effectively
  near-passthrough on their noise. Worth a follow-up: re-run at a hotter input (e.g. +20 dB
  normalize) to see whether DFN3 wakes up — but on the audio as actually captured, the
  numbers above stand.
- **RNNoise is essentially a hard gate on pure typing** (-93 dBFS out). That is exactly what
  you want for typing_only, but it is why its crest factor looks pathological — noted and
  explained in section A so nobody mis-reads that row.
- **No clipping, negligible DC** in any recording. Recordings themselves are clean.
- **Crest-factor caution:** do not use crest_reduction alone to rank keyboard suppression;
  it rewards leaving a noisy floor. Use absolute p99 / residual dBFS (this report does).

---

## Files left in the repo root for the user to A/B (keep these; not committed)

| clip | DFN3-stream | RNNoise |
|---|---|---|
| typing only (12 s)         | `typing_dfn3.wav`  | `typing_rnnoise.wav` |
| speech only (12 s)         | `speech_dfn3.wav`  | `speech_rnnoise.wav` |
| **speech + typing (20 s)** — the real A/B | `abtest_dfn3.wav` | `abtest_rnnoise.wav` |

(`*_pass.wav` passthrough copies were also generated as the 0-dB reference but can be
deleted.)

Reproduce: `python tools/dfn3_realtest.py` (expects the input recordings + processed
outputs in the repo root).
