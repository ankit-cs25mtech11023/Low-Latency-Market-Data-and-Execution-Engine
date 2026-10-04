cmake_minimum_required(VERSION 3.25)

project(low_latency_engine
    VERSION 0.1.0
    DESCRIPTION "Low-latency NASDAQ ITCH 5.0 market data and execution engine"
    LANGUAGES C CXX)

# Strict C++20: no compiler extensions (-std=c++20, not gnu++20), no C++23.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LLE_WERROR       "Treat warnings as errors (CI)"           OFF)
option(LLE_BUILD_TESTS  "Build unit tests"                         ON)
option(LLE_BUILD_BENCH  "Build Google Benchmark micro-benchmarks"  ON)
option(LLE_BUILD_APPS   "Build apps and experiment drivers"        ON)
option(LLE_BUILD_FUZZ   "Build libFuzzer targets (clang only)"     OFF)
option(LLE_HDR_VALIDATE "Fetch HdrHistogram_c to cross-check our histogram" ON)

# CMake's default Release flags are -O3. The project baseline for every performance
# number is -O2 -DNDEBUG (plan.md, Key Decision 13), so the defaults are overridden here.
set(CMAKE_C_FLAGS_RELEASE            "-O2 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE          "-O2 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO     "-O2 -g -DNDEBUG -fno-omit-frame-pointer" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO   "-O2 -g -DNDEBUG -fno-omit-frame-pointer" CACHE STRING "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)

# Record the exact compile flags of this build so result files can carry them.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _lle_bt)
set(LLE_BUILD_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_lle_bt}}")
string(STRIP "${LLE_BUILD_FLAGS}" LLE_BUILD_FLAGS)
# The header is regenerated on every build (see cmake/GenBuildInfo.cmake for why).
set(LLE_BUILD_INFO_HPP ${CMAKE_CURRENT_BINARY_DIR}/generated/lle/build_info.hpp)
add_custom_target(lle_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.hpp.in
        -DOUT=${LLE_BUILD_INFO_HPP}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}
        -DCMAKE_CXX_COMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}
        "-DLLE_BUILD_FLAGS=${LLE_BUILD_FLAGS}"
        -DLLE_SANITIZER=${LLE_SANITIZER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenBuildInfo.cmake
    BYPRODUCTS ${LLE_BUILD_INFO_HPP}
    COMMENT "Recording git SHA in build_info.hpp")

add_subdirectory(src)

if(LLE_BUILD_TESTS OR LLE_BUILD_BENCH)
    include(Dependencies)
endif()

if(LLE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(LLE_BUILD_BENCH)
    add_subdirectory(benchmarks)
endif()
if(LLE_BUILD_APPS)
    add_subdirectory(apps)
    add_subdirectory(tools)
endif()
if(LLE_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()
