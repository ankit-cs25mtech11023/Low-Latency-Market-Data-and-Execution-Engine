#include "lle/core/tsc.hpp"

#include <ctime>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace lle {
namespace {

std::int64_t mono_raw_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

struct Pair {
    std::uint64_t tsc;
    std::int64_t ns;
    std::uint64_t width;  // ticks between the two TSC reads bracketing the clock read
};

// Reads (TSC, clock) as close together as possible: the clock read is bracketed by two
// TSC reads and the narrowest of several attempts wins. The midpoint of the bracket is
// the TSC value that corresponds to the clock reading, +/- width/2.
Pair sample_pair() noexcept {
    Pair best{0, 0, std::numeric_limits<std::uint64_t>::max()};
    for (int i = 0; i < 64; ++i) {
        const std::uint64_t a = Tsc::start();
        const std::int64_t ns = mono_raw_ns();
        const std::uint64_t b = Tsc::stop();
        if (b - a < best.width) best = Pair{a + (b - a) / 2, ns, b - a};
    }
    return best;
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool has_flag(const std::string& flags_line, const std::string& flag) {
    std::istringstream ss(flags_line);
    std::string tok;
    while (ss >> tok)
        if (tok == flag) return true;
    return false;
}

}  // namespace

TscCalibration calibrate_tsc(double seconds) {
    const Pair p0 = sample_pair();
    const auto target_ns = static_cast<std::int64_t>(seconds * 1e9);
    while (mono_raw_ns() - p0.ns < target_ns) Tsc::relax();
    const Pair p1 = sample_pair();

    TscCalibration c;
    const auto dns = static_cast<double>(p1.ns - p0.ns);
    c.ticks_per_ns = static_cast<double>(p1.tsc - p0.tsc) / dns;
    c.tsc0 = p0.tsc;
    c.mono_raw_ns0 = p0.ns;
    c.duration_s = dns / 1e9;
    c.max_pair_error_ns = static_cast<double>(std::max(p0.width, p1.width)) / 2.0 / c.ticks_per_ns;
    return c;
}

std::string TscCalibration::to_json() const {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  R"({"ticks_per_ns":%.9f,"tsc0":%llu,"mono_raw_ns0":%lld,"duration_s":%.6f,"max_pair_error_ns":%.3f})",
                  ticks_per_ns, static_cast<unsigned long long>(tsc0), static_cast<long long>(mono_raw_ns0),
                  duration_s, max_pair_error_ns);
    return buf;
}

TscFeatures detect_tsc_features() {
    TscFeatures f;
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.starts_with("flags")) {
            f.constant_tsc = has_flag(line, "constant_tsc");
            f.nonstop_tsc = has_flag(line, "nonstop_tsc");
            f.rdtscp = has_flag(line, "rdtscp");
            break;
        }
    }
    f.clocksource = read_file("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    while (!f.clocksource.empty() && (f.clocksource.back() == '\n' || f.clocksource.back() == ' '))
        f.clocksource.pop_back();
    return f;
}

std::string TscFeatures::to_json() const {
    std::ostringstream o;
    o << R"({"constant_tsc":)" << (constant_tsc ? "true" : "false") << R"(,"nonstop_tsc":)"
      << (nonstop_tsc ? "true" : "false") << R"(,"rdtscp":)" << (rdtscp ? "true" : "false")
      << R"(,"clocksource":")" << clocksource << R"("})";
    return o.str();
}

void require_invariant_tsc() {
    const TscFeatures f = detect_tsc_features();
    if (!f.invariant())
        throw std::runtime_error("TSC is not invariant (need constant_tsc, nonstop_tsc, rdtscp): " + f.to_json());
}

}  // namespace lle
