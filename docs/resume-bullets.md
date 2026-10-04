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

## Bullets (pick 3–5)

<!-- E2 bullets are filled in from docs/experiments/E2-orderbook.md once the runs finish. -->
E2_BULLETS_PLACEHOLDER

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
  (8 build-and-test jobs + a clang-format check); 166 unit/differential tests.


## Interview talking points (what each number means)

- **Why measure the timer first (E0).** Every latency is a difference of two timestamps; the
  21.1 ns floor is inside every per-message number, so anything near that size is not
  resolvable. The cores ran at 1.6 GHz while the TSC counts at 1.8 GHz — cycle claims
  therefore come from PMU counters, not TSC ticks.
- **Why the hash function matters (E2, H3).** See the E2 write-up once filled in.
- **Why not a direct array for order refs (E1).** 260 M ref range vs 1.96 M live → ~1 GiB of
  mostly empty slots; open addressing with 2^22 slots stays at load ≤ 0.47.

## Not yet measured (neutral wording only)

Network feed over UDP multicast (MoldUDP64 A/B arbitration, gap recovery), threading models,
SPSC queues, strategy/risk/gateway and tick-to-trade latency: "designed / in progress".
