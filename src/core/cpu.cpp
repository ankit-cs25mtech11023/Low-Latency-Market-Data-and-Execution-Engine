#include "lle/core/cpu.hpp"

#include <pthread.h>
#include <sched.h>

#include <cstddef>
#include <system_error>

namespace lle {

void pin_current_thread(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set); rc != 0)
        throw std::system_error(rc, std::generic_category(), "pthread_setaffinity_np(cpu=" + std::to_string(cpu) + ")");
    // The affinity change takes effect at the next scheduling point; yield so we are
    // guaranteed to be running on the target CPU when this function returns.
    sched_yield();
}

int current_cpu() noexcept { return sched_getcpu(); }

void set_thread_name(const std::string& name) noexcept {
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

}  // namespace lle
