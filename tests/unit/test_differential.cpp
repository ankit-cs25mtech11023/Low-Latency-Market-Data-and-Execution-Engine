// Tests for the synthetic fixture generator and the differential tester.
//
// The differential tester is only useful if it really detects divergence, so besides
// "reference vs reference agrees", it is run against mutant books with planted bugs and
// must stop at the message that introduced each bug.
#include "lle/book/differential.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <memory>

#include "lle/book/reference_book.hpp"
#include "lle/protocol/itch.hpp"
#include "lle/protocol/endian.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/protocol/itch_slice.hpp"
#include "lle/testing/fixture_gen.hpp"

namespace {

using namespace lle;
using namespace lle::book;
using lle::testing::FixtureConfig;
using lle::testing::FixtureMode;

// Feeds a framed in-memory stream to a handler, message by message.
template <class H>
void replay(const std::vector<std::byte>& data, H& h) {
    itch::for_each_framed(data, [&](const std::byte* p, std::size_t n) { (void)itch::decode(p, n, h); });
}

FixtureConfig cfg(FixtureMode m, std::uint64_t n = 40'000, std::uint64_t seed = 7) {
    FixtureConfig c;
    c.mode = m;
    c.messages = n;
    c.seed = seed;
    c.symbols = 30;
    return c;
}

// ---- fixture generator --------------------------------------------------------------------

TEST(FixtureGen, SameSeedSameBytesDifferentSeedDifferentBytes) {
    const auto a = lle::testing::generate_fixture_bytes(cfg(FixtureMode::Mixed, 20'000, 1));
    const auto b = lle::testing::generate_fixture_bytes(cfg(FixtureMode::Mixed, 20'000, 1));
    const auto c = lle::testing::generate_fixture_bytes(cfg(FixtureMode::Mixed, 20'000, 2));
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(FixtureGen, ModeNamesRoundTrip) {
    for (const char* n : {"realistic", "deep-queue", "wide-prices", "replace-chains", "crossing", "many-symbols",
                          "error-paths", "mixed"})
        EXPECT_STREQ(lle::testing::fixture_mode_name(lle::testing::parse_fixture_mode(n)), n);
    EXPECT_THROW((void)lle::testing::parse_fixture_mode("nope"), std::invalid_argument);
}

// A valid mode must produce a stream the reference book processes with zero anomalies:
// the generator only references live orders with valid quantities, as the real feed does.
class ValidModes : public ::testing::TestWithParam<FixtureMode> {};

TEST_P(ValidModes, ReferenceBookSeesNoAnomalies) {
    const auto data = lle::testing::generate_fixture_bytes(cfg(GetParam()));
    auto book = std::make_unique<ReferenceBook>();
    auto dir = std::make_unique<SymbolDirectory>();
    BookBuilder<ReferenceBook> bb(*book, *dir);
    replay(data, bb);
    const BookCounters& c = book->counters();
    EXPECT_EQ(c.unknown_ref, 0u);
    EXPECT_EQ(c.duplicate_ref, 0u);
    EXPECT_EQ(c.overfill, 0u);
    EXPECT_EQ(c.locate_mismatch, 0u);
    EXPECT_EQ(c.bad_locate, 0u);
    EXPECT_EQ(c.zero_qty, 0u);
    EXPECT_GT(c.adds, 1000u);
    EXPECT_GT(bb.counts().total, 40'000u);
    EXPECT_EQ(bb.last_system_event(), 'C') << "stream ends with end-of-messages";
    EXPECT_TRUE(dir->known(1));
}

INSTANTIATE_TEST_SUITE_P(Modes, ValidModes,
                         ::testing::Values(FixtureMode::Realistic, FixtureMode::DeepQueue, FixtureMode::WidePrices,
                                           FixtureMode::ReplaceChains, FixtureMode::Crossing,
                                           FixtureMode::ManySymbols),
                         [](const auto& param_info) {
                             std::string n = lle::testing::fixture_mode_name(param_info.param);
                             for (auto& ch : n)
                                 if (ch == '-') ch = '_';
                             return n;
                         });

TEST(FixtureGen, ErrorPathsTriggerEveryAnomalyCounter) {
    const auto data = lle::testing::generate_fixture_bytes(cfg(FixtureMode::ErrorPaths, 100'000));
    auto book = std::make_unique<ReferenceBook>();
    auto dir = std::make_unique<SymbolDirectory>();
    BookBuilder<ReferenceBook> bb(*book, *dir);
    replay(data, bb);
    const BookCounters& c = book->counters();
    EXPECT_GT(c.unknown_ref, 0u);
    EXPECT_GT(c.duplicate_ref, 0u);
    EXPECT_GT(c.overfill, 0u);
    EXPECT_GT(c.locate_mismatch, 0u);
    EXPECT_GT(c.zero_qty, 0u);
    EXPECT_GT(bb.counts().by_type[static_cast<unsigned char>('z')], 0u) << "unknown message type injected";
}

TEST(FixtureGen, CrossingModeProducesCrossedAndLockedBooks) {
    const auto data = lle::testing::generate_fixture_bytes(cfg(FixtureMode::Crossing));
    auto a = std::make_unique<ReferenceBook>();
    auto b = std::make_unique<ReferenceBook>();
    auto t = std::make_unique<DifferentialTester<ReferenceBook, ReferenceBook>>(*a, *b);
    replay(data, *t);
    EXPECT_GT(t->invariants().crossed, 0u);
    EXPECT_GT(t->invariants().locked, 0u);
}

// ---- differential tester ------------------------------------------------------------------

class AllModes : public ::testing::TestWithParam<FixtureMode> {};

TEST_P(AllModes, ReferenceAgreesWithItself) {
    const auto data = lle::testing::generate_fixture_bytes(cfg(GetParam()));
    auto a = std::make_unique<ReferenceBook>();
    auto b = std::make_unique<ReferenceBook>();
    auto t = std::make_unique<DifferentialTester<ReferenceBook, ReferenceBook>>(*a, *b, 1000);
    replay(data, *t);
    t->finish();
    EXPECT_FALSE(t->failed()) << t->mismatch().what;
    EXPECT_GT(t->top_checks(), 30'000u);
    EXPECT_GT(t->full_checks(), 30u);
    EXPECT_EQ(t->invariants().empty_level, 0u);
    EXPECT_EQ(t->invariants().unsorted, 0u);
}

INSTANTIATE_TEST_SUITE_P(Modes, AllModes,
                         ::testing::Values(FixtureMode::Realistic, FixtureMode::DeepQueue, FixtureMode::WidePrices,
                                           FixtureMode::ReplaceChains, FixtureMode::Crossing,
                                           FixtureMode::ManySymbols, FixtureMode::ErrorPaths, FixtureMode::Mixed),
                         [](const auto& param_info) {
                             std::string n = lle::testing::fixture_mode_name(param_info.param);
                             for (auto& ch : n)
                                 if (ch == '-') ch = '_';
                             return n;
                         });

// Mutant 1: silently drops the N-th partial cancel. The very next top-of-book check of that
// symbol must differ, so the tester must stop exactly at that X message.
class DropsNthCancel : public ReferenceBook {
public:
    explicit DropsNthCancel(std::uint64_t n) : n_(n) {}
    void cancel(Locate loc, OrderRef ref, Qty qty) {
        if (++seen_ == n_) {
            dropped_ = true;
            return;
        }
        ReferenceBook::cancel(loc, ref, qty);
    }
    bool dropped_ = false;

private:
    std::uint64_t n_, seen_ = 0;
};

TEST(Differential, CatchesDroppedCancelAtTheExactMessage) {
    const auto data = lle::testing::generate_fixture_bytes(cfg(FixtureMode::DeepQueue));
    auto a = std::make_unique<ReferenceBook>();
    auto b = std::make_unique<DropsNthCancel>(50);
    // full_every = 1: depth is compared after every message, so even a cancel at a level
    // below the best is caught at the X message itself.
    auto t = std::make_unique<DifferentialTester<ReferenceBook, DropsNthCancel>>(*a, *b, 1);
    replay(data, *t);
    ASSERT_TRUE(b->dropped_);
    ASSERT_TRUE(t->failed());
    EXPECT_EQ(t->mismatch().locate, 1);
    EXPECT_EQ(t->mismatch().message_type, 'X') << t->mismatch().what;
}

// Mutant 2: a bug invisible at the top of the book. Orders far from the touch (deep asks)
// get one extra share. Only the periodic full-depth comparison can see it.
class InflatesDeepAsks : public ReferenceBook {
public:
    void add(Locate loc, OrderRef ref, Side side, Price px, Qty qty) {
        const Top t = top(loc);
        const bool deep = side == Side::Sell && t.ask_qty != 0 && px > t.ask_px + 5 * 100;
        ReferenceBook::add(loc, ref, side, px, deep ? qty + 1 : qty);
    }
};

TEST(Differential, FullDepthCheckCatchesBugsBelowTheTop) {
    const auto data = lle::testing::generate_fixture_bytes(cfg(FixtureMode::Realistic));
    auto a = std::make_unique<ReferenceBook>();
    auto b = std::make_unique<InflatesDeepAsks>();
    // The full-depth check catches it at the add that planted it...
    auto t = std::make_unique<DifferentialTester<ReferenceBook, InflatesDeepAsks>>(*a, *b, 1);
    replay(data, *t);
    ASSERT_TRUE(t->failed());
    EXPECT_NE(t->mismatch().what.find("ask depth differs"), std::string::npos) << t->mismatch().what;
    // ...while the top-of-book check alone sees it only later (once cancels and executions
    // have moved an inflated order up to the best ask) or never. That delay is why the
    // tester also compares full depth periodically.
    auto a0 = std::make_unique<ReferenceBook>();
    auto b0 = std::make_unique<InflatesDeepAsks>();
    auto t0 = std::make_unique<DifferentialTester<ReferenceBook, InflatesDeepAsks>>(*a0, *b0, UINT64_MAX);
    replay(data, *t0);
    if (t0->failed()) {
        EXPECT_GT(t0->mismatch().message_index, t->mismatch().message_index)
            << "top-only checking cannot see a deep-level bug before full-depth checking does";
    }
    EXPECT_TRUE(t->mismatch().message_type == 'A' || t->mismatch().message_type == 'F');
}

TEST(Differential, MismatchIsReproducedFromAOneSymbolSlice) {
    // Full loop used on real data: run the diff on a big stream, cut the offending symbol's
    // messages up to the mismatch into a small file, replay that file, get the same mismatch.
    const auto data = lle::testing::generate_fixture_bytes(cfg(FixtureMode::Realistic, 60'000));
    const auto dir = std::filesystem::temp_directory_path() / ("lle_repro_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::string repro = (dir / "repro.itch").string();

    auto a = std::make_unique<ReferenceBook>();
    auto b = std::make_unique<DropsNthCancel>(200);
    auto t = std::make_unique<DifferentialTester<ReferenceBook, DropsNthCancel>>(*a, *b, 1);
    replay(data, *t);
    ASSERT_TRUE(t->failed());
    const Mismatch m = t->mismatch();

    itch::ItchFileReader in(itch::memory_source(data));
    const auto st = itch::write_locate_slice(in, m.locate, m.message_index, repro);
    EXPECT_LT(st.written, st.read / 2) << "the slice should be much smaller than the stream";

    // Replaying only the slice: the mutant must drop the same cancel. Count how many cancels
    // of that symbol came before it in the full stream, so the mutant drops the same one.
    std::uint64_t cancels_before = 0, idx = 0;
    itch::for_each_framed(data, [&](const std::byte* p, std::size_t /*len*/) {
        if (idx++ < m.message_index && static_cast<char>(p[0]) == 'X' && proto::load_be<std::uint16_t>(p + 1) == m.locate)
            ++cancels_before;
    });
    auto a2 = std::make_unique<ReferenceBook>();
    auto b2 = std::make_unique<DropsNthCancel>(cancels_before + 1);
    auto t2 = std::make_unique<DifferentialTester<ReferenceBook, DropsNthCancel>>(*a2, *b2, 1);
    itch::ItchFileReader slice(itch::open_source(repro));
    const std::byte* p = nullptr;
    std::size_t n = 0;
    while (slice.next(p, n)) (void)itch::decode(p, n, *t2);
    ASSERT_TRUE(t2->failed());
    EXPECT_EQ(t2->mismatch().locate, m.locate);
    EXPECT_EQ(t2->mismatch().message_type, 'X');
    EXPECT_EQ(t2->mismatch().what, m.what) << "same divergence, from the slice alone";
    std::filesystem::remove_all(dir);
}

TEST(Differential, FinishComparesCounters) {
    // A mutant that counts an extra add on every replace: books agree, counters do not.
    struct CountsWrong : ReferenceBook {
        void replace(Locate l, OrderRef o, OrderRef n, Qty q, Price p) {
            ReferenceBook::replace(l, o, n, q, p);
            ReferenceBook::add(l, o, Side::Buy, 0, 0);  // zero-share add: no book change, bumps counters
        }
    };
    const auto data = lle::testing::generate_fixture_bytes(cfg(FixtureMode::ReplaceChains, 5'000));
    auto a = std::make_unique<ReferenceBook>();
    auto b = std::make_unique<CountsWrong>();
    auto t = std::make_unique<DifferentialTester<ReferenceBook, CountsWrong>>(*a, *b);
    replay(data, *t);
    EXPECT_FALSE(t->failed()) << "books themselves agree until finish()";
    t->finish();
    ASSERT_TRUE(t->failed());
    EXPECT_NE(t->mismatch().what.find("counter"), std::string::npos) << t->mismatch().what;
}

}  // namespace
