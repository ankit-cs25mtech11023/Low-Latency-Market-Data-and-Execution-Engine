#include "lle/core/cpu.hpp"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
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

namespace {

// Adds the CPUs of /sys/devices/system/cpu/cpuN/topology/thread_siblings_list ("2,6" or
// "2-3") to `set`. N itself is always added, also when the file is missing.
void add_with_siblings(int cpu, cpu_set_t& set) {
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list");
    std::string list;
    if (!std::getline(f, list)) return;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const auto dash = item.find('-');
        const int lo = std::stoi(item.substr(0, dash));
        const int hi = dash == std::string::npos ? lo : std::stoi(item.substr(dash + 1));
        for (int c = lo; c <= hi && c < CPU_SETSIZE; ++c) CPU_SET(static_cast<std::size_t>(c), &set);
    }
}

}  // namespace

ChildAffinityScope::ChildAffinityScope() {
    if (pthread_getaffinity_np(pthread_self(), sizeof saved_, &saved_) != 0) return;
    cpu_set_t avoid;
    CPU_ZERO(&avoid);
    for (int c = 0; c < CPU_SETSIZE; ++c)
        if (CPU_ISSET(static_cast<std::size_t>(c), &saved_)) add_with_siblings(c, avoid);
    cpu_set_t others;
    CPU_ZERO(&others);
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (int c = 0; c < n && c < CPU_SETSIZE; ++c)
        if (!CPU_ISSET(static_cast<std::size_t>(c), &avoid)) CPU_SET(static_cast<std::size_t>(c), &others);
    if (CPU_COUNT(&others) == 0) return;  // caller not pinned (or no other core): leave as is
    changed_ = pthread_setaffinity_np(pthread_self(), sizeof others, &others) == 0;
}

ChildAffinityScope::~ChildAffinityScope() {
    if (!changed_) return;
    pthread_setaffinity_np(pthread_self(), sizeof saved_, &saved_);
    sched_yield();  // be back on the original CPU(s) when the scope ends, as pin_current_thread
}

int current_cpu() noexcept {
    return sched_getcpu();
}

void set_thread_name(const std::string& name) noexcept {
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

}  // namespace lle
