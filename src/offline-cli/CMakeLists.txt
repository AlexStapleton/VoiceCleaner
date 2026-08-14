add_executable(offline-cli main.cpp)
target_link_libraries(offline-cli PRIVATE denoise-core transport-core dfn3)
# copy the ORT DLL next to the exe so --engine dfn3 runs
add_custom_command(TARGET offline-cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_PROPERTY:onnxruntime,IMPORTED_LOCATION>" "$<TARGET_FILE_DIR:offline-cli>")
