add_executable(phase0-app main.cpp bridge.cpp logger.cpp)
target_link_libraries(phase0-app PRIVATE transport-core win-audio)
