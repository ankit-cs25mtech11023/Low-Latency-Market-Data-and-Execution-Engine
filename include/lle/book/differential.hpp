#pragma once
// Differential testing: run two order-book implementations on the same message stream and
// stop at the first message after which they disagree.
//
// Why: the optimized book (Phase 2) replaces maps and lists with ladders, bitmaps, pools and
// open-addressing hash maps. Unit tests cover the cases someone thought of; a full trading
// day (hundreds of millions of messages) covers the cases nobody thought of. The reference
// book is the oracle: whenever the two differ, the optimized one is presumed wrong.
//
// What is compared:
//   * after EVERY message: the top of book (best bid/ask price and quantity) of the symbol
//     that message touched. Cheap, and catches most bugs at the exact message that caused them.
//   * every `full_every` messages: the full depth (every level: price, quantity, order count)
//     of every symbol touched since the last full check. Catches bugs deeper in the book that
//     do not show at the top yet.
//   * at the end (finish()): the anomaly counters (unknown refs, overfills, ...) and the number
//     of live orders, which must also agree.
//
// On the first mismatch the tester records where it happened (message index, locate, what
// differed) and ignores the rest of the stream. The caller can then cut a reproduction: all
// messages of that one locate up to that index (order refs never move between symbols, so one
// symbol's messages alone rebuild its book).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "lle/book/book_builder.hpp"
#include "lle/book/book_concept.hpp"
#include "lle/protocol/itch.hpp"

namespace lle::book {

struct Mismatch {
    std::uint64_t message_index = 0;  // 0-based index in the stream of the offending message
    Locate locate = 0;
    char message_type = 0;
    std::string what;
};

// Holds two 850 KB symbol directories: allocate it on the heap (std::make_unique).
template <OrderBook A, OrderBook B>
class DifferentialTester {
public:
    DifferentialTester(A& a, B& b, std::uint64_t full_every = 100'000)
        : a_(a), b_(b), ba_(a, dir_a_), bb_(b, dir_b_), full_every_(full_every), dirty_flag_(65536, false) {}

    // ItchHandler interface: apply to both books, then compare.
    void on_add(const itch::AddOrder& m) {
        both([&](auto& x) { x.on_add(m); }, m.h);
    }
    void on_executed(const itch::OrderExecuted& m) {
        both([&](auto& x) { x.on_executed(m); }, m.h);
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        both([&](auto& x) { x.on_executed_with_price(m); }, m.h);
    }
    void on_cancel(const itch::OrderCancel& m) {
        both([&](auto& x) { x.on_cancel(m); }, m.h);
    }
    void on_delete(const itch::OrderDelete& m) {
        both([&](auto& x) { x.on_delete(m); }, m.h);
    }
    void on_replace(const itch::OrderReplace& m) {
        both([&](auto& x) { x.on_replace(m); }, m.h);
    }
    void on_directory(const itch::StockDirectory& m) {
        ba_.on_directory(m);
        bb_.on_directory(m);
        ++index_;
    }
    void on_system(const itch::SystemEvent& m) {
        ba_.on_system(m);
        bb_.on_system(m);
        ++index_;
    }
    void on_trading_action(const itch::TradingAction& m) {
        ba_.on_trading_action(m);
        bb_.on_trading_action(m);
        ++index_;
    }
    void on_other(char t, const std::byte* p, std::size_t n) {
        ba_.on_other(t, p, n);
        bb_.on_other(t, p, n);
        ++index_;
    }

    // The decoder dispatches nothing for empty or truncated messages; call this instead so
    // message indices stay aligned with positions in the stream (needed to cut a repro).
    void on_undecodable() noexcept { ++index_; }

    // Call once after the last message: final full-depth pass + counters + live orders.
    void finish() {
        if (failed()) return;
        full_check(index_ == 0 ? 0 : index_ - 1, 0);
        if (failed()) return;
        const BookCounters& ca = a_.counters();
        const BookCounters& cb = b_.counters();
        auto cmp = [&](const char* name, std::uint64_t x, std::uint64_t y) {
            if (!failed() && x != y)
                fail(index_, 0, 0,
                     std::string("counter ") + name + ": " + std::to_string(x) + " vs " + std::to_string(y));
        };
        cmp("adds", ca.adds, cb.adds);
        cmp("executes", ca.executes, cb.executes);
        cmp("cancels", ca.cancels, cb.cancels);
        cmp("deletes", ca.deletes, cb.deletes);
        cmp("replaces", ca.replaces, cb.replaces);
        cmp("unknown_ref", ca.unknown_ref, cb.unknown_ref);
        cmp("duplicate_ref", ca.duplicate_ref, cb.duplicate_ref);
        cmp("overfill", ca.overfill, cb.overfill);
        cmp("bad_locate", ca.bad_locate, cb.bad_locate);
        cmp("zero_qty", ca.zero_qty, cb.zero_qty);
        cmp("live_orders", a_.live_orders(), b_.live_orders());
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const Mismatch& mismatch() const noexcept { return mismatch_; }
    [[nodiscard]] std::uint64_t messages() const noexcept { return index_; }
    [[nodiscard]] std::uint64_t top_checks() const noexcept { return top_checks_; }
    [[nodiscard]] std::uint64_t full_checks() const noexcept { return full_checks_; }
    // Shape invariants observed on book A (the reference) after every book message.
    [[nodiscard]] const InvariantCounters& invariants() const noexcept { return inv_; }

private:
    template <class F>
    void both(F&& apply, const itch::Header& h) {
        const std::uint64_t idx = index_++;
        if (failed_) return;
        apply(ba_);
        apply(bb_);
        const Top ta = a_.top(h.locate);
        const Top tb = b_.top(h.locate);
        ++top_checks_;
        check_top(ta, inv_);
        if (!(ta == tb)) {
            fail(idx, h.locate, h.type, "top of book differs: " + describe(ta) + " vs " + describe(tb));
            return;
        }
        if (!dirty_flag_[h.locate]) {
            dirty_flag_[h.locate] = true;
            dirty_.push_back(h.locate);
        }
        if (++since_full_ >= full_every_) full_check(idx, h.type);
    }

    void full_check(std::uint64_t idx, char type) {
        since_full_ = 0;
        ++full_checks_;
        for (const Locate loc : dirty_) {
            dirty_flag_[loc] = false;
            if (failed_) continue;
            for (const Side s : {Side::Buy, Side::Sell}) {
                a_.depth(loc, s, da_, SIZE_MAX);
                b_.depth(loc, s, db_, SIZE_MAX);
                check_depth(a_, loc, s, da_, inv_);
                if (da_ != db_) {
                    fail(idx, loc, type,
                         std::string(s == Side::Buy ? "bid" : "ask") + " depth differs: " + std::to_string(da_.size()) +
                             " vs " + std::to_string(db_.size()) + " levels; first difference " + first_diff());
                    break;
                }
            }
        }
        dirty_.clear();
    }

    [[nodiscard]] std::string first_diff() const {
        const std::size_t n = std::min(da_.size(), db_.size());
        for (std::size_t i = 0; i < n; ++i)
            if (!(da_[i] == db_[i]))
                return "at level " + std::to_string(i) + ": " + describe(da_[i]) + " vs " + describe(db_[i]);
        return "at level " + std::to_string(n) + " (one side has more levels)";
    }

    static std::string describe(const Top& t) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "[bid %u x %llu | ask %u x %llu]", t.bid_px,
                      static_cast<unsigned long long>(t.bid_qty), t.ask_px, static_cast<unsigned long long>(t.ask_qty));
        return buf;
    }
    static std::string describe(const LevelView& l) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "(px %u qty %llu orders %u)", l.price, static_cast<unsigned long long>(l.qty),
                      l.orders);
        return buf;
    }

    void fail(std::uint64_t idx, Locate loc, char type, std::string what) {
        failed_ = true;
        mismatch_ = Mismatch{idx, loc, type, std::move(what)};
    }

    A& a_;
    B& b_;
    SymbolDirectory dir_a_, dir_b_;
    BookBuilder<A> ba_;
    BookBuilder<B> bb_;
    std::uint64_t full_every_;
    std::uint64_t since_full_ = 0;
    std::uint64_t index_ = 0;
    std::uint64_t top_checks_ = 0, full_checks_ = 0;
    std::vector<bool> dirty_flag_;
    std::vector<Locate> dirty_;
    std::vector<LevelView> da_, db_;
    InvariantCounters inv_;
    bool failed_ = false;
    Mismatch mismatch_;
};

}  // namespace lle::book
