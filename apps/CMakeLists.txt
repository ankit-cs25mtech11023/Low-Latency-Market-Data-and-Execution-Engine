# Experiment drivers and engine binaries. Each experiment's drivers live in apps/eN/.
function(lle_add_app name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE lle_core lle_warnings)
endfunction()

lle_add_app(e0_timers e0/e0_timers.cpp)
lle_add_app(e0_skew   e0/e0_skew.cpp)
lle_add_app(e1_workload e1/e1_workload.cpp)
lle_add_app(e2_book e2/e2_book.cpp e2/alloc_count.cpp)
# The allocation-counting shim replaces the global operator new/delete. Sanitizers own the
# allocator (clang links its TSan/ASan runtime statically, and that runtime already defines
# every operator new/delete, so the link fails with "multiple definition"). A count taken
# under a sanitizer would also be meaningless, so the shim is compiled only in plain builds.
if(LLE_SANITIZER STREQUAL "")
    target_compile_definitions(e2_book PRIVATE LLE_ALLOC_SHIM=1)
else()
    target_compile_definitions(e2_book PRIVATE LLE_ALLOC_SHIM=0)
endif()
