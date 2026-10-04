#pragma once
// Synthetic ITCH 5.0 stream generator for tests and CI.
//
// Real Nasdaq data is never committed to the repository (data/README.md), so CI needs its own
// input. This generator writes valid ITCH-format streams (same framing and message layouts
// as a real day file) from a fixed seed: the same config always produces the same bytes.
//
// It keeps track of every live order it has created, so execute/cancel/delete/replace
// messages always refer to real orders with valid quantities, like the real feed. Error
// modes then deliberately break those rules to exercise the anomaly paths.
//
// Modes (each targets something the optimized book in Phase 2 could get wrong):
//   Realistic      Zipf-skewed symbols, prices near the touch, adds/deletes dominate, few
//                  executions: the rough shape of a real day.
//   DeepQueue      one symbol, a handful of prices, thousands of orders per level, removals
//                  from random queue positions: intrusive-list unlinking, FIFO order.
//   WidePrices     prices from $0.0001 to $199,999.9999 and jumps far from the touch:
//                  tick-ladder range limits, recentring, sparse fallback.
//   ReplaceChains  most messages are replaces, often at the same price: priority loss,
//                  ref-map churn (insert new + erase old every message).
//   Crossing       both sides placed randomly around the mid: locked and crossed books,
//                  which a book builder must represent, never match.
//   ManySymbols    thousands of symbols touched uniformly: per-symbol state and cache misses.
//   ErrorPaths     injects unknown refs, overfills, duplicate refs, locate mismatches,
//                  unknown message types and zero-share orders between valid messages.
//   Mixed          concatenation of all of the above, one block each.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lle::testing {

enum class FixtureMode : std::uint8_t {
    Realistic,
    DeepQueue,
    WidePrices,
    ReplaceChains,
    Crossing,
    ManySymbols,
    ErrorPaths,
    Mixed
};

[[nodiscard]] FixtureMode parse_fixture_mode(const std::string& s);  // throws std::invalid_argument
[[nodiscard]] const char* fixture_mode_name(FixtureMode m) noexcept;

struct FixtureConfig {
    std::uint64_t seed = 1;
    std::uint64_t messages = 100'000;  // book-changing messages (directory/system come extra)
    std::uint32_t symbols = 50;        // ignored by single-symbol modes
    FixtureMode mode = FixtureMode::Realistic;
};

// Called once per message with the FRAMED bytes ([len:2 BE][message]).
using FrameSink = std::function<void(const std::byte* framed, std::size_t len)>;

void generate_fixture(const FixtureConfig& cfg, const FrameSink& sink);

// Convenience: the whole stream in memory (tests; keep `messages` small).
[[nodiscard]] std::vector<std::byte> generate_fixture_bytes(const FixtureConfig& cfg);

}  // namespace lle::testing
