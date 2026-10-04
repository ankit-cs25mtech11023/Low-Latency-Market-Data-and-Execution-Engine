// E4 driver: run-to-completion (Model A) vs pipelined (Model B) under open-loop load.
//
//   e4_threading --model a|b --input FILE --out PREFIX
//                [--mode latency|throughput] [--arrival smooth|bursty] [--rate MSGS_PER_S]
//                [--work-ns K] [--start-time HH:MM:SS] [--window M] [--hops 0|1]
//                [--cpu-feeder 0] [--cpu-a 2] [--cpus-b 1,2,3] [--counters LIST]
//
// One process = one run of one configuration (run_bench.py interleaves N runs per config).
//
// What happens in a run:
//   1. Load the feed prefix into memory: every message of FILE up to the end of the measured
//      window (M messages starting at feed time --start-time). Not timed.
//   2. Start the feeder thread (pinned to --cpu-feeder) and the engine thread(s): Model A
//      on --cpu-a, Model B's decode / decision / sink stages on --cpus-b.
//   3. Warm-up: all messages before the window are pushed through the engine closed loop
//      (books rebuilt exactly as in the real day), unmeasured.
//   4. Window: the feeder replays M messages on the open-loop schedule (latency mode) or as
//      fast as the engine takes them (throughput mode, for capacity).
//
// Results (<prefix>.hist.csv + .meta.json, as for every experiment):
//   latency mode     histogram of end-to-end latency per message, from the intended send
//                    time to the end of the sink: includes queueing in every ring.
//   throughput mode  ns per message (value_divisor 1024) and msgs_per_s in extra_metrics.
//   extra_metrics    feeder lag percentiles (how late the feeder could send = how full the
//                    input ring was), orders sent, per-step percentiles with --hops 1.
//   params.digest    hash of every order decision: must be identical for A and B on the
//                    same input (same work done), checked by scripts/e4_check_digest.py.
//
// Hardware counters default to "none" in latency mode: there the engine threads spend most
// of their time spinning on an empty ring, so cycles/instructions per message describe the
// spin loop, not the work. In throughput mode the threads are always busy and the counters
// (summed over all engine threads for Model B) are cycles and instructions per message.

#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lle/book/fast_book.hpp"
#include "lle/concurrency/spsc.hpp"
#include "lle/core/cli.hpp"
#include "lle/core/cpu.hpp"
#include "lle/core/tsc.hpp"
#include "lle/engine/feeder.hpp"
#include "lle/engine/messages.hpp"
#include "lle/engine/models.hpp"
#include "lle/engine/stages.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/telemetry/histogram.hpp"
#include "lle/telemetry/perf_counters.hpp"
#include "lle/telemetry/result_io.hpp"

namespace {

using namespace lle;
using namespace lle::engine;
using concurrency::Ordering;
using concurrency::SpscRing;

// E3's best variant: acquire/release, cached remote index, 128-byte index padding.
constexpr std::size_t kRingSlots = 1024;
using InRing = SpscRing<RawSlot, kRingSlots, Ordering::kAcqRel, true, 128>;
using EventRing = SpscRing<Event, kRingSlots, Ordering::kAcqRel, true, 128>;
using DecisionRing = SpscRing<Decision, kRingSlots, Ordering::kAcqRel, true, 128>;
using Book = book::FastBookFib;

struct Options {
    std::string model, mode, arrival, input, out, counters;
    double rate = 1e6;
    double work_ns = 0;
    std::uint64_t start_ns = 0;
    std::size_t window = 2'000'000;
    bool hops = false;
    int cpu_feeder = 0, cpu_a = 2;
    int cpus_b[3] = {1, 2, 3};
};

// Hardware counters for one engine thread, enabled from its first measured record to its
// last record. The group must be opened on the thread it measures (pid = 0 follows the
// calling thread), so open() runs on that thread before the start barrier.
struct Probe {
    std::vector<PerfEvent> events;
    std::unique_ptr<PerfCounters> pmu;
    std::optional<PerfReading> reading;
    void open() {
        if (!events.empty()) pmu = std::make_unique<PerfCounters>(events);
    }
    void begin() {
        if (pmu) pmu->start();
    }
    void end() {
        if (pmu) reading = pmu->stop();
    }
};

// Sum of several threads' readings (same event list): total engine work per message.
std::string summed_counters_json(const Probe* probes, int n) {
    std::optional<PerfReading> sum;
    for (int i = 0; i < n; ++i) {
        if (!probes[i].reading) return "null";
        const PerfReading& r = *probes[i].reading;
        if (!sum) {
            sum = r;
            continue;
        }
        for (std::size_t k = 0; k < r.values.size(); ++k) sum->values[k] += r.values[k];
        // Enabled/running times: keep the worst (lowest) running fraction across threads.
        if (r.running_fraction() < sum->running_fraction()) {
            sum->time_enabled_ns = r.time_enabled_ns;
            sum->time_running_ns = r.time_running_ns;
        }
    }
    return sum ? sum->to_json() : "null";
}

std::uint64_t parse_hms(const std::string& s) {
    unsigned h = 0, m = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%u:%u:%u", &h, &m, &sec) != 3 || h > 23 || m > 59 || sec > 59)
        throw std::invalid_argument("--start-time must be HH:MM:SS");
    return (std::uint64_t{h} * 3600 + m * 60 + sec) * 1'000'000'000ULL;
}

std::string pct_json(const char* name, const Histogram& h, const TscCalibration& cal) {
    char buf[200];
    std::snprintf(buf, sizeof buf, R"("%s_p50_ns":%.1f,"%s_p99_ns":%.1f,"%s_max_ns":%.1f)", name,
                  cal.to_ns(static_cast<double>(h.value_at_quantile(0.5))), name,
                  cal.to_ns(static_cast<double>(h.value_at_quantile(0.99))), name,
                  cal.to_ns(static_cast<double>(h.max())));
    return buf;
}

std::string book_counters_json(const book::BookCounters& c, std::size_t live) {
    return R"({"adds":)" + std::to_string(c.adds) + R"(,"executes":)" + std::to_string(c.executes) + R"(,"cancels":)" +
           std::to_string(c.cancels) + R"(,"deletes":)" + std::to_string(c.deletes) + R"(,"replaces":)" +
           std::to_string(c.replaces) + R"(,"unknown_ref":)" + std::to_string(c.unknown_ref) + R"(,"duplicate_ref":)" +
           std::to_string(c.duplicate_ref) + R"(,"overfill":)" + std::to_string(c.overfill) + R"(,"live_orders_end":)" +
           std::to_string(live) + "}";
}

template <bool kHops>
void run_engine(const Options& o, const FeedData& feed, const std::vector<std::uint64_t>& schedule,
                const TscCalibration& cal, Book& book, DecisionStage<Book>& stage, Sink& sink, HopHistograms& hops,
                Probe (&probes)[3], FeederStats& fstats) {
    auto in = std::make_unique<InRing>();
    std::atomic<bool> warmup_done{false};
    std::atomic<int> ready{0};
    const bool b = o.model == "b";
    const int ready_target = b ? 4 : 2;
    (void)book;

    std::thread feeder([&] {
        pin_current_thread(o.cpu_feeder);
        set_thread_name("e4-feeder");
        ready.fetch_add(1);
        while (ready.load() < ready_target) {
        }
        run_feeder(feed, schedule, *in, warmup_done, cal.ticks_per_ns, fstats);
    });

    if (!b) {
        pin_current_thread(o.cpu_a);
        set_thread_name("e4-model-a");
        probes[0].open();
        ready.fetch_add(1);
        while (ready.load() < ready_target) {
        }
        run_model_a<kHops>(*in, stage, sink, feed.total(), feed.warmup, warmup_done, hops, probes[0]);
    } else {
        PipelineRings<EventRing, DecisionRing> rings;
        static const char* const kNames[3] = {"e4-b-decode", "e4-b-decide", "e4-b-sink"};
        const auto pin = [&](int stage_idx) {
            pin_current_thread(o.cpus_b[stage_idx]);
            set_thread_name(kNames[stage_idx]);
        };
        run_model_b<kHops>(*in, rings, stage, sink, feed.total(), feed.warmup, warmup_done, hops, probes, pin, ready,
                           ready_target);
    }
    feeder.join();
}

}  // namespace

int main(int argc, char** argv) try {
    const Cli cli(argc, argv);
    Options o;
    o.model = cli.str("model");
    if (o.model != "a" && o.model != "b") throw std::invalid_argument("--model a|b");
    o.mode = cli.str("mode", "latency");
    if (o.mode != "latency" && o.mode != "throughput") throw std::invalid_argument("--mode latency|throughput");
    o.arrival = cli.str("arrival", "smooth");
    if (o.arrival != "smooth" && o.arrival != "bursty") throw std::invalid_argument("--arrival smooth|bursty");
    o.input = cli.str("input");
    o.out = cli.str("out");
    o.rate = cli.f64("rate", 1e6);
    o.work_ns = cli.f64("work-ns", 0);
    o.start_ns = parse_hms(cli.str("start-time", "10:00:00"));
    o.window = static_cast<std::size_t>(cli.i64("window", 2'000'000));
    o.hops = cli.i64("hops", 0) != 0;
    o.cpu_feeder = static_cast<int>(cli.i64("cpu-feeder", 0));
    o.cpu_a = static_cast<int>(cli.i64("cpu-a", 2));
    if (cli.has("cpus-b")) {
        const auto v = cli.list("cpus-b");
        if (v.size() != 3) throw std::invalid_argument("--cpus-b needs three CPUs (decode,decide,sink)");
        for (int i = 0; i < 3; ++i) o.cpus_b[i] = std::stoi(v[static_cast<std::size_t>(i)]);
    }
    const bool throughput = o.mode == "throughput";
    o.counters = cli.str("counters", throughput ? "cycles,instructions,cache-misses,L1-dcache-load-misses" : "none");

    require_invariant_tsc();
    const TscCalibration cal = calibrate_tsc(cli.f64("calib-s", 1.0));

    // 1. Load (startup, not timed).
    FeedData feed;
    {
        itch::ItchFileReader reader(itch::open_source(o.input));
        // A plain (uncompressed) file's size bounds the feed's size: reserve once, no regrowth.
        const bool plain = !o.input.ends_with(".gz");
        feed = load_feed(reader, o.start_ns, o.window, plain ? std::filesystem::file_size(o.input) : 0);
    }
    const std::vector<std::uint64_t> schedule =
        throughput ? std::vector<std::uint64_t>{}
                   : make_schedule(feed, o.arrival == "bursty" ? Arrival::kBursty : Arrival::kSmooth, o.rate,
                                   cal.ticks_per_ns);
    const double window_feed_s = static_cast<double>(feed.feed_ns[feed.total() - 1] - feed.feed_ns[feed.warmup]) / 1e9;

    // 2. Engine state, allocated before any thread starts (no allocation on the hot path).
    auto book = std::make_unique<Book>();
    StrategyConfig scfg;
    scfg.work_ticks = static_cast<std::uint64_t>(o.work_ns * cal.ticks_per_ns);
    DecisionStage<Book> stage(*book, scfg);
    auto hist = std::make_unique<Histogram>();
    Sink sink(*hist, throughput);
    HopHistograms hops;
    auto fstats = std::make_unique<FeederStats>();
    Probe probes[3];
    if (o.counters != "none")
        for (Probe& p : probes) p.events = parse_perf_events(o.counters);

    // 3. Run.
    if (o.hops)
        run_engine<true>(o, feed, schedule, cal, *book, stage, sink, hops, probes, *fstats);
    else
        run_engine<false>(o, feed, schedule, cal, *book, stage, sink, hops, probes, *fstats);

    if (sink.measured() != feed.window) throw std::runtime_error("sink measured a different number of messages");

    // 4. Results.
    const bool b = o.model == "b";
    std::string extra = "{" + pct_json("feeder_lag", fstats->lag, cal) + R"(,"ring_full_spins":)" +
                        std::to_string(fstats->ring_full_spins) + R"(,"orders":)" + std::to_string(sink.orders());
    if (throughput) {
        const double span_s = cal.to_ns(static_cast<double>(sink.span_ticks())) / 1e9;
        char buf[64];
        std::snprintf(buf, sizeof buf, R"(,"msgs_per_s":%.1f)", static_cast<double>(feed.window - 1) / span_s);
        extra += buf;
    }
    if (o.hops) {
        extra += "," + pct_json("in_wait", *hops.in_wait, cal) + "," + pct_json("svc_decode", *hops.svc_decode, cal) +
                 "," + pct_json("svc_decide", *hops.svc_decide, cal) + "," + pct_json("svc_sink", *hops.svc_sink, cal);
        if (b) extra += "," + pct_json("hop12", *hops.hop12, cal) + "," + pct_json("hop23", *hops.hop23, cal);
    }
    extra += "}";

    char digest[32];
    std::snprintf(digest, sizeof digest, "%016llx", static_cast<unsigned long long>(sink.digest()));
    char rate_buf[48];
    std::snprintf(rate_buf, sizeof rate_buf, "%.1f", o.rate);
    const std::string cpus = b ? "[" + std::to_string(o.cpus_b[0]) + "," + std::to_string(o.cpus_b[1]) + "," +
                                     std::to_string(o.cpus_b[2]) + "]"
                               : "[" + std::to_string(o.cpu_a) + "]";
    const std::string counters =
        b ? summed_counters_json(probes, 3) : (probes[0].reading ? probes[0].reading->to_json() : "null");
    std::string stage_counters = "null";
    if (b && probes[0].reading && probes[1].reading && probes[2].reading)
        stage_counters = "[" + probes[0].reading->to_json() + "," + probes[1].reading->to_json() + "," +
                         probes[2].reading->to_json() + "]";
    const std::string params =
        R"({"experiment":"E4","model":")" + o.model + R"(","mode":")" + o.mode + R"(","arrival":")" +
        (throughput ? std::string("closed-loop") : o.arrival) + R"(","rate_msgs_per_s":)" +
        (throughput ? std::string("null") : std::string(rate_buf)) + R"(,"work_ns":)" + std::to_string(o.work_ns) +
        R"(,"hops":)" + (o.hops ? "true" : "false") + R"(,"input":")" + json_escape(o.input) + R"(","start_time_ns":)" +
        std::to_string(o.start_ns) + R"(,"warmup_msgs":)" + std::to_string(feed.warmup) + R"(,"measured_msgs":)" +
        std::to_string(sink.measured()) + R"(,"window_feed_seconds":)" + std::to_string(window_feed_s) +
        R"(,"cpu_feeder":)" + std::to_string(o.cpu_feeder) + R"(,"engine_cpus":)" + cpus + R"(,"ring_slots":)" +
        std::to_string(kRingSlots) + R"(,"digest":")" + digest + R"(","negative_samples":)" +
        std::to_string(sink.negative()) + R"(,"value_divisor":)" + std::to_string(throughput ? Sink::kBlock : 1) +
        R"(,"unit":"ticks","book_counters":)" + book_counters_json(book->counters(), book->live_orders()) +
        R"(,"extra_metrics":)" + extra + R"(,"counters":)" + counters + R"(,"stage_counters":)" + stage_counters + "}";
    write_run(o.out, *hist, cal, params);

    const double div = throughput ? static_cast<double>(Sink::kBlock) : 1.0;
    std::printf("model %s %s %s rate %.0f work %.0f ns: p50 %.1f ns%s p99 %.1f ns, orders %llu, digest %s\n",
                o.model.c_str(), o.mode.c_str(), throughput ? "closed-loop" : o.arrival.c_str(), o.rate, o.work_ns,
                cal.to_ns(static_cast<double>(hist->value_at_quantile(0.5)) / div), throughput ? "/msg" : "",
                cal.to_ns(static_cast<double>(hist->value_at_quantile(0.99)) / div),
                static_cast<unsigned long long>(sink.orders()), digest);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "e4_threading: %s\n", e.what());
    return 1;
}
