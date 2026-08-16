add_executable(win-ipc-tests test_pipe.cpp test_telemetry_shm.cpp)
target_link_libraries(win-ipc-tests PRIVATE win-ipc GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(win-ipc-tests)
