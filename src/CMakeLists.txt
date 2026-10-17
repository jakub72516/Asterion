file(GLOB_RECURSE ASTERION_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/net/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/replication/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/fault/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/metrics/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/client/*.cpp"
)

add_library(asterion_core STATIC ${ASTERION_SOURCES})
asterion_configure_target(asterion_core)
target_include_directories(asterion_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(asterion-node node/main.cpp)
asterion_configure_target(asterion-node)
target_link_libraries(asterion-node PRIVATE asterion_core)

add_executable(asterion-cli cli/main.cpp)
asterion_configure_target(asterion-cli)
target_link_libraries(asterion-cli PRIVATE asterion_core)

set_target_properties(asterion-node asterion-cli
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
