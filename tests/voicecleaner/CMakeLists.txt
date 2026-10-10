add_executable(voicecleaner-tests test_ui_state.cpp test_device_picker.cpp test_slider.cpp test_respawn_policy.cpp test_param_outbox.cpp)
target_link_libraries(voicecleaner-tests PRIVATE voicecleaner-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(voicecleaner-tests)
