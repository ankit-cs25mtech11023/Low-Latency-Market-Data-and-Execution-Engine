#pragma once
// Hardware performance counters around one code region, read with perf_event_open(2).
//
// Why not just wrap the process in `perf stat`? Because `perf stat` counts the whole
// process: TSC calibration, warm-up, file loading, result writing. An experiment wants the
// counts for the *measured region only* (e.g. "instructions per ITCH message while replaying",
// not including gzip decompression of the input). This class opens a counter group,
// enables it right before the region and disables it right after.
//
// How it works:
//   * One perf event per counter, all in one *group* (the first is the leader). A group is
//     scheduled onto the PMU together, so all counters cover exactly the same time window
//     and ratios such as instructions/cycles are consistent.
//   * Counting mode only (no sampling, no interrupts), so the overhead inside the region is
//     zero: the hardware increments the counters, nobody reads them until stop().
//   * The counters follow the calling thread (pid = 0, cpu = -1) and count user and kernel
//     work done for it. kernel.perf_event_paranoid <= 1 allows kernel counting for one's
//     own process (see docs/linux-tuning.md).
//   * time_enabled vs time_running tells whether the kernel had to multiplex (time-share)
//     the PMU between more events than there are hardware counters. Multiplexed counts are
//     estimates; result files record the ratio so a reader can see it.
//
// The i5-8250U has 3 fixed counters (instructions, core cycles, reference cycles) plus 4
// general-purpose counters per logical CPU (one of which the NMI watchdog takes unless
// tune_machine.sh disabled it).
//
// Not for the hot path itself: start()/stop() are system calls (~1 µs each).

#include <cstdint>
#include <string>
#include <vector>

namespace lle {

enum class PerfEvent : std::uint8_t {
    kCycles,        // core clock cycles (runs at the actual core frequency)
    kInstructions,  // instructions retired
    kRefCycles,     // reference cycles: ticks at a fixed rate regardless of core frequency
    kBranchMisses,
    kCacheMisses,  // "LLC misses" on Intel (generic PERF_COUNT_HW_CACHE_MISSES)
    kL1dReadMisses,
    kDtlbReadMisses,
    kPageFaults,      // software event: no PMU counter needed
    kContextSwitches  // software event
};

[[nodiscard]] const char* perf_event_name(PerfEvent e) noexcept;

// Parses a comma-separated list such as "cycles,instructions,ref-cycles".
// Throws std::invalid_argument on an unknown name.
[[nodiscard]] std::vector<PerfEvent> parse_perf_events(const std::string& csv);

struct PerfReading {
    std::vector<PerfEvent> events;
    std::vector<std::uint64_t> values;  // scaled by time_enabled/time_running if multiplexed
    std::uint64_t time_enabled_ns = 0;
    std::uint64_t time_running_ns = 0;

    [[nodiscard]] bool has(PerfEvent e) const noexcept;
    [[nodiscard]] std::uint64_t get(PerfEvent e) const noexcept;  // 0 if not measured
    // Fraction of the enabled time the group was actually on the PMU (1.0 = not multiplexed).
    [[nodiscard]] double running_fraction() const noexcept;
    [[nodiscard]] std::string to_json() const;
};

class PerfCounters {
public:
    // Opens the group for the calling thread. Throws std::system_error if the kernel refuses
    // (no PMU in a VM/container, perf_event_paranoid too high, unsupported event).
    explicit PerfCounters(std::vector<PerfEvent> events);
    ~PerfCounters();
    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    void start();        // reset + enable the group
    PerfReading stop();  // disable the group and read it
    // Accumulate over several separate regions without resetting: start() then pause()
    // once, then resume()/pause() around each region, then stop() to read the sum.
    void pause();   // disable, keep counts
    void resume();  // enable, keep counts

private:
    std::vector<PerfEvent> events_;
    std::vector<int> fds_;  // fds_[0] is the group leader
};

}  // namespace lle
