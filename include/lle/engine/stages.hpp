#pragma once
// Stages 2 and 3 of the engine (E4): the decision stage (book update + strategy + risk +
// synthetic work) and the sink (latency measurement + order digest).
//
// This is the "minimal decision stage" of plan.md P4: just enough real work that both
// threading models do something a trading engine does after each market-data message
// (update the book, look at the top of book, maybe decide to trade, check risk), before
// the real strategy/risk/gateway of P6 exist. It is deterministic: the same input always
// produces the same decisions, so Model A and Model B must produce the same order digest.

#include <cstdint>
#include <vector>

#include "lle/core/tsc.hpp"
#include "lle/engine/messages.hpp"
#include "lle/telemetry/histogram.hpp"

namespace lle::engine {

struct StrategyConfig {
    // Trade when one side of the top of book has at least `ratio` times the other side's
    // quantity (a crude order-book-imbalance signal) and the spread is tight.
    std::uint64_t ratio = 4;
    Price max_spread = 500;  // Price(4) units: 500 = $0.05
    Qty order_qty = 100;     // shares per order (one round lot)
    // Risk: per-symbol position limit (orders are assumed filled at once, the conservative
    // assumption for a limit), and a per-symbol cool-down counted in messages so one symbol
    // cannot emit an order on every message. Counting messages (not time) keeps it
    // deterministic across models and runs.
    std::int32_t max_position = 1000;
    std::uint64_t cooldown_msgs = 1000;
    // Work knob: spin this many TSC ticks per message after the decision, standing in for
    // a more expensive strategy (E4 sweeps 0 / 200 / 500 / 1000 ns).
    std::uint64_t work_ticks = 0;
};

template <class Book>
class DecisionStage {
public:
    DecisionStage(Book& book, const StrategyConfig& cfg)
        : book_(book), cfg_(cfg), position_(book.max_locates(), 0), last_order_(book.max_locates(), 0) {}

    void process(const Event& e, Decision& d) noexcept {
        d = Decision{};
        d.intended = e.intended;
        d.t_decode = e.t_done;
        ++msgs_;
        switch (e.type) {
            case EventType::kAdd:
                book_.add(e.locate, e.ref, e.side, e.price, e.qty);
                break;
            case EventType::kExecute:
                book_.execute(e.locate, e.ref, e.qty);
                break;
            case EventType::kCancel:
                book_.cancel(e.locate, e.ref, e.qty);
                break;
            case EventType::kDelete:
                book_.remove(e.locate, e.ref);
                break;
            case EventType::kReplace:
                book_.replace(e.locate, e.ref, e.new_ref, e.qty, e.price);
                break;
            case EventType::kNone:
                break;
        }
        if (e.type != EventType::kNone && e.locate < position_.size()) decide(e.locate, d);
        // The spin changes no state, so it is skipped for warm-up messages (intended == 0):
        // the books end up identical, and a 30M-message warm-up does not cost 30 s per run
        // at 1000 ns of work.
        if (cfg_.work_ticks != 0 && e.intended != 0) {
            const std::uint64_t end = Tsc::now() + cfg_.work_ticks;
            while (Tsc::now() < end) {
            }
        }
    }

private:
    void decide(Locate loc, Decision& d) noexcept {
        const book::Top t = book_.top(loc);
        if (t.bid_qty == 0 || t.ask_qty == 0 || t.ask_px <= t.bid_px) return;  // one-sided, locked or crossed
        if (t.ask_px - t.bid_px > cfg_.max_spread) return;
        Action a = Action::kNone;
        if (t.bid_qty >= cfg_.ratio * t.ask_qty)
            a = Action::kBuy;  // buyers dominate: take the ask
        else if (t.ask_qty >= cfg_.ratio * t.bid_qty)
            a = Action::kSell;  // sellers dominate: hit the bid
        if (a == Action::kNone) return;
        // Risk checks: cool-down, then position limit.
        if (last_order_[loc] != 0 && msgs_ - last_order_[loc] < cfg_.cooldown_msgs) return;
        const auto q = static_cast<std::int32_t>(cfg_.order_qty);
        const std::int32_t next = position_[loc] + (a == Action::kBuy ? q : -q);
        if (next > cfg_.max_position || next < -cfg_.max_position) return;
        position_[loc] = next;
        last_order_[loc] = msgs_;
        d.action = a;
        d.locate = loc;
        d.price = a == Action::kBuy ? t.ask_px : t.bid_px;
        d.qty = cfg_.order_qty;
    }

    Book& book_;
    StrategyConfig cfg_;
    std::vector<std::int32_t> position_;  // allocated once, indexed by locate
    std::vector<std::uint64_t> last_order_;
    std::uint64_t msgs_ = 0;
};

// Stage 3: the end of the measured path. In a real engine this is the order gateway; here
// it folds every order into a digest (so A and B can be checked for identical output) and
// records the end-to-end latency of every measured message.
//
// Two measurement modes:
//   latency     histogram of (TSC now - intended send time) per measured message.
//   throughput  closed loop (the feeder sends as fast as the engine accepts): one histogram
//               sample per 1 024 measured messages (time between consecutive blocks), so
//               analyse.py reports ns per message with value_divisor = 1024.
class Sink {
public:
    static constexpr std::uint64_t kBlock = 1024;

    Sink(Histogram& hist, bool throughput) noexcept : hist_(hist), throughput_(throughput) {}

    // Returns the end-of-path TSC stamp.
    std::uint64_t consume(const Decision& d) noexcept {
        if (d.action != Action::kNone) {
            ++orders_;
            mix(d.locate);
            mix(static_cast<std::uint64_t>(d.action));
            mix(d.price);
            mix(d.qty);
        }
        const std::uint64_t now = Tsc::stop();
        if (d.intended != 0) {
            if (throughput_) {
                if (measured_ == 0) {
                    first_ = block_start_ = now;
                } else if (measured_ % kBlock == 0) {
                    hist_.record(now - block_start_);
                    block_start_ = now;
                }
            } else if (now >= d.intended) {
                hist_.record(now - d.intended);
            } else {
                ++negative_;  // stamp before the intended time: clock skew (E0 bounds it)
                hist_.record(0);
            }
            ++measured_;
            last_ = now;
        }
        return now;
    }

    [[nodiscard]] std::uint64_t digest() const noexcept { return digest_; }
    [[nodiscard]] std::uint64_t orders() const noexcept { return orders_; }
    [[nodiscard]] std::uint64_t measured() const noexcept { return measured_; }
    [[nodiscard]] std::uint64_t negative() const noexcept { return negative_; }
    // TSC span from the first to the last measured message (throughput mode).
    [[nodiscard]] std::uint64_t span_ticks() const noexcept { return last_ - first_; }

private:
    void mix(std::uint64_t v) noexcept {  // FNV-1a over the value's 8 bytes
        for (int i = 0; i < 8; ++i) {
            digest_ ^= (v >> (8 * i)) & 0xFF;
            digest_ *= 0x100000001B3ULL;
        }
    }

    Histogram& hist_;
    bool throughput_;
    std::uint64_t digest_ = 0xCBF29CE484222325ULL;
    std::uint64_t orders_ = 0, measured_ = 0, negative_ = 0;
    std::uint64_t first_ = 0, last_ = 0, block_start_ = 0;
};

}  // namespace lle::engine
