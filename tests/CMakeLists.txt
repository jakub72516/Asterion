file(GLOB_RECURSE ASTERION_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(asterion-tests ${ASTERION_TEST_SOURCES})
asterion_configure_target(asterion-tests)
target_link_libraries(asterion-tests PRIVATE asterion_core)
target_include_directories(asterion-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(asterion-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_test(NAME asterion-tests COMMAND asterion-tests)
set_tests_properties(asterion-tests PROPERTIES TIMEOUT 600)
