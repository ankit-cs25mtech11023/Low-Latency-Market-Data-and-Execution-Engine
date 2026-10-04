# libFuzzer targets (clang only) are added here from Phase 1.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LLE_BUILD_FUZZ requires clang (libFuzzer)")
endif()
if(NOT LLE_SANITIZER STREQUAL "fuzzer")
    message(FATAL_ERROR "LLE_BUILD_FUZZ requires LLE_SANITIZER=fuzzer (use the 'fuzz' preset)")
endif()

function(lle_add_fuzzer name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE lle_core lle_warnings)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

# Seed corpora are generated (never committed Nasdaq data): see fuzz/README.md.
lle_add_fuzzer(fuzz_itch_decoder fuzz_itch_decoder.cpp)
