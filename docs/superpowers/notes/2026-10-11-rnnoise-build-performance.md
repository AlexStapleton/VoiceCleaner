# RNNoise build performance (2026-10-11)

_Follow-up to `reports/VoiceCleaner approach comparison.md`, item QW1, and the earlier review's P3.
RNNoise is the only engine used day to day (DFN3 lets keyboard clicks through), so this pass targets
only the RNNoise engine's own cost._

## What was wrong

`third_party/rnnoise/CMakeLists.txt` built RNNoise differently from how upstream ships it, in two ways:

1. **Float32 "debug" weights.** `DISABLE_DEBUG_FLOAT` was never defined, so `rnnoise_data.c`
   compiled a float32 copy of every layer, and `compute_linear()` (`src/nnet_arch.h`) uses
   `float_weights` whenever it is non-NULL. Upstream's `configure` defines `DISABLE_DEBUG_FLOAT` by
   default and runs on the int8 weights instead.
2. **No SIMD on MSVC.** `src/vec.h` selects the vectorized kernels only when `__SSE2__` or
   `__AVX__` is defined. MSVC defines neither, so the Windows build ran RNNoise's plain-C
   fallback (gcc even warns "Compiling without any vectorization. This code will be very slow"
   for that path). The x86 runtime CPU detection (RTCD) sources were vendored but not built.

## What changed

`third_party/rnnoise/CMakeLists.txt` only. No VoiceCleaner code changed.

- `DISABLE_DEBUG_FLOAT`: the build uses the int8 weights.
- `RNN_ENABLE_X86_RTCD` plus `src/x86/{x86cpu,x86_dnn_map,nnet_sse4_1,nnet_avx2}.c`: at
  `rnnoise_create()`, `rnn_select_arch()` reads CPUID and picks one path:

  | Path | When it is used |
  |---|---|
  | C/SSE2 (arch 0) | baseline |
  | SSE4.1 (arch 1) | CPU has SSE4.1 |
  | AVX2+FMA (arch 2) | CPU reports both AVX2 and FMA |

- MSVC: `OPUS_X86_MAY_HAVE_SSE`/`SSE2` are defined globally. SSE2 is baseline on x64, and
  `x86_arch_macros.h` maps these to `__SSE__`/`__SSE2__`, which selects the SSE2 kernels in
  `vec_avx.h`.
- Each higher ISA is enabled **only on its own kernel file**:
  - `OPUS_X86_MAY_HAVE_SSE4_1` on `nnet_sse4_1.c`.
  - `/arch:AVX2` and `__FMA__` on `nnet_avx2.c`. `/arch:AVX2` doesn't define `__FMA__`; it is
    safe to add because that kernel is only selected when CPUID reports FMA.

  Shared code therefore never contains SSE4.1 or AVX2 instructions and still runs on any x64
  CPU. MSVC doesn't police intrinsics the way GCC does, so a global SSE4.1 define could have
  leaked SSE4.1 instructions into the baseline path.

## Measurements

Setup:
- **Clip:** `tools/dfn3_make_quality_clip.py` (synthetic speech plus keyboard clicks plus a
  fan/hum floor, 5 dB SNR in voiced regions, 4 s = 400 frames at 48 kHz).
- **Harness:** calls `rnnoise_process_frame` directly; timings are the median of 9–15 runs.
- **Machine:** Linux gcc `-O2` on a 4-core AVX2/FMA cloud VM. The old MSVC configuration was
  reproduced by compiling with `-U__SSE2__ -U__SSE__ -U__AVX__`.

| Build | µs per 10 ms frame | vs old | Output vs old build | max \|ΔVAD\| |
|---|---:|---:|---:|---:|
| Old: float weights, no SIMD (what MSVC compiled) | 1207 | 1.0x | reference | 0 |
| Float weights + SSE2 | 558 | 2.2x | −68 dB | 0.001 |
| Int8 weights, no SIMD | 1202 | 1.0x | −59 dB | 0.032 |
| **New: int8 + RTCD (selects AVX2 on this CPU)** | **190–204** | **~6x** | **−59 dB** | **0.031** |
| New, forced arch 0 (C/SSE2) | 335 | 3.6x | −58 dB | 0.034 |
| New, forced arch 1 (SSE4.1) | 221 | 5.5x | −58 dB | 0.034 |
| New, forced arch 2 (AVX2+FMA) | 203 | 5.9x | −59 dB | 0.031 |

**Quality:**
- **Output difference:** about −59 dB relative to the signal (≈0.1% in amplitude). It comes from
  upstream's int8 quantization and isn't audible.
- **Noise and click removal:** the residual in speech-free regions is −20.95 dB relative to the
  input for both the old and new builds, so no change.
- **VAD:** moves by at most 0.03. That stays well inside the DSP chain's 0.3/0.6 hysteresis
  thresholds.

**CMake check:** the real `third_party/rnnoise/CMakeLists.txt`, built through CMake on Linux,
produces output byte-identical to the hand-built variant.

## Still to verify on Windows

- **MSVC build:** CI compiles the MSVC path (`/arch:AVX2`, the per-file defines), and the
  unit tests run there, including `Rnnoise.ProcessesFrameFiniteAndVadInRange`.
- **Live timing:** the worker's `frame-time (engine+chain)` / `hop-time` summary and the
  `frameP50/P99` telemetry should drop from roughly 1–2 ms to well under 0.5 ms per hop.
- **Listening test:** a quick A/B by ear with the user's own mic and keyboard.
