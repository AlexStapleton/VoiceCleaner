# Audit — live DeepFilterNet3 + warm-standby (2026-08-14)

_Critical self-review of the work merged to `master` (live DFN3 `_ll` engine + `SwitchableEngine`
warm-standby). Findings verified by fresh-eyes code read; the two most consequential were confirmed
against the source (noted "✓ verified")._

## Headline

The work is engineered carefully for **RT-safety** and **internal self-consistency**, and the three
gates each prove something real:

- **Gate A** — the streaming ONNX surgery reproduces the whole-utterance model frame-by-frame (1.2e-5).
- **Gate B** — the C++ streaming orchestrator reproduces our own whole-utterance `_ll` pipeline (r=1.0).
- **Gate C** — inference fits the 10 ms real-time budget with large headroom (mean ~1.56/2.07 ms, 0% over).

But the validation is **circular where it matters most**: Gate B runs the *same* `Dfn3Offline` math
with the *same* `dfLeftPad=4` on both sides, so it is **blind to a systematic error** in the `_ll`
pipeline. The shipped `_ll` model has **no external ground truth**, and the actual **denoising
quality** — the entire point of the feature — has **never been measured**. We have proven "the math is
self-consistent," not "the output is correct," and not "the output is good."

**The single most important next step is to validate that live DFN3 actually removes keyboard noise.**

> **Update (2026-08-14, applied on master `678b22f`):** the verified code defects are **FIXED** —
> ORT-failure resync + surfacing (2.1), `requestActive` bounds check (2.3), crossfade endpoint (2.4),
> explicit/documented VAD mapping (3.2), `reset()` non-RT doc (2.2). **Still open** (their own efforts,
> not code fixes): **P1 correctness/quality validation** (1.1–1.3 — the headline gap), **aggression
> wire-or-delete** (3.1), the **A/B dry tap** (3.3), and the hygiene items (P4/P5).

## What is actually proven vs. assumed

| Claim | Status |
|---|---|
| Streaming mechanism faithfully reproduces the `_ll` graphs | **Proven** (Gate A) |
| C++ orchestrator reproduces whole-utterance `_ll` | **Proven** (Gate B) — but only self-vs-self |
| Fits the RT budget on this hardware | **Proven** (Gate C, live) |
| The `_ll` **offline** pipeline is itself correct | **Assumed** — offset `leftPad=4` is formula-derived; only the *standard* model's `2` was ever checked externally (r=0.9946) |
| Live output actually suppresses (keyboard) noise well | **Unmeasured** — only tested on a synthetic sine+white-noise clip |
| Warm-standby crossfade sounds clean | **Unverified by ear** |

---

## P1 — Correctness & quality validation (HIGH)

**1.1 The shipped `_ll` model has no external ground truth.** The only authoritative check
(r=0.9946 vs the official `deep-filter` binary, `docs/dfn3-validation.md`) is for the **standard**
model `models/dfn3`, which is **not what ships live** (`bridge.cpp:45` loads `models/dfn3-ll`). The
`deep-filter` 0.5.6 binary bundles the standard weights, so it cannot sample-match `_ll`.
→ Validate `_ll` against DeepFilterNet's Python with the `_ll` checkpoint, or exported golden `.npz`
for a known clip.

**1.2 The `_ll` deep-filter offset `leftPad=4` is formula-derived, never ground-truthed**
(`dfn3_streaming.cpp:52`, `offline-cli/main.cpp:108`). Because Gate B uses the same offset on both
sides, an off-by-one here passes every gate in the repo. This is the highest-leverage single number
in the pipeline. → Ground-truth it against a `_ll` reference (even a directional check).

**1.3 Denoising quality has never been measured, and the test signal is wrong for the use case.**
The one validation clip (`tools/dfn3/dfn3_make_val_wav.py`) is synthetic sines + **white** noise; DFN3's
justification is **impulsive/keyboard** noise (non-stationary), which white noise doesn't represent.
`r` is *similarity to a reference*, not an SNR/noise-removal metric. → Build a small real corpus
(real speech + real keyboard/fan/babble), measure segmental-SNR improvement, and listen.

## P2 — Error handling / RT correctness

**2.1 (✓ verified) ORT-failure path is a silent, unrecoverable desync. [MED→HIGH for observability]**
`Dfn3Streaming::process` catches all exceptions, silences the hop, bumps `failedHops`
(`dfn3_streaming.cpp:467-474`). But (a) `lastHopFailed()`/`failedHops` are **never read** anywhere
(grep: defined, never consumed in `bridge.cpp`/`adapter`/`main.cpp`) — a failing engine emits
continuous silence with zero logging; and (b) `WindowRunner::run` advances the window **and** GRU
state (`dfn3_streaming.cpp:189-200`) *before* `runInference` (line 220), so a mid-stream throw leaves
window vs GRU **permanently desynced** — the code comment itself admits "State carried into the next
hop may be stale." → Surface `failedHops` (log first failure; add to the frame-time summary / dropout
telemetry) and reset/resync the engine on failure instead of limping on.

**2.2 `Dfn3Streaming::reset()` is not RT-safe (allocates).** Reconstructs `DeepFilterRing` and forces
an ORT re-bind (`dfn3_streaming.cpp:405`, `WindowRunner::reset`). Safe **today** (only called once in
the adapter ctor, off-thread), but Phase-1 device-reconnect will be tempted to call it on the audio
path. → Document as non-RT, or make it allocation-free.

**2.3 (✓ verified) `SwitchableEngine::requestActive(idx)` has no bounds check → OOB deref on a bad
index.** `switchable_engine.h:17` stores `idx` verbatim; `eng_[cur_]` is dereferenced later. Safe by
convention (callers only pass 0/1), not by construction. → Clamp to `{0,1}`.

**2.4 Crossfade never emits the identity endpoint → tiny amplitude step at fade start.** `p` starts
at `1/fadeFrames` not `0` (`switchable_engine.cpp:40`); the first fade frame steps source gain from
1.0 to ~0.9945. ~0.5% at the default 15 frames (likely inaudible), but a genuine off-by-one that the
unit test bakes in as "correct" (`test_switchable_engine.cpp:36`). → Step `p` over `0…1` inclusive.

## P3 — Incomplete / placeholder features

**3.1 (✓ verified) The `aggression` module is fully built + unit-tested but wired to NOTHING.**
`apply_aggression`/`aggression_gamma` (`aggression.cpp`) have no non-test caller; every engine's
`SetAggression` is a no-op; there's no `aggression` field in `Config` reaching an engine. → Decide:
wire `apply_aggression` into the gain path (RNNoise band gains / DFN3 ERB mask) + add the config knob,
or delete the module and its no-op interface method.

**3.2 DFN3's VAD probability is an untuned guess, and AGC behavior depends on it.**
`Dfn3StreamAdapter::LastVadProbability` = `1/(1+exp(-0.3·lsnr))` (`dfn3_stream_adapter.cpp:31-36`,
self-described as a rough default). `DspChain` feeds this to the VAD-gated AGC (`dsp_chain.cpp:19`,
thresholds 0.6/0.3). So AGC gating silently differs between RNNoise (a real probability) and DFN3 (a
fabricated mapping). → Calibrate the logistic so the speech/noise `lsnr` boundary maps to ~0.5;
document the operating point.

**3.3 No A/B path for judging output.** Delay-matched bypass + dry/wet mix (§7.2), VAD pre-roll
(§7.3), dry tap, and the input level meter are all unbuilt (on the Phase-3 roadmap). This is *why*
1.3 is hard to answer even manually — you can't compare processed vs raw. → A delay-matched dry tap is
the cheapest enabler for the quality validation.

## P4 — Optimization / hygiene

**4.1 (strategic) Spend the ~8 ms RT headroom on QUALITY, not micro-opts.** The *validated* model
(`dfn3`, r=0.9946) is not shipped; the *shipped* model (`_ll`, chosen for latency) is unvalidated.
With 8 ms of headroom, the latency argument for `_ll` deserves re-examination — the higher-quality
standard model (or a post-filter) is now affordable. Caveat: standard-model streaming surgery
**previously failed Gate A** (`dfn3-progress.md`), so it's non-trivial; but it's the right
conversation given the numbers.

**4.2 Repo binary bloat: ~87 MB of ONNX tracked, ~39 MB runtime-dead.** The `_ll` **non-streaming**
originals (enc/erb_dec/df_dec ≈ 39 MB) are used only as the Gate B offline golden; live loads only the
`*_streaming.onnx`. The standard `models/dfn3` (8.6 MB) is only the offline r=0.9946 reference. → Move
golden-only/reference models to Git LFS or fetch-on-demand; keep only `*_streaming.onnx` in-tree.

**4.3 Duplicated ORT plumbing.** `widen`/`OwnedTensor`/`NamedSession`/session-run are defined twice
(`dfn3_offline.cpp:35-58` and `dfn3_streaming.cpp:55-88`). → Shared `dfn3_ort_util.h` (~40 lines).

## P5 — Test coverage

**5.1 Everything hinges on one synthetic wav; no real speech/keyboard, no regression corpus**
(blueprint §11). See 1.3.

**5.2 `SwitchableEngine`: only 2 tests.** Untested: mid-crossfade switch, out-of-range index (2.3),
delay-line wraparound over long runs, both-engines-identical passthrough, back-to-back switches. The
endpoint bug (2.4) is *encoded as correct* in the existing test.

**5.3 No test for `Dfn3StreamAdapter` or `Dfn3Streaming`.** Untested: adapter warm-up actually fills
windows / leaves OrtValues persistent, `lsnr`→VAD mapping, the ORT-failure/silence path (2.1). Like
Gate B, `DeepFilterRing`'s self-consistency test can't catch a wrong `leftPad`.

---

## Recommended order

1. **Prove the output is correct and actually denoises** (1.1, 1.2, 1.3 + 3.3 dry tap to enable A/B).
   This gates whether shipping DFN3 is even worth it — everything else is secondary.
2. **Surface / recover the ORT-failure path** (2.1) — today it's a silent, unrecoverable desync.
3. **Resolve aggression** (3.1, dead code) and **calibrate the DFN3 VAD** (3.2, silently skews AGC).
4. Cheap wins: `requestActive` clamp (2.3), crossfade endpoint (2.4), models → LFS (4.2).
5. Revisit the strategic model choice (4.1) once quality is measured.

## Meta-lesson

The gated, subagent-driven process was strong at proving **faithfulness of a transformation**
(surgery == model, streaming == offline, fits budget) but weak at proving **correctness of the base
and value of the result**. A validation chain is only as good as its ground truth: several gates here
compare the system to *itself*. Future engine work should pin an **external** reference and a
**quality metric** before declaring done.
