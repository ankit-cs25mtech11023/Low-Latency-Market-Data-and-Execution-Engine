#include "lle/testing/fixture_gen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>

#include "lle/protocol/itch_encode.hpp"

namespace lle::testing {
namespace {

using itch::Locate;
using itch::OrderRef;
using itch::Price;
using itch::Qty;
using itch::Side;

constexpr std::array<std::pair<FixtureMode, const char*>, 8> kModeNames{{
    {FixtureMode::Realistic, "realistic"},
    {FixtureMode::DeepQueue, "deep-queue"},
    {FixtureMode::WidePrices, "wide-prices"},
    {FixtureMode::ReplaceChains, "replace-chains"},
    {FixtureMode::Crossing, "crossing"},
    {FixtureMode::ManySymbols, "many-symbols"},
    {FixtureMode::ErrorPaths, "error-paths"},
    {FixtureMode::Mixed, "mixed"},
}};

constexpr Price kMaxPrice = 1'999'999'999;  // $199,999.9999, the largest ITCH Price(4)
constexpr std::uint32_t kManySymbols = 5000;

// Relative frequency of each book event; normalized when sampling.
struct Mix {
    double add, exec, exec_px, cancel, del, repl, trade;
};

struct Live {
    OrderRef ref;
    Locate loc;
    Side side;
    Price px;
    Qty qty;
};

class Generator {
public:
    Generator(const FixtureConfig& cfg, const FrameSink& sink) : cfg_(cfg), sink_(sink), rng_(cfg.seed) {}

    void run() {
        const std::uint32_t nsym = symbols_for(cfg_.mode);
        mids_.resize(nsym + 1);
        for (std::uint32_t l = 1; l <= nsym; ++l) mids_[l] = random_mid();
        // Prologue, as in a real day file: start of messages, directory, market open.
        emit_system('O');
        for (std::uint32_t l = 1; l <= nsym; ++l) {
            char name[16];
            std::snprintf(name, sizeof name, "S%05u", l);
            emit(itch::encode_directory(buf_.data(), static_cast<Locate>(l), tick(), itch::make_symbol(name)));
        }
        emit_system('S');
        emit_system('Q');

        if (cfg_.mode == FixtureMode::Mixed) {
            constexpr FixtureMode blocks[] = {
                FixtureMode::Realistic, FixtureMode::DeepQueue,   FixtureMode::WidePrices, FixtureMode::ReplaceChains,
                FixtureMode::Crossing,  FixtureMode::ManySymbols, FixtureMode::ErrorPaths};
            const std::uint64_t per = cfg_.messages / std::size(blocks);
            for (const FixtureMode m : blocks) run_block(m, per);
            run_block(FixtureMode::Realistic, cfg_.messages - per * std::size(blocks));
        } else {
            run_block(cfg_.mode, cfg_.messages);
        }
        emit_system('M');
        emit_system('E');
        emit_system('C');
    }

private:
    [[nodiscard]] std::uint32_t symbols_for(FixtureMode m) const {
        switch (m) {
            case FixtureMode::DeepQueue:
                return 1;
            case FixtureMode::ManySymbols:
            case FixtureMode::Mixed:
                return std::max(cfg_.symbols, kManySymbols);
            default:
                return std::max<std::uint32_t>(cfg_.symbols, 1);
        }
    }

    void run_block(FixtureMode m, std::uint64_t n) {
        mode_ = m;
        // Symbols this block draws from. Zipf weights for the realistic shape: the k-th most
        // active symbol gets weight 1/k, so a few symbols carry most of the traffic.
        nsym_ = m == FixtureMode::DeepQueue     ? 1
                : m == FixtureMode::ManySymbols ? static_cast<std::uint32_t>(mids_.size() - 1)
                                                : std::min<std::uint32_t>(std::max<std::uint32_t>(cfg_.symbols, 1),
                                                                          static_cast<std::uint32_t>(mids_.size() - 1));
        std::vector<double> w(nsym_);
        for (std::uint32_t i = 0; i < nsym_; ++i) w[i] = m == FixtureMode::ManySymbols ? 1.0 : 1.0 / (i + 1.0);
        zipf_ = std::discrete_distribution<std::uint32_t>(w.begin(), w.end());

        Mix mix{44, 4, 0.5, 3, 38, 9, 1.5};  // realistic default
        if (m == FixtureMode::DeepQueue) mix = {50, 4, 0, 4, 40, 2, 0};
        if (m == FixtureMode::ReplaceChains) mix = {20, 2, 0, 2, 6, 70, 0};
        const std::array<double, 7> wv{mix.add, mix.exec, mix.exec_px, mix.cancel, mix.del, mix.repl, mix.trade};
        std::discrete_distribution<int> pick(wv.begin(), wv.end());

        for (std::uint64_t i = 0; i < n; ++i) {
            if (m == FixtureMode::ErrorPaths && chance(0.05)) {
                inject_error();
                continue;
            }
            int a = live_.empty() ? 0 : pick(rng_);
            if (m == FixtureMode::DeepQueue && live_.size() > 20'000 && a == 0) a = 4;  // bound memory
            switch (a) {
                case 0:
                    add();
                    break;
                case 1:
                    execute(false);
                    break;
                case 2:
                    execute(true);
                    break;
                case 3:
                    cancel();
                    break;
                case 4:
                    del();
                    break;
                case 5:
                    replace();
                    break;
                default:
                    trade();
                    break;
            }
        }
    }

    // ---- book events ------------------------------------------------------------------

    void add() {
        const Locate loc = pick_locate();
        const Side side = chance(0.5) ? Side::Buy : Side::Sell;
        const Price px = price_for(loc, side);
        const Qty qty = random_qty();
        const OrderRef ref = new_ref();
        if (chance(0.1)) {
            emit(itch::encode_add_mpid(buf_.data(), loc, tick(), ref, side, qty, sym_, px, {'T', 'E', 'S', 'T'}));
        } else {
            emit(itch::encode_add(buf_.data(), loc, tick(), ref, side, qty, sym_, px));
        }
        live_.push_back(Live{ref, loc, side, px, qty});
    }

    void execute(bool with_price) {
        const std::size_t i = pick_live();
        Live& o = live_[i];
        const Qty q = (o.qty == 1 || chance(0.5)) ? o.qty : uniform<Qty>(1, o.qty - 1);
        if (with_price)
            emit(itch::encode_executed_with_price(buf_.data(), o.loc, tick(), o.ref, q, ++match_, chance(0.8), o.px));
        else
            emit(itch::encode_executed(buf_.data(), o.loc, tick(), o.ref, q, ++match_));
        reduce(i, q);
    }

    void cancel() {
        const std::size_t i = pick_live();
        Live& o = live_[i];
        const Qty q = o.qty == 1 ? 1 : uniform<Qty>(1, o.qty - 1);  // X is a partial cancel
        emit(itch::encode_cancel(buf_.data(), o.loc, tick(), o.ref, q));
        reduce(i, q);
    }

    void del() {
        const std::size_t i = pick_live();
        emit(itch::encode_delete(buf_.data(), live_[i].loc, tick(), live_[i].ref));
        erase(i);
    }

    void replace() {
        const std::size_t i = pick_live();
        Live& o = live_[i];
        const OrderRef nref = new_ref();
        const Price npx = chance(0.5) ? o.px : price_for(o.loc, o.side);  // same price: pure priority loss
        const Qty nq = random_qty();
        emit(itch::encode_replace(buf_.data(), o.loc, tick(), o.ref, nref, nq, npx));
        o.ref = nref;
        o.px = npx;
        o.qty = nq;
    }

    void trade() {  // P message: hidden-order execution, no effect on the displayed book
        const Locate loc = pick_locate();
        emit(itch::encode_trade(buf_.data(), loc, tick(), 0, Side::Buy, random_qty(), sym_, mids_[loc], ++match_));
    }

    // ---- error injection (ErrorPaths) --------------------------------------------------

    void inject_error() {
        const Locate loc = pick_locate();
        const OrderRef ghost = next_ref_ + 1'000'000'000'000ull + uniform<OrderRef>(0, 1'000'000);
        switch (uniform<int>(0, 8)) {
            case 0:
                emit(itch::encode_executed(buf_.data(), loc, tick(), ghost, 10, ++match_));
                break;
            case 1:
                emit(itch::encode_cancel(buf_.data(), loc, tick(), ghost, 10));
                break;
            case 2:
                emit(itch::encode_delete(buf_.data(), loc, tick(), ghost));
                break;
            case 3:
                emit(itch::encode_replace(buf_.data(), loc, tick(), ghost, new_ref(), 100, mids_[loc]));
                break;
            case 4:  // overfill: execute more than the order has; the book removes the order
                if (!live_.empty()) {
                    const std::size_t i = pick_live();
                    emit(itch::encode_executed(buf_.data(), live_[i].loc, tick(), live_[i].ref, live_[i].qty + 7,
                                               ++match_));
                    erase(i);
                }
                break;
            case 5:  // duplicate ref: the book must reject it (the live order stays unchanged)
                if (!live_.empty()) {
                    const Live& o = live_[pick_live()];
                    emit(itch::encode_add(buf_.data(), o.loc, tick(), o.ref, Side::Sell, 100, sym_, o.px + 100));
                }
                break;
            case 6:  // locate mismatch: the ref still identifies the order, so it is reduced
                if (!live_.empty()) {
                    const std::size_t i = pick_live();
                    if (live_[i].qty > 1) {
                        const auto wrong = static_cast<Locate>(live_[i].loc % nsym_ + 1);
                        emit(itch::encode_cancel(buf_.data(), wrong, tick(), live_[i].ref, 1));
                        reduce(i, 1);
                    }
                }
                break;
            case 7: {  // a message type that does not exist in ITCH 5.0
                std::array<std::byte, 5> m{std::byte{'z'}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}};
                emit_raw(m.data(), m.size());
                break;
            }
            default:  // zero-share add: never sent by Nasdaq; books must ignore and count it
                emit(itch::encode_add(buf_.data(), loc, tick(), new_ref(), Side::Buy, 0, sym_, mids_[loc]));
                break;
        }
    }

    // ---- helpers -------------------------------------------------------------------------

    void reduce(std::size_t i, Qty q) {
        if (q >= live_[i].qty)
            erase(i);
        else
            live_[i].qty -= q;
    }
    void erase(std::size_t i) {  // O(1) swap-remove; order inside live_ does not matter
        live_[i] = live_.back();
        live_.pop_back();
    }
    std::size_t pick_live() { return uniform<std::size_t>(0, live_.size() - 1); }
    Locate pick_locate() { return static_cast<Locate>(zipf_(rng_) + 1); }

    [[nodiscard]] Price tick_size(Price p) const { return p < 10'000 ? 1 : 100; }  // $0.0001 below $1, else $0.01

    Price random_mid() {
        // Log-uniform between $1 and $500, rounded to a cent.
        const double dollars = std::exp(std::uniform_real_distribution<double>(0.0, std::log(500.0))(rng_));
        return static_cast<Price>(std::llround(dollars * 100.0)) * 100;
    }

    Price price_for(Locate loc, Side side) {
        Price mid = mids_[loc];
        if (mode_ == FixtureMode::WidePrices) {
            if (chance(0.001)) mids_[loc] = mid = log_uniform_price();  // jump: forces recentring
            if (chance(0.3)) return log_uniform_price();                // far from the touch
        }
        const Price t = tick_size(mid);
        if (mode_ == FixtureMode::Crossing) {  // either side anywhere near the mid
            const auto off = uniform<std::int64_t>(-5, 5) * static_cast<std::int64_t>(t);
            return clamp_price(static_cast<std::int64_t>(mid) + off);
        }
        // Distance from the mid in ticks: geometric, so most orders rest near the touch.
        const auto d = static_cast<std::int64_t>(
                           std::geometric_distribution<int>(mode_ == FixtureMode::DeepQueue ? 0.7 : 0.25)(rng_)) +
                       1;
        const std::int64_t off = d * static_cast<std::int64_t>(t);
        return clamp_price(side == Side::Buy ? static_cast<std::int64_t>(mid) - off
                                             : static_cast<std::int64_t>(mid) + off);
    }

    Price log_uniform_price() {
        const double v =
            std::exp(std::uniform_real_distribution<double>(0.0, std::log(static_cast<double>(kMaxPrice)))(rng_));
        return clamp_price(std::llround(v));
    }

    static Price clamp_price(std::int64_t p) { return static_cast<Price>(std::clamp<std::int64_t>(p, 1, kMaxPrice)); }

    Qty random_qty() {
        if (chance(0.15)) return uniform<Qty>(1, 99);  // odd lot
        return 100 * uniform<Qty>(1, 10);
    }

    OrderRef new_ref() {
        // Real refs increase through the day with gaps (other symbols' orders in between).
        next_ref_ += uniform<OrderRef>(1, 4);
        return next_ref_;
    }

    itch::Timestamp tick() {
        ts_ += uniform<std::uint64_t>(1, 2000);
        return ts_;
    }

    bool chance(double p) { return std::bernoulli_distribution(p)(rng_); }

    template <class T>
    T uniform(T lo, T hi) {
        return std::uniform_int_distribution<T>(lo, hi)(rng_);
    }

    void emit_system(char code) { emit(itch::encode_system(buf_.data(), tick(), code)); }
    void emit(std::size_t len) { emit_raw(buf_.data(), len); }
    void emit_raw(const std::byte* msg, std::size_t len) {
        const std::size_t n = itch::frame(frame_.data(), msg, len);
        sink_(frame_.data(), n);
    }

    const FixtureConfig& cfg_;
    const FrameSink& sink_;
    std::mt19937_64 rng_;
    FixtureMode mode_ = FixtureMode::Realistic;
    std::uint32_t nsym_ = 1;
    std::discrete_distribution<std::uint32_t> zipf_;
    std::vector<Price> mids_;
    std::vector<Live> live_;
    OrderRef next_ref_ = 0;
    std::uint64_t match_ = 0;
    itch::Timestamp ts_ = 4ull * 3600 * 1'000'000'000;  // 04:00, when the real feed starts
    const std::array<char, 8> sym_ = itch::make_symbol("SYNTH");
    std::array<std::byte, 64> buf_{};
    std::array<std::byte, 66> frame_{};
};

}  // namespace

FixtureMode parse_fixture_mode(const std::string& s) {
    for (const auto& [m, n] : kModeNames)
        if (s == n) return m;
    throw std::invalid_argument("unknown fixture mode '" + s + "'");
}

const char* fixture_mode_name(FixtureMode m) noexcept {
    for (const auto& [mm, n] : kModeNames)
        if (mm == m) return n;
    return "?";
}

void generate_fixture(const FixtureConfig& cfg, const FrameSink& sink) {
    Generator(cfg, sink).run();
}

std::vector<std::byte> generate_fixture_bytes(const FixtureConfig& cfg) {
    std::vector<std::byte> out;
    generate_fixture(cfg, [&](const std::byte* p, std::size_t n) { out.insert(out.end(), p, p + n); });
    return out;
}

}  // namespace lle::testing
