add_library(denoise-core passthrough_filter.cpp dc_blocker.cpp biquad.cpp limiter.cpp agc.cpp declicker.cpp noise_gate.cpp dry_wet_mix.cpp dsp_chain.cpp rnnoise_filter.cpp)
target_include_directories(denoise-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denoise-core PUBLIC transport-core rnnoise)   # frame.h; denoise-core stays windows.h-free
