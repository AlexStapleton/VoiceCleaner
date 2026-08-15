add_executable(vc-ipc-tests test_params.cpp)
target_link_libraries(vc-ipc-tests PRIVATE vc-ipc GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(vc-ipc-tests)
