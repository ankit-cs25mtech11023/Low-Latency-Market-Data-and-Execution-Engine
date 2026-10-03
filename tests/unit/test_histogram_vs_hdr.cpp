// Cross-validation against HdrHistogram_c, the reference implementation of the idea.
// Both histograms see identical data; their quantiles must agree within the coarser
// of the two precisions (ours: 2^-7 ≈ 0.78%; HdrHistogram with 3 significant digits: ~0.1%).
#include "lle/telemetry/histogram.hpp"

#include <gtest/gtest.h>
#include <hdr/hdr_histogram.h>

#include <cmath>
#include <memory>
#include <random>

namespace {

struct HdrDeleter {
    void operator()(hdr_histogram* h) const noexcept { hdr_close(h); }
};

void compare_on(const std::vector<std::uint64_t>& data) {
    hdr_histogram* raw = nullptr;
    ASSERT_EQ(hdr_init(1, INT64_C(1) << 40, 3, &raw), 0);
    std::unique_ptr<hdr_histogram, HdrDeleter> hdr(raw);
    auto ours = std::make_unique<lle::Histogram>();
    for (auto v : data) {
        ours->record(v);
        ASSERT_TRUE(hdr_record_value(hdr.get(), static_cast<int64_t>(v)));
    }
    EXPECT_EQ(static_cast<std::int64_t>(ours->count()), hdr->total_count);
    EXPECT_EQ(static_cast<std::int64_t>(ours->max()), hdr_max(hdr.get()));
    EXPECT_EQ(static_cast<std::int64_t>(ours->min()), hdr_min(hdr.get()));
    for (double q : {0.5, 0.9, 0.99, 0.999, 0.9999}) {
        const double a = static_cast<double>(ours->value_at_quantile(q));
        const double b = static_cast<double>(hdr_value_at_percentile(hdr.get(), q * 100.0));
        const double rel = std::abs(a - b) / std::max(b, 1.0);
        EXPECT_LT(rel, 1.0 / 128 + 1.0 / 1024) << "q=" << q << " ours=" << a << " hdr=" << b;
    }
}

TEST(HistogramVsHdr, LogNormal) {
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> dist(6.0, 1.2);
    std::vector<std::uint64_t> data(500'000);
    for (auto& v : data) v = static_cast<std::uint64_t>(dist(rng)) + 1;
    compare_on(data);
}

TEST(HistogramVsHdr, BimodalWithOutliers) {
    std::mt19937_64 rng(11);
    std::normal_distribution<double> fast(40.0, 3.0), slow(2'000.0, 200.0);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<std::uint64_t> data(500'000);
    for (auto& v : data) {
        const double r = u(rng);
        double x = r < 0.95 ? fast(rng) : (r < 0.9999 ? slow(rng) : 5'000'000.0 * u(rng));
        v = static_cast<std::uint64_t>(std::max(1.0, x));
    }
    compare_on(data);
}

}  // namespace
