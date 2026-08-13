add_library(transport-core
  framer.cpp channel.cpp occupancy.cpp dropout.cpp config.cpp)
target_include_directories(transport-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(transport-core PUBLIC tomlplusplus::tomlplusplus)
# GUARD: transport-core must never depend on windows.h — do not link any Win32 lib here.
