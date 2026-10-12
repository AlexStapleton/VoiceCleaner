cmake_minimum_required(VERSION 3.24)
project(VoiceCleaner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Developer tools (offline-cli: WAV in -> engine/chain -> WAV out, for A/B listening and the
# tools/*.py harnesses). Not part of the app or the release; off by default.
option(VC_BUILD_DEV_TOOLS "Build developer tools (offline-cli)" OFF)
if (MSVC)
  add_compile_options(/W4 /permissive-)
endif()

include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0)
FetchContent_MakeAvailable(tomlplusplus)

enable_testing()
add_subdirectory(third_party/rnnoise)
add_subdirectory(src/transport-core)
add_subdirectory(src/vc-ipc)
add_subdirectory(src/win-ipc)
add_subdirectory(src/denoise-core)
add_subdirectory(src/win-audio)
add_subdirectory(src/voicecleaner-worker)
if (VC_BUILD_DEV_TOOLS)
  add_subdirectory(src/offline-cli)
endif()
add_subdirectory(src/voicecleaner)
add_subdirectory(tests/transport-core)
add_subdirectory(tests/denoise-core)
add_subdirectory(tests/vc-ipc)
add_subdirectory(tests/win-ipc)
add_subdirectory(tests/voicecleaner)
