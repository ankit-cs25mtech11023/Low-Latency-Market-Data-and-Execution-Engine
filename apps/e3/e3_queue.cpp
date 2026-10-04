// E3: SPSC queue study. One producer thread, one consumer thread, one queue variant.
//
//   e3_queue --queue {mutex-cv|mutex-spin|seqcst|acqrel|acqrel-cached} --pad {0|64|128}
//            --mode {latency|throughput} --cpu-producer P --cpu-consumer C --out PREFIX
//            [--msgs N] [--warmup-msgs W] [--interval-ns I] [--counters LIST]
//   A CPU of -1 leaves that thread unpinned (the scheduler places it).
//
// Two modes, one per process run (run_bench.py makes the mode a parameter):
//
// latency (open loop): the producer sends one 64-byte message every I ns on a fixed
//   schedule computed up front, t_i = t_start + i * I. Each message carries its *intended*
//   send time t_i, and the consumer records (TSC when it has the message) - t_i. If the
//   producer falls behind (queue full, a sleeping consumer that is slow to wake, an
//   interrupt), the schedule does not move, so the delay shows up in the latency instead of
//   silently lowering the offered load ("coordinated omission"). E0 showed the per-core TSCs
//   agree within +/-2.2 ns, so a stamp taken on the producer's core can be subtracted from
//   one taken on the consumer's core. At I = 1000 ns the ring is almost always empty, so
//   this measures the cost of handing one message from one core to another: at least two
//   cache-line transfers (the slot, and the line holding `tail`).
//
// throughput (closed loop): the producer pushes as fast as the queue accepts; the consumer
//   times every block of 1 024 consecutive pops (value_divisor = 1024, so analyse.py reports
//   ns per message). This is the sustainable rate when both sides are always busy, where
//   batching effects (cached indices, several messages per cache line transfer) show up.
//
// Counters: one perf_event_open group per thread, enabled only over the measured messages.
// `counters` in the result file are the consumer's; `producer_counters` the producer's.
// CPU% per thread = thread CPU time / wall time over the measured window: a sleeping
// (condition-variable) consumer uses less than 100%, a spinning one 100%.

#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "lle/concurrency/spsc.hpp"
#include "lle/core/cli.hpp"
#include "lle/core/cpu.hpp"
#include "lle/core/tsc.hpp"
#include "lle/telemetry/histogram.hpp"
#include "lle/telemetry/perf_counters.hpp"
#include "lle/telemetry/result_io.hpp"

namespace {

using namespace lle;
using namespace lle::concurrency;

// One cache line per message: a realistic size for a decoded market-data event, and it makes
// "one slot = one line transfer" exact.
struct alignas(64) Msg {
    std::uint64_t seq;
    std::uint64_t tsc;  // intended send time (latency mode)
    std::uint64_t payload[6];
};
static_assert(sizeof(Msg) == 64);

constexpr std::size_t kCapacity = 1024;
constexpr std::size_t kBatch = 1024;

struct Options {
    std::string queue, mode, counters;
    std::size_t pad = 64;
    int cpu_producer = 2, cpu_consumer = 3;
    std::uint64_t msgs = 0, warmup = 0;
    double interval_ns = 1000.0;
};

std::int64_t clock_ns(clockid_t id) {
    timespec ts{};
    clock_gettime(id, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Per-thread measurement window: CPU time + wall time + PMU group.
// A perf_event_open group with pid = 0 follows the thread that opened it, so open() must be
// called on the thread being measured (each thread opens its own before the start barrier).
struct Window {
    std::string events;
    std::unique_ptr<PerfCounters> pmu;
    std::int64_t cpu0 = 0, wall0 = 0, cpu_ns = 0, wall_ns = 0;
    std::optional<PerfReading> reading;

    explicit Window(std::string ev) : events(std::move(ev)) {}
    void open() {
        if (events != "none") pmu = std::make_unique<PerfCounters>(parse_perf_events(events));
    }
    void begin() {
        if (pmu) pmu->start();
        cpu0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        wall0 = clock_ns(CLOCK_MONOTONIC);
    }
    void end() {
        cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu0;
        wall_ns = clock_ns(CLOCK_MONOTONIC) - wall0;
        if (pmu) reading = pmu->stop();
    }
    [[nodiscard]] double cpu_pct() const {
        return wall_ns > 0 ? 100.0 * static_cast<double>(cpu_ns) / static_cast<double>(wall_ns) : 0.0;
    }
    [[nodiscard]] std::string counters_json() const { return reading ? reading->to_json() : "null"; }
};

void maybe_pin(int cpu) {
    if (cpu >= 0) pin_current_thread(cpu);
}

struct Result {
    std::unique_ptr<Histogram> hist = std::make_unique<Histogram>();
    std::uint64_t measured_msgs = 0;
    std::uint64_t order_errors = 0;     // wrong sequence number: lost / duplicated / reordered
    std::uint64_t negative_samples = 0;  // consumer stamp before intended time (clock skew)
    int producer_ran_on = -1, consumer_ran_on = -1;
};

template <class Q>
Result run(const Options& o, const TscCalibration& cal, Window& wp, Window& wc) {
    auto q = std::make_unique<Q>();
    Result r;
    const std::uint64_t total = o.warmup + o.msgs;
    const bool latency = o.mode == "latency";
    const auto interval_ticks = static_cast<std::uint64_t>(o.interval_ns * cal.ticks_per_ns);
    std::atomic<int> ready{0};

    std::thread producer([&] {
        maybe_pin(o.cpu_producer);
        set_thread_name("e3-producer");
        wp.open();
        ready.fetch_add(1);
        while (ready.load() < 2) {
        }
        Msg m{};
        if (latency) {
            // Start 1 ms from now so both threads are spinning before the first send.
            const std::uint64_t t0 = Tsc::now() + static_cast<std::uint64_t>(1e6 * cal.ticks_per_ns);
            for (std::uint64_t i = 0; i < total; ++i) {
                if (i == o.warmup) wp.begin();
                const std::uint64_t intended = t0 + i * interval_ticks;
                while (Tsc::now() < intended) {
                }
                m.seq = i;
                m.tsc = intended;
                push_blocking(*q, m);
            }
        } else {
            for (std::uint64_t i = 0; i < total; ++i) {
                if (i == o.warmup) wp.begin();
                m.seq = i;
                push_blocking(*q, m);
            }
        }
        wp.end();
        r.producer_ran_on = current_cpu();
    });

    maybe_pin(o.cpu_consumer);
    set_thread_name("e3-consumer");
    wc.open();
    ready.fetch_add(1);
    while (ready.load() < 2) {
    }
    Msg m{};
    std::uint64_t t_prev = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
        if (i == o.warmup) {
            wc.begin();
            t_prev = Tsc::start();
        }
        pop_blocking(*q, m);
        if (latency) {
            const std::uint64_t now = Tsc::stop();
            r.order_errors += m.seq != i ? 1U : 0U;
            if (i >= o.warmup) {
                if (now >= m.tsc) {
                    r.hist->record(now - m.tsc);
                } else {
                    ++r.negative_samples;
                    r.hist->record(0);
                }
            }
        } else {
            r.order_errors += m.seq != i ? 1U : 0U;
            if (i >= o.warmup && (i - o.warmup + 1) % kBatch == 0) {
                const std::uint64_t now = Tsc::stop();
                r.hist->record(now - t_prev);
                t_prev = now;
            }
        }
    }
    wc.end();
    r.consumer_ran_on = current_cpu();
    producer.join();
    r.measured_msgs = latency ? o.msgs : (o.msgs / kBatch) * kBatch;
    return r;
}

template <bool kCached, Ordering Ord>
Result run_padded(const Options& o, const TscCalibration& cal, Window& wp, Window& wc) {
    switch (o.pad) {
        case 0: return run<SpscRing<Msg, kCapacity, Ord, kCached, 0>>(o, cal, wp, wc);
        case 64: return run<SpscRing<Msg, kCapacity, Ord, kCached, 64>>(o, cal, wp, wc);
        case 128: return run<SpscRing<Msg, kCapacity, Ord, kCached, 128>>(o, cal, wp, wc);
        default: throw std::invalid_argument("--pad must be 0, 64 or 128");
    }
}

Result dispatch(const Options& o, const TscCalibration& cal, Window& wp, Window& wc) {
    if (o.queue == "mutex-cv") return run<MutexCvQueue<Msg, kCapacity>>(o, cal, wp, wc);
    if (o.queue == "mutex-spin") return run<MutexSpinQueue<Msg, kCapacity>>(o, cal, wp, wc);
    if (o.queue == "seqcst") return run_padded<false, Ordering::kSeqCst>(o, cal, wp, wc);
    if (o.queue == "acqrel") return run_padded<false, Ordering::kAcqRel>(o, cal, wp, wc);
    if (o.queue == "acqrel-cached") return run_padded<true, Ordering::kAcqRel>(o, cal, wp, wc);
    throw std::invalid_argument("unknown --queue " + o.queue);
}

}  // namespace

int main(int argc, char** argv) try {
    const Cli cli(argc, argv);
    Options o;
    o.queue = cli.str("queue");
    o.mode = cli.str("mode");
    if (o.mode != "latency" && o.mode != "throughput") throw std::invalid_argument("--mode latency|throughput");
    o.pad = static_cast<std::size_t>(cli.i64("pad", 64));
    o.cpu_producer = static_cast<int>(cli.i64("cpu-producer", 2));
    o.cpu_consumer = static_cast<int>(cli.i64("cpu-consumer", 3));
    // --place P-C (e.g. 2-3 separate cores, 2-6 SMT siblings) or "unpinned": one parameter
    // for the placement sweep, overriding --cpu-producer/--cpu-consumer.
    if (cli.has("place")) {
        const std::string p = cli.str("place");
        if (p == "unpinned") {
            o.cpu_producer = o.cpu_consumer = -1;
        } else {
            const auto dash = p.find('-');
            if (dash == std::string::npos) throw std::invalid_argument("--place P-C or unpinned");
            o.cpu_producer = std::stoi(p.substr(0, dash));
            o.cpu_consumer = std::stoi(p.substr(dash + 1));
        }
    }
    o.msgs = static_cast<std::uint64_t>(cli.i64("msgs", o.mode == "latency" ? 2'000'000 : 20'000'000));
    o.warmup = static_cast<std::uint64_t>(cli.i64("warmup-msgs", 100'000));
    o.interval_ns = cli.f64("interval-ns", 1000.0);
    o.counters = cli.str("counters", "cycles,instructions,cache-misses,L1-dcache-load-misses,context-switches");
    const std::string out = cli.str("out");

    require_invariant_tsc();
    const TscCalibration cal = calibrate_tsc(cli.f64("calib-s", 1.0));

    Window wp(o.counters), wc(o.counters);
    const Result r = dispatch(o, cal, wp, wc);
    if (r.order_errors != 0) throw std::runtime_error(std::to_string(r.order_errors) + " messages out of order");

    const bool latency = o.mode == "latency";
    const bool padded = o.queue != "mutex-cv" && o.queue != "mutex-spin";
    char cpu_pct[160];
    std::snprintf(cpu_pct, sizeof cpu_pct,
                  R"({"producer_cpu_pct":%.2f,"consumer_cpu_pct":%.2f,"consumer_wall_ms":%.3f})", wp.cpu_pct(),
                  wc.cpu_pct(), static_cast<double>(wc.wall_ns) / 1e6);
    const std::string params =
        R"({"experiment":"E3","queue":")" + o.queue + R"(","pad":)" + std::to_string(o.pad) +
        R"(,"pad_applies":)" + (padded ? "true" : "false") + R"(,"mode":")" + o.mode + R"(","capacity":)" +
        std::to_string(kCapacity) + R"(,"msg_bytes":)" + std::to_string(sizeof(Msg)) + R"(,"cpu_producer":)" +
        std::to_string(o.cpu_producer) + R"(,"cpu_consumer":)" + std::to_string(o.cpu_consumer) +
        R"(,"producer_ran_on_cpu":)" + std::to_string(r.producer_ran_on) + R"(,"consumer_ran_on_cpu":)" +
        std::to_string(r.consumer_ran_on) + R"(,"interval_ns":)" + (latency ? std::to_string(o.interval_ns) : "null") +
        R"(,"value_divisor":)" + std::to_string(latency ? 1 : kBatch) + R"(,"unit":"ticks","warmup_msgs":)" +
        std::to_string(o.warmup) + R"(,"measured_msgs":)" + std::to_string(r.measured_msgs) +
        R"(,"negative_samples":)" + std::to_string(r.negative_samples) + R"(,"extra_metrics":)" + cpu_pct +
        R"(,"counters":)" + wc.counters_json() + R"(,"producer_counters":)" + wp.counters_json() + "}";
    write_run(out, *r.hist, cal, params);

    const double div = latency ? 1.0 : static_cast<double>(kBatch);
    std::printf("%s pad=%zu %s P%d->C%d: p50 %.1f ns%s, p99 %.1f ns, CPU%% prod %.0f cons %.0f\n", o.queue.c_str(),
                o.pad, o.mode.c_str(), o.cpu_producer, o.cpu_consumer,
                cal.to_ns(static_cast<double>(r.hist->value_at_quantile(0.5)) / div), latency ? "" : "/msg",
                cal.to_ns(static_cast<double>(r.hist->value_at_quantile(0.99)) / div), wp.cpu_pct(), wc.cpu_pct());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "e3_queue: %s\n", e.what());
    return 1;
}
