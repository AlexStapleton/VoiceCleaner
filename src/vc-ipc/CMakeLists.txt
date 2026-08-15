add_library(vc-ipc STATIC command.cpp)
target_include_directories(vc-ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# GUARD: vc-ipc must never depend on windows.h — POD structs + a pure codec only.
