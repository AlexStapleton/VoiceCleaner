add_executable(denoise-core-tests test_passthrough.cpp test_dc_blocker.cpp test_biquad.cpp test_limiter.cpp test_agc.cpp test_declicker.cpp test_noise_gate.cpp test_dry_wet_mix.cpp test_dsp_chain.cpp test_rnnoise.cpp)
target_link_libraries(denoise-core-tests PRIVATE denoise-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(denoise-core-tests)
