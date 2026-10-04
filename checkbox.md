# Project Progress — Low-Latency Market Data & Execution Engine

> Mirrors plan.md phases/tasks. Mark `[x]` only when fully implemented and working.

## Phase 0: Toolchain and Measurement Infrastructure → E0 (weeks 1–1.5)
- [x] Install toolchain: clang, lld, llvm (llvm-mca, llvm-profdata, llvm-bolt), linux-perf, cmake, ninja, gdb, valgrind, pigz, python3 + numpy/pandas/matplotlib, pmu-tools (toplev), rtla, tcpdump, wireshark
- [x] Set `kernel.perf_event_paranoid=1` for development and document it in `docs/linux-tuning.md`
- [x] Initialize git repo with `.gitignore` (build dirs, `data/` downloads, results), README stub and LICENSE
- [x] Create the CMake project skeleton with the directory layout from master plan §6
- [x] Add CMakePresets: debug, release (`-O2 -DNDEBUG`), relwithdebinfo (`-O2 -g -fno-omit-frame-pointer`), asan, ubsan, tsan
- [x] Enable warnings `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wold-style-cast`, with `-Werror` in CI
- [x] Add `.clang-format` and `.clang-tidy`
- [x] Integrate GoogleTest and Google Benchmark via CMake FetchContent
- [x] Add a GitHub Actions CI skeleton: {gcc, clang} × {debug, asan, ubsan, tsan}, build + ctest
- [x] Implement the TSC clock: `lfence;rdtsc` start stamp, `rdtscp;lfence` end stamp, behind an arch-abstract interface
- [x] Implement TSC calibration against `CLOCK_MONOTONIC_RAW` (~1 s) and a startup check for `constant_tsc`/`nonstop_tsc`
- [x] Implement the log-linear histogram (fixed memory, no allocation on record) with unit tests; validate against HdrHistogram_c
- [x] Implement a CPU pinning helper (`pthread_setaffinity_np`)
- [x] Implement region-scoped hardware counters (`perf_event_open` group around the timed region only; cycles/op, instructions/op in every result)
- [x] Write `scripts/env_capture.sh` (CPU, microcode, kernel, cmdline, governor, turbo, isolated CPUs, compiler, flags, git SHA → JSON)
- [ ] Write `scripts/tune_machine.sh` apply/restore (performance governor, `no_turbo=1`, `nmi_watchdog=0` so top-down analysis gets all PMU counters)
- [x] Write `scripts/run_bench.py`: N interleaved runs, warm-up, pinning, CSV output + env JSON
- [x] Write `scripts/analyse.py`: percentiles, median across runs, 95% bootstrap CI, percentile plots
- [x] E0: benchmark per-call cost of `rdtsc`, `lfence;rdtsc`, `rdtscp`, `steady_clock::now()`, `clock_gettime(MONOTONIC / MONOTONIC_RAW)`
- [x] E0: measure cross-core TSC skew for core pairs via ping-pong (NTP-style offset, bounded by RTT/2)
- [x] E0: measure histogram record cost
- [x] Write `docs/experiments/E0-measurement.md` and `docs/methodology.md`
- [ ] Tag v0.1

## Phase 1: ITCH Decoder, Reference Book, Differential Testing → E1 (weeks 2–3)
- [x] Verify the current ITCH 5.0 sample-data location and terms of use; document them in `data/README.md`
- [x] Write `scripts/fetch_itch.sh` with `data/checksums.sha256` verification (Nasdaq data is never committed)
- [x] Implement a streaming ITCH file reader (pigz/zlib) handling the 2-byte big-endian length framing
- [x] Write `tools/slice_itch` (C++; a Python per-message loop is too slow for a ~300M-message day) for local dev slices (first N messages; named symbols; top-K symbols)
- [x] Implement C++20 `bswap`/`load_be` helpers and the 48-bit timestamp loader, with unit tests
- [x] Implement the ITCH decoder for A, F, E, C, X, D, U, R, S, H with bounds checks, skipping other types by length
- [x] Build the stock-locate → symbol directory from `R` messages; books indexed by locate
- [x] Define a common book interface shared by the reference and optimized books
- [x] Implement the reference book: `std::map` levels, `std::list` orders, `std::unordered_map` ref → handle
- [x] Unit tests per message type: partial executions, replace priority loss, unknown refs, zero-qty results
- [x] Implement `tools/gen_fixture`: synthetic ITCH-format generator (fixed seed, adversarial modes) for CI
- [x] Implement the differential harness: compare top-of-book every message and full depth every N; dump a reproduction on mismatch
- [x] Add property-based random and adversarial stream tests
- [x] Add a libFuzzer target for the ITCH decoder (ASan/UBSan)
- [x] Add logged invariants: crossed/locked books, negative quantities
- [x] Process a full real ITCH day through the reference book within the RAM budget
- [x] E1 analysis tool: message mix, add/cancel/execute ratios, order lifetimes, depth and level occupancy, price distance from best, symbol skew, burstiness (1 ms / 10 ms / 1 s peaks vs mean), order-ref density
- [x] Write `docs/experiments/E1-workload.md` and `docs/itch-moldudp64.md`
- [ ] Tag v0.2

## Phase 2: Optimized Order Book, Memory, Profiling → E2 (weeks 4–6)
- [x] Implement a fixed-size object pool with 32-bit indices
- [x] Define the `OrderNode` (32 B) and `Level` structs with `static_assert` sizes
- [ ] L1: map levels + pooled intrusive doubly-linked order lists
- [x] L2a (deadline build): per-symbol/side sorted level vector, best at the back, linear-then-binary search; compared against the reference book first
- [ ] L2: per-symbol/side tick ladder (±W ticks; E1 coverage gives the candidates, E2 sweeps W ∈ {64, 256, 1024}), sparse map fallback, recentring with a counter
- [ ] L2: two-level bitmap best-price search using `std::countr_zero`/`std::countl_zero`
- [x] L3: open-addressing order-ref map (linear probing, backward-shift delete); E1 ruled out a direct vector (refs span 260 M, peak live 1.96 M)
- [ ] L4: `mlockall` + prefault (`MAP_POPULATE`/touch)
- [ ] L5: 2 MiB pages (`MAP_HUGETLB` or THP `madvise`) for pools and the ID map
- [ ] Make layers selectable so every layer and every leave-one-out build can be benchmarked
- [ ] Add an allocation-counting shim test asserting 0 heap allocations after warm-up
- [ ] Add a page-fault check asserting 0 steady-state page faults
- [ ] Every layer passes the differential test on a full real day
- [x] Build the book benchmark harness: replay a slice, per-message TSC latency into the histogram
- [ ] Write profiling scripts: `perf stat` counter set, `perf record` + FlameGraph
- [ ] Top-down analysis (toplev / `perf stat -M TopdownL1/L2`) per layer
- [ ] Use Intel PT / magic-trace to explain at least one p99.9 event
- [ ] Compare llvm-mca's prediction for the hottest loop with measured throughput
- [ ] E2 session B (`fast`, identity hash, N = 10 full days): after the resume deadline
- [ ] E2 runs: L0–L5 plus leave-one-out ablation on the full day, top-symbol slice and adversarial synthetic stream
- [ ] Write `docs/experiments/E2-orderbook.md` (mechanism per layer) and `docs/orderbook.md`
- [ ] Tag v0.3

## Phase 3: Queues and Concurrency → E3 (week 7)
- [x] Implement SPSC variants: mutex+condvar, mutex+spin, `seq_cst` atomics, acquire/release, +cached remote index
- [ ] SPSC batch publish variant
- [x] Make index padding configurable: none / 64 B / 128 B
- [x] Write the SPSC happens-before correctness argument in `docs/concurrency.md`
- [x] Unit tests: wraparound, full, empty, producer/consumer ordering
- [x] TSan stress tests with randomized delays (~10⁸ operations)
- [ ] Implement the seqlock (relaxed-atomic fields + fences) with tests
- [ ] Timeboxed (≤2 days) GenMC/Relacy model check: accepts acquire/release, rejects weakened ordering; or document why it was dropped
- [x] Queue benchmark: throughput, one-way latency percentiles, CPU%, context switches
- [x] E3 sweeps: variants → padding → placement (unpinned / SMT siblings 2-6 / separate cores 2-3)
- [ ] Confirm the padding results with `perf c2c` HITM counts
- [ ] Write `docs/experiments/E3-queues.md` (part 1 before the 2026-10-04 deadline: variants, padding, placement; batch publish, seqlock, model check, `perf c2c` after)
- [ ] Tag v0.4

## Phase 4: Hero Threading-Model Study → E4 (week 8)
- [ ] Implement `apps/feeder`: open-loop replay into a raw-message SPSC ring with intended send TSC; speed-factor and fixed-rate modes
- [ ] Implement a minimal decision stage + output sink so both models do identical work before P6 exists
- [ ] Implement Model A: run-to-completion on one pinned thread
- [ ] Implement Model B: 3-stage pipeline over SPSC rings
- [ ] Add the synthetic per-message work knob (spin k ns)
- [ ] Sweep driver: offered load 10% → past saturation × work {0, 200, 500, 1000 ns} × bursty vs smooth arrival
- [ ] Measure per-hop costs in Model B (TSC stamps + `perf c2c`)
- [ ] Produce latency-vs-offered-load plots (the hero plot)
- [ ] Write `docs/experiments/E4-threading-model.md`, including where the hypothesis was wrong
- [ ] Checkpoint "core": README draft with the hero plot, core resume bullets filled with measured numbers
- [ ] Tag v0.5

## Phase 5: Network Feed Handler → E5 (weeks 9–10)
- [ ] Write `scripts/netns_setup.sh`: `exch`/`engine` namespaces, veth pair, multicast routes
- [ ] Implement the MoldUDP64 encoder/decoder with unit tests and a libFuzzer target
- [ ] Publisher: pack messages to MTU, send on A/B multicast legs, open-loop pacing shared with the feeder, `(seq, intended_tsc)` side log
- [ ] Implement the publisher's re-request (retransmit) server
- [ ] Receiver: non-blocking sockets, `IP_ADD_MEMBERSHIP`, tuned `SO_RCVBUF`
- [ ] Receive strategies: blocking `recvfrom`, `epoll`, busy-spin, `recvmmsg`, `SO_BUSY_POLL`
- [ ] Capture `SO_TIMESTAMPING` software RX timestamps, with `CLOCK_REALTIME` → TSC conversion
- [ ] Drop accounting from `/proc/net/snmp` before and after each run
- [ ] A/B arbitration with per-leg and merged sequence tracking and duplicate drop
- [ ] Gap timeout → re-request client, with stale-book marking while a gap is open
- [ ] Integration tests with `tc netem` loss/reorder scenarios
- [ ] pcap recorder and replay tool (1× / 10× / 100× / max)
- [ ] E5 part 1: receive strategies × offered rates (latency, kernel → user delay, CPU%, drops)
- [ ] E5 part 2: resilience under loss on A / B / both / correlated bursts (gaps hidden, recovery latency, stale time)
- [ ] E5 part 3: re-validate the E4 comparison end-to-end over the network
- [ ] Write `docs/experiments/E5-feed-handler.md`
- [ ] Tag v0.6

## Phase 6: Minimal Trading Layer and Exchange Emulator (week 11)
- [ ] Strategy: deterministic imbalance/spread trigger with static dispatch
- [ ] Risk: max qty, max position, max notional, price band, order-rate token bucket, duplicate client-order-ID check, stale-book guard
- [ ] Kill switch (atomic flag; set by command, loss limit or prolonged staleness); measure risk-check cost
- [ ] OUCH-style Enter/Cancel/Replace messages with SoupBinTCP-style framing
- [ ] Order gateway over TCP (`TCP_NODELAY`, preallocated buffers)
- [ ] Exchange emulator TCP server using the reference book with matching (LIMIT, MARKET, IOC, CANCEL, REPLACE)
- [ ] Emulator matching unit tests: price-time priority, partial fills, multi-level sweeps, IOC remainder cancel
- [ ] Async binary logger (per-thread SPSC, background formatting); measure log-call cost
- [ ] Telemetry: per-thread counters/histograms → seqlock snapshot → telemetry thread → CSV/JSON
- [ ] Tick-to-trade measurement with per-stage breakdown
- [ ] Replay output-digest determinism test across runs and builds
- [ ] Full integration test: publisher → engine → emulator in netns

## Phase 7: Tick-to-Trade and Jitter Tuning → E6 (week 12)
- [ ] Implement `apps/hiccup` (pinned TSC-gap histogram)
- [ ] Set up `rtla osnoise` / `rtla timerlat` attribution
- [ ] Script and document the tuning configs: isolcpus / nohz_full / rcu_nocbs / irqaffinity cmdline, IRQ pinning + irqbalance off, C-states, THP modes, SCHED_FIFO, mlockall
- [ ] E6 runs: each knob alone, then combined (hiccup p99.99/max, tick-to-trade percentiles + stage breakdown)
- [ ] Write `docs/experiments/E6-jitter.md` and finish `docs/linux-tuning.md`
- [ ] Tag v0.7

## Phase 8: Report, README, Resume → v1.0 (week 13)
- [ ] CI final: 60 s fuzz smoke test, replay digest check, cachegrind instruction-count regression
- [ ] Write `docs/technical-report.md` (paper-style, E0–E6)
- [ ] Write `docs/limitations.md`
- [ ] Final README (hero plot, results, reproduce commands)
- [ ] Fill HFT resume bullets and pitch with measured numbers
- [ ] Write `docs/study-guide.md`: beginner reading order through the code and experiments, concepts per file, links to references (for the user's interview preparation)
- [ ] Mock interview preparation using master plan §18
- [ ] Verify the v1.0 definition of done (master plan §14)
- [ ] Tag v1.0

## Phase 9: AArch64 Port and Memory-Model Study → E7 (v1.1)
- [ ] Acquire ARM hardware (Raspberry Pi 5 / Ampere / Graviton / Snapdragon)
- [ ] AArch64 build preset (native or cross) and functional tests under qemu-user
- [ ] `CNTVCT_EL0` timer backend with resolution characterization
- [ ] Cross-ISA replay digest matches x86
- [ ] Litmus tests MP and SB on both ISAs (observed reorderings per N iterations)
- [ ] Weakened-SPSC stress test on both ISAs, plus TSan/model-checker evidence
- [ ] Ordering-cost study: generated assembly + SPSC throughput per ordering per ISA
- [ ] Write `docs/experiments/E7-memory-model.md`

## Phase 10: Compiler Study → E8 (v1.1)
- [ ] Build-ladder presets: O2, O3, `-march=native`, LTO, PGO, BOLT
- [ ] PGO training on ITCH replay; BOLT with an LBR profile
- [ ] Measure ns/msg, instructions/msg, IPC, top-down frontend-bound share, i-cache/iTLB/branch misses, binary size
- [ ] Write `docs/experiments/E8-compiler.md` (link to the LLVM project)

## Phase 11: Energy vs Latency → E9 (v1.1)
- [ ] RAPL measurement script (`power/energy-pkg/`, `power/energy-cores/`)
- [ ] Busy-poll vs blocking wait at 3–4 offered loads; joules/message vs p99 Pareto plot
- [ ] Write `docs/experiments/E9-energy.md`; add E7–E9 to the report and the silicon resume version
- [ ] Tag v1.1

## Extensions (optional, no deadline)
- [ ] AF_XDP (veth native XDP) and/or `io_uring` multishot receive path in E5
- [ ] DPDK / hardware timestamps / PTP (only with a supported lab NIC)
- [ ] Core-to-core latency matrix (only on AMD multi-CCD or ARM hardware)
- [ ] SIMD (AVX2/NEON) for bitmap scan / byte-swap
- [ ] LSE vs LL/SC atomics on ARM
- [ ] Virtual vs CRTP vs `variant` dispatch cost
- [ ] eBPF/bpftrace attribution (`runqlat`, `offcputime`) for E6
- [ ] Grafana live demo (off the hot path)
