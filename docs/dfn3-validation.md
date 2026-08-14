# DeepFilterNet3 Offline Validation vs `deep-filter` Reference

_Run 2026-08-14 on Windows 11 (x86-64)._

End-to-end validation of VoiceCleaner's offline DFN3 engine
(`offline-cli --engine dfn3`, model `models/dfn3/`) against the **official**
DeepFilterNet runtime.

## Reference obtained

- **Tool:** `deep-filter` prebuilt Windows binary,
  `deep-filter-0.5.6-x86_64-pc-windows-msvc.exe` from
  `github.com/Rikorose/DeepFilterNet/releases/tag/v0.5.6`.
- **Version:** `deep_filter 0.5.6` (DeepFilterNet3 weights built in — the same
  DFN3 model family as our vendored `models/dfn3/` split ONNX export).
- **Invocation:** `deep-filter.exe -o refout val_in.wav` (defaults: attenuation
  limit 100 dB = full reduction, post-filter off). `-D` delay-compensation was
  also tried; it only shifts the output and does not change the correlation.

## Method

1. `tools/dfn3_make_val_wav.py` → `val_in.wav`: 48 kHz mono PCM16, 3.0 s,
   deterministic (seed 20260814). Three AM-modulated formant tones
   (200/900/2500 Hz, 3 Hz envelope) + white noise at ~7 dB SNR, with a 0.5 s
   noise-only lead-in.
2. Reference: `deep-filter.exe -o refout val_in.wav` → `refout/val_in.wav`.
3. Ours: `offline-cli --engine dfn3 val_in.wav val_ours.wav`.
4. `tools/dfn3_compare.py`: cross-correlate to find the integer sample lag,
   align, trim to common length, then report Pearson r, per-region RMS, and a
   coarse magnitude-spectrum MSE.

Both outputs are 144000 samples (identical length to the input).

## Results (real measured numbers)

Alignment lag ≈ -718 samples (~15 ms; the model/STFT algorithmic delay).
Correlation is invariant to the lag search and to `-D`.

| Metric | Value |
|---|---|
| **Pearson r at best lag (global)** | **0.671** |
| Pearson r, speech region only | 0.693 |
| Pearson r, noise-only lead-in | -0.004 (uncorrelated) |
| Magnitude-spectrum MSE | 2.68e-02 |

Per-region suppression **relative to the noisy input**:

| Region | Input | Reference | Ours |
|---|---|---|---|
| Noise-only lead-in (0–0.5 s) | -21.7 dBFS | **-61.5 dBFS (-39.8 dB)** | **-29.3 dBFS (-7.6 dB)** |
| Speech (0.5 s–end) | -13.9 dBFS | -26.1 dBFS (-12.2 dB) | -24.4 dBFS (-10.5 dB) |

RMS ratio (ours/ref): noise-only ≈ **40x**, speech ≈ 1.21x.

## Verdict

**Our offline DFN3 does NOT yet sample-match libDF.** Global waveform
correlation is r ≈ 0.67 — well below the >0.95 "excellent libDF match" bar and
below the 0.8 "close with minor DSP differences" bar; this is a real
discrepancy to investigate.

Two distinct symptoms:

1. **Noise-only segments are barely suppressed.** libDF drives the pure-noise
   lead-in ~40 dB down; ours only ~7.6 dB. In that region the two outputs are
   uncorrelated (r ≈ 0). This is the dominant error and is worse than the
   ~-19.6 dB noise-only figure cited earlier from a different clip — on this
   synthetic signal our gate/mask is not closing on noise the way libDF's does.
2. **Speech-region waveforms differ.** Even where levels are close (speech RMS
   within ~1.7 dB of the reference), the waveform correlation is only 0.69,
   indicating structural DSP differences rather than a mere scale/delay offset.

The reference handles this exact input cleanly (-39.8 dB on the same lead-in),
so the input is not the problem — the gap is in our pipeline. Likely suspects
(unverified, for follow-up): ERB-gain mask application / gain floor, the
`norm_tau` exponential-mean normalization warmup, deep-filter coefficient
application, or ISTFT overlap-add normalization.

## Recommended next step

Move from end-to-end correlation to **stage-level golden vectors** to localize
the divergence: export libDF intermediates with DeepFilterNet's
`df/scripts/export.py` (`erb_dec_input/output.npz`, `df_dec_*.npz`) and compare
our `feat_erb` / `feat_spec`, ERB mask `m`, and DF `coefs` tensor-by-tensor
against them. That will pin the discrepancy to a specific stage (feature
extraction vs. mask vs. deep filter vs. ISTFT) instead of the whole chain.

## Reproduce

```
python tools/dfn3_make_val_wav.py val_in.wav
deep-filter.exe -o refout val_in.wav            # deep_filter 0.5.6, from releases
offline-cli --engine dfn3 val_in.wav val_ours.wav
python tools/dfn3_compare.py refout/val_in.wav val_ours.wav
```
