#pragma once
// The common order-book interface, expressed as a C++20 concept.
//
// Every book implementation (the reference book, and each layer of the optimized book in
// Phase 2) provides exactly these operations. Code that drives a book (BookBuilder, the
// differential tester, benchmarks) is a template constrained by this concept, so:
//   * there is no virtual-function call on the hot path (static dispatch, fully inlinable);
//   * a book that forgets an operation, or gets a signature wrong, fails to compile with a
//     readable error instead of failing at run time.
//
// Semantics every implementation must follow (pinned down by tests/unit/test_reference_book.cpp):
//   add      new order at the BACK of its price level's queue (lowest time priority)
//   execute  E and C messages: remove `qty` shares at the order's own price; at 0 the order dies
//   cancel   X message: same effect on the book as execute
//   remove   D message: the whole order leaves the book
//   replace  U message: old order removed, new ref added at the back (loses time priority),
//            same side and symbol
//   Unknown refs, duplicate refs, overfills and bad locates are counted, never fatal.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lle/book/book_types.hpp"

namespace lle::book {

template <class B>
concept OrderBook =
    requires(B& b, const B& cb, Locate loc, OrderRef ref, Side side, Price px, Qty qty, std::vector<LevelView>& out) {
        b.add(loc, ref, side, px, qty);
        b.execute(loc, ref, qty);
        b.cancel(loc, ref, qty);
        b.remove(loc, ref);
        b.replace(loc, ref, ref, qty, px);
        { cb.top(loc) } -> std::same_as<Top>;
        cb.depth(loc, side, out, std::size_t{});
        { cb.live_orders() } -> std::convertible_to<std::size_t>;
        { cb.counters() } -> std::convertible_to<const BookCounters&>;
    };

// Book-shape invariants that can be checked from the outside after any message.
//
// "Crossed" (best bid > best ask) and "locked" (best bid == best ask) books are impossible in
// a continuously matching market, but they DO appear legitimately in an ITCH feed: before the
// opening cross and after the closing cross, orders rest without matching. So they are
// counted and reported, not treated as errors. A non-zero level quantity with zero orders, or
// a level with orders but zero quantity, would be a book bug.
struct InvariantCounters {
    std::uint64_t checks = 0;  // check_top: tops checked; check_depth: book sides checked
    std::uint64_t crossed = 0;
    std::uint64_t locked = 0;
    std::uint64_t empty_level = 0;  // a level listed with 0 qty or 0 orders (bug)
    std::uint64_t unsorted = 0;     // depth not strictly ordered best-first (bug)
};

// Cheap check (top of book only), suitable for every message.
inline void check_top(const Top& t, InvariantCounters& c) noexcept {
    ++c.checks;
    if (t.bid_qty == 0 || t.ask_qty == 0) return;
    if (t.bid_px > t.ask_px)
        ++c.crossed;
    else if (t.bid_px == t.ask_px)
        ++c.locked;
}

// Full-depth check of one side; allocates via `scratch`, so not for the hot path.
template <OrderBook B>
void check_depth(const B& book, Locate loc, Side side, std::vector<LevelView>& scratch, InvariantCounters& c) {
    ++c.checks;
    book.depth(loc, side, scratch, SIZE_MAX);
    for (std::size_t i = 0; i < scratch.size(); ++i) {
        if (scratch[i].qty == 0 || scratch[i].orders == 0) ++c.empty_level;
        if (i > 0) {
            const bool ordered =
                side == Side::Buy ? scratch[i].price < scratch[i - 1].price : scratch[i].price > scratch[i - 1].price;
            if (!ordered) ++c.unsorted;
        }
    }
}

}  // namespace lle::book
