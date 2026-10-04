// Tests for the SPSC queues (lle/concurrency/spsc.hpp).
//
// Single-thread tests pin down the index arithmetic (empty, full, wraparound, FIFO order).
// The two-thread stress test is the one that matters for the memory ordering: run under the
// tsan preset, ThreadSanitizer reports a data race if a slot could be read before the
// producer's write to it is visible, or overwritten before the consumer finished reading
// it. Random delays on both sides make the threads meet in many different interleavings
// (ring nearly empty, nearly full, both sides racing on the same slot).
// LLE_SPSC_STRESS_OPS sets the message count per queue (default 1 M; the E3 write-up cites
// the count used for the local 10^8 run).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>

#include "lle/concurrency/spsc.hpp"
#include "lle/core/cpu.hpp"

namespace {

using lle::concurrency::MutexCvQueue;
using lle::concurrency::MutexSpinQueue;
using lle::concurrency::Ordering;
using lle::concurrency::SpscRing;

struct Msg {
    std::uint64_t seq;
    std::uint64_t check;  // derived from seq, so a torn or stale slot is detected
};

constexpr std::uint64_t check_of(std::uint64_t seq) {
    return seq * 0x9E3779B97F4A7C15ULL + 1;
}

template <class Q>
class SpscTest : public ::testing::Test {};

template <std::size_t Cap>
using AllQueues =
    ::testing::Types<SpscRing<Msg, Cap, Ordering::kSeqCst, false, 64>, SpscRing<Msg, Cap, Ordering::kAcqRel, false, 0>,
                     SpscRing<Msg, Cap, Ordering::kAcqRel, false, 64>, SpscRing<Msg, Cap, Ordering::kAcqRel, true, 0>,
                     SpscRing<Msg, Cap, Ordering::kAcqRel, true, 64>, SpscRing<Msg, Cap, Ordering::kAcqRel, true, 128>,
                     MutexSpinQueue<Msg, Cap>, MutexCvQueue<Msg, Cap>>;

TYPED_TEST_SUITE(SpscTest, AllQueues<8>);

template <class Q>
constexpr bool kNonBlocking = requires(Q q, Msg m) { q.try_pop(m); };

TYPED_TEST(SpscTest, EmptyThenFullThenEmpty) {
    if constexpr (!kNonBlocking<TypeParam>) {
        GTEST_SKIP() << "blocking queue: no try_push/try_pop";
    } else {
        auto q = std::make_unique<TypeParam>();
        Msg m{};
        EXPECT_FALSE(q->try_pop(m)) << "a new queue is empty";
        for (std::uint64_t i = 0; i < TypeParam::kCapacity; ++i) EXPECT_TRUE(q->try_push({i, check_of(i)}));
        EXPECT_FALSE(q->try_push({99, 0})) << "capacity slots are usable, one more is not";
        for (std::uint64_t i = 0; i < TypeParam::kCapacity; ++i) {
            ASSERT_TRUE(q->try_pop(m));
            EXPECT_EQ(m.seq, i);
        }
        EXPECT_FALSE(q->try_pop(m)) << "empty again after draining";
    }
}

TYPED_TEST(SpscTest, WraparoundKeepsFifoOrder) {
    // Pushes and pops in uneven chunks so head and tail cross the end of the slot array at
    // every possible offset, many times.
    auto q = std::make_unique<TypeParam>();
    std::uint64_t next_push = 0, next_pop = 0;
    for (int round = 0; round < 1000; ++round) {
        const std::uint64_t n_push = 1 + static_cast<std::uint64_t>(round) % TypeParam::kCapacity;
        for (std::uint64_t i = 0; i < n_push && next_push - next_pop < TypeParam::kCapacity; ++i, ++next_push)
            lle::concurrency::push_blocking(*q, Msg{next_push, check_of(next_push)});
        const std::uint64_t n_pop = 1 + static_cast<std::uint64_t>(round * 7) % TypeParam::kCapacity;
        for (std::uint64_t i = 0; i < n_pop && next_pop < next_push; ++i, ++next_pop) {
            Msg m{};
            lle::concurrency::pop_blocking(*q, m);
            ASSERT_EQ(m.seq, next_pop);
            ASSERT_EQ(m.check, check_of(next_pop));
        }
    }
    EXPECT_GT(next_pop, 100 * TypeParam::kCapacity) << "indices wrapped many times";
}

std::uint64_t stress_ops() {
    if (const char* s = std::getenv("LLE_SPSC_STRESS_OPS")) return std::strtoull(s, nullptr, 10);
    return 1'000'000;
}

// xorshift64: cheap per-thread randomness for the delays.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

void random_delay(Rng& r) {
    const std::uint64_t x = r.next();
    if ((x & 63) == 0) {  // ~1 message in 64 waits a random 0-1023 iterations
        for (std::uint64_t i = 0, n = (x >> 8) & 1023; i < n; ++i) lle::compiler_barrier();
    }
}

TYPED_TEST(SpscTest, TwoThreadStressDeliversEveryMessageInOrder) {
    const std::uint64_t n = stress_ops();
    auto q = std::make_unique<TypeParam>();
    std::thread producer([&] {
        Rng r{0x1234567ULL};
        for (std::uint64_t i = 0; i < n; ++i) {
            random_delay(r);
            lle::concurrency::push_blocking(*q, Msg{i, check_of(i)});
        }
    });
    Rng r{0x89ABCDEFULL};
    std::uint64_t bad = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        random_delay(r);
        Msg m{};
        lle::concurrency::pop_blocking(*q, m);
        bad += (m.seq != i || m.check != check_of(i)) ? 1U : 0U;
    }
    producer.join();
    EXPECT_EQ(bad, 0U) << "messages lost, duplicated, reordered or torn";
    if constexpr (kNonBlocking<TypeParam>) {
        Msg m{};
        EXPECT_FALSE(q->try_pop(m)) << "nothing left after n pops";
    }
}

}  // namespace
