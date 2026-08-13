add_library(win-audio
  device_enumerator.cpp mmcss_thread.cpp rt_memory.cpp
  wasapi_capture.cpp wasapi_render.cpp)
target_include_directories(win-audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(win-audio PUBLIC transport-core PRIVATE avrt ole32 ksuser)
