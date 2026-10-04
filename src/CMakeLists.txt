# lle_core: clock, CPU placement, histogram, result I/O and ITCH file streaming.
# The hot-path pieces (decoder, books, queues) are header-only templates so they inline.
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

add_library(lle_core
    core/tsc.cpp
    core/cpu.cpp
    telemetry/result_io.cpp
    telemetry/perf_counters.cpp
    testing/fixture_gen.cpp
    protocol/itch_file.cpp
    protocol/itch_slice.cpp
    protocol/itch_writer.cpp)
target_include_directories(lle_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/generated)
target_compile_features(lle_core PUBLIC cxx_std_20)
target_link_libraries(lle_core PRIVATE lle_warnings PUBLIC Threads::Threads ZLIB::ZLIB)
add_dependencies(lle_core lle_build_info)
