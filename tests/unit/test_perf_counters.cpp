#include "lle/telemetry/perf_counters.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <system_error>
#include <vector>

#include "lle/core/cpu.hpp"

namespace {

using lle::PerfEvent;

// Opening counters fails where there is no PMU access (VMs, containers, CI runners with
// perf_event_paranoid > 1). That is an environment limit, not a bug, so the test skips.
std::unique_ptr<lle::PerfCounters> try_open(std::vector<PerfEvent> ev) {
    try {
        return std::make_unique<lle::PerfCounters>(std::move(ev));
    } catch (const std::system_error&) {
        return nullptr;
    }
}

TEST(PerfCounters, ParsesNames) {
    const auto ev = lle::parse_perf_events("cycles,instructions,ref-cycles,page-faults");
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_EQ(ev[0], PerfEvent::kCycles);
    EXPECT_EQ(ev[3], PerfEvent::kPageFaults);
    EXPECT_STREQ(lle::perf_event_name(PerfEvent::kRefCycles), "ref-cycles");
    EXPECT_THROW((void)lle::parse_perf_events("cycles,bogus"), std::invalid_argument);
}

TEST(PerfCounters, CountsScaleWithWork) {
    auto pc = try_open({PerfEvent::kInstructions, PerfEvent::kCycles});
    if (!pc) GTEST_SKIP() << "perf_event_open not permitted here";
    auto spin = [](std::uint64_t n) {
        std::uint64_t x = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            x += i;
            lle::compiler_barrier();
        }
        lle::do_not_optimize(x);
    };
    pc->start();
    spin(1'000'000);
    const auto small = pc->stop();
    pc->start();
    spin(10'000'000);
    const auto big = pc->stop();
    ASSERT_GT(small.get(PerfEvent::kInstructions), 1'000'000u);
    ASSERT_GT(small.get(PerfEvent::kCycles), 0u);
    // 10x the loop iterations -> roughly 10x the instructions (start/stop overhead is small).
    const double ratio = static_cast<double>(big.get(PerfEvent::kInstructions)) /
                         static_cast<double>(small.get(PerfEvent::kInstructions));
    EXPECT_GT(ratio, 8.0);
    EXPECT_LT(ratio, 12.0);
    EXPECT_GT(big.running_fraction(), 0.0);
}

TEST(PerfCounters, SoftwareEventsCountPageFaults) {
    auto pc = try_open({PerfEvent::kPageFaults});
    if (!pc) GTEST_SKIP() << "perf_event_open not permitted here";
    pc->start();
    // Touch 64 fresh pages: each first write faults the page in.
    auto mem = std::make_unique<char[]>(64 * 4096);
    for (int i = 0; i < 64; ++i) mem[static_cast<std::size_t>(i) * 4096] = 1;
    lle::do_not_optimize(mem[0]);
    const auto r = pc->stop();
    EXPECT_GE(r.get(PerfEvent::kPageFaults), 1u);
}

}  // namespace
