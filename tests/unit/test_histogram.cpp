#include "lle/telemetry/histogram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace {

using lle::Histogram;
using H = lle::Histogram;

// Exact nearest-rank quantile over sorted data, the definition the histogram approximates.
std::uint64_t exact_quantile(const std::vector<std::uint64_t>& sorted, double q) {
    if (q <= 0.0) return sorted.front();
    const auto rank = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size()))));
    return sorted[rank - 1];
}

TEST(Histogram, SmallValuesAreExact) {
    for (std::uint64_t v = 0; v < H::kSubBuckets * 2; ++v) {
        const auto idx = H::index_of(v);
        EXPECT_EQ(H::lowest_of(idx), v);
        EXPECT_EQ(H::highest_of(idx), v) << "value " << v;
    }
}

TEST(Histogram, BucketsAreContiguousAndOrdered) {
    // Every bucket's range starts right after the previous one ends, covering all of uint64.
    EXPECT_EQ(H::lowest_of(0), 0u);
    for (std::size_t i = 1; i < H::kBucketCount; ++i) {
        ASSERT_EQ(H::lowest_of(i), H::highest_of(i - 1) + 1) << "bucket " << i;
        ASSERT_LE(H::lowest_of(i), H::highest_of(i));
    }
    EXPECT_EQ(H::highest_of(H::kBucketCount - 1), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(H::index_of(std::numeric_limits<std::uint64_t>::max()), H::kBucketCount - 1);
}

TEST(Histogram, IndexRoundTripsAtBoundaries) {
    for (std::size_t i = 0; i < H::kBucketCount; ++i) {
        ASSERT_EQ(H::index_of(H::lowest_of(i)), i);
        ASSERT_EQ(H::index_of(H::highest_of(i)), i);
    }
}

TEST(Histogram, RelativeErrorBound) {
    // Bucket width / bucket low < 2^-P for every bucket above the exact range.
    // Checked in integer arithmetic: width < low / 2^P, i.e. width < (low >> P).
    // (A double-precision ratio rounds to exactly 2^-P for the huge top buckets, where
    // width = 2^k - 1 and low = 2^(k+P), and would report a false failure.)
    for (std::size_t i = H::kSubBuckets; i < H::kBucketCount; ++i) {
        const std::uint64_t lo = H::lowest_of(i);
        const std::uint64_t width = H::highest_of(i) - lo;
        ASSERT_LT(width, lo >> H::kPrecisionBits) << "bucket " << i;
    }
}

TEST(Histogram, EmptyHistogram) {
    auto h = std::make_unique<Histogram>();
    EXPECT_EQ(h->count(), 0u);
    EXPECT_EQ(h->min(), 0u);
    EXPECT_EQ(h->max(), 0u);
    EXPECT_EQ(h->value_at_quantile(0.5), 0u);
    EXPECT_DOUBLE_EQ(h->mean(), 0.0);
}

TEST(Histogram, SummaryStatsAreExact) {
    auto h = std::make_unique<Histogram>();
    for (std::uint64_t v : {5u, 1000u, 123456u, 7u}) h->record(v);
    EXPECT_EQ(h->count(), 4u);
    EXPECT_EQ(h->min(), 5u);
    EXPECT_EQ(h->max(), 123456u);
    EXPECT_DOUBLE_EQ(h->mean(), (5.0 + 1000.0 + 123456.0 + 7.0) / 4.0);
    EXPECT_EQ(h->value_at_quantile(1.0), 123456u);
    EXPECT_EQ(h->value_at_quantile(0.0), 5u);
}

TEST(Histogram, QuantilesMatchExactWithinBound) {
    std::mt19937_64 rng(42);
    // Log-normal-ish spread similar to real latency data: most small, long tail.
    std::lognormal_distribution<double> dist(5.0, 1.5);
    std::vector<std::uint64_t> data;
    auto h = std::make_unique<Histogram>();
    for (int i = 0; i < 200'000; ++i) {
        const auto v = static_cast<std::uint64_t>(dist(rng));
        data.push_back(v);
        h->record(v);
    }
    std::ranges::sort(data);
    for (double q : {0.01, 0.1, 0.5, 0.9, 0.99, 0.999, 0.9999}) {
        const auto exact = exact_quantile(data, q);
        const auto approx = h->value_at_quantile(q);
        // The histogram reports the top of the exact value's bucket: never below it,
        // and above it by less than one bucket width.
        EXPECT_GE(approx, exact) << "q=" << q;
        EXPECT_EQ(H::index_of(approx), H::index_of(exact)) << "q=" << q;
    }
}

TEST(Histogram, RecordNAndMergeEquivalentToRecord) {
    auto a = std::make_unique<Histogram>();
    auto b = std::make_unique<Histogram>();
    auto c = std::make_unique<Histogram>();
    for (int i = 0; i < 10; ++i) a->record(300);
    b->record_n(300, 10);
    EXPECT_EQ(a->value_at_quantile(0.5), b->value_at_quantile(0.5));
    EXPECT_EQ(a->count(), b->count());
    c->record(1);
    c->merge(*a);
    EXPECT_EQ(c->count(), 11u);
    EXPECT_EQ(c->min(), 1u);
    EXPECT_EQ(c->max(), 300u);
    c->reset();
    EXPECT_EQ(c->count(), 0u);
    EXPECT_EQ(c->max(), 0u);
}

TEST(Histogram, MaxValueDoesNotOverflow) {
    auto h = std::make_unique<Histogram>();
    h->record(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(h->value_at_quantile(0.5), std::numeric_limits<std::uint64_t>::max());
}

}  // namespace
