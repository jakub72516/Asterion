file(GLOB_RECURSE ASTERION_WORKLOAD_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(asterion-bench ${ASTERION_WORKLOAD_SOURCES})
asterion_configure_target(asterion-bench)
target_link_libraries(asterion-bench PRIVATE asterion_core)
target_include_directories(asterion-bench PRIVATE "${CMAKE_SOURCE_DIR}/src")

set_target_properties(asterion-bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
