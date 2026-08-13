add_library(denoise-core passthrough_filter.cpp dc_blocker.cpp)
target_include_directories(denoise-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denoise-core PUBLIC transport-core)   # frame.h; denoise-core stays windows.h-free
