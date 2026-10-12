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

1. `docs/archive/dfn3-tools/dfn3_make_val_wav.py` → `val_in.wav`: 48 kHz mono PCM16, 3.0 s,
   deterministic (seed 20260814). Three AM-modulated formant tones
   (200/900/2500 Hz, 3 Hz envelope) + white noise at ~7 dB SNR, with a 0.5 s
   noise-only lead-in.
2. Reference: `deep-filter.exe -o refout val_in.wav` → `refout/val_in.wav`.
3. Ours: `offline-cli --engine dfn3 val_in.wav val_ours.wav`.
4. `docs/archive/dfn3-tools/dfn3_compare.py`: cross-correlate to find the integer sample lag,
   align, trim to common length, then report Pearson r, per-region RMS, and a
   coarse magnitude-spectrum MSE.

Both outputs are 144000 samples (identical length to the input).

## Results — VALIDATED (after the STFT-normalization fix, commit `30400ff`)

Alignment lag ≈ -718 samples (~15 ms; the model/STFT algorithmic delay).

| Metric | Before fix | **After fix** |
|---|---|---|
| **Pearson r at best lag (global)** | 0.671 | **0.9946** |
| Noise-only lead-in RMS ratio (ours/ref) | 40.3× | **1.06×** (within ~0.5 dB) |
| mean `lsnr` on noise frames | +13.1 (wrong: read as speech) | **-14.9** (correctly low) |

**Verdict: our offline DFN3 now sample-matches libDF** (r = 0.9946, > the 0.95
"excellent match" bar), with noise suppression within ~0.5 dB of the reference.

## Root cause (found & fixed)

The forward STFT was missing libDF's analysis normalization
`wnorm = 2·hop/fft_size² ≈ 0.00104` (`libDF/src/lib.rs:133`, applied at
`:390-393`). Our `Stft::analyze` emitted the raw `kiss_fftr` output — ~960×
too large — which offset `feat_erb` by ~+59 dB and `feat_spec` magnitudes by
~960×. Because the normalization states initialize to libDF-scale values
(`-60..-90` dB, `0.001..0.0001`), the mismatch corrupted the model input
worst during the warmup window → the under-suppressed lead-in. The output
*level* was accidentally correct because the synthesis `1/N` cancelled the
missing forward scale, which is exactly why speech levels looked close while
the correlation stayed low.

**Fix:** apply `wnorm` in `Stft::analyze` and drop the compensating `1/N` in
`Stft::synthesize`. Round-trip gain `wnorm·fftSize = 2·hop/fftSize = 1` at 50%
overlap, so perfect reconstruction is preserved (`Stft.PerfectReconstruction`
still passes). This single fix took r from 0.671 to 0.9946.

## Remaining

Offline DFN3 is validated. The live/real-time path is a separate effort: the
ONNX export has no recurrent-state I/O, so frame-by-frame streaming needs a
state-carrying solution (a state-exposing re-export, or a bounded sliding
window). Until then, RNNoise is the live engine and DFN3 is offline-only.

## Reproduce

```
python docs/archive/dfn3-tools/dfn3_make_val_wav.py val_in.wav
deep-filter.exe -o refout val_in.wav            # deep_filter 0.5.6, from releases
offline-cli --engine dfn3 val_in.wav val_ours.wav
python docs/archive/dfn3-tools/dfn3_compare.py refout/val_in.wav val_ours.wav
```
