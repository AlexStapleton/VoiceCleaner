# CPU inference and DSP optimization for VoiceCleaner (10 ms hop, Windows x64, MSVC)

Research date: 2026-10-11. Note on method: onnxruntime.ai, learn.microsoft.com and arxiv.org were not reachable from the research sandbox through WebFetch (DNS failure), so ONNX Runtime and MSVC documentation was read from the docs' source markdown on GitHub (`microsoft/onnxruntime` gh-pages branch, `MicrosoftDocs/cpp-docs`). The published pages render from those files. Several RNNoise findings come from reading the vendored source in `third_party/rnnoise` (upstream: https://github.com/xiph/rnnoise). Those are cited as file paths plus the upstream repo URL. Every number is labelled **[vendor-claimed]**, **[paper-claimed]**, **[third-party]** or **[measured]**.

## Q1. RNNoise: enabling x86 RTCD, expected speedup, bit-exactness, MSVC issues

### Takeaway
The current VoiceCleaner RNNoise build is slower than it needs to be, for three reasons. (1) It never defines `DISABLE_DEBUG_FLOAT`, so it runs the float-weight "debug" matrices instead of the int8 weights the upstream build uses. (2) On MSVC, `__SSE2__` is never defined, so `vec.h` falls back to the pure-C `NO_OPTIMIZATIONS` kernels. (3) The SSE4.1/AVX2 RTCD files are not built. The Linux g++ measurements are therefore not representative of the MSVC build, because GCC x64 defines `__SSE2__` and picks up the `vec_avx.h` SSE2 path. Enabling RTCD on MSVC is a small CMake change: add defines and compile one file with `/arch:AVX2`. Output will not be bit-exact against the current build: the int8 vs float weights differ, and so do the SIMD activation approximations.

### Cited Findings
- The upstream README recommends: "either set -march= in the CFLAGS to an architecture with AVX2 support or to add --enable-x86-rtcd to the configure script so that AVX2 (or SSE4.1) can at least be used as an option." — [xiph/rnnoise README](https://github.com/xiph/rnnoise) (vendored copy `third_party/rnnoise/README`, lines 19-21)
- `configure.ac`: `--enable-x86-rtcd` defines `RNN_ENABLE_X86_RTCD=1` and `CPU_INFO_BY_ASM=1`. The per-file SIMD flags are `OPUS_X86_SSE4_1_CFLAGS='-msse4.1'` and `OPUS_X86_AVX2_CFLAGS='-mavx -mfma -mavx2'`. — [xiph/rnnoise configure.ac](https://github.com/xiph/rnnoise/blob/main/configure.ac) (vendored lines 89-101)
- `Makefile.am` adds `src/x86/nnet_sse4_1.c` (SSE4.1 flags), `src/x86/nnet_avx2.c` (AVX2 flags) and the RTCD glue `src/x86/x86_dnn_map.c` + `src/x86/x86cpu.c`. — [xiph/rnnoise Makefile.am](https://github.com/xiph/rnnoise/blob/main/Makefile.am) (vendored lines 42-46)
- RTCD dispatch is a 3-entry function-pointer table: `[0]=compute_*_c`, `[1]=*_sse4_1`, `[2]=*_avx2`, applied to `compute_linear`, `compute_activation` and `compute_conv2d` (`OPUS_ARCHMASK 3`, comment "arch[0] -> sse2, arch[1] -> sse4.1, arch[2] -> avx2"). — vendored `src/cpu_support.h`, `src/x86/x86_dnn_map.c`, [xiph/rnnoise src/x86](https://github.com/xiph/rnnoise/tree/main/src/x86)
- MSVC support already exists in the RTCD code. `x86cpu.c` uses `__cpuid` from `<intrin.h>` under `_MSC_VER`, so `CPU_INFO_BY_ASM` is not needed on MSVC. `x86_arch_macros.h` maps `OPUS_X86_MAY_HAVE_SSE`/`SSE2`/`SSE4_1` to `__SSE__`/`__SSE2__`/`__SSE4_1__` under `_MSC_VER`, because MSVC does not define those GCC macros. — vendored `src/x86/x86cpu.c` lines 38-44, `src/x86/x86_arch_macros.h` lines 27-47
- `nnet_avx2.c` contains `#ifndef __AVX2__ #error nnet_avx2.c is being compiled without AVX2 enabled`, and `nnet_sse4_1.c` has the same guard for `__SSE4_1__`. — vendored `src/x86/nnet_avx2.c` line 34, `src/x86/nnet_sse4_1.c` line 34
- `vec.h` uses the SIMD kernels only `#if defined(__AVX__) || defined(__SSE2__)`, and otherwise falls to `#define NO_OPTIMIZATIONS` scalar C. — vendored `src/vec.h` lines 39-47
- `nnet.h` emits `#pragma message ("Only SSE and SSE2 are available. On newer machines, enable SSSE3/AVX/AVX2 to get better performance")` on MSVC when neither RTCD nor `__AVX2__` is enabled. The current build should already show this in its logs. — vendored `src/nnet.h` lines 159-165
- Upstream configure defines `DISABLE_DEBUG_FLOAT` by default (unless `--enable-dnn-debug-float` is passed, "Use floating-point DNN computation everywhere"). The generated `rnnoise_data.c` wraps the float copies of the weights in `#ifndef DISABLE_DEBUG_FLOAT`. — vendored `configure.ac` lines 81-87, `src/rnnoise_data.c`
- `compute_linear` prefers `float_weights` when present (`sgemv`/`sparse_sgemv8x4`) and only uses int8 `cgemv8x4` with SU-bias otherwise. The GRU1 input matrix alone is `gru1_input_weights_float[442368]` (~1.77 MB as float32) vs `gru1_input_weights_int8[442368]` (~442 KB). — vendored `src/nnet_arch.h` lines 135-150, `src/rnnoise_data.c` lines 40229, 95536
- The VoiceCleaner CMake builds rnnoise "Scalar (C) build only: no x86 RTCD/SIMD variants, no autotools config.h" and sets no `DISABLE_DEBUG_FLOAT`. — `third_party/rnnoise/CMakeLists.txt`
- The SIMD path (`vec_avx.h`) uses its own polynomial/rational approximations for `tanh_approx`, `sigmoid_approx` and `lpcnet_exp`, and FMA (`_mm256_fmadd_ps`, emulated as mul+add on non-FMA builds). This is different arithmetic from the scalar `vec.h` path. — vendored `src/vec_avx.h` lines 299-460
- RNNoise 0.2 (April 2024) added SSE4.1/AVX2 paths with RTCD. Phoronix said the author planned benchmarks, but none were published in that coverage. — [Phoronix: RNNoise 0.2 Released With AVX2 Optimizations](https://www.phoronix.com/news/RNNoise-0.2-Released)

### Inferences
- Concrete MSVC CMake recipe, inferred from the sources above:
  - Add `src/x86/x86cpu.c`, `src/x86/x86_dnn_map.c`, `src/x86/nnet_sse4_1.c` and `src/x86/nnet_avx2.c`.
  - Define `RNN_ENABLE_X86_RTCD`, `OPUS_X86_MAY_HAVE_SSE`, `OPUS_X86_MAY_HAVE_SSE2`, `OPUS_X86_MAY_HAVE_SSE4_1` and `DISABLE_DEBUG_FLOAT` target-wide.
  - Set per-source `COMPILE_OPTIONS /arch:AVX2` on `nnet_avx2.c` only. MSVC defines `__AVX2__` under `/arch:AVX2`; MSVC x64 allows SSE4.1 intrinsics without an `/arch` flag, and the macro shim supplies `__SSE4_1__`.
  - Do not add `CPU_INFO_BY_ASM`; the MSVC branch uses `__cpuid`.
  - Defining `OPUS_X86_MAY_HAVE_SSE2` alone (always safe on x64) already moves the non-dispatched code (`vec.h`) from pure-C to the SSE2 kernels. This is probably the single biggest win available for MSVC.
- Defining `DISABLE_DEBUG_FLOAT` cuts weight memory traffic about 4x (int8 vs float32) and matches the reference upstream behaviour, i.e. what the model was validated with. The output will change slightly, so regression tests need tolerance-based comparison, not bit-exact goldens.
- Bit-exactness: the scalar, SSE4.1 and AVX2 builds should be expected to differ at the LSB level. They use different approximations and FMA, and the int8 path quantizes activations (`SCALE 128*127`). The differences should be perceptually negligible, but no xiph statement quantifying this was found.
- Expected speedup: no xiph-published SSE4.1/AVX2 vs scalar numbers were found. From the code structure (8-wide FMA int8/float GEMV) an order-of-magnitude-ish gain over the pure-C MSVC path is plausible, but this is unverified. **The team should measure it on MSVC.**

### Gaps
- No official or measured xiph benchmark of scalar vs SSE4.1 vs AVX2 for RNNoise 0.2 was found.
- No xiph statement on numeric equivalence between arch variants was found.
- Whether a pure `/arch:AVX2` whole-library build (no RTCD) is acceptable depends on the minimum-CPU policy; this is not researched.

## Q2. ONNX Runtime configuration for real-time single-stream CPU inference

### Takeaway
VoiceCleaner already uses the right core settings: CPU EP, 1 intra-op thread, sequential execution, and bound reused OrtValues. The remaining levers are:
- Make sure `ORT_ENABLE_ALL` is in effect (it is the default), and pre-optimize offline to cut startup.
- Use a single shared `Ort::Env`, optionally with global thread pools and shared allocators, instead of one Env per engine.
- Handle denormals on the audio thread.
- Treat spinning knobs as irrelevant at 1 thread.

Int8 dynamic quantization is the documented recommendation for RNN models. GPU/NPU EPs (DirectML, QNN, OpenVINO, WinML) are unlikely to beat a single CPU thread for a model this small with a 10 ms deadline.

### Cited Findings
- The CPU EP defaults are `intra_op_num_threads = 0` (= number of physical cores, with affinitization), `ORT_SEQUENTIAL`, `ORT_ENABLE_ALL`, and `session.intra_op.allow_spinning = 1`. — [ORT docs: Thread management](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/tune-performance/threading.md) (published at https://onnxruntime.ai/docs/performance/tune-performance/threading.html)
- With an explicit intra-op thread count N, the calling thread plus N-1 pool threads participate, and no affinity is set. — [ORT docs: Thread management](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/tune-performance/threading.md)
- Spinning "Controls whether additional INTRA or INTER threads spin waiting for work. Provides faster inference but consumes more CPU cycles". Newer keys include `session.intra_op.spin_duration_us` (time-bounded spin) and `session.intra_op.spin_backoff_max`. In ORT's benchmarks (PR #28096), "spin_duration_us=1000 combined with spin_backoff_max=8 was the most consistent best performer" **[vendor-claimed]**. — [ORT docs: Thread management](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/tune-performance/threading.md); [onnxruntime PR #28096](https://github.com/microsoft/onnxruntime/pull/28096)
- The `allow_spinning` default is "0" when ORT is built with `ORT_CLIENT_PACKAGE_BUILD` and "1" otherwise: "Thread spinning is disabled by default for client/on-device workloads to reduce cpu utilization and improve power efficiency." — [onnxruntime_session_options_config_keys.h](https://github.com/microsoft/onnxruntime/blob/main/include/onnxruntime/core/session/onnxruntime_session_options_config_keys.h)
- `ORT_PARALLEL` plus `inter_op_num_threads` only helps "when a model has many branches" and "could also hurt performance on some models without many branches". — [ORT docs: Thread management](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/tune-performance/threading.md)
- Global thread pools: create the env with `CreateEnvWithGlobalThreadPools()` and call `DisablePerSessionThreads()` on each session's options. This avoids several thread pools when "several sessions need to be created ... in the same process". — [ORT docs: Get started with C/C++](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/get-started/with-c.md)
- Shared allocators: `CreateAndRegisterAllocator` on the env, plus `session.use_env_allocators = "1"` per session. Each session otherwise has its own BFC arena that "is never returned to the system". `OrtArenaCfg` is created via `CreateArenaCfgV2`. — [ORT docs: Get started with C/C++](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/get-started/with-c.md); [config keys header](https://github.com/microsoft/onnxruntime/blob/main/include/onnxruntime/core/session/onnxruntime_session_options_config_keys.h)
- `session.dynamic_block_base` (e.g. "4") "For some models ... helps to reduce the variance of E2E inference latency". It only matters with multiple threads (available since 1.11). — [config keys header](https://github.com/microsoft/onnxruntime/blob/main/include/onnxruntime/core/session/onnxruntime_session_options_config_keys.h)
- `session.set_denormal_as_zero = "1"` applies FTZ+DAZ. "When multiple sessions are created, a main thread doesn't override changes from succeeding session options, but threads in session thread pools follow option changes." The header also notes the option "may hurt model accuracy" and recommends exporting models without denormals instead. — [config keys header](https://github.com/microsoft/onnxruntime/blob/main/include/onnxruntime/core/session/onnxruntime_session_options_config_keys.h)
- On Windows, ORT can be built with `--use_mimalloc`, which "can deliver single- or double-digit improvements in performance" **[vendor-claimed]**. — [ORT docs: Memory](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/tune-performance/memory.md)
- Graph optimization levels:
  - Basic: constant folding and redundant node elimination, for all EPs.
  - Extended: fusions, for CPU/CUDA/ROCm.
  - Layout (ALL): the NCHWc optimizer, CPU only.
  
  "All optimizations are enabled by default." Offline mode (`SetOptimizedModelFilePath` / `session.optimized_model_filepath`) serializes the optimized graph to reduce startup. A layout-optimized offline model "for AVX2 ... would require CPUs that support AVX2", so save offline at `ORT_ENABLE_EXTENDED` for portability. An ORT-format save is available via `session.save_model_format = "ORT"`. — [ORT docs: Graph optimizations](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/model-optimizations/graph-optimizations.md); [config keys header](https://github.com/microsoft/onnxruntime/blob/main/include/onnxruntime/core/session/onnxruntime_session_options_config_keys.h)
- Quantization:
  - "it is recommended to use dynamic quantization for RNNs and transformer-based models, and static quantization for CNN models."
  - Dynamic quantization computes activation scales on the fly, which costs extra compute but is usually more accurate.
  - S8S8 with QDQ is the default and the first choice. "S8S8 with QOperator will be slow on x86-64 CPUs."
  - On AVX2/AVX512 without VNNI, U8S8 uses VPMADDUBSW and may saturate. If accuracy drops, use `reduce_range` or U8U8.
  
  — [ORT docs: Quantization](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/model-optimizations/quantization.md)
- DirectML "is in sustained engineering ... new feature development has moved to WinML". WinML "dynamically select[s] the best execution provider". Windows 11 24H2+ adds automation for EP selection; earlier versions require manual EP management. — [ORT docs: DirectML EP](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/execution-providers/DirectML-ExecutionProvider.md); [ORT docs: Windows / WinML](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/get-started/with-windows.md)
- The QNN EP targets Qualcomm Snapdragon (including Snapdragon X on Windows ARM64), and its `htp` backend "Offloads compute to NPU". — [ORT docs: QNN EP](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/execution-providers/QNN-ExecutionProvider.md)
- Third-party opinion: blocking (non-spinning) ORT threads wake with "microsecond-scale latency". This is a single project's claim **[third-party]**. — [headroom commit 5c561bd](https://upd.dev/chopratejas/headroom/commit/5c561bd)

### Inferences
- With `SetIntraOpNumThreads(1)` there are no pool threads, so the `allow_spinning`, `spin_duration_us`, `dynamic_block_base` and affinity knobs have no effect. The only spin/wake jitter source is gone. Keep 1 thread: for a 10 ms deadline on a small model, extra threads add wake-up jitter more than they save compute.
- Four sessions, each with per-session thread pools and arenas, plus one `Ort::Env` per engine instance, is wasteful in memory and creation time but mostly harmless for latency at 1 thread. One process-wide Env with `DisablePerSessionThreads()` and env-registered allocators is cleaner. Add an `OrtArenaCfg` with a fixed initial chunk so that no arena extension happens on the audio path.
- Denormals: GRU states decaying toward zero in silence are a classic cause of p99 spikes. Prefer setting FTZ/DAZ on the audio thread itself (`_MM_SET_FLUSH_ZERO_MODE` / `_MM_SET_DENORMALS_ZERO_MODE`, or `_controlfp_s`) before calling `Run()`. With 1 intra-op thread, ORT computes on the calling thread, and the header's wording about the "main thread" is ambiguous. FTZ/DAZ also helps the DSP and RNNoise code.
- For p99, warm up each session with a few dummy `Run()` calls at init so that arena growth and prepacking happen off the audio thread. Keep `Run()` off the real-time audio callback (use a worker thread with a lock-free queue) if the host's buffer cadence is not 10 ms.
- Int8 dynamic quantization of DFN3: the GRU/Linear-heavy parts (`MatMul`/`Gemm`) are the documented candidates. Quantizing convs statically needs calibration, and DFN3's tiny conv layers may not gain much. Expect weight memory around 4x smaller. The speedup on 1 thread is model-dependent. Quality must be checked (PESQ/DNSMOS) because DFN outputs a mask/filter coefficients that are sensitive to error.
- FP16 on the CPU EP is generally not faster on x86 (no native FP16 math in most consumer CPUs). It is relevant only for GPU/NPU EPs.
- GPU/NPU EPs (DirectML, QNN, OpenVINO-GPU/NPU) add per-call submission and synchronization overhead and transfers. For four tiny graphs per 10 ms hop, that overhead likely dominates and adds jitter; they suit batch/offline use better. OpenVINO's CPU device might be competitive but is untested here.

### Gaps
- No primary-source per-hop latency numbers for DirectML, QNN, OpenVINO or WinML on DFN-sized models were found.
- No published measurement of int8 DFN3 quality or speed in ORT was found.
- The exact semantics of `set_denormal_as_zero` for the calling thread with 1 intra-op thread is not confirmed.

## Q3. Streaming-friendly DFN3 (avoiding W-fold window recompute)

### Takeaway
The reference real-time DeepFilterNet implementation (libDF, Rust/tract) does not recompute windows. It loads the same three ONNX graphs (enc, erb_dec, df_dec) with a symbolic time axis `S` and uses tract-pulse to convert them into stateful 1-frame "pulsed" models. Tract inserts the conv delay-line state automatically. VoiceCleaner's rolling-window approach (W = 6/1/3/5 frames) does about W times the conv work per hop. The GRUs are also re-run over W frames: the GRU state is either re-derived from scratch or the windowed output only approximates true streaming. A stateful export would remove most of that redundant work.

### Cited Findings
- libDF `tract.rs`: each of the encoder, ERB decoder and DF decoder is loaded with input facts shaped `(n_ch, 1, S, nb_erb)` / `(n_ch, 2, S, nb_df)` where `S = m.symbol_table.sym("S")`. Each is then `declutter()`-ed and converted with `PulsedModel::new(&m, s, &1.to_dim())` and `into_typed()?.into_optimized()`, and run via `SimpleState` (stateful). Encoder outputs are named `e0, e1, e2, e3, emb, c0, lsnr`. — [Rikorose/DeepFilterNet libDF/src/tract.rs](https://github.com/Rikorose/DeepFilterNet/blob/main/libDF/src/tract.rs) (lines ~769-967)
- libDF keeps rolling spectral buffers sized by `df_order + lookahead`. For DFN3 the lookahead is `conv_lookahead.max(df_lookahead)`. — [libDF/src/tract.rs](https://github.com/Rikorose/DeepFilterNet/blob/main/libDF/src/tract.rs) (lines ~276-323)
- DeepFilterNet ships a real-time LADSPA plugin (PipeWire filter-chain) built on this path. — [DeepFilterNet README](https://github.com/Rikorose/DeepFilterNet)
- RTF figures:
  - Original DeepFilterNet (Interspeech 2023 demo paper): RTF 0.19 on a single-threaded notebook CPU **[paper-claimed]**. — [arXiv 2305.08227](https://arxiv.org/abs/2305.08227)
  - DeepFilterNet2: RTF 0.04 on a notebook Core-i5 **[paper-claimed]**. — [arXiv 2205.05474](https://arxiv.org/abs/2205.05474)
  - DFN3: 10 min of audio in 27 s on one core (~0.045 RTF) **[third-party, not peer-reviewed]**. — [creativeainews comparison](https://www.creativeainews.com/articles/deepfilternet-vs-rnnoise-vs-ffmpeg-noise-removal-2026/)
- Community DFN3 ports:
  - A stateful fixed-shape Core ML conversion runs 10 ms hops with recurrent state passed in and out as explicit tensors (30 ms algorithmic delay). — [iky1e/DeepFilterNet3-Streaming-CoreML](https://huggingface.co/iky1e/DeepFilterNet3-Streaming-CoreML)
  - A DFN3 ONNX export via the grazder "torchDF" fork path. — [kimtos-labs/denoiser-dfn3](https://huggingface.co/kimtos-labs/denoiser-dfn3)
  - Full-sequence ONNX with a dynamic frame axis (960-pt FFT, 480 hop, df order 5, lookahead 2). — [soniqo/DeepFilterNet3-ONNX](https://huggingface.co/soniqo/DeepFilterNet3-ONNX)
  - A related DFN2-derived model (DPDFNet) is exported as one-frame-plus-state ONNX graphs. — [TigreGotico/audiosronnx-dpdfnet](https://huggingface.co/TigreGotico/audiosronnx-dpdfnet)

### Inferences
- Options, in rough order of effort:
  - (a) Re-export DFN3 with explicit state I/O. Each causal Conv2d gets a cache tensor holding its last `kt-1` input frames. Each GRU gets an `h` in/out. The graph then processes T=1 frame per hop, and the caches/states are bound as ORT input/output pairs (ping-pong two OrtValues per state to avoid copies). The torchDF / grazder fork and the Core ML streaming conversion are the closest references.
  - (b) Use tract (Rust) via libDF's C API / LADSPA approach, which already pulses the stock ONNX graphs.
  - (c) Keep the rolling window but shrink W to the minimum the receptive field needs.
- Expected speedup from (a): per-hop conv cost drops from ~W frames to 1 frame for each graph. Encoder W=6 gives up to ~6x on its conv part; the DF decoder (W=5) up to ~5x; erb_dec (W=3) ~3x. GRU cost drops by W as well. Overall, the DFN3 per-hop time could fall by roughly 3-5x, minus fixed per-`Run()` overhead (4 calls per hop). That overhead then becomes a larger share, so fusing enc+decoders into one graph would also help. These are estimates, not measured.
- Correctness: true streaming with carried GRU state matches the offline/full-sequence model. The windowed approach restarts the GRUs from a zero or fixed state each hop, unless state is carried, and that likely degrades quality relative to the trained model. This should be checked against the current VoiceCleaner implementation.

### Gaps
- No published per-hop latency comparison of windowed vs stateful DFN3 ONNX was found.
- The internals of the grazder torchDF streaming export (state tensor layout, single-graph vs split) were not retrieved; the GitHub fork should be read directly.

## Q4. Alternative runtimes for small streaming GRU models

### Takeaway
tract is the proven runtime for DFN3 streaming (used by upstream libDF) and handles conv state automatically via pulsing. ORT CPU with a stateful export is the lowest-friction option in a C++/MSVC codebase. No primary-source head-to-head per-hop latency comparison of tract, ORT, XNNPACK, OpenVINO and LiteRT was found for DFN-sized models.

### Cited Findings
- libDF's real-time path uses `tract_pulse` / `tract_onnx` on the stock DFN3 ONNX graphs. — [libDF/src/tract.rs](https://github.com/Rikorose/DeepFilterNet/blob/main/libDF/src/tract.rs)
- RTF figures for the libDF path appear in Q3 (0.19 for DFN1 single-thread; 0.04 for DFN2) **[paper-claimed]**. — [arXiv 2305.08227](https://arxiv.org/abs/2305.08227), [arXiv 2205.05474](https://arxiv.org/abs/2205.05474)
- The ORT CPU EP ships prepacking and NCHWc layout optimizations on x86 (see Q2). — [ORT graph optimizations](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/model-optimizations/graph-optimizations.md)

### Inferences
- For models this small (DFN3 ≈ 2M params; RNNoise far smaller), per-call framework overhead (graph executor, allocation, kernel dispatch) matters as much as GEMM speed. That favors runtimes with a static plan (tract `SimplePlan`, ORT with bound I/O) or hand-written kernels (RNNoise's own `nnet` code).
- XNNPACK and LiteRT need a TFLite conversion of DFN3, which is non-trivial (complex-valued DF ops, GRUs). OpenVINO CPU is an option on Intel but adds a large dependency. Recommendation: stateful ONNX export plus ORT first; evaluate tract via libDF's C API only if ORT per-hop overhead stays high.

### Gaps
- No sourced benchmarks comparing these runtimes on small streaming audio models were found.

## Q5. DSP stage optimization (limiter, AGC, MSVC flags)

### Takeaway
Replace the O(n·L) lookahead peak scan (L=144) with an O(1)-amortized sliding-window max: a monotonic deque (Lemire's streaming max-min, ≤3 comparisons per element, no extra latency) or van Herk/Gil-Werman. This should take the limiter from ~60 µs/hop toward a few µs. For AGC, compute the gain once per block (or per sub-block) in the log domain and linearly interpolate per sample. That removes per-sample `pow`/`log10`/`sqrt`. Turn on `/fp:fast` (or at least `/fp:contract`) and `/arch:AVX2` only for the DSP translation units if the minimum-CPU policy allows. Float reductions will not auto-vectorize under `/fp:precise`.

### Cited Findings
- Lemire, "Streaming Maximum-Minimum Filter Using No More than Three Comparisons per Element" (Nordic J. Computing, 2006/2007): an online running max/min over windows of size w with "no more than 3 comparisons per element, in the worst case", and "reduced latency and memory usage". — [arXiv cs/0610046](https://arxiv.org/abs/cs/0610046)
- van Herk/Gil-Werman computes the window max with a constant number of comparisons per element via block prefix/suffix maxima over blocks. A later comparison lists it at ~6 − 8/w comparisons per element worst case, with 4w + O(1) stream latency **[as summarized by search result; primary table not retrieved]**. — [arXiv cs/0610046 PDF](https://arxiv.org/pdf/cs/0610046)
- MSVC auto-vectorizer targets SSE2/AVX/AVX2 according to `/arch`. Even under `/arch:SSE2` it may emit SSE4.2 instructions guarded by a runtime check. — [MS Docs: Auto-parallelization and auto-vectorization](https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/parallel/auto-parallelization-and-auto-vectorization.md)
- Reason code 1105, "Loop includes an unrecognized reduction operation": "reductions of 'float' or 'double' types require that the /fp:fast switch is thrown". Code 1100 = loop contains control flow (`if`, `?:`). Code 1104 = loop body includes scalar variables. Use `/Qvec-report:2` to see these reasons. — [MS Docs: Vectorizer and parallelizer messages](https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/error-messages/tool-errors/vectorizer-and-parallelizer-messages.md)
- Since VS2022, `/fp:precise` defaults to `fp_contract(off)`, so no FMA contraction. `/fp:contract` (new in VS2022) enables FMA under precise. `/fp:fast` enables contraction plus reassociation. The `float_control`, `fenv_access` and `fp_contract` pragmas allow per-function control. The default FP environment "preserves subnormal (denormal) values". — [MS Docs: /fp](https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/build/reference/fp-specify-floating-point-behavior.md)
- Measured baseline (team, Linux g++ -O2): limiter 60 µs/hop, AGC 11 µs/hop **[measured, from assignment brief]**.

### Inferences
- Limiter: with L=144 and 480 samples per hop, the naive scan is ~69k comparisons per hop. A monotonic deque does ≤3 per sample (~1.4k), i.e. roughly a 20-50x reduction in the peak-detection cost. The deque is a fixed-capacity ring buffer of size L+1 holding (index, value); no allocation.
- An alternative with fixed cost and branch-free code is the "two-stack"/vHGW block max (prefix max going forward, suffix max going backward, over blocks of L). It is SIMD-friendly and deterministic per sample, which suits p99.
- Common lookahead limiter design: delay the audio by L samples; take the sliding-window max of |x| (or of the required gain `min(1, thr/|x|)` with a sliding *min*); smooth the gain with an attack ramp that reaches the target within L samples (often a second moving-average or one-pole stage over the min-gain signal) and an exponential release. This is the approach in many open-source limiters (e.g. LSP, x42-dpl). I could not retrieve and verify those sources here.
- AGC: compute the level once per hop or per 1-2 ms sub-block (RMS via sum of squares, which vectorizes with `/fp:fast`). Convert to dB with one `log10f` per block, smooth in the dB domain with attack/release one-poles, convert back with one `exp2f`/`powf` per block, then linearly interpolate the linear gain across the block (a ramp, vectorizable). That replaces 480× `pow`/`log10`/`sqrt` with ~1-3 per block, so 11 µs → likely under 2 µs. Cheap `log2`/`exp2` bit-trick approximations are adequate for gain control if more speed is needed.
- To make loops vectorize on MSVC:
  - Avoid `if` in inner loops (use `std::min`/`std::max` or `?:`-free math, code 1100).
  - Use local accumulators.
  - Mark pointers `__restrict`.
  - Compile DSP units with `/O2 /fp:fast /arch:AVX2`, or `/fp:contract` if strictness is needed. `/arch:AVX2` on a whole binary drops pre-Haswell support; per-file `/arch:AVX2` with a CPUID dispatch keeps the baseline.
- Enable FTZ/DAZ on the audio thread (see Q2). Recursive one-pole smoothers decaying to zero are a classic denormal stall in exactly these AGC and limiter release paths.

### Gaps
- No primary-source code for LSP, JUCE or Airwindows lookahead limiters was retrieved. JUCE's `juce_dsp` `Limiter` header was located but its contents were not read, so its design (lookahead or not) is unverified.
- No MSVC-specific benchmark of auto-vectorized audio loops was found.
- No published vHGW table was retrieved directly; the vHGW latency/comparison figures above come from a search summary.
