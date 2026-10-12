add_library(denoise-core passthrough_filter.cpp dc_blocker.cpp biquad.cpp aggression.cpp limiter.cpp agc.cpp declicker.cpp noise_gate.cpp dry_wet_mix.cpp dsp_chain.cpp rnnoise_filter.cpp stft.cpp erb.cpp dfn3_features.cpp dfn3_postdsp.cpp dynamic_engine.cpp input_level_normalizer.cpp)
target_include_directories(denoise-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denoise-core PUBLIC transport-core rnnoise)   # frame.h; denoise-core stays windows.h-free
target_link_libraries(denoise-core PRIVATE kissfft)                 # Stft impl detail; kiss_fft types stay out of headers

# Separate target for the DFN3->INoiseFilter adapter. Kept OUT of denoise-core so
# the core lib needn't depend on dfn3/ORT (dfn3 already links denoise-core; putting
# the adapter in the core would create a dependency cycle).
add_library(dfn3-engine dfn3_stream_adapter.cpp)
target_include_directories(dfn3-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dfn3-engine PUBLIC denoise-core dfn3)
