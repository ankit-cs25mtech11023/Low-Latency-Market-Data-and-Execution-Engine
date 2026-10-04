// E0, part 2: can TSC stamps taken on different cores be compared?
//
// Later experiments subtract a timestamp taken on one core (e.g. the feeder's "intended
// send time") from one taken on another (the engine's "done" time). That is only valid if
// the per-core TSCs agree. The kernel synchronizes them at boot (TSC_ADJUST), but we
// verify it rather than assume it.
//
// Method (NTP-style ping-pong between core A and core B, one cache line each way):
//   A: t0 = TSC_A; write ping=i                 B: wait for ping==i; tb = TSC_B; write pong=i
//   A: wait for pong==i; t3 = TSC_A
// If the clocks agree, tb lies inside [t0, t3]. The estimated offset of B relative to A is
//   offset = tb - (t0 + t3) / 2
// and its error is bounded by RTT/2 = (t3 - t0)/2 (we do not know where inside the round
// trip B actually read its clock). Samples with the smallest RTT give the tightest bound.
// A true offset larger than RTT/2 would show up as tb outside [t0, t3] ("violations").
//
// Side result: RTT/2 is the one-way cost of moving a cache line between the two cores.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lle/core/cli.hpp"
#include "lle/core/cpu.hpp"
#include "lle/core/tsc.hpp"
#include "lle/telemetry/histogram.hpp"
#include "lle/telemetry/result_io.hpp"

namespace {

namespace x86 = lle::arch::x86;

// Each flag on its own 128-byte block so the two directions never share a line (and the
// adjacent-line prefetcher does not pair them).
struct alignas(128) Flag {
    std::atomic<std::int64_t> v{-1};
};

struct PairResult {
    int a = 0, b = 0;
    std::size_t n = 0;
    double min_rtt = 0, p50_rtt = 0, p99_rtt = 0;
    double offset_at_min_rtt = 0;   // ticks, B relative to A
    double offset_median_best = 0;  // median offset over the 1% lowest-RTT samples
    double bound_best = 0;          // max RTT/2 within that best 1%
    std::size_t violations = 0;     // samples where tb fell outside [t0, t3]
};

PairResult measure_pair(int cpu_a, int cpu_b, std::size_t samples, std::size_t warmup) {
    const std::size_t total = samples + warmup;
    std::vector<std::uint64_t> t0(total), t3(total), tb(total);
    auto ping = std::make_unique<Flag>();
    auto pong = std::make_unique<Flag>();
    std::atomic<bool> ready{false};

    std::thread responder([&] {
        lle::pin_current_thread(cpu_b);
        ready.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < total; ++i) {
            const auto want = static_cast<std::int64_t>(i);
            while (ping->v.load(std::memory_order_acquire) != want) x86::cpu_relax();
            // lfence on both sides: the read happens after the ping was observed and before
            // the pong store can become visible.
            tb[i] = x86::rdtsc_fenced();
            pong->v.store(want, std::memory_order_release);
        }
    });

    lle::pin_current_thread(cpu_a);
    while (!ready.load(std::memory_order_acquire)) x86::cpu_relax();
    for (std::size_t i = 0; i < total; ++i) {
        const auto want = static_cast<std::int64_t>(i);
        t0[i] = lle::Tsc::start();
        ping->v.store(want, std::memory_order_release);
        while (pong->v.load(std::memory_order_acquire) != want) x86::cpu_relax();
        t3[i] = x86::rdtsc_fenced();
    }
    responder.join();

    struct S {
        double rtt, offset;
    };
    std::vector<S> s;
    s.reserve(samples);
    PairResult r;
    r.a = cpu_a;
    r.b = cpu_b;
    for (std::size_t i = warmup; i < total; ++i) {
        const auto rtt = static_cast<double>(t3[i] - t0[i]);
        // Signed differences: tb may legitimately be "before" t0 if B's clock is behind A's.
        // offset = tb - (t0 + t3)/2 = (tb - t0) - rtt/2
        const auto tb_minus_t0 = static_cast<std::int64_t>(tb[i] - t0[i]);
        const auto t3_minus_tb = static_cast<std::int64_t>(t3[i] - tb[i]);
        const double off = static_cast<double>(tb_minus_t0) - rtt / 2.0;
        if (tb_minus_t0 < 0 || t3_minus_tb < 0) ++r.violations;
        s.push_back({rtt, off});
    }
    std::ranges::sort(s, {}, &S::rtt);
    r.n = s.size();
    r.min_rtt = s.front().rtt;
    r.p50_rtt = s[s.size() / 2].rtt;
    r.p99_rtt = s[s.size() * 99 / 100].rtt;
    r.offset_at_min_rtt = s.front().offset;
    const std::size_t best = std::max<std::size_t>(1, s.size() / 100);
    std::vector<double> offs;
    offs.reserve(best);
    for (std::size_t i = 0; i < best; ++i) offs.push_back(s[i].offset);
    std::nth_element(offs.begin(), offs.begin() + static_cast<std::ptrdiff_t>(offs.size() / 2), offs.end());
    r.offset_median_best = offs[offs.size() / 2];
    r.bound_best = s[best - 1].rtt / 2.0;
    return r;
}

}  // namespace

int main(int argc, char** argv) try {
    const lle::Cli cli(argc, argv);
    const auto samples = static_cast<std::size_t>(cli.i64("samples", 200'000));
    const auto warmup = static_cast<std::size_t>(cli.i64("warmup", 10'000));
    const std::string out = cli.str("out");
    const int ncpu = static_cast<int>(std::thread::hardware_concurrency());

    std::vector<std::pair<int, int>> pairs;
    if (cli.str("pairs", "all") == "all") {
        for (int a = 0; a < ncpu; ++a)
            for (int b = 0; b < ncpu; ++b)
                if (a != b) pairs.emplace_back(a, b);
    } else {
        for (const auto& p : cli.list("pairs")) {  // "0-1,2-6"
            const auto dash = p.find('-');
            pairs.emplace_back(std::stoi(p.substr(0, dash)), std::stoi(p.substr(dash + 1)));
        }
    }

    lle::require_invariant_tsc();
    const lle::TscCalibration cal = lle::calibrate_tsc(cli.f64("calib-s", 1.0));

    const std::string csv_path = out + ".skew.csv";
    auto closer = [](std::FILE* p) { std::fclose(p); };
    std::unique_ptr<std::FILE, decltype(closer)> f(std::fopen(csv_path.c_str(), "w"), closer);
    if (!f) throw std::runtime_error("cannot open " + csv_path);
    std::fprintf(
        f.get(),
        "cpu_a,cpu_b,n,min_rtt,p50_rtt,p99_rtt,offset_at_min_rtt,offset_median_best1pct,bound_best1pct,violations\n");

    auto rtt_hist = std::make_unique<lle::Histogram>();  // RTT/2 over all pairs, for the summary
    for (const auto& [a, b] : pairs) {
        const PairResult r = measure_pair(a, b, samples, warmup);
        std::fprintf(f.get(), "%d,%d,%zu,%.0f,%.0f,%.0f,%.1f,%.1f,%.1f,%zu\n", r.a, r.b, r.n, r.min_rtt, r.p50_rtt,
                     r.p99_rtt, r.offset_at_min_rtt, r.offset_median_best, r.bound_best, r.violations);
        rtt_hist->record(static_cast<std::uint64_t>(r.p50_rtt));
        std::printf("%d->%d  rtt p50 %6.0f ticks  offset(best1%%) %+7.1f +/- %5.1f ticks  violations %zu\n", a, b,
                    r.p50_rtt, r.offset_median_best, r.bound_best, r.violations);
    }
    const std::string params = R"({"experiment":"E0","variant":"skew","samples_per_pair":)" + std::to_string(samples) +
                               R"(,"pairs":)" + std::to_string(pairs.size()) + R"(,"unit":"ticks"})";
    lle::write_run(out, *rtt_hist, cal, params);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "e0_skew: %s\n", e.what());
    return 1;
}
