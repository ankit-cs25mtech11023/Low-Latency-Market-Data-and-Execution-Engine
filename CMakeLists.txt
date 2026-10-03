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
execute_process(COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE LLE_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
configure_file(cmake/build_info.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/lle/build_info.hpp @ONLY)

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
