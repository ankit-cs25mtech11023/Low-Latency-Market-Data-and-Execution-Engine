#include "lle/telemetry/perf_counters.hpp"

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace lle {
namespace {

struct EventSpec {
    PerfEvent event;
    const char* name;
    std::uint32_t type;
    std::uint64_t config;
};

constexpr std::uint64_t cache_event(std::uint64_t cache, std::uint64_t op, std::uint64_t result) {
    // Encoding defined in perf_event_open(2) for PERF_TYPE_HW_CACHE.
    return cache | (op << 8) | (result << 16);
}

constexpr EventSpec kSpecs[] = {
    {PerfEvent::kCycles, "cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {PerfEvent::kInstructions, "instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
    {PerfEvent::kRefCycles, "ref-cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_REF_CPU_CYCLES},
    {PerfEvent::kBranchMisses, "branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
    {PerfEvent::kCacheMisses, "cache-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
    {PerfEvent::kL1dReadMisses, "L1-dcache-load-misses", PERF_TYPE_HW_CACHE,
     cache_event(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
    {PerfEvent::kDtlbReadMisses, "dTLB-load-misses", PERF_TYPE_HW_CACHE,
     cache_event(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
    {PerfEvent::kPageFaults, "page-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS},
    {PerfEvent::kContextSwitches, "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES},
};

const EventSpec& spec_of(PerfEvent e) {
    for (const auto& s : kSpecs)
        if (s.event == e) return s;
    throw std::invalid_argument("unknown PerfEvent");
}

int perf_event_open(perf_event_attr* attr, int group_fd) {
    // pid = 0, cpu = -1: this thread, on whatever CPU it runs.
    return static_cast<int>(syscall(SYS_perf_event_open, attr, 0, -1, group_fd, 0UL));
}

}  // namespace

const char* perf_event_name(PerfEvent e) noexcept {
    for (const auto& s : kSpecs)
        if (s.event == e) return s.name;
    return "?";
}

std::vector<PerfEvent> parse_perf_events(const std::string& csv) {
    std::vector<PerfEvent> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        bool found = false;
        for (const auto& s : kSpecs)
            if (tok == s.name) {
                out.push_back(s.event);
                found = true;
            }
        if (!found) throw std::invalid_argument("unknown perf event '" + tok + "'");
    }
    return out;
}

bool PerfReading::has(PerfEvent e) const noexcept {
    for (const auto x : events)
        if (x == e) return true;
    return false;
}

std::uint64_t PerfReading::get(PerfEvent e) const noexcept {
    for (std::size_t i = 0; i < events.size(); ++i)
        if (events[i] == e) return values[i];
    return 0;
}

double PerfReading::running_fraction() const noexcept {
    return time_enabled_ns ? static_cast<double>(time_running_ns) / static_cast<double>(time_enabled_ns) : 0.0;
}

std::string PerfReading::to_json() const {
    std::string s = "{";
    for (std::size_t i = 0; i < events.size(); ++i) {
        s += "\"" + std::string(perf_event_name(events[i])) + "\":" + std::to_string(values[i]) + ",";
    }
    char buf[128];
    std::snprintf(buf, sizeof buf, R"("time_enabled_ns":%llu,"time_running_ns":%llu})",
                  static_cast<unsigned long long>(time_enabled_ns), static_cast<unsigned long long>(time_running_ns));
    return s + buf;
}

PerfCounters::PerfCounters(std::vector<PerfEvent> events) : events_(std::move(events)) {
    if (events_.empty()) throw std::invalid_argument("PerfCounters: no events");
    for (std::size_t i = 0; i < events_.size(); ++i) {
        const EventSpec& spec = spec_of(events_[i]);
        perf_event_attr attr{};
        attr.size = sizeof attr;
        attr.type = spec.type;
        attr.config = spec.config;
        // Only the leader starts disabled; members are enabled and follow the leader.
        attr.disabled = i == 0 ? 1 : 0;
        attr.exclude_hv = 1;
        attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        const int fd = perf_event_open(&attr, i == 0 ? -1 : fds_[0]);
        if (fd < 0) {
            const int err = errno;
            for (const int f : fds_) close(f);
            throw std::system_error(err, std::generic_category(),
                                    std::string("perf_event_open(") + spec.name + ")");
        }
        fds_.push_back(fd);
    }
}

PerfCounters::~PerfCounters() {
    for (const int f : fds_) close(f);
}

void PerfCounters::start() {
    ioctl(fds_[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(fds_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

PerfReading PerfCounters::stop() {
    ioctl(fds_[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    // PERF_FORMAT_GROUP layout: nr, time_enabled, time_running, value[nr].
    std::vector<std::uint64_t> buf(3 + events_.size());
    const auto want = static_cast<ssize_t>(buf.size() * sizeof(std::uint64_t));
    if (read(fds_[0], buf.data(), static_cast<std::size_t>(want)) != want)
        throw std::system_error(errno, std::generic_category(), "read(perf group)");

    PerfReading r;
    r.events = events_;
    r.time_enabled_ns = buf[1];
    r.time_running_ns = buf[2];
    const double scale = r.time_running_ns ? static_cast<double>(r.time_enabled_ns) /
                                                 static_cast<double>(r.time_running_ns)
                                           : 0.0;
    for (std::size_t i = 0; i < events_.size(); ++i) {
        const std::uint64_t raw = buf[3 + i];
        r.values.push_back(r.time_running_ns == r.time_enabled_ns
                               ? raw
                               : static_cast<std::uint64_t>(static_cast<double>(raw) * scale));
    }
    return r;
}

}  // namespace lle
