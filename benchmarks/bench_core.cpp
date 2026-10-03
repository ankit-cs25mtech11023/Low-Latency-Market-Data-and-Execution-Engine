#include <benchmark/benchmark.h>

#include <chrono>
#include <memory>
#include <random>
#include <vector>

#include "lle/core/tsc.hpp"
#include "lle/telemetry/histogram.hpp"

namespace {

void BM_rdtsc(benchmark::State& s) {
    for (auto _ : s) benchmark::DoNotOptimize(lle::arch::x86::rdtsc());
}
BENCHMARK(BM_rdtsc);

void BM_tsc_start(benchmark::State& s) {
    for (auto _ : s) benchmark::DoNotOptimize(lle::Tsc::start());
}
BENCHMARK(BM_tsc_start);

void BM_tsc_stop(benchmark::State& s) {
    for (auto _ : s) benchmark::DoNotOptimize(lle::Tsc::stop());
}
BENCHMARK(BM_tsc_stop);

void BM_steady_clock(benchmark::State& s) {
    for (auto _ : s) benchmark::DoNotOptimize(std::chrono::steady_clock::now());
}
BENCHMARK(BM_steady_clock);

void BM_histogram_record(benchmark::State& s) {
    auto h = std::make_unique<lle::Histogram>();
    std::vector<std::uint64_t> v(4096);
    std::mt19937_64 rng(1);
    std::lognormal_distribution<double> d(5.0, 1.0);
    for (auto& x : v) x = static_cast<std::uint64_t>(d(rng));
    std::size_t i = 0;
    for (auto _ : s) {
        h->record(v[i++ & 4095u]);
    }
    benchmark::DoNotOptimize(h->count());
}
BENCHMARK(BM_histogram_record);

}  // namespace
