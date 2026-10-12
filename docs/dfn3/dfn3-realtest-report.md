# DFN3 vs RNNoise — REAL keyboard-audio comparison

**Date:** 2026-08-15
**Rig:** VoiceCleaner `offline-cli.exe` (Release), both engines **BARE** (engine only, no
`--chain`, so no AGC/limiter to confound the comparison).
**Source:** three real 48 kHz mono PCM16 recordings from the user's K688 mechanical
keyboard — `typing_only.wav` (12 s), `speech_only.wav` (12 s), `speech_typing.wav` (20 s).
These have **no ground-truth separation**, so every output is scored against its own **raw
input** after xcorr-lag alignment (tool: `tools/dfn3/dfn3_realtest.py`, reusing
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

Reproduce: `python tools/dfn3/dfn3_realtest.py` (expects the input recordings + processed
outputs in the repo root).

---

## Follow-up: normalized (+20 dB) re-test

**Question.** The quiet-level result raised a fair objection: was DFN3's weak keyboard
suppression a *real* limitation, or an artifact of the quiet recordings (peaks -27 to
-42 dBFS) starving the level-dependent `_ll` gain mask? If DFN3 is just conservative at low
input, feeding it a hotter signal should make it "wake up."

**Method.** Applied a uniform **+20 dB** gain (x10.0) to each of the three raw recordings
(`tools/dfn3/dfn3_gain.py`), producing `*_n.wav`. +20 dB on -27 to -42 dBFS peaks lands at -7 to
-22 dBFS — comfortably below full scale. **Clipped samples after gain: 0 (0.0000%) on all
three files.** Re-ran both engines BARE on the normalized clips and re-scored with the same
metrics.

### A. Keyboard suppression — normalized vs quiet

| metric | DFN3 quiet | **DFN3 +20 dB** | RNNoise quiet | **RNNoise +20 dB** |
|---|---:|---:|---:|---:|
| rms_in (dBFS)          | -66.41 | -46.41 | -66.41 | -46.41 |
| rms_out (dBFS)         | -69.39 | -51.99 | -93.57 | -78.50 |
| **rms_reduction (dB)** | -2.98  | **-5.57** | -27.16 | **-32.09** |
| **p99 short-time reduction (dB)** | -2.37 | **-5.38** | -22.58 | **-27.36** |
| residual (dBFS)        | -69.39 | **-51.99** | -93.57 | **-78.50** |

### B. Voice preservation — normalized vs quiet

| metric | DFN3 quiet | **DFN3 +20 dB** | RNNoise quiet | **RNNoise +20 dB** |
|---|---:|---:|---:|---:|
| **speechband_retention** | 0.9586 | **0.9600** | 0.9183 | **0.8757** |
| **lsd (dB, voiced)**     | 1.314  | **0.982**  | 2.436  | **2.891** |
| **nrmse (voiced)**       | 0.0288 | **0.0319** | 0.1030 | **0.1685** |

### C. Real use case (speech+typing) — normalized vs quiet

| metric | DFN3 quiet | **DFN3 +20 dB** | RNNoise quiet | **RNNoise +20 dB** |
|---|---:|---:|---:|---:|
| **HF (>3 kHz) transient reduction (dB)** | -1.68 | **-1.97** | -2.02 | **-2.11** |

### Updated verdict — did DFN3 wake up? No, not enough. RNNoise still wins by ~26 dB.

**The decisive number:** at normal level, DFN3-stream removes **-5.57 dB rms / -5.38 dB p99**
of keyboard noise; RNNoise removes **-32.09 dB rms / -27.36 dB p99**. That is a **~26.5 dB
gap — actually slightly *wider* than the 24 dB gap at quiet level.**

- **DFN3 did partially wake up.** Its keyboard suppression roughly doubled with the hotter
  input (2.98 → 5.57 dB rms; 2.37 → 5.38 dB p99), so the level-dependence hypothesis is
  **partially confirmed** — the `_ll` mask *is* more conservative at low input. Level matters.
- **But it does not change the verdict, and it does not close the gap.** RNNoise *also*
  scaled up with more level (27.16 → 32.09 dB), so DFN3 never catches up. 5.6 dB of removal
  leaves a -52 dBFS keyboard residual you can plainly hear; RNNoise's -78 dBFS is effectively
  gone. DFN3's earlier weakness was **not primarily a gain-staging artifact** — it is a real,
  structural limitation on impulsive mechanical-keyboard noise that persists at proper levels.
- **Voice preservation gap actually widened in DFN3's favor.** At normal level RNNoise gets
  more aggressive on the voice too (retention 0.9183 → 0.8757, LSD 2.44 → 2.89), while DFN3
  holds steady (0.960, LSD 0.98). So the tradeoff is unchanged in shape, just sharper: DFN3 =
  gentle on voice + weak on noise; RNNoise = strong on noise + a bit more voice coloration.
- **Real-use HF click removal stays a near-tie** (DFN3 -1.97 vs RNNoise -2.11 dB), both up
  slightly with level, RNNoise still marginally ahead.

**Default recommendation: unchanged — RNNoise is the default; DFN3 stays opt-in.** The core
product objective is killing mechanical-keyboard noise, and even correctly gain-staged, DFN3
suppresses ~26 dB *less* of it than RNNoise at roughly one-third the CPU. The "it was just
quiet input" objection is answered: no, DFN3 is genuinely weaker on this noise class.

**Caveat worth carrying forward:** DFN3 *is* level-sensitive (suppression nearly doubled
with +20 dB), so the full chain still needs proper gain-staging / a level meter, and DFN3
should be re-tested inside the full chain where AGC sets a consistent hot level. That would
let DFN3 perform at its best — but its best here is still ~5.6 dB, far short of RNNoise.

Reproduce: `python tools/dfn3/dfn3_gain.py <in.wav> <out.wav> 20` on each raw clip, re-run the
engines on the `_n.wav` inputs, then `python tools/dfn3/dfn3_realtest.py _n`.

### Normalized A/B clips left for the user (repo root, not committed)

| clip | DFN3-stream | RNNoise |
|---|---|---|
| **speech + typing, +20 dB** | `abtest_dfn3_n.wav` | `abtest_rnnoise_n.wav` |

(Also present from the run: `typing_*_n.wav`, `speech_*_n.wav`, `*_pass_n.wav`, and the
gained inputs `*_n.wav` — all untracked, deletable.)
