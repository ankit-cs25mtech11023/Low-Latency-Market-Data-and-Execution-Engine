# E2: Order-Book Data Structures on a Real ITCH Day

> Status: **partial (part 1 of E2).** This part compares the reference book (standard
> containers) with the optimized book (pool + intrusive FIFO + sorted level vector +
> open-addressing ref map) and isolates one layer, the ref-map hash function. Still to come
> (see `plan.md` P2): per-layer ablation and leave-one-out builds, the tick-ladder + bitmap
> variant, `mlockall`/prefault (L4) and 2 MiB pages (L5, both need memlock/hugepage limits
> set with sudo), top-down analysis, Intel PT for a p99.9 event, and llvm-mca.

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

(filled in from `env.json` after the runs)

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

(filled in after the runs)

## Counter evidence

(filled in after the runs)

## Where the hypotheses were wrong

(filled in after the runs)

## Conclusion

(filled in after the runs)

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
- **Identity hash and adversarial keys.** The identity hash is good because exchange refs are
  nearly sequential. Keys clustered on multiples of 2^22 would make probe runs long; this is
  not measured here (an adversarial stream is planned, `plan.md` P2).
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

# Full-day correctness of the optimized book against the reference book
./build/release/tools/diff_books --input data/07302019.NASDAQ_ITCH50.gz --book fast --out results/E2/diff_fullday_fast.json
./build/release/tools/diff_books --input data/07302019.NASDAQ_ITCH50.gz --book fast-fib --out results/E2/diff_fullday_fast-fib.json

sudo scripts/tune_machine.sh restore
```
