// ChildAffinityScope: a child process must not inherit the pinned CPU of its parent thread.
#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "lle/core/cpu.hpp"

namespace {

cpu_set_t current_mask() {
    cpu_set_t s;
    CPU_ZERO(&s);
    pthread_getaffinity_np(pthread_self(), sizeof s, &s);
    return s;
}

// Restores the test thread's original affinity even if an assertion fails.
struct RestoreAffinity {
    cpu_set_t saved = current_mask();
    ~RestoreAffinity() { pthread_setaffinity_np(pthread_self(), sizeof saved, &saved); }
};

TEST(ChildAffinityScope, MovesOffPinnedCpuAndRestores) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) GTEST_SKIP() << "needs at least 2 CPUs";
    const RestoreAffinity restore;
    lle::pin_current_thread(0);
    {
        const lle::ChildAffinityScope scope;
        const cpu_set_t inside = current_mask();
        if (!scope.active()) GTEST_SKIP() << "CPU 0 has no non-sibling CPU on this machine";
        EXPECT_FALSE(CPU_ISSET(0, &inside));
        EXPECT_GT(CPU_COUNT(&inside), 0);
    }
    const cpu_set_t after = current_mask();
    EXPECT_EQ(CPU_COUNT(&after), 1);
    EXPECT_TRUE(CPU_ISSET(0, &after));
}

TEST(ChildAffinityScope, ChildProcessRunsOffThePinnedCpu) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) GTEST_SKIP() << "needs at least 2 CPUs";
    const RestoreAffinity restore;
    lle::pin_current_thread(0);
    std::FILE* f = nullptr;
    {
        const lle::ChildAffinityScope scope;
        if (!scope.active()) GTEST_SKIP() << "CPU 0 has no non-sibling CPU on this machine";
        // The child prints its own allowed-CPU list (procfs), e.g. "Cpus_allowed_list: 1-3,5-7".
        f = ::popen("grep Cpus_allowed_list /proc/self/status", "r");
    }
    ASSERT_NE(f, nullptr);
    char buf[256] = {};
    const bool got = std::fgets(buf, sizeof buf, f) != nullptr;
    ::pclose(f);
    ASSERT_TRUE(got);
    const std::string line(buf);
    EXPECT_EQ(line.find("Cpus_allowed_list:\t0\n"), std::string::npos) << line;
    EXPECT_EQ(line.find("\t0,"), std::string::npos) << line;
    EXPECT_EQ(line.find("\t0-"), std::string::npos) << line;
}

}  // namespace
