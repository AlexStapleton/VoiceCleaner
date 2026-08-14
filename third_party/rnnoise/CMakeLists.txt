# Vendored xiph/rnnoise (https://github.com/xiph/rnnoise)
# Scalar (C) build only: no x86 RTCD/SIMD variants, no autotools config.h.
# Model weights are compiled in via src/rnnoise_data.c (downloaded via
# download_model.sh at vendoring time; committed here).

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

# Third-party C: quiet the warnings the parent project turns on with /W4.
if (MSVC)
  target_compile_options(rnnoise PRIVATE /W0 /wd4996)
  # Match the rest of the project's dynamic CRT (avoids LNK4098 LIBCMT vs MSVCRT
  # mismatch when linked into offline-cli / denoise-core).
  set_property(TARGET rnnoise PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
