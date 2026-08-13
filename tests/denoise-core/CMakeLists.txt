add_executable(denoise-core-tests test_passthrough.cpp test_dc_blocker.cpp)
target_link_libraries(denoise-core-tests PRIVATE denoise-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(denoise-core-tests)
