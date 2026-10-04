// E1: characterize the workload (one full ITCH day) before optimizing anything for it.
//
// Every design choice of the optimized book in Phase 2 depends on what the data looks like:
//   * tick-ladder width W: how far from the best price do adds/cancels/executions happen?
//   * pool sizes: how many orders are live at the peak? how many levels does a book have?
//   * order-ref map: are order refs dense (a plain array indexed by ref works) or sparse?
//   * which symbols matter: how skewed is activity across symbols?
//   * burstiness: how far above the average rate are the 1 ms / 10 ms / 1 s peaks? This sets
//     the offered loads for E4/E5.
// The day is streamed once through the reference book; statistics are gathered on the side.
// Book-shape invariants are checked on the way (top of book after every book message, full
// depth at every snapshot), so this run is also the reference book's full-day sanity check:
// there is no "truth" to diff a real day against, but a correct book never lists an empty
// level, never mis-orders levels, and never sees an execution larger than the order (overfill).
// This is a measurement of the DATA, not of our code's speed (the wall time printed at the
// end is for information only, untuned, and never reported as a benchmark).
//
//   e1_workload --in data/07302019.NASDAQ_ITCH50.gz --out results/E1/07302019

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/resource.h>

#include "lle/book/book_builder.hpp"
#include "lle/book/book_concept.hpp"
#include "lle/book/reference_book.hpp"
#include "lle/core/cli.hpp"
#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/telemetry/histogram.hpp"
#include "lle/telemetry/result_io.hpp"

namespace {

using namespace lle;
using namespace lle::book;
using itch::Timestamp;

constexpr Timestamp kNsPerSec = 1'000'000'000ull;
constexpr Timestamp kOpen = 34'200 * kNsPerSec;   // 09:30:00
constexpr Timestamp kClose = 57'600 * kNsPerSec;  // 16:00:00

// Nasdaq's minimum price increment: $0.01 at or above $1.00, $0.0001 below.
constexpr Price tick_of(Price px) { return px >= 10'000 ? 100 : 1; }

// Counts messages per fixed window of feed time, including empty windows, during regular
// trading hours. Timestamps in the file are non-decreasing, so one counter suffices.
class WindowCounter {
public:
    explicit WindowCounter(Timestamp width) : width_(width) {}
    void on_message(Timestamp ts) {
        if (ts < kOpen || ts >= kClose) return;
        const Timestamp w = (ts - kOpen) / width_;
        if (w != cur_) {
            if (cur_ != kNone) hist_.record(count_);
            if (cur_ != kNone && w > cur_ + 1) hist_.record_n(0, w - cur_ - 1);  // empty windows
            if (cur_ == kNone && w > 0) hist_.record_n(0, w);
            cur_ = w;
            count_ = 0;
        }
        ++count_;
        peak_ = std::max(peak_, count_);
    }
    void finish() {
        const Timestamp total = (kClose - kOpen) / width_;
        if (cur_ != kNone) {
            hist_.record(count_);
            if (total > cur_ + 1) hist_.record_n(0, total - cur_ - 1);
        }
        cur_ = kNone;
    }
    [[nodiscard]] const Histogram& hist() const { return hist_; }
    [[nodiscard]] Timestamp width() const { return width_; }

private:
    static constexpr Timestamp kNone = ~Timestamp{0};
    Timestamp width_;
    Timestamp cur_ = kNone;
    std::uint64_t count_ = 0, peak_ = 0;
    Histogram hist_;
};

enum End : std::uint8_t { kExecuted, kCancelled, kDeleted, kReplaced, kEndCount };
constexpr const char* kEndName[kEndCount] = {"executed", "cancelled", "deleted", "replaced"};

class Workload {
public:
    Workload() : builder_(book_, dir_) {
        add_ts_.reserve(1u << 22);
        for (auto& w : windows_) w = nullptr;
        windows_[0] = std::make_unique<WindowCounter>(1'000'000);       // 1 ms
        windows_[1] = std::make_unique<WindowCounter>(10'000'000);      // 10 ms
        windows_[2] = std::make_unique<WindowCounter>(kNsPerSec);       // 1 s
    }

    // ---- ItchHandler -----------------------------------------------------------------------
    void on_add(const itch::AddOrder& m) {
        tick(m.h);
        record_distance(m.h.locate, m.side, m.price, dist_add_);
        builder_.on_add(m);
        check(m.h);
        if (book_.find(m.ref)) {
            add_ts_[m.ref] = m.h.ts;
            note_ref(m.ref);
            note_price(m.h.locate, m.price);
        }
        ++book_msgs_[m.h.locate];
        peak_live_ = std::max<std::uint64_t>(peak_live_, book_.live_orders());
    }
    void on_executed(const itch::OrderExecuted& m) {
        tick(m.h);
        on_reduce(m.h, m.ref, kExecuted, [&] { builder_.on_executed(m); });
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        tick(m.h);
        on_reduce(m.h, m.ref, kExecuted, [&] { builder_.on_executed_with_price(m); });
    }
    void on_cancel(const itch::OrderCancel& m) {
        tick(m.h);
        on_reduce(m.h, m.ref, kCancelled, [&] { builder_.on_cancel(m); });
    }
    void on_delete(const itch::OrderDelete& m) {
        tick(m.h);
        on_reduce(m.h, m.ref, kDeleted, [&] { builder_.on_delete(m); });
    }
    void on_replace(const itch::OrderReplace& m) {
        tick(m.h);
        const auto old = book_.find(m.old_ref);
        if (old) record_distance(m.h.locate, old->side, m.price, dist_replace_new_);
        on_reduce(m.h, m.old_ref, kReplaced, [&] { builder_.on_replace(m); });  // also checks
        if (book_.find(m.new_ref)) {
            add_ts_[m.new_ref] = m.h.ts;
            note_ref(m.new_ref);
            note_price(m.h.locate, m.price);
        }
        peak_live_ = std::max<std::uint64_t>(peak_live_, book_.live_orders());
    }
    void on_directory(const itch::StockDirectory& m) {
        tick(m.h);
        builder_.on_directory(m);
    }
    void on_system(const itch::SystemEvent& m) {
        tick(m.h);
        builder_.on_system(m);
    }
    void on_trading_action(const itch::TradingAction& m) {
        tick(m.h);
        builder_.on_trading_action(m);
    }
    void on_other(char t, const std::byte* p, std::size_t n) {
        if (n >= 11) tick(itch::Header{t, proto::load_be<std::uint16_t>(p + 1), 0, proto::load_be48(p + 5)});
        builder_.on_other(t, p, n);
    }

    void finish() {
        for (auto& w : windows_) w->finish();
        snapshot(last_ts_);
    }

    void write(const std::string& dir, double wall_s, const std::string& input) const;

private:
    // Called for every message with a header: drives windows and periodic depth snapshots.
    void tick(const itch::Header& h) {
        if (h.ts < last_ts_) ++ts_regressions_;  // the window counters assume non-decreasing time
        last_ts_ = h.ts;
        for (auto& w : windows_) w->on_message(h.ts);
        while (next_snapshot_ <= kClose && h.ts >= next_snapshot_) {
            snapshot(next_snapshot_);
            next_snapshot_ += 1800 * kNsPerSec;  // every 30 minutes of feed time
        }
    }

    // Distance in ticks of `px` from the best price on its own side, before the event:
    // positive = behind the best (less aggressive), 0 = at the best, negative = improves it.
    void record_distance(Locate loc, Side side, Price px, Histogram* hists) {
        const Top t = book_.top(loc);
        const bool has = side == Side::Buy ? t.bid_qty != 0 : t.ask_qty != 0;
        if (!has) {
            ++hists_no_best_;
            return;
        }
        const Price best = side == Side::Buy ? t.bid_px : t.ask_px;
        const std::int64_t diff = side == Side::Buy ? static_cast<std::int64_t>(best) - px
                                                    : static_cast<std::int64_t>(px) - best;
        const auto ticks = static_cast<std::uint64_t>(std::llabs(diff) / tick_of(best));
        if (diff >= 0) hists[0].record(ticks);  // behind or at best
        else hists[1].record(ticks);            // improves best
    }

    template <class Apply>
    void on_reduce(const itch::Header& h, OrderRef ref, End how, Apply&& apply) {
        const auto before = book_.find(ref);
        if (before) record_distance(h.locate, before->side, before->price, dist_event_[how]);
        apply();
        check(h);
        ++book_msgs_[h.locate];
        if (before && !book_.find(ref)) {  // the order died on this message
            if (auto it = add_ts_.find(ref); it != add_ts_.end()) {
                lifetime_[how].record(h.ts - it->second);
                add_ts_.erase(it);
            }
        }
    }

    // A crossed/locked top is expected while the exchange is not matching that symbol (halted,
    // paused or quotation-only, from H messages) and is suspicious otherwise. The counts are
    // split three ways (outside 09:30-16:00 / regular hours and trading / regular hours and not
    // trading), and every hit is logged with its timestamp so each one can be explained.
    void check(const itch::Header& h) {
        const Top t = book_.top(h.locate);
        const bool regular = h.ts >= kOpen && h.ts < kClose;
        const char state = dir_.state(h.locate);
        InvariantCounters& c = inv_[!regular ? 0 : state == 'T' ? 1 : 2];
        const std::uint64_t before = c.crossed + c.locked;
        check_top(t, c);
        if (c.crossed + c.locked != before && cross_log_.size() < kMaxCrossLog)
            cross_log_.push_back({h.ts, t, h.locate, h.type, state});
    }

    void note_ref(OrderRef r) {
        min_ref_ = std::min(min_ref_, r);
        max_ref_ = std::max(max_ref_, r);
        ++refs_;
    }
    void note_price(Locate loc, Price px) {
        auto& [lo, hi] = price_range_[loc];
        if (lo == 0 || px < lo) lo = px;
        hi = std::max(hi, px);
    }

    // Depth of every non-empty book at one moment: levels per side, orders per level.
    void snapshot(Timestamp at) {
        Snapshot s;
        s.ts = at;
        s.live_orders = book_.live_orders();
        std::vector<LevelView> lv;
        for (std::size_t loc = 0; loc < book_.max_locates(); ++loc) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                check_depth(book_, static_cast<Locate>(loc), side, lv, depth_inv_);
                if (lv.empty()) continue;
                s.levels_per_side.record(lv.size());
                for (const auto& l : lv) s.orders_per_level.record(l.orders);
                ++s.sides;
            }
        }
        snaps_.push_back(s);
    }

    struct Snapshot {
        Timestamp ts = 0;
        std::uint64_t live_orders = 0, sides = 0;
        Histogram levels_per_side, orders_per_level;
    };

    ReferenceBook book_;
    SymbolDirectory dir_;
    BookBuilder<ReferenceBook> builder_;
    std::unordered_map<OrderRef, Timestamp> add_ts_;
    std::array<std::unique_ptr<WindowCounter>, 3> windows_;
    Histogram dist_add_[2], dist_replace_new_[2], dist_event_[kEndCount][2];
    Histogram lifetime_[kEndCount];
    std::uint64_t hists_no_best_ = 0;
    std::vector<std::uint64_t> book_msgs_ = std::vector<std::uint64_t>(65536, 0);
    std::vector<std::pair<Price, Price>> price_range_ = std::vector<std::pair<Price, Price>>(65536, {0, 0});
    OrderRef min_ref_ = ~OrderRef{0}, max_ref_ = 0;
    std::uint64_t refs_ = 0, peak_live_ = 0;
    Timestamp last_ts_ = 0, next_snapshot_ = kOpen;
    std::uint64_t ts_regressions_ = 0;
    std::vector<Snapshot> snaps_;
    // [0] outside regular hours, [1] 09:30-16:00 and trading, [2] 09:30-16:00 and not trading
    InvariantCounters inv_[3];
    struct CrossEvent {
        Timestamp ts;
        Top top;
        Locate loc;
        char type, state;
    };
    static constexpr std::size_t kMaxCrossLog = 100'000;
    std::vector<CrossEvent> cross_log_;
    InvariantCounters depth_inv_;  // full-depth checks at the snapshots
};

std::FILE* open_out(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) throw std::runtime_error("cannot write " + path);
    return f;
}

void write_hist(const std::string& path, const Histogram& h) {
    std::FILE* f = open_out(path);
    h.write_csv(f);
    std::fclose(f);
}

std::string inv_json(const InvariantCounters& c) {
    char buf[200];
    std::snprintf(buf, sizeof buf, R"({"checks":%llu,"crossed":%llu,"locked":%llu,"empty_level":%llu,"unsorted":%llu})",
                  static_cast<unsigned long long>(c.checks), static_cast<unsigned long long>(c.crossed),
                  static_cast<unsigned long long>(c.locked), static_cast<unsigned long long>(c.empty_level),
                  static_cast<unsigned long long>(c.unsorted));
    return buf;
}

std::string q(const Histogram& h) {  // compact percentile summary as a JSON object
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  R"({"count":%llu,"mean":%.3f,"p50":%llu,"p90":%llu,"p99":%llu,"p99.9":%llu,"max":%llu})",
                  static_cast<unsigned long long>(h.count()), h.mean(),
                  static_cast<unsigned long long>(h.value_at_quantile(0.5)),
                  static_cast<unsigned long long>(h.value_at_quantile(0.9)),
                  static_cast<unsigned long long>(h.value_at_quantile(0.99)),
                  static_cast<unsigned long long>(h.value_at_quantile(0.999)),
                  static_cast<unsigned long long>(h.max()));
    return buf;
}

// Fraction of events within W ticks of the best (behind or improving), for ladder sizing.
std::string coverage(const Histogram* hs) {
    std::string out = "{";
    const std::uint64_t total = hs[0].count() + hs[1].count();
    bool first = true;
    for (const std::uint64_t w : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u, 16384u}) {
        std::uint64_t in = 0;
        for (int k = 0; k < 2; ++k)
            for (std::size_t i = 0; i < Histogram::kBucketCount && Histogram::lowest_of(i) <= w; ++i)
                if (Histogram::highest_of(i) <= w) in += hs[k].bucket_count(i);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s\"%llu\":%.6f", first ? "" : ",", static_cast<unsigned long long>(w),
                      total ? static_cast<double>(in) / static_cast<double>(total) : 0.0);
        out += buf;
        first = false;
    }
    return out + "}";
}

void Workload::write(const std::string& dir, double wall_s, const std::string& input) const {
    std::filesystem::create_directories(dir);
    const auto& counts = builder_.counts();
    const auto& c = book_.counters();

    // Message mix.
    {
        std::FILE* f = open_out(dir + "/message_mix.csv");
        std::fprintf(f, "type,count\n");
        for (int t = 0; t < 256; ++t)
            if (counts.by_type[static_cast<std::size_t>(t)] != 0)
                std::fprintf(f, "%c,%llu\n", static_cast<char>(t),
                             static_cast<unsigned long long>(counts.by_type[static_cast<std::size_t>(t)]));
        std::fclose(f);
    }
    // Per-symbol activity (symbol skew) and price range (ladder sizing without recentring).
    {
        std::FILE* f = open_out(dir + "/symbols.csv");
        std::fprintf(f, "locate,symbol,book_messages,min_price,max_price,range_ticks\n");
        for (std::size_t loc = 0; loc < book_msgs_.size(); ++loc) {
            if (book_msgs_[loc] == 0) continue;
            const auto [lo, hi] = price_range_[loc];
            const auto sym = dir_.symbol(static_cast<Locate>(loc));
            std::fprintf(f, "%zu,%.*s,%llu,%u,%u,%llu\n", loc, static_cast<int>(sym.size()), sym.data(),
                         static_cast<unsigned long long>(book_msgs_[loc]), lo, hi,
                         static_cast<unsigned long long>(lo ? (hi - lo) / tick_of(lo) : 0));
        }
        std::fclose(f);
    }
    // Snapshots.
    {
        std::FILE* f = open_out(dir + "/depth_snapshots.csv");
        std::fprintf(f, "ts_s,live_orders,nonempty_sides,levels_p50,levels_p90,levels_p99,levels_max,"
                        "orders_per_level_p50,orders_per_level_p99,orders_per_level_max\n");
        for (const auto& s : snaps_)
            std::fprintf(f, "%.0f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                         static_cast<double>(s.ts) / 1e9, static_cast<unsigned long long>(s.live_orders),
                         static_cast<unsigned long long>(s.sides),
                         static_cast<unsigned long long>(s.levels_per_side.value_at_quantile(0.5)),
                         static_cast<unsigned long long>(s.levels_per_side.value_at_quantile(0.9)),
                         static_cast<unsigned long long>(s.levels_per_side.value_at_quantile(0.99)),
                         static_cast<unsigned long long>(s.levels_per_side.max()),
                         static_cast<unsigned long long>(s.orders_per_level.value_at_quantile(0.5)),
                         static_cast<unsigned long long>(s.orders_per_level.value_at_quantile(0.99)),
                         static_cast<unsigned long long>(s.orders_per_level.max()));
        std::fclose(f);
    }
    // Every crossed/locked top (state: trading state from H; '-' = none seen yet).
    {
        std::FILE* f = open_out(dir + "/crossed_events.csv");
        std::fprintf(f, "ts_ns,locate,symbol,msg_type,state,bid_px,bid_qty,ask_px,ask_qty\n");
        for (const auto& e : cross_log_) {
            const auto sym = dir_.symbol(e.loc);
            std::fprintf(f, "%llu,%u,%.*s,%c,%c,%u,%llu,%u,%llu\n", static_cast<unsigned long long>(e.ts), e.loc,
                         static_cast<int>(sym.size()), sym.data(), e.type, e.state ? e.state : '-', e.top.bid_px,
                         static_cast<unsigned long long>(e.top.bid_qty), e.top.ask_px,
                         static_cast<unsigned long long>(e.top.ask_qty));
        }
        std::fclose(f);
    }
    for (int e = 0; e < kEndCount; ++e) write_hist(dir + "/lifetime_" + kEndName[e] + ".hist.csv", lifetime_[e]);
    write_hist(dir + "/dist_add_behind.hist.csv", dist_add_[0]);
    write_hist(dir + "/dist_add_improve.hist.csv", dist_add_[1]);
    for (const auto& w : windows_)
        write_hist(dir + "/rate_window_" + std::to_string(w->width() / 1'000'000) + "ms.hist.csv", w->hist());

    std::FILE* f = open_out(dir + "/summary.json");
    std::fprintf(f, "{\n  \"input\": \"%s\",\n  \"build\": %s,\n", json_escape(input).c_str(), build_info_json().c_str());
    std::fprintf(f, "  \"messages\": %llu,\n", static_cast<unsigned long long>(counts.total));
    std::fprintf(f,
                 "  \"book_counters\": {\"adds\":%llu,\"executes\":%llu,\"cancels\":%llu,\"deletes\":%llu,"
                 "\"replaces\":%llu,\"unknown_ref\":%llu,\"duplicate_ref\":%llu,\"overfill\":%llu,"
                 "\"locate_mismatch\":%llu,\"bad_locate\":%llu,\"zero_qty\":%llu},\n",
                 static_cast<unsigned long long>(c.adds), static_cast<unsigned long long>(c.executes),
                 static_cast<unsigned long long>(c.cancels), static_cast<unsigned long long>(c.deletes),
                 static_cast<unsigned long long>(c.replaces), static_cast<unsigned long long>(c.unknown_ref),
                 static_cast<unsigned long long>(c.duplicate_ref), static_cast<unsigned long long>(c.overfill),
                 static_cast<unsigned long long>(c.locate_mismatch), static_cast<unsigned long long>(c.bad_locate),
                 static_cast<unsigned long long>(c.zero_qty));
    std::fprintf(f, "  \"timestamp_regressions\": %llu,\n", static_cast<unsigned long long>(ts_regressions_));
    std::fprintf(f,
                 "  \"invariants\": {\"outside_regular_hours\":%s,\"regular_hours_trading\":%s,"
                 "\"regular_hours_not_trading\":%s,\"depth_snapshots\":%s},\n",
                 inv_json(inv_[0]).c_str(), inv_json(inv_[1]).c_str(), inv_json(inv_[2]).c_str(),
                 inv_json(depth_inv_).c_str());
    std::fprintf(f, "  \"live_orders_end\": %llu,\n  \"peak_live_orders\": %llu,\n",
                 static_cast<unsigned long long>(book_.live_orders()), static_cast<unsigned long long>(peak_live_));
    const double span = max_ref_ >= min_ref_ ? static_cast<double>(max_ref_ - min_ref_) + 1.0 : 0.0;
    std::fprintf(f, "  \"order_refs\": {\"count\":%llu,\"min\":%llu,\"max\":%llu,\"density\":%.6f},\n",
                 static_cast<unsigned long long>(refs_), static_cast<unsigned long long>(min_ref_),
                 static_cast<unsigned long long>(max_ref_), span > 0 ? static_cast<double>(refs_) / span : 0.0);
    std::fprintf(f, "  \"lifetime_ns\": {");
    for (int e = 0; e < kEndCount; ++e) std::fprintf(f, "%s\"%s\":%s", e ? "," : "", kEndName[e], q(lifetime_[e]).c_str());
    std::fprintf(f, "},\n  \"distance_ticks\": {\"add_behind\":%s,\"add_improve\":%s,\"no_best_on_side\":%llu,",
                 q(dist_add_[0]).c_str(), q(dist_add_[1]).c_str(), static_cast<unsigned long long>(hists_no_best_));
    std::fprintf(f, R"("coverage_add":%s,"coverage_replace_new":%s)", coverage(dist_add_).c_str(),
                 coverage(dist_replace_new_).c_str());
    for (int e = 0; e < kEndCount; ++e)
        std::fprintf(f, ",\"coverage_%s\":%s", kEndName[e], coverage(dist_event_[e]).c_str());
    std::fprintf(f, "},\n  \"rate_per_window_regular_hours\": {");
    for (std::size_t i = 0; i < windows_.size(); ++i)
        std::fprintf(f, "%s\"%llums\":%s", i ? "," : "",
                     static_cast<unsigned long long>(windows_[i]->width() / 1'000'000), q(windows_[i]->hist()).c_str());
    // Peak resident memory of the whole run: the machine has 7.6 GiB, so the full day must be
    // streamed and the book's footprint must stay well below that (ru_maxrss is in KiB on Linux).
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    std::fprintf(f, "},\n  \"peak_rss_mib\": %.1f,\n", static_cast<double>(ru.ru_maxrss) / 1024.0);
    std::fprintf(f, "  \"wall_seconds_untuned_informational\": %.1f\n}\n", wall_s);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) try {
    const lle::Cli cli(argc, argv);
    const std::string in = cli.str("in");
    const std::string out = cli.str("out");
    const auto max_messages = static_cast<std::uint64_t>(cli.i64("max-messages", INT64_MAX));

    auto w = std::make_unique<Workload>();
    itch::ItchFileReader reader(itch::open_source(in));
    const auto t0 = std::chrono::steady_clock::now();
    const std::byte* p = nullptr;
    std::size_t len = 0;
    std::uint64_t n = 0;
    while (n < max_messages && reader.next(p, len)) {
        (void)itch::decode(p, len, *w);
        if ((++n & ((1u << 24) - 1)) == 0) {
            std::fprintf(stderr, "\r%llu M messages", static_cast<unsigned long long>(n >> 20));
            std::fflush(stderr);
        }
    }
    w->finish();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    w->write(out, wall, in);
    std::fprintf(stderr, "\n%llu messages in %.1f s -> %s/summary.json\n", static_cast<unsigned long long>(n), wall,
                 out.c_str());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "e1_workload: %s\n", e.what());
    return 1;
}
