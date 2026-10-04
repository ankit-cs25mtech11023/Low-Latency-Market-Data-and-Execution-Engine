#pragma once
// The two threading models compared in E4.
//
//   Model A, run-to-completion: ONE thread pops a raw message from the feeder's ring and
//   runs decode -> book/strategy/risk -> sink to completion before taking the next one.
//     + no cross-core hand-offs: the message and the book stay in this core's caches
//     - every message pays the whole path; the thread's capacity is 1 / (total cost)
//
//   Model B, pipelined: THREE threads, one per stage, joined by SPSC rings:
//     [decode] --Event ring--> [book + strategy + risk] --Decision ring--> [sink]
//     + each core does only part of the work, so the pipeline's capacity is
//       1 / (cost of the slowest stage), and each stage has a core's caches to itself
//     - every message crosses two more cores: each hop costs at least one cache-line
//       transfer for the record plus the ring index traffic (E3: ~250 ns one-way p50
//       between cores at low load, ~11 ns/msg when streaming)
//
// Both models call the same functions (messages.hpp, stages.hpp). The feeder -> first
// thread hop is the same ring type in both.
//
// Every thread processes exactly `total` records (each stage emits one record per input),
// so no end-of-stream marker is needed. The last stage sets `warmup_done` after the last
// warm-up message, which releases the feeder into the measured window.
//
// kHops = true adds TSC stamps between the steps (each rdtsc costs ~16 ns, E0), to break
// the latency down per stage and per hop. The main sweep runs with kHops = false.
//
// Probe: per-thread hook with begin() (called on the thread's first measured record) and
// end() (after its last record), used for hardware counters.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "lle/core/tsc.hpp"
#include "lle/engine/messages.hpp"
#include "lle/engine/stages.hpp"
#include "lle/telemetry/histogram.hpp"

namespace lle::engine {

// Per-step breakdown (kHops). Each histogram is written by exactly one thread and is a
// separate heap object, so no two threads write the same cache line.
struct HopHistograms {
    std::unique_ptr<Histogram> in_wait = std::make_unique<Histogram>();     // feeder ring: picked up - intended
    std::unique_ptr<Histogram> svc_decode = std::make_unique<Histogram>();  // decode step
    std::unique_ptr<Histogram> hop12 = std::make_unique<Histogram>();       // decode done -> decision picks up (B)
    std::unique_ptr<Histogram> svc_decide = std::make_unique<Histogram>();  // book + strategy + risk + work
    std::unique_ptr<Histogram> hop23 = std::make_unique<Histogram>();       // decision done -> sink picks up (B)
    std::unique_ptr<Histogram> svc_sink = std::make_unique<Histogram>();    // sink step
};

template <class Ring, class T>
[[gnu::always_inline]] inline void pop_spin(Ring& r, T& out) noexcept {
    while (!r.try_pop(out)) {
    }
}

template <class Ring, class T>
[[gnu::always_inline]] inline void push_spin(Ring& r, const T& v) noexcept {
    while (!r.try_push(v)) {
    }
}

// Model A. Runs on the calling thread.
template <bool kHops, class InRing, class Book, class Probe>
void run_model_a(InRing& in, DecisionStage<Book>& stage, Sink& sink, std::size_t total, std::size_t warmup,
                 std::atomic<bool>& warmup_done, HopHistograms& hops, Probe& probe) {
    if (warmup == 0) warmup_done.store(true, std::memory_order_release);
    RawSlot raw{};
    Event ev{};
    Decision d{};
    bool started = false;
    for (std::size_t n = 0; n < total; ++n) {
        pop_spin(in, raw);
        const std::uint64_t t_pop = kHops ? Tsc::now() : 0;
        if (!started && raw.intended != 0) {
            probe.begin();
            started = true;
        }
        decode_event(raw, ev);
        if constexpr (kHops) ev.t_done = Tsc::now();
        stage.process(ev, d);
        if constexpr (kHops) d.t_done = Tsc::now();
        const std::uint64_t end = sink.consume(d);
        if constexpr (kHops) {
            if (raw.intended != 0) {
                hops.in_wait->record(t_pop >= raw.intended ? t_pop - raw.intended : 0);
                hops.svc_decode->record(ev.t_done - t_pop);
                hops.svc_decide->record(d.t_done - ev.t_done);
                hops.svc_sink->record(end - d.t_done);
            }
        }
        if (n + 1 == warmup) warmup_done.store(true, std::memory_order_release);
    }
    probe.end();
}

// The two internal rings of Model B, allocated together (heap: each is ~64 KiB).
template <class EventRing, class DecisionRing>
struct PipelineRings {
    std::unique_ptr<EventRing> events = std::make_unique<EventRing>();
    std::unique_ptr<DecisionRing> decisions = std::make_unique<DecisionRing>();
};

// Model B. Starts the decode and decision threads, runs the sink on the calling thread,
// and joins. pin(stage) is called first thing on each stage's thread (stage 0, 1, 2).
template <bool kHops, class InRing, class EventRing, class DecisionRing, class Book, class Probe, class Pin>
void run_model_b(InRing& in, PipelineRings<EventRing, DecisionRing>& rings, DecisionStage<Book>& stage, Sink& sink,
                 std::size_t total, std::size_t warmup, std::atomic<bool>& warmup_done, HopHistograms& hops,
                 Probe (&probes)[3], Pin&& pin, std::atomic<int>& ready, int ready_target) {
    if (warmup == 0) warmup_done.store(true, std::memory_order_release);
    EventRing& r12 = *rings.events;
    DecisionRing& r23 = *rings.decisions;

    std::thread decode_thread([&] {
        pin(0);
        probes[0].open();
        ready.fetch_add(1);
        while (ready.load() < ready_target) {
        }
        RawSlot raw{};
        Event ev{};
        bool started = false;
        for (std::size_t n = 0; n < total; ++n) {
            pop_spin(in, raw);
            const std::uint64_t t_pop = kHops ? Tsc::now() : 0;
            if (!started && raw.intended != 0) {
                probes[0].begin();
                started = true;
            }
            decode_event(raw, ev);
            if constexpr (kHops) {
                ev.t_done = Tsc::now();
                if (raw.intended != 0) {
                    hops.in_wait->record(t_pop >= raw.intended ? t_pop - raw.intended : 0);
                    hops.svc_decode->record(ev.t_done - t_pop);
                }
            }
            push_spin(r12, ev);
        }
        probes[0].end();
    });

    std::thread decision_thread([&] {
        pin(1);
        probes[1].open();
        ready.fetch_add(1);
        while (ready.load() < ready_target) {
        }
        Event ev{};
        Decision d{};
        bool started = false;
        for (std::size_t n = 0; n < total; ++n) {
            pop_spin(r12, ev);
            const std::uint64_t t_pop = kHops ? Tsc::now() : 0;
            if (!started && ev.intended != 0) {
                probes[1].begin();
                started = true;
            }
            stage.process(ev, d);
            if constexpr (kHops) {
                d.t_done = Tsc::now();
                if (ev.intended != 0) {
                    hops.hop12->record(t_pop >= ev.t_done ? t_pop - ev.t_done : 0);
                    hops.svc_decide->record(d.t_done - t_pop);
                }
            }
            push_spin(r23, d);
        }
        probes[1].end();
    });

    // Sink on this thread.
    pin(2);
    probes[2].open();
    ready.fetch_add(1);
    while (ready.load() < ready_target) {
    }
    Decision d{};
    bool started = false;
    for (std::size_t n = 0; n < total; ++n) {
        pop_spin(r23, d);
        const std::uint64_t t_pop = kHops ? Tsc::now() : 0;
        if (!started && d.intended != 0) {
            probes[2].begin();
            started = true;
        }
        const std::uint64_t end = sink.consume(d);
        if constexpr (kHops) {
            if (d.intended != 0) {
                hops.hop23->record(t_pop >= d.t_done ? t_pop - d.t_done : 0);
                hops.svc_sink->record(end - t_pop);
            }
        }
        if (n + 1 == warmup) warmup_done.store(true, std::memory_order_release);
    }
    probes[2].end();
    decode_thread.join();
    decision_thread.join();
}

}  // namespace lle::engine
