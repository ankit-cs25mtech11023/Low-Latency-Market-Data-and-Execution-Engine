# The order books: what they do and why they are built this way

This page explains the two book implementations in `include/lle/book/`. Speed numbers are
**not** here: they are in [E2](experiments/E2-orderbook.md), next to the result files that
back them. Workload facts quoted below come from [E1](experiments/E1-workload.md).

## 1. What a feed-side order book does

Nasdaq TotalView-ITCH is a **market-by-order** feed: it reports every individual resting
order, not just the totals per price. A consumer rebuilds the book itself:

| ITCH message | book action |
|---|---|
| `A` / `F` add order | new order `ref` at `price` for `qty` on a side of symbol `locate`; joins the back of the queue at that price |
| `E` / `C` executed | `qty` of order `ref` traded; remove the order if nothing is left |
| `X` partial cancel | reduce order `ref` by `qty` |
| `D` delete | remove order `ref` |
| `U` replace | remove `old_ref`, add `new_ref` with a new price/qty (it loses its queue position) |

Only `A F E C X D U` change the book (98.07% of the 2019-07-30 stream). The book here does
**book building only**: it never matches orders. Matching happens at the exchange; the feed
tells us the result. (Matching exists in this project only inside the exchange emulator.)

Every message except an add names the order **only by its ref**, so every message needs a
fast `ref → order` lookup. And every message changes one price level, usually near the best
price. Those two facts drive the whole design.

## 2. The reference book (`reference_book.hpp`): correct first

Built from standard containers, written to be obviously correct, and used as the oracle in
every differential test:

```
per symbol, per side:  std::map<Price, Level>          (red-black tree, one heap node per level)
per level:             std::list<Order>                (one heap node per order, FIFO)
global:                std::unordered_map<Ref, iterator into that list>   (one heap node per entry)
```

Costs, by mechanism:

- **Heap allocation on almost every message.** An add allocates a list node and a hash node,
  plus a tree node for a new price. A delete frees them. `malloc`/`free` are tens of
  instructions each in the fast case, can take locks, and sometimes call the kernel.
- **Pointer chasing.** A delete goes hash bucket → hash node → list node → (tree node to
  update the level total). Each hop is a load whose address depends on the previous load, so
  the CPU cannot start the next one early. If the node is not in cache, each hop costs a
  full cache miss (tens to hundreds of ns).
- **Scattered memory.** Nodes allocated at different times sit on different cache lines and
  pages, so even a small book touches many lines and many TLB entries.

## 3. The optimized book (`fast_book.hpp`): same behaviour, different memory layout

```
pool_      : OrderNode[2^22]                 one array, 32 B per node, allocated once
refs_      : RefMap  (2^22 slots, 16 B each) one array, allocated once
per symbol : bids  std::vector<Level>        sorted worst → best (best at the BACK)
             asks  std::vector<Level>
Level (24 B): price, orders, total qty, head, tail   (head/tail = pool indices)
OrderNode (32 B): ref, qty, next, prev, price, locate, side
```

### 3.1 Order pool + 32-bit indices (layer L1)

All orders live in one preallocated array. A freed node goes on a free list and is reused,
so **after construction no order operation calls the allocator**. Nodes are addressed by a
32-bit index instead of a 64-bit pointer: that keeps `OrderNode` at 32 bytes (two per cache
line), enforced by `static_assert`. Size: E1's peak was 1.96 M live orders, so 2^22 = 4.19 M
slots leave 2× headroom.

### 3.2 Intrusive FIFO per level (L1)

The `next`/`prev` links are fields **inside** the order node ("intrusive"), so a level only
stores `head` (oldest order, first to trade) and `tail` (newest). Append at the tail and
unlink from the middle are O(1) with no separate list node to allocate or chase.

### 3.3 Sorted level vector, best at the back (L2a)

Each side keeps its levels in one contiguous `std::vector`, sorted from the worst price to
the best, so **the best level is the last element**. Why that order:

- **Where events happen.** E1: executions are 100% at the best; 91% of adds and deletes are
  within 16 ticks of it. So `find_level` scans backwards from the end for up to 16 levels
  (contiguous 24-byte entries, already in cache, predictable branches) and only falls back
  to binary search for far prices.
- **Cheap insert/erase near the best.** Inserting or erasing level *i* shifts every element
  after *i*. With the best at the back, a change near the best shifts only a few elements.
  (With the best at the front, every change at the touch would shift the whole side.)
- Levels are stored **by value**, so a level's index changes when another level is inserted
  or erased. Orders therefore store their price, not a level index, and look the level up
  again when needed (cheap, because it is usually near the back).

This "best at the back" vector book is the design David Gross (Optiver) presented in "When
Nanoseconds Matter", CppCon 2024. The vector grows (doubling) only when a side holds more
levels than ever before, so growth stops after warm-up; E2 counts allocations to check it.

### 3.4 Open-addressing ref map (L3, `ref_map.hpp`)

`ref → pool index` in one flat array of 16-byte slots (4 per cache line):

- **Linear probing:** on a collision, try the next slot, usually in the same cache line.
- **Backward-shift delete:** when an entry is removed, later entries of the same probe run
  move back into the hole. No tombstones, so lookups do not get slower over a long day.
- **Fixed capacity 2^22**, sized for load ≤ 0.5 at E1's peak, so there is never a rehash on
  the hot path.
- **Why not a plain array indexed by ref?** E1: refs span 0…260 M but at most 1.96 M are live
  at once. A direct array would be about 130× larger than needed, mostly empty.

**The hash function is an experiment variable.** ITCH assigns refs nearly sequentially and
most orders die young, so the live refs are a sliding window of mostly consecutive numbers.

- **Identity** (`ref mod 2^22`, `FastBook`): consecutive refs go to consecutive slots, so
  recent orders share cache lines and 4 KiB pages.
- **Fibonacci** (multiply by 2^64/φ, keep the top bits, `FastBookFib`): the textbook choice;
  it spreads keys evenly, which here scatters recent orders over the whole 64 MiB table.

Identity is only safe because exchange-assigned refs are not adversarial: keys that are all
multiples of 2^22 would create one long probe run. E2 measures what each choice costs.

## 4. Walk-through: one delete message

1. `refs_.erase(ref)` → hash the ref, probe to its slot, read the pool index, backward-shift.
2. `pool_[idx]` → the node: side, price, qty, `next`/`prev`.
3. `find_level(side, price)` → scan back from the best level of that side.
4. Unlink the node from the level's FIFO (fix `prev.next`, `next.prev`, or `head`/`tail`),
   subtract its qty from `total`, decrement `orders`; erase the level if it is now empty.
5. Push the node on the free list.

No allocation, no free, and in the common case every touched line (ref slot, node, the last
few levels) is already in cache.

## 5. How correctness is checked

- **Differential tests** (`tests/unit/test_fast_book.cpp`): the
  optimized book and the reference book process the same stream; the top of book is compared
  after **every** message and full depth periodically. Synthetic streams cover 8 modes
  (realistic, deep queues, wide prices, replace chains, crossing, many symbols, error paths, mixed) × 3 seeds ×
  both hash functions.
- **Full real day** (`tools/diff_books`): the whole 2019-07-30 file through both books side
  by side (result in E2).
- `RefMap` is tested against `std::unordered_map` on random insert/erase sequences, for both
  hashes; the tests also run under ASan and UBSan.

## 6. What is planned next (see `plan.md` P2)

Tick ladder with a two-level bitmap for best-price search (L2), `mlockall` + prefault (L4),
2 MiB pages (L5), selectable layers with leave-one-out builds, top-down analysis per layer,
Intel PT for one p99.9 event, and an adversarial input stream.

## 7. Questions to be ready for

- *Why is the best level at the back of the vector?* §3.3.
- *Why 32-bit indices instead of pointers?* Half-size nodes (two per line), and indices stay
  valid if the pool were ever moved; the pool size (2^22) fits easily.
- *Why not `std::unordered_map`?* One heap node per entry and a pointer hop per lookup (§2).
- *What breaks the identity hash?* Keys clustered on multiples of the capacity (§3.4).
- *Is a sorted vector O(n) insert a problem?* Only if inserts happen far from the best; E1
  shows they mostly do not, and E2 measures it on the real day.
- *Why is this book-building, not matching?* The feed already reports the exchange's matching
  results (§1).
