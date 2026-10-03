#pragma once
// Architecture-neutral cycle clock.
//
// All latency measurements in the engine are taken in raw counter ticks (cheap, monotonic)
// and converted to nanoseconds only offline, using a TscCalibration recorded alongside
// the results. Code outside lle/arch/ must use this header, never the arch intrinsics,
// so an AArch64 backend (CNTVCT_EL0, v1.1) can be dropped in later.

#include <cstdint>
#include <string>

#if defined(__x86_64__)
#include "lle/arch/x86/tsc.hpp"
#else
#error "Only x86-64 is supported until the AArch64 backend lands (plan.md Phase 9)"
#endif

namespace lle {

struct Tsc {
    using ticks = std::uint64_t;

#if defined(__x86_64__)
    // Unordered read; use for timestamps that do not bracket a region (e.g. "message arrived").
    [[gnu::always_inline]] static ticks now() noexcept { return arch::x86::rdtsc(); }
    // Region brackets (see lle/arch/x86/tsc.hpp for why these differ).
    [[gnu::always_inline]] static ticks start() noexcept { return arch::x86::rdtsc_start(); }
    [[gnu::always_inline]] static ticks stop() noexcept { return arch::x86::rdtsc_end(); }
    [[gnu::always_inline]] static void relax() noexcept { arch::x86::cpu_relax(); }
#endif
};

// Linear mapping between TSC ticks and CLOCK_MONOTONIC_RAW nanoseconds.
struct TscCalibration {
    double ticks_per_ns = 0.0;   // e.g. 1.8 for a 1.8 GHz TSC
    std::uint64_t tsc0 = 0;      // reference point: tsc0 <-> mono_raw_ns0
    std::int64_t mono_raw_ns0 = 0;
    double duration_s = 0.0;     // length of the calibration window
    double max_pair_error_ns = 0.0;  // uncertainty of the clock/TSC sample pairs

    [[nodiscard]] double to_ns(double ticks) const noexcept { return ticks / ticks_per_ns; }
    [[nodiscard]] double ghz() const noexcept { return ticks_per_ns; }
    [[nodiscard]] std::string to_json() const;
};

// Calibrates the TSC against CLOCK_MONOTONIC_RAW (not adjusted by NTP) over `seconds`.
// Busy-waits rather than sleeps, so the core stays awake for the whole window.
[[nodiscard]] TscCalibration calibrate_tsc(double seconds = 1.0);

struct TscFeatures {
    bool constant_tsc = false;  // rate does not change with P-states
    bool nonstop_tsc = false;   // keeps ticking in deep C-states
    bool rdtscp = false;
    std::string clocksource;    // kernel clocksource; "tsc" means the vDSO uses it too
    [[nodiscard]] bool invariant() const noexcept { return constant_tsc && nonstop_tsc && rdtscp; }
    [[nodiscard]] std::string to_json() const;
};

// Reads /proc/cpuinfo and /sys clocksource. Never throws; missing files leave fields false/empty.
[[nodiscard]] TscFeatures detect_tsc_features();

// Startup gate for benchmarks: throws std::runtime_error if the TSC is not invariant,
// because then tick deltas are not proportional to wall time.
void require_invariant_tsc();

}  // namespace lle
