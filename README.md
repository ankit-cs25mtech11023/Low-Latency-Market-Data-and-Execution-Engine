# Low-Latency Market Data & Execution Engine

A Linux C++20 engine that consumes NASDAQ TotalView-ITCH 5.0 market data, builds per-symbol
limit order books, and runs a minimal strategy, risk and order-gateway path against a local
exchange emulator. It is a testbed for reproducible, hardware-counter-explained latency
experiments (E0–E9).

**Status:** under active development. Results appear here only once measured, each backed by
a result file, the captured environment, and a reproduce command.

## Results so far

Machine: Intel i5-8250U laptop CPU, cores at 1.6 GHz (turbo off, `performance` governor),
GCC 15.2 `-O2 -DNDEBUG`, Linux 6.16. Medians of 10 interleaved runs with 95% bootstrap CIs.

| experiment | question | headline (measured) |
|---|---|---|
| [E0](docs/experiments/E0-measurement.md) | What does measuring time cost, and can stamps from different cores be compared? | `rdtsc` 15.9 ns, fenced start/stop stamps 23.5 / 31.1 ns, vDSO `clock_gettime` ~37 ns per call; start/stop floor **21.1 ns** (p50); cross-core TSC skew within ±2.2 ns over all 28 core pairs, **0 causality violations in 112 M round trips** |
| [E1](docs/experiments/E1-workload.md) | What does a real ITCH day look like, and what does it imply for the book design? | 2019-07-30: **282 M messages** through the reference book with 0 invariant violations; all 989 crossed/locked tops explained; 91% of adds/deletes within 16 ticks of the best; peak 1.96 M live orders; 1 ms bursts up to 2.0 M msg/s (175× the mean) |
| [E2](docs/experiments/E2-orderbook.md) (part 1) | How fast can the book be made by changing only its data layout, and which hardware effect does each change remove? | Full day (282 M msgs): pool + intrusive FIFO + level vector + open-addressing ref map is **2.64× faster** than `std::map`/`std::list`/`std::unordered_map` (821 → 311 ns/msg, 3.2 M msg/s on one core), per-message p50 766 → 302 ns, p99 2.47 → 1.20 µs; IPC 0.32 → 0.60, dTLB misses 3.4× and LLC misses 1.8× fewer, heap allocations 1.19 → 7e-5 per msg; identical to the reference book on all 282 M messages. An identity hash for the ref map degrades through primary clustering on the full day (profiled) |
| [E3](docs/experiments/E3-queues.md) (part 1) | What does handing a message between cores cost, and what does each SPSC design choice remove? | Lock-free SPSC ring (acquire/release, cached remote index, padded indices): **249 ns one-way p50 cross-core, 112 ns between SMT siblings; 11 ns/msg (~91 M msg/s)**; 44× lower latency and 30× higher throughput than mutex + condition variable; `seq_cst` 3.2× slower throughput than acquire/release; cached index 4.9× (≈1 L1D miss per message); 128 B index padding 1.5× faster than 64 B without the cache |

## Build

Requirements: Linux x86-64 with an invariant TSC, GCC ≥ 13 or Clang ≥ 18, CMake ≥ 3.25, Ninja.

```bash
cmake --preset release            # -O2 -DNDEBUG: the only build used for performance numbers
cmake --build --preset release
ctest --preset release
```

Other presets: `debug`, `relwithdebinfo` (profiling), `asan`, `ubsan`, `tsan`, and `clang-*` variants.

## Data

No Nasdaq data is stored in this repository. See [`data/README.md`](data/README.md) for how to
obtain the ITCH 5.0 files.

## Documentation

- [`plan.md`](plan.md): project blueprint and phases
- [`docs/methodology.md`](docs/methodology.md): how every number is measured
- [`docs/experiments/`](docs/experiments/): one write-up per experiment

## License

MIT, see [LICENSE](LICENSE).
