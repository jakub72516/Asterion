cmake_minimum_required(VERSION 3.24)

project(asterion
    VERSION 0.1.0
    DESCRIPTION "Asterion: Windows-first distributed append-only event database"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use folders in IDE generators.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(ASTERION_BUILD_TESTS "Build the test suite" ON)
option(ASTERION_BUILD_BENCH "Build the workload/benchmark generator" ON)
option(ASTERION_ENABLE_ASAN "Enable AddressSanitizer (clang-cl only)" OFF)

# ---------------------------------------------------------------------------
# Platform / toolchain detection
# ---------------------------------------------------------------------------
if(MSVC)
    # cl (MSVC) or clang-cl both define _MSC_VER; detect clang-cl via CMAKE_CXX_COMPILER_ID.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(STATUS "Toolchain: clang-cl")
    else()
        message(STATUS "Toolchain: MSVC (cl)")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(WARNING "Asterion targets Windows; building with GCC is unsupported but attempted.")
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
function(asterion_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus
            /Zc:preprocessor
            /D_WIN32_WINNT=0x0A00
            /DNOMINMAX
            /DUNICODE
            /D_UNICODE
        )
        # Link winsock2, ws2_32, bcrypt, and the OneCore-friendly libs.
        target_link_libraries(${target} PRIVATE ws2_32 bcrypt)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND ASTERION_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            target_link_options(${target} PRIVATE /fsanitize=address)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Core static library + executables
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ASTERION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Bench / workload
# ---------------------------------------------------------------------------
if(ASTERION_BUILD_BENCH)
    add_subdirectory(workload)
endif()
