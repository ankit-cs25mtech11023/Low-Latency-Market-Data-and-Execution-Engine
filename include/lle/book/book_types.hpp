#pragma once
// Types shared by every order-book implementation (reference and optimized), so the
// differential tester and benchmarks can treat them interchangeably.
//
// Terminology:
//   * level: all resting orders at one price on one side. Its quantity is the sum of theirs.
//   * top of book (BBO): best bid (highest buy price) and best ask (lowest sell price).
//   * book building: reconstructing the exchange's book from its feed. ITCH reports events
//     the exchange has ALREADY matched; we never match orders here (only the exchange
//     emulator in P6 does).

#include <cstdint>
#include <limits>

#include "lle/protocol/itch.hpp"

namespace lle::book {

using itch::Locate;
using itch::OrderRef;
using itch::Price;
using itch::Qty;
using itch::Side;

struct Top {
    Price bid_px = 0;
    std::uint64_t bid_qty = 0;  // 0 means "no bid"
    Price ask_px = 0;
    std::uint64_t ask_qty = 0;  // 0 means "no ask"
    friend bool operator==(const Top&, const Top&) = default;
};

struct LevelView {
    Price price;
    std::uint64_t qty;
    std::uint32_t orders;
    friend bool operator==(const LevelView&, const LevelView&) = default;
};

struct OrderInfo {
    Locate locate;
    Side side;
    Price price;
    Qty qty;
};

// Anomaly counters. On a clean, complete day file all of these except possibly
// `locate_mismatch` should stay 0; a non-zero value means a bug or a corrupt/partial input.
struct BookCounters {
    std::uint64_t adds = 0, executes = 0, cancels = 0, deletes = 0, replaces = 0;
    std::uint64_t unknown_ref = 0;      // E/C/X/D/U for an order we do not know
    std::uint64_t duplicate_ref = 0;    // add with a ref that is already live
    std::uint64_t overfill = 0;         // executed/cancelled more shares than the order had
    std::uint64_t locate_mismatch = 0;  // message locate differs from the order's locate
    std::uint64_t bad_locate = 0;       // locate outside the book's range
};

inline constexpr Price kNoPrice = 0;
inline constexpr Price kMaxItchPrice = 2'000'000'000;  // spec: max Price(4) = 200,000.0000

}  // namespace lle::book
