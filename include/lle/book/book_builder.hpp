#pragma once
// Glue between the ITCH decoder and any order book: an ItchHandler that turns decoded
// messages into book operations, and keeps the locate -> symbol directory.
//
// Templated on the book type so the call into the book is a direct (inlinable) call:
// this is the "static dispatch" used throughout the hot path.

#include <array>
#include <cstdint>
#include <string_view>

#include "lle/book/book_types.hpp"
#include "lle/protocol/itch.hpp"

namespace lle::book {

// Locate codes are assigned per day by the R (stock directory) messages at the start of the
// file. Books are indexed by locate directly (a plain array index, no string hashing).
class SymbolDirectory {
public:
    void set(Locate loc, const std::array<char, 8>& sym, std::uint32_t round_lot) noexcept {
        syms_[loc] = sym;
        lots_[loc] = round_lot;
        known_[loc] = true;
    }
    [[nodiscard]] std::string_view symbol(Locate loc) const noexcept {
        return known_[loc] ? itch::trim_symbol(syms_[loc]) : std::string_view{};
    }
    [[nodiscard]] bool known(Locate loc) const noexcept { return known_[loc]; }
    [[nodiscard]] std::uint32_t round_lot(Locate loc) const noexcept { return lots_[loc]; }

private:
    std::array<std::array<char, 8>, 65536> syms_{};
    std::array<std::uint32_t, 65536> lots_{};
    std::array<bool, 65536> known_{};
};

struct MessageCounts {
    std::array<std::uint64_t, 256> by_type{};
    std::uint64_t total = 0;
};

template <class Book>
class BookBuilder {
public:
    BookBuilder(Book& book, SymbolDirectory& dir) noexcept : book_(book), dir_(dir) {}

    void on_add(const itch::AddOrder& m) {
        count(m.h.type);
        book_.add(m.h.locate, m.ref, m.side, m.price, m.shares);
    }
    void on_executed(const itch::OrderExecuted& m) {
        count('E');
        book_.execute(m.h.locate, m.ref, m.shares);
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        count('C');
        book_.execute(m.h.locate, m.ref, m.shares);
    }
    void on_cancel(const itch::OrderCancel& m) {
        count('X');
        book_.cancel(m.h.locate, m.ref, m.shares);
    }
    void on_delete(const itch::OrderDelete& m) {
        count('D');
        book_.remove(m.h.locate, m.ref);
    }
    void on_replace(const itch::OrderReplace& m) {
        count('U');
        book_.replace(m.h.locate, m.old_ref, m.new_ref, m.shares, m.price);
    }
    void on_directory(const itch::StockDirectory& m) {
        count('R');
        dir_.set(m.h.locate, m.stock, m.round_lot_size);
    }
    void on_system(const itch::SystemEvent& m) {
        count('S');
        last_system_event_ = m.event_code;
    }
    void on_trading_action(const itch::TradingAction& m) {
        count('H');
        (void)m;
    }
    void on_other(char type, const std::byte*, std::size_t) { count(type); }

    [[nodiscard]] const MessageCounts& counts() const noexcept { return counts_; }
    [[nodiscard]] char last_system_event() const noexcept { return last_system_event_; }

private:
    void count(char t) noexcept {
        ++counts_.by_type[static_cast<unsigned char>(t)];
        ++counts_.total;
    }

    Book& book_;
    SymbolDirectory& dir_;
    MessageCounts counts_;
    char last_system_event_ = 0;
};

}  // namespace lle::book
