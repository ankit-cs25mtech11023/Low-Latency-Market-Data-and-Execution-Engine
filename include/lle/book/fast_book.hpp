#pragma once
// Optimized order book (Phase 2, E2): same behaviour as ReferenceBook, different data layout.
//
// Where the reference book spends its time (see reference_book.hpp): every add/delete calls
// new/delete three times (map node, list node, hash node), and every operation chases
// pointers between nodes scattered over the heap. This book removes both costs:
//
//   1. Order pool (L1). All orders live in ONE preallocated array of 32-byte OrderNodes and
//      are addressed by 32-bit indices. A freed slot goes on a free list and is reused, so
//      after construction no order operation allocates. 32-bit indices instead of 64-bit
//      pointers keep the node at half a cache line.
//   2. Intrusive FIFO per price level (L1). The node itself holds next/prev indices; a level
//      stores only head/tail. Appending at the tail and unlinking from the middle are O(1)
//      with no separate list nodes.
//   3. Sorted level vector per book side (L2a). A side is a std::vector<Level> sorted from
//      WORST price to BEST price, so the best level is at the back. E1 showed activity is
//      concentrated at the touch (99.9998% of executions at the best price, 91% of deletes
//      and adds within 16 ticks of it), so a level is found by scanning backwards from the
//      best: usually 1-3 contiguous 24-byte entries, already in cache, with predictable
//      branches. Far prices fall back to a binary search. Inserting/erasing a level shifts
//      only the (few) better levels behind it, because the best is at the END of the array.
//      Levels are stored by value, so a level index changes when levels are inserted or
//      erased; orders therefore store their price, and the level is looked up again (cheap
//      near the best). This layout follows the "best at the back" vector book described by
//      David Gross (Optiver), "When Nanoseconds Matter", CppCon 2024.
//   4. Open-addressing ref map (L3), see ref_map.hpp: ref -> pool index, no per-entry nodes.
//      The hash policy is a template parameter: FastBook uses the locality-preserving identity
//      hash, FastBookFib the Fibonacci hash (E2 compares the two).
//
// Allocation behaviour: the pool and the ref map are allocated once in the constructor. A
// side's level vector reserves kInitialLevels the first time it is used and grows (doubling)
// only if that side ever holds more levels than its capacity; growth stops after warm-up.
// e2_book counts every heap allocation during the measured region to show this.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lle/book/book_types.hpp"
#include "lle/book/ref_map.hpp"

namespace lle::book {

template <RefHash H>
class BasicFastBook {
public:
    static constexpr std::uint32_t kNone = 0xFFFF'FFFFu;
    static constexpr std::size_t kInitialLevels = 64;  // reserved per side on first use
    static constexpr std::size_t kLinearScan = 16;     // levels scanned from the best before binary search

    // max_orders: pool size (E1 peak live orders = 1.96 M -> 2^22 = 4.19 M).
    // ref_slots: ref map slots (load <= 0.5 at the peak -> 2^22).
    explicit BasicFastBook(std::size_t max_locates = 65536, std::size_t max_orders = std::size_t{1} << 22,
                           std::size_t ref_slots = std::size_t{1} << 22)
        : books_(max_locates), pool_(max_orders), refs_(ref_slots) {}

    void add(Locate loc, OrderRef ref, Side side, Price px, Qty qty) {
        ++c_.adds;
        if (loc >= books_.size()) {
            ++c_.bad_locate;
            return;
        }
        if (qty == 0) {
            ++c_.zero_qty;
            return;
        }
        const std::uint32_t idx = alloc_node();
        if (!refs_.insert(ref, idx)) {  // refs are day-unique; a repeat is an anomaly
            free_node(idx);
            ++c_.duplicate_ref;
            return;
        }
        OrderNode& n = pool_[idx];
        n.ref = ref;
        n.qty = qty;
        n.price = px;
        n.locate = loc;
        n.side = side;
        n.next = kNone;

        std::vector<Level>& lv = side_of(loc, side);
        const Found f = find_level(lv, side, px);
        if (!f.exists) {
            if (lv.capacity() == 0) lv.reserve(kInitialLevels);
            lv.insert(lv.begin() + static_cast<std::ptrdiff_t>(f.pos), Level{px, 0, 0, kNone, kNone});
        }
        Level& l = lv[f.pos];
        // Append at the tail: the newest order has the lowest time priority.
        n.prev = l.tail;
        if (l.tail != kNone)
            pool_[l.tail].next = idx;
        else
            l.head = idx;
        l.tail = idx;
        ++l.orders;
        l.total += qty;
    }

    // E and C messages.
    void execute(Locate loc, OrderRef ref, Qty qty) {
        ++c_.executes;
        reduce(loc, ref, qty);
    }

    // X message.
    void cancel(Locate loc, OrderRef ref, Qty qty) {
        ++c_.cancels;
        reduce(loc, ref, qty);
    }

    // D message.
    void remove(Locate loc, OrderRef ref) {
        ++c_.deletes;
        const std::uint32_t idx = refs_.erase(ref);
        if (idx == kNone) {
            ++c_.unknown_ref;
            return;
        }
        if (pool_[idx].locate != loc) ++c_.locate_mismatch;
        unlink(idx);
    }

    // U message: old order leaves, the new ref joins the back of its level (priority lost).
    void replace(Locate loc, OrderRef old_ref, OrderRef new_ref, Qty qty, Price px) {
        ++c_.replaces;
        const std::uint32_t idx = refs_.erase(old_ref);
        if (idx == kNone) {
            ++c_.unknown_ref;
            return;
        }
        const Locate l = pool_[idx].locate;
        const Side side = pool_[idx].side;
        if (l != loc) ++c_.locate_mismatch;
        unlink(idx);
        --c_.adds;  // add() counts itself; a replace is not an add in the statistics
        add(l, new_ref, side, px, qty);
    }

    [[nodiscard]] Top top(Locate loc) const noexcept {
        Top t;
        if (loc >= books_.size()) return t;
        const SymbolBook& sb = books_[loc];
        if (!sb.bids.empty()) {
            t.bid_px = sb.bids.back().price;
            t.bid_qty = sb.bids.back().total;
        }
        if (!sb.asks.empty()) {
            t.ask_px = sb.asks.back().price;
            t.ask_qty = sb.asks.back().total;
        }
        return t;
    }

    // Levels of one side, best first. Allocates via `out`: not for the hot path.
    void depth(Locate loc, Side side, std::vector<LevelView>& out, std::size_t max_levels = SIZE_MAX) const {
        out.clear();
        if (loc >= books_.size()) return;
        const std::vector<Level>& lv = side == Side::Buy ? books_[loc].bids : books_[loc].asks;
        for (auto it = lv.rbegin(); it != lv.rend() && out.size() < max_levels; ++it)
            out.push_back(LevelView{it->price, it->total, it->orders});
    }

    // Refs at one level in priority order (tests).
    [[nodiscard]] std::vector<OrderRef> queue_at(Locate loc, Side side, Price px) const {
        std::vector<OrderRef> out;
        const std::vector<Level>& lv = side == Side::Buy ? books_[loc].bids : books_[loc].asks;
        const Found f = find_level(lv, side, px);
        if (!f.exists) return out;
        for (std::uint32_t i = lv[f.pos].head; i != kNone; i = pool_[i].next) out.push_back(pool_[i].ref);
        return out;
    }

    [[nodiscard]] std::size_t live_orders() const noexcept { return refs_.size(); }
    [[nodiscard]] std::size_t max_locates() const noexcept { return books_.size(); }
    [[nodiscard]] const BookCounters& counters() const noexcept { return c_; }

private:
    // One resting order. 32 bytes = two per cache line.
    struct OrderNode {
        OrderRef ref;
        Qty qty;
        std::uint32_t next;  // pool index of the next (younger) order at this level, or kNone;
                             // for a free node: the next free node
        std::uint32_t prev;  // pool index of the previous (older) order, or kNone
        Price price;
        Locate locate;
        Side side;
    };
    static_assert(sizeof(OrderNode) == 32, "OrderNode must stay at half a cache line");

    // One price level. 24 bytes: the ~3 levels nearest the best share two cache lines.
    struct Level {
        Price price;
        std::uint32_t orders;
        std::uint64_t total;
        std::uint32_t head;  // oldest order (highest time priority)
        std::uint32_t tail;  // newest order
    };
    static_assert(sizeof(Level) == 24, "Level layout changed");

    struct SymbolBook {
        std::vector<Level> bids;  // ascending price: best (highest) bid at the back
        std::vector<Level> asks;  // descending price: best (lowest) ask at the back
    };

    struct Found {
        std::size_t pos;  // index of the level, or where to insert it
        bool exists;
    };

    std::vector<Level>& side_of(Locate loc, Side side) noexcept {
        return side == Side::Buy ? books_[loc].bids : books_[loc].asks;
    }

    // `a` is a strictly worse price than `b` on this side (lower bid, higher ask).
    static bool worse(Side side, Price a, Price b) noexcept { return side == Side::Buy ? a < b : a > b; }

    // Scan from the best (back) towards worse prices. A level better than px means px is
    // deeper: keep going. A level worse than px means px belongs right after it.
    static Found find_level(const std::vector<Level>& lv, Side side, Price px) noexcept {
        std::size_t i = lv.size();
        for (std::size_t steps = 0; i > 0 && steps < kLinearScan; --i, ++steps) {
            const Price p = lv[i - 1].price;
            if (p == px) return {i - 1, true};
            if (worse(side, p, px)) return {i, false};
        }
        if (i == 0) return {0, false};
        // Far from the best: binary search over [0, i), which is sorted worst -> best.
        const auto first = lv.begin();
        const auto it = std::lower_bound(first, first + static_cast<std::ptrdiff_t>(i), px,
                                         [side](const Level& l, Price v) { return worse(side, l.price, v); });
        const auto pos = static_cast<std::size_t>(it - first);
        return {pos, pos < i && lv[pos].price == px};
    }

    void reduce(Locate loc, OrderRef ref, Qty qty) {
        const std::uint32_t idx = refs_.find(ref);
        if (idx == kNone) {
            ++c_.unknown_ref;
            return;
        }
        OrderNode& n = pool_[idx];
        if (n.locate != loc) ++c_.locate_mismatch;
        if (qty >= n.qty) {
            if (qty > n.qty) ++c_.overfill;
            refs_.erase(ref);
            unlink(idx);  // fully filled/cancelled: the order is dead
            return;
        }
        n.qty -= qty;
        std::vector<Level>& lv = side_of(n.locate, n.side);
        lv[find_level(lv, n.side, n.price).pos].total -= qty;
    }

    // Removes a live order (already erased from the ref map) from its level and frees it.
    void unlink(std::uint32_t idx) noexcept {
        const OrderNode& n = pool_[idx];
        std::vector<Level>& lv = side_of(n.locate, n.side);
        const std::size_t pos = find_level(lv, n.side, n.price).pos;
        Level& l = lv[pos];
        if (n.prev != kNone)
            pool_[n.prev].next = n.next;
        else
            l.head = n.next;
        if (n.next != kNone)
            pool_[n.next].prev = n.prev;
        else
            l.tail = n.prev;
        l.total -= n.qty;
        if (--l.orders == 0) lv.erase(lv.begin() + static_cast<std::ptrdiff_t>(pos));
        free_node(idx);
    }

    std::uint32_t alloc_node() {
        if (free_head_ != kNone) {
            const std::uint32_t idx = free_head_;
            free_head_ = pool_[idx].next;
            return idx;
        }
        if (bump_ < pool_.size()) return static_cast<std::uint32_t>(bump_++);
        throw std::length_error("FastBook order pool exhausted: increase max_orders");
    }

    void free_node(std::uint32_t idx) noexcept {
        pool_[idx].next = free_head_;
        free_head_ = idx;
    }

    std::vector<SymbolBook> books_;
    std::vector<OrderNode> pool_;
    std::size_t bump_ = 0;  // slots [bump_, size) have never been used
    std::uint32_t free_head_ = kNone;
    BasicRefMap<H> refs_;
    BookCounters c_;
};

using FastBook = BasicFastBook<RefHash::Identity>;
using FastBookFib = BasicFastBook<RefHash::Fibonacci>;

}  // namespace lle::book
