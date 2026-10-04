#pragma once
// Reference order book: the "obviously correct" implementation.
//
// It uses only standard containers, chosen for clarity, not speed:
//   std::map<Price, Level>               sorted price levels per side (O(log L) find/insert)
//   std::list<Order> inside each Level   FIFO time priority; list iterators stay valid when
//                                        other orders are inserted/erased, so we can store them
//   std::unordered_map<OrderRef, Handle> ref -> (locate, side, price, list iterator), because
//                                        E/C/X/D/U messages carry only the order reference
//
// Every operation is O(log L) or O(1) amortized, but each touches several heap nodes
// scattered in memory (pointer chasing) and add/remove call new/delete. That is exactly what
// the optimized book (Phase 2, E2) removes layer by layer. This class is (1) the L0 baseline
// of E2, (2) the oracle of the differential tester, and (3) the matching engine's book in the
// exchange emulator (P6).

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lle/book/book_types.hpp"

namespace lle::book {

class ReferenceBook {
public:
    explicit ReferenceBook(std::size_t max_locates = 65536) : books_(max_locates) {}

    void add(Locate loc, OrderRef ref, Side side, Price px, Qty qty) {
        ++c_.adds;
        if (loc >= books_.size()) {
            ++c_.bad_locate;
            return;
        }
        if (qty == 0) {  // never sent by Nasdaq; adding it would leave a level with 0 shares
            ++c_.zero_qty;
            return;
        }
        if (orders_.count(ref) != 0) {  // should never happen: refs are day-unique
            ++c_.duplicate_ref;
            return;
        }
        auto& sb = books_[loc];
        Queue* q = nullptr;
        Level* lvl = nullptr;
        if (side == Side::Buy) {
            lvl = &sb.bids[px];
        } else {
            lvl = &sb.asks[px];
        }
        q = &lvl->orders;
        q->push_back(Order{ref, qty});  // back of the queue: newest order has lowest priority
        lvl->total += qty;
        orders_.emplace(ref, Handle{loc, side, px, std::prev(q->end())});
    }

    // E and C messages: shares leave the book at the order's own price.
    void execute(Locate loc, OrderRef ref, Qty qty) {
        ++c_.executes;
        reduce(loc, ref, qty);
    }

    // X message: partial cancel.
    void cancel(Locate loc, OrderRef ref, Qty qty) {
        ++c_.cancels;
        reduce(loc, ref, qty);
    }

    // D message: the whole order leaves the book.
    void remove(Locate loc, OrderRef ref) {
        ++c_.deletes;
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++c_.unknown_ref;
            return;
        }
        if (it->second.locate != loc) ++c_.locate_mismatch;
        erase_order(it);
    }

    // U message: remove the old order, add the new one (new ref, same side/symbol) at the
    // back of its level's queue. Time priority is lost even if the price is unchanged.
    void replace(Locate loc, OrderRef old_ref, OrderRef new_ref, Qty qty, Price px) {
        ++c_.replaces;
        auto it = orders_.find(old_ref);
        if (it == orders_.end()) {
            ++c_.unknown_ref;
            return;
        }
        if (it->second.locate != loc) ++c_.locate_mismatch;
        const Locate l = it->second.locate;
        const Side side = it->second.side;
        erase_order(it);
        --c_.adds;  // add() below counts itself; a replace is not an add in the statistics
        add(l, new_ref, side, px, qty);
    }

    [[nodiscard]] Top top(Locate loc) const {
        Top t;
        if (loc >= books_.size()) return t;
        const auto& sb = books_[loc];
        if (!sb.bids.empty()) {
            t.bid_px = sb.bids.begin()->first;
            t.bid_qty = sb.bids.begin()->second.total;
        }
        if (!sb.asks.empty()) {
            t.ask_px = sb.asks.begin()->first;
            t.ask_qty = sb.asks.begin()->second.total;
        }
        return t;
    }

    // Levels of one side, best first (at most max_levels). Allocates: not for the hot path.
    void depth(Locate loc, Side side, std::vector<LevelView>& out, std::size_t max_levels = SIZE_MAX) const {
        out.clear();
        if (loc >= books_.size()) return;
        auto emit = [&](const auto& m) {
            for (const auto& [px, lvl] : m) {
                if (out.size() >= max_levels) break;
                out.push_back(LevelView{px, lvl.total, static_cast<std::uint32_t>(lvl.orders.size())});
            }
        };
        if (side == Side::Buy)
            emit(books_[loc].bids);
        else
            emit(books_[loc].asks);
    }

    // Refs of the orders at one price level in priority order (tests: verifies FIFO).
    [[nodiscard]] std::vector<OrderRef> queue_at(Locate loc, Side side, Price px) const {
        std::vector<OrderRef> out;
        const auto& sb = books_[loc];
        const Level* lvl = nullptr;
        if (side == Side::Buy) {
            if (auto it = sb.bids.find(px); it != sb.bids.end()) lvl = &it->second;
        } else {
            if (auto it = sb.asks.find(px); it != sb.asks.end()) lvl = &it->second;
        }
        if (lvl != nullptr)
            for (const auto& o : lvl->orders) out.push_back(o.ref);
        return out;
    }

    [[nodiscard]] std::optional<OrderInfo> find(OrderRef ref) const {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return std::nullopt;
        return OrderInfo{it->second.locate, it->second.side, it->second.price, it->second.it->qty};
    }

    [[nodiscard]] std::size_t live_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] std::size_t max_locates() const noexcept { return books_.size(); }
    [[nodiscard]] const BookCounters& counters() const noexcept { return c_; }

private:
    struct Order {
        OrderRef ref;
        Qty qty;
    };
    using Queue = std::list<Order>;
    struct Level {
        std::uint64_t total = 0;
        Queue orders;
    };
    struct SymbolBook {
        std::map<Price, Level, std::greater<>> bids;  // highest price first
        std::map<Price, Level> asks;                  // lowest price first
    };
    struct Handle {
        Locate locate;
        Side side;
        Price price;
        Queue::iterator it;
    };
    using OrderMap = std::unordered_map<OrderRef, Handle>;

    void reduce(Locate loc, OrderRef ref, Qty qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++c_.unknown_ref;
            return;
        }
        if (it->second.locate != loc) ++c_.locate_mismatch;
        Order& o = *it->second.it;
        if (qty >= o.qty) {
            if (qty > o.qty) ++c_.overfill;
            erase_order(it);  // fully filled/cancelled: the order is dead
            return;
        }
        o.qty -= qty;
        level_of(it->second).total -= qty;
    }

    Level& level_of(const Handle& h) {
        auto& sb = books_[h.locate];
        return h.side == Side::Buy ? sb.bids.find(h.price)->second : sb.asks.find(h.price)->second;
    }

    void erase_order(OrderMap::iterator it) {
        const Handle& h = it->second;
        auto& sb = books_[h.locate];
        if (h.side == Side::Buy) {
            auto lit = sb.bids.find(h.price);
            lit->second.total -= h.it->qty;
            lit->second.orders.erase(h.it);
            if (lit->second.orders.empty()) sb.bids.erase(lit);
        } else {
            auto lit = sb.asks.find(h.price);
            lit->second.total -= h.it->qty;
            lit->second.orders.erase(h.it);
            if (lit->second.orders.empty()) sb.asks.erase(lit);
        }
        orders_.erase(it);
    }

    std::vector<SymbolBook> books_;
    OrderMap orders_;
    BookCounters c_;
};

}  // namespace lle::book
