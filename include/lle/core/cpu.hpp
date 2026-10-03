#pragma once
// CPU placement helpers.
//
// Pinning a thread to one logical CPU removes scheduler migrations (which flush the
// private L1/L2 working set and can move the thread next to a noisy neighbour), and makes
// "which core talks to which" an explicit experimental variable. On the i5-8250U, logical
// CPUs n and n+4 are SMT siblings sharing one physical core.

#include <string>

namespace lle {

// Pins the calling thread to `cpu`. Throws std::system_error on failure.
void pin_current_thread(int cpu);

// Logical CPU the calling thread is running on right now (sched_getcpu).
[[nodiscard]] int current_cpu() noexcept;

// Names the calling thread (visible in top -H, perf, gdb). Truncated to 15 characters.
void set_thread_name(const std::string& name) noexcept;

// Prevents the compiler from optimizing away `value` or reordering memory accesses
// across this point. Used by benchmarks; it emits no instructions.
template <class T>
[[gnu::always_inline]] inline void do_not_optimize(T const& value) noexcept {
    asm volatile("" : : "r,m"(value) : "memory");
}

[[gnu::always_inline]] inline void compiler_barrier() noexcept { asm volatile("" : : : "memory"); }

}  // namespace lle
