# Concurrency: why the SPSC ring is correct

This explains the memory-ordering argument behind `include/lle/concurrency/spsc.hpp`, the
queue every pipeline hop uses (E3 measures it, E4 builds on it). It is written for someone
new to the C++ memory model; interviewers ask exactly these questions.

## 1. The problem

Two threads on two cores share a ring of slots. The producer writes a message into a slot,
then tells the consumer "slot `t` is ready" by advancing `tail`. The consumer reads the slot,
then tells the producer "slot `h` is free again" by advancing `head`.

Two things can go wrong without the right ordering:

1. **Reading a slot too early.** The consumer sees the new `tail` but still reads the *old*
   bytes of the slot (the compiler or the CPU made the `tail` write visible before the slot
   write). It processes garbage.
2. **Overwriting a slot too early.** The producer sees the new `head` and overwrites a slot
   the consumer has not finished reading.

In C++ terms, both are **data races** on the slot (a plain, non-atomic object written by one
thread and accessed by another without a happens-before relation). A data race is undefined
behaviour, not just "a stale value".

## 2. The rule that fixes it: release/acquire

C++ gives a happens-before edge between two threads when:

- thread A does a **release** store to an atomic `x`, and
- thread B does an **acquire** load of `x` that **reads the value A stored** (or a later one
  in `x`'s modification order).

Then everything A did *before* the release store happens-before everything B does *after*
the acquire load. Plain (non-atomic) writes included.

## 3. Applying it to the ring

Indices are 64-bit counters that only grow; the slot is `index & (Capacity - 1)`.

**Producer, `try_push(v)`:**

```
t = tail (relaxed: only the producer writes tail)
if t - head_seen == Capacity: refresh head_seen = head.load(acquire); still full -> return false
buf[t & mask] = v                 // (P1) plain write of the slot
tail.store(t + 1, release)        // (P2) publish
```

**Consumer, `try_pop(out)`:**

```
h = head (relaxed: only the consumer writes head)
if h == tail_seen: refresh tail_seen = tail.load(acquire); still empty -> return false
out = buf[h & mask]               // (C1) plain read of the slot
head.store(h + 1, release)        // (C2) free the slot
```

**Claim 1: the consumer never reads a slot before the producer's write to it is visible.**
The consumer reads slot `h` only after an acquire load of `tail` returned a value `> h`. That
value was written by a release store `tail.store(t + 1)` with `t >= h`. The producer wrote
slot `h` (P1) before that store in its own program order (slots are written in index order,
each before its own publish, and `h <= t`). Sequenced-before + release → acquire gives
(P1 for slot `h`) happens-before (C1): no race.

**Claim 2: the producer never overwrites a slot the consumer is still reading.** The producer
writes slot `t` only after it saw `head > t - Capacity`, i.e. the consumer's release store
`head.store(h + 1)` with `h = t - Capacity` (the previous user of the same slot). That store
came after the consumer's (C1) read of that slot. Acquire load of `head` → so (C1 of the old
message) happens-before (P1 of the new message). No race.

**Claim 3: the cached copies do not break this.** `head_seen` / `tail_seen` are private to one
thread and only ever lag behind the real index. A stale copy makes the ring look *more* full
(producer) or *more* empty (consumer) than it is, so the side refreshes it with an acquire
load before giving up. It can never make the producer write into a slot that is not free, or
the consumer read one that is not published: every decision to touch a slot is based on a
value that was obtained by an acquire load of the other side's index at some point, which is
all Claims 1 and 2 need.

**Why the own-index load can be relaxed:** only one thread ever writes `tail`, and it is that
thread reading it. A thread always sees its own latest write (sequenced-before).

## 4. What it costs on x86-64 (and why `seq_cst` is slower)

x86 is TSO (total store order): the hardware already never reorders a load with an older
load, or a store with an older store. So:

| C++ operation | x86-64 instruction | extra cost |
|---|---|---|
| `load(acquire)` | `mov` | none (only stops the *compiler* reordering) |
| `store(release)` | `mov` | none |
| `load(seq_cst)` | `mov` | none |
| `store(seq_cst)` | `xchg` (implicitly `lock`ed) | waits for the store buffer to drain: a full barrier |

The only reordering x86 does is **store → later load** (a store sits in the store buffer while
a later load to another address already reads the cache). `seq_cst` forbids that too, so the
compiler emits `xchg` (or `mov` + `mfence`) for every `seq_cst` store. In the ring that is one
full barrier per push and one per pop, for an ordering guarantee the protocol does not need.
E3 measures the difference (`seqcst` vs `acqrel`).

On ARM (v1.1, E7) acquire/release are *not* free: they become `ldar`/`stlr` (or `ldapr`), and
a plain `ldr`/`str` would be wrong. The C++ source stays the same; only the codegen changes.

## 5. Where the time actually goes: cache-line transfers

Even with free ordering instructions, passing a message from core A to core B moves cache
lines between the cores' private caches (MESI coherence through the shared L3):

- the **slot** line (written by A, read by B),
- the line holding **`tail`** (written by A, read by B),
- the line holding **`head`** (written by B, read by A).

Each transfer is a round trip through the L3 ring (E0 measured the one-way cost of moving a
line between cores as part of the TSC-skew ping-pong). The two optimizations in the ring
target these transfers:

- **Cached remote index:** without it, every push reads `head` and every pop reads `tail`,
  so both index lines bounce on every message. With it, each side re-reads the other's index
  only when the ring looks full/empty, i.e. once per "catch-up" instead of once per message.
- **Padding (none / 64 / 128 B):** if `head` and `tail` share a line, every index write by one
  core invalidates the line the other core is using (false sharing: different variables, same
  line). 64 B separates the lines; 128 B also defeats Intel's adjacent-line prefetcher, which
  fetches lines in 128-byte pairs and can drag the neighbouring line along.

## 6. The mutex baselines

- `MutexCvQueue`: lock, check, wait on a condition variable (futex) if empty/full. Every message
  costs a lock and unlock on both sides (atomic read-modify-write instructions on the mutex
  word, whose line bounces between the cores), and a sleeping side costs a `futex` wake system
  call plus a scheduler wake-up before it runs again.
- `MutexSpinQueue`: same lock, but nobody sleeps; both threads poll. Removes the wake-up,
  keeps the lock traffic.

Both are correct by construction (the mutex gives happens-before between unlock and the next
lock); they exist to show what the lock-free ring removes.

## 7. How this is checked

- **Unit tests** (`tests/unit/test_spsc.cpp`): empty, full (exactly `Capacity` slots usable),
  wraparound at every offset, FIFO order, for every variant.
- **Two-thread stress test with random delays** in the same file: every message carries a
  sequence number and a checksum derived from it; the consumer checks order and content. Run
  under the `tsan` preset, ThreadSanitizer instruments every memory access and reports any
  pair of conflicting accesses without a happens-before relation, i.e. it checks Claims 1
  and 2 on the interleavings that actually occurred.
- **Limits:** a test only explores interleavings that happened. A model checker (GenMC or
  Relacy) explores all of them under the C++ memory model; that is planned with a 2-day
  timebox (`plan.md` P3), together with showing that it *rejects* a weakened ordering
  (e.g. a relaxed `tail` store).
