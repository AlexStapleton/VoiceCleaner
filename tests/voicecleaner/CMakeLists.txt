add_executable(voicecleaner-tests test_ui_state.cpp)
target_link_libraries(voicecleaner-tests PRIVATE voicecleaner-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(voicecleaner-tests)
