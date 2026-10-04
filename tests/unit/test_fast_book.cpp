// Tests for the optimized book (FastBook) and its open-addressing ref map.
//
// FastBook must behave exactly like ReferenceBook, so most of its testing is differential:
// the same synthetic streams (every fixture mode, several seeds) are fed to both books and
// the DifferentialTester compares the top of book after every message and the full depth
// every 500 messages. A few direct tests pin down the cases that a layout change could
// plausibly break (FIFO order inside a level, far prices taking the binary-search path,
// level erase in the middle of a side, pool slot reuse).
#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <unordered_map>

#include "lle/book/differential.hpp"
#include "lle/book/fast_book.hpp"
#include "lle/book/ref_map.hpp"
#include "lle/book/reference_book.hpp"
#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/testing/fixture_gen.hpp"

namespace {

using namespace lle;
using namespace lle::book;
using lle::testing::FixtureConfig;
using lle::testing::FixtureMode;

// ---- RefMap -------------------------------------------------------------------------------

TEST(RefMap, InsertFindEraseBasics) {
    RefMap m(16);
    EXPECT_EQ(m.capacity(), 16u);
    EXPECT_EQ(m.find(5), RefMap::kEmpty);
    EXPECT_TRUE(m.insert(5, 50));
    EXPECT_TRUE(m.insert(0, 7));  // key 0 is a valid key (emptiness is marked in the value)
    EXPECT_FALSE(m.insert(5, 99));
    EXPECT_EQ(m.find(5), 50u);
    EXPECT_EQ(m.find(0), 7u);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m.erase(5), 50u);
    EXPECT_EQ(m.erase(5), RefMap::kEmpty);
    EXPECT_EQ(m.find(5), RefMap::kEmpty);
    EXPECT_EQ(m.size(), 1u);
}

TEST(RefMap, ThrowsWhenFullInsteadOfLoopingForever) {
    RefMap m(8);  // load limit: 7 of 8 slots
    for (std::uint64_t k = 0; k < 7; ++k) ASSERT_TRUE(m.insert(k, static_cast<std::uint32_t>(k)));
    EXPECT_THROW(m.insert(100, 1), std::length_error);
    EXPECT_EQ(m.find(12345), RefMap::kEmpty);  // terminates: one slot is always empty
}

// Randomized against std::unordered_map, in a small table so probe runs are long, wrap
// around the end of the array and get erased from the middle: this is what exercises the
// backward-shift deletion.
template <class Map>
void check_against_unordered_map() {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Map m(64);
        std::unordered_map<std::uint64_t, std::uint32_t> oracle;
        std::mt19937_64 rng(seed);
        for (int op = 0; op < 20'000; ++op) {
            const std::uint64_t key = rng() % 120;  // small key space: many repeats
            const auto r = rng() % 3;
            if (r == 0 && oracle.size() < 50) {
                const auto v = static_cast<std::uint32_t>(rng() % 1000);
                const bool inserted = m.insert(key, v);
                ASSERT_EQ(inserted, oracle.emplace(key, v).second);
            } else if (r == 1) {
                auto it = oracle.find(key);
                const std::uint32_t got = m.erase(key);
                if (it == oracle.end()) {
                    ASSERT_EQ(got, RefMap::kEmpty);
                } else {
                    ASSERT_EQ(got, it->second);
                    oracle.erase(it);
                }
            }
            ASSERT_EQ(m.size(), oracle.size());
            if (op % 97 == 0)
                for (std::uint64_t k = 0; k < 120; ++k) {
                    auto it = oracle.find(k);
                    ASSERT_EQ(m.find(k), it == oracle.end() ? RefMap::kEmpty : it->second) << "seed " << seed;
                }
        }
    }
}

TEST(RefMap, IdentityHashMatchesUnorderedMapUnderHeavyCollisions) {
    check_against_unordered_map<RefMap>();
}
TEST(RefMap, FibonacciHashMatchesUnorderedMapUnderHeavyCollisions) {
    check_against_unordered_map<RefMapFib>();
}

// ---- FastBook direct tests ------------------------------------------------------------------

constexpr Locate kL = 1;

TEST(FastBook, AddsBuildSortedLevelsBestFirst) {
    auto b = std::make_unique<FastBook>(16, 1024, 1024);
    b->add(kL, 1, Side::Buy, 100, 10);
    b->add(kL, 2, Side::Buy, 102, 20);
    b->add(kL, 3, Side::Buy, 101, 30);
    b->add(kL, 4, Side::Sell, 105, 5);
    b->add(kL, 5, Side::Sell, 104, 6);
    b->add(kL, 6, Side::Buy, 102, 1);
    EXPECT_EQ(b->top(kL), (Top{102, 21, 104, 6}));
    std::vector<LevelView> d;
    b->depth(kL, Side::Buy, d);
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[0], (LevelView{102, 21, 2}));
    EXPECT_EQ(d[1], (LevelView{101, 30, 1}));
    EXPECT_EQ(d[2], (LevelView{100, 10, 1}));
    b->depth(kL, Side::Sell, d);
    ASSERT_EQ(d.size(), 2u);
    EXPECT_EQ(d[0], (LevelView{104, 6, 1}));
    EXPECT_EQ(d[1], (LevelView{105, 5, 1}));
}

TEST(FastBook, FifoInsideLevelAndUnlinkFromMiddle) {
    auto b = std::make_unique<FastBook>(16, 1024, 1024);
    for (OrderRef r = 1; r <= 5; ++r) b->add(kL, r, Side::Sell, 200, 10);
    EXPECT_EQ(b->queue_at(kL, Side::Sell, 200), (std::vector<OrderRef>{1, 2, 3, 4, 5}));
    b->remove(kL, 3);  // middle
    b->remove(kL, 1);  // head
    b->remove(kL, 5);  // tail
    EXPECT_EQ(b->queue_at(kL, Side::Sell, 200), (std::vector<OrderRef>{2, 4}));
    b->add(kL, 6, Side::Sell, 200, 10);
    EXPECT_EQ(b->queue_at(kL, Side::Sell, 200), (std::vector<OrderRef>{2, 4, 6}));
    EXPECT_EQ(b->top(kL).ask_qty, 30u);
}

TEST(FastBook, ReplaceLosesPriorityAndPartialExecuteKeepsIt) {
    auto b = std::make_unique<FastBook>(16, 1024, 1024);
    b->add(kL, 1, Side::Buy, 50, 10);
    b->add(kL, 2, Side::Buy, 50, 10);
    b->execute(kL, 1, 4);  // partial: stays first
    EXPECT_EQ(b->queue_at(kL, Side::Buy, 50), (std::vector<OrderRef>{1, 2}));
    b->replace(kL, 1, 3, 6, 50);  // same price, new ref goes to the back
    EXPECT_EQ(b->queue_at(kL, Side::Buy, 50), (std::vector<OrderRef>{2, 3}));
    EXPECT_EQ(b->top(kL), (Top{50, 16, 0, 0}));
    EXPECT_EQ(b->counters().adds, 2u);
    EXPECT_EQ(b->counters().replaces, 1u);
}

// More levels than the linear scan covers, so adds/deletes far from the best take the
// binary-search path, and levels are erased from the middle of the vector.
TEST(FastBook, FarPricesUseBinarySearchAndMatchReference) {
    auto f = std::make_unique<FastBook>(16, 1u << 14, 1u << 15);  // ~6.7k live at the end
    auto r = std::make_unique<ReferenceBook>(16);
    std::mt19937_64 rng(3);
    OrderRef next = 1;
    std::vector<OrderRef> live;
    std::vector<LevelView> df, dr;
    for (int i = 0; i < 20'000; ++i) {
        if (live.empty() || rng() % 3 != 0) {
            const Side s = rng() % 2 ? Side::Buy : Side::Sell;
            const auto off = static_cast<Price>(rng() % 300);
            const Price px = s == Side::Buy ? 1000 - off : 1001 + off;
            const auto q = 1 + static_cast<Qty>(rng() % 50);
            f->add(kL, next, s, px, q);
            r->add(kL, next, s, px, q);
            live.push_back(next++);
        } else {
            const std::size_t k = rng() % live.size();
            f->remove(kL, live[k]);
            r->remove(kL, live[k]);
            live[k] = live.back();
            live.pop_back();
        }
        ASSERT_EQ(f->top(kL), r->top(kL)) << "step " << i;
        if (i % 500 == 0)
            for (const Side s : {Side::Buy, Side::Sell}) {
                f->depth(kL, s, df);
                r->depth(kL, s, dr);
                ASSERT_EQ(df, dr) << "step " << i;
            }
    }
    f->depth(kL, Side::Buy, df);
    EXPECT_GT(df.size(), FastBook::kLinearScan * 4);  // the binary-search path was exercised
    EXPECT_EQ(f->live_orders(), r->live_orders());
}

TEST(FastBook, PoolSlotsAreReusedSoTheBookRunsForeverInAFixedPool) {
    auto b = std::make_unique<FastBook>(16, 8, 16);  // only 8 order slots
    for (OrderRef r = 1; r <= 100'000; ++r) {
        b->add(kL, r, Side::Buy, 10 + static_cast<Price>(r % 5), 1);
        if (r > 4) b->remove(kL, r - 4);  // at most 5 live
    }
    EXPECT_EQ(b->live_orders(), 4u);
    EXPECT_EQ(b->counters().unknown_ref, 0u);
}

TEST(FastBook, AnomalyCountersMatchReferenceSemantics) {
    auto b = std::make_unique<FastBook>(16, 64, 64);
    b->add(kL, 1, Side::Buy, 10, 5);
    b->add(kL, 1, Side::Buy, 11, 5);  // duplicate
    b->add(kL, 2, Side::Buy, 10, 0);  // zero shares
    b->add(99, 3, Side::Buy, 10, 5);  // bad locate (max 16)
    b->execute(kL, 77, 1);            // unknown
    b->execute(2, 1, 9);              // locate mismatch + overfill: order dies
    const BookCounters& c = b->counters();
    EXPECT_EQ(c.duplicate_ref, 1u);
    EXPECT_EQ(c.zero_qty, 1u);
    EXPECT_EQ(c.bad_locate, 1u);
    EXPECT_EQ(c.unknown_ref, 1u);
    EXPECT_EQ(c.locate_mismatch, 1u);
    EXPECT_EQ(c.overfill, 1u);
    EXPECT_EQ(b->live_orders(), 0u);
    EXPECT_EQ(b->top(kL), Top{});
}

// ---- Differential: FastBook vs ReferenceBook on every fixture mode ------------------------

struct DiffCase {
    FixtureMode mode;
    std::uint64_t seed;
};

template <class Fast>
void run_diff(const std::vector<std::byte>& data) {
    auto ref = std::make_unique<ReferenceBook>(8192);
    auto fast = std::make_unique<Fast>(8192, 1u << 18, 1u << 18);
    auto t = std::make_unique<DifferentialTester<ReferenceBook, Fast>>(*ref, *fast, 500);
    itch::for_each_framed(data, [&](const std::byte* p, std::size_t n) {
        const auto st = itch::decode(p, n, *t);
        if (st == itch::DecodeStatus::Truncated || st == itch::DecodeStatus::Empty) t->on_undecodable();
    });
    t->finish();
    EXPECT_FALSE(t->failed()) << "message " << t->mismatch().message_index << " locate " << t->mismatch().locate << ": "
                              << t->mismatch().what;
    EXPECT_GT(t->top_checks(), 40'000u);
}

class FastVsReference : public ::testing::TestWithParam<DiffCase> {};

TEST_P(FastVsReference, AgreesAfterEveryMessage) {
    FixtureConfig c;
    c.mode = GetParam().mode;
    c.seed = GetParam().seed;
    c.messages = 60'000;
    c.symbols = 40;
    const auto data = lle::testing::generate_fixture_bytes(c);
    auto ref = std::make_unique<ReferenceBook>(8192);
    run_diff<FastBook>(data);
    run_diff<FastBookFib>(data);
}

std::vector<DiffCase> diff_cases() {
    std::vector<DiffCase> v;
    for (const FixtureMode m :
         {FixtureMode::Realistic, FixtureMode::DeepQueue, FixtureMode::WidePrices, FixtureMode::ReplaceChains,
          FixtureMode::Crossing, FixtureMode::ManySymbols, FixtureMode::ErrorPaths, FixtureMode::Mixed})
        for (std::uint64_t seed = 1; seed <= 3; ++seed) v.push_back({m, seed});
    return v;
}

INSTANTIATE_TEST_SUITE_P(Modes, FastVsReference, ::testing::ValuesIn(diff_cases()), [](const auto& param_info) {
    std::string n = lle::testing::fixture_mode_name(param_info.param.mode);
    for (auto& ch : n)
        if (ch == '-') ch = '_';
    return n + "_seed" + std::to_string(param_info.param.seed);
});

}  // namespace
