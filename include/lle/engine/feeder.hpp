#pragma once
// The E4 feeder: replays ITCH messages from memory into the engine's input ring on an
// open-loop schedule.
//
// Why open loop: a closed-loop load generator ("send the next message when the engine has
// taken the previous one") slows down exactly when the engine is slow, so the slow periods
// get fewer messages and the measured latency looks better than reality (coordinated
// omission). Here the send time of every message is fixed BEFORE the run starts. If the
// engine falls behind, the ring fills, the feeder blocks, and it sends late; but each
// message carries its *intended* send time and latency is measured from that, so the wait
// is counted.
//
// A run has two parts:
//   1. Warm-up: every message before the measured window is sent as fast as the engine
//      accepts it (closed loop, intended = 0 = "do not measure"). This rebuilds the order
//      books up to the window start exactly as in the real day. The feeder then waits until
//      the engine has finished the warm-up, so the window starts with empty queues.
//   2. Window: M messages, each sent at t0 + offset[i] (TSC ticks). Offsets come from one
//      of two arrival patterns with the same mean rate:
//        smooth  fixed interval 1/rate (every message evenly spaced);
//        bursty  the window's original feed timestamps, compressed or stretched so the
//                window's mean rate equals `rate`. The real feed's bursts (E1: 1 ms peaks
//                at 175x the mean) are kept, only scaled in time.
//      Throughput mode (closed loop, for capacity) sends the window as fast as possible.
//
// Loading: the messages are read into one contiguous byte array before the run (startup
// cost, not measured), so the feeder never touches the disk or a decompressor while
// sending.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "lle/concurrency/spsc.hpp"
#include "lle/core/tsc.hpp"
#include "lle/engine/messages.hpp"
#include "lle/protocol/endian.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/telemetry/histogram.hpp"

namespace lle::engine {

// Messages [0, warmup + window) of a feed, in memory.
struct FeedData {
    std::vector<std::byte> bytes;        // message bodies back to back (no length prefixes)
    std::vector<std::uint32_t> offset;   // body i is bytes[offset[i], offset[i+1])
    std::vector<std::uint64_t> feed_ns;  // ITCH timestamp of each message (ns since midnight)
    std::size_t warmup = 0;              // messages before the window
    std::size_t window = 0;              // measured messages

    [[nodiscard]] std::size_t total() const noexcept { return warmup + window; }
    [[nodiscard]] const std::byte* msg(std::size_t i) const noexcept { return bytes.data() + offset[i]; }
    [[nodiscard]] std::uint16_t len(std::size_t i) const noexcept {
        return static_cast<std::uint16_t>(offset[i + 1] - offset[i]);
    }
};

// Reads messages from `reader` until `window` messages at or after feed time `start_ns`
// have been read. Everything before the first message with timestamp >= start_ns is the
// warm-up. Throws if the input ends before the window is complete or a message is too long.
//
// `size_hint` (optional) is the size of an uncompressed input file in bytes. It is an upper
// bound on the bodies' total size, so the vectors are reserved once up front. Without it,
// push_back doubles the capacity as it grows: during each reallocation the old and the new
// buffer both exist, so a 1 GB feed would briefly need ~3 GB on this 7.6 GiB machine.
// Reserving is cheap: pages the kernel never touches are never backed by RAM, so the
// generous message-count bound (every message at least 12 bytes + 2-byte length) is free.
inline FeedData load_feed(itch::ItchFileReader& reader, std::uint64_t start_ns, std::size_t window,
                          std::size_t size_hint = 0) {
    FeedData f;
    if (size_hint > 0) {
        f.bytes.reserve(size_hint);
        f.offset.reserve(size_hint / 14 + 2);
        f.feed_ns.reserve(size_hint / 14 + 1);
    }
    f.offset.push_back(0);
    const std::byte* p = nullptr;
    std::size_t len = 0;
    bool in_window = false;
    while (f.window < window && reader.next(p, len)) {
        if (len > kMaxRawLen) throw std::runtime_error("ITCH message longer than a RawSlot");
        const std::uint64_t ts = len >= 11 ? proto::load_be48(p + 5) : 0;
        if (!in_window && ts >= start_ns) {
            in_window = true;
            f.warmup = f.feed_ns.size();
        }
        f.bytes.insert(f.bytes.end(), p, p + len);
        f.offset.push_back(static_cast<std::uint32_t>(f.bytes.size()));
        f.feed_ns.push_back(ts);
        if (in_window) ++f.window;
        if (f.bytes.size() > 0xF000'0000u) throw std::runtime_error("feed prefix larger than 3.75 GiB");
    }
    if (f.window < window)
        throw std::runtime_error("input ended after " + std::to_string(f.window) + " of " + std::to_string(window) +
                                 " window messages");
    return f;
}

enum class Arrival : std::uint8_t { kSmooth, kBursty };

// Send offsets (TSC ticks from the window start) for the window's messages, mean rate `rate`
// messages per second. The last message is sent at about (window - 1) / rate seconds in both
// patterns, so smooth and bursty runs at the same rate carry the same total load.
inline std::vector<std::uint64_t> make_schedule(const FeedData& f, Arrival a, double rate, double ticks_per_ns) {
    if (rate <= 0) throw std::invalid_argument("rate must be > 0");
    const double ticks_per_msg = 1e9 / rate * ticks_per_ns;
    std::vector<std::uint64_t> off(f.window);
    if (a == Arrival::kSmooth || f.window < 2) {
        for (std::size_t i = 0; i < f.window; ++i)
            off[i] = static_cast<std::uint64_t>(std::llround(static_cast<double>(i) * ticks_per_msg));
        return off;
    }
    const std::uint64_t first = f.feed_ns[f.warmup];
    const std::uint64_t span = f.feed_ns[f.warmup + f.window - 1] - first;
    if (span == 0) throw std::runtime_error("bursty arrival: the window has no time span");
    // feed ns -> ticks, chosen so the whole window lasts (window - 1) / rate seconds.
    const double scale = static_cast<double>(f.window - 1) * ticks_per_msg / static_cast<double>(span);
    for (std::size_t i = 0; i < f.window; ++i) {
        const std::uint64_t ts = f.feed_ns[f.warmup + i];
        // Feed timestamps are non-decreasing in a valid file; clamp just in case.
        off[i] = ts >= first ? static_cast<std::uint64_t>(std::llround(static_cast<double>(ts - first) * scale)) : 0;
        if (i > 0 && off[i] < off[i - 1]) off[i] = off[i - 1];
    }
    return off;
}

struct FeederStats {
    // How late the feeder actually handed each window message to the ring, relative to the
    // schedule (TSC ticks). Large values mean the ring was full: the engine was saturated.
    Histogram lag;
    std::uint64_t ring_full_spins = 0;  // failed push attempts (ring full)
};

// Runs on the feeder thread. `warmup_done` is set by the engine's last stage after it has
// consumed every warm-up message. `schedule` empty = throughput mode (window sent closed
// loop; every window message gets intended = 1, a non-zero "measure me" marker).
template <class Ring>
void run_feeder(const FeedData& f, const std::vector<std::uint64_t>& schedule, Ring& ring,
                const std::atomic<bool>& warmup_done, double ticks_per_ns, FeederStats& st) {
    RawSlot s{};
    const auto send = [&](std::size_t i, std::uint64_t intended) {
        s.intended = intended;
        s.len = f.len(i);
        std::memcpy(s.data, f.msg(i), s.len);
        while (!ring.try_push(s)) ++st.ring_full_spins;
    };
    for (std::size_t i = 0; i < f.warmup; ++i) send(i, 0);
    while (!warmup_done.load(std::memory_order_acquire)) {
    }
    st.ring_full_spins = 0;  // only the window's count matters
    if (schedule.empty()) {
        for (std::size_t i = 0; i < f.window; ++i) send(f.warmup + i, 1);
        return;
    }
    // Start 1 ms from now so the engine threads are spinning on an empty ring.
    const std::uint64_t t0 = Tsc::now() + static_cast<std::uint64_t>(1e6 * ticks_per_ns);
    for (std::size_t i = 0; i < f.window; ++i) {
        const std::uint64_t intended = t0 + schedule[i];
        while (Tsc::now() < intended) {
        }
        send(f.warmup + i, intended);
        st.lag.record(Tsc::now() - intended);
    }
}

}  // namespace lle::engine
