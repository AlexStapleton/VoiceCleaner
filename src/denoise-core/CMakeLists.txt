add_library(denoise-core passthrough_filter.cpp dc_blocker.cpp biquad.cpp aggression.cpp limiter.cpp agc.cpp dsp_chain.cpp rnnoise_filter.cpp stft.cpp erb.cpp dfn3_features.cpp dfn3_postdsp.cpp)
target_include_directories(denoise-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denoise-core PUBLIC transport-core rnnoise)   # frame.h; denoise-core stays windows.h-free
target_link_libraries(denoise-core PRIVATE kissfft)                 # Stft impl detail; kiss_fft types stay out of headers
