add_executable(transport-core-tests
  test_spsc_ring.cpp test_framer.cpp test_channel.cpp
  test_occupancy.cpp test_dropout.cpp test_config.cpp
  test_resampler.cpp test_drift_control.cpp test_wav.cpp)
target_link_libraries(transport-core-tests PRIVATE transport-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(transport-core-tests)
