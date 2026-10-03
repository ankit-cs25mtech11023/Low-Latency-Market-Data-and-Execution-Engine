// E0, part 1: what does it cost to read the time?
//
// Every latency number in this project is a difference of two timestamps, so the cost
// and ordering behaviour of the timestamp itself is measured first.
//
// Two kinds of variant:
//   * per-call cost: K back-to-back calls of one clock, bracketed by one start/stop pair.
//     Value recorded = total ticks for the batch; analysis divides by K (value_divisor).
//     The bracket overhead (~tens of ticks) is amortized over K calls.
//   * "empty_region": Tsc::start() immediately followed by Tsc::stop(), one sample each.
//     This is the measurement floor: the smallest interval the start/stop pair can report.
//     Every stage latency measured later includes this floor.
//
// Run one variant per process (scripts/run_bench.py interleaves variants across runs).
//
// TSC ticks are not core cycles: the TSC ticks at a fixed rate (1.8 GHz on this CPU) while
// the core clock depends on the governor/turbo state. To report true core cycles per call,
// hardware counters (--counters, default cycles,instructions,ref-cycles) are enabled around
// the measured batches only, excluding calibration and warm-up. analyse.py divides them by
// the number of calls. Use --counters none where perf_event_open is not permitted.

#include <time.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lle/core/cli.hpp"
#include "lle/core/cpu.hpp"
#include "lle/core/tsc.hpp"
#include "lle/telemetry/histogram.hpp"
#include "lle/telemetry/perf_counters.hpp"
#include "lle/telemetry/result_io.hpp"

namespace {

namespace x86 = lle::arch::x86;

std::uint64_t clock_ns(clockid_t id) noexcept {
    timespec ts{};
    clock_gettime(id, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

// One call of the clock under test. Returning the value and folding it into a sink keeps
// the compiler from deleting the call.
template <int V>
[[gnu::always_inline]] inline std::uint64_t read_clock() noexcept {
    if constexpr (V == 0) return x86::rdtsc();
    else if constexpr (V == 1) return x86::rdtsc_start();     // lfence; rdtsc
    else if constexpr (V == 2) return x86::rdtscp();          // rdtscp
    else if constexpr (V == 3) return x86::rdtsc_end();       // rdtscp; lfence
    else if constexpr (V == 4) return x86::rdtsc_fenced();    // lfence; rdtsc; lfence
    else if constexpr (V == 5)
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    else if constexpr (V == 6) return clock_ns(CLOCK_MONOTONIC);
    else if constexpr (V == 7) return clock_ns(CLOCK_MONOTONIC_RAW);
    else if constexpr (V == 8) return clock_ns(CLOCK_REALTIME);
    else return 0;  // V == 9: empty loop body (loop overhead baseline)
}

template <int V>
void run_batches(lle::Histogram& h, std::int64_t batches, std::int64_t k) {
    std::uint64_t sink = 0;
    for (std::int64_t b = 0; b < batches; ++b) {
        const std::uint64_t t0 = lle::Tsc::start();
        for (std::int64_t i = 0; i < k; ++i) {
            sink += read_clock<V>();
            lle::compiler_barrier();  // keep the loop from being collapsed or vectorized
        }
        const std::uint64_t t1 = lle::Tsc::stop();
        h.record(t1 - t0);
    }
    lle::do_not_optimize(sink);
}

void run_empty_region(lle::Histogram& h, std::int64_t samples) {
    for (std::int64_t i = 0; i < samples; ++i) {
        const std::uint64_t t0 = lle::Tsc::start();
        const std::uint64_t t1 = lle::Tsc::stop();
        h.record(t1 - t0);
    }
}

// Histogram record cost: K records of precomputed latency-like values (no RNG in the loop).
// The "hist_baseline" variant runs the same loop but only sums the values, so the difference
// isolates record() itself.
void run_hist(lle::Histogram& h, std::int64_t batches, std::int64_t k, bool baseline) {
    std::vector<std::uint64_t> values(4096);  // 32 KiB: stays L1/L2-resident
    std::mt19937_64 rng(1234);
    std::lognormal_distribution<double> dist(5.0, 1.0);
    for (auto& v : values) v = static_cast<std::uint64_t>(dist(rng));
    auto target = std::make_unique<lle::Histogram>();
    std::uint64_t sink = 0;
    for (std::int64_t b = 0; b < batches; ++b) {
        const std::uint64_t t0 = lle::Tsc::start();
        for (std::int64_t i = 0; i < k; ++i) {
            const std::uint64_t v = values[static_cast<std::size_t>(i) & 4095u];
            if (baseline) sink += v;
            else target->record(v);
            lle::compiler_barrier();
        }
        const std::uint64_t t1 = lle::Tsc::stop();
        h.record(t1 - t0);
    }
    lle::do_not_optimize(sink);
    lle::do_not_optimize(target->count());
}

}  // namespace

int main(int argc, char** argv) try {
    const lle::Cli cli(argc, argv);
    const std::string variant = cli.str("variant");
    const int cpu = static_cast<int>(cli.i64("cpu", 2));
    const std::int64_t batches = cli.i64("batches", 200'000);
    const std::int64_t k = cli.i64("batch-size", 1000);
    const std::int64_t warmup = cli.i64("warmup-batches", 10'000);
    const std::string out = cli.str("out");
    const std::string counters_arg = cli.str("counters", "cycles,instructions,ref-cycles");

    lle::require_invariant_tsc();
    lle::pin_current_thread(cpu);
    const lle::TscCalibration cal = lle::calibrate_tsc(cli.f64("calib-s", 1.0));

    auto hist = std::make_unique<lle::Histogram>();
    auto run = [&](std::int64_t n) {
        if (variant == "rdtsc") run_batches<0>(*hist, n, k);
        else if (variant == "lfence_rdtsc") run_batches<1>(*hist, n, k);
        else if (variant == "rdtscp") run_batches<2>(*hist, n, k);
        else if (variant == "rdtscp_lfence") run_batches<3>(*hist, n, k);
        else if (variant == "lfence_rdtsc_lfence") run_batches<4>(*hist, n, k);
        else if (variant == "steady_clock") run_batches<5>(*hist, n, k);
        else if (variant == "clock_monotonic") run_batches<6>(*hist, n, k);
        else if (variant == "clock_monotonic_raw") run_batches<7>(*hist, n, k);
        else if (variant == "clock_realtime") run_batches<8>(*hist, n, k);
        else if (variant == "empty_loop") run_batches<9>(*hist, n, k);
        else if (variant == "empty_region") run_empty_region(*hist, n);
        else if (variant == "hist_record") run_hist(*hist, n, k, false);
        else if (variant == "hist_baseline") run_hist(*hist, n, k, true);
        else throw std::invalid_argument("unknown variant " + variant);
    };
    // Open the counters before warm-up so the open() syscalls are not inside the region.
    std::unique_ptr<lle::PerfCounters> pmu;
    if (counters_arg != "none") pmu = std::make_unique<lle::PerfCounters>(lle::parse_perf_events(counters_arg));

    run(warmup);  // warm caches, branch predictors, and let the core settle at its frequency
    hist->reset();
    if (pmu) pmu->start();
    run(batches);
    const std::string counters_json = pmu ? pmu->stop().to_json() : "null";

    const bool per_call = variant != "empty_region";
    const std::string params = R"({"experiment":"E0","variant":")" + lle::json_escape(variant) +
                               R"(","cpu":)" + std::to_string(cpu) + R"(,"batches":)" + std::to_string(batches) +
                               R"(,"batch_size":)" + std::to_string(per_call ? k : 1) +
                               R"(,"value_divisor":)" + std::to_string(per_call ? k : 1) +
                               R"(,"unit":"ticks","ran_on_cpu":)" + std::to_string(lle::current_cpu()) +
                               R"(,"counters":)" + counters_json + "}";
    lle::write_run(out, *hist, cal, params);
    std::printf("%s: p50 %.2f ticks/call (%.2f ns), TSC %.4f GHz\n", variant.c_str(),
                static_cast<double>(hist->value_at_quantile(0.5)) / static_cast<double>(per_call ? k : 1),
                cal.to_ns(static_cast<double>(hist->value_at_quantile(0.5)) / static_cast<double>(per_call ? k : 1)),
                cal.ghz());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "e0_timers: %s\n", e.what());
    return 1;
}
