#include "lle/core/tsc.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "lle/core/cpu.hpp"

namespace {

TEST(Tsc, IsMonotonicOnOneCpu) {
    lle::pin_current_thread(lle::current_cpu());
    std::uint64_t prev = lle::Tsc::start();
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t t = lle::Tsc::stop();
        ASSERT_GE(t, prev);
        prev = t;
    }
}

TEST(Tsc, FeatureDetectionReadsCpuinfo) {
    const auto f = lle::detect_tsc_features();
    // Every x86-64 CPU from the last ~15 years has rdtscp; if this fails, /proc parsing broke.
    EXPECT_TRUE(f.rdtscp);
    EXPECT_FALSE(f.clocksource.empty());
}

TEST(Tsc, CalibrationIsConsistentWithSteadyClock) {
    const auto cal = lle::calibrate_tsc(0.2);
    ASSERT_GT(cal.ticks_per_ns, 0.1);  // > 100 MHz
    ASSERT_LT(cal.ticks_per_ns, 10.0); // < 10 GHz
    // Independent check: time a ~100 ms sleep with both clocks; they must agree to ~1%.
    const auto t0 = lle::Tsc::start();
    const auto c0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t1 = lle::Tsc::stop();
    const auto c1 = std::chrono::steady_clock::now();
    const double tsc_ns = cal.to_ns(static_cast<double>(t1 - t0));
    const double clk_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
    EXPECT_NEAR(tsc_ns / clk_ns, 1.0, 0.01);
}

TEST(Tsc, PinningMovesThread) {
    lle::pin_current_thread(0);
    EXPECT_EQ(lle::current_cpu(), 0);
}

}  // namespace
