// Property-based (model-based) tests for the reference book.
//
// The unit tests pin down single events; the differential tester compares two books. Neither
// checks the reference book against something *structurally different*. Here a deliberately
// naive model, one flat map ref -> {locate, side, price, qty} with no price levels at all, is
// driven by the same random streams (many seeds x every valid fixture mode). Every so often the
// model's per-price totals are rebuilt from scratch and must equal the book's depth exactly:
// same prices, same quantities, same order counts, in best-first order. Because the model never
// maintains levels, a bookkeeping bug in the book (a level left behind, a quantity not moved
// on replace, a mis-sorted side) cannot be mirrored by the same bug in the model.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lle/book/book_builder.hpp"
#include "lle/book/book_concept.hpp"
#include "lle/book/reference_book.hpp"
#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/testing/fixture_gen.hpp"

namespace {

using namespace lle;
using namespace lle::book;
using lle::testing::FixtureMode;

struct ModelOrder {
    Locate loc;
    Side side;
    Price px;
    Qty qty;
};

// The naive model. Valid fixture modes never reference unknown orders or overfill (asserted
// below through the book's anomaly counters), so only the normal semantics are modelled.
class Model {
public:
    void add(Locate loc, OrderRef ref, Side side, Price px, Qty qty) { orders_[ref] = {loc, side, px, qty}; }
    void reduce(OrderRef ref, Qty by) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return;
        if (by >= it->second.qty)
            orders_.erase(it);
        else
            it->second.qty -= by;
    }
    void remove(OrderRef ref) { orders_.erase(ref); }
    void replace(OrderRef old_ref, OrderRef new_ref, Qty qty, Price px) {
        auto it = orders_.find(old_ref);
        if (it == orders_.end()) return;
        const ModelOrder o = it->second;
        orders_.erase(it);
        orders_[new_ref] = {o.loc, o.side, px, qty};  // same symbol and side, new price/size
    }
    [[nodiscard]] std::size_t live() const { return orders_.size(); }

    // Per-price totals of every (locate, side), rebuilt from scratch, best price first.
    using Depths = std::map<std::pair<Locate, Side>, std::vector<LevelView>>;
    [[nodiscard]] Depths depths() const {
        std::map<std::pair<Locate, Side>, std::map<Price, LevelView>> by_px;
        for (const auto& [ref, o] : orders_) {
            auto& l = by_px[{o.loc, o.side}][o.px];
            l.price = o.px;
            l.qty += o.qty;
            ++l.orders;
        }
        Depths out;
        for (const auto& [key, levels] : by_px) {
            auto& v = out[key];
            for (const auto& [px, l] : levels) v.push_back(l);
            if (key.second == Side::Buy) std::ranges::reverse(v);  // bids: highest first
        }
        return out;
    }

private:
    std::unordered_map<OrderRef, ModelOrder> orders_;
};

// ItchHandler that applies each message to the model, then to the book via the builder.
class Driver {
public:
    Driver(ReferenceBook& b, SymbolDirectory& d, Model& m) : bb_(b, d), m_(m) {}
    void on_add(const itch::AddOrder& x) {
        seen(x.h.locate);
        m_.add(x.h.locate, x.ref, x.side, x.price, x.shares);
        bb_.on_add(x);
    }
    void on_executed(const itch::OrderExecuted& x) {
        m_.reduce(x.ref, x.shares);
        bb_.on_executed(x);
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& x) {
        m_.reduce(x.ref, x.shares);
        bb_.on_executed_with_price(x);
    }
    void on_cancel(const itch::OrderCancel& x) {
        m_.reduce(x.ref, x.shares);
        bb_.on_cancel(x);
    }
    void on_delete(const itch::OrderDelete& x) {
        m_.remove(x.ref);
        bb_.on_delete(x);
    }
    void on_replace(const itch::OrderReplace& x) {
        m_.replace(x.old_ref, x.new_ref, x.shares, x.price);
        bb_.on_replace(x);
    }
    void on_directory(const itch::StockDirectory& x) { bb_.on_directory(x); }
    void on_system(const itch::SystemEvent& x) { bb_.on_system(x); }
    void on_trading_action(const itch::TradingAction& x) { bb_.on_trading_action(x); }
    void on_other(char t, const std::byte* p, std::size_t n) { bb_.on_other(t, p, n); }

    // Locates that ever received an add. In valid modes an order's locate never changes, so
    // the book can only hold levels at these locates (a final sweep over all locates checks it).
    [[nodiscard]] const std::vector<Locate>& locates() const { return locates_; }

private:
    void seen(Locate loc) {
        if (!seen_[loc]) locates_.push_back(loc);
        seen_[loc] = true;
    }
    std::vector<bool> seen_ = std::vector<bool>(65536, false);
    std::vector<Locate> locates_;
    BookBuilder<ReferenceBook> bb_;
    Model& m_;
};

struct Case {
    FixtureMode mode;
    std::uint64_t seed;
};
void PrintTo(const Case& c, std::ostream* os) {
    *os << lle::testing::fixture_mode_name(c.mode) << " seed " << c.seed;
}

class ModelBased : public ::testing::TestWithParam<Case> {};

TEST_P(ModelBased, BookDepthEqualsNaiveModelThroughoutTheStream) {
    constexpr std::uint32_t kSymbols = 8;      // ignored by single-symbol modes
    constexpr std::uint64_t kCheckEvery = 97;  // prime, so checks do not align with generator cycles
    lle::testing::FixtureConfig cfg;
    cfg.mode = GetParam().mode;
    cfg.seed = GetParam().seed;
    cfg.messages = 12'000;
    cfg.symbols = kSymbols;
    const auto data = lle::testing::generate_fixture_bytes(cfg);

    auto book = std::make_unique<ReferenceBook>(8192);  // many-symbols mode uses 5000 locates
    auto dir = std::make_unique<SymbolDirectory>();
    Model model;
    Driver drv(*book, *dir, model);
    std::vector<LevelView> got;
    InvariantCounters inv;
    std::uint64_t n = 0, checks = 0;

    const auto check = [&](bool every_locate) {
        ++checks;
        ASSERT_EQ(book->live_orders(), model.live()) << "after message " << n;
        const auto all = model.depths();
        const std::vector<LevelView> none;
        std::vector<Locate> locs = drv.locates();
        if (every_locate) {
            locs.clear();
            for (std::size_t li = 0; li < book->max_locates(); ++li) locs.push_back(static_cast<Locate>(li));
        }
        for (const Locate loc : locs) {
            for (const Side s : {Side::Buy, Side::Sell}) {
                book->depth(loc, s, got);
                const auto it = all.find({loc, s});
                const auto& want = it == all.end() ? none : it->second;
                ASSERT_TRUE(got == want) << "locate " << loc << " side " << static_cast<int>(s) << " after message "
                                         << n << ": book has " << got.size() << " levels, model " << want.size();
                check_depth(*book, loc, s, got, inv);
                const Top t = book->top(loc);
                const auto& best = want.empty() ? LevelView{} : want.front();
                if (s == Side::Buy) {
                    ASSERT_EQ(t.bid_qty, best.qty);
                    if (best.qty != 0) {
                        ASSERT_EQ(t.bid_px, best.price);
                    }
                } else {
                    ASSERT_EQ(t.ask_qty, best.qty);
                    if (best.qty != 0) {
                        ASSERT_EQ(t.ask_px, best.price);
                    }
                }
            }
        }
    };

    itch::for_each_framed(data, [&](const std::byte* p, std::size_t len) {
        (void)itch::decode(p, len, drv);
        if (++n % kCheckEvery == 0) check(false);
    });
    check(true);

    const BookCounters& c = book->counters();
    EXPECT_EQ(c.unknown_ref + c.duplicate_ref + c.overfill + c.zero_qty + c.bad_locate, 0u)
        << "valid modes must not exercise anomaly paths (the model does not implement them)";
    EXPECT_EQ(inv.empty_level, 0u);
    EXPECT_EQ(inv.unsorted, 0u);
    EXPECT_GT(checks, 100u);
    EXPECT_GT(c.adds, 1000u);
}

std::vector<Case> cases() {
    std::vector<Case> v;
    for (const FixtureMode m : {FixtureMode::Realistic, FixtureMode::DeepQueue, FixtureMode::WidePrices,
                                FixtureMode::ReplaceChains, FixtureMode::Crossing, FixtureMode::ManySymbols})
        for (std::uint64_t seed = 1; seed <= 8; ++seed) v.push_back({m, seed});
    return v;
}

INSTANTIATE_TEST_SUITE_P(Seeds, ModelBased, ::testing::ValuesIn(cases()), [](const auto& param_info) {
    std::string n = lle::testing::fixture_mode_name(param_info.param.mode);
    for (auto& ch : n)
        if (ch == '-') ch = '_';
    return n + "_seed" + std::to_string(param_info.param.seed);
});

}  // namespace
