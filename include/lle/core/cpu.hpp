#pragma once
// CPU placement helpers.
//
// Pinning a thread to one logical CPU removes scheduler migrations (which flush the
// private L1/L2 working set and can move the thread next to a noisy neighbour), and makes
// "which core talks to which" an explicit experimental variable. On the i5-8250U, logical
// CPUs n and n+4 are SMT siblings sharing one physical core.

#include <sched.h>

#include <string>

namespace lle {

// Pins the calling thread to `cpu`. Throws std::system_error on failure.
void pin_current_thread(int cpu);

// While alive, moves the calling thread off the CPUs it may run on now AND their SMT
// siblings (onto every other online CPU); the destructor restores the original affinity.
//
// Why: a child process started with popen/fork/exec inherits the CPU affinity of the thread
// that starts it. A benchmark thread pinned to CPU 2 that then starts `pigz` would get pigz
// pinned to CPU 2 as well, time-sliced with the code being measured (found in E2: the
// decompressor's time slices landed inside the timed regions). Wrap the spawn in this scope
// so the child runs on other physical cores. If the caller is not pinned (no CPU would be
// left), it does nothing.
class ChildAffinityScope {
public:
    ChildAffinityScope();
    ~ChildAffinityScope();
    ChildAffinityScope(const ChildAffinityScope&) = delete;
    ChildAffinityScope& operator=(const ChildAffinityScope&) = delete;

    // True if the affinity was changed (the caller was pinned and other CPUs exist).
    [[nodiscard]] bool active() const noexcept { return changed_; }

private:
    cpu_set_t saved_{};
    bool changed_ = false;
};

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

[[gnu::always_inline]] inline void compiler_barrier() noexcept {
    asm volatile("" : : : "memory");
}

}  // namespace lle
