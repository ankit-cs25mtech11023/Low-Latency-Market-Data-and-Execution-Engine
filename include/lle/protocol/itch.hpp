#pragma once
// NASDAQ TotalView-ITCH 5.0 decoder (book-relevant subset).
//
// Spec: "Nasdaq TotalView-ITCH 5.0" (nasdaqtrader.com, NQTVITCHspecification.pdf).
// Summary in docs/itch-moldudp64.md.
//
// Every message starts with a common 11-byte header:
//   offset 0  Message Type     1 byte  (ASCII letter)
//   offset 1  Stock Locate     2 bytes (per-day integer id of the symbol; 0 for system msgs)
//   offset 3  Tracking Number  2 bytes
//   offset 5  Timestamp        6 bytes (ns since midnight)
//
// Messages that change the displayed order book:
//   A / F  Add Order (F also carries the market participant id)
//   E      Order Executed            (shares leave the book at the order's price)
//   C      Order Executed With Price (same effect on the book; trade printed at another price)
//   X      Order Cancel              (partial cancel: shares leave the book)
//   D      Order Delete              (whole order leaves the book)
//   U      Order Replace             (old order removed; new ref, price, shares added at the
//                                     BACK of the queue, i.e. it loses time priority)
// Also decoded: R (stock directory: locate -> symbol), S (system event), H (trading action).
// Everything else (trades P/Q/B, NOII I, ...) is passed to the handler as "other" so the
// caller can count it, and skipped using the framing length.
//
// Design:
//   * The decoder is a function template over a Handler type: each message is dispatched
//     with a switch to an inline call, no virtual functions, so the compiler can inline the
//     whole decode -> book-update path.
//   * Fields are read with load_be (memcpy + bswap), never by casting the buffer to a struct.
//   * The declared message length is checked against the spec length for its type before any
//     field is read (a truncated or corrupt message can never cause an out-of-bounds read).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "lle/protocol/endian.hpp"

namespace lle::itch {

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

// Price(4): integer with 4 implied decimals, e.g. 1234500 = $123.45. Never converted to floating
// point in the engine, so there is no rounding anywhere.
using Price = std::uint32_t;
using Qty = std::uint32_t;
using OrderRef = std::uint64_t;
using Locate = std::uint16_t;
using Timestamp = std::uint64_t;  // ns since midnight

struct Header {
    char type;
    Locate locate;
    std::uint16_t tracking;
    Timestamp ts;
};

struct AddOrder {
    Header h;
    OrderRef ref;
    Side side;
    Qty shares;
    Price price;
    std::array<char, 8> stock;
    std::array<char, 4> mpid;  // F only; spaces for A
    bool attributed;           // true for F
};

struct OrderExecuted {  // E
    Header h;
    OrderRef ref;
    Qty shares;
    std::uint64_t match;
};

struct OrderExecutedWithPrice {  // C
    Header h;
    OrderRef ref;
    Qty shares;
    std::uint64_t match;
    bool printable;
    Price exec_price;
};

struct OrderCancel {  // X
    Header h;
    OrderRef ref;
    Qty shares;
};

struct OrderDelete {  // D
    Header h;
    OrderRef ref;
};

struct OrderReplace {  // U
    Header h;
    OrderRef old_ref;
    OrderRef new_ref;
    Qty shares;
    Price price;
};

struct StockDirectory {  // R
    Header h;
    std::array<char, 8> stock;
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
    char round_lots_only;
};

struct SystemEvent {  // S
    Header h;
    char event_code;  // O S Q M E C
};

struct TradingAction {  // H
    Header h;
    std::array<char, 8> stock;
    char state;  // H halted, P paused, Q quotation only, T trading
    std::array<char, 4> reason;
};

// Message lengths from the ITCH 5.0 spec (0 = unknown type). Used for bounds checks.
inline constexpr std::array<std::uint8_t, 256> kMessageLength = [] {
    std::array<std::uint8_t, 256> t{};
    t['S'] = 12; t['R'] = 39; t['H'] = 25; t['Y'] = 20; t['L'] = 26; t['V'] = 35;
    t['W'] = 12; t['K'] = 28; t['J'] = 35; t['h'] = 21; t['A'] = 36; t['F'] = 40;
    t['E'] = 31; t['C'] = 36; t['X'] = 23; t['D'] = 19; t['U'] = 35; t['P'] = 44;
    t['Q'] = 40; t['B'] = 19; t['I'] = 50; t['N'] = 20; t['O'] = 48;
    return t;
}();

enum class DecodeStatus : std::uint8_t {
    Ok,           // book/directory/system message decoded and dispatched
    Other,        // valid message of a type we do not decode (dispatched to on_other)
    Truncated,    // shorter than the spec length for its type: rejected, nothing dispatched
    UnknownType,  // type byte not in the spec: dispatched to on_other, length-skipped
    Empty,        // zero-length message
};

// A Handler must provide these members (any return type; results are ignored):
//   on_add(const AddOrder&), on_executed(const OrderExecuted&),
//   on_executed_with_price(const OrderExecutedWithPrice&), on_cancel(const OrderCancel&),
//   on_delete(const OrderDelete&), on_replace(const OrderReplace&),
//   on_directory(const StockDirectory&), on_system(const SystemEvent&),
//   on_trading_action(const TradingAction&), on_other(char type, const std::byte* p, size_t len)
template <class H>
concept ItchHandler = requires(H& h, const std::byte* p, std::size_t n) {
    h.on_add(AddOrder{});
    h.on_executed(OrderExecuted{});
    h.on_executed_with_price(OrderExecutedWithPrice{});
    h.on_cancel(OrderCancel{});
    h.on_delete(OrderDelete{});
    h.on_replace(OrderReplace{});
    h.on_directory(StockDirectory{});
    h.on_system(SystemEvent{});
    h.on_trading_action(TradingAction{});
    h.on_other(char{}, p, n);
};

namespace detail {

[[gnu::always_inline]] inline Header header(const std::byte* p) noexcept {
    return Header{static_cast<char>(p[0]), proto::load_be<std::uint16_t>(p + 1), proto::load_be<std::uint16_t>(p + 3),
                  proto::load_be48(p + 5)};
}

template <std::size_t N>
[[gnu::always_inline]] inline std::array<char, N> alpha(const std::byte* p) noexcept {
    std::array<char, N> a;
    std::memcpy(a.data(), p, N);
    return a;
}

[[gnu::always_inline]] inline char ch(const std::byte* p) noexcept { return static_cast<char>(*p); }

}  // namespace detail

// Decodes one message (without the 2-byte framing length) and dispatches it to `h`.
template <ItchHandler H>
[[gnu::always_inline]] inline DecodeStatus decode(const std::byte* p, std::size_t len, H& h) {
    using namespace detail;
    using proto::load_be;
    if (len == 0) return DecodeStatus::Empty;
    const char type = static_cast<char>(p[0]);
    const std::size_t need = kMessageLength[static_cast<unsigned char>(type)];
    if (need == 0) {
        h.on_other(type, p, len);
        return DecodeStatus::UnknownType;
    }
    if (len < need) return DecodeStatus::Truncated;

    switch (type) {
        case 'A':
        case 'F': {
            AddOrder m{header(p),
                       load_be<std::uint64_t>(p + 11),
                       ch(p + 19) == 'B' ? Side::Buy : Side::Sell,
                       load_be<std::uint32_t>(p + 20),
                       load_be<std::uint32_t>(p + 32),
                       alpha<8>(p + 24),
                       {' ', ' ', ' ', ' '},
                       type == 'F'};
            if (type == 'F') m.mpid = alpha<4>(p + 36);
            h.on_add(m);
            return DecodeStatus::Ok;
        }
        case 'E':
            h.on_executed(OrderExecuted{header(p), load_be<std::uint64_t>(p + 11), load_be<std::uint32_t>(p + 19),
                                        load_be<std::uint64_t>(p + 23)});
            return DecodeStatus::Ok;
        case 'C':
            h.on_executed_with_price(OrderExecutedWithPrice{
                header(p), load_be<std::uint64_t>(p + 11), load_be<std::uint32_t>(p + 19),
                load_be<std::uint64_t>(p + 23), ch(p + 31) == 'Y', load_be<std::uint32_t>(p + 32)});
            return DecodeStatus::Ok;
        case 'X':
            h.on_cancel(OrderCancel{header(p), load_be<std::uint64_t>(p + 11), load_be<std::uint32_t>(p + 19)});
            return DecodeStatus::Ok;
        case 'D':
            h.on_delete(OrderDelete{header(p), load_be<std::uint64_t>(p + 11)});
            return DecodeStatus::Ok;
        case 'U':
            h.on_replace(OrderReplace{header(p), load_be<std::uint64_t>(p + 11), load_be<std::uint64_t>(p + 19),
                                      load_be<std::uint32_t>(p + 27), load_be<std::uint32_t>(p + 31)});
            return DecodeStatus::Ok;
        case 'R':
            h.on_directory(StockDirectory{header(p), alpha<8>(p + 11), ch(p + 19), ch(p + 20),
                                          load_be<std::uint32_t>(p + 21), ch(p + 25)});
            return DecodeStatus::Ok;
        case 'S':
            h.on_system(SystemEvent{header(p), ch(p + 11)});
            return DecodeStatus::Ok;
        case 'H':
            h.on_trading_action(TradingAction{header(p), alpha<8>(p + 11), ch(p + 19), alpha<4>(p + 21)});
            return DecodeStatus::Ok;
        default:
            h.on_other(type, p, len);
            return DecodeStatus::Other;
    }
}

// Symbol with trailing padding spaces removed.
[[nodiscard]] inline std::string_view trim_symbol(const std::array<char, 8>& s) noexcept {
    std::size_t n = 8;
    while (n > 0 && s[n - 1] == ' ') --n;
    return {s.data(), n};
}

// A handler that ignores everything; derive from it to implement only what you need.
struct NullHandler {
    void on_add(const AddOrder&) noexcept {}
    void on_executed(const OrderExecuted&) noexcept {}
    void on_executed_with_price(const OrderExecutedWithPrice&) noexcept {}
    void on_cancel(const OrderCancel&) noexcept {}
    void on_delete(const OrderDelete&) noexcept {}
    void on_replace(const OrderReplace&) noexcept {}
    void on_directory(const StockDirectory&) noexcept {}
    void on_system(const SystemEvent&) noexcept {}
    void on_trading_action(const TradingAction&) noexcept {}
    void on_other(char, const std::byte*, std::size_t) noexcept {}
};

}  // namespace lle::itch
