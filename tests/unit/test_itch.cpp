// ITCH 5.0 decoder tests.
//
// Two kinds of check:
//   1. Golden bytes: messages written out byte by byte from the field tables in the Nasdaq
//      spec, independent of our encoder. If the decoder and encoder shared the same wrong
//      offset, a round-trip test alone would still pass; these catch that.
//   2. Round trips through itch_encode.hpp for every decoded type, plus the error paths
//      (empty, truncated, unknown type, valid-but-ignored types).
#include "lle/protocol/itch.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "lle/protocol/endian.hpp"
#include "lle/protocol/itch_encode.hpp"

namespace {

using namespace lle::itch;
namespace proto = lle::proto;

std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (int b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}

// Records the last message of each kind it receives.
struct Capture {
    int calls = 0;
    AddOrder add{};
    OrderExecuted exec{};
    OrderExecutedWithPrice exec_px{};
    OrderCancel cancel{};
    OrderDelete del{};
    OrderReplace repl{};
    StockDirectory dir{};
    SystemEvent sys{};
    TradingAction act{};
    char other_type = 0;
    std::size_t other_len = 0;

    void on_add(const AddOrder& m) { ++calls; add = m; }
    void on_executed(const OrderExecuted& m) { ++calls; exec = m; }
    void on_executed_with_price(const OrderExecutedWithPrice& m) { ++calls; exec_px = m; }
    void on_cancel(const OrderCancel& m) { ++calls; cancel = m; }
    void on_delete(const OrderDelete& m) { ++calls; del = m; }
    void on_replace(const OrderReplace& m) { ++calls; repl = m; }
    void on_directory(const StockDirectory& m) { ++calls; dir = m; }
    void on_system(const SystemEvent& m) { ++calls; sys = m; }
    void on_trading_action(const TradingAction& m) { ++calls; act = m; }
    void on_other(char t, const std::byte*, std::size_t n) { ++calls; other_type = t; other_len = n; }
};

static_assert(ItchHandler<Capture>);
static_assert(ItchHandler<NullHandler>);

// ---- endian helpers ----------------------------------------------------------------------

TEST(Endian, BswapAllWidths) {
    EXPECT_EQ(proto::bswap<std::uint8_t>(0xAB), 0xAB);
    EXPECT_EQ(proto::bswap<std::uint16_t>(0x1234), 0x3412);
    EXPECT_EQ(proto::bswap<std::uint32_t>(0x12345678u), 0x78563412u);
    EXPECT_EQ(proto::bswap<std::uint64_t>(0x0102030405060708ull), 0x0807060504030201ull);
    static_assert(proto::bswap<std::uint32_t>(0x11223344u) == 0x44332211u, "usable in constexpr");
}

TEST(Endian, LoadBigEndianAtUnalignedOffsets) {
    const auto b = bytes({0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    EXPECT_EQ(proto::load_be<std::uint16_t>(b.data() + 1), 0x0102);
    EXPECT_EQ(proto::load_be<std::uint32_t>(b.data() + 1), 0x01020304u);
    EXPECT_EQ(proto::load_be<std::uint64_t>(b.data() + 1), 0x0102030405060708ull);
}

TEST(Endian, Load48BitTimestamp) {
    // 23:59:59.999999999 = 86 399 999 999 999 ns, the largest ITCH timestamp; needs 47 bits.
    const std::uint64_t ts = 86'399'999'999'999ull;
    std::array<std::byte, 6> b{};
    proto::store_be48(b.data(), ts);
    EXPECT_EQ(proto::load_be48(b.data()), ts);
    const auto g = bytes({0x00, 0x00, 0x12, 0x34, 0x56, 0x78});
    EXPECT_EQ(proto::load_be48(g.data()), 0x12345678ull);
    const auto hi = bytes({0xAB, 0xCD, 0x00, 0x00, 0x00, 0x01});
    EXPECT_EQ(proto::load_be48(hi.data()), 0xABCD00000001ull);
}

TEST(Endian, StoreLoadRoundTrip) {
    std::array<std::byte, 8> b{};
    proto::store_be<std::uint32_t>(b.data(), 0xDEADBEEFu);
    EXPECT_EQ(b[0], std::byte{0xDE});
    EXPECT_EQ(b[3], std::byte{0xEF});
    EXPECT_EQ(proto::load_be<std::uint32_t>(b.data()), 0xDEADBEEFu);
}

// ---- golden messages from the spec field tables -------------------------------------------

TEST(ItchGolden, AddOrderNoMpid) {
    // Spec 1.3.1 "Add Order - No MPID Attribution", 36 bytes.
    const auto m = bytes({
        'A',                                             // 0  message type
        0x00, 0x01,                                      // 1  stock locate = 1
        0x00, 0x02,                                      // 3  tracking number = 2
        0x00, 0x00, 0x12, 0x34, 0x56, 0x78,              // 5  timestamp = 0x12345678 ns
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // 11 order reference
        'B',                                             // 19 buy/sell indicator
        0x00, 0x00, 0x00, 0x64,                          // 20 shares = 100
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',          // 24 stock
        0x00, 0x12, 0xD6, 0x44,                          // 32 price = 1234500 ($123.45)
    });
    ASSERT_EQ(m.size(), 36u);
    Capture c;
    ASSERT_EQ(decode(m.data(), m.size(), c), DecodeStatus::Ok);
    EXPECT_EQ(c.calls, 1);
    EXPECT_EQ(c.add.h.type, 'A');
    EXPECT_EQ(c.add.h.locate, 1);
    EXPECT_EQ(c.add.h.tracking, 2);
    EXPECT_EQ(c.add.h.ts, 0x12345678u);
    EXPECT_EQ(c.add.ref, 0x0102030405060708ull);
    EXPECT_EQ(c.add.side, Side::Buy);
    EXPECT_EQ(c.add.shares, 100u);
    EXPECT_EQ(trim_symbol(c.add.stock), "AAPL");
    EXPECT_EQ(c.add.price, 1'234'500u);
    EXPECT_FALSE(c.add.attributed);
}

TEST(ItchGolden, AddOrderWithMpid) {
    // Spec 1.3.2: same layout as A plus a 4-byte attribution at offset 36 (40 bytes).
    auto m = bytes({'F', 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x2A, 'S', 0x00, 0x00, 0x01, 0xF4, 'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ', 0x00,
                    0x00, 0x27, 0x10, 'G', 'S', 'C', 'O'});
    ASSERT_EQ(m.size(), 40u);
    Capture c;
    ASSERT_EQ(decode(m.data(), m.size(), c), DecodeStatus::Ok);
    EXPECT_EQ(c.add.h.type, 'F');
    EXPECT_EQ(c.add.h.locate, 7);
    EXPECT_EQ(c.add.h.ts, 9u);
    EXPECT_EQ(c.add.ref, 42u);
    EXPECT_EQ(c.add.side, Side::Sell);
    EXPECT_EQ(c.add.shares, 500u);
    EXPECT_EQ(c.add.price, 10'000u);  // $1.00
    EXPECT_TRUE(c.add.attributed);
    EXPECT_EQ(std::string_view(c.add.mpid.data(), 4), "GSCO");
}

TEST(ItchGolden, OrderExecuted) {
    // Spec 1.5.1, 31 bytes: ref @11, executed shares @19, match number @23.
    auto m = bytes({'E', 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8});
    ASSERT_EQ(m.size(), 31u);
    Capture c;
    ASSERT_EQ(decode(m.data(), m.size(), c), DecodeStatus::Ok);
    EXPECT_EQ(c.exec.h.locate, 3);
    EXPECT_EQ(c.exec.ref, 42u);
    EXPECT_EQ(c.exec.shares, 10u);
    EXPECT_EQ(c.exec.match, 1000u);
}

TEST(ItchGolden, OrderReplace) {
    // Spec 1.6.2 "Order Replace", 35 bytes: original ref @11, new ref @19, shares @27, price @31.
    auto m = bytes({'U', 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00,
                    0xC8, 0x00, 0x01, 0x86, 0xA0});
    ASSERT_EQ(m.size(), 35u);
    Capture c;
    ASSERT_EQ(decode(m.data(), m.size(), c), DecodeStatus::Ok);
    EXPECT_EQ(c.repl.old_ref, 42u);
    EXPECT_EQ(c.repl.new_ref, 43u);
    EXPECT_EQ(c.repl.shares, 200u);
    EXPECT_EQ(c.repl.price, 100'000u);
}

TEST(ItchGolden, MessageLengthsMatchSpec) {
    // Section 1 of the spec lists the length of every message type.
    const std::pair<char, int> spec[] = {{'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35},
                                         {'W', 12}, {'K', 28}, {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40},
                                         {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19}, {'U', 35}, {'P', 44},
                                         {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
    int known = 0;
    for (auto [t, n] : spec) {
        EXPECT_EQ(kMessageLength[static_cast<unsigned char>(t)], n) << t;
        ++known;
    }
    for (int t = 0; t < 256; ++t)
        if (kMessageLength[static_cast<std::size_t>(t)] != 0) --known;
    EXPECT_EQ(known, 0) << "a type has a length that is not in the spec list";
}

// ---- round trips through the encoder ------------------------------------------------------

TEST(ItchRoundTrip, AllDecodedTypes) {
    std::array<std::byte, 64> b{};
    Capture c;
    const auto sym = make_symbol("SPY");

    ASSERT_EQ(decode(b.data(), encode_add(b.data(), 5, 111, 9001, Side::Sell, 300, sym, 4'500'000), c),
              DecodeStatus::Ok);
    EXPECT_EQ(c.add.ref, 9001u);
    EXPECT_EQ(c.add.side, Side::Sell);
    EXPECT_EQ(c.add.price, 4'500'000u);
    EXPECT_EQ(trim_symbol(c.add.stock), "SPY");

    ASSERT_EQ(decode(b.data(), encode_executed_with_price(b.data(), 5, 112, 9001, 50, 77, true, 4'499'900), c),
              DecodeStatus::Ok);
    EXPECT_EQ(c.exec_px.shares, 50u);
    EXPECT_TRUE(c.exec_px.printable);
    EXPECT_EQ(c.exec_px.exec_price, 4'499'900u);
    EXPECT_EQ(c.exec_px.match, 77u);

    ASSERT_EQ(decode(b.data(), encode_cancel(b.data(), 5, 113, 9001, 25), c), DecodeStatus::Ok);
    EXPECT_EQ(c.cancel.ref, 9001u);
    EXPECT_EQ(c.cancel.shares, 25u);

    ASSERT_EQ(decode(b.data(), encode_delete(b.data(), 5, 114, 9001), c), DecodeStatus::Ok);
    EXPECT_EQ(c.del.ref, 9001u);
    EXPECT_EQ(c.del.h.ts, 114u);

    ASSERT_EQ(decode(b.data(), encode_directory(b.data(), 5, 1, sym, 100), c), DecodeStatus::Ok);
    EXPECT_EQ(c.dir.h.locate, 5);
    EXPECT_EQ(trim_symbol(c.dir.stock), "SPY");
    EXPECT_EQ(c.dir.round_lot_size, 100u);

    ASSERT_EQ(decode(b.data(), encode_system(b.data(), 2, 'Q'), c), DecodeStatus::Ok);
    EXPECT_EQ(c.sys.event_code, 'Q');
    EXPECT_EQ(c.sys.h.locate, 0);

    ASSERT_EQ(decode(b.data(), encode_trading_action(b.data(), 5, 3, sym, 'T'), c), DecodeStatus::Ok);
    EXPECT_EQ(c.act.state, 'T');
    EXPECT_EQ(c.calls, 7);
}

// ---- error paths --------------------------------------------------------------------------

TEST(ItchErrors, EmptyMessage) {
    Capture c;
    std::byte dummy{};
    EXPECT_EQ(decode(&dummy, 0, c), DecodeStatus::Empty);
    EXPECT_EQ(c.calls, 0);
}

TEST(ItchErrors, TruncatedMessagesAreRejectedForEveryType) {
    std::array<std::byte, 64> b{};
    for (int t = 0; t < 256; ++t) {
        const std::size_t need = kMessageLength[static_cast<std::size_t>(t)];
        if (need == 0) continue;
        b[0] = static_cast<std::byte>(t);
        for (std::size_t len = 1; len < need; ++len) {
            Capture c;
            ASSERT_EQ(decode(b.data(), len, c), DecodeStatus::Truncated) << "type " << char(t) << " len " << len;
            ASSERT_EQ(c.calls, 0) << "a truncated message must not be dispatched";
        }
    }
}

TEST(ItchErrors, UnknownTypeGoesToOther) {
    auto m = bytes({'z', 0, 0, 0, 0});
    Capture c;
    EXPECT_EQ(decode(m.data(), m.size(), c), DecodeStatus::UnknownType);
    EXPECT_EQ(c.other_type, 'z');
    EXPECT_EQ(c.other_len, 5u);
}

TEST(ItchErrors, KnownButIgnoredTypeGoesToOther) {
    std::array<std::byte, 64> b{};
    const auto n = encode_trade(b.data(), 1, 1, 5, Side::Buy, 10, make_symbol("X"), 100, 1);
    Capture c;
    EXPECT_EQ(decode(b.data(), n, c), DecodeStatus::Other);
    EXPECT_EQ(c.other_type, 'P');
    EXPECT_EQ(c.other_len, 44u);
}

TEST(ItchErrors, LongerThanSpecIsAccepted) {
    // A future spec revision may append fields; the decoder reads only what it knows.
    std::array<std::byte, 64> b{};
    const auto n = encode_delete(b.data(), 1, 1, 77);
    Capture c;
    EXPECT_EQ(decode(b.data(), n + 4, c), DecodeStatus::Ok);
    EXPECT_EQ(c.del.ref, 77u);
}

TEST(ItchSymbol, TrimSymbol) {
    EXPECT_EQ(trim_symbol(make_symbol("A")), "A");
    EXPECT_EQ(trim_symbol(make_symbol("ABCDEFGH")), "ABCDEFGH");
    EXPECT_EQ(trim_symbol(make_symbol("")), "");
    EXPECT_EQ(trim_symbol(make_symbol("BRK A")), "BRK A");  // inner space kept
}

}  // namespace
