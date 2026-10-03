# Low-Latency C++ Systems Engine
## Master Plan v3: HFT-First, Depth over Breadth

**Project:** Low-Latency Market Data & Execution Engine
**Language:** C++20 (no C++23 dependencies; see the `bswap` helper in P1)
**Platform:** Linux, x86-64 primary, AArch64 in v1.1
**Priority:** HFT / trading infrastructure first (v1.0); CPU/SoC systems roles second (v1.1)
**Revision:** v3 (2026-10-04). Supersedes v2; v1 and v2 are kept for reference.

---

## 0. Guiding Principle and What Changed from v2

> **What is the smallest set of experiments that tells the strongest systems story?**
> Each experiment must add new insight, not just a new technology name.

An interviewer gets far more from "we expected B to win because of X; it won only below load Y; counters show why; here is where our hypothesis was wrong" than from a list of 15 technologies.

| Change | v2 | v3 |
|---|---|---|
| What "done" means | All 3 tiers and 21 experiments | **v1.0 = Core + HFT specialization** (7 experiments). Silicon specialization is v1.1. Everything else is an extension |
| Experiments | 21, overlapping | **10**, merged: Core E0–E4, HFT E5–E6, Silicon E7–E9 |
| Hero result | One of many | **E4: run-to-completion vs pipelined, latency vs offered load.** Done in-process, so it doesn't wait for networking |
| ITCH data in git | ~1 MB ITCH fixture committed | **No Nasdaq data in the repo.** CI fixture is synthetic; real data is fetched by a script and verified by checksum |
| C++ version | "C++20 + some C++23" | Strictly C++20; `bswap` helper falls back to builtins |
| Timer framing | "`steady_clock` too coarse" | Its resolution is 1 ns; the issue is **per-call cost**. E0 characterizes cost, serialization, invariance and **cross-core TSC skew** |
| Broken-SPSC-on-ARM | Stated as an expected outcome | Stated as a **hypothesis**; observed failure counts are reported, including 0 |
| Model checking | Full phase item | **Timeboxed to 2 days** inside P3; drop if tooling blocks |
| Core-to-core matrix | Core experiment | **Extension.** A single-ring 4-core laptop gives ~2 distinct values; only worth it on AMD multi-CCD or ARM |
| Dispatch cost, SIMD, LSE atomics | Experiments | **Extensions** (known results or likely low insight) |
| Intel PT, BOLT, energy | (reviewer suggested demoting) | **Kept.** High insight per hour: PT explains tail events; BOLT follows PGO; RAPL is one `perf` flag |
| Timeline | 12 weeks (optimistic) | **~13 weeks to v1.0**, ~4 more weeks to v1.1, at 15–20 h/week |

---

## 1. Goal and Narrative

Build a Linux C++20 engine that consumes **real NASDAQ TotalView-ITCH 5.0** market data over UDP multicast (MoldUDP64, A/B legs, gap recovery). It maintains per-symbol limit order books, runs a minimal strategy and pre-trade risk layer, and sends orders to a local exchange emulator.

The engine is a **testbed for a small number of deep, reproducible experiments** on data layout, memory, concurrency, threading architecture, the kernel receive path and OS jitter.

Three layers:

1. **Engine:** a correct and fast hot path.
2. **Measurement lab:** TSC timer, histograms, open-loop load generation, environment capture, statistics.
3. **Experiments:** written-up studies that explain *why* results happen, using hardware counters.

**Narrative (v1.0):**

> A C++20 low-latency market-data and execution engine on real NASDAQ ITCH data. I characterized the workload, designed the order book from that data, and showed with hardware counters why each optimization worked. I compared run-to-completion and pipelined architectures across offered load without coordinated omission, and measured tick-to-trade latency and its tail under Linux jitter tuning.

---

## 2. Release Structure

```text
CORE (required)                         -> Checkpoint "core" (~week 8): already presentable
  E0 measurement, E1 workload, E2 book study, E3 queue study, E4 HERO threading-model study

HFT SPECIALIZATION (required for v1.0)  -> v1.0 (~week 13)
  E5 feed-handler study (receive path + A/B arbitration + recovery)
  E6 tick-to-trade + jitter tuning study

SILICON SPECIALIZATION (v1.1)           -> v1.1 (~week 17)
  E7 x86 vs AArch64 memory-model study
  E8 compiler study (PGO/LTO/BOLT)
  E9 energy vs latency study

EXTENSIONS (optional, no deadline)
  AF_XDP, io_uring, DPDK (with lab NIC), SIMD, LSE atomics, core-to-core matrix (on AMD/ARM),
  dispatch-cost study, Grafana demo, eBPF tracing, NUMA, PTP/hardware timestamps
```

**Project success never depends on an extension.**

### Why these 10 and not others

| Kept | Reason |
|---|---|
| E1 workload | Turns data-structure choices into evidence-driven decisions; almost nobody does it |
| E2 book study | The core HFT data structure, and the best vehicle for discussing caches, TLB, allocation and branches |
| E3 queue study | Lock-free correctness plus coherence costs, all in one experiment |
| E4 threading model | Hero result: throughput vs queueing vs coherence vs tail latency in one plot |
| E5 feed study | Real exchange feed mechanics (multicast, arbitration, recovery); the kernel receive path |
| E6 jitter | What HFT infra teams actually tune; it explains the tail |
| E7–E9 | The bridge to CPU/SoC roles; complements your LLVM/CUDA project instead of duplicating it |

---

## 3. Your Machine and What It Allows

From `lscpu` on the dev laptop:

| Item | Value | Implication |
|---|---|---|
| CPU | Intel Core i5-8250U (Kaby Lake R), 4C/8T, 1 socket, 1 NUMA node | No NUMA claims. SMT siblings are CPU *n* and *n+4* (0/4, 1/5, 2/6, 3/7) |
| Caches | L1d 32 KiB/core, L2 256 KiB/core, L3 6 MiB shared | Size the book working set against these |
| ISA | AVX2, BMI2. No AVX-512 | |
| TSC | `constant_tsc`, `nonstop_tsc`, `rdtscp`, `tsc_adjust` | TSC usable; cross-core skew still verified in E0 |
| PMU | PEBS, LBR, Intel PT | Top-down analysis, `perf c2c`, Intel PT/magic-trace, BOLT all possible |
| Power | intel_pstate with HWP, RAPL | Turbo/thermal noise must be controlled; energy measurable (E9) |
| RAM | 7.6 GiB | Stream ITCH; never load a full day |
| NIC | Wi-Fi only | veth + network namespaces; no NIC/wire claims; DPDK not meaningful |
| Virtualization | Bare metal | PMU counters fully available |
| Toolchain | GCC 15.2. **clang, perf, llvm-bolt missing** | Install in P0 |

**Can claim:** user-space engine performance, kernel UDP receive path over veth, scheduler/IRQ jitter, microarchitecture behavior on Kaby Lake.

**Cannot claim:** NIC or wire-to-wire latency, hardware timestamps, NUMA, kernel-bypass speedups, or behavior of server CPUs you didn't run on.

The hardware limits are stated as part of the methodology, not hidden.

**v1.1 needs ARM hardware:** a Raspberry Pi 5 (bare metal, full PMU), an Oracle Ampere A1 instance (historically has an always-free tier), an AWS Graviton instance, or a Snapdragon X device. Check current availability.

---

## 4. Architecture

```text
 ITCH 5.0 day file (streamed, never in repo)
        |
        v
 +--------------------------------+                    netns "exch"
 | Feed Publisher                 |
 |  MoldUDP64 framing             |
 |  open-loop pacing (Nx / rate)  |
 |  A + B multicast legs          |
 |  re-request (retransmit) server|
 +--------------------------------+
        | A: 239.1.1.1:30001      | B: 239.1.1.2:30002       (tc netem: loss / reorder)
 ======================== veth pair ========================
        |                         |
 +--------------------------------+                    netns "engine"
 | Feed Handler                   |  recvmmsg / busy-poll / epoll
 | A/B arbitration + seq tracker  |--gap--> Retransmit client
 | ITCH decoder (zero-copy, BE)   |
 | Order books (per stock locate) |<-- differential test --> Reference book
 | Strategy (static dispatch)     |
 | Risk + kill switch + stale-book guard
 | Order gateway (OUCH-style / SoupBinTCP-style TCP)
 +--------------------------------+
        |
        v
 +--------------------------------+
 | Exchange Emulator              |  matching = reference book + LIMIT/MARKET/IOC/CANCEL/REPLACE
 +--------------------------------+

 Off the hot path:
   per-thread counters + histograms --seqlock snapshot--> telemetry thread --> CSV/JSON
   hot-path log calls --------------SPSC--------------> async logger thread --> binary log
   receiver --> pcap recorder --> replay tool
```

**Book building vs matching:** the engine's feed-side book does **book building**. ITCH reports events the exchange already matched; the engine reconstructs state. Only the **exchange emulator** matches orders. These are two separate workloads; keep them separate.

### Threading models (E4)

```text
Model A, run-to-completion: one pinned thread
  input -> decode -> book -> strategy -> risk -> gateway

Model B, pipelined:
  [thread 1] input/decode --SPSC--> [thread 2] book/strategy/risk --SPSC--> [thread 3] gateway
```

---

## 5. Engineering Principles

### 5.1 Experiment loop

```text
Question -> Hypothesis (with mechanism) -> Baseline -> Change ONE thing -> Measure
         -> Explain with counters -> Note where the hypothesis was wrong -> Write up
```

### 5.2 Hard rules

1. **Never fabricate numbers.** The README and resume use only measured, reproducible results. Before data exists, use neutral wording ("cycle-level instrumentation", "tail-latency analysis"), not unit claims.
2. **Measure the measurement first** (E0).
3. **No coordinated omission.** Load is open-loop; latency is measured from the *intended* send time.
4. **Statistics, not anecdotes.** Use multiple interleaved runs and confidence intervals (Section 9).
5. **One variable at a time**, then combinations.
6. **Explain with counters.** "Faster" is not a result. "Faster because LLC misses/msg fell from X to Y" is.
7. **Negative results count**, and so do wrong hypotheses. Write them up.
8. **No sanitizer, debug or instrumented builds in performance numbers.**
9. **Reproducibility is part of the result.** Every result file carries the environment JSON (CPU, microcode, kernel, cmdline, compiler and flags, git SHA, workload, offered load, affinity, governor/turbo state).

### 5.3 Experiment write-up template (`docs/experiments/EN-name.md`)

```text
# EN: Title
Question:
Hypothesis (expected mechanism):
Setup: hardware, kernel cmdline, build flags, git SHA, input slice, offered load, pinning
Variants:
Method: runs, warm-up, samples/run, open-loop schedule
Results: table + percentile plot, 95% CI
Counter evidence:
Where the hypothesis was wrong:
Conclusion (workload-specific):
Threats to validity:
Reproduce: exact commands
```

---

## 6. Repository Layout

```text
low-latency-engine/
├── CMakeLists.txt, CMakePresets.json   # debug, release, relwithdebinfo, asan, ubsan, tsan (+ pgo/lto/arm64 in v1.1)
├── README.md, .clang-format, .clang-tidy
├── data/
│   ├── README.md        # where to obtain ITCH files + terms; NO Nasdaq data committed
│   └── checksums.sha256
├── docs/
│   ├── architecture.md, itch-moldudp64.md, orderbook.md, concurrency.md,
│   ├── linux-tuning.md, methodology.md, limitations.md, technical-report.md
│   └── experiments/E0-…md … E9-…md
├── include/ src/
│   ├── core/         # types, TSC clock, pinning, config
│   ├── protocol/     # ITCH decoder, MoldUDP64, OUCH-style, SoupBinTCP-style
│   ├── feed/         # sockets, recvmmsg, arbitration, seq tracking, recovery
│   ├── book/         # reference_book, ladder_book, id_map, bitmap
│   ├── memory/       # pool, hugepage arena, prefault
│   ├── concurrency/  # spsc_ring, seqlock
│   ├── strategy/ risk/ gateway/
│   ├── telemetry/    # counters, histogram, async logger
│   └── arch/         # x86 (rdtscp); aarch64 (CNTVCT) in v1.1
├── apps/  engine/ publisher/ exchange_emulator/ replay/ feeder/ hiccup/
├── tests/ unit/ protocol/ book_differential/ concurrency/ replay/ integration/
│   └── fixtures/     # SYNTHETIC ITCH-format streams generated by tools/gen_fixture
├── fuzz/             # libFuzzer: itch_decoder, moldudp64
├── verify/           # timeboxed GenMC/Relacy model of the SPSC queue
├── benchmarks/       # Google Benchmark micro-benches
├── tools/  gen_fixture/
├── scripts/  fetch_itch.sh  env_capture.sh  tune_machine.sh  netns_setup.sh
│             run_bench.py  analyse.py  slice_itch.py
└── .github/workflows/ci.yml
```

---

## 7. Core (required): Phases P0–P4, ~8 weeks

### P0: Toolchain and Measurement Infrastructure → **E0** (weeks 1–1.5)

**Install:** `clang lld llvm` (llvm-mca, llvm-profdata, llvm-bolt), `linux-perf`, `cmake ninja gdb valgrind pigz`, Python with `numpy pandas matplotlib`, GoogleTest, Google Benchmark, pmu-tools (`toplev`), `rtla`, `tcpdump`, `wireshark`. For development, set `kernel.perf_event_paranoid=1` and document it.

**Presets:** `release` = `-O2 -DNDEBUG` is the baseline for all numbers. `relwithdebinfo` = `-O2 -g -fno-omit-frame-pointer` for profiling. Sanitizer presets for tests only. CI uses `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wold-style-cast -Werror`.

**TSC clock:**
- Start stamp: `lfence; rdtsc`. End stamp: `rdtscp; lfence`. `rdtscp` waits for earlier instructions but not later ones, so the trailing `lfence` stops later work from starting early.
- Calibrate ticks/ns against `CLOCK_MONOTONIC_RAW` over ~1 s, and record the calibration with each result.
- Wrap it behind an interface so AArch64 `CNTVCT_EL0` can plug in for v1.1.

**Histogram:** log-linear, fixed memory, no allocation on record. Validate against HdrHistogram_c.

**`env_capture.sh`** writes JSON alongside every result. **`tune_machine.sh`** (apply/restore): `cpupower frequency-set -g performance`, `intel_pstate/no_turbo=1` (repeatability matters more than peak speed), AC power, thermal state recorded.

**E0 (measurement characterization):**
- Per-call cost (cycles and ns) of `rdtsc`, `lfence;rdtsc`, `rdtscp`, `steady_clock::now()`, `clock_gettime(CLOCK_MONOTONIC / _RAW)`. All have ~1 ns *resolution*; the comparison is about *overhead and serialization*.
- **Cross-core TSC skew:** ping-pong timestamps between core pairs. Estimate the offset NTP-style, `((t1−t0) + (t2−t3)) / 2`, bounded by RTT/2. This justifies comparing stamps taken on different cores (E4, E5, E6).
- Histogram record cost.
- Every later stage-latency number is read against these.

---

### P1: ITCH Decoder, Reference Book, Differential Testing → **E1** (weeks 2–3)

**Data handling (licensing-safe):**
- Real data: NASDAQ TotalView-ITCH 5.0 sample day files (historically at `emi.nasdaq.com/ITCH/`). Verify the current location and terms. `scripts/fetch_itch.sh` downloads them and checks `data/checksums.sha256`. **Never commit Nasdaq data or derived slices.**
- Stream with `pigz -dc` or zlib; files are several GB compressed. `slice_itch.py` makes local dev slices: the first N million messages, or the top 50 symbols.
- **CI fixture:** `tools/gen_fixture` generates **synthetic ITCH-format** streams (adds, executes, cancels, deletes, replaces; deep books; edge cases) from a fixed seed. This tests the decoder and book without redistributing any data.

**Decoder facts:**
- Every message in the file is prefixed by a 2-byte big-endian length. All fields are big-endian. The common header is Type(1), Stock Locate(2), Tracking(2), Timestamp(6, ns since midnight).
- Prices are 4-byte integers with 4 implied decimals. No floating point anywhere.
- **Stock Locate** indexes a plain array of books, so there is no string hashing on the hot path.
- Book messages: `A`, `F` (add), `E`, `C` (executed), `X` (partial cancel), `D` (delete), `U` (replace: **new ref, loses priority**). Also handle `R` (directory), `S` (system), `H` (trading action). For other types, parse the length and skip.
- `E/C/X/D` carry only the order ref, so the order map must store side, price and locate.

**C++20-only big-endian loads:**

```cpp
#include <bit>
#include <concepts>
#include <cstring>
#include <version>

template <std::unsigned_integral T>
[[nodiscard]] constexpr T bswap(T v) noexcept {
#if defined(__cpp_lib_byteswap)               // only when built as C++23
    return std::byteswap(v);
#else
    if constexpr (sizeof(T) == 1)      return v;
    else if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
    else if constexpr (sizeof(T) == 4) return __builtin_bswap32(v);
    else                               return __builtin_bswap64(v);
#endif
}

template <std::unsigned_integral T>
[[nodiscard]] inline T load_be(const std::byte* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof v);              // no reinterpret_cast of packed structs (aliasing/alignment UB)
    if constexpr (std::endian::native == std::endian::little) v = bswap(v);
    return v;
}
// 48-bit ITCH timestamp: (uint64_t(load_be<uint16_t>(p)) << 32) | load_be<uint32_t>(p + 2)
```

Bounds-check every declared length against the remaining buffer.

**Reference book (obviously correct):** `std::map` price levels → `std::list<Order>` per level → `std::unordered_map<OrderRef, Handle>` (handle = locate, side, price, list iterator). `std::list` iterators are stable, so cancel is O(1).

**Tests:**
- Unit tests per message type: partial executions, replace priority loss, unknown refs, zero-qty results.
- **Differential harness** (used from P2 on): reference vs optimized book on the same stream. Compare top-of-book after every message and full depth every N messages. On mismatch, dump the message index and a minimal reproduction.
- **Property-based streams:** random and adversarial (deep books, far prices, add/cancel churn, many orders on one level).
- **libFuzzer** on the decoder and MoldUDP64 parser under ASan/UBSan.
- Logged invariants (not asserts): crossed/locked books, negative quantities. Investigate any hits.

**E1 (workload characterization)** on a full day:
- message-type mix and add/cancel/execute ratios
- order lifetime distribution
- book depth and level occupancy
- distance of order prices from best
- active-symbol skew
- burstiness: peak msgs per 1 ms / 10 ms / 1 s vs mean
- whether order refs are dense

**E1's numbers justify every choice in P2, and they set the offered-load range for E4.**

---

### P2: Optimized Order Book, Memory, Profiling → **E2** (weeks 4–6)

**Design:**

```cpp
struct OrderNode {          // 32 bytes: two per cache line
    uint64_t ref;
    uint32_t price;         // ITCH price, 4 implied decimals
    uint32_t qty;
    uint32_t prev, next;    // pool indices, intrusive doubly-linked list
    uint16_t locate;
    uint8_t  side;
    uint8_t  _pad[5];
};
static_assert(sizeof(OrderNode) == 32);

struct Level { uint64_t total_qty; uint32_t head, tail, count; };
```

- **Price ladder:** per symbol and side, an array of `Level` indexed by `(price − base) / tick` within ±W ticks. Prices outside the window go to a sparse `std::map` fallback. Recentre when needed and count recentres. **W comes from E1.**
- **Best price:** a two-level bitmap of non-empty levels; find the next level with `std::countr_zero` / `std::countl_zero`.
- **Order-ref map:** open addressing (linear probing, power-of-2 capacity, backward-shift delete). Use a direct-indexed vector if E1 shows refs are dense.
- **Memory:** fixed pools; `mlockall(MCL_CURRENT|MCL_FUTURE)` plus prefault (`MAP_POPULATE`/touch); optional 2 MiB pages (`MAP_HUGETLB` or THP `madvise`) for pools and the ID map.
- **Asserted invariants:** **0 heap allocations** after warm-up (counting `operator new`/`malloc` shim in a test build) and **0 page faults** in steady state (`perf stat -e page-faults`).

**Profiling toolkit (used here, reused everywhere):**
- `perf stat`; `perf record --call-graph=fp` → FlameGraph.
- **Top-down analysis:** `toplev`, or `perf stat -M TopdownL1` (check `perf list metricgroups`). Classify stages as frontend-bound / bad speculation / backend (memory vs core) / retiring, and drill down to L2–L3.
- **Intel PT / magic-trace:** explain at least one real p99.9 event instruction by instruction.
- **llvm-mca** on the hottest loop: predicted vs measured throughput.

**E2 (order-book study):** layered build-up plus ablation.

```text
L0  reference: map + list + unordered_map + new/delete
L1  + pooled intrusive order lists (still map levels)
L2  + tick ladder + bitmap best-price
L3  + open-addressing (or direct) order-ref map
L4  + prefault + mlockall
L5  + 2 MiB pages
Ablation: final build minus each single layer (leave-one-out) -> each layer's real contribution
```

- **Metrics per layer:** ns/msg p50/p99/p99.9/max, msgs/s, instructions/msg, IPC, L1d/LLC misses/msg, branch misses/msg, dTLB walk cycles, page faults, TMA L1/L2.
- **Workloads:** full day, top-symbol slice, adversarial synthetic.
- **The write-up must explain the mechanism behind each layer's change** (pointer chasing → MLP, allocation → page faults/cache pollution, map depth → branch misses, TLB reach), plus at least one Intel PT tail-event explanation.

---

### P3: Queues and Concurrency → **E3** (week 7)

**SPSC ring:**
- Power-of-2 capacity, masked indices.
- Producer and consumer indices on separate cache lines.
- **Cached remote index:** each side reloads the other's index only when the ring looks full or empty.
- Batch publish.

**Correctness argument** (`docs/concurrency.md`): the producer writes the slot, then does a release store of the tail; the consumer does an acquire load of the tail, then reads the slot. The release/acquire pair gives happens-before, so there is no data race. A symmetric argument covers the head for slot reuse.

**Seqlock** for telemetry snapshots: fields as relaxed atomics plus fences, per Boehm's "Can Seqlocks Get Along with Programming Language Memory Models?"

**Verification:**
- TSan stress tests (randomized delays, 10⁸ operations).
- **Timeboxed model check (2 days max):** GenMC or Relacy on the SPSC logic. The goal is to show it accepts acquire/release and rejects a weakened ordering. If tooling blocks, drop it, note the reason, and move on.

**E3 (queue study).** Avoid a full cross-product; sweep one axis at a time from the best configuration.
1. **Variants:** mutex+condvar → mutex+spin → atomics `seq_cst` → acquire/release → +cached indices → +batching.
2. **Padding:** none vs 64 B vs 128 B (the adjacent-line prefetcher works on 128 B pairs). Confirm with `perf c2c` HITM counts.
3. **Placement:** unpinned vs SMT siblings (e.g. 2/6) vs separate physical cores (2/3).
- **Metrics:** throughput, one-way latency percentiles (TSC-stamped, valid per E0 skew bounds), CPU%, context switches, HITM.

---

### P4: HERO: Threading-Model Study → **E4** (week 8)

**Setup (in-process, no networking needed):**
- `apps/feeder` runs on its own pinned core. It replays an ITCH slice into a raw-message SPSC ring on an **open-loop schedule**: either the original timestamps scaled by a speed factor, or a fixed rate. Each message carries its **intended** send TSC.
- If the ring is full, the feeder stalls, but latency is still measured from the intended time, so queueing is captured (no coordinated omission).
- The engine runs **Model A** (one thread does everything) or **Model B** (3-stage pipeline). The feeder → engine hop is identical in both, so differences come from the internal architecture.
- **Work knob:** a configurable synthetic strategy cost (spin for *k* ns per message) to sweep per-message work.

**Sweep:** offered load from 10% to past saturation, with the range taken from E1's mean and burst rates, × per-message work {0, 200, 500, 1000 ns}.

**Outputs:**
- **Latency percentiles vs offered load** (the hockey-stick curve) for A vs B at each work level.
- The crossover point where B starts to win, if it exists.
- Max sustainable throughput.
- Per-hop costs in B (coherence transfers, measured with `perf c2c` and TSC stamps).
- Bursty (original timestamps) vs smooth (fixed-rate) arrival.

**Hypothesis:** A wins on latency below saturation because there are no cross-core hops. B extends the saturation point once per-message work exceeds the inter-arrival time. Real ITCH bursts may push A into queueing well below its average-rate capacity. **Report where this was wrong.**

**This is the headline plot of the README.**

**Checkpoint "core" (~end of week 8):** E0–E4 written. The project is already presentable and resume-ready. Tag `v0.5`.

---

## 8. HFT Specialization (required for v1.0): Phases P5–P8, ~5 weeks

### P5: Network Feed Handler → **E5** (weeks 9–10)

**Topology:** `netns_setup.sh` creates namespaces `exch` and `engine` with a veth pair and multicast routes. Both share one TSC (cross-process comparisons are valid per E0).

State the scope clearly: this measures the **kernel UDP receive path, softirq and scheduling over veth**, not NIC DMA, interrupt moderation or the wire.

**Publisher (MoldUDP64, public spec):**
- Packet = Session(10) + Sequence(8, first message) + Count(2), then blocks of Length(2) + ITCH message. Count 0 is a heartbeat; `0xFFFF` is end of session.
- Packs messages up to MTU and sends identical packets on legs A and B.
- **Open-loop** pacing (same scheduler as `feeder`). Logs `(seq, intended_tsc)` to shared memory/side log; the protocol stays unchanged and analysis joins on sequence number.
- Re-request server answers retransmission requests for missed ranges.
- Faults injected with `tc netem` (loss, correlated burst loss, reordering) on one or both legs.

**Receiver:**
- Non-blocking sockets, `IP_ADD_MEMBERSHIP`, tuned `SO_RCVBUF`.
- Strategies: blocking `recvfrom`, `epoll`, busy-spin, **`recvmmsg`** batching, **`SO_BUSY_POLL`** (+ `net.core.busy_poll`).
- **`SO_TIMESTAMPING`** software RX timestamps measure the kernel → user-space delay. They are `CLOCK_REALTIME`, so convert carefully against the TSC calibration.
- Drops: read `/proc/net/snmp` (`RcvbufErrors`, `InErrors`) before and after each run.

**A/B arbitration and recovery:**
- Per-leg and merged sequence tracking; the first copy wins and duplicates are dropped.
- If a gap stays unfilled by the other leg for T µs, send a re-request.
- While a gap is open, the affected books are **stale**: risk blocks trading on them (P6).

**E5 (feed-handler study):**
- **Part 1, receive path:** strategies × 3–4 offered rates. Measure intended-send → user-space latency percentiles, kernel → user delay, CPU%, and drops.
- **Part 2, resilience:** loss on A only / B only / both / correlated bursts. Measure the fraction of gaps hidden by arbitration, the recovery latency distribution, and total stale time.
- **Part 3, validation:** re-run the E4 comparison end-to-end over the network at 2–3 loads, to check that the in-process conclusion holds.

---

### P6: Minimal Trading Layer and Emulator (week 11)

Keep this small. It exists to make tick-to-trade measurable and to show a correct risk design; there is no experiment of its own.

- **Strategy:** order-book imbalance or spread trigger. Deterministic, configurable, static dispatch (templates/CRTP).
- **Risk:** max order qty, max position, max notional, price band vs mid, order-rate token bucket, duplicate client-order-ID check, **stale-book guard**, and a **kill switch** (atomic flag checked once per order; set by command, loss limit or prolonged staleness). Measure risk-check cost in ns.
- **Gateway:** OUCH-style Enter/Cancel/Replace, SoupBinTCP-style framing over TCP, `TCP_NODELAY`, preallocated buffers.
- **Exchange emulator:** a TCP server using the **reference book with matching enabled** (LIMIT, MARKET, IOC, CANCEL, REPLACE). Replies Accepted/Executed/Canceled/Rejected. Unit-test price-time priority, partial fills, multi-level sweeps and IOC remainder cancel.
- **Async logger:** per-thread SPSC of binary records (format ID + raw args); a background thread formats them. Measure log-call cost.
- **Telemetry:** per-thread, cache-line-aligned counters and histograms → seqlock snapshot → telemetry thread → CSV/JSON. Analysis and plots are done in Python. A Grafana live demo is an optional extension and never touches the hot path.

**Tick-to-trade definition:** from packet RX (both the kernel SW timestamp and the first user-space TSC) to order bytes handed to `send()`. Always reported with a per-stage breakdown.

---

### P7: Tick-to-Trade and Jitter Tuning → **E6** (week 12)

**Tools:**
- `apps/hiccup`: a pinned spin loop that records TSC gaps above a threshold, giving a histogram of OS interruptions.
- `rtla osnoise` / `rtla timerlat`: attributes noise to IRQ, softirq, threads or timer ticks.

**Knobs.** Apply one at a time, then combined. Engine on cores 2 and 3; siblings 6 and 7 kept idle.

```text
isolcpus=managed_irq,domain,2,3,6,7  nohz_full=2,3,6,7  rcu_nocbs=2,3,6,7  irqaffinity=0,1,4,5
governor performance + no_turbo=1           (baseline for all configs)
C-states limited (cpupower idle-set)
THP: always vs madvise vs never
irqbalance off; IRQs pinned away from engine cores
SCHED_FIFO for engine threads (mind kernel.sched_rt_runtime_us throttling)
mlockall + prefault (from P2)
```

**E6 (tick-to-trade and jitter study):** per configuration, measure hiccup p99.99/max with `rtla` attribution, and tick-to-trade p50/p99/p99.9/max with stage breakdown. Explain each change by mechanism (e.g. "`nohz_full` removed the periodic tick → max hiccup fell from X to Y µs").

Laptop caveat: firmware SMIs and thermal management stay as residual noise. Quantify it and report it.

---

### P8: Report, README, Resume (week 13) → **v1.0**

`docs/technical-report.md`, written like a short paper:

1. Abstract with real numbers
2. Questions
3. Design
4. Methodology (hardware, tuning, timer, open-loop load, statistics, limitations)
5. Workload (E1)
6. Results: E2, E3, **E4**, E5, E6
7. Discussion: what generalizes and what is machine-specific; wrong hypotheses
8. Threats to validity
9. Future work (v1.1 and extensions)

Optional: publish a condensed blog post.

---

## 9. Benchmark Methodology

**Procedure:**
1. `tune_machine.sh apply` (or the named E6 configuration), AC power, `env_capture.sh`.
2. Pin all threads and record the CPU map.
3. Warm up and discard.
4. **N ≥ 10 runs per configuration, interleaved** (A B A B …).
5. Open-loop load.

**Tail sample counts:** p99 needs ≥10⁴ samples per run, p99.9 needs ≥10⁵, p99.99 needs ≥10⁶. The max is reported separately as a single event.

**Statistics:** per-run percentiles → median across runs with a **95% bootstrap CI**. For A-vs-B claims, use the CI of the difference (or Mann-Whitney U). Claim an improvement only if the CI excludes 0.

**Units:** cycles and ns (state the TSC frequency). Instructions/msg is the noise-free secondary metric.

**Plots:** HdrHistogram-style percentile plot (log 1/(1−p) axis); latency vs offered load (E4, E5); TMA stacked bars (E2); hiccup histograms (E6).

---

## 10. Testing, Replay and CI (continuous)

| Kind | What |
|---|---|
| Unit | Decoder per type, book ops, risk rules, MoldUDP64 framing, ring wrap/full/empty, emulator matching |
| Differential | Optimized vs reference book: full real day locally, synthetic fixture in CI |
| Property-based | Random and adversarial synthetic streams |
| Fuzz | libFuzzer on the decoder and MoldUDP64 under ASan/UBSan |
| Concurrency | TSan stress; timeboxed model check |
| Integration | publisher → engine → emulator in netns, with netem loss |
| Allocation / faults | 0 hot-path allocations and 0 steady-state page faults, asserted |
| Replay | pcap capture → replay at 1×/10×/100×/max. **Output digest** (hash of book events, signals, orders) identical across runs and builds |
| Perf regression | CI: **instruction counts** (cachegrind `Ir`), since wall-clock is too noisy on shared runners. Locally: `run_bench.py --compare baseline.json` for latency/tails |

**CI matrix:** {gcc, clang} × {debug, asan, ubsan, tsan}, plus a 60 s fuzz smoke test and the replay digest check on the synthetic fixture.

---

## 11. Silicon Specialization (v1.1): Phases P9–P11, ~4 weeks after v1.0

Builds on the same engine. Each item reuses existing tooling.

### P9: AArch64 Port and Memory-Model Study → **E7** (~2 weeks)

- **Port:** build natively on ARM hardware, or cross-compile and run functional tests under `qemu-user`. **qemu does not reproduce weak-memory behavior**, so experiments run on real hardware.
- **Timer:** `CNTVCT_EL0` with `isb`, frequency from `CNTFRQ_EL0`. It is usually much coarser than the TSC; report the resolution and its effect.
- **Cross-ISA determinism:** the replay digest must match x86.

**E7 (x86 TSO vs ARM weak ordering):**
1. **Litmus tests** (`litmus7` or own harness), Message Passing and Store Buffering with relaxed atomics, run on both ISAs. Report **observed reorderings per N iterations, including 0**. Expected (hypothesis): MP reorders only on ARM; SB reorders on both, because of the x86 store buffer.
2. **Weakened SPSC** (relaxed tail store). *Hypothesis:* passes stress tests on x86, can fail on ARM. Report the observed failure frequency, which may be rare or zero. **Independent evidence:** TSan and the model checker flag it on both ISAs regardless of hardware behavior.
3. **Ordering cost:** generated assembly and measured SPSC throughput per ordering. On x86, a `seq_cst` store becomes `xchg` (or `mov`+`mfence`) while acquire/release are plain `mov`. On ARM, they are `stlr`/`ldar` (plus `ldapr` with RCpc).

### P10: Compiler Study → **E8** (~1 week)

- **Build ladder:** `-O2` → `-O3` → `-march=native` → `+LTO` → `+PGO` (trained on ITCH replay) → `+BOLT` (LBR profile via `perf record -e cycles:u -j any,u`, then `llvm-bolt`).
- **Metrics:** ns/msg percentiles, instructions/msg, IPC, **TMA frontend-bound share**, i-cache/iTLB misses, branch misses, binary size.
- **Hypothesis:** PGO and BOLT mostly help frontend-bound stages (decode/dispatch); a memory-bound book update gains little. Verify with TMA and inspect the assembly diffs.
- Link the write-up to your LLVM/CUDA project.

### P11: Energy vs Latency → **E9** (~2–3 days)

- RAPL counters (`perf stat -a -e power/energy-pkg/,power/energy-cores/`).
- Compare busy-poll vs blocking wait (socket receive, or queue consumer spin vs futex/condvar) at 3–4 offered loads.
- Plot **joules/message vs p99** as a Pareto curve.
- RAPL is a modeled estimate; state that.

**v1.1 done:** E7–E9 written and added to the report. The silicon resume version becomes available.

---

## 12. Extensions (optional, no deadline)

| Extension | When it's worth it |
|---|---|
| AF_XDP (veth native XDP), `io_uring` multishot recv | After v1.0, as a receive-path comparison in E5 |
| DPDK, hardware timestamps, PTP | Only with lab access to a supported NIC |
| Core-to-core latency matrix | Only on AMD multi-CCD or ARM hardware |
| SIMD (AVX2/NEON) for bitmap scan / byte-swap | Expect modest gains; a fine negative result |
| LSE vs LL/SC atomics | With the ARM box, ~1 day |
| Virtual vs CRTP vs `variant` dispatch | Known result; small add-on |
| eBPF/bpftrace (`runqlat`, `offcputime`) | Extra attribution for E6 |
| Grafana live demo | Demos only, off the hot path |
| NUMA | Rented 2-socket instance |
| MPMC queue vs libraries | Concurrency deep-dive |

---

## 13. Timeline (15–20 h/week)

| Week | Work | Exit criterion |
|---|---|---|
| 1–1.5 | P0 tooling, TSC clock, histogram, env capture → **E0** | E0 written, incl. cross-core skew |
| 2–3 | P1 decoder, reference book, fixture generator, differential harness, fuzzing → **E1** | Full real day processed; E1 written |
| 4–6 | P2 optimized book layers, profiling, Intel PT → **E2** | Passes differential test on a full day; E2 with ablation |
| 7 | P3 SPSC, seqlock, TSan, model check (≤2 days) → **E3** | E3 written |
| 8 | P4 feeder + Model A/B + sweep → **E4 (hero)** | **Checkpoint "core", tag v0.5; first resume version** |
| 9–10 | P5 netns, publisher, receiver, arbitration, recovery → **E5** | Loss tests pass; E5 written |
| 11 | P6 strategy, risk, gateway, emulator, logger, telemetry | Tick-to-trade measured |
| 12 | P7 jitter tuning → **E6** | E6 written |
| 13 | P8 report, README, resume, mock interviews | **v1.0** |
| 14–17 | P9–P11 → **E7, E8, E9** | **v1.1** |

**If the HFT deadline comes sooner:**
- **≤ 8 weeks:** stop at Checkpoint "core". Use the core resume bullets.
- **≤ 11 weeks:** core + P5 without retransmit recovery (arbitration only) + P6 minimal; skip E6 and measure tick-to-trade untuned.
- Always reserve the last week for the write-up. Unwritten experiments don't count in interviews.

---

## 14. Definition of Done

### Checkpoint "core" (v0.5)
- [ ] Clean-checkout build; CI green ({gcc, clang} × {asan, ubsan, tsan})
- [ ] No Nasdaq data in the repo; `fetch_itch.sh` + checksums; synthetic CI fixture
- [ ] Decoder handles a full real day; fuzzed
- [ ] Optimized book passes the differential test on a full day
- [ ] 0 hot-path allocations, 0 steady-state page faults (asserted)
- [ ] SPSC correctness argument written; TSan clean; model check done or its omission documented
- [ ] E0–E4 written with CIs, counter evidence, and "where the hypothesis was wrong"

### v1.0
- [ ] MoldUDP64 multicast, A/B arbitration, retransmit recovery, stale-book guard
- [ ] Risk + kill switch; emulator matching tests pass
- [ ] Tick-to-trade with stage breakdown
- [ ] E5–E6 written
- [ ] Replay digest deterministic across runs and builds
- [ ] Technical report + README placement-ready
- [ ] **Every number backed by a result file + env JSON + reproduce command**

### v1.1
- [ ] AArch64 build; cross-ISA digest matches
- [ ] E7–E9 written and in the report

---

## 15. README Structure

```text
# Low-Latency Market Data & Execution Engine
One line + 3 headline results (hardware stated)
## Hero result            (E4 latency-vs-load plot)
## Results at a glance    (E2 layer table, E5, E6)
## Architecture           (diagram, threading models)
## Correctness            (differential, fuzz, model check, determinism)
## Methodology            (one paragraph + link)
## Build / Run / Reproduce (incl. fetch_itch.sh)
## Limitations
## Experiments index      (E0–E9)
```

---

## 16. Resume Bullets (templates)

Fill each `[ ]` only with measured values.

### HFT version (v1.0)

**Low-Latency Market Data & Execution Engine** | C++20, Linux, UDP multicast, lock-free, perf

- Built a C++20 feed handler and per-symbol order book on real **NASDAQ TotalView-ITCH 5.0** data (MoldUDP64 multicast, A/B line arbitration, retransmit gap recovery, stale-book risk guard), verified against a reference book via differential testing over [N] M messages.
- Designed the book from measured workload statistics (tick-indexed ladder, bitmap best-price search, pooled intrusive lists, zero hot-path allocations); layer-by-layer ablation with hardware counters showed [X]% lower p99 and [Y]% fewer LLC misses/msg vs `std::map`.
- Compared run-to-completion and pipelined lock-free architectures under open-loop load (no coordinated omission): [A] kept p99 below [X] up to [N] M msgs/s, with the crossover at [Y]. Tuned isolcpus/nohz_full/IRQ affinity, cutting tick-to-trade p99.9 by [Z]×.

### Core-only version (if you stop at the checkpoint)

- Built a C++20 ITCH 5.0 decoder and order book on real NASDAQ data, verified bit-exact against a reference implementation via differential testing and fuzzing.
- Layer-by-layer ablation (pools, tick ladder, bitmap, open-addressing map, prefaulting, hugepages) with top-down microarchitecture analysis: [X]% lower p99 update latency, IPC [a]→[b].
- Built an open-loop benchmark harness and showed with latency-vs-load curves that [run-to-completion/pipelined] wins below/above [N] msgs/s, because of [mechanism].

### Silicon version (after v1.1)

**High-Performance C++ Systems Engine: Microarchitecture & ISA Study** | C++20, perf, x86-64/AArch64

- Characterized hot-path bottlenecks with top-down analysis and Intel PT; data-layout changes moved the book update from [X]% to [Y]% backend-bound (IPC [a]→[b]).
- Studied x86 TSO vs AArch64 weak ordering with litmus tests and a weakened lock-free queue ([observed result]), plus a model checker; measured ordering costs per ISA from generated code.
- Evaluated PGO/LTO/BOLT ([X]% speedup, frontend-bound −[Y] pts) and the busy-poll energy/latency tradeoff with RAPL ([X] µJ/msg vs [Y] µs p99).

---

## 17. 60-Second Pitch (HFT)

> I built a C++20 market-data and execution engine on real NASDAQ ITCH data. It receives MoldUDP64 over multicast with A/B arbitration and gap recovery, builds per-symbol books, and sends orders through risk to an exchange emulator. I treated it as a performance study. I first characterized the real workload, then built the order book layer by layer and used hardware counters to show what each layer actually bought. The main result compares run-to-completion and pipelined designs under open-loop load: [one-sentence finding]. Finally I measured tick-to-trade and cut its tail with kernel isolation and IRQ tuning, attributing the remaining noise with `rtla`. Every number is reproducible from the repo.

---

## 18. Interview Preparation

**Market data and order book:** book building vs matching; complexity of each operation per variant and why `deque` is wrong; why replace loses priority; how E1 shaped the design; what happens during a gap and why trading stops; what if both legs lose a packet; ITCH price encoding and why integer prices.

**C++:** `memcpy` vs `reinterpret_cast` (aliasing, alignment); hidden allocations (`std::function`, `std::string`, exceptions); how you proved zero allocations; templates/CRTP vs virtual; move semantics and object lifetime.

**Concurrency:** prove the SPSC queue correct (happens-before edges); acquire/release vs `seq_cst` and what each compiles to; why `seq_cst` stores need `xchg`/`mfence` on x86 (store buffer); lock-free vs wait-free; ABA and why SPSC avoids it; false vs true sharing and 64 vs 128 B; why seqlock reads are formally racy in C++; MESI(F)/MOESI on a contended write.

**Architecture (hero):** why run-to-completion is usually lower latency; when pipelining wins; what queueing theory says near saturation; why bursts matter more than averages.

**Linux:** what `isolcpus`, `nohz_full` and `rcu_nocbs` each remove; syscall and context-switch costs; minor vs major faults and prefaulting; why THP can hurt latency; `epoll` vs busy-poll vs `SO_BUSY_POLL` vs `recvmmsg`; the packet path (IRQ → NAPI/softirq → socket buffer → user); what kernel bypass removes and what it costs.

**CPU:** top-down categories and what you did about backend-bound code; what limits IPC in the book update; sources of branch misses; TLB reach with 4K vs 2M pages; MLP and pointer chasing; when llvm-mca is wrong.

**Measurement:** coordinated omission; `rdtscp` + `lfence` semantics; invariant TSC and cross-core skew; samples needed for p99.9; why report CIs; why the mean can improve while p99 worsens; your biggest bottleneck and biggest win, with numbers; **a hypothesis that turned out wrong**.

---

## 19. Honest Limitations (`docs/limitations.md`)

- Educational engine, not production infrastructure.
- ITCH/MoldUDP64 follow public specs. Order entry is OUCH-*style* / SoupBinTCP-*style*, not certified. No Nasdaq data is redistributed.
- Measured on a 4-core Kaby Lake laptop (plus the named ARM machine in v1.1). Server CPUs, NICs and kernels differ.
- Networking runs over veth: no NIC, no wire, no hardware timestamps. Not comparable to colocated wire-to-wire figures.
- Laptop firmware (SMIs, thermal) leaves noise that tuning can't remove.
- Emulator latency is not exchange latency. RAPL is a modeled estimate. ARM timer resolution limits fine-grained timing on AArch64.

---

## 20. What NOT to Build

Web UI, login, charts, portfolio app, REST backend, crypto, ML price prediction, microservices, Kubernetes, a dashboard as the centerpiece, DPDK on unsupported hardware with claimed speedups, or any technology added for a keyword rather than an insight.

---

## 21. Portfolio Fit

| Project | Shows |
|---|---|
| **This engine** | C++, Linux, networking, concurrency, CPU behavior, measurement rigor (HFT lead project) |
| LLVM/CUDA | Compiler internals, IR, GPU optimization (v1.1's E8 links the two) |
| Text-to-SQL | Applied AI, databases |
| TransactiWar | Security, backend |

For HFT applications, lead with this engine. For CPU/SoC roles after v1.1, lead with this engine plus the LLVM project.

---

## 22. Reading and Watching

- **C++ / low latency:** Williams, *C++ Concurrency in Action*; Meyers, *Effective Modern C++*; Carl Cook, "When a Microsecond Is an Eternity" (CppCon 2017); David Gross, "When Nanoseconds Matter: Ultrafast Trading Systems in C++" (CppCon 2024); Preshing on Programming; Erik Rigtorp's blog (SPSC, latency tuning).
- **Measurement:** Gil Tene, "How NOT to Measure Latency".
- **Architecture:** Drepper, *What Every Programmer Should Know About Memory*; Yasin, "A Top-Down Method for Performance Analysis…" (ISPASS 2014); Bakhvalov, *Performance Analysis and Tuning on Modern CPUs*; Agner Fog's manuals; Gregg, *Systems Performance*.
- **Memory models (v1.1):** Nagarajan et al., *A Primer on Memory Consistency and Cache Coherence*; Boehm on seqlocks; herdtools7 docs.
- **Linux / networking:** Kerrisk, *The Linux Programming Interface*; kernel docs for `nohz_full`, `isolcpus`, `rtla`, `SO_TIMESTAMPING`, busy polling.
- **Specs:** NASDAQ TotalView-ITCH 5.0, MoldUDP64, SoupBinTCP, OUCH (public PDFs from Nasdaq).

---

## 23. Commits and Releases

Use Conventional Commits (`feat(book): …`, `perf(memory): …`, `test(diff): …`, `docs(exp): E2 …`).

```text
v0.1  Measurement infra (E0)
v0.2  Decoder + reference book + differential/fuzz (E1)
v0.3  Optimized book (E2)
v0.4  Queues (E3)
v0.5  Hero threading study (E4)       <- Checkpoint "core"
v0.6  Network feed + recovery (E5)
v0.7  Trading layer + jitter (E6)
v1.0  Report: HFT release
v1.1  AArch64 + compiler + energy (E7–E9)
```
