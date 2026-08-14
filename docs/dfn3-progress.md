# DeepFilterNet3 Integration — Reference & Progress

_Source: `github.com/Rikorose/DeepFilterNet` (MIT OR Apache-2.0). Scouted + foundation built 2026-08-13._

DFN3 is the intended **default** engine (best for impulsive/keyboard noise — RNNoise's weak spot,
DFN3's strength; blueprint D9). The DSP that surrounds the neural graphs is implemented by us; the
ONNX graphs are only the encoder/decoders.

## Signal parameters (confirmed from `models/dfn3/config.ini` `[df]` + libDF `src/lib.rs`)

| Param | Value |
|---|---|
| Sample rate | 48000 Hz |
| FFT size | 960 |
| Hop | 480 (50% overlap) |
| Window | **Vorbis**: `w[n] = sin( (π/2)·sin²( π·(n+0.5)/N ) )` (libDF lib.rs:126–132) |
| Freq bins | 481 (960/2+1) |
| ERB bands (`nb_erb`) | 32 (`min_nb_erb_freqs=2`) |
| DF bins (`nb_df`) | 96 |
| DF order | 5 |
| Lookahead | 2 df + 2 conv frames |
| Normalization | exponential mean, `norm_tau = 1` |

## ONNX models (`models/dfn3/`, split form — the form the official Rust runtime uses)

Vendored; ONNX Runtime **1.28.0** (FetchContent, pinned SHA256
`abef733dacbe2f571547a7150b479b5cb9cc0df22f96c24983a42cadb1b4f8bc`). Use `intra_op_num_threads=1`.

**Exact I/O (from `dfn3-probe`, dynamic dims shown as T):**

- **enc**
  - in:  `feat_erb [1,1,T,32]`, `feat_spec [1,2,T,96]` (2 = re/im)
  - out: `e0 [1,64,T,32]`, `e1 [1,64,T,16]`, `e2 [1,64,T,8]`, `e3 [1,64,T,8]`,
         `emb [1,T,512]` (bidir GRU → 512), `c0 [1,64,T,96]`, `lsnr [1,T,1]`
- **erb_dec**(`emb, e3, e2, e1, e0`) → `m [1,1,T,32]`  (ERB gain mask)
- **df_dec**(`emb, c0`) → `coefs [1,T,96,10]` (10 = df_order·2 re/im), plus `[1,T,1]` (node "235")

## Pipeline (per 480-sample frame)

```
input → STFT(Vorbis,960/480) → spectrum(481)
      → feat_erb = normalize(log(ERB_compress(|spec|²)))        [1,1,T,32]
      → feat_spec = normalize(spec[0:96] as re/im)              [1,2,T,96]
      → enc → (erb_dec → m), (df_dec → coefs)
      → spec' = apply_erb_mask(spec, m)         (expand m over ERB bands, multiply)
      → spec'' = apply_deep_filter(spec', coefs) (order-5 complex temporal filter over first 96 bins)
      → ISTFT overlap-add → output
```
Recurrent GRU state + the 2+2-frame lookahead must be carried across frames for streaming.

## Done
- `third_party/kissfft/` (real FFT), `src/denoise-core/stft.{h,cpp}` (Vorbis, perfect reconstruction).
- `src/denoise-core/erb.{h,cpp}` (32-band, exact libDF widths, sum 481).
- ONNX Runtime + `models/dfn3/` + `src/dfn3-probe/` (loads all 3, dummy inference OK).

## Remaining
1. Feature extraction (`feat_erb`, `feat_spec` + `norm_tau` exponential-mean normalization) — match libDF.
2. Inference orchestration (3 sessions/frame, GRU state, lookahead).
3. Post-DSP: apply mask `m`, apply deep filter `coefs`, ISTFT.
4. `DeepFilterFilter : INoiseFilter` wrapper; wire into offline-cli + live relay.
5. Validation (sample-for-sample): golden `.npz` from DeepFilterNet `df/scripts/export.py`, and/or the
   `deep-filter` CLI (Windows binaries on the releases page) end-to-end reference. **Decision open.**

## Reference commands
- Golden vectors: `export.py` writes `erb_dec_input.npz` / `erb_dec_output.npz` / `df_dec_*.npz`.
- End-to-end reference: `deep-filter noisy.wav` (add `-D` for delay compensation, `--pf` post-filter).
