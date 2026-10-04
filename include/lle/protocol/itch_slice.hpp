#pragma once
// Cutting smaller ITCH streams out of a big one.
//
// Main use: reproducing a differential-test mismatch. A mismatch on symbol L at message i of a
// full day is reproduced by the messages of L alone, up to i: order refs are unique for the
// day and an order never changes symbol, so L's book depends on nothing else. The result is
// usually a few thousand messages instead of hundreds of millions, and can be replayed in a
// debugger or turned into a unit test.
//
// System messages (S) are kept so the slice has the same start/end-of-day markers.

#include <cstdint>
#include <string>

#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_file.hpp"

namespace lle::itch {

struct SliceStats {
    std::uint64_t read = 0;     // messages read from the input
    std::uint64_t written = 0;  // messages written to the slice
};

// Writes every message of `locate` (and every S message) with stream index <= last_index to
// `out_path` as a plain (uncompressed) framed ITCH file. Throws std::runtime_error on I/O errors.
SliceStats write_locate_slice(ItchFileReader& in, Locate locate, std::uint64_t last_index, const std::string& out_path);

}  // namespace lle::itch
