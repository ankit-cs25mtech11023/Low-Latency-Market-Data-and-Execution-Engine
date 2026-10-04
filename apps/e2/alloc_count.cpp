// Allocation-counting shim for e2_book: replaces the global operator new/delete so the
// benchmark can report how many heap allocations happened inside the measured region.
//
// Why: "zero allocations on the hot path" is a claim that must be measured, not assumed.
// malloc can take a lock, touch cold memory, or fall into mmap/brk system calls; any of
// those shows up as a latency spike, so the count is reported next to every latency result.
//
// The counter is a plain thread_local integer (no atomic read-modify-write, no lock), so the
// shim adds about one increment per allocation, which is small next to malloc itself. Only the
// benchmark thread's count is read.

#include "alloc_count.hpp"

#include <cstdlib>
#include <new>

namespace e2 {
thread_local std::uint64_t g_allocations = 0;
}

namespace {
void* counted_alloc(std::size_t n) {
    ++e2::g_allocations;
    if (n == 0) n = 1;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void* counted_aligned(std::size_t n, std::align_val_t al) {
    ++e2::g_allocations;
    const auto a = static_cast<std::size_t>(al);
    const std::size_t rounded = (n + a - 1) / a * a;  // aligned_alloc needs a multiple of the alignment
    if (void* p = std::aligned_alloc(a, rounded == 0 ? a : rounded)) return p;
    throw std::bad_alloc();
}
}  // namespace

void* operator new(std::size_t n) {
    return counted_alloc(n);
}
void* operator new[](std::size_t n) {
    return counted_alloc(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return counted_alloc(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return counted_alloc(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t n, std::align_val_t al) {
    return counted_aligned(n, al);
}
void* operator new[](std::size_t n, std::align_val_t al) {
    return counted_aligned(n, al);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
