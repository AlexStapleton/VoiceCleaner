# Live DeepFilterNet3 (streaming) + warm-standby crossfade — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run DeepFilterNet3 in real time (frame-by-frame) as a live-selectable engine, then add warm-standby switching with a delay-aligned crossfade to RNNoise.

**Architecture:** Rewrite the shipped whole-utterance DFN3 ONNX models into streaming variants that expose each GRU's hidden state as graph I/O (pure `onnx` graph surgery). A new `Dfn3Streaming` C++ class runs the three models at `T=1` per 10 ms hop, carrying GRU state and buffering a short ring for the deep filter's 2-frame lookahead. It is wrapped behind `INoiseFilter` and selected with `engine="dfn3"`. Finally a `SwitchableEngine` holds both RNNoise and DFN3, both kept warm, and crossfades between them delay-aligned by each engine's algorithmic latency.

**Tech Stack:** C++20, CMake/MSVC, ONNX Runtime 1.28.0 (CPU), GoogleTest, Python 3.13 (`onnx`, `onnxruntime`, `onnxsim`, `numpy`) for model surgery + validation.

**Design spec:** `docs/superpowers/specs/2026-08-14-live-dfn3-streaming-design.md`

> **PLAN REVISION (2026-08-14) — read spec §11 first.** Gate A on the original Phase 1 failed:
> GRU-state surgery alone is insufficient (the encoder has **temporal convolutions** whose per-frame
> history must also be carried), and `_ll` is not a shortcut. **Phase 1 below is REPLACED** by the
> windowed-encoder-split approach in spec §11 (conv sub-graph fed a rolling frame window + slice-last;
> GRU sub-graph made stateful `h0`→`h1`; decoders GRU-state-only; deep-filter FIR stays in host C++).
> **Target model = `_ll`** (512-wide, 3 decoder GRU layers, ~10 ms latency), vendored under
> `models/dfn3-ll/`. Downstream changes to Phases 2–4: (a) `DeepFilterRing`/`apply_deep_filter` use the
> causal offset `order−lookahead−1 = 4` for `_ll` (not 2); (b) `Dfn3Streaming` feeds `enc_conv` a
> rolling window and `enc_gru` one frame/hop, and carries stacked decoder GRU state; (c) model dir is
> `models/dfn3-ll`; (d) Gate B's golden reference is a whole-utterance `_ll` run (not the standard
> r=0.9946 output); (e) `AlgorithmicDelaySamples()` ≈ 480. The gated build order and Phases 3–4
> (`SwitchableEngine`, live wiring) are otherwise unchanged.

---

## Conventions (read once)

CMake/CTest/Python are not all on PATH. At the start of each execution session, run:

```bash
export PATH="/c/Espressif/tools/cmake/4.0.3/bin:$PATH"
```

Then `cmake` / `ctest` resolve. Build/test commands used throughout:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`python` is the scoop Python 3.13 (`python --version` → 3.13.x). One-time Python deps
(the surgery/validation tooling; `onnx` is already installed):

```bash
python -m pip install onnxsim onnxruntime
```

All shell commands assume the repo root `I:\Coding-Projects\VoiceCleaner` as CWD.

**Gate discipline:** Tasks 3, 7, and 11 are hard gates (A, B, C). If a gate fails, STOP and report — do not proceed to the next phase.

---

## File structure

**New files**
- `tools/dfn3_make_streaming.py` — graph surgery: whole-utterance models → `*_streaming.onnx`.
- `tools/dfn3_stream_check.py` — Gate A: streaming models reproduce whole-utterance models.
- `models/dfn3/{enc,erb_dec,df_dec}_streaming.onnx` — generated, committed.
- `src/dfn3/dfn3_streaming.{h,cpp}` — streaming orchestrator `Dfn3Streaming`.
- `src/denoise-core/dfn3_stream_adapter.{h,cpp}` — `INoiseFilter` wrapper over `Dfn3Streaming`.
- `src/denoise-core/switchable_engine.{h,cpp}` — dual-engine warm-standby + crossfade.
- `tests/denoise-core/test_switchable_engine.cpp` — crossfade unit tests.
- Test additions in `tests/denoise-core/test_dfn3_postdsp.cpp` — streaming deep-filter equivalence.

**Modified files**
- `src/denoise-core/dfn3_postdsp.{h,cpp}` — add per-frame streaming `apply_deep_filter_frame`.
- `src/dfn3/CMakeLists.txt` — add `dfn3_streaming.cpp`.
- `src/denoise-core/CMakeLists.txt` — add adapter + switchable engine sources; link `dfn3`/`onnxruntime`.
- `src/offline-cli/main.cpp` — add `--engine dfn3-stream`.
- `src/phase0-app/{bridge.cpp,CMakeLists.txt}`, `main.cpp` — engine selection + ORT dll copy + switch trigger.
- `src/transport-core/config.{h,cpp}` — `engine` accepts `dfn3`; warm-standby options.
- `docs/STATUS.md`, `docs/dfn3-progress.md`, memory files — status update.

---

## Phase 1 — Streaming ONNX models + Gate A

### Task 1: Graph-surgery script exposing GRU state I/O

**Files:**
- Create: `tools/dfn3_make_streaming.py`

- [ ] **Step 1: Write the surgery script**

```python
#!/usr/bin/env python3
"""Rewrite the shipped whole-utterance DFN3 ONNX models into streaming variants
that expose each GRU's hidden state as graph I/O.

For every GRU node in enc/erb_dec/df_dec:
  * its `initial_h` input (GRU input index 5, currently a baked ConstantOfShape
    zeros tensor) is replaced by a new graph INPUT  `<prefix>_h<i>_in`  [1,1,256];
  * its `Y_h` output (GRU output index 1, currently dangling) is exposed as a new
    graph OUTPUT `<prefix>_h<i>_out` of the same shape.
The i-th GRU (in node order) of each model gets index i. Models stay dynamic on
the time axis; run them with sequence length T=1 per hop.

Pure onnx graph surgery + onnxsim cleanup -- no PyTorch, no retraining.
Deps: onnx, onnxsim.  Usage: python tools/dfn3_make_streaming.py [models/dfn3]
"""
import os
import sys
import onnx
from onnx import helper, TensorProto

HIDDEN = 256
STATE_SHAPE = [1, 1, HIDDEN]          # [num_directions, batch, hidden]


def patch(model_path, out_path, prefix):
    model = onnx.load(model_path)
    graph = model.graph
    grus = [n for n in graph.node if n.op_type == "GRU"]
    if not grus:
        raise SystemExit(f"{model_path}: no GRU nodes found")

    new_inputs, new_outputs = [], []
    for i, node in enumerate(grus):
        in_name = f"{prefix}_h{i}_in"
        # GRU inputs: X, W, R, B, sequence_lens, initial_h. Pad optionals, set [5].
        while len(node.input) < 6:
            node.input.append("")
        node.input[5] = in_name
        # GRU outputs: Y, Y_h. Ensure a named Y_h and expose it.
        while len(node.output) < 2:
            node.output.append("")
        if not node.output[1]:
            node.output[1] = f"{prefix}_h{i}_out"
        out_name = node.output[1]
        new_inputs.append(
            helper.make_tensor_value_info(in_name, TensorProto.FLOAT, STATE_SHAPE))
        new_outputs.append(
            helper.make_tensor_value_info(out_name, TensorProto.FLOAT, STATE_SHAPE))

    graph.input.extend(new_inputs)
    graph.output.extend(new_outputs)

    # Best-effort cleanup: prune the now-dead ConstantOfShape/Shape subgraphs that
    # used to feed initial_h. A dead node with no consumer is harmless if this fails.
    try:
        from onnxsim import simplify
        simp, ok = simplify(model)
        if ok:
            model = simp
    except Exception as e:  # noqa: BLE001
        print(f"  (onnxsim skipped: {e})")

    onnx.checker.check_model(model)
    onnx.save(model, out_path)
    names = ", ".join(f"{prefix}_h{i}_in/out" for i in range(len(grus)))
    print(f"{out_path}: exposed {len(grus)} GRU state(s): {names}")


def main(model_dir):
    jobs = [
        ("enc.onnx", "enc_streaming.onnx", "enc"),
        ("erb_dec.onnx", "erb_dec_streaming.onnx", "erb"),
        ("df_dec.onnx", "df_dec_streaming.onnx", "df"),
    ]
    for src, dst, prefix in jobs:
        patch(os.path.join(model_dir, src), os.path.join(model_dir, dst), prefix)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "models/dfn3")
```

- [ ] **Step 2: Run the script**

Run: `python tools/dfn3_make_streaming.py models/dfn3`
Expected: three lines like `models/dfn3/enc_streaming.onnx: exposed 1 GRU state(s): enc_h0_in/out`,
`erb_dec_streaming.onnx: exposed 2 GRU state(s): erb_h0_in/out, erb_h1_in/out`,
`df_dec_streaming.onnx: exposed 2 GRU state(s): df_h0_in/out, df_h1_in/out`. No checker error.

- [ ] **Step 3: Verify new graph I/O with onnx**

Run:
```bash
python -c "import onnx; m=onnx.load('models/dfn3/enc_streaming.onnx'); print('IN',[i.name for i in m.graph.input]); print('OUT',[o.name for o in m.graph.output])"
```
Expected: inputs include `feat_erb`, `feat_spec`, `enc_h0_in`; outputs include `emb`, `e0`..`e3`, `c0`, `lsnr`, `enc_h0_out`.

- [ ] **Step 4: Commit**

```bash
git add tools/dfn3_make_streaming.py models/dfn3/enc_streaming.onnx models/dfn3/erb_dec_streaming.onnx models/dfn3/df_dec_streaming.onnx
git commit -m "feat(dfn3): graph surgery -> streaming ONNX models with GRU state I/O"
```

---

### Task 2: (documentation) record the streaming model I/O names + shapes

**Files:**
- Modify: `docs/dfn3-progress.md` (append a "Streaming models" section)

- [ ] **Step 1: Query and record exact I/O for all three streaming models**

Run:
```bash
python -c "import onnx;\
[print(f) or [print(' IN ',i.name) for i in onnx.load(f).graph.input] or [print(' OUT',o.name) for o in onnx.load(f).graph.output] for f in ['models/dfn3/enc_streaming.onnx','models/dfn3/erb_dec_streaming.onnx','models/dfn3/df_dec_streaming.onnx']]"
```

- [ ] **Step 2: Append the observed names to `docs/dfn3-progress.md`**

Write a short section listing, for each streaming model, its input names (feature inputs + `*_h*_in`) and output names (data outputs + `*_h*_out`), and the state shape `[1,1,256]`. This is the reference the C++ orchestrator (Task 5) wires against.

- [ ] **Step 3: Commit**

```bash
git add docs/dfn3-progress.md
git commit -m "docs(dfn3): record streaming model I/O names and state shapes"
```

---

### Task 3: **GATE A** — streaming models reproduce whole-utterance models (Python)

**Files:**
- Create: `tools/dfn3_stream_check.py`

- [ ] **Step 1: Write the validation harness**

```python
#!/usr/bin/env python3
"""GATE A: prove the streaming DFN3 models reproduce the whole-utterance models.

For each model (enc / erb_dec / df_dec) run the SAME random feature input two ways:
  * whole-T : original model, sequence length T at once (baked zero initial state)
  * stream  : *_streaming model, T calls of length 1, feeding h_out -> h_in
then report the max abs difference of every non-state output. PASS if < 1e-3
(float GRU accumulation tolerance). enc feeds erb_dec/df_dec, so its emb from the
whole-T run is reused as their input.

Deps: onnx, onnxruntime, numpy.  Usage: python tools/dfn3_stream_check.py [models/dfn3]
"""
import os
import sys
import numpy as np
import onnxruntime as ort

HIDDEN = 256
T = 40                      # avoid collision with fixed dims (64,32,16,8,96,512,481,2,1)
TOL = 1e-3
RNG = np.random.default_rng(0)


def sess(path):
    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    return ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])


def data_outputs(s):
    return [o.name for o in s.get_outputs() if not o.name.endswith("_out")]


def time_axis(shape):
    # the (unique) axis whose size == T. T is chosen to not collide with fixed dims.
    axes = [ax for ax, d in enumerate(shape) if d == T]
    assert len(axes) == 1, f"ambiguous/absent time axis in {shape}"
    return axes[0]


def run_whole(s, feats):
    names = data_outputs(s)
    return dict(zip(names, s.run(names, feats)))


def run_stream(s, feats, ref):
    """Stream T length-1 calls, carrying h_out -> h_in. `ref` (the whole-T output
    dict) is used only to locate each output's time axis for concatenation."""
    st_in = sorted(i.name for i in s.get_inputs() if i.name.endswith("_in"))
    st_out = sorted(o.name for o in s.get_outputs() if o.name.endswith("_out"))
    states = {n: np.zeros([1, 1, HIDDEN], np.float32) for n in st_in}
    dnames = data_outputs(s)
    all_out = dnames + st_out
    acc = {n: [] for n in dnames}
    for t in range(T):
        feed = {}
        for k, v in feats.items():
            idx = [slice(None)] * v.ndim
            idx[time_axis(v.shape)] = slice(t, t + 1)   # slice this frame on its time axis
            feed[k] = np.ascontiguousarray(v[tuple(idx)])
        feed.update(states)
        res = dict(zip(all_out, s.run(all_out, feed)))
        for n in dnames:
            acc[n].append(res[n])
        for si, so in zip(st_in, st_out):               # h_out(i) -> h_in(i), paired by sort order
            states[si] = res[so]
    return {n: np.concatenate(acc[n], axis=time_axis(ref[n].shape)) for n in dnames}


def cmp(name, whole, stream):
    worst = 0.0
    for k in whole:
        d = float(np.max(np.abs(whole[k].astype(np.float64) - stream[k].astype(np.float64))))
        worst = max(worst, d)
        print(f"  {name}.{k:6s} max|diff| = {d:.3e}")
    return worst


def main(md):
    feat_erb = RNG.standard_normal((1, 1, T, 32)).astype(np.float32)
    feat_spec = RNG.standard_normal((1, 2, T, 96)).astype(np.float32)

    enc_w, enc_s = sess(md + "/enc.onnx"), sess(md + "/enc_streaming.onnx")
    ew = run_whole(enc_w, {"feat_erb": feat_erb, "feat_spec": feat_spec})
    es = run_stream(enc_s, {"feat_erb": feat_erb, "feat_spec": feat_spec}, ew)
    worst = cmp("enc", ew, es)

    # decoders consume enc's whole-T outputs; reuse ew[...] as their feature input.
    erb_w, erb_s = sess(md + "/erb_dec.onnx"), sess(md + "/erb_dec_streaming.onnx")
    ef = {n: ew[n] for n in ("emb", "e3", "e2", "e1", "e0")}
    erb_ref = run_whole(erb_w, ef)
    worst = max(worst, cmp("erb_dec", erb_ref, run_stream(erb_s, ef, erb_ref)))

    df_w, df_s = sess(md + "/df_dec.onnx"), sess(md + "/df_dec_streaming.onnx")
    dff = {n: ew[n] for n in ("emb", "c0")}
    df_ref = run_whole(df_w, dff)
    worst = max(worst, cmp("df_dec", df_ref, run_stream(df_s, dff, df_ref)))

    print(f"\nWORST max|diff| = {worst:.3e}  TOL = {TOL:.0e}")
    print("GATE A: PASS" if worst < TOL else "GATE A: FAIL")
    return 0 if worst < TOL else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "models/dfn3"))
```

- [ ] **Step 2: Run Gate A**

Run: `python tools/dfn3_stream_check.py models/dfn3`
Expected: per-output diffs printed, then `WORST max|diff| = <small>` and `GATE A: PASS`.

- [ ] **Step 3: If Gate A fails, STOP**

If any decoder's outputs diverge, the likely cause is wrong `initial_h`/`Y_h` index or the
2-layer state mapping (h0/h1 order). Inspect the failing model's GRU nodes
(`python -c "import onnx; [print(n.name, list(n.input), list(n.output)) for n in onnx.load('models/dfn3/erb_dec.onnx').graph.node if n.op_type=='GRU']"`)
and fix Task 1's surgery. Do not proceed to Phase 2 until PASS.

- [ ] **Step 4: Commit**

```bash
git add tools/dfn3_stream_check.py
git commit -m "test(dfn3): Gate A - streaming ONNX reproduces whole-utterance (PASS)"
```

---

## Phase 2 — Streaming C++ orchestrator + Gate B

### Task 4: Streaming (ring-based) deep filter

**Files:**
- Modify: `src/denoise-core/dfn3_postdsp.h`
- Modify: `src/denoise-core/dfn3_postdsp.cpp`
- Test: `tests/denoise-core/test_dfn3_postdsp.cpp`

The live orchestrator applies the deep filter per output frame from a small ring, not the
whole-`T` buffer. Add a single-output-frame variant and prove it equals the whole-`T` result.

- [ ] **Step 1: Write the failing test**

Append to `tests/denoise-core/test_dfn3_postdsp.cpp`:

```cpp
TEST(Dfn3Post, StreamingDeepFilterMatchesWholeT){
  const int T=12,nbDf=96,order=5,la=2;
  // random-ish low-bin spectrum + coefs
  std::vector<float> re(T*481), im(T*481);
  for(int i=0;i<T*481;++i){ re[i]=std::sin(0.021f*i); im[i]=std::cos(0.017f*i); }
  std::vector<float> coefs(T*nbDf*order*2);
  for(size_t i=0;i<coefs.size();++i) coefs[i]=0.1f*std::sin(0.003f*(float)i);

  // reference: whole-T in-place
  std::vector<float> reW=re, imW=im;
  apply_deep_filter(reW.data(), imW.data(), coefs.data(), T, nbDf, order, la);

  // streaming: feed frames 0..T-1 into a ring; output frame t emitted when t+la seen.
  std::vector<float> reS(T*481,0.0f), imS(T*481,0.0f);
  for(int f=nbDf;f<481;++f) for(int t=0;t<T;++t){ reS[t*481+f]=re[t*481+f]; imS[t*481+f]=im[t*481+f]; }
  DeepFilterRing ring(nbDf, order, la);
  for(int t=0;t<T;++t){
    int outT; float outRe[96], outIm[96];
    bool ready = ring.push(re.data()+t*481, im.data()+t*481,
                           coefs.data()+(size_t)t*nbDf*order*2, outT, outRe, outIm);
    if(ready){ for(int b=0;b<nbDf;++b){ reS[outT*481+b]=outRe[b]; imS[outT*481+b]=outIm[b]; } }
  }
  // flush the final `la` frames
  int outT; float outRe[96], outIm[96];
  while(ring.flush(outT, outRe, outIm))
    for(int b=0;b<nbDf;++b){ reS[outT*481+b]=outRe[b]; imS[outT*481+b]=outIm[b]; }

  for(int t=0;t<T;++t) for(int b=0;b<nbDf;++b){
    EXPECT_NEAR(reS[t*481+b], reW[t*481+b], 1e-5f) << "t="<<t<<" b="<<b;
    EXPECT_NEAR(imS[t*481+b], imW[t*481+b], 1e-5f) << "t="<<t<<" b="<<b;
  }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Debug` then
`ctest --test-dir build -C Debug -R Dfn3Post.StreamingDeepFilterMatchesWholeT --output-on-failure`
Expected: compile error / FAIL — `DeepFilterRing` undefined.

- [ ] **Step 3: Add `DeepFilterRing` to the header**

Add to `src/denoise-core/dfn3_postdsp.h` (inside `namespace vc`):

```cpp
// Streaming deep filter: push one frame's low-bin noisy spectrum + coefs at a time.
// Emits the enhanced low bins for the frame `lookahead` positions in the past once
// enough future frames are buffered (matches the whole-T apply_deep_filter exactly,
// with constant-zero padding at the stream ends). One instance per stream; not thread-safe.
class DeepFilterRing {
public:
  DeepFilterRing(int nbDf, int order, int lookahead);
  // push frame t (in arrival order). Returns true and fills outT/outRe[nbDf]/outIm[nbDf]
  // when an output frame becomes ready (outT = t - lookahead).
  bool push(const float* re481, const float* im481, const float* coefs_nbDf_order2,
            int& outT, float* outRe, float* outIm) noexcept;
  // after the last push, drain the final `lookahead` frames (constant-zero future).
  bool flush(int& outT, float* outRe, float* outIm) noexcept;
private:
  int nbDf_, order_, la_, depth_;      // depth_ = order + lookahead
  int pushed_ = 0, emitted_ = 0;
  std::vector<float> re_, im_, cf_;    // ring: depth_ frames of low-bin spec + coefs
  bool emit(int outT, float* outRe, float* outIm) noexcept;
};
```

Ensure `#include <vector>` is present in the header.

- [ ] **Step 4: Implement `DeepFilterRing` in the .cpp**

Add to `src/denoise-core/dfn3_postdsp.cpp`:

```cpp
DeepFilterRing::DeepFilterRing(int nbDf, int order, int lookahead)
    : nbDf_(nbDf), order_(order), la_(lookahead), depth_(order + lookahead),
      re_(static_cast<size_t>(depth_) * nbDf, 0.0f),
      im_(static_cast<size_t>(depth_) * nbDf, 0.0f),
      cf_(static_cast<size_t>(depth_) * nbDf * order * 2, 0.0f) {}

bool DeepFilterRing::emit(int outT, float* outRe, float* outIm) noexcept {
  // Output frame outT reads input frames [outT-la .. outT-la+order-1] and coefs[outT].
  // Ring slot for absolute frame f is f % depth_. Frames outside [0, pushed_) are zero.
  const int slotC = outT % depth_;
  const float* coefs = cf_.data() + static_cast<size_t>(slotC) * nbDf_ * order_ * 2;
  for (int b = 0; b < nbDf_; ++b) {
    float accRe = 0.0f, accIm = 0.0f;
    for (int k = 0; k < order_; ++k) {
      const int ti = outT + k - la_;
      if (ti < 0 || ti >= pushed_) continue;          // constant-zero padding
      const int slot = ti % depth_;
      const float sRe = re_[static_cast<size_t>(slot) * nbDf_ + b];
      const float sIm = im_[static_cast<size_t>(slot) * nbDf_ + b];
      const size_t ci = (static_cast<size_t>(b) * order_ + k) * 2;
      const float cRe = coefs[ci], cIm = coefs[ci + 1];
      accRe += sRe * cRe - sIm * cIm;
      accIm += sIm * cRe + sRe * cIm;
    }
    outRe[b] = accRe;
    outIm[b] = accIm;
  }
  return true;
}

bool DeepFilterRing::push(const float* re481, const float* im481,
                          const float* coefs_nbDf_order2, int& outT,
                          float* outRe, float* outIm) noexcept {
  const int slot = pushed_ % depth_;
  for (int b = 0; b < nbDf_; ++b) {
    re_[static_cast<size_t>(slot) * nbDf_ + b] = re481[b];
    im_[static_cast<size_t>(slot) * nbDf_ + b] = im481[b];
  }
  for (int i = 0; i < nbDf_ * order_ * 2; ++i)
    cf_[static_cast<size_t>(slot) * nbDf_ * order_ * 2 + i] = coefs_nbDf_order2[i];
  ++pushed_;
  // an output frame is ready once its lookahead future frame has arrived
  if (pushed_ - 1 - la_ >= emitted_) {
    outT = emitted_++;
    return emit(outT, outRe, outIm);
  }
  return false;
}

bool DeepFilterRing::flush(int& outT, float* outRe, float* outIm) noexcept {
  if (emitted_ >= pushed_) return false;
  outT = emitted_++;
  return emit(outT, outRe, outIm);
}
```

Note: for correctness the ring depth must cover the tap span. Output frame `outT` reads up to
`outT+order-1-la` (future) and `outT-la` (past). When `outT` is emitted, the newest pushed frame is
`outT+la`; the oldest tap is `outT-la`. Span = `2*la .. order` → `depth = order + lookahead` (=7)
covers both the newest tapped future frame and the oldest tapped past frame simultaneously.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --config Debug` then
`ctest --test-dir build -C Debug -R Dfn3Post.StreamingDeepFilterMatchesWholeT --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/denoise-core/dfn3_postdsp.h src/denoise-core/dfn3_postdsp.cpp tests/denoise-core/test_dfn3_postdsp.cpp
git commit -m "feat(dfn3): streaming DeepFilterRing (matches whole-T deep filter)"
```

---

### Task 5: `Dfn3Streaming` orchestrator

**Files:**
- Create: `src/dfn3/dfn3_streaming.h`
- Create: `src/dfn3/dfn3_streaming.cpp`
- Modify: `src/dfn3/CMakeLists.txt`

- [ ] **Step 1: Write the header**

`src/dfn3/dfn3_streaming.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>
namespace vc {
// Real-time DeepFilterNet3. Loads the *_streaming ONNX models (GRU state exposed
// as graph I/O) from modelDir and processes one 480-sample hop per call, carrying
// GRU + normalization state. Output is delayed by latencySamples(): early hops
// return zeros until the pipeline primes. One instance per stream; not thread-safe.
class Dfn3Streaming {
public:
  explicit Dfn3Streaming(const std::string& modelDir);  // throws std::runtime_error on load failure
  ~Dfn3Streaming();
  Dfn3Streaming(const Dfn3Streaming&) = delete;
  Dfn3Streaming& operator=(const Dfn3Streaming&) = delete;
  void process(const float* in480, float* out480) noexcept;  // exactly 480 in/out
  uint32_t latencySamples() const noexcept;                  // STFT (480) + lookahead*hop
  float lastLsnr() const noexcept;                           // most recent lsnr scalar
  void reset() noexcept;
private:
  struct Impl; Impl* p_;
};
}
```

- [ ] **Step 2: Implement the orchestrator**

`src/dfn3/dfn3_streaming.cpp` — per hop: `Stft::analyze` → `Dfn3Features::process` →
`enc`(state) → `erb_dec`(state) → `df_dec`(state) → push into `DeepFilterRing` + a matching
481-bin spectrum/mask ring → when an output frame is ready, assemble (deep-filter low bins,
ERB-mask high bins) → `Stft::synthesize` → emit hop (delayed). Reuse the exact constants and
tensor layouts from `dfn3_offline.cpp` (kHop=480, kBins=481, kErb=32, kDf=96, kOrder=5,
kLookahead=2). Model I/O names come from Task 2's doc: enc inputs `feat_erb`,`feat_spec`,`enc_h0_in`;
outputs include `emb`,`e0..e3`,`c0`,`lsnr`,`enc_h0_out`; `erb_dec` adds `erb_h0_in/erb_h1_in` →
`m`,`erb_h0_out/erb_h1_out`; `df_dec` adds `df_h0_in/df_h1_in` → `coefs`,`df_h0_out/df_h1_out`.

```cpp
#include "dfn3_streaming.h"

#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "dfn3_features.h"
#include "dfn3_postdsp.h"
#include "erb.h"
#include "stft.h"

namespace vc {
namespace {
constexpr int kHop = 480, kBins = 481, kErb = 32, kDf = 96, kOrder = 5, kLookahead = 2;
constexpr int kHidden = 256;
std::wstring widen(const std::string& s){ return std::wstring(s.begin(), s.end()); }

struct NamedSession {
  Ort::Session session{nullptr};
  std::vector<std::string> in, out;
  void init(){
    Ort::AllocatorWithDefaultOptions a;
    for (size_t i=0;i<session.GetInputCount();++i) in.emplace_back(session.GetInputNameAllocated(i,a).get());
    for (size_t i=0;i<session.GetOutputCount();++i) out.emplace_back(session.GetOutputNameAllocated(i,a).get());
  }
};
}  // namespace

struct Dfn3Streaming::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "dfn3-streaming"};
  Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  NamedSession enc, erb, df;

  Stft analysis{960, kHop}, synth{960, kHop};
  Dfn3Features feats;
  ErbFilterBank fb{kBins, kErb, 48000.0f, 2};
  DeepFilterRing dfRing{kDf, kOrder, kLookahead};

  // GRU state buffers, initialized to zeros, carried across hops.
  std::map<std::string, std::vector<float>> state;   // name -> [1*1*256]

  // Rings of the full 481-bin noisy spectrum + ERB mask, keyed by absolute frame, so a
  // ready output frame (t-lookahead) can be assembled. Depth covers the lookahead delay.
  std::deque<std::vector<float>> specRe, specIm;      // each 481
  std::deque<std::array<float,kErb>> masks;
  int pushed = 0;
  float lsnr = 0.0f;

  explicit Impl(const std::string& modelDir){
    std::string d = modelDir;
    if(!d.empty() && d.back()!='/' && d.back()!='\\') d += '/';
    Ort::SessionOptions opts; opts.SetIntraOpNumThreads(1);
    auto load=[&](NamedSession& ns, const std::string& f){
      try { ns.session = Ort::Session(env, widen(d+f).c_str(), opts); }
      catch(const Ort::Exception& e){ throw std::runtime_error("Dfn3Streaming: load "+d+f+": "+e.what()); }
      ns.init();
    };
    load(enc,"enc_streaming.onnx"); load(erb,"erb_dec_streaming.onnx"); load(df,"df_dec_streaming.onnx");
    // zero-init every "*_in" state tensor across the three models
    for(NamedSession* ns : {&enc,&erb,&df})
      for(const std::string& n : ns->in)
        if(n.size()>3 && n.substr(n.size()-3)=="_in" && n.find("_h")!=std::string::npos)
          state[n] = std::vector<float>(static_cast<size_t>(kHidden), 0.0f);
  }

  // helper: run session ns given a name->(ptr,shape) map, return name->owned data (+shape).
  struct Out { std::vector<float> data; std::vector<int64_t> shape; };
  std::map<std::string,Out> run(NamedSession& ns,
      std::map<std::string,std::pair<float*,std::vector<int64_t>>>& ins){
    std::vector<Ort::Value> iv; std::vector<const char*> ip;
    for(const std::string& n : ns.in){
      auto it=ins.find(n);
      if(it==ins.end()) throw std::runtime_error("Dfn3Streaming: missing input "+n);
      size_t c=1; for(int64_t d:it->second.second) c*=static_cast<size_t>(d);
      iv.push_back(Ort::Value::CreateTensor<float>(mem, it->second.first, c,
                   it->second.second.data(), it->second.second.size()));
      ip.push_back(n.c_str());
    }
    std::vector<const char*> op; for(const std::string& n:ns.out) op.push_back(n.c_str());
    auto outs=ns.session.Run(Ort::RunOptions{nullptr}, ip.data(), iv.data(), iv.size(), op.data(), op.size());
    std::map<std::string,Out> r;
    for(size_t i=0;i<outs.size();++i){
      if(!outs[i].IsTensor()) continue;
      auto info=outs[i].GetTensorTypeAndShapeInfo();
      Out o; o.shape=info.GetShape();
      const float* d=outs[i].GetTensorData<float>();
      o.data.assign(d, d+info.GetElementCount());
      r.emplace(ns.out[i], std::move(o));
    }
    return r;
  }
};

Dfn3Streaming::Dfn3Streaming(const std::string& md) : p_(new Impl(md)) {}
Dfn3Streaming::~Dfn3Streaming(){ delete p_; }
uint32_t Dfn3Streaming::latencySamples() const noexcept { return kHop + kLookahead*kHop; }
float Dfn3Streaming::lastLsnr() const noexcept { return p_->lsnr; }

void Dfn3Streaming::reset() noexcept {
  p_->analysis = Stft{960,kHop}; p_->synth = Stft{960,kHop}; p_->feats.reset();
  p_->dfRing = DeepFilterRing{kDf,kOrder,kLookahead};
  for(auto& kv : p_->state) std::fill(kv.second.begin(), kv.second.end(), 0.0f);
  p_->specRe.clear(); p_->specIm.clear(); p_->masks.clear(); p_->pushed=0; p_->lsnr=0.0f;
}

void Dfn3Streaming::process(const float* in480, float* out480) noexcept {
  Impl& s = *p_;
  // 1. STFT analyze current hop -> noisy spectrum
  std::vector<float> re(kBins), im(kBins);
  s.analysis.analyze(in480, re.data(), im.data());
  // 2. features
  std::array<float,kErb> fe; std::array<float,2*kDf> fs;
  s.feats.process(re.data(), im.data(), fe.data(), fs.data());
  // 3. enc (T=1)
  std::vector<float> erbIn(fe.begin(), fe.end());                 // [1,1,1,32]
  std::vector<float> specIn(2*kDf);
  for(int b=0;b<kDf;++b){ specIn[b]=fs[b]; specIn[kDf+b]=fs[kDf+b]; } // [1,2,1,96]
  std::map<std::string,std::pair<float*,std::vector<int64_t>>> ei;
  for(const std::string& n : s.enc.in){
    if(n.find("erb")!=std::string::npos && n.find("_h")==std::string::npos) ei[n]={erbIn.data(),{1,1,1,kErb}};
    else if(n.find("spec")!=std::string::npos) ei[n]={specIn.data(),{1,2,1,kDf}};
    else ei[n]={s.state[n].data(),{1,1,kHidden}};                 // enc_h0_in
  }
  auto eo = s.run(s.enc, ei);
  // carry enc state
  for(auto& kv : eo) if(kv.first.size()>4 && kv.first.substr(kv.first.size()-4)=="_out" && kv.first.find("_h")!=std::string::npos){
    std::string in=kv.first.substr(0,kv.first.size()-4)+"_in"; s.state[in]=kv.second.data; }
  s.lsnr = eo.count("lsnr") ? eo["lsnr"].data.front() : s.lsnr;
  // 4. erb_dec
  std::map<std::string,std::pair<float*,std::vector<int64_t>>> ri;
  for(const std::string& n : s.erb.in){
    if(n.find("_h")!=std::string::npos && n.substr(n.size()-3)=="_in") ri[n]={s.state[n].data(),{1,1,kHidden}};
    else { auto& src=eo.at(n); ri[n]={src.data.data(),src.shape}; }
  }
  auto ro = s.run(s.erb, ri);
  for(auto& kv : ro) if(kv.first.find("_h")!=std::string::npos && kv.first.substr(kv.first.size()-4)=="_out"){
    std::string in=kv.first.substr(0,kv.first.size()-4)+"_in"; s.state[in]=kv.second.data; }
  std::vector<float> mask; for(auto& kv:ro) if(kv.first.find("_h")==std::string::npos) mask=kv.second.data;  // m[1,1,1,32]
  // 5. df_dec
  std::map<std::string,std::pair<float*,std::vector<int64_t>>> di;
  for(const std::string& n : s.df.in){
    if(n.find("_h")!=std::string::npos && n.substr(n.size()-3)=="_in") di[n]={s.state[n].data(),{1,1,kHidden}};
    else { auto& src=eo.at(n); di[n]={src.data.data(),src.shape}; }
  }
  auto dfo = s.run(s.df, di);
  for(auto& kv : dfo) if(kv.first.find("_h")!=std::string::npos && kv.first.substr(kv.first.size()-4)=="_out"){
    std::string in=kv.first.substr(0,kv.first.size()-4)+"_in"; s.state[in]=kv.second.data; }
  const float* coefs=nullptr; for(auto& kv:dfo) if(kv.second.shape.size()==4){ coefs=kv.second.data.data(); break; }

  // 6. push spectrum + mask; deep-filter via ring
  s.specRe.push_back(re); s.specIm.push_back(im);
  std::array<float,kErb> mk{}; for(int b=0;b<kErb;++b) mk[b]=mask.empty()?1.0f:mask[b]; s.masks.push_back(mk);
  int outT; std::array<float,kDf> loRe{}, loIm{};
  bool ready = coefs && s.dfRing.push(re.data(), im.data(), coefs, outT, loRe.data(), loIm.data());
  ++s.pushed;

  // 7. assemble + ISTFT for the ready (delayed) frame, else emit silence during warmup
  if(!ready){ std::fill(out480, out480+kHop, 0.0f); return; }
  // pop the oldest buffered spectrum (== outT); it aligns with the ring output
  std::vector<float> oRe = s.specRe.front(); std::vector<float> oIm = s.specIm.front();
  std::array<float,kErb> m = s.masks.front();
  s.specRe.pop_front(); s.specIm.pop_front(); s.masks.pop_front();
  // low bins: deep-filtered result
  for(int b=0;b<kDf;++b){ oRe[b]=loRe[b]; oIm[b]=loIm[b]; }
  // high bins: ERB mask broadcast (bins >= kDf)
  apply_erb_mask(s.fb, m.data(), oRe.data(), oIm.data());   // masks all bins; we keep >=kDf below
  // note: low bins were just overwritten by the deep filter AFTER apply_erb_mask would clobber them,
  // so re-set low bins from the deep-filtered values:
  for(int b=0;b<kDf;++b){ oRe[b]=loRe[b]; oIm[b]=loIm[b]; }
  s.synth.synthesize(oRe.data(), oIm.data(), out480);
}
}  // namespace vc
```

Implementation note for the engineer: the assemble order must match `dfn3_offline.cpp` — the ERB
mask is applied to a copy of the NOISY spectrum for bins `>= kDf`, and the deep filter supplies bins
`< kDf`. The snippet above applies the mask then re-writes low bins from the deep filter to guarantee
that split; keep that ordering. `specRe/specIm/masks` deques are drained one frame per ready output,
so they stay bounded at `lookahead+1` entries.

- [ ] **Step 3: Add to the CMake target**

Modify `src/dfn3/CMakeLists.txt`:

```cmake
add_library(dfn3 dfn3_offline.cpp dfn3_streaming.cpp)
target_include_directories(dfn3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dfn3 PUBLIC denoise-core onnxruntime)
```

- [ ] **Step 4: Build**

Run: `cmake --build build --config Debug`
Expected: `dfn3` library compiles and links (no test yet — validated end-to-end in Task 6).

- [ ] **Step 5: Commit**

```bash
git add src/dfn3/dfn3_streaming.h src/dfn3/dfn3_streaming.cpp src/dfn3/CMakeLists.txt
git commit -m "feat(dfn3): Dfn3Streaming real-time orchestrator (T=1 per hop, GRU state carried)"
```

---

### Task 6: **GATE B** — streaming C++ matches reference (offline driver)

**Files:**
- Modify: `src/offline-cli/main.cpp`

- [ ] **Step 1: Add a `dfn3-stream` engine branch to offline-cli**

In `src/offline-cli/main.cpp`, add `#include "dfn3_streaming.h"`, a `bool useDfn3Stream=false;`
flag, accept `--engine dfn3-stream`, and before the generic loop handle it like the `dfn3` branch
but driving `Dfn3Streaming` hop-by-hop over 480-sample blocks:

```cpp
} else if (std::strcmp(eng, "dfn3-stream") == 0) {
  useDfn3Stream = true;
}
```

and a processing block (mirroring the `useDfn3` block, placed just after it):

```cpp
if (useDfn3Stream) {
  vc::Dfn3Streaming eng("models/dfn3");
  const size_t pad = eng.latencySamples();
  std::vector<float> inPad = mono;              // `mono` is the extracted ch0 (already defined above)
  inPad.resize(mono.size() + pad, 0.0f);        // flush the delay line with trailing zeros
  std::vector<float> outAll;
  outAll.reserve(inPad.size());
  std::vector<float> hopOut(vc::FRAME_SAMPLES);
  for (size_t off = 0; off + vc::FRAME_SAMPLES <= inPad.size(); off += vc::FRAME_SAMPLES) {
    eng.process(inPad.data() + off, hopOut.data());
    outAll.insert(outAll.end(), hopOut.begin(), hopOut.end());
  }
  vc::WavData outw;
  outw.sampleRate = in.sampleRate;              // `in` is the input vc::WavData read at the top
  outw.channels = 1;
  outw.samples = outAll;
  if (!vc::wav_write(outPath, outw, /*asFloat=*/false)) {
    std::fprintf(stderr, "error: write %s\n", outPath);
    return 1;
  }
  std::printf("engine=dfn3-stream: processed %zu samples -> %s\n", outAll.size(), outPath);
  return 0;
}
```

(Also update the two usage/`--engine` guide strings in `main.cpp` to include `dfn3-stream`.)

- [ ] **Step 2: Build**

Run: `cmake --build build --config Release`
Expected: `offline-cli.exe` builds; ORT dll already copied next to it by the existing post-build rule.

- [ ] **Step 3: Generate validation WAV + reference (if not present)**

Run:
```bash
python tools/dfn3_make_val_wav.py val_in.wav
```
The `deep-filter` reference output (`val_ref.wav`) is produced by the official binary per
`docs/dfn3-validation.md`; reuse the existing one if already committed/available.

- [ ] **Step 4: Run streaming engine + compare to reference AND to offline**

Run:
```bash
build/src/offline-cli/Release/offline-cli.exe --engine dfn3-stream val_in.wav val_stream.wav
build/src/offline-cli/Release/offline-cli.exe --engine dfn3 val_in.wav val_offline.wav
python tools/dfn3_compare.py val_ref.wav val_stream.wav
python tools/dfn3_compare.py val_offline.wav val_stream.wav
```
Expected: `pearson_r_at_best_lag` vs reference **≥ 0.99** (matching the offline pipeline's 0.9946),
and vs offline **≥ 0.999** (same math, only framing/latency differ). `best_lag` reflects the added
streaming latency and is fine — the tool aligns.

- [ ] **Step 5: If Gate B fails, STOP**

r well below 0.99 vs reference but high vs offline ⇒ a shared upstream issue (unlikely; offline is
already validated). Low vs BOTH ⇒ a streaming bug: check state carry (h_out→h_in mapping), the
assemble order (deep filter low bins vs ERB mask high bins), or deque/ring alignment. Fix in Task 5.

- [ ] **Step 6: Commit**

```bash
git add src/offline-cli/main.cpp
git commit -m "feat(offline-cli): --engine dfn3-stream + Gate B validation (r>=0.99 vs reference)"
```

---

## Phase 3 — Live single-engine integration + Gate C

### Task 7: `INoiseFilter` adapter for `Dfn3Streaming`

**Files:**
- Create: `src/denoise-core/dfn3_stream_adapter.h`
- Create: `src/denoise-core/dfn3_stream_adapter.cpp`
- Modify: `src/denoise-core/CMakeLists.txt`

The adapter lets `Dfn3Streaming` drop into `DspChain` and the live relay. `denoise-core` must not
hard-depend on ORT for all consumers, so the adapter lives in its own small unit that links `dfn3`.

- [ ] **Step 1: Write the header**

`src/denoise-core/dfn3_stream_adapter.h`:

```cpp
#pragma once
#include <memory>
#include <string>
#include "inoise_filter.h"
namespace vc {
class Dfn3Streaming;
// Adapts the real-time DFN3 orchestrator to INoiseFilter (480 samples/frame).
class Dfn3StreamAdapter : public INoiseFilter {
public:
  explicit Dfn3StreamAdapter(const std::string& modelDir);   // throws on model load failure
  ~Dfn3StreamAdapter() override;
  void     ProcessFrame(const float* in, float* out) noexcept override;
  float    LastVadProbability() const noexcept override;      // derived from lsnr
  void     SetAggression(float) noexcept override {}          // DFN3 aggression: future work
  uint32_t AlgorithmicDelaySamples() const noexcept override;
private:
  std::unique_ptr<Dfn3Streaming> eng_;
};
}
```

- [ ] **Step 2: Implement**

`src/denoise-core/dfn3_stream_adapter.cpp`:

```cpp
#include "dfn3_stream_adapter.h"
#include <algorithm>
#include "dfn3_streaming.h"
namespace vc {
Dfn3StreamAdapter::Dfn3StreamAdapter(const std::string& md)
    : eng_(std::make_unique<Dfn3Streaming>(md)) {}
Dfn3StreamAdapter::~Dfn3StreamAdapter() = default;
void Dfn3StreamAdapter::ProcessFrame(const float* in, float* out) noexcept { eng_->process(in, out); }
float Dfn3StreamAdapter::LastVadProbability() const noexcept {
  // map lsnr (dB-ish) to [0,1] via a simple logistic; tuning is future work.
  const float x = eng_->lastLsnr();
  return std::clamp(1.0f / (1.0f + std::exp(-0.3f * x)), 0.0f, 1.0f);
}
uint32_t Dfn3StreamAdapter::AlgorithmicDelaySamples() const noexcept { return eng_->latencySamples(); }
}
```

Add `#include <cmath>` for `std::exp`.

- [ ] **Step 3: Wire CMake so denoise-core exposes the adapter and links dfn3**

In `src/denoise-core/CMakeLists.txt`, add `dfn3_stream_adapter.cpp` to the `denoise-core` sources.
Because `denoise-core` is a dependency OF `dfn3`, do not create a cycle: instead put the adapter in a
separate small target. Add:

```cmake
add_library(dfn3-engine dfn3_stream_adapter.cpp)
target_include_directories(dfn3-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dfn3-engine PUBLIC denoise-core dfn3)
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Debug`
Expected: `dfn3-engine` links cleanly (no dependency cycle).

- [ ] **Step 5: Commit**

```bash
git add src/denoise-core/dfn3_stream_adapter.h src/denoise-core/dfn3_stream_adapter.cpp src/denoise-core/CMakeLists.txt
git commit -m "feat(dfn3): INoiseFilter adapter (dfn3-engine target)"
```

---

### Task 8: Live wiring + Gate C (real-time budget)

**Files:**
- Modify: `src/transport-core/config.cpp` (accept `engine="dfn3"`)
- Modify: `src/phase0-app/bridge.cpp` (construct adapter)
- Modify: `src/phase0-app/bridge.h` (include)
- Modify: `src/phase0-app/CMakeLists.txt` (link `dfn3-engine` + copy ORT dll)

- [ ] **Step 1: Accept the `dfn3` engine value in config**

In `src/transport-core/config.cpp`, wherever `engine` is validated/parsed, allow `"dfn3"` (alongside
`"none"`/`"rnnoise"`). If there is an allowlist, extend it; otherwise the string is passed through and
the bridge decides. Update the comment in `config.h:16` to include `| "dfn3"`.

- [ ] **Step 2: Construct the adapter in the bridge**

In `src/phase0-app/bridge.cpp`, add `#include "dfn3_stream_adapter.h"` and extend the engine
construction at lines ~41-42:

```cpp
if      (cfg_.engine == "rnnoise") engine_ = std::make_unique<RnnoiseFilter>();
else if (cfg_.engine == "dfn3")    engine_ = std::make_unique<Dfn3StreamAdapter>("models/dfn3");
else                               engine_ = std::make_unique<PassthroughFilter>();
```

Add the include to `src/phase0-app/bridge.h` next to the other engine includes.

- [ ] **Step 3: Link + copy the ORT dll for phase0-app**

In `src/phase0-app/CMakeLists.txt`, add `dfn3-engine` to `target_link_libraries` and copy the ORT dll
next to the exe (mirror `src/offline-cli/CMakeLists.txt:4-6`):

```cmake
add_custom_command(TARGET phase0-app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_PROPERTY:onnxruntime,IMPORTED_LOCATION>" "$<TARGET_FILE_DIR:phase0-app>")
```

- [ ] **Step 4: Build Release + confirm dll present**

Run: `cmake --build build --config Release`
Expected: `build/src/phase0-app/Release/phase0-app.exe` and `onnxruntime.dll` beside it.

- [ ] **Step 5: GATE C — measure per-hop time + zero dropouts on hardware**

This is a real-hardware run (K688 → CABLE Input) and needs the user. Provide the command and pass
criteria; the user runs it and reports the CSV.

Run (fixed 60 s, DFN3 live, drift + chain on):
```bash
build/src/phase0-app/Release/phase0-app.exe --config drift.toml --seconds 60
```
with `drift.toml` setting `engine = "dfn3"`, `dsp_chain = true`, `drift_compensation = true`.
Then analyze dropouts + occupancy:
```bash
python tools/analyze_drift.py drift.csv
```
Pass criteria: **zero dropouts** over 60 s and Ring B occupancy held flat (as in the RNNoise live
run). If the relay can't keep up (dropouts, occupancy draining), the three ORT `Run` calls per hop
exceed budget on the relay thread — STOP and escalate to the fallback in the spec §4.3 (dedicated
inference thread with a one-frame handoff) before Phase 4.

- [ ] **Step 6: Commit**

```bash
git add src/transport-core/config.cpp src/transport-core/config.h src/phase0-app/bridge.cpp src/phase0-app/bridge.h src/phase0-app/CMakeLists.txt
git commit -m "feat(dfn3): live engine=\"dfn3\" wiring (Gate C: zero dropouts @60s)"
```

---

## Phase 4 — Warm-standby + delay-aligned crossfade

### Task 9: `SwitchableEngine` (dual engine, equal-power crossfade)

**Files:**
- Create: `src/denoise-core/switchable_engine.h`
- Create: `src/denoise-core/switchable_engine.cpp`
- Modify: `src/denoise-core/CMakeLists.txt`
- Test: `tests/denoise-core/test_switchable_engine.cpp`

Holds two `INoiseFilter`s, runs BOTH every frame (warm standby), delay-aligns their outputs by their
`AlgorithmicDelaySamples()`, and equal-power crossfades on switch.

- [ ] **Step 1: Write the header**

`src/denoise-core/switchable_engine.h`:

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include "inoise_filter.h"
namespace vc {
// Runs two engines in parallel (both kept warm), emits the active one delay-aligned to the
// higher-latency engine, and equal-power crossfades over `fadeFrames` frames on switch.
// requestActive() is safe to call from a non-RT thread; ProcessFrame reads it once per frame.
class SwitchableEngine : public INoiseFilter {
public:
  SwitchableEngine(INoiseFilter* a, INoiseFilter* b, int fadeFrames = 15);
  void     ProcessFrame(const float* in, float* out) noexcept override;  // 480 in/out
  float    LastVadProbability() const noexcept override;
  void     SetAggression(float z) noexcept override;
  uint32_t AlgorithmicDelaySamples() const noexcept override;            // == max(a,b)
  void     requestActive(int idx) noexcept { want_.store(idx, std::memory_order_relaxed); }
  int      activeIndex() const noexcept { return cur_; }
private:
  INoiseFilter* eng_[2];
  int   delaySamples_[2];        // per-engine delay lines to align to the max latency
  std::vector<float> dline_[2];  // circular sample delay buffers
  int   dpos_[2] = {0,0};
  int   fadeFrames_, fadeLeft_ = 0, cur_ = 0, from_ = 0;
  std::atomic<int> want_{0};
  float pushDelay(int e, float x) noexcept;   // push sample into delay line e, return delayed sample
};
}
```

- [ ] **Step 2: Write the failing tests**

`tests/denoise-core/test_switchable_engine.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "switchable_engine.h"
using namespace vc;

// Test double: fixed-gain "engine" with configurable reported latency.
struct GainEngine : INoiseFilter {
  float g; uint32_t d;
  GainEngine(float gain, uint32_t delay):g(gain),d(delay){}
  void ProcessFrame(const float* in, float* out) noexcept override { for(int i=0;i<480;++i) out[i]=g*in[i]; }
  float LastVadProbability() const noexcept override { return 0.0f; }
  void SetAggression(float) noexcept override {}
  uint32_t AlgorithmicDelaySamples() const noexcept override { return d; }
};

TEST(Switchable, SteadyStateEqualsActiveEngineDelayAligned){
  GainEngine a(2.0f,0), b(3.0f,960);          // b reports 2 frames extra latency
  SwitchableEngine sw(&a,&b,/*fadeFrames=*/1);
  EXPECT_EQ(sw.AlgorithmicDelaySamples(), 960u);
  std::vector<float> in(480,1.0f), out(480);
  for(int f=0; f<4; ++f) sw.ProcessFrame(in.data(), out.data());   // prime the 960-sample delay line
  for(int i=0;i<480;++i) EXPECT_NEAR(out[i], 2.0f, 1e-4f);         // engine a, gain 2, delay-aligned
}

TEST(Switchable, EqualPowerGainLawOnSwitch){
  // a passes the input through (the "from" stream); b outputs 0 (the "to" stream). With
  // delay 0 on both, the crossfade output equals the equal-power gain cos(theta) each frame,
  // theta stepping 0->pi/2 across fadeFrames=4. This pins the equal-power law to exact values.
  GainEngine a(1.0f,0), b(0.0f,0);
  SwitchableEngine sw(&a,&b,/*fadeFrames=*/4);
  std::vector<float> in(480,1.0f), out(480);
  sw.ProcessFrame(in.data(), out.data());
  EXPECT_NEAR(out[0], 1.0f, 1e-4f);                          // steady on engine 0 before switch
  sw.requestActive(1);
  const float expect[4] = {0.92388f, 0.70711f, 0.38268f, 0.0f};   // cos(0.25,0.5,0.75,1.0 * pi/2)
  for(int f=0; f<4; ++f){
    sw.ProcessFrame(in.data(), out.data());
    for(int i=0;i<480;++i) EXPECT_NEAR(out[i], expect[f], 1e-4f) << "fade frame " << f;
  }
  sw.ProcessFrame(in.data(), out.data());
  for(int i=0;i<480;++i) EXPECT_NEAR(out[i], 0.0f, 1e-4f);   // settled on engine 1
  EXPECT_EQ(sw.activeIndex(), 1);
}
```

**Design note (correlated outputs):** equal-power crossfade is per the spec, but the two engines'
outputs are *correlated* (both approximate the same clean speech), so an equal-power switch can add a
transient ~+3 dB bump. Switches are rare and brief, so this is acceptable; if it proves audible in the
Task 10 live smoke, swap to a linear (equal-gain) law (`out = (1-p)*src + p*target`) and update the
expected values in this test. Keep equal-power unless the live test shows otherwise.

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --config Debug` then
`ctest --test-dir build -C Debug -R Switchable --output-on-failure`
Expected: link/compile FAIL — `SwitchableEngine` not implemented.

- [ ] **Step 4: Implement**

`src/denoise-core/switchable_engine.cpp`:

```cpp
#include "switchable_engine.h"
#include <algorithm>
#include <cmath>
namespace vc {
SwitchableEngine::SwitchableEngine(INoiseFilter* a, INoiseFilter* b, int fadeFrames)
    : eng_{a,b}, fadeFrames_(fadeFrames) {
  const uint32_t da = a->AlgorithmicDelaySamples(), db = b->AlgorithmicDelaySamples();
  const uint32_t mx = std::max(da, db);
  delaySamples_[0] = static_cast<int>(mx - da);
  delaySamples_[1] = static_cast<int>(mx - db);
  for (int e=0;e<2;++e) dline_[e].assign(static_cast<size_t>(delaySamples_[e]) + 1, 0.0f);
}
float SwitchableEngine::pushDelay(int e, float x) noexcept {
  if (delaySamples_[e] == 0) return x;
  float y = dline_[e][static_cast<size_t>(dpos_[e])];
  dline_[e][static_cast<size_t>(dpos_[e])] = x;
  dpos_[e] = (dpos_[e] + 1) % static_cast<int>(dline_[e].size());
  return y;
}
uint32_t SwitchableEngine::AlgorithmicDelaySamples() const noexcept {
  return std::max(eng_[0]->AlgorithmicDelaySamples(), eng_[1]->AlgorithmicDelaySamples());
}
float SwitchableEngine::LastVadProbability() const noexcept { return eng_[cur_]->LastVadProbability(); }
void  SwitchableEngine::SetAggression(float z) noexcept { eng_[0]->SetAggression(z); eng_[1]->SetAggression(z); }

void SwitchableEngine::ProcessFrame(const float* in, float* out) noexcept {
  float o0[480], o1[480];
  eng_[0]->ProcessFrame(in, o0);       // BOTH run every frame -> both stay warm
  eng_[1]->ProcessFrame(in, o1);
  const int want = want_.load(std::memory_order_relaxed);
  if (want != cur_ && fadeLeft_ == 0) { from_ = cur_; cur_ = want; fadeLeft_ = fadeFrames_; }

  for (int i=0;i<480;++i) {
    const float d0 = pushDelay(0, o0[i]);
    const float d1 = pushDelay(1, o1[i]);
    const float target = (cur_==0) ? d0 : d1;
    if (fadeLeft_ > 0) {
      const float src = (from_==0) ? d0 : d1;
      // progress goes 0 -> 1 across the fade (equal power)
      const float p = 1.0f - (static_cast<float>(fadeLeft_) - 1.0f) / static_cast<float>(fadeFrames_);
      const float th = p * 1.5707963f;   // 0..pi/2
      out[i] = std::cos(th)*src + std::sin(th)*target;
    } else {
      out[i] = target;
    }
  }
  if (fadeLeft_ > 0) --fadeLeft_;
}
}
```

- [ ] **Step 5: Add test + source to CMake**

Add `switchable_engine.cpp` to the `denoise-core` sources and
`tests/denoise-core/test_switchable_engine.cpp` to the denoise-core test executable in
`src/denoise-core/CMakeLists.txt` (follow the pattern the other `test_*.cpp` files use).

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build --config Debug` then
`ctest --test-dir build -C Debug -R Switchable --output-on-failure`
Expected: both tests PASS.

- [ ] **Step 7: Commit**

```bash
git add src/denoise-core/switchable_engine.h src/denoise-core/switchable_engine.cpp tests/denoise-core/test_switchable_engine.cpp src/denoise-core/CMakeLists.txt
git commit -m "feat(engine): SwitchableEngine warm-standby + delay-aligned equal-power crossfade"
```

---

### Task 10: Live warm-standby wiring + switch trigger

**Files:**
- Modify: `src/phase0-app/bridge.h` (hold both engines + SwitchableEngine)
- Modify: `src/phase0-app/bridge.cpp` (construct dual engine when configured; expose switch)
- Modify: `src/phase0-app/main.cpp` (minimal stdin key trigger)
- Modify: `src/transport-core/config.{h,cpp}` (warm-standby option)

- [ ] **Step 1: Add a warm-standby config flag**

In `src/transport-core/config.h`, add `bool warmStandby = false;  // run rnnoise+dfn3, live switch`
and parse `warm_standby` in `config.cpp` next to `dsp_chain`.

- [ ] **Step 2: Construct the dual engine in the bridge**

In `bridge.cpp` `start()`, when `cfg_.warmStandby` is set, build both leaf engines and a
`SwitchableEngine` as the `engine_`, keeping ownership of the leaves:

```cpp
if (cfg_.warmStandby) {
  leafA_ = std::make_unique<RnnoiseFilter>();
  leafB_ = std::make_unique<Dfn3StreamAdapter>("models/dfn3");
  auto sw = std::make_unique<SwitchableEngine>(leafA_.get(), leafB_.get());
  switch_ = sw.get();
  engine_ = std::move(sw);
} else if (cfg_.engine == "rnnoise") { /* ... existing ... */ }
```

Add members to `bridge.h`: `std::unique_ptr<INoiseFilter> leafA_, leafB_;` and
`SwitchableEngine* switch_ = nullptr;` plus `#include "switchable_engine.h"`. Add a public
`void request_engine(int idx) noexcept { if (switch_) switch_->requestActive(idx); }`.

- [ ] **Step 3: Minimal live switch trigger (stdin)**

In `src/phase0-app/main.cpp`, when `warm_standby` is on, spawn a small non-RT thread that reads keys:
`1` → `bridge.request_engine(0)` (RNNoise), `2` → `bridge.request_engine(1)` (DFN3). Print the
requested engine. Keep it trivially simple (blocking `std::cin.get()` loop) and joined on shutdown.
This is the minimal switch trigger the spec calls for — not the §8.5 command channel.

- [ ] **Step 4: Build Release**

Run: `cmake --build build --config Release`
Expected: builds; `phase0-app.exe` runs with `warm_standby = true`.

- [ ] **Step 5: Live smoke (user, hardware)**

Run: `build/src/phase0-app/Release/phase0-app.exe --config drift.toml --seconds 120` with
`warm_standby = true`, `dsp_chain = true`. Press `1`/`2` mid-run to switch engines. Pass criteria:
audible switch with no click/gap, zero dropouts (check `drift.csv` via `analyze_drift.py`).

- [ ] **Step 6: Commit**

```bash
git add src/phase0-app/bridge.h src/phase0-app/bridge.cpp src/phase0-app/main.cpp src/transport-core/config.h src/transport-core/config.cpp
git commit -m "feat(dfn3): live warm-standby + runtime RNNoise<->DFN3 switch"
```

---

### Task 11: Docs + memory update, finish

**Files:**
- Modify: `docs/STATUS.md`, `docs/dfn3-progress.md`
- Modify: memory `voicecleaner-phase0.md`, `dfn3-integration.md` (via the memory system)

- [ ] **Step 1: Update STATUS + progress docs**

Move DFN3 from "offline-only" to "live streaming engine" in `docs/STATUS.md`: record Gate A/B/C
results (the actual r and dropout numbers), the ~30 ms added latency, and the warm-standby switch.
Note the streaming models are committed in `models/dfn3/*_streaming.onnx`.

- [ ] **Step 2: Update memory**

Update `dfn3-integration.md` ("Remaining: LIVE/real-time DFN3" → done, with the graph-surgery
approach + Gate results) and `voicecleaner-phase0.md` (DFN3 now a live engine; warm-standby switch).

- [ ] **Step 3: Full test sweep**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: all tests pass (the prior 55 + the new streaming deep-filter + switchable-engine tests).

- [ ] **Step 4: Commit**

```bash
git add docs/STATUS.md docs/dfn3-progress.md
git commit -m "docs(dfn3): live streaming engine done (Gate A/B/C results, warm-standby)"
```

---

## Self-review notes (author)

- **Spec coverage:** §4.1 graph surgery → Tasks 1-3; §4.2 orchestrator → Tasks 4-6 (streaming deep
  filter split out as Task 4); §4.3 live integration + Gate C → Tasks 7-8; §4.4 warm-standby
  crossfade → Tasks 9-10; §6 testing → Gate A (Task 3), Gate B (Task 6), unit tests (Tasks 4, 9),
  Gate C (Task 8); docs → Task 11. All spec sections map to tasks.
- **Model decision:** `*_streaming.onnx` are **committed** (Task 1 Step 4), no Python in the C++ build.
- **Type consistency:** `Dfn3Streaming::process/latencySamples/lastLsnr/reset`, `DeepFilterRing::push/flush`,
  `Dfn3StreamAdapter`, `SwitchableEngine::requestActive/activeIndex/ProcessFrame` are used consistently
  across tasks. State I/O names (`enc_h0_in/out`, `erb_h{0,1}_*`, `df_h{0,1}_*`) fixed in Task 1 and
  consumed in Task 5.
- **Known iteration points (flagged in-task):** exact ORT output ordering (Task 5 uses name-based
  lookup, robust to order); the lsnr→VAD mapping is a placeholder (Task 7) and explicitly future work;
  Gate C may force the inference-thread fallback (spec §4.3).
```
