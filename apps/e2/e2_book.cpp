// E2 driver: how fast does each order-book implementation process a real ITCH day?
//
//   e2_book --book ref|fast|fast-fib --input FILE --cpu 2 --out PREFIX
//           [--mode batch|permsg|both] [--out-permsg PREFIX2] [--batch 1024]
//           [--warmup-msgs 1000000] [--max-msgs N] [--counters LIST]
//
// What is timed: decode + book update (the BookBuilder call) of each message. NOT timed:
// reading/decompressing the file. The file is streamed (pigz runs in a separate process on
// another core) into a staging buffer of --batch messages; only the processing of the staged
// batch is inside the timed region. A staged batch of ~30 KB is what a feed handler would hold
// after receiving a burst of packets, so it is a realistic "messages already in cache" input.
//
// Two timing methods:
//   batch   one TSC interval per full batch of --batch messages -> throughput (ns/msg, after
//           dividing by the batch size). The measurement floor (~21 ns, E0) is amortized over
//           the batch, and the CPU can overlap consecutive messages as it would in production.
//   permsg  one TSC interval per message -> the per-message latency distribution (p50, p99,
//           p99.9). Each interval includes the E0 measurement floor (21.1 ns at p50), and the
//           lfence/rdtscp pair stops the CPU from overlapping consecutive messages, so permsg
//           p50 is higher than batch ns/msg. Both are reported; neither is the "true" one.
//   both    alternate full batches between the two methods in ONE pass over the day (even
//           batches: batch timing, odd batches: per-message timing). A full-day pass takes
//           minutes, so this halves the cost of N >= 10 runs per book; both samples cover the
//           whole day evenly. Results go to --out (batch) and --out-permsg (per message).
//
// The first --warmup-msgs messages are processed but not measured (directory messages, first
// allocations of the book). Hardware counters (one perf_event_open group per method) and the
// heap-allocation count (operator new shim, alloc_count.cpp) cover that method's measured
// batches only.

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "alloc_count.hpp"
#include "lle/book/book_builder.hpp"
#include "lle/book/fast_book.hpp"
#include "lle/book/reference_book.hpp"
#include "lle/core/cli.hpp"
#include "lle/core/cpu.hpp"
#include "lle/core/tsc.hpp"
#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/telemetry/histogram.hpp"
#include "lle/telemetry/perf_counters.hpp"
#include "lle/telemetry/result_io.hpp"

namespace {

using namespace lle;

enum Method : std::size_t { kBatch = 0, kPerMsg = 1 };

struct Options {
    std::string input, mode;
    std::array<std::string, 2> out;  // output prefix per method ("" = method not used)
    int cpu = 2;
    std::size_t batch = 1024;
    std::uint64_t warmup = 1'000'000;
    std::uint64_t max_msgs = UINT64_MAX;
};

// Everything recorded for one timing method.
struct Sink {
    Histogram hist;
    std::unique_ptr<PerfCounters> pmu;
    std::uint64_t measured_msgs = 0;
    std::uint64_t measured_batches = 0;
    std::uint64_t allocations = 0;  // heap allocations inside measured regions
};

template <class Book>
std::uint64_t run(Book& book, const Options& o, std::array<std::unique_ptr<Sink>, 2>& sinks) {
    auto dir = std::make_unique<book::SymbolDirectory>();
    book::BookBuilder<Book> bb(book, *dir);

    // Staging buffer: messages are copied here back to back; offsets give their positions.
    constexpr std::size_t kMaxMsg = 64;  // longest ITCH 5.0 message is 50 bytes
    std::vector<std::byte> stage(o.batch * kMaxMsg);
    std::vector<std::uint32_t> off(o.batch + 1);

    itch::ItchFileReader reader(itch::open_source(o.input));
    const std::byte* p = nullptr;
    std::size_t len = 0;
    std::uint64_t total = 0;
    std::uint64_t full_batches = 0;

    for (;;) {
        // Fill one batch (untimed).
        std::size_t n = 0;
        std::uint32_t pos = 0;
        while (n < o.batch && total + n < o.max_msgs && reader.next(p, len)) {
            if (len > kMaxMsg) throw std::runtime_error("message longer than 64 bytes");
            std::memcpy(stage.data() + pos, p, len);
            off[n++] = pos;
            pos += static_cast<std::uint32_t>(len);
        }
        off[n] = pos;
        if (n == 0) break;
        const auto msg = [&](std::size_t i) { return stage.data() + off[i]; };
        const auto mlen = [&](std::size_t i) { return static_cast<std::size_t>(off[i + 1] - off[i]); };

        const bool measure = total >= o.warmup && n == o.batch;
        Method m = o.mode == "permsg" ? kPerMsg : kBatch;
        if (measure && o.mode == "both") m = (full_batches++ % 2 == 0) ? kBatch : kPerMsg;
        if (!measure) {
            for (std::size_t i = 0; i < n; ++i) (void)itch::decode(msg(i), mlen(i), bb);
        } else {
            Sink& s = *sinks[m];
            const std::uint64_t a0 = e2::g_allocations;
            if (s.pmu) s.pmu->resume();
            if (m == kPerMsg) {
                for (std::size_t i = 0; i < n; ++i) {
                    const std::uint64_t t0 = Tsc::start();
                    (void)itch::decode(msg(i), mlen(i), bb);
                    const std::uint64_t t1 = Tsc::stop();
                    s.hist.record(t1 - t0);
                }
            } else {
                const std::uint64_t t0 = Tsc::start();
                for (std::size_t i = 0; i < n; ++i) (void)itch::decode(msg(i), mlen(i), bb);
                const std::uint64_t t1 = Tsc::stop();
                s.hist.record(t1 - t0);
            }
            if (s.pmu) s.pmu->pause();
            s.allocations += e2::g_allocations - a0;
            s.measured_msgs += n;
            ++s.measured_batches;
        }
        total += n;
    }
    return total;
}

std::string counters_json(const book::BookCounters& c, std::size_t live) {
    return R"({"adds":)" + std::to_string(c.adds) + R"(,"executes":)" + std::to_string(c.executes) + R"(,"cancels":)" +
           std::to_string(c.cancels) + R"(,"deletes":)" + std::to_string(c.deletes) + R"(,"replaces":)" +
           std::to_string(c.replaces) + R"(,"unknown_ref":)" + std::to_string(c.unknown_ref) + R"(,"duplicate_ref":)" +
           std::to_string(c.duplicate_ref) + R"(,"overfill":)" + std::to_string(c.overfill) + R"(,"live_orders_end":)" +
           std::to_string(live) + "}";
}

}  // namespace

int main(int argc, char** argv) try {
    const Cli cli(argc, argv);
    Options o;
    const std::string book_name = cli.str("book");
    o.input = cli.str("input");
    o.mode = cli.str("mode", "batch");
    o.cpu = static_cast<int>(cli.i64("cpu", 2));
    o.batch = static_cast<std::size_t>(cli.i64("batch", 1024));
    o.warmup = static_cast<std::uint64_t>(cli.i64("warmup-msgs", 1'000'000));
    if (cli.has("max-msgs")) o.max_msgs = static_cast<std::uint64_t>(cli.i64("max-msgs", 0));
    const std::string counters_arg =
        cli.str("counters", "cycles,instructions,ref-cycles,branch-misses,cache-misses,dTLB-load-misses,page-faults");
    if (o.mode == "batch") {
        o.out[kBatch] = cli.str("out");
    } else if (o.mode == "permsg") {
        o.out[kPerMsg] = cli.str("out");
    } else if (o.mode == "both") {
        o.out[kBatch] = cli.str("out");
        o.out[kPerMsg] = cli.str("out-permsg");
    } else {
        throw std::invalid_argument("--mode must be batch, permsg or both");
    }
    if (o.batch == 0) throw std::invalid_argument("--batch must be > 0");

    require_invariant_tsc();
    pin_current_thread(o.cpu);
    const TscCalibration cal = calibrate_tsc(cli.f64("calib-s", 1.0));

    std::array<std::unique_ptr<Sink>, 2> sinks;
    for (auto& s : sinks) {
        s = std::make_unique<Sink>();
        if (counters_arg != "none") {
            s->pmu = std::make_unique<PerfCounters>(parse_perf_events(counters_arg));
            s->pmu->start();
            s->pmu->pause();  // counts accumulate only inside measured batches (resume/pause)
        }
    }

    std::uint64_t total = 0;
    std::string book_json;
    if (book_name == "ref") {
        auto b = std::make_unique<book::ReferenceBook>();
        total = run(*b, o, sinks);
        book_json = counters_json(b->counters(), b->live_orders());
    } else if (book_name == "fast") {
        auto b = std::make_unique<book::FastBook>();
        total = run(*b, o, sinks);
        book_json = counters_json(b->counters(), b->live_orders());
    } else if (book_name == "fast-fib") {
        auto b = std::make_unique<book::FastBookFib>();
        total = run(*b, o, sinks);
        book_json = counters_json(b->counters(), b->live_orders());
    } else {
        throw std::invalid_argument("--book must be ref, fast or fast-fib");
    }

    for (const Method m : {kBatch, kPerMsg}) {
        if (o.out[m].empty()) continue;
        Sink& s = *sinks[m];
        const std::string pmu_json = s.pmu ? s.pmu->stop().to_json() : "null";
        const std::size_t divisor = m == kBatch ? o.batch : 1;
        const std::string params =
            R"({"experiment":"E2","book":")" + book_name + R"(","method":")" + (m == kBatch ? "batch" : "permsg") +
            R"(","run_mode":")" + o.mode + R"(","input":")" + json_escape(o.input) + R"(","cpu":)" +
            std::to_string(o.cpu) + R"(,"ran_on_cpu":)" + std::to_string(current_cpu()) + R"(,"batch":)" +
            std::to_string(o.batch) + R"(,"value_divisor":)" + std::to_string(divisor) +
            R"(,"unit":"ticks","warmup_msgs":)" + std::to_string(o.warmup) + R"(,"total_msgs":)" +
            std::to_string(total) + R"(,"measured_msgs":)" + std::to_string(s.measured_msgs) +
            R"(,"measured_batches":)" + std::to_string(s.measured_batches) + R"(,"heap_allocations_measured":)" +
            std::to_string(s.allocations) + R"(,"book_counters":)" + book_json + R"(,"counters":)" + pmu_json + "}";
        write_run(o.out[m], s.hist, cal, params);
        std::printf("%s/%s: %llu msgs measured, p50 %.1f ns/msg, heap allocations %llu\n", book_name.c_str(),
                    m == kBatch ? "batch" : "permsg", static_cast<unsigned long long>(s.measured_msgs),
                    cal.to_ns(static_cast<double>(s.hist.value_at_quantile(0.5)) / static_cast<double>(divisor)),
                    static_cast<unsigned long long>(s.allocations));
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "e2_book: %s\n", e.what());
    return 1;
}
