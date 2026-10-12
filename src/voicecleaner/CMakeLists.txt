# Pure, Windows-free decision core — the unit-tested heart of the UI.
add_library(voicecleaner-core STATIC ui_state.cpp device_picker.cpp slider.cpp slider_table.cpp respawn_policy.cpp)
target_include_directories(voicecleaner-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voicecleaner-core PUBLIC vc-ipc transport-core)   # transport-core: MicProfile
if (MSVC)
  target_compile_options(voicecleaner-core PUBLIC /utf-8)
endif()

# Icon resources: configure app.rc.in with absolute paths to resource.h and the branding .ico
# files, so rc.exe resolves them regardless of its working directory (design: state-aware tray icon).
set(VC_RC_HEADER    "${CMAKE_CURRENT_SOURCE_DIR}/resource.h")
set(VC_BRANDING_DIR "${CMAKE_SOURCE_DIR}/assets/branding")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)

# The GUI executable: thin Win32/D2D/tray glue over voicecleaner-core.
add_executable(voicecleaner WIN32
  main.cpp worker_supervisor.cpp app_window.cpp tray.cpp device_enum.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
target_link_libraries(voicecleaner PRIVATE
  voicecleaner-core win-ipc transport-core win-audio
  d2d1 dwrite user32 shell32 ole32)
# The GUI is wide throughout (wWinMain, wide literals, explicit ...W calls). Define UNICODE so the
# generic Win32 macros (DefWindowProc, the message pump, CREATESTRUCT) resolve to their W variants —
# without this, DefWindowProcA's WM_NCCREATE handler truncates the wide window caption to one char.
target_compile_definitions(voicecleaner PRIVATE UNICODE _UNICODE)
if (MSVC)
  target_compile_options(voicecleaner PRIVATE /utf-8)
endif()

# The UI spawns voicecleaner-worker.exe from its own directory - make sure it is built first and
# co-locate it next to voicecleaner.exe.
add_dependencies(voicecleaner voicecleaner-worker)
add_custom_command(TARGET voicecleaner POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:voicecleaner-worker>" "$<TARGET_FILE_DIR:voicecleaner>"
  COMMENT "Co-locating worker next to voicecleaner.exe")
