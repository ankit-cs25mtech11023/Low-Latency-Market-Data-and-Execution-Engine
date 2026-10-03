#pragma once
// Big-endian field loads for wire protocols (ITCH, MoldUDP64, OUCH-style).
//
// Network protocols send integers most-significant byte first ("big-endian"); x86 and
// ARM store them least-significant first. Every multi-byte field must therefore be
// byte-swapped after loading.
//
// Why memcpy and not `*reinterpret_cast<const uint32_t*>(p)`:
//   * the bytes are not a uint32_t object, so reading them through a uint32_t* breaks the
//     strict-aliasing rule (undefined behaviour: the optimizer may reorder or drop reads);
//   * ITCH fields are packed and unaligned (a 4-byte price at offset 31), and unaligned
//     typed access is UB too.
// A fixed-size memcpy is recognized by GCC/Clang and compiles to a single load (plus a
// `bswap`/`movbe`), so it costs nothing over the cast.

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <version>

namespace lle::proto {

// Byte-swap. std::byteswap is C++23, so outside C++23 builds fall back to the builtins.
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr T bswap(T v) noexcept {
#if defined(__cpp_lib_byteswap)
    return std::byteswap(v);
#else
    if constexpr (sizeof(T) == 1) return v;
    else if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
    else if constexpr (sizeof(T) == 4) return __builtin_bswap32(v);
    else {
        static_assert(sizeof(T) == 8);
        return __builtin_bswap64(v);
    }
#endif
}

template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] inline T load_be(const std::byte* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) v = bswap(v);
    return v;
}

// ITCH timestamps are 6 bytes (48 bits) of nanoseconds since midnight.
[[nodiscard, gnu::always_inline]] inline std::uint64_t load_be48(const std::byte* p) noexcept {
    return (std::uint64_t{load_be<std::uint16_t>(p)} << 32) | load_be<std::uint32_t>(p + 2);
}

template <std::unsigned_integral T>
[[gnu::always_inline]] inline void store_be(std::byte* p, T v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = bswap(v);
    std::memcpy(p, &v, sizeof v);
}

[[gnu::always_inline]] inline void store_be48(std::byte* p, std::uint64_t v) noexcept {
    store_be<std::uint16_t>(p, static_cast<std::uint16_t>(v >> 32));
    store_be<std::uint32_t>(p + 2, static_cast<std::uint32_t>(v));
}

}  // namespace lle::proto
