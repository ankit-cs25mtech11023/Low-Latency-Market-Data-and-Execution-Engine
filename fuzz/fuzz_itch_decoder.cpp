// libFuzzer target: the ITCH decoder and the book update path behind it.
//
// The input is treated as an ITCH byte stream ([len:2 BE][message] ...), exactly what the
// file reader or (later) a MoldUDP64 packet hands to the decoder. Malformed framing, lengths
// shorter than the spec, unknown types and nonsense field values (refs to unknown orders,
// overfills, prices at the limits) must never crash, read out of bounds or hit undefined
// behaviour; ASan and UBSan turn any such bug into a crash the fuzzer reports.
//
// Run:  build/fuzz/fuzz/fuzz_itch_decoder -max_total_time=60 fuzz/corpus/itch
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

#include "lle/book/book_builder.hpp"
#include "lle/book/differential.hpp"
#include "lle/book/reference_book.hpp"
#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_file.hpp"

namespace {

// Small locate range keeps per-input setup cheap and exercises the bad-locate path.
constexpr std::size_t kLocates = 64;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> in(reinterpret_cast<const std::byte*>(data), size);  // NOLINT: fuzzer ABI

    // 1. The whole input as one message: every length from 0 up, every type byte.
    lle::itch::NullHandler null;
    (void)lle::itch::decode(in.data(), in.size(), null);

    // 2. The input as a framed stream through two reference books compared message by
    //    message. The comparison must always agree (same code); the point is to drive every
    //    book path with adversarial values and let the sanitizers check memory safety.
    auto a = std::make_unique<lle::book::ReferenceBook>(kLocates);
    auto b = std::make_unique<lle::book::ReferenceBook>(kLocates);
    auto t =
        std::make_unique<lle::book::DifferentialTester<lle::book::ReferenceBook, lle::book::ReferenceBook>>(*a, *b, 16);
    try {
        lle::itch::for_each_framed(in, [&](const std::byte* p, std::size_t n) { (void)lle::itch::decode(p, n, *t); });
    } catch (const std::runtime_error&) {
        // Truncated final frame: a reported error, not a bug.
    }
    t->finish();
    if (t->failed()) __builtin_trap();  // identical books disagreeing would be a real bug
    return 0;
}
