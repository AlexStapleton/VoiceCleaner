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

Vendored; ONNX Runtime **1.28.3** (FetchContent, pinned SHA256
`1d6fab48e85f948436af7c8c971d2c145cf224e2c444755dec894f8b0de11a83`; 1.28.0 until v1.5). Use `intra_op_num_threads=1`.

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

Generated by `docs/archive/dfn3-tools/dfn3_make_streaming.py`; validated by Gate A (`docs/archive/dfn3-tools/dfn3_stream_check.py`).
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

### Gate A result (`python docs/archive/dfn3-tools/dfn3_stream_check.py models/dfn3-ll`, seed 0, T=40)

| model    | window | worst data-output max\|diff\| |
|----------|:------:|:-----------------------------:|
| enc      |   6    | `1.24e-05` (`lsnr`; `emb` `9.5e-07`; convs `0.0`) |
| erb_dec  |   3    | `5.66e-07` (`m`)              |
| df_dec   |   5    | `4.77e-07` (`coefs`)         |

**WORST = `1.24e-05`  <  TOL `1e-3`  →  GATE A: PASS.** The convs reproduce bit-exactly; the tiny
residuals are float GRU-accumulation only.

---

## Streaming models (standard) — the 2-frame-lookahead variant (2026-08-15)

Same windowed-surgery design as `_ll`, extended to the **standard** model (`models/dfn3/`), which
is **not** purely causal: `conv_lookahead=2`, `df_lookahead=2`. The standard model is the externally
validated one (r=0.9946 vs the `deep-filter` binary). The single tooling
(`docs/archive/dfn3-tools/dfn3_make_streaming.py`) now reads the model's `config.ini` and handles `lookahead ≥ 0`, so
the identical code produces both the `_ll` (lookahead 0) and standard (lookahead 2) streaming sets;
Gate A (`docs/archive/dfn3-tools/dfn3_stream_check.py`) auto-detects GRU hidden size and carries the lookahead lag.

### The lookahead change (vs `_ll`)

The encoder convolutions look **2 frames into the future**. When the streaming conv sub-graph is fed
a window ending at the newest frame, that newest frame's 2 future frames are missing (zero-padded →
wrong), exactly the original desync. The frame with **FULL** context is `conv_lookahead` positions
**before** the newest, so the conv sub-graph slices **frame `S-1-conv_lookahead`** instead of
slice-last. Implemented as a negative-index `Slice` (window-length-independent):

| lookahead `L` | `Slice` starts / ends | selects |
|:---:|:---:|:---|
| 0 (`_ll`) | `-1` / `INT64_MAX` | last frame (unchanged) |
| 2 (standard) | `-3` / `-2` | 3rd-from-last frame |

Consequently the encoder stream **lags the whole-utterance frames by `conv_lookahead=2`**: to emit
logical encoder frame `t`, the host must already have fed input frame `t+2`. This is the model's
~30 ms algorithmic latency (expected, fine). The **decoders are unchanged** — DFN3 decoders are
causal in time, so they stay windowed + stateful with **slice-last (offset 0)**; the DF path's
`df_lookahead` lives host-side in `DeepFilterRing` (`leftPad = df_order − df_lookahead − 1 = 2`),
**not** in `df_dec`. Gate A confirms decoders reproduce whole-utterance at offset 0.

### Structural deltas vs `_ll` (auto-detected by the tooling)

- **GRU hidden size = 256** (not 512). Encoder pre-GRU embedding is **256** wide; the post-GRU
  `emb_out` (and thus the decoders' `emb` input) is still **512**.
- **`df_dec` has 2 GRU layers** (`df_num_layers=2`, not 3) → `df_h0/df_h1` are `[2,1,256]`.
- Encoder has a **single** GRU node (as `_ll`); the split point (pre-GRU convs) is unchanged.

### Exact I/O (names + shapes; `S` = streamed time dim, sliced outputs are 1 frame)

- **`enc_conv_streaming.onnx`** — *windowed, stateless, slice offset 2.* **W = 5.**
  - inputs:  `feat_erb` `[1,1,S,32]`, `feat_spec` `[1,2,S,96]`
  - outputs (each = the single frame `S-1-2`): `e0` `[1,64,1,32]`, `e1` `[1,64,1,16]`,
    `e2` `[1,64,1,8]`, `e3` `[1,64,1,8]`, `c0` `[1,64,1,96]`, `emb` `[1,1,256]`
- **`enc_gru_streaming.onnx`** — *stateful, one frame/hop.*
  - inputs:  `emb` `[1,S,256]` (= `enc_conv`'s `emb`), `h0` `[1,1,256]`
  - outputs: `emb_out` `[1,1,512]`, `lsnr` `[1,1,1]`, `h1` `[1,1,256]`
- **`erb_dec_streaming.onnx`** — *windowed + stateful (2 GRU layers).* **W = 1.**
  - inputs:  `emb` `[1,S,512]`, `e3` `[1,64,S,8]`, `e2` `[1,64,S,8]`, `e1` `[1,64,S,16]`,
    `e0` `[1,64,S,32]`, `erb_h0` `[2,1,256]`
  - outputs: `m` `[1,1,1,32]` (slice-last), `erb_h1` `[2,1,256]`
- **`df_dec_streaming.onnx`** — *windowed + stateful (2 GRU layers).* **W = 5.**
  - inputs:  `emb` `[1,S,512]`, `c0` `[1,64,S,96]`, `df_h0` `[2,1,256]`
  - outputs: `coefs` `[1,1,96,10]` (slice-last), `df_h1` `[2,1,256]`

`erb_dec` needs only **W = 1** (standard `conv_kernel=1,3` → no temporal conv beyond the GRU);
`df_dec` needs **W = 5** (`df_pathway_kernel_size_t=5`); the encoder needs **W = 5** covering the
compounded conv receptive field **plus** the 2 lookahead frames.

### Host driving delta vs `_ll`

Identical rolling-window + advance-by-one contract as the `_ll` section, with **one addition**: the
encoder conv sub-graph now returns the frame **2 behind** the newest fed frame. The host feeds
`feat_erb`/`feat_spec` for input frame `f` but the `enc_conv` outputs (and everything downstream)
correspond to **logical frame `f-2`**. During the first 2 hops (`f<2`) the conv sub-graph returns
**0 frames** (empty) — the host must **not** advance `enc_gru` until a frame appears. State sizes
change with hidden=256 (`enc_gru h` `[1,1,256]`, `erb_h` `[2,1,256]`, `df_h` `[2,1,256]`). Everything
else (decoder window buffers, `pending_h1` advance-by-one) is unchanged.

### Gate A result (`python docs/archive/dfn3-tools/dfn3_stream_check.py models/dfn3`, seed 0, T=40, lag 2)

| model    | window | worst data-output max\|diff\| |
|----------|:------:|:-----------------------------:|
| enc      |   5    | `4.77e-06` (`lsnr`; `emb` `2.4e-07`; convs `0.0`) |
| erb_dec  |   1    | `1.07e-06` (`m`)              |
| df_dec   |   5    | `2.09e-07` (`coefs`)         |

**WORST = `4.77e-06`  <  TOL `1e-3`  →  GATE A: PASS.** (38 of 40 encoder frames compared — the
last 2 are the lookahead lag, not emitted from real input.) Convs reproduce bit-exactly; residuals
are float GRU-accumulation only. The `_ll` regression (`… models/dfn3-ll`) still PASSES unchanged
(WORST `1.24e-05`).
