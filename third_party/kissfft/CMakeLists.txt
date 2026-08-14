# Vendored Mark Borgerding kiss_fft (https://github.com/mborgerding/kissfft)
# BSD-3-Clause. Real-FFT wrapper (kiss_fftr) + core, single precision.
add_library(kissfft STATIC kiss_fft.c kiss_fftr.c)
target_include_directories(kissfft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(kissfft PUBLIC kiss_fft_scalar=float)   # single precision

# Third-party C: quiet the warnings the parent project turns on with /W4.
if (MSVC)
  target_compile_options(kissfft PRIVATE /W0 /wd4996)
  set_property(TARGET kissfft PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
