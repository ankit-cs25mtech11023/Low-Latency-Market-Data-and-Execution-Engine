include(GoogleTest)

# One test binary per area keeps rebuilds fast and failures easy to locate.
function(lle_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE lle_core lle_warnings GTest::gtest_main GTest::gmock)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()

lle_add_test(test_core
    unit/test_histogram.cpp
    unit/test_tsc.cpp
    unit/test_perf_counters.cpp
    unit/test_cpu.cpp)

if(LLE_HDR_VALIDATE)
    lle_add_test(test_histogram_vs_hdr unit/test_histogram_vs_hdr.cpp)
    target_link_libraries(test_histogram_vs_hdr PRIVATE hdr_histogram_static)
endif()

lle_add_test(test_protocol
    unit/test_itch.cpp
    unit/test_itch_file.cpp)

lle_add_test(test_book
    unit/test_reference_book.cpp
    unit/test_differential.cpp
    unit/test_book_properties.cpp
    unit/test_fast_book.cpp)
