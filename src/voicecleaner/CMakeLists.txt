# Pure, Windows-free decision core — the unit-tested heart of the UI (design §4).
add_library(voicecleaner-core STATIC ui_state.cpp)
target_include_directories(voicecleaner-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voicecleaner-core PUBLIC vc-ipc)
if (MSVC)
  target_compile_options(voicecleaner-core PUBLIC /utf-8)
endif()
