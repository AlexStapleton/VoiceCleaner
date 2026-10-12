add_library(dfn3 dfn3_streaming.cpp)
target_include_directories(dfn3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dfn3 PUBLIC denoise-core onnxruntime)
