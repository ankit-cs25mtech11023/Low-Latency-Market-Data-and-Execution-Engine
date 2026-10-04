#pragma once
// The three record types that flow through the engine (E4), and the decode step that turns
// a raw ITCH message into the first of them.
//
//   feeder --RawSlot--> [decode] --Event--> [book + strategy + risk] --Decision--> [sink]
//
// Model A (run-to-completion) runs the three steps back to back on one thread, so an Event
// or a Decision lives only in registers / the stack. Model B (pipelined) runs each step on
// its own core, and the records are copied through SPSC rings between them. Both models call
// the SAME functions, so any difference E4 measures comes from the architecture (cross-core
// hops, more cores, separate caches), not from different code.
//
// Every record is exactly one 64-byte cache line (alignas(64) + static_assert): handing a
// record to another core then costs one cache-line transfer for the payload (E3), and two
// records never share a line (no false sharing between neighbouring ring slots).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lle/book/book_types.hpp"
#include "lle/protocol/itch.hpp"

namespace lle::engine {

using book::Locate;
using book::OrderRef;
using book::Price;
using book::Qty;
using book::Side;

// A raw ITCH message as it would arrive from the network, plus its intended send time.
//
// `intended` is the TSC tick at which the open-loop schedule says this message should have
// been sent. Latency is always measured from it (not from when the feeder actually managed
// to send it), so time a message spends waiting because the engine is busy is counted
// ("no coordinated omission", methodology §9). intended == 0 marks a warm-up message: it is
// processed like any other but never measured.
struct alignas(64) RawSlot {
    std::uint64_t intended;
    std::uint16_t len;
    std::byte data[54];  // the longest ITCH 5.0 message is 50 bytes
};
static_assert(sizeof(RawSlot) == 64, "RawSlot must be one cache line");
inline constexpr std::size_t kMaxRawLen = sizeof(RawSlot::data);

enum class EventType : std::uint8_t { kNone, kAdd, kExecute, kCancel, kDelete, kReplace };

// A decoded, book-relevant message. Non-book messages (directory, system, trades, ...)
// become kNone: they still travel through every stage, so every message is measured
// end to end and every stage processes exactly as many records as the feeder sent.
struct alignas(64) Event {
    std::uint64_t intended;
    std::uint64_t t_done;  // TSC when the decode stage finished (only with hop stamps)
    OrderRef ref;
    OrderRef new_ref;  // replace only
    Price price;
    Qty qty;
    Locate locate;
    EventType type;
    Side side;
};
static_assert(sizeof(Event) == 64, "Event must be one cache line");

enum class Action : std::uint8_t { kNone, kBuy, kSell };

// What the strategy decided for one message (usually: nothing).
struct alignas(64) Decision {
    std::uint64_t intended;
    std::uint64_t t_decode;  // copied from Event::t_done (hop stamps)
    std::uint64_t t_done;    // TSC when the decision stage finished (hop stamps)
    Price price;
    Qty qty;
    Locate locate;
    Action action;
};
static_assert(sizeof(Decision) == 64, "Decision must be one cache line");

namespace detail {
// ITCH handler that writes the decoded message into an Event (see itch::decode).
struct EventWriter {
    Event& e;
    void on_add(const itch::AddOrder& m) noexcept {
        e.type = EventType::kAdd;
        e.locate = m.h.locate;
        e.ref = m.ref;
        e.side = m.side;
        e.price = m.price;
        e.qty = m.shares;
    }
    void on_executed(const itch::OrderExecuted& m) noexcept {
        reduce(EventType::kExecute, m.h.locate, m.ref, m.shares);
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) noexcept {
        reduce(EventType::kExecute, m.h.locate, m.ref, m.shares);
    }
    void on_cancel(const itch::OrderCancel& m) noexcept { reduce(EventType::kCancel, m.h.locate, m.ref, m.shares); }
    void on_delete(const itch::OrderDelete& m) noexcept { reduce(EventType::kDelete, m.h.locate, m.ref, 0); }
    void on_replace(const itch::OrderReplace& m) noexcept {
        e.type = EventType::kReplace;
        e.locate = m.h.locate;
        e.ref = m.old_ref;
        e.new_ref = m.new_ref;
        e.qty = m.shares;
        e.price = m.price;
    }
    void on_directory(const itch::StockDirectory&) noexcept {}
    void on_system(const itch::SystemEvent&) noexcept {}
    void on_trading_action(const itch::TradingAction&) noexcept {}
    void on_other(char, const std::byte*, std::size_t) noexcept {}

    void reduce(EventType t, Locate loc, OrderRef ref, Qty q) noexcept {
        e.type = t;
        e.locate = loc;
        e.ref = ref;
        e.qty = q;
    }
};
}  // namespace detail

// Stage 1: raw bytes -> Event. Fields are read with load_be inside itch::decode (no casting
// of wire bytes to structs), and the length is checked against the spec before any read.
[[gnu::always_inline]] inline void decode_event(const RawSlot& raw, Event& out) noexcept {
    out = Event{};
    out.intended = raw.intended;
    detail::EventWriter w{out};
    (void)itch::decode(raw.data, raw.len, w);
}

}  // namespace lle::engine
