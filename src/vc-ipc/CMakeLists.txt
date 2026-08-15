add_library(vc-ipc INTERFACE)
target_include_directories(vc-ipc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# GUARD: vc-ipc must never depend on windows.h — POD structs + a pure codec only.
