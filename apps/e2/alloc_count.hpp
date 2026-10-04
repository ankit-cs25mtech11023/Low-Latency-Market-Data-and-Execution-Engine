#pragma once
// Heap allocations made by the current thread (counted by the operator new shim in
// alloc_count.cpp, which is linked into e2_book only).
#include <cstdint>

namespace e2 {
extern thread_local std::uint64_t g_allocations;
}
