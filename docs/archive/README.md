# Archive

Material kept for the record. Nothing here is built, tested or shipped.

- `dfn3/` — DeepFilterNet3 evaluation reports (validation against the official binary, streaming
  gate results, the real-recording test, the quality report and the competitive investigation).
- `dfn3-tools/` — the Python scripts used for those evaluations (validation WAV generation, the
  streaming model export and gate check, the real-recording harness, quality metrics, gain staging).

Why: DeepFilterNet3 was removed in v1.6, after evaluation showed RNNoise was the better engine for this
use. It lets keyboard clicks through during speech, and the Dual mode built to combine the two was
shelved after a listening test (see `docs/superpowers/notes/2026-10-12-dual-engine-eval.md`).

The scripts reference files and engines that no longer exist (`models/dfn3/`, the `offline-cli
--engine dfn3` path, `src/dfn3/`). They will not run against the current tree without restoring that
code from git history.
