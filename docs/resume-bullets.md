# Resume bullets (measured numbers only)

Every number below is copied from a write-up in `docs/experiments/` and the result files in
`docs/results/` it cites (result file + `env.json` + reproduce command). Nothing is estimated.
Machine for every timing: Intel i5-8250U at 1.6 GHz (turbo off, `performance` governor),
GCC 15.2 `-O2 -DNDEBUG`, Linux 6.16. Say "on a laptop CPU at 1.6 GHz, turbo off" when
quoting ns numbers, because absolute ns do not transfer to a tuned server.

If a claim is not in this file, it is not measured yet: describe it as "in progress" or
"designed", never with a number.

## Project line

**Low-Latency Market Data Engine (C++20, Linux)** — github.com/ankit-cs25mtech11023/Low-Latency-Market-Data-and-Execution-Engine

## One-page version (3 bullets, submitted 2026-10-04)

Condensed from the long bullets below; same numbers, same sources (E0–E3).

- Built a NASDAQ TotalView-ITCH 5.0 decoder and order book that processes a full trading day
  (**282 M messages**) with zero invariant violations, verified by per-message differential
  testing, fuzzing and sanitizer CI (192 tests); calibrated TSC timing first (21 ns floor,
  cross-core skew within ±2.2 ns).
- Redesigned the order book for cache locality (pooled 32-byte nodes, intrusive FIFO lists,
  open-addressing hash map): **2.64× faster** full-day book building (821 → 311 ns/msg,
  95% CI 2.57–2.69×, N = 10 runs); PMU counters trace the gain to memory stalls
  (IPC 0.32 → 0.60, 3.4× fewer dTLB misses).
- Built a lock-free SPSC queue: **249 ns one-way p50 between cores, 91 M msg/s**, 44× lower
  latency than a mutex queue; showed with hardware counters that acquire/release beats
  `seq_cst` by 3.2× and cached indices give 4.9×.

## Bullets (pick 3–5)

- Rebuilt the order book around the memory hierarchy (32-byte pooled order nodes with intrusive
  FIFO lists, a sorted level vector per side, an open-addressing order-ref map) and made
  full-day book building **2.64× faster** than a `std::map`/`std::list`/`std::unordered_map`
  baseline (**821 → 311 ns/msg**, 95% CI of the speedup 2.57–2.69×; per-message p99
  2.47 → 1.20 µs) while matching the reference book on all **282 M messages**; hardware
  counters attribute the gain to memory stalls (IPC 0.32 → 0.60, dTLB misses 3.4× and LLC
  misses 1.8× fewer, heap allocations 1.19 → 0.00007 per message), not fewer instructions
  (1.4×). *(E2)*
- Found with `perf record`/`perf annotate` that a locality-preserving identity hash for order
  refs, fine on a 1 M-message slice, collapsed on the full day through primary clustering
  (97% of `add` samples in the 5-instruction linear-probe loop) once sequential refs wrapped
  the table; switched to Fibonacci hashing. *(E2)*
- Built and measured a lock-free SPSC ring: **249 ns one-way p50 between cores, 112 ns between
  SMT siblings, 11 ns/msg (~91 M msg/s)**, i.e. 44× lower latency and 30× higher throughput
  than a mutex + condition-variable queue; showed with PMU counters that acquire/release beats
  `seq_cst` by 3.2× in throughput (IPC 0.54 vs 0.14: the `xchg` waits on a cross-core miss per message), a cached remote
  index gives 4.9× (down to ~1 L1D miss per message, the message itself), and 128-byte index
  padding beats 64-byte by 1.5× (consistent with Intel's 128-byte adjacent-line prefetcher). Correctness argued from the C++
  memory model and stress-tested under ThreadSanitizer. *(E3)*

- Built a NASDAQ TotalView-ITCH 5.0 decoder and reference order book that processes a full
  trading day (**282 M messages**, 2019-07-30) with zero invariant violations: 0 unknown or
  duplicate order refs, 0 overfills, 0 live orders at end of day, and all **989 crossed/locked
  tops traced** to halts, LULD pauses or auction re-opens. *(E1)*
- Characterized the real feed to drive the data-structure design: 98% of messages change the
  book, 91% of adds/deletes land within 16 ticks of the best, executions are 100% at the
  best, peak **1.96 M live orders**, bursts up to **2.0 M msg/s in 1 ms windows (175× the
  mean)**; showed order refs span 260 M values (132× peak live) so a direct-indexed array is
  ruled out in favour of an open-addressing hash map. *(E1)*
- Measured the measurement before trusting any latency: `rdtsc` 15.9 ns, fenced start/stop
  stamps 23.5 / 31.1 ns, vDSO `clock_gettime` 37 ns per call; timing floor **21.1 ns (p50)**;
  cross-core TSC skew within **±2.2 ns over all 28 core pairs** with **0 causality violations in
  112 M round trips**; histogram record 4.5 ns. All with N = 10 interleaved runs, 95% bootstrap
  CIs and hardware-counter attribution (cycles, IPC, branch and L1D misses). *(E0)*
- Verified correctness with differential testing against a reference implementation (after
  every message), property-based and adversarial stream tests, a libFuzzer harness for the
  wire decoder, and GitHub Actions CI over {GCC, Clang} × {debug, ASan, UBSan, TSan}
  (8 build-and-test jobs + a clang-format check); 192 unit, property, differential and
  concurrency tests.


## Interview talking points (what each number means)

- **Why measure the timer first (E0).** Every latency is a difference of two timestamps; the
  21.1 ns floor is inside every per-message number, so anything near that size is not
  resolvable. The cores ran at 1.6 GHz while the TSC counts at 1.8 GHz — cycle claims
  therefore come from PMU counters, not TSC ticks.
- **Why the hash function matters (E2, H3).** Sequential order refs + `ref mod 2^22` + linear
  probing: after the refs wrap the table, new refs hit slots still held by long-lived orders,
  probe runs merge and grow (primary clustering). Fibonacci hashing scatters consecutive keys.
  A 1 M-message test never wrapped, so only the full day showed it.
- **Why 2.64× and not more (E2).** The p99 improved less than the median (2.06× vs 2.54×):
  the slow messages are slow for reasons both books share (cold symbols, interrupts).
- **Why acquire/release is enough for SPSC (E3).** Release store of `tail` after writing the
  slot, acquire load of `tail` before reading it: happens-before, no data race. On x86 both are
  plain `mov`; `seq_cst` stores become `xchg`, a full barrier. See `docs/concurrency.md`.
- **Why the cached index matters (E3).** Without it every push reads `head` and every pop reads
  `tail`, so index lines bounce between cores on every message; with it, ~1 L1D miss per
  message remains: the message's own cache line.
- **Why `std::mutex` + spinning is not lock-free (E3).** glibc's mutex calls `futex` on
  contention; the spinning variant spent 29% of CPU time in the kernel and had 5.7 µs p50.
- **Why not a direct array for order refs (E1).** 260 M ref range vs 1.96 M live → ~1 GiB of
  mostly empty slots; open addressing with 2^22 slots stays at load ≤ 0.47.

## Not yet measured (neutral wording only)

Network feed over UDP multicast (MoldUDP64 A/B arbitration, gap recovery), threading models
(E4), strategy/risk/gateway and tick-to-trade latency: "designed / in progress". The
identity-hash book's full-day timing (E2 session B), the seqlock, the model check and
`perf c2c` confirmation of false sharing (E3 part 2) are not measured yet.
