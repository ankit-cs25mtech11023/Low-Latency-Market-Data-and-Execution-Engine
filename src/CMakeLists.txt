# lle_core: clock, CPU placement, histogram, result I/O and ITCH file streaming.
# The hot-path pieces (decoder, books, queues) are header-only templates so they inline.
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

add_library(lle_core
    core/tsc.cpp
    core/cpu.cpp
    telemetry/result_io.cpp
    protocol/itch_file.cpp)
target_include_directories(lle_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/generated)
target_compile_features(lle_core PUBLIC cxx_std_20)
target_link_libraries(lle_core PRIVATE lle_warnings PUBLIC Threads::Threads ZLIB::ZLIB)
