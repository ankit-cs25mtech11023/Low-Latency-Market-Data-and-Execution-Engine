#pragma once
// Log-linear latency histogram (the idea behind HdrHistogram).
//
// Latency distributions span many orders of magnitude (a 30-cycle common case and a
// 5-million-cycle page-fault outlier), and what matters is *relative* precision:
// "p99 = 1 012 ns ± 1%" is useful, "± 1 ns" is not needed at 1 ms. A log-linear layout
// gives exactly that with fixed memory:
//
//   * values below 2^P are counted exactly (one bucket per value);
//   * every power-of-two range [2^e, 2^(e+1)) above that is split into 2^P equal
//     sub-buckets, so any recorded value is known to within a relative error < 2^-P.
//
// With P = 7 that is < 0.79% error across the whole uint64 range, in 7 424 buckets.
//
// Hot-path properties: record() is a handful of instructions (one count-leading-zeros,
// a shift, an add, an increment), never allocates and never branches on the value's
// magnitude except for min/max tracking. Construct the histogram before the hot path starts.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace lle {

template <unsigned PrecisionBits = 7>
class LogLinearHistogram {
    static_assert(PrecisionBits >= 1 && PrecisionBits <= 16, "precision bits out of range");

public:
    static constexpr unsigned kPrecisionBits = PrecisionBits;
    static constexpr std::uint64_t kSubBuckets = std::uint64_t{1} << PrecisionBits;
    static constexpr std::size_t kBucketCount = static_cast<std::size_t>((65 - PrecisionBits) * kSubBuckets);

    // Bucket index for value v.
    //   shift = max(bit_width(v) - 1 - P, 0)  -> how many low bits are below the precision
    //   index = shift * 2^P + (v >> shift)
    // For v < 2^P, shift = 0 and the index is v itself (exact). For larger v, (v >> shift)
    // lies in [2^P, 2^(P+1)), so consecutive power-of-two ranges map to consecutive blocks.
    [[nodiscard]] static constexpr std::size_t index_of(std::uint64_t v) noexcept {
        const int width = std::bit_width(v);  // 0 for v == 0
        const int shift = std::max(width - 1 - static_cast<int>(PrecisionBits), 0);
        return (static_cast<std::size_t>(shift) << PrecisionBits) + static_cast<std::size_t>(v >> shift);
    }

    // Smallest value that maps to bucket `idx`.
    [[nodiscard]] static constexpr std::uint64_t lowest_of(std::size_t idx) noexcept {
        const std::size_t group = idx >> PrecisionBits;
        if (group == 0) return idx;
        const auto shift = static_cast<unsigned>(group - 1);
        const std::uint64_t mantissa = idx - (static_cast<std::uint64_t>(shift) << PrecisionBits);
        return mantissa << shift;
    }

    // Largest value that maps to bucket `idx` (HdrHistogram's "highest equivalent value").
    [[nodiscard]] static constexpr std::uint64_t highest_of(std::size_t idx) noexcept {
        const std::size_t group = idx >> PrecisionBits;
        if (group == 0) return idx;
        const auto shift = static_cast<unsigned>(group - 1);
        return lowest_of(idx) + ((std::uint64_t{1} << shift) - 1);
    }

    void record(std::uint64_t v) noexcept {
        ++counts_[index_of(v)];
        ++count_;
        sum_ += v;
        min_ = std::min(min_, v);
        max_ = std::max(max_, v);
    }

    void record_n(std::uint64_t v, std::uint64_t n) noexcept {
        if (n == 0) return;
        counts_[index_of(v)] += n;
        count_ += n;
        sum_ += v * n;
        min_ = std::min(min_, v);
        max_ = std::max(max_, v);
    }

    void merge(const LogLinearHistogram& o) noexcept {
        for (std::size_t i = 0; i < kBucketCount; ++i) counts_[i] += o.counts_[i];
        count_ += o.count_;
        sum_ += o.sum_;
        min_ = std::min(min_, o.min_);
        max_ = std::max(max_, o.max_);
    }

    void reset() noexcept {
        counts_.fill(0);
        count_ = 0;
        sum_ = 0;
        min_ = std::numeric_limits<std::uint64_t>::max();
        max_ = 0;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }
    [[nodiscard]] std::uint64_t bucket_count(std::size_t idx) const noexcept { return counts_[idx]; }

    // Value at quantile q in [0, 1] using the nearest-rank definition: the smallest recorded
    // value v such that at least ceil(q * count) samples are <= v. Reported as the highest
    // value of the bucket (an upper bound, like HdrHistogram), clamped to the exact max.
    [[nodiscard]] std::uint64_t value_at_quantile(double q) const noexcept {
        if (count_ == 0) return 0;
        if (q <= 0.0) return min_;
        if (q >= 1.0) return max_;
        const auto rank =
            std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count_))));
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            cum += counts_[i];
            if (cum >= rank) return std::min(highest_of(i), max_);
        }
        return max_;
    }

    // Writes the non-empty buckets as CSV rows "low,high,count" (header included).
    // This is the raw per-run output format consumed by scripts/analyse.py.
    void write_csv(std::FILE* f) const {
        std::fprintf(f, "low,high,count\n");
        for (std::size_t i = 0; i < kBucketCount; ++i)
            if (counts_[i] != 0)
                std::fprintf(f, "%llu,%llu,%llu\n", static_cast<unsigned long long>(lowest_of(i)),
                             static_cast<unsigned long long>(std::min(highest_of(i), max_)),
                             static_cast<unsigned long long>(counts_[i]));
    }

private:
    std::array<std::uint64_t, kBucketCount> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_ = 0;
};

using Histogram = LogLinearHistogram<7>;

}  // namespace lle
