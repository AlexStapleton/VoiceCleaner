add_executable(dfn3-probe main.cpp)
target_link_libraries(dfn3-probe PRIVATE onnxruntime)
# copy the ORT DLL next to the exe so it runs
add_custom_command(TARGET dfn3-probe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_PROPERTY:onnxruntime,IMPORTED_LOCATION>" "$<TARGET_FILE_DIR:dfn3-probe>")
