add_executable(phase0-app main.cpp bridge.cpp logger.cpp)
target_link_libraries(phase0-app PRIVATE transport-core win-audio denoise-core dfn3-engine)
# copy the ORT DLL next to the exe so engine="dfn3" runs
add_custom_command(TARGET phase0-app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_PROPERTY:onnxruntime,IMPORTED_LOCATION>" "$<TARGET_FILE_DIR:phase0-app>")
