# DeepFilterNet3 research scripts

Python harnesses from the DFN3 integration work (model export, streaming-split validation,
quality scoring). Nothing here is built or shipped.

- `dfn3_make_streaming.py` produces `models/dfn3/*_streaming.onnx` from the whole-utterance
  `enc.onnx` / `erb_dec.onnx` / `df_dec.onnx` in the same folder (model provenance — keep).
- `dfn3_stream_check.py` validates that split against the whole-utterance graphs ("Gate A").
- The rest generate test clips and score/compare engine output; they drive the developer
  `offline-cli` (configure with `-DVC_BUILD_DEV_TOOLS=ON`).

History: the causal low-latency `models/dfn3-ll` set and the `offline-cli` engines
`dfn3-ll` / `dfn3-stream` / `dfn3-std-stream` were removed in v1.5 (the live app only ever used
`models/dfn3`; `offline-cli --engine dfn3` now runs the same streaming engine as the app).
`dfn3_realtest.py` still lists clip names from those runs.
