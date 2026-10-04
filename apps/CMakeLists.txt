# Experiment drivers and engine binaries. Each experiment's drivers live in apps/eN/.
function(lle_add_app name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE lle_core lle_warnings)
endfunction()

lle_add_app(e0_timers e0/e0_timers.cpp)
lle_add_app(e0_skew   e0/e0_skew.cpp)
lle_add_app(e1_workload e1/e1_workload.cpp)
lle_add_app(e2_book e2/e2_book.cpp e2/alloc_count.cpp)
