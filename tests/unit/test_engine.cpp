// Tests for the E4 engine pieces (lle/engine/*.hpp).
//
// The key test is ModelsAgree: Model A (one thread) and Model B (three threads + rings) must
// do exactly the same work, so on the same input they must produce the same order digest,
// the same number of orders and the same final books, and those books must equal a book
// built directly by BookBuilder (the path the differential tests already validate against
// the reference book). Run under the tsan preset, the multi-threaded runs also check the
// ring hand-offs for data races.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "lle/book/book_builder.hpp"
#include "lle/book/fast_book.hpp"
#include "lle/concurrency/spsc.hpp"
#include "lle/engine/feeder.hpp"
#include "lle/engine/messages.hpp"
#include "lle/engine/models.hpp"
#include "lle/engine/stages.hpp"
#include "lle/protocol/itch_encode.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/testing/fixture_gen.hpp"

namespace {

using namespace lle;
using namespace lle::engine;
using book::FastBookFib;
using concurrency::Ordering;
using concurrency::SpscRing;

constexpr std::uint64_t kT0 = 4ULL * 3600 * 1'000'000'000;  // 04:00 feed time

RawSlot slot_of(const std::byte* msg, std::size_t len, std::uint64_t intended = 7) {
    RawSlot s{};
    s.intended = intended;
    s.len = static_cast<std::uint16_t>(len);
    std::memcpy(s.data, msg, len);
    return s;
}

TEST(DecodeEvent, BookMessages) {
    std::byte buf[64];
    Event e{};

    decode_event(slot_of(buf, itch::encode_add(buf, 5, kT0, 42, Side::Sell, 300, itch::make_symbol("X"), 1'234'500)),
                 e);
    EXPECT_EQ(e.type, EventType::kAdd);
    EXPECT_EQ(e.intended, 7U);
    EXPECT_EQ(e.locate, 5);
    EXPECT_EQ(e.ref, 42U);
    EXPECT_EQ(e.side, Side::Sell);
    EXPECT_EQ(e.qty, 300U);
    EXPECT_EQ(e.price, 1'234'500U);

    decode_event(slot_of(buf, itch::encode_executed(buf, 5, kT0, 42, 100, 9)), e);
    EXPECT_EQ(e.type, EventType::kExecute);
    EXPECT_EQ(e.qty, 100U);

    decode_event(slot_of(buf, itch::encode_executed_with_price(buf, 5, kT0, 42, 50, 9, true, 1)), e);
    EXPECT_EQ(e.type, EventType::kExecute);
    EXPECT_EQ(e.qty, 50U);

    decode_event(slot_of(buf, itch::encode_cancel(buf, 5, kT0, 42, 25)), e);
    EXPECT_EQ(e.type, EventType::kCancel);
    EXPECT_EQ(e.qty, 25U);

    decode_event(slot_of(buf, itch::encode_delete(buf, 5, kT0, 42)), e);
    EXPECT_EQ(e.type, EventType::kDelete);
    EXPECT_EQ(e.ref, 42U);

    decode_event(slot_of(buf, itch::encode_replace(buf, 5, kT0, 42, 43, 10, 999)), e);
    EXPECT_EQ(e.type, EventType::kReplace);
    EXPECT_EQ(e.ref, 42U);
    EXPECT_EQ(e.new_ref, 43U);
    EXPECT_EQ(e.qty, 10U);
    EXPECT_EQ(e.price, 999U);
}

TEST(DecodeEvent, NonBookAndBrokenMessagesBecomeNone) {
    std::byte buf[64];
    Event e{};
    decode_event(slot_of(buf, itch::encode_directory(buf, 5, kT0, itch::make_symbol("X"))), e);
    EXPECT_EQ(e.type, EventType::kNone);
    decode_event(slot_of(buf, itch::encode_system(buf, kT0, 'O')), e);
    EXPECT_EQ(e.type, EventType::kNone);
    const std::size_t n = itch::encode_add(buf, 5, kT0, 42, Side::Buy, 1, itch::make_symbol("X"), 1);
    decode_event(slot_of(buf, n - 1), e);  // truncated add: rejected by the length check
    EXPECT_EQ(e.type, EventType::kNone);
}

// A tiny two-sided book for the decision-stage tests: bid 1 000 @ 10.0000, ask `ask_qty` @ 10.0100.
struct DecisionFixture : ::testing::Test {
    std::unique_ptr<FastBookFib> book = std::make_unique<FastBookFib>(16, 1024, 1024);
    StrategyConfig cfg;
    Event ev(EventType t, OrderRef ref, Side s, Price px, Qty q) {
        Event e{};
        e.intended = 1;
        e.type = t;
        e.locate = 3;
        e.ref = ref;
        e.side = s;
        e.price = px;
        e.qty = q;
        return e;
    }
};

TEST_F(DecisionFixture, ImbalanceTriggersBuyAtTheAsk) {
    cfg.cooldown_msgs = 0;
    DecisionStage<FastBookFib> st(*book, cfg);
    Decision d{};
    st.process(ev(EventType::kAdd, 1, Side::Buy, 100'000, 1000), d);
    EXPECT_EQ(d.action, Action::kNone);  // one-sided book
    st.process(ev(EventType::kAdd, 2, Side::Sell, 100'100, 100), d);
    ASSERT_EQ(d.action, Action::kBuy);  // 1000 >= 4 * 100
    EXPECT_EQ(d.price, 100'100U);
    EXPECT_EQ(d.qty, 100U);
    EXPECT_EQ(d.locate, 3);
}

TEST_F(DecisionFixture, WideSpreadOrBalancedBookDoesNotTrade) {
    DecisionStage<FastBookFib> st(*book, cfg);
    Decision d{};
    st.process(ev(EventType::kAdd, 1, Side::Buy, 100'000, 1000), d);
    st.process(ev(EventType::kAdd, 2, Side::Sell, 101'000, 100), d);  // spread $0.10 > $0.05
    EXPECT_EQ(d.action, Action::kNone);
    st.process(ev(EventType::kAdd, 3, Side::Sell, 100'100, 900), d);  // tight, but 1000 < 4 * 900
    EXPECT_EQ(d.action, Action::kNone);
}

// The work knob spins only on measured messages (intended != 0); warm-up messages
// (intended == 0) skip it. 2 ms of work makes the difference far above timer noise.
TEST_F(DecisionFixture, WorkKnobSpinsOnlyOnMeasuredMessages) {
    const TscCalibration cal = calibrate_tsc(0.05);
    cfg.work_ticks = static_cast<std::uint64_t>(2'000'000 * cal.ticks_per_ns);
    DecisionStage<FastBookFib> st(*book, cfg);
    Decision d{};
    Event warm = ev(EventType::kAdd, 1, Side::Buy, 100'000, 1000);
    warm.intended = 0;
    std::uint64_t t0 = Tsc::now();
    st.process(warm, d);
    const std::uint64_t warm_ticks = Tsc::now() - t0;
    t0 = Tsc::now();
    st.process(ev(EventType::kAdd, 2, Side::Sell, 100'100, 100), d);
    const std::uint64_t measured_ticks = Tsc::now() - t0;
    EXPECT_LT(warm_ticks, cfg.work_ticks / 2);
    EXPECT_GE(measured_ticks, cfg.work_ticks);
}

TEST_F(DecisionFixture, CooldownAndPositionLimit) {
    cfg.cooldown_msgs = 3;
    cfg.max_position = 200;
    DecisionStage<FastBookFib> st(*book, cfg);
    Decision d{};
    st.process(ev(EventType::kAdd, 1, Side::Buy, 100'000, 1000), d);
    st.process(ev(EventType::kAdd, 2, Side::Sell, 100'100, 100), d);
    ASSERT_EQ(d.action, Action::kBuy);  // msg 2: position 100
    int orders = 0;
    for (OrderRef r = 10; r < 30; ++r) {  // keep the imbalance: more bids
        st.process(ev(EventType::kAdd, r, Side::Buy, 100'000, 10), d);
        orders += d.action == Action::kBuy ? 1 : 0;
    }
    EXPECT_EQ(orders, 1);  // one more after the cool-down (position 200), then the limit blocks
}

TEST(Sink, LatencyAndThroughputModes) {
    auto h = std::make_unique<Histogram>();
    Sink s(*h, false);
    Decision d{};
    d.intended = 0;  // warm-up: not measured
    s.consume(d);
    EXPECT_EQ(s.measured(), 0U);
    d.intended = Tsc::now();
    s.consume(d);
    EXPECT_EQ(s.measured(), 1U);
    EXPECT_EQ(h->count(), 1U);

    auto h2 = std::make_unique<Histogram>();
    Sink t(*h2, true);
    d.intended = 1;
    for (std::uint64_t i = 0; i < 3 * Sink::kBlock + 1; ++i) t.consume(d);
    EXPECT_EQ(h2->count(), 3U);  // one sample per completed block of 1024
}

TEST(Sink, DigestDependsOnDecisions) {
    auto h = std::make_unique<Histogram>();
    Sink a(*h, false), b(*h, false);
    Decision d{};
    d.action = Action::kBuy;
    d.price = 5;
    d.qty = 100;
    a.consume(d);
    d.price = 6;
    b.consume(d);
    EXPECT_NE(a.digest(), b.digest());
    EXPECT_EQ(a.orders(), 1U);
}

std::vector<std::byte> framed_messages(const std::vector<std::uint64_t>& ts) {
    std::vector<std::byte> out;
    std::byte msg[64], fr[66];
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const std::size_t n = itch::encode_delete(msg, 1, ts[i], i + 1);
        const std::size_t f = itch::frame(fr, msg, n);
        out.insert(out.end(), fr, fr + f);
    }
    return out;
}

TEST(Feeder, LoadSplitsWarmupAndWindowByFeedTime) {
    itch::ItchFileReader r(itch::memory_source(framed_messages({kT0, kT0 + 10, kT0 + 20, kT0 + 30, kT0 + 40})));
    const FeedData f = load_feed(r, kT0 + 15, 2);
    EXPECT_EQ(f.warmup, 2U);
    EXPECT_EQ(f.window, 2U);
    EXPECT_EQ(f.total(), 4U);  // the fifth message is never loaded
    EXPECT_EQ(f.feed_ns[2], kT0 + 20);
    EXPECT_EQ(f.len(0), 19U);

    itch::ItchFileReader r2(itch::memory_source(framed_messages({kT0, kT0 + 10})));
    EXPECT_THROW((void)load_feed(r2, kT0 + 5, 5), std::runtime_error);  // window incomplete
}

TEST(Feeder, SmoothAndBurstySchedulesHaveTheSameMeanRate) {
    itch::ItchFileReader r(itch::memory_source(framed_messages({kT0, kT0, kT0 + 100, kT0 + 1000, kT0 + 1000})));
    const FeedData f = load_feed(r, kT0, 5);
    const double tpn = 2.0;                                            // 2 ticks per ns
    const auto smooth = make_schedule(f, Arrival::kSmooth, 1e6, tpn);  // 1 us apart = 2000 ticks
    ASSERT_EQ(smooth.size(), 5U);
    EXPECT_EQ(smooth[1], 2000U);
    EXPECT_EQ(smooth[4], 8000U);
    const auto bursty = make_schedule(f, Arrival::kBursty, 1e6, tpn);
    EXPECT_EQ(bursty[0], 0U);
    EXPECT_EQ(bursty[1], 0U);     // same feed timestamp: sent back to back
    EXPECT_EQ(bursty[4], 8000U);  // same total span as smooth
    EXPECT_EQ(bursty[2], 800U);   // 100/1000 of the span
    EXPECT_EQ(bursty[3], 8000U);
}

using TestInRing = SpscRing<RawSlot, 64, Ordering::kAcqRel, true, 128>;
using TestEventRing = SpscRing<Event, 64, Ordering::kAcqRel, true, 128>;
using TestDecisionRing = SpscRing<Decision, 64, Ordering::kAcqRel, true, 128>;

struct NoProbe {
    void open() {}
    void begin() {}
    void end() {}
};

struct RunResult {
    std::uint64_t digest = 0, orders = 0, measured = 0;
    std::unique_ptr<FastBookFib> book;
};

// Runs the feeder + one model on unpinned threads (tests must not depend on the CPU count).
template <bool kHops>
RunResult run_model(const FeedData& f, bool model_b, bool open_loop) {
    RunResult res;
    res.book = std::make_unique<FastBookFib>(65536, 1 << 18, 1 << 18);
    StrategyConfig cfg;
    cfg.cooldown_msgs = 50;
    DecisionStage<FastBookFib> st(*res.book, cfg);
    auto hist = std::make_unique<Histogram>();
    Sink sink(*hist, !open_loop);
    HopHistograms hops;
    auto in = std::make_unique<TestInRing>();
    auto fstats = std::make_unique<FeederStats>();
    std::atomic<bool> warm{false};
    std::atomic<int> ready{0};
    const double tpn = calibrate_tsc(0.05).ticks_per_ns;
    const std::vector<std::uint64_t> sched =
        open_loop ? make_schedule(f, Arrival::kBursty, 5e6, tpn) : std::vector<std::uint64_t>{};
    std::thread feeder([&] { run_feeder(f, sched, *in, warm, tpn, *fstats); });
    NoProbe probes[3];
    if (model_b) {
        PipelineRings<TestEventRing, TestDecisionRing> rings;
        run_model_b<kHops>(*in, rings, st, sink, f.total(), f.warmup, warm, hops, probes, [](int) {}, ready, 3);
    } else {
        run_model_a<kHops>(*in, st, sink, f.total(), f.warmup, warm, hops, probes[0]);
    }
    feeder.join();
    res.digest = sink.digest();
    res.orders = sink.orders();
    res.measured = sink.measured();
    return res;
}

FeedData fixture_feed(std::size_t window) {
    lle::testing::FixtureConfig cfg;
    cfg.seed = 11;
    cfg.messages = 60'000;
    cfg.symbols = 40;
    cfg.mode = lle::testing::FixtureMode::Realistic;
    itch::ItchFileReader r(itch::memory_source(lle::testing::generate_fixture_bytes(cfg)));
    // Start the window ~20 ms of feed time after 04:00 (fixture messages are 1-2000 ns apart).
    return load_feed(r, kT0 + 20'000'000, window);
}

void expect_same_books(const FastBookFib& a, const FastBookFib& b) {
    for (Locate l = 0; l < 1000; ++l) ASSERT_EQ(a.top(l), b.top(l)) << "locate " << l;
    EXPECT_EQ(a.live_orders(), b.live_orders());
}

TEST(Models, AgreeWithEachOtherAndWithBookBuilder) {
    const FeedData f = fixture_feed(30'000);
    ASSERT_GT(f.warmup, 1000U);

    // Ground truth: the same messages through BookBuilder (no events, no threads).
    auto direct = std::make_unique<FastBookFib>(65536, 1 << 18, 1 << 18);
    auto dir = std::make_unique<book::SymbolDirectory>();
    book::BookBuilder<FastBookFib> bb(*direct, *dir);
    for (std::size_t i = 0; i < f.total(); ++i) (void)itch::decode(f.msg(i), f.len(i), bb);

    const RunResult a = run_model<false>(f, false, false);
    const RunResult b = run_model<false>(f, true, false);
    const RunResult a_open = run_model<true>(f, false, true);
    const RunResult b_open = run_model<true>(f, true, true);

    EXPECT_GT(a.orders, 0U) << "the fixture should trigger some decisions";
    for (const RunResult* r : {&b, &a_open, &b_open}) {
        EXPECT_EQ(r->digest, a.digest);
        EXPECT_EQ(r->orders, a.orders);
        EXPECT_EQ(r->measured, f.window);
        expect_same_books(*r->book, *a.book);
    }
    expect_same_books(*a.book, *direct);
}

}  // namespace
