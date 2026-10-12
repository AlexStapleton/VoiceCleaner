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

FetchContent_Declare(onnxruntime
  URL https://github.com/microsoft/onnxruntime/releases/download/v1.28.0/onnxruntime-win-x64-1.28.0.zip
  URL_HASH SHA256=abef733dacbe2f571547a7150b479b5cb9cc0df22f96c24983a42cadb1b4f8bc)
FetchContent_MakeAvailable(onnxruntime)
add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_IMPLIB "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib"
  IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include")

enable_testing()
add_subdirectory(third_party/rnnoise)
add_subdirectory(third_party/kissfft)
add_subdirectory(src/transport-core)
add_subdirectory(src/vc-ipc)
add_subdirectory(src/win-ipc)
add_subdirectory(src/denoise-core)
add_subdirectory(src/dfn3)
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
