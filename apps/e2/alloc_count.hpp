#pragma once
// Heap allocations made by the current thread (counted by the operator new shim in
// alloc_count.cpp, which is linked into e2_book only).
#include <cstdint>

#ifndef LLE_ALLOC_SHIM
#define LLE_ALLOC_SHIM 1
#endif

namespace e2 {
// False in sanitizer builds: the shim is compiled out there (see apps/CMakeLists.txt), so
// g_allocations stays 0 and must be reported as "not measured", never as "0 allocations".
inline constexpr bool kAllocShim = LLE_ALLOC_SHIM != 0;
extern thread_local std::uint64_t g_allocations;
}  // namespace e2
