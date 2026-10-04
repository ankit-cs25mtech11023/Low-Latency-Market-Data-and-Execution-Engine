// Reference order book tests: one test per ITCH book event and per edge case.
//
// The reference book is the oracle for the differential tester, so its behaviour must be
// pinned down independently here. Prices use ITCH's 4 implied decimals (1'000'000 = $100).
#include "lle/book/reference_book.hpp"

#include "lle/book/book_builder.hpp"
#include "lle/book/book_concept.hpp"
#include "lle/protocol/itch_encode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace {

using namespace lle::book;

static_assert(OrderBook<ReferenceBook>, "the reference book must satisfy the common interface");

constexpr Locate kL = 1;
Price px(double dollars) { return static_cast<Price>(std::llround(dollars * 10'000)); }

std::vector<LevelView> levels(const ReferenceBook& b, Side s, Locate l = kL) {
    std::vector<LevelView> v;
    b.depth(l, s, v);
    return v;
}

TEST(ReferenceBook, EmptyBookHasNoTop) {
    ReferenceBook b(16);
    EXPECT_EQ(b.top(kL), Top{});
    EXPECT_EQ(b.live_orders(), 0u);
}

TEST(ReferenceBook, AddsBuildSortedLevels) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(100.00), 100);
    b.add(kL, 2, Side::Buy, px(100.01), 200);
    b.add(kL, 3, Side::Buy, px(99.99), 300);
    b.add(kL, 4, Side::Sell, px(100.05), 50);
    b.add(kL, 5, Side::Sell, px(100.03), 60);
    b.add(kL, 6, Side::Buy, px(100.01), 10);  // joins an existing level

    const Top t = b.top(kL);
    EXPECT_EQ(t.bid_px, px(100.01));
    EXPECT_EQ(t.bid_qty, 210u);
    EXPECT_EQ(t.ask_px, px(100.03));
    EXPECT_EQ(t.ask_qty, 60u);

    // Bids best (highest) first, asks best (lowest) first.
    EXPECT_EQ(levels(b, Side::Buy), (std::vector<LevelView>{{px(100.01), 210, 2}, {px(100.00), 100, 1}, {px(99.99), 300, 1}}));
    EXPECT_EQ(levels(b, Side::Sell), (std::vector<LevelView>{{px(100.03), 60, 1}, {px(100.05), 50, 1}}));
    EXPECT_EQ(b.live_orders(), 6u);
    EXPECT_EQ(b.counters().adds, 6u);
}

TEST(ReferenceBook, FifoQueueAtALevel) {
    ReferenceBook b(16);
    for (OrderRef r : {10u, 11u, 12u}) b.add(kL, r, Side::Sell, px(5), 100);
    EXPECT_EQ(b.queue_at(kL, Side::Sell, px(5)), (std::vector<OrderRef>{10, 11, 12}));
}

TEST(ReferenceBook, PartialExecutionKeepsPriority) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(10), 100);
    b.add(kL, 2, Side::Buy, px(10), 100);
    b.execute(kL, 1, 30);
    const auto o1 = b.find(1);
    if (!o1) FAIL() << "order 1 must still be live";
    EXPECT_EQ(o1->qty, 70u);
    EXPECT_EQ(b.top(kL).bid_qty, 170u);
    EXPECT_EQ(b.queue_at(kL, Side::Buy, px(10)), (std::vector<OrderRef>{1, 2})) << "partial fill keeps its place";
    EXPECT_EQ(b.counters().executes, 1u);
}

TEST(ReferenceBook, FullExecutionRemovesOrderAndEmptyLevel) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Sell, px(10), 100);
    b.add(kL, 2, Side::Sell, px(11), 100);
    b.execute(kL, 1, 60);
    b.execute(kL, 1, 40);  // exactly to zero: the order is gone
    EXPECT_FALSE(b.find(1).has_value());
    EXPECT_EQ(b.top(kL).ask_px, px(11)) << "empty level must not remain as best ask";
    EXPECT_EQ(levels(b, Side::Sell).size(), 1u);
    EXPECT_EQ(b.counters().overfill, 0u);
}

TEST(ReferenceBook, PartialCancelThenCancelToZero) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(20), 500);
    b.cancel(kL, 1, 200);
    const auto o1 = b.find(1);
    if (!o1) FAIL() << "order 1 must still be live";
    EXPECT_EQ(o1->qty, 300u);
    EXPECT_EQ(b.top(kL).bid_qty, 300u);
    b.cancel(kL, 1, 300);
    EXPECT_FALSE(b.find(1).has_value());
    EXPECT_EQ(b.top(kL), Top{});
    EXPECT_EQ(b.counters().cancels, 2u);
}

TEST(ReferenceBook, OverfillIsCountedAndRemovesTheOrder) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(20), 100);
    b.execute(kL, 1, 150);
    EXPECT_FALSE(b.find(1).has_value());
    EXPECT_EQ(b.counters().overfill, 1u);
}

TEST(ReferenceBook, DeleteRemovesFromMiddleOfQueue) {
    ReferenceBook b(16);
    for (OrderRef r : {1u, 2u, 3u}) b.add(kL, r, Side::Buy, px(7), 10);
    b.remove(kL, 2);
    EXPECT_EQ(b.queue_at(kL, Side::Buy, px(7)), (std::vector<OrderRef>{1, 3}));
    EXPECT_EQ(b.top(kL).bid_qty, 20u);
    EXPECT_EQ(b.counters().deletes, 1u);
}

TEST(ReferenceBook, ReplaceAtSamePriceLosesTimePriority) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Sell, px(30), 100);
    b.add(kL, 2, Side::Sell, px(30), 100);
    b.replace(kL, 1, 3, 100, px(30));  // same price, same size: still goes to the back
    EXPECT_EQ(b.queue_at(kL, Side::Sell, px(30)), (std::vector<OrderRef>{2, 3}));
    EXPECT_FALSE(b.find(1).has_value());
    const auto o3 = b.find(3);
    if (!o3) FAIL() << "order 3 must still be live";
    EXPECT_EQ(o3->side, Side::Sell) << "replace keeps the side";
    EXPECT_EQ(b.counters().replaces, 1u);
    EXPECT_EQ(b.counters().adds, 2u) << "a replace is not counted as an add";
}

TEST(ReferenceBook, ReplaceMovesPriceAndSize) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(30), 100);
    b.replace(kL, 1, 2, 40, px(31));
    EXPECT_EQ(b.top(kL).bid_px, px(31));
    EXPECT_EQ(b.top(kL).bid_qty, 40u);
    EXPECT_EQ(levels(b, Side::Buy).size(), 1u) << "old level must be removed";
}

TEST(ReferenceBook, UnknownRefsAreCountedAndIgnored) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(1), 10);
    b.execute(kL, 99, 5);
    b.cancel(kL, 99, 5);
    b.remove(kL, 99);
    b.replace(kL, 99, 100, 5, px(1));
    EXPECT_EQ(b.counters().unknown_ref, 4u);
    EXPECT_FALSE(b.find(100).has_value()) << "replace of an unknown order must not add the new one";
    EXPECT_EQ(b.top(kL).bid_qty, 10u);
}

TEST(ReferenceBook, DuplicateRefIsRejected) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(1), 10);
    b.add(kL, 1, Side::Sell, px(2), 20);
    EXPECT_EQ(b.counters().duplicate_ref, 1u);
    EXPECT_EQ(b.top(kL).ask_qty, 0u);
    EXPECT_EQ(b.live_orders(), 1u);
}

TEST(ReferenceBook, ZeroShareAddAndReplaceAreIgnored) {
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(1), 0);
    EXPECT_EQ(b.counters().zero_qty, 1u);
    EXPECT_EQ(b.live_orders(), 0u);
    EXPECT_TRUE(levels(b, Side::Buy).empty()) << "no empty level may be created";
    b.add(kL, 2, Side::Buy, px(1), 10);
    b.replace(kL, 2, 3, 0, px(1));  // old order leaves; the zero-share new one is not added
    EXPECT_EQ(b.counters().zero_qty, 2u);
    EXPECT_EQ(b.live_orders(), 0u);
    EXPECT_EQ(b.top(kL), Top{});
}

TEST(ReferenceBook, BadLocateIsRejected) {
    ReferenceBook b(4);
    b.add(4, 1, Side::Buy, px(1), 10);
    EXPECT_EQ(b.counters().bad_locate, 1u);
    EXPECT_EQ(b.live_orders(), 0u);
    EXPECT_EQ(b.top(4), Top{});
}

TEST(ReferenceBook, LocateMismatchIsCountedButOrderRefWins) {
    // Order refs are unique for the whole day, so the order is found by ref even if a later
    // message carried a different locate; the mismatch is counted as an anomaly.
    ReferenceBook b(16);
    b.add(1, 1, Side::Buy, px(1), 10);
    b.execute(2, 1, 4);
    EXPECT_EQ(b.counters().locate_mismatch, 1u);
    EXPECT_EQ(b.top(1).bid_qty, 6u);
}

TEST(ReferenceBook, SymbolsAreIndependent) {
    ReferenceBook b(16);
    b.add(1, 1, Side::Buy, px(10), 10);
    b.add(2, 2, Side::Buy, px(20), 20);
    b.remove(1, 1);
    EXPECT_EQ(b.top(1), Top{});
    EXPECT_EQ(b.top(2).bid_px, px(20));
}

TEST(ReferenceBook, CrossedBookIsRepresentedNotMatched) {
    // Book building never matches: if the feed shows a crossed book (it can, e.g. before the
    // opening cross), the book shows it as is.
    ReferenceBook b(16);
    b.add(kL, 1, Side::Buy, px(10.05), 10);
    b.add(kL, 2, Side::Sell, px(10.00), 10);
    const Top t = b.top(kL);
    EXPECT_GT(t.bid_px, t.ask_px);
    EXPECT_EQ(b.live_orders(), 2u);
}

TEST(BookBuilder, TradingActionUpdatesSymbolState) {
    // H messages drive the per-symbol trading state; E1 uses it to tell a legitimately crossed
    // book (halted / quotation-only: the exchange is not matching) from a suspicious one.
    auto book = std::make_unique<ReferenceBook>(16);
    auto dir = std::make_unique<SymbolDirectory>();
    BookBuilder<ReferenceBook> bb(*book, *dir);
    std::array<std::byte, 64> buf{};
    const auto sym = lle::itch::make_symbol("ABC");
    const auto feed = [&](std::size_t n) { ASSERT_EQ(lle::itch::decode(buf.data(), n, bb), lle::itch::DecodeStatus::Ok); };

    EXPECT_EQ(dir->state(kL), 0) << "no H seen yet";
    feed(lle::itch::encode_directory(buf.data(), kL, 1, sym));
    feed(lle::itch::encode_trading_action(buf.data(), kL, 2, sym, 'Q'));
    EXPECT_EQ(dir->state(kL), 'Q');
    feed(lle::itch::encode_trading_action(buf.data(), kL, 3, sym, 'T'));
    EXPECT_EQ(dir->state(kL), 'T');
    feed(lle::itch::encode_trading_action(buf.data(), kL, 4, sym, 'H'));
    EXPECT_EQ(dir->state(kL), 'H');
    EXPECT_EQ(dir->symbol(kL), "ABC");
    EXPECT_EQ(bb.counts().by_type['H'], 3u);
}

}  // namespace
