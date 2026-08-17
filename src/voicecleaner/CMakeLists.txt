# Pure, Windows-free decision core — the unit-tested heart of the UI (design §4).
add_library(voicecleaner-core STATIC ui_state.cpp)
target_include_directories(voicecleaner-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voicecleaner-core PUBLIC vc-ipc)
if (MSVC)
  target_compile_options(voicecleaner-core PUBLIC /utf-8)
endif()

# The GUI executable: thin Win32/D2D/tray glue over voicecleaner-core.
add_executable(voicecleaner WIN32
  main.cpp worker_supervisor.cpp app_window.cpp tray.cpp)
target_link_libraries(voicecleaner PRIVATE
  voicecleaner-core win-ipc transport-core
  d2d1 dwrite user32 shell32 ole32)
if (MSVC)
  target_compile_options(voicecleaner PRIVATE /utf-8)
endif()

# Ship settings.toml next to the exe on first build only (never clobber a user-edited file).
add_custom_command(TARGET voicecleaner POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/settings.toml
    -DDST=$<TARGET_FILE_DIR:voicecleaner>/settings.toml
    -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_absent.cmake)

# The UI spawns voicecleaner-worker.exe from its own directory — make sure it is built first
# and its ORT dependency is available alongside.
add_dependencies(voicecleaner voicecleaner-worker)

# Co-locate everything the spawned worker resolves relative to the UI's dir (CWD): the worker exe,
# its ORT dll, and the DFN3 models (bridge.cpp resolves them via the relative path "models/dfn3").
add_custom_command(TARGET voicecleaner POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:voicecleaner-worker>" "$<TARGET_FILE_DIR:voicecleaner>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_PROPERTY:onnxruntime,IMPORTED_LOCATION>" "$<TARGET_FILE_DIR:voicecleaner>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
    "${CMAKE_SOURCE_DIR}/models/dfn3" "$<TARGET_FILE_DIR:voicecleaner>/models/dfn3"
  COMMENT "Co-locating worker + ORT dll + DFN3 models next to voicecleaner.exe")
