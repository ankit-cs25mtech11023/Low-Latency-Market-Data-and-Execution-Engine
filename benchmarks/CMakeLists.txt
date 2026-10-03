# Google Benchmark micro-benchmarks: quick development feedback only. Experiment numbers
# come from the apps/eN drivers run through scripts/run_bench.py (interleaved runs + CIs).
function(lle_add_bench name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE lle_core lle_warnings benchmark::benchmark_main)
endfunction()

lle_add_bench(bench_core bench_core.cpp)
