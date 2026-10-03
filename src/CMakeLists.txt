# lle_core: clock, CPU placement, histogram and result I/O. Everything else links to it.
add_library(lle_core
    core/tsc.cpp
    core/cpu.cpp
    telemetry/result_io.cpp)
target_include_directories(lle_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/generated)
target_compile_features(lle_core PUBLIC cxx_std_20)
target_link_libraries(lle_core PRIVATE lle_warnings)
find_package(Threads REQUIRED)
target_link_libraries(lle_core PUBLIC Threads::Threads)
