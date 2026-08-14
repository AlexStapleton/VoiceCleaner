cmake_minimum_required(VERSION 3.24)
project(VoiceCleaner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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
add_subdirectory(src/denoise-core)
add_subdirectory(src/win-audio)
add_subdirectory(src/phase0-app)
add_subdirectory(src/offline-cli)
add_subdirectory(tests/transport-core)
add_subdirectory(tests/denoise-core)
