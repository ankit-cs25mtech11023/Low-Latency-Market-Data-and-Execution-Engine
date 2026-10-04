# E2: Order-Book Data Structures on a Real ITCH Day

> Status: **partial (part 1 of E2).** This part compares the reference book (standard
> containers) with the optimized book (pool + intrusive FIFO + sorted level vector +
> open-addressing ref map) and isolates one layer, the ref-map hash function. Still to come
> (see `plan.md` P2): per-layer ablation and leave-one-out builds, the tick-ladder + bitmap
> variant, `mlockall`/prefault (L4) and 2 MiB pages (L5, both need memlock/hugepage limits
> set with sudo), top-down analysis, Intel PT for a p99.9 event, and llvm-mca.
> Session A (`ref` vs `fast-fib`, N = 10 full days each) and session B (the identity-hash
> `fast` book, N = 10 full days, run afterwards) are complete and reported below.

## Question

How much faster can a book that reconstructs Nasdaq's order book from ITCH be made by
changing only its data layout, and **which hardware effect** does each change remove?

## Hypotheses (expected mechanism), written before the measured runs

Honest framing: a smoke run on a 1 M-message pre-market slice (`data/slices/first1m.itch`,
not a reported result) was used to debug the tools before these were written, and it is
the reason H3 exists. H1, H2 and H4 come from the design (`plan.md` P2) and E1.

- **H1. The reference book is bound by heap allocation and pointer chasing.** Every add
  allocates a list node, a hash node and sometimes a map node; every message chases pointers
  through a red-black tree and a hash bucket. We expect about **3 heap allocations per add**,
  several cache misses per message, and a few hundred ns per message.
- **H2. The optimized book removes allocation from the hot path** (0 allocations per message
  after warm-up, except the rare growth of a level vector) and is **at least 2x faster** in
  throughput, because the levels touched by an event sit at the back of one contiguous
  vector (E1: 99.9998% of executions at the best price; 91% of deletes within 16 ticks).
- **H3. The hash function matters more than the hash table.** ITCH order refs are assigned
  nearly sequentially and most orders die young, so the live refs are a sliding window of
  mostly consecutive numbers. A locality-preserving identity hash (`ref mod 2^22`) keeps
  consecutive refs in the same cache line and 4 KiB page; the Fibonacci hash, the textbook
  default, scatters them over the 64 MiB table, so nearly every lookup is a cache miss and a
  dTLB miss. We expect identity to show **far fewer dTLB misses per message** and to be
  faster than Fibonacci with otherwise identical code.
- **H4. The tail improves more than the median.** Allocation (malloc slow paths, `brk`/`mmap`)
  and long pointer chains hurt the worst messages most, so the p99/p99.9 gap between the
  books should be larger than the p50 gap.

## Setup

From `docs/results/E2/env.json` (captured before and after session A):

- Intel Core i5-8250U (Kaby Lake R, 4 cores / 8 threads, microcode `0xf6`), `intel_pstate` with
  the `performance` governor on all 8 CPUs, **turbo off** (`no_turbo = 1`), so cores run at
  1.6 GHz nominal; TSC 1.8 GHz, invariant. NMI watchdog off (all 4 general PMU counters free).
- Linux 6.16.8, no `isolcpus` / `nohz_full`, `irqbalance` inactive, transparent huge pages
  `always`, on AC power. Load average 2.10 at the start (desktop session), 1.13 at the end.
- GCC 15.2, `release` preset (`-O2 -DNDEBUG`), git `7e2a7a2`, clean tree.
- Book thread pinned to CPU 2; `pigz` on CPUs 0-1, 3-5, 7 (not CPU 2 or its sibling 6).
- Session A ran 10:57:58–12:28:45 UTC on 2026-10-04: 20 full-day runs, none discarded.

## Variants

| variant | levels | orders | ref -> order map |
|---|---|---|---|
| `ref` | `std::map<Price, Level>` per side | `std::list` per level | `std::unordered_map` |
| `fast` | sorted `std::vector<Level>` per side, best at the back; linear scan from the best (16 levels) then binary search | 32-byte nodes in one preallocated pool (2^22), 32-bit indices, intrusive FIFO | open addressing, 2^22 slots (load ≤ 0.47 at the E1 peak), linear probing, backward-shift delete, **identity hash** |
| `fast-fib` | same as `fast` | same | same, **Fibonacci hash** |

## Method

- **Input: the full day.** Every run streams the whole 2019-07-30 file (282 229 684
  messages) through one book. E1 showed that no small symbol slice is representative, so
  there is no slice result. `pigz` decompresses in a separate process on another core; the
  driver copies messages into a staging buffer of 1 024 messages (untimed), then times only
  decode + book update of that buffer (`apps/e2/e2_book.cpp`).
- **Warm-up:** the first 1 000 000 messages (pre-market, directory messages, first
  allocations) are processed but not measured.
- **Two timing methods in one pass (`--mode both`).** Full batches alternate:
  even batches are timed as one interval per 1 024 messages (**throughput**, ns/msg after
  dividing by 1 024; the E0 measurement floor is amortized and the CPU can overlap
  consecutive messages, as in production); odd batches are timed one interval per message
  (**per-message latency distribution**; every sample includes the E0 floor of 21.1 ns at
  p50, and the `lfence`/`rdtscp` stamps stop the CPU from overlapping messages). Each method
  therefore sees half of the day, spread evenly over it (~140.6 M messages each).
- **Counters:** one `perf_event_open` group per method, enabled only inside that method's
  timed batches: cycles, instructions, ref-cycles, branch misses, cache misses (LLC), dTLB
  load misses, page faults. Divided by measured messages. Per-message counters include the
  instrumentation itself (stamp pair + histogram record, ~97 cycles per message from E0).
- **Heap allocations:** an `operator new` counting shim (`apps/e2/alloc_count.cpp`) counts
  allocations inside the timed region only.
- **Runs:** N = 10 per book, interleaved in rotating order (session A: `ref, fast-fib`, then
  `fast-fib, ref`, ...), no discarded warm-up runs (each run is a full pass of the day and
  already has its own 1 M-message warm-up). The book thread is pinned to CPU 2; `pigz` is
  started off that core (allowed CPUs 0-1, 3-5, 7: not CPU 2 and not its SMT sibling CPU 6)
  by `ChildAffinityScope` (`lle/core/cpu.hpp`).
- **Two sessions.** Session A: `ref` vs `fast-fib`, N = 10 each, interleaved. Session B:
  `fast` (identity hash), N = 10, run afterwards, because a full-day pass of `fast` takes far
  longer than the others (see "Where the hypotheses were wrong", H3).
- **Aborted first attempt (not used).** Run `results/E2-book/20261004T102616Z` was stopped
  after 2 of 30 runs: `pigz` had inherited the book thread's pinning and ran on CPU 2,
  time-sliced with the measured code. Fixed in `7e2a7a2`; no number from that run is used.
  Median across runs with 95% bootstrap CI; speedups are ratios of medians with their own
  bootstrap CI (`scripts/analyse.py compare`).
- **Correctness of what was timed:** `tools/diff_books` replays the same day through the
  reference book and the optimized book side by side and compares the top of book after
  every message and the full depth periodically.

## Results

All numbers: median across N = 10 runs, [95% bootstrap CI]; ns per message. Full table:
[`docs/results/E2/summary.md`](../results/E2/summary.md); plot:
[`percentiles.png`](../results/E2/percentiles.png). Each method covers ~140.6 M messages per run.

**Throughput** (batch of 1 024 messages, ns per message):

| book | mean | p50 | p99 | p99.9 |
|---|---|---|---|---|
| `ref` | 821.0 [816.6, 837.0] | 831.1 [826.7, 833.3] | 1209 [1209, 1387] | 1569 [1564, 2738] |
| `fast-fib` | **311.0** [310.4, 320.0] | 313.3 [313.3, 315.6] | 461.1 [457.8, 517.8] | 655.6 [631.1, 1449] |
| speedup (ratio of medians) | **2.64×** [2.57, 2.69] | 2.65× [2.63, 2.66] | 2.62× [2.34, 3.01] | 2.39× [1.08, 4.17] |

Mean throughput: 821 → 311 ns/msg is **1.22 M → 3.22 M messages/s** on one core at 1.6 GHz.

**Per-message latency** (one stamp pair per message; includes the 21.1 ns E0 floor):

| book | p50 | p99 | p99.9 | max |
|---|---|---|---|---|
| `ref` | 766.1 [763.9, 772.8] | 2471 [2453, 2577] | 3777 [3288, 6915] | 94.0 ms [93.6, 95.0] |
| `fast-fib` | **301.7** [301.7, 305.0] | **1199** [1191, 1271] | **1715** [1706, 2719] | 5.9 ms [0.55, 6.0] |
| ratio | 2.54× [2.51, 2.55] | 2.06× [1.93, 2.15] | 2.20× [1.32, 3.90] | |

All differences significant (Mann-Whitney, p < 0.006). Run-to-run spread is small: the
fast-fib batch p50 CI is 313.3–315.6 ns.

**Correctness of what was timed.** `tools/diff_books` replayed the full day through `ref`
and `fast-fib` side by side: **282 229 684 messages, top of book compared after 276 789 789
of them, full depth compared 2 768 times (every 100 000), all agree**; 0 unknown refs,
0 live orders at the end ([`diff_fullday_fast-fib.json`](../results/E2/diff_fullday_fast-fib.json)).
**Session B: the identity-hash `fast` book** (N = 10 full days, run after session A, so this is
a cross-session comparison, not an interleaved one; see the caveat below the table):

| book | throughput mean | throughput p50 | per-msg p50 | per-msg p99 | per-msg p99.9 | instr/msg | dTLB misses/msg |
|---|---|---|---|---|---|---|---|
| `fast-fib` (session A) | **311.0** [310.4, 320.0] | 313.3 [313.3, 315.6] | 301.7 [301.7, 305.0] | **1 199** [1 191, 1 271] | 1 715 [1 706, 2 719] | **301** | 1.55 |
| `fast` identity (session B) | 4 233 [4 208, 4 435] | 560.0 [554.4, 604.4] | **239.4** [236.1, 261.7] | 180 337 [179 768, 184 319] | 397 084 [394 808, 418 702] | 21 081 | **0.62** |

Full table: [`sessionB-summary.md`](../results/E2/sessionB-summary.md), counters
[`sessionB-counters.csv`](../results/E2/sessionB-counters.csv). The identity hash is faster on
the *typical* message (per-message p50 239 vs 302 ns, 2.5× fewer dTLB misses: consecutive refs
share pages) but **13.6× slower on average** and **150× worse at p99**, because it executes
**70× more instructions per message** (21 081 vs 301): the probe loop of H3 below. Caveat: the
two sessions ran at different times and the core clock differed (1.585 GHz in session B vs
1.465 GHz in session A, cycles / time), so only differences far larger than that 8% are claimed;
13.6× and 70× instructions (a clock-independent count) are.

The full-day differential of the identity-hash `fast` book has not been run yet (it shares
all code with `fast-fib` except the hash function; both run in the unit and property tests).

## Counter evidence

Per message, timed region only, throughput method (median [95% CI]; full table
[`counters.csv`](../results/E2/counters.csv)):

| book | cycles | instructions | IPC | LLC misses | dTLB load misses | branch misses | heap allocations | page faults |
|---|---|---|---|---|---|---|---|---|
| `ref` | 1319 [1312, 1344] | 425.3 | 0.322 | 29.9 [29.8, 30.8] | 5.25 | 5.29 | **1.187** | 2.6e-4 |
| `fast-fib` | 505.3 [504.1, 519.8] | 301.0 | 0.596 | 16.4 [16.3, 16.7] | 1.55 | 4.15 | **7.1e-5** | 3.3e-5 |
| ratio | 2.61× | 1.41× | 1.85× | 1.82× | 3.39× | 1.28× | ~17 000× | 8× |

What the counters say:

- **The speedup is mostly from stalling less, not from doing less.** The optimized book
  retires 1.41× fewer instructions but takes 2.61× fewer cycles: IPC goes from 0.32 to 0.60.
  At 0.32 IPC the reference book's core is idle most of the time, waiting on memory.
- **Fewer trips to DRAM and fewer page walks.** LLC misses per message drop 1.82×
  (29.9 → 16.4; the generic `cache-misses` event also counts misses caused by the hardware
  prefetchers, so the absolute count is higher than the demand misses alone) and dTLB load
  misses 3.39× (5.25 → 1.55). The reference book's map nodes, list nodes and hash nodes are
  separate heap objects spread over the heap, each access to them a likely cache and TLB miss;
  the optimized book keeps orders in one pool of 32-byte nodes, levels in one contiguous
  vector per side, and the ref map in one flat array.
- **Allocation is gone from the steady state, almost.** 1.19 heap allocations per message
  for `ref` (on the 52% of messages that are adds or replaces, ≈ 2.3 each: a list node and a
  hash node per order, plus a map node whenever the price level is new), vs 9 939 allocations
  per run for `fast-fib` (7.1e-5 per message), see H2 below.
- Branch misses barely change (5.3 → 4.2 per message): branch prediction is not what
  separates the two books.
- The core ran at 1.54 GHz (`ref`) and 1.47 GHz (`fast-fib`) on average (cycles / time),
  slightly below the 1.6 GHz nominal; both numbers are in `counters.csv`. Not explained
  yet; it does not change the comparison (the cycle ratio, 2.61×, matches the time ratio).

## Where the hypotheses were wrong

- **H1 (reference book: ~3 allocations per add, a few hundred ns per message): partly
  wrong.** Measured: 1.19 allocations per message, ≈ 2.3 per add or replace, not 3; the third
  allocation (a `std::map` node) happens only when an order opens a new price level, and most
  orders join an existing one. And it is slower than predicted: 821 ns per message mean, with
  ~30 LLC misses and 5.2 dTLB misses per message: closer to "one DRAM trip after another"
  than "a few hundred ns". The mechanism (allocation + pointer chasing) is supported by the
  counters (IPC 0.32).
- **H2 (optimized book: 0 allocations after warm-up, ≥ 2× faster): speedup right, "0" wrong.**
  2.64× faster in throughput [2.57, 2.69]. But the timed region still made **9 939 heap
  allocations per run** (identical in all 10 runs, so deterministic). The likely source is the
  per-side level vectors: a symbol first seen after the 1 M-message warm-up reserves its
  level vectors on first use, and a side that grows past its reserved capacity reallocates.
  Not yet verified (planned: count allocations per call site). 7.1e-5 per message does not move
  the average, but each one is a `malloc` (sometimes a page fault) inside one message, so it
  can show up in the tail.
- **H3 (identity hash beats Fibonacci hash): wrong, and in the most expensive way.** The
  identity hash (`ref mod 2^22`) was so slow on the full day that a `fast` run had not finished
  after 18 minutes, where a `ref` run took about 9.5 minutes under the same conditions (in the
  aborted first attempt). Session B then timed it (N = 10 full days): mean **4 233 ns/msg vs
  311 for `fast-fib` (13.6×)**, per-message p99 **180 µs vs 1.2 µs**, and **21 081 instructions per
  message vs 301**. Yet its per-message p50 is *lower* (239 vs 302 ns) with 2.5× fewer dTLB
  misses, so the locality half of H3 was right for the typical message. `perf record` on that run (`docs/results/E2/identity-profile/`) shows where the
  time went: `add` 34%, `on_delete` 26%, `main` 25% of samples (other
  handlers are inlined into it; its hottest instructions are again a probe loop), and inside `add` **97% of the samples sit on the 5
  instructions of the linear-probe loop** (`cmp (%rax),%rcx; add $1,%rdx; shl $4,%rax;
  add %rdi,%rax; jne`). Mechanism: order refs are nearly sequential, so with the identity
  hash they fill *consecutive* slots. Once refs pass 2^22 the slot index wraps around and
  new refs land on slots still occupied by long-lived orders from 2^22 refs earlier; linear
  probing then walks to the end of that occupied run, and every insertion makes the run
  longer (primary clustering). The Fibonacci hash scatters consecutive refs, so runs stay
  short. The 1 M-message smoke slice never reached 2^22 refs, which is why the smoke test did
  not show it. The locality argument behind H3 (consecutive refs share cache lines) was
  real; it was outweighed by clustering. Lesson: a hash function has to be evaluated on the
  real key sequence over the full day, not on a prefix.
- **H4 (the tail improves more than the median): wrong.** Per-message ratios are 2.54× at
  p50, 2.06× at p99, 2.20× at p99.9 (wide CI); throughput ratios are flat to p99. The slowest
  messages get faster by *less* than the typical one. The optimized book removed allocation
  and pointer chasing, which cost every message, but whatever makes the p99 message slow
  (likely cold cache lines for rarely touched symbols and levels, timer interrupts, and the
  remaining allocations) is shared by both books. Only the extreme maximum differs a lot:
  the reference book's slowest single message took **94 ms**, in every run (CI 93.6–95.0 ms),
  i.e. a deterministic event; a full rehash of the `std::unordered_map` holding ~2 M live
  orders fits that pattern but is not verified. Explaining a p99.9 event with Intel PT is
  part of E2 part 2.

## Conclusion

Changing only the data layout (one preallocated pool of 32-byte order nodes with intrusive
FIFO lists, one sorted level vector per side with the best price at the back, and a flat
open-addressing ref map) makes full-day book building **2.64× faster** (821 → 311 ns per
message, 1.2 M → 3.2 M messages/s on one 1.6 GHz core), with per-message p50 766 → 302 ns
and p99 2.47 → 1.20 µs, and **matches the reference book on all 282 M messages**. The gain
comes from memory behaviour, not from executing less code: 1.4× fewer instructions but 1.8×
fewer LLC misses, 3.4× fewer dTLB misses and IPC nearly doubled. The hash function of the
ref map decides whether the design works at all: the locality-preserving identity hash,
fine on a 1 M-message slice, degrades through primary clustering once order refs wrap
the table on the full day: 13.6× slower on average and 150× worse at p99 than the Fibonacci hash,
with 70× more instructions per message, even though its median message is faster.

## Threats to validity

- **Laptop CPU at 1.6 GHz, turbo off.** Absolute ns do not transfer to a tuned server; cycle
  and counter ratios transfer better. The measured CPU is not isolated (`isolcpus` /
  `nohz_full` not set), so timer interrupts land in timed regions; they mostly affect the
  per-message tail.
- **`pigz` shares the machine.** It runs on other physical cores, but it still shares the L3
  cache and memory bandwidth with the book thread. All books run under the same conditions,
  interleaved, so the comparison is fair; absolute numbers include this background load.
- **Allocation shim overhead.** The `operator new` replacement adds one thread-local increment
  per allocation. It slows the reference book slightly (it allocates) and not the optimized
  book (it does not), so it inflates the measured speedup by at most the cost of that
  increment per allocation.
- **Per-message numbers include the instrumentation.** Each per-message sample includes the
  E0 measurement floor (21.1 ns at p50) and the fences stop the CPU from overlapping
  messages; per-message counters also count the stamp pair and the histogram record.
  Throughput (batch) numbers amortize this over 1 024 messages.
- **Identity hash.** It is bad on the real day (H3), not only on adversarial keys. Its timing
  (session B) was not interleaved with session A and ran at a ~8% higher core clock; the
  reported differences (13.6× mean, 70× instructions) are far larger than that. The
  reported optimized book uses the Fibonacci hash; an adversarial stream for it is planned
  (`plan.md` P2).
- **Core clock below nominal.** Average core clock in the timed region was 1.47–1.54 GHz
  (cycles / time), not 1.6 GHz; cause not identified. Cycle counts are reported alongside ns.
- **Background load.** The machine ran a desktop session (load average 2.1 at the start).
- **Level-vector growth.** A side's level vector grows when it holds more levels than ever
  before; such growth inside the timed region is counted in the allocation column.
- **One day, one machine, one compiler.** Results are for 2019-07-30, this CPU, GCC 15.2.

## Reproduce

```bash
sudo scripts/tune_machine.sh apply
cmake --preset release && cmake --build --preset release
scripts/fetch_itch.sh 07302019            # see data/README.md; checksum-verified

# Session A (ref vs fast-fib); session B is the same command with --param book=fast
python3 scripts/run_bench.py --name E2-book --build build/release --runs 10 --warmup-runs 0 \
  --cmd "{bin}/apps/e2_book --book {book} --mode both --input data/07302019.NASDAQ_ITCH50.gz --cpu 2 --out {out} --out-permsg {out}_permsg" \
  --param book=ref,fast-fib
python3 scripts/e2_split.py results/E2-book/latest
python3 scripts/analyse.py summary results/E2-book/latest
python3 scripts/analyse.py compare results/E2-book/latest "book=ref__method=batch" "book=fast-fib__method=batch" --metric mean
# Session B (results/E2-book/20261004T131348Z here)
python3 scripts/run_bench.py --name E2-book --build build/release --runs 10 --warmup-runs 0 \
  --cmd "{bin}/apps/e2_book --book {book} --mode both --input data/07302019.NASDAQ_ITCH50.gz --cpu 2 --out {out} --out-permsg {out}_permsg" \
  --param book=fast
python3 scripts/e2_split.py results/E2-book/latest && python3 scripts/analyse.py summary results/E2-book/latest

# Full-day correctness of the optimized book against the reference book
./build/release/tools/diff_books --input data/07302019.NASDAQ_ITCH50.gz --book fast --out results/E2/diff_fullday_fast.json
./build/release/tools/diff_books --input data/07302019.NASDAQ_ITCH50.gz --book fast-fib --out results/E2/diff_fullday_fast-fib.json

sudo scripts/tune_machine.sh restore
```
