# Can DeepFilterNet3 compete with RNNoise on REAL keyboard noise?

**Branch:** `investigate/dfn3-competitive`
**Inputs:** +20 dB-normalized real K688 recordings in repo root
(`typing_only_n.wav` 12 s clicks, `speech_only_n.wav` 12 s speech,
`speech_typing_n.wav` 20 s mixed). 48 kHz mono PCM16.
**Metrics tool:** `python tools/dfn3_realtest.py _n` (each engine output aligned to
its raw input by xcorr lag, then scored).
**Engines:** `build/src/offline-cli/Release/offline-cli.exe --engine <...> [--aggression <0..1>]`.

**Prior finding being re-examined:** the shipped low-latency `_ll` streaming model
suppresses keyboard clicks only ~5.6 dB, vs RNNoise's ~32 dB.

## TL;DR verdict

**Yes — DFN3 can be made competitive on real keyboard noise, but only via the
STANDARD model, not via aggression.**

- The whole failure was the **`_ll` low-latency (0-lookahead) choice.** The
  **standard** DFN3 model (2-frame conv+df lookahead) suppresses keyboard clicks
  **−30.3 dB RMS** — right in RNNoise's ballpark (−32.1 dB) — while preserving
  voice **better** than RNNoise. `_ll` gets only −5.6 dB. Hypothesis 1 confirmed,
  decisively.
- **Aggression (Hypothesis 2) is a dead end.** Pushing the ERB gain mask harder
  changes keyboard suppression by <0.6 dB on the standard model and by ~0 dB on
  `_ll`, because the ERB mask only governs bins ≥ 4800 Hz; the keyboard energy
  DFN3 actually removes lives in the **deep-filter** band (< 4800 Hz), which the
  aggression hook never touches. It only costs voice quality at the top of the
  sweep. Not run beyond the sweep — it cannot close the gap.
- **Cost of the standard-model path:** latency 10 → ~30 ms, ~3× CPU (three ONNX
  sessions), and — the real blocker — the standard model must be **re-streamed**;
  standard-model streaming surgery previously FAILED Gate A and would need the
  windowed-conv approach re-applied. The −30 dB number is **whole-utterance
  offline**; a streaming standard model is not yet proven to preserve it.
- RNNoise still **beats DFN3 on peak clicks** (p99 −27.4 dB vs standard's
  −20.8 dB) and is 10 ms / cheap / already shipping. DFN3-standard's only edge is
  **cleaner voice.**

**Recommendation:** For a keyboard-noise-primary use case, RNNoise is the right
default. DFN3-standard is a viable *opt-in premium* mode **if** the streaming
re-surgery is done — justified by voice quality, not by out-suppressing RNNoise.
The shipped `_ll` RT model cannot be rescued for this noise (aggression included).

---

## Hypothesis 1 — standard model (2-frame lookahead) vs `_ll` (0 lookahead)

Standard DFN3 (`models/dfn3`, `--engine dfn3`, whole-utterance, externally
validated r=0.9946) has 2 frames of conv+df lookahead — future context that lets
it bracket a transient click. `_ll` has 0. Run on `typing_only_n` (suppression)
and `speech_only_n` (voice cost).

| engine | approx latency | **kbd RMS red (dB)** | **kbd p99 red (dB)** | residual dBFS | crest_out | **voice retention** | **voice LSD (dB)** | **voice nrmse** |
|---|---|---|---|---|---|---|---|---|
| **rnnoise** | ~10 ms | **−32.09** | **−27.36** | −78.50 | 96.5 | 0.8757 | 2.891 | 0.1685 |
| **dfn3-standard** (2-frame LA) | ~30 ms | **−30.30** | **−20.79** | −76.72 | 18.6 | **0.8927** | **1.815** | **0.0730** |
| dfn3-ll (whole-utterance golden) | ~10 ms | −5.57 | −5.38 | −51.99 | 9.6 | 0.9600 | 0.982 | 0.0319 |
| dfn3-stream (_ll, shipped RT path) | ~10 ms | −5.57 | −5.38 | −51.99 | 9.6 | 0.9600 | 0.982 | 0.0319 |
| passthrough | — | 0.00 | 0.00 | −46.42 | 16.2 | 1.0000 | 0.000 | 0.0000 |

(more-negative RMS/p99 = more suppression; retention ~1.0 = voice preserved;
lower LSD/nrmse = less voice damage. `_ll` whole-utterance == `_ll` streaming to
2 decimals, so the shipped RT path is a faithful stand-in for the `_ll` model.)

**Reading it:**

- **Standard DFN3 lands in RNNoise's keyboard-suppression ballpark** on average
  level: −30.3 dB vs −32.1 dB (a 1.8 dB gap) — and it is **~25 dB better than
  `_ll`.** The prior −5.6 dB result was entirely an artifact of the low-latency
  0-lookahead export. H1 confirmed.
- **On voice, standard DFN3 beats RNNoise:** higher speech-band retention
  (0.893 vs 0.876), less than two-thirds the log-spectral distortion
  (1.82 vs 2.89 dB), less than half the spectral error (nrmse 0.073 vs 0.169).
  This is DFN3's genuine edge.
- **RNNoise still wins on peak clicks (p99):** −27.4 dB vs standard's −20.8 dB, a
  6.6 dB gap. RNNoise drives the average residual lower (−78.5 vs −76.7 dBFS) but
  leaves *spiky* residual click transients (crest factor balloons to 96 — sharp
  spikes over near-silence); DFN3-standard leaves a smoother, flatter residual
  (crest 18.6) but a couple dB louder on average and on peaks. Net: RNNoise
  removes the loudest clicks harder; DFN3-standard removes them more cleanly.

**H1 conclusion:** the standard model closes almost all of the keyboard-suppression
gap (within ~2 dB RMS) and is *superior* on voice. The remaining gap is peak-click
suppression (p99). This is not a fundamental DFN3 weakness on keyboard noise — it
is the `_ll` latency choice. Fixable in principle by shipping a **streaming
standard** model at ~30 ms latency (see Cost, below).

---

## Hypothesis 2 — aggression / attenuation (tested because H1 leaves a p99 gap)

DFN3's ERB gain mask is pushed toward more attenuation via the (previously unused)
`src/denoise-core/aggression.{h,cpp}`: `g' = max(gMin, g^gamma)` with
`gamma = aggression_gamma(slider)` (slider 0→γ0.4, 0.5→γ1.0, 1.0→γ2.5), `gMin=0.1`
(−20 dB floor). Wired offline-only into `dfn3_offline.cpp` before `apply_erb_mask`,
exposed as `offline-cli --aggression <0..1>`; **default 0 = disabled = byte-for-byte
original behavior** (the RT streaming path `dfn3_streaming.cpp` is untouched).

Sweep on the best H1 variant (standard dfn3), `typing_only_n` + `speech_only_n`:

| aggression (→ gamma) | **kbd RMS red (dB)** | **kbd p99 red (dB)** | **voice retention** | **voice LSD (dB)** |
|---|---|---|---|---|
| 0.00 (disabled, raw mask) | **−30.30** | −20.79 | 0.8927 | 1.815 |
| 0.25 (γ 0.7) | −29.73 | −20.68 | 0.8927 | 1.666 |
| 0.50 (γ 1.0) | −29.75 | −20.70 | 0.8927 | 1.670 |
| 0.75 (γ 1.6) | −29.76 | −20.71 | 0.8927 | 1.675 |
| 1.00 (γ 2.5) | −29.88 | −20.82 | 0.8926 | 2.313 |

Cross-check on `_ll` (`--engine dfn3-ll --aggression 1.0`, `typing_only_n`):
**−5.59 dB** RMS / −5.38 dB p99 — identical to the −5.57 baseline. Inert.

**Reading it:**

- **Aggression does essentially nothing to keyboard suppression.** The entire
  sweep sits within 0.6 dB of baseline on standard, and moves `_ll` by 0.02 dB.
  It never approaches RNNoise's p99.
- It even goes slightly *backwards* vs the raw-mask baseline: the `gMin=0.1`
  floor caps ERB-band attenuation at −20 dB, so flooring undoes a hair of the
  native suppression regardless of gamma.
- **Root cause (architectural):** the ERB gain mask governs only bins ≥ 96 —
  i.e. ≥ 4800 Hz (bin width 50 Hz at nfft 960). The keyboard-click energy DFN3
  removes is dominated by the **deep-filter** stage on bins < 96 (< 4800 Hz),
  which `apply_aggression` never reaches. Shaping the ERB mask harder only
  sharpens > 4.8 kHz, which for broadband clicks is a rounding error — and it
  starts damaging voice (LSD 1.82 → 2.31 at the top).

**H2 conclusion:** aggression, as specified (ERB-mask shaping), **cannot** close the
gap on keyboard noise on either model. To matter it would have to shape the
deep-filter (< 4.8 kHz) band, which the current mask hook does not expose. Not a
path to competitiveness. (The `--aggression` flag is left in place, default-off,
as a documented, tested no-op-by-default offline knob; it did no harm and future
> 4.8 kHz-hiss experiments may want it.)

---

## Cost of the only working path (standard model streamed at ~30 ms)

| axis | `_ll` (shipped RT) | standard (what H1 needs) |
|---|---|---|
| keyboard RMS suppression | −5.6 dB | **−30.3 dB** (≈ RNNoise −32.1) |
| keyboard p99 (peak clicks) | −5.4 dB | −20.8 dB (RNNoise −27.4 — RNNoise still wins) |
| voice quality vs RNNoise | best | **better than RNNoise** |
| latency | ~10 ms | ~30 ms (2 frames × 10 ms lookahead + STFT) |
| CPU | ~3× RNNoise | ~3× RNNoise (same 3 ONNX sessions) |
| **streaming readiness** | ships today | **NOT streaming** — standard-model streaming surgery previously **FAILED Gate A**; needs the windowed-conv approach re-applied and re-gated. The −30 dB is whole-utterance offline only. |

**Bottom line:** DFN3 is not fundamentally weak on keyboard noise — the standard
model proves it reaches RNNoise territory and keeps voice cleaner. But turning that
offline result into a shippable real-time engine means (a) accepting 30 ms latency
and ~3× CPU, (b) redoing the standard-model streaming surgery that already failed
once, and (c) still conceding peak-click suppression to RNNoise. RNNoise —
−32 dB / −27 dB p99 / 10 ms / cheap / already shipping — remains the right default
for keyboard noise. **DFN3-standard is worth pursuing only as an opt-in premium
mode whose selling point is voice quality, and only if the streaming re-surgery is
funded.** Aggression is not part of any recommended path.

## Reproduce

```
# build (offline-only aggression change; RT path untouched)
export PATH="/c/Espressif/tools/cmake/4.0.3/bin:$PATH"
cmake --build build --config Release
ctest --test-dir build -C Debug        # 74/74 pass

EXE=build/src/offline-cli/Release/offline-cli.exe
# H1
for clip in typing_only_n speech_only_n; do
  "$EXE" --engine dfn3        "$clip.wav" "${clip/_only_n/}_dfn3std_n.wav"
  "$EXE" --engine dfn3-ll     "$clip.wav" "${clip/_only_n/}_dfn3ll_n.wav"
  "$EXE" --engine dfn3-stream "$clip.wav" "${clip/_only_n/}_dfn3stream_n.wav"
  "$EXE" --engine rnnoise     "$clip.wav" "${clip/_only_n/}_rnnoise_n.wav"
done
# H2 aggression sweep on standard
for a in 025 050 075 100; do af="0.${a}"; [ "$a" = 100 ] && af=1.0
  "$EXE" --engine dfn3 --aggression $af typing_only_n.wav typing_dfn3a${a}_n.wav
  "$EXE" --engine dfn3 --aggression $af speech_only_n.wav speech_dfn3a${a}_n.wav
done
python tools/dfn3_realtest.py _n
```
