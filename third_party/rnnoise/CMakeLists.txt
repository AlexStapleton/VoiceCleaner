# Vendored xiph/rnnoise (https://github.com/xiph/rnnoise)
# Built the way upstream's configure builds it by default on x86: int8 weights
# (DISABLE_DEBUG_FLOAT) plus runtime CPU detection (RTCD) that dispatches the
# network to SSE4.1 or AVX2 kernels, with an SSE2 baseline. No autotools config.h.
# Model weights are compiled in via src/rnnoise_data.c (downloaded via
# download_model.sh at vendoring time; committed here).
# Only the library sources are vendored: upstream's autotools build, examples, docs and the
# training/dataset scripts (dump_features.c, write_weights.c, rnn_train.py, scripts/) are not.
#
# Measured on the same clip (Linux gcc -O2, AVX2 CPU): ~1207 us/frame for the old
# configuration (float32 "debug" weights, no SIMD — what MSVC compiled, since MSVC
# defines neither __SSE2__ nor __AVX__) vs ~190 us/frame for this one (~6x). The
# output differs from the old build by ~-59 dB (inaudible) and VAD by <= 0.03,
# because upstream's int8 path quantizes weights/activations; see
# docs/superpowers/notes/2026-10-11-rnnoise-build-performance.md.

add_library(rnnoise STATIC
  src/denoise.c
  src/rnn.c
  src/pitch.c
  src/kiss_fft.c
  src/celt_lpc.c
  src/nnet.c
  src/nnet_default.c
  src/parse_lpcnet_weights.c
  src/rnnoise_data.c
  src/rnnoise_tables.c
)

# Public API header lives in include/; internal headers live in src/.
target_include_directories(rnnoise
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Use the int8 weights. Without this the float32 debug copies are compiled in and
# compute_linear() prefers them (4x the weight memory traffic, and not what upstream ships).
target_compile_definitions(rnnoise PRIVATE DISABLE_DEBUG_FLOAT)

# x86/x64 SIMD with runtime CPU detection (rnn_select_arch picks C/SSE2, SSE4.1 or AVX2+FMA).
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|x86|i[3-6]86)$")
  set(RNN_X86 ${CMAKE_CURRENT_SOURCE_DIR}/src/x86)
  target_sources(rnnoise PRIVATE
    ${RNN_X86}/x86cpu.c
    ${RNN_X86}/x86_dnn_map.c
    ${RNN_X86}/nnet_sse4_1.c
    ${RNN_X86}/nnet_avx2.c)
  target_compile_definitions(rnnoise PRIVATE RNN_ENABLE_X86_RTCD)
  # Each ISA is enabled ONLY on its own kernel file, so the shared code (and the arch-0
  # fallback) never contains SSE4.1/AVX2 instructions and still runs on any x64 CPU.
  if (MSVC)
    # MSVC predefines no __SSE*__ macros; x86_arch_macros.h maps OPUS_X86_MAY_HAVE_* onto
    # them. SSE/SSE2 are baseline on x64, so they are safe everywhere and select vec_avx.h's
    # SSE2 path instead of the plain-C one.
    target_compile_definitions(rnnoise PRIVATE OPUS_X86_MAY_HAVE_SSE OPUS_X86_MAY_HAVE_SSE2)
    set_source_files_properties(${RNN_X86}/nnet_sse4_1.c PROPERTIES
      COMPILE_DEFINITIONS OPUS_X86_MAY_HAVE_SSE4_1)
    # /arch:AVX2 defines __AVX__/__AVX2__ but not __FMA__; rnn_select_arch only picks this
    # kernel when CPUID reports AVX2 *and* FMA, so enabling the FMA intrinsics here is safe.
    set_source_files_properties(${RNN_X86}/nnet_avx2.c PROPERTIES
      COMPILE_OPTIONS "/arch:AVX2" COMPILE_DEFINITIONS __FMA__)
  else()
    target_compile_definitions(rnnoise PRIVATE CPU_INFO_BY_C)
    set_source_files_properties(${RNN_X86}/nnet_sse4_1.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(${RNN_X86}/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
  endif()
endif()

# Third-party C: quiet the warnings the parent project turns on with /W4.
if (MSVC)
  target_compile_options(rnnoise PRIVATE /W0 /wd4996)
  # Match the rest of the project's dynamic CRT (avoids LNK4098 LIBCMT vs MSVCRT
  # mismatch when linked into offline-cli / denoise-core).
  set_property(TARGET rnnoise PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
