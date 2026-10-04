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

(filled in after the runs)
