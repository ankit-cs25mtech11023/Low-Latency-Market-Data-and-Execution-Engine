#pragma once
// ITCH 5.0 encoder: the inverse of itch.hpp's decoder.
//
// Used by the synthetic fixture generator (tools/gen_fixture), unit tests, and fuzz seeds.
// Every function writes exactly kMessageLength[type] bytes at `out` and returns that length.
// Fields we do not model (tracking numbers, directory flags) are written as zeros/spaces.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lle/protocol/endian.hpp"
#include "lle/protocol/itch.hpp"

namespace lle::itch {

namespace detail {
inline void put_header(std::byte* out, char type, Locate locate, Timestamp ts, std::uint16_t tracking = 0) noexcept {
    out[0] = static_cast<std::byte>(type);
    proto::store_be<std::uint16_t>(out + 1, locate);
    proto::store_be<std::uint16_t>(out + 3, tracking);
    proto::store_be48(out + 5, ts);
}
inline void put_char(std::byte* out, char c) noexcept {
    *out = static_cast<std::byte>(c);
}
template <std::size_t N>
inline void put_alpha(std::byte* out, const std::array<char, N>& a) noexcept {
    std::memcpy(out, a.data(), N);
}
}  // namespace detail

// "AAPL" -> {'A','A','P','L',' ',' ',' ',' '}
[[nodiscard]] inline std::array<char, 8> make_symbol(std::string_view s) noexcept {
    std::array<char, 8> a;
    a.fill(' ');
    std::memcpy(a.data(), s.data(), s.size() < 8 ? s.size() : 8);
    return a;
}

inline std::size_t encode_add(std::byte* out, Locate locate, Timestamp ts, OrderRef ref, Side side, Qty shares,
                              const std::array<char, 8>& stock, Price price) noexcept {
    detail::put_header(out, 'A', locate, ts);
    proto::store_be<std::uint64_t>(out + 11, ref);
    detail::put_char(out + 19, side == Side::Buy ? 'B' : 'S');
    proto::store_be<std::uint32_t>(out + 20, shares);
    detail::put_alpha(out + 24, stock);
    proto::store_be<std::uint32_t>(out + 32, price);
    return 36;
}

inline std::size_t encode_add_mpid(std::byte* out, Locate locate, Timestamp ts, OrderRef ref, Side side, Qty shares,
                                   const std::array<char, 8>& stock, Price price,
                                   const std::array<char, 4>& mpid) noexcept {
    encode_add(out, locate, ts, ref, side, shares, stock, price);
    out[0] = static_cast<std::byte>('F');
    detail::put_alpha(out + 36, mpid);
    return 40;
}

inline std::size_t encode_executed(std::byte* out, Locate locate, Timestamp ts, OrderRef ref, Qty shares,
                                   std::uint64_t match) noexcept {
    detail::put_header(out, 'E', locate, ts);
    proto::store_be<std::uint64_t>(out + 11, ref);
    proto::store_be<std::uint32_t>(out + 19, shares);
    proto::store_be<std::uint64_t>(out + 23, match);
    return 31;
}

inline std::size_t encode_executed_with_price(std::byte* out, Locate locate, Timestamp ts, OrderRef ref, Qty shares,
                                              std::uint64_t match, bool printable, Price price) noexcept {
    detail::put_header(out, 'C', locate, ts);
    proto::store_be<std::uint64_t>(out + 11, ref);
    proto::store_be<std::uint32_t>(out + 19, shares);
    proto::store_be<std::uint64_t>(out + 23, match);
    detail::put_char(out + 31, printable ? 'Y' : 'N');
    proto::store_be<std::uint32_t>(out + 32, price);
    return 36;
}

inline std::size_t encode_cancel(std::byte* out, Locate locate, Timestamp ts, OrderRef ref, Qty shares) noexcept {
    detail::put_header(out, 'X', locate, ts);
    proto::store_be<std::uint64_t>(out + 11, ref);
    proto::store_be<std::uint32_t>(out + 19, shares);
    return 23;
}

inline std::size_t encode_delete(std::byte* out, Locate locate, Timestamp ts, OrderRef ref) noexcept {
    detail::put_header(out, 'D', locate, ts);
    proto::store_be<std::uint64_t>(out + 11, ref);
    return 19;
}

inline std::size_t encode_replace(std::byte* out, Locate locate, Timestamp ts, OrderRef old_ref, OrderRef new_ref,
                                  Qty shares, Price price) noexcept {
    detail::put_header(out, 'U', locate, ts);
    proto::store_be<std::uint64_t>(out + 11, old_ref);
    proto::store_be<std::uint64_t>(out + 19, new_ref);
    proto::store_be<std::uint32_t>(out + 27, shares);
    proto::store_be<std::uint32_t>(out + 31, price);
    return 35;
}

inline std::size_t encode_directory(std::byte* out, Locate locate, Timestamp ts, const std::array<char, 8>& stock,
                                    std::uint32_t round_lot = 100) noexcept {
    std::memset(out, 0, 39);
    detail::put_header(out, 'R', locate, ts);
    detail::put_alpha(out + 11, stock);
    detail::put_char(out + 19, 'Q');  // market category
    detail::put_char(out + 20, 'N');  // financial status: normal
    proto::store_be<std::uint32_t>(out + 21, round_lot);
    detail::put_char(out + 25, 'N');
    for (std::size_t i = 26; i < 39; ++i) detail::put_char(out + i, ' ');
    return 39;
}

inline std::size_t encode_system(std::byte* out, Timestamp ts, char code) noexcept {
    detail::put_header(out, 'S', 0, ts);
    detail::put_char(out + 11, code);
    return 12;
}

inline std::size_t encode_trading_action(std::byte* out, Locate locate, Timestamp ts, const std::array<char, 8>& stock,
                                         char state) noexcept {
    detail::put_header(out, 'H', locate, ts);
    detail::put_alpha(out + 11, stock);
    detail::put_char(out + 19, state);
    detail::put_char(out + 20, ' ');
    for (std::size_t i = 21; i < 25; ++i) detail::put_char(out + i, ' ');
    return 25;
}

// Non-cross trade (P): an execution against a hidden order. Does not change the displayed book.
inline std::size_t encode_trade(std::byte* out, Locate locate, Timestamp ts, OrderRef ref, Side side, Qty shares,
                                const std::array<char, 8>& stock, Price price, std::uint64_t match) noexcept {
    detail::put_header(out, 'P', locate, ts);
    proto::store_be<std::uint64_t>(out + 11, ref);
    detail::put_char(out + 19, side == Side::Buy ? 'B' : 'S');
    proto::store_be<std::uint32_t>(out + 20, shares);
    detail::put_alpha(out + 24, stock);
    proto::store_be<std::uint32_t>(out + 32, price);
    proto::store_be<std::uint64_t>(out + 36, match);
    return 44;
}

// Writes the 2-byte big-endian length prefix used by the ITCH file format (and by MoldUDP64
// message blocks), followed by the message. Returns total bytes written.
inline std::size_t frame(std::byte* out, const std::byte* msg, std::size_t len) noexcept {
    proto::store_be<std::uint16_t>(out, static_cast<std::uint16_t>(len));
    std::memmove(out + 2, msg, len);
    return len + 2;
}

}  // namespace lle::itch
