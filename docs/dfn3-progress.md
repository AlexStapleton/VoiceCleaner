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

_An earlier GRU-only surgery on the standard models failed Gate A — see spec §11; the current
contract is the `_ll` split below._

---

## Streaming models (`_ll`) — the shipped design (2026-08-14)

> The section above (standard-model, GRU-state-only surgery) **failed Gate A**: exposing GRU
> state is necessary but not sufficient because the encoder *and* the `_ll` decoders also contain
> **temporal convolutions** whose per-frame receptive field a `T=1` call feeds with zero padding.
> The design below supersedes it. Target model is `_ll` (512-wide, `df_num_layers=3`,
> `conv_lookahead=0`/`df_lookahead=0`), vendored under `models/dfn3-ll/`.

Two kinds of recurrent state are handled by their nature:

1. **GRUs → stateful.** `initial_h` (input 5, a baked constant) becomes a graph input; the next
   hidden state becomes a graph output. Multi-layer decoder state is stacked `[num_layers,1,512]`,
   `Slice`d per layer and `Concat`ed back.
2. **Temporal convs (time-kernel > 1) → host-side windowing.** The host feeds the graph a rolling
   window of the last `W` input frames each hop; the graph `Slice`s its data outputs to the **last
   time frame** only. A window covering the compounded causal receptive field reproduces the
   whole-utterance result **exactly** (convs are cheap, so recomputing the window is fine).

You cannot window a graph whose GRU state is carried by a plain `h_out→h_in` (windowing
re-processes seen frames and would advance the GRU by `W` frames/hop). Two resolutions:

- **Encoder split at the GRU boundary** — `enc.onnx` → `enc_conv_streaming.onnx` (pre-GRU convs,
  stateless, windowed, slice-last) + `enc_gru_streaming.onnx` (GRU + `lsnr`, stateful, one
  frame/hop; its `emb` input is already the single sliced frame).
- **Decoders windowed *and* stateful in one graph** — the whole decoder (GRU included) is fed a
  rolling window; the GRU gets the **pre-window** hidden state as `*_h0` and emits, as `*_h1`, the
  hidden state **after the FIRST window frame** (GRU `Y[0]`), so the host advances the carried
  state by exactly **one** frame/hop. Data outputs are sliced to the last frame. Exact for any
  `W ≥` the post-GRU conv receptive field; degrades to a plain stateful GRU at `W=1`.

Generated by `tools/dfn3_make_streaming.py`; validated by Gate A (`tools/dfn3_stream_check.py`).
All GRU hidden size = **512**.

### Exact I/O (names + shapes; `S` = streamed time dim)

- **`enc_conv_streaming.onnx`** — *windowed, stateless.* **W = 6.**
  - inputs:  `feat_erb` `[1,1,S,32]`, `feat_spec` `[1,2,S,96]`
  - outputs (each already sliced to last frame): `e0` `[1,64,1,32]`, `e1` `[1,64,1,16]`,
    `e2` `[1,64,1,8]`, `e3` `[1,64,1,8]`, `c0` `[1,64,1,96]`, `emb` `[1,1,512]`
- **`enc_gru_streaming.onnx`** — *stateful, one frame/hop (W=1).*
  - inputs:  `emb` `[1,1,512]` (= `enc_conv`’s `emb`), `h0` `[1,1,512]`
  - outputs: `emb_out` `[1,1,512]`, `lsnr` `[1,1,1]`, `h1` `[1,1,512]`
- **`erb_dec_streaming.onnx`** — *windowed + stateful (2 GRU layers).* **W = 3.**
  - inputs:  `emb` `[1,S,512]`, `e3` `[1,64,S,8]`, `e2` `[1,64,S,8]`, `e1` `[1,64,S,16]`,
    `e0` `[1,64,S,32]`, `erb_h0` `[2,1,512]`
  - outputs: `m` `[1,1,1,32]` (slice-last), `erb_h1` `[2,1,512]`
- **`df_dec_streaming.onnx`** — *windowed + stateful (3 GRU layers).* **W = 5.**
  - inputs:  `emb` `[1,S,512]`, `c0` `[1,64,S,96]`, `df_h0` `[3,1,512]`
  - outputs: `coefs` `[1,1,96,10]` (slice-last), `302` `[1,S,1]` (aux, unused), `df_h1` `[3,1,512]`

`emb` for the decoders is `enc_gru`’s `emb_out`; `erb_dec` also takes `enc_conv`’s `e0..e3`;
`df_dec` also takes `enc_conv`’s `c0`.

### Host driving contract (per 10 ms hop, for the Phase-2 C++ orchestrator)

Windowed graphs are fed a rolling window of the last `W` input frames and return the last frame;
stateful graphs run one frame/hop carrying state. The host keeps, per stream:

- **`enc_conv`** — rolling window of the last **6** `feat_erb`/`feat_spec` frames → run → take
  `e0..e3,c0,emb` (each the single last frame). No state.
- **`enc_gru`** — feed `enc_conv`’s `emb` frame + `h` (init zeros `[1,1,512]`) → `emb_out`,
  `lsnr`, `h1`; set `h ← h1`. One frame/hop.
- Buffer each hop’s `emb_out` and `e0..e3`/`c0` into rolling windows for the decoders.
- **`erb_dec`** — window of the last **3** frames of (`emb_out`,`e3`,`e2`,`e1`,`e0`) + `erb_h`
  (init zeros `[2,1,512]`) → `m` (last frame) + `erb_h1`.
- **`df_dec`** — window of the last **5** frames of (`emb_out`,`c0`) + `df_h` (init zeros
  `[3,1,512]`) → `coefs` (last frame) + `df_h1`.

**Decoder state advance-by-one (critical).** `*_h1` is the GRU state **after the first window
frame**, not the last. The host carries `h` = state *before* the oldest frame currently in the
window. Only once the window is full (about to drop its oldest frame) does the host advance
`h ← (h1 from the run made while that frame was still the oldest)`. Concretely, per hop `t` with
window `buf`:

```
if len(buf) == W:  h = pending_h1;  buf.pop_front()   # advance past the dropped frame
buf.push_back(frame_t)
res = run(window=buf, h0=h)          # slice-last data output for frame t
pending_h1 = res.h1                  # = state after buf[0]
```

This reproduces the persistent-GRU result exactly (see Gate A). At `W=1` it is the trivial
`h ← h1` stateful GRU.

### Gate A result (`python tools/dfn3_stream_check.py models/dfn3-ll`, seed 0, T=40)

| model    | window | worst data-output max\|diff\| |
|----------|:------:|:-----------------------------:|
| enc      |   6    | `1.24e-05` (`lsnr`; `emb` `9.5e-07`; convs `0.0`) |
| erb_dec  |   3    | `5.66e-07` (`m`)              |
| df_dec   |   5    | `4.77e-07` (`coefs`)         |

**WORST = `1.24e-05`  <  TOL `1e-3`  →  GATE A: PASS.** The convs reproduce bit-exactly; the tiny
residuals are float GRU-accumulation only.
