add_executable(voicecleaner-worker main.cpp bridge.cpp logger.cpp)
target_link_libraries(voicecleaner-worker PRIVATE transport-core win-audio denoise-core win-ipc shell32)   # shell32: CommandLineToArgvW
