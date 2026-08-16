add_library(win-ipc pipe.cpp telemetry_shm.cpp)
target_include_directories(win-ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(win-ipc PUBLIC vc-ipc)
