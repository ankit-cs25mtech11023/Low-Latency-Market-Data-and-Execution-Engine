#pragma once
// x86-64 Time Stamp Counter (TSC) primitives.
//
// The TSC is a 64-bit counter that, on CPUs with `constant_tsc` + `nonstop_tsc`
// ("invariant TSC"), ticks at a fixed rate regardless of frequency scaling or
// C-states. Reading it costs a few tens of cycles, far cheaper than a syscall.
//
// The catch is ordering. `rdtsc` is not a serializing instruction: an out-of-order
// core may execute it before earlier instructions finish, or let later instructions
// start before it. To time a region [start, end] we want:
//   * start stamp: all *earlier* work finished before the counter is read
//       -> `lfence; rdtsc`     (lfence waits for prior instructions to complete locally)
//   * end stamp:   all work *inside* the region finished before the counter is read,
//                  and nothing *after* the region starts before it
//       -> `rdtscp; lfence`    (rdtscp waits for prior instructions; the trailing lfence
//                               stops later instructions from being executed early)
// E0 measures what each of these variants costs.
//
// Only compiler intrinsics are used (no inline asm), so the code works with GCC and Clang.

#include <x86intrin.h>

#include <cstdint>

namespace lle::arch::x86 {

// Plain read: cheapest, no ordering guarantees at all.
[[gnu::always_inline]] inline std::uint64_t rdtsc() noexcept {
    return __rdtsc();
}

// Start stamp: wait for earlier instructions, then read.
[[gnu::always_inline]] inline std::uint64_t rdtsc_start() noexcept {
    _mm_lfence();
    return __rdtsc();
}

// rdtscp also returns IA32_TSC_AUX, which Linux sets to (node << 12) | cpu.
[[gnu::always_inline]] inline std::uint64_t rdtscp(std::uint32_t& aux) noexcept {
    unsigned int a = 0;
    const std::uint64_t t = __rdtscp(&a);
    aux = a;
    return t;
}

[[gnu::always_inline]] inline std::uint64_t rdtscp() noexcept {
    unsigned int a = 0;
    return __rdtscp(&a);
}

// End stamp: wait for the timed region, read, then fence off later instructions.
[[gnu::always_inline]] inline std::uint64_t rdtsc_end() noexcept {
    unsigned int a = 0;
    const std::uint64_t t = __rdtscp(&a);
    _mm_lfence();
    return t;
}

// Fully fenced read (lfence; rdtsc; lfence): ordered against both sides.
[[gnu::always_inline]] inline std::uint64_t rdtsc_fenced() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// Spin-wait hint: reduces power and frees resources for the SMT sibling while polling.
[[gnu::always_inline]] inline void cpu_relax() noexcept {
    _mm_pause();
}

}  // namespace lle::arch::x86
