# libFuzzer targets (clang only) are added here from Phase 1.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LLE_BUILD_FUZZ requires clang (libFuzzer)")
endif()
