add_executable(voicecleaner-worker main.cpp bridge.cpp logger.cpp)
target_link_libraries(voicecleaner-worker PRIVATE transport-core win-audio denoise-core dfn3-engine)
# copy the ORT DLL next to the exe so engine="dfn3" runs
add_custom_command(TARGET voicecleaner-worker POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_PROPERTY:onnxruntime,IMPORTED_LOCATION>" "$<TARGET_FILE_DIR:voicecleaner-worker>")
