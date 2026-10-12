add_executable(offline-cli main.cpp)
target_link_libraries(offline-cli PRIVATE denoise-core transport-core)
