# Live DeepFilterNet3 (streaming) + warm-standby crossfade — Design

_Date: 2026-08-14_
_Status: approved (brainstorming) — pending implementation plan_

## 1. Problem

DeepFilterNet3 (DFN3) is VoiceCleaner's intended **default** engine (blueprint D9: best for
non-stationary keyboard clatter). The offline DFN3 pipeline is complete and validated against the
official `deep-filter` reference at Pearson r = 0.9946 (`docs/dfn3-validation.md`). But it runs
**whole-utterance only**: the three shipped ONNX models (`models/dfn3/{enc,erb_dec,df_dec}.onnx`)
process the entire time axis `T` at once, and their internal GRUs carry recurrent state across `T`
implicitly. The current export **bakes each GRU's initial state as a constant zero and discards the
final state**, so calling a model at `T=1` per 10 ms hop resets the GRU every frame → garbage output.

RNNoise is therefore the only live engine today; DFN3 is offline-only. This work unblocks
**real-time, frame-by-frame DFN3** and makes it live-selectable, then adds warm-standby engine
switching with a delay-aligned crossfade.

## 2. Feasibility (established by investigation, 2026-08-14)

- **The DFN3 encoder is causal.** All five GRUs across the three models are **unidirectional
  forward** (op `GRU`, no `direction` attribute → ONNX default `forward`; W/R first dim = 1 =
  num_directions), hidden size 256. None are bidirectional. Frame-by-frame streaming is
  architecturally sound. (The earlier "emb = bidirectional 512" note was wrong: the 512-wide `emb`
  comes from a GroupedLinear projection 256→512 **after** a single causal GRU.)
- **The blocker is purely export shape.** enc's GRU 6th input `initial_h` is a `ConstantOfShape`
  zeros tensor; its `Y_h` final state is neither a graph output nor consumed by any node. Exposing
  those as graph I/O is a pure ONNX graph-surgery transform — **no PyTorch, no retraining**. Only
  `onnx` + `onnxsim` are required, both available on this machine (torch is not installed and not
  needed).
- **Latency is bounded and inherent.** DFN3 has `conv_lookahead=2` and `df_lookahead=2`. The GRUs
  are strictly causal; the deep filter reads up to 2 frames into the future
  (`spec[t+k-lookahead]`), so streaming carries a fixed ~2-frame algorithmic delay. Unavoidable in
  the causal design, standard for real-time DFN.

## 3. What is already frame-by-frame (reused as-is)

The offline orchestrator (`src/dfn3/dfn3_offline.cpp`) already drives most stages per hop:

- `Stft::analyze` / `Stft::synthesize` — per-hop (Vorbis window, libDF `wnorm`), perfect reconstruction.
- `Dfn3Features::process` — **already stateful**: holds the exp-mean ERB normalization (alpha=0.99)
  and per-bin unit-norm state across `process()` calls. Streaming-ready with no change.
- `apply_erb_mask` — per-frame (broadcast band gains).

The **only** whole-`T` pieces are (a) the three ONNX calls (GRU state) and (b) `apply_deep_filter`,
which currently indexes a full `[T, nbDf, order, 2]` coefs buffer and taps a full-`T` noisy
spectrum. Both are addressed below.

## 4. Architecture

### 4.1 Streaming ONNX models — our own graph surgery

New committed script `tools/dfn3_make_streaming.py` (deps `onnx`, `onnxsim`). Reads the shipped
`models/dfn3/{enc,erb_dec,df_dec}.onnx` (untouched) and writes `*_streaming.onnx` alongside them.
For each GRU node:

- Replace the `initial_h` constant with a new **graph input** `h_in`.
- Expose the GRU's second output `Y_h` as a new **graph output** `h_out`.
- **enc** — 1 GRU → state `[1,1,256]` (`num_directions, batch, hidden`).
- **erb_dec** — 2 stacked GRUs → a `[2,1,256]` state input `Slice`d per layer, outputs re-stacked.
- **df_dec** — 2 stacked GRUs → same `[2,1,256]` pattern.

Models remain dynamic on the time axis; we invoke them with `T=1`. `onnxsim` folds/cleans the
patched graphs. The script prints the exact new input/output names + shapes (consumed by §4.2).

**Gate A (de-risk before any C++):** a Python validation (extend `tools/dfn3_*`) streams the patched
models frame-by-frame — feeding each frame's `h_out` back as the next `h_in` — over the validation
input, and asserts the concatenated result is **bit-close** (max abs error within float tolerance)
to the current whole-`T` run through the original models. If Gate A fails, stop: nothing downstream
is worth building.

### 4.2 Streaming C++ orchestrator — `Dfn3Streaming`

New class in `src/dfn3/` (e.g. `dfn3_streaming.{h,cpp}`), constructed from the model dir, exposing a
per-hop `process(const float in[480], float out[480])` (plus an explicit warm-up/latency contract).

State held across hops:

- 3 ORT sessions (`intra_op_num_threads=1`, blueprint §6.2) + persistent GRU state buffers
  (`h`, `erb_h`, `df_h`), initialized to zeros, carried hop→hop.
- The stateful `Stft` (analysis + synthesis) and `Dfn3Features` instances.
- A ring of the last `order + lookahead` (= 7) frames of **noisy** low-bin spectrum, plus the
  `coefs[96,10]` and ERB `mask[32]` per frame, so the deep filter's taps `spec[t+k-lookahead]`
  (reaching `t+2`) resolve. Output for input-frame `t` is emitted after input-frame `t+2` is seen.

Per hop `t`:
1. `Stft::analyze` → noisy complex spectrum `[481]`; push low bins into the ring.
2. `Dfn3Features::process` → `feat_erb[32]`, `feat_spec[2,96]`.
3. `enc(feat_erb, feat_spec, h_in) → {e0..e3, emb, c0, lsnr, h_out}`; `h ← h_out`.
4. `erb_dec(emb, e3, e2, e1, e0, erb_h_in) → {m[32], erb_h_out}`; `erb_h ← erb_h_out`.
5. `df_dec(emb, c0, df_h_in) → {coefs[96,10], df_h_out}`; `df_h ← df_h_out`.
6. Once ≥ `lookahead` future frames are buffered, assemble the enhanced spectrum for the delayed
   output frame: streaming `apply_deep_filter` over the ring for low bins (< 96) on the **noisy**
   spectrum, `apply_erb_mask` for high bins (≥ 96); `Stft::synthesize` → emit 480 output samples.

A **streaming variant of `apply_deep_filter`** operates on the ring (per output frame, taps
`k=0..order-1` at ring positions) rather than a whole-`T` buffer, using the authoritative
`spec[t+k-lookahead]` tap ordering and complex multiply already in `dfn3_postdsp.cpp`. During warm-up
(before the ring is primed) the orchestrator emits silence/zeros and reports the delay.

`AlgorithmicDelaySamples()` returns STFT latency (480) + `lookahead` frames (2 × 480 = 960) ≈
**1440 samples (~30 ms)**.

**Gate B:** an offline driver (new `--engine dfn3-stream` in `offline-cli`, or a dedicated tool)
runs `Dfn3Streaming` over the validation WAV and compares against (a) the existing offline DFN3
output and (b) the `deep-filter` reference, via `tools/dfn3_compare.py`. Must hold **r ≈ 0.99**
against the reference (matching the offline pipeline).

### 4.3 Live single-engine integration

`Dfn3Streaming` is wrapped behind the existing `INoiseFilter` interface
(`ProcessFrame(in,out)` for exactly 480 samples; `AlgorithmicDelaySamples()`; `SetAggression`;
`LastVadProbability` sourced from `lsnr`/VAD as appropriate). Config `engine = "dfn3"` selects it;
it drops into the existing `DspChain` (DC → engine → HPF → AGC → limiter) and the live relay with no
structural change.

**Gate C (the real RT risk):** measure per-hop wall-time on the MMCSS relay thread — three ORT
`Run` calls + the DSP chain must fit the 10 ms hop with comfortable headroom, **zero dropouts**, on
the 9950X3D. Instrument frame-time (reuse the telemetry/logger path). If it does not fit, **stop**
and reconsider (dedicated inference thread with a one-frame handoff, session-option tuning, etc.)
before building the crossfade on top. Single-engine live must be solid before §4.4.

### 4.4 Warm-standby + delay-aligned crossfade

A `SwitchableEngine` (itself an `INoiseFilter`) holds **both** RNNoise and `Dfn3Streaming`. Both are
fed every hop so the standby engine's recurrent/normalization state stays warm (instant switch, no
priming stall). On a switch request it performs an **equal-power crossfade** over ~10–20 frames
between the two engine outputs, **delay-aligning** by each engine's `AlgorithmicDelaySamples()`:
the lower-latency engine (RNNoise) is delayed by the difference so both streams line up before the
mix. A minimal runtime switch trigger (config hot value or a simple command flag) selects the active
engine. The full §8.5 command channel / snapshot machinery is **out of scope** here.

## 5. Build order (gated, subagent-driven)

1. **Graph surgery + Gate A** — streaming models reproduce whole-`T` output in Python.
2. **C++ orchestrator + Gate B** — `Dfn3Streaming` matches offline/reference r ≈ 0.99.
3. **Live wiring + Gate C** — `engine="dfn3"` runs live within the RT deadline, zero dropouts.
4. **Warm-standby crossfade** — `SwitchableEngine`, delay-aligned equal-power crossfade.

Each gate is a hard checkpoint. A failed gate pauses for a decision instead of proceeding.

## 6. Testing

- **Unit:** streaming `apply_deep_filter` equals the whole-`T` `apply_deep_filter` on identical
  coefs/spectrum (ring vs buffer). `SwitchableEngine` crossfade is equal-power and delay-aligned
  (mid-crossfade energy continuity; steady-state equals the single engine). Warm-up/latency contract
  of `Dfn3Streaming` (correct emit delay, no NaNs, silence during priming).
- **Golden/regression:** Gate A (Python streaming == whole-`T`) and Gate B (streaming C++ vs
  reference r ≈ 0.99) become checked-in validation harnesses.
- **RT:** Gate C frame-time histogram + zero-dropout run on real hardware (K688 → CABLE Input).

## 7. Risks

- **RT budget (Gate C)** — the one risk that could genuinely block live DFN3; mitigated by ordering
  single-engine live before the crossfade and by having an inference-thread fallback in reserve.
- **Streaming/whole-`T` fidelity** — GRU state threading and the ring-based deep filter must
  reproduce the offline math exactly; Gates A and B guard this before and after the C++ port.
- **Crossfade latency alignment** — DFN3's ~30 ms delay vs RNNoise's smaller delay must be aligned
  or the switch will click; handled via `AlgorithmicDelaySamples()`.

## 8. Out of scope

- Full §8.5 command channel / parameter-snapshot machinery (only a minimal switch trigger here).
- The 0-lookahead `_ll` model variant.
- Any model retraining or PyTorch re-export (graph surgery only).
- Tray UI, first-run wizard, and other Phase 1/3 items unrelated to the engine.

## 9. Affected / new files (indicative)

- New: `tools/dfn3_make_streaming.py`, streaming validation tooling, `src/dfn3/dfn3_streaming.{h,cpp}`,
  `SwitchableEngine` in `src/denoise-core/`, `models/dfn3/*_streaming.onnx` (generated, committed or
  build-time — TBD in plan).
- Modified: streaming `apply_deep_filter` in `dfn3_postdsp.{h,cpp}`, engine selection in the live
  app / config, `offline-cli` (`--engine dfn3-stream`), `docs/STATUS.md`.
- Reused unchanged: `stft.h`, `erb.h`, `dfn3_features.{h,cpp}`, `dsp_chain.h`, `inoise_filter.h`.
