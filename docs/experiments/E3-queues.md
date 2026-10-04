# E3: Passing Messages Between Cores (SPSC Queues)

> Status: **partial (part 1 of E3).** This part compares five queue variants, three index
> paddings and three thread placements. Still to come (see `plan.md` P3): batch publish,
> the seqlock, the timeboxed model check (GenMC/Relacy), and `perf c2c` HITM counts to
> confirm the false-sharing mechanism directly.

## Question

A trading pipeline hands every market-data message from one thread to another at least once.
What does one hand-off cost, what limits the rate, and **which hardware effect** does each
queue design choice remove?

## Hypotheses (expected mechanism), written before any E3 run

Prior from E0: moving one cache line between cores and getting an answer back (ping-pong,
RTT/2) took **~188 ns between separate physical cores** and **~77 ns between SMT siblings**
on this CPU at 1.6 GHz. A hand-off needs at least the slot line and the `tail` line to reach
the consumer.

- **H1. Blocking (mutex + condition variable) costs microseconds per hand-off.** At one
  message per µs the consumer often finds the queue empty and goes to sleep in `futex`; each
  wake-up is a system call on the producer plus a scheduler wake-up on the consumer. We expect
  one-way latency p50 **in the µs range, ≥ 5× the lock-free ring**, at least one context
  switch per few messages on the consumer, and consumer CPU% well below 100%.
- **H2. Mutex + spinning removes the wake-up but not the lock.** Latency drops to a few hundred
  ns, but every attempt by either thread writes the mutex word, so its line bounces between
  the cores; throughput stays far below the lock-free ring.
- **H3. `seq_cst` vs acquire/release: same latency, lower throughput.** The only difference on
  x86 is `xchg` instead of `mov` for the index stores (a store-buffer drain, tens of cycles).
  At low rate that is small next to a ~190 ns line transfer, so p50 latency should be within
  a few percent. In throughput mode the barrier sits on every message, so `seq_cst` should be
  measurably slower.
- **H4. The cached remote index helps throughput, not low-rate latency.** At 1 msg/µs the
  ring is nearly always empty, so the consumer must re-read `tail` for every message anyway:
  no latency change. In throughput mode the producer reads `head` only when the ring looks
  full and the consumer reads `tail` only when it looks empty, so the index lines move about
  once per batch instead of once per message: we expect the **largest single throughput gain
  of the sweep (≥ 2×)** and fewer LLC misses per message.
- **H5. Padding matters most when indices are read every message.** With `head` and `tail` on
  one line (pad 0), each index write invalidates the line the other core reads (false
  sharing): throughput worst. 64 B vs 128 B: a small effect from the adjacent-line
  prefetcher, possibly not resolvable.
- **H6. Placement: SMT siblings hand off faster, separate cores run faster.** Siblings share
  L1/L2, so the hand-off avoids the L3 round trip (E0: ~77 vs ~188 ns); but two busy-spinning
  threads share one core's execution resources, so throughput may drop. Unpinned threads
  should show worse tails (migrations, sharing a core with other tasks).

## Setup

From `docs/results/E3/*-env.json`: the same tuned machine as E0–E2. Intel Core i5-8250U
(4 cores / 8 threads; SMT siblings n and n+4), `performance` governor on all CPUs, **turbo
off** (1.6 GHz; the counters show 1.59 GHz in every run), NMI watchdog off, Linux 6.16.8,
**no `isolcpus` / `nohz_full`**, GCC 15.2 `-O2 -DNDEBUG`. Built from git `a495e87`
(variants) and `f14b9b4` (padding, placement; differs only in documentation), clean trees.
Sessions ran 12:44–13:00 UTC on 2026-10-04; load average 1.3–2.2 at their start (desktop
session).

## Variants

| variant | synchronization | index reads | index layout |
|---|---|---|---|
| `mutex-cv` | `std::mutex` + 2 `std::condition_variable` (sleep when empty / full) | under the lock | one struct |
| `mutex-spin` | `std::mutex`, callers poll `try_push`/`try_pop` | under the lock | one struct |
| `seqcst` | atomics, every access `seq_cst` (index store = `xchg`) | other side's index every message | `pad` |
| `acqrel` | release store / acquire load (plain `mov` on x86) | other side's index every message | `pad` |
| `acqrel-cached` | as `acqrel` | other side's index only when the ring looks full / empty | `pad` |

All: 1 024 slots of 64-byte messages (one cache line each), slot array aligned to 128 B,
64-bit ever-growing indices. Polling loops spin without `PAUSE`. Code:
`include/lle/concurrency/spsc.hpp`; correctness argument: [`docs/concurrency.md`](../concurrency.md).

## Method

- **Driver:** `apps/e3/e3_queue.cpp`, one producer and one consumer thread, one variant and one
  mode per process run.
- **Latency mode (open loop):** the producer sends one message every 1 000 ns on a fixed
  schedule; each message carries its intended send time; the consumer records (TSC after the
  pop) − (intended time). A late producer does not shift the schedule, so queueing delay is
  measured, not hidden (no coordinated omission). 100 000 warm-up messages, then 2 000 000
  measured. Cross-core subtraction is valid within E0's skew bound (±2.2 ns).
- **Throughput mode (closed loop):** the producer pushes as fast as the queue accepts; the
  consumer times each block of 1 024 pops; reported as ns per message. 100 000 warm-up, then
  20 000 000 measured.
- **Counters:** one `perf_event_open` group per thread, enabled only over the measured
  messages: cycles, instructions, LLC misses, L1D load misses, context switches. Per message.
  CPU% per thread = thread CPU time / wall time over the same window.
- **Sweeps, one axis at a time** (master plan §P3), each its own interleaved session with
  N = 10 runs per variant:
  1. variants (pad 64 B, producer CPU 2 → consumer CPU 3),
  2. padding 0 / 64 / 128 B for `acqrel` and `acqrel-cached` (CPUs 2 → 3),
  3. placement 2→3 (separate cores), 2→6 (SMT siblings), unpinned, for the best variant and
     padding of steps 1–2.
- One discarded warm-up pass per variant before the measured runs (`run_bench.py` default).
- Median across runs with 95% bootstrap CI; ratios of medians with their own bootstrap CI.

## Results

Median across N = 10 runs per variant, [95% bootstrap CI]. Latency = one-way, open loop at
1 msg/µs, 2 M measured messages per run. Throughput = mean ns per message in closed loop,
20 M messages per run. Full tables: [`docs/results/E3/`](../results/E3/) (`*-summary.md`,
`*-counters.csv`, `*-percentiles.png`).

### Step 1: variants (pad 64 B, producer CPU 2 → consumer CPU 3)

| variant | latency p50 | p99 | p99.9 | throughput ns/msg | messages/s |
|---|---|---|---|---|---|
| `mutex-cv` | 11 377 [11 128, 12 017] | 153 µs | 892 µs | 326.5 [321.9, 331.5] | 3.1 M |
| `mutex-spin` | 5 706 [5 404, 5 973] | 54.2 µs | 355 µs | 263.8 [255.9, 268.7] | 3.8 M |
| `seqcst` | 312.8 [310.6, 330.6] | 2 111 | 38.3 µs | 175.9 [165.5, 178.5] | 5.7 M |
| `acqrel` | 313.9 [301.7, 330.6] | 1 599 | 39.8 µs | 54.2 [53.5, 55.7] | 18.5 M |
| `acqrel-cached` | **259.4** [241.7, 274.4] | 1 533 | 36.5 µs | **11.0** [10.6, 11.9] | **91 M** |

Ratios of medians [95% CI]:

- Throughput: `mutex-cv` / `acqrel-cached` = **29.6×** [27.3, 30.6]; `seqcst` / `acqrel` =
  **3.24×** [3.04, 3.31]; `acqrel` / `acqrel-cached` = **4.91×** [4.54, 5.13];
  `mutex-cv` / `mutex-spin` = 1.24× [1.21, 1.28].
- Latency p50: `mutex-cv` / `acqrel-cached` = **43.9×** [40.8, 48.7]; `mutex-spin` /
  `acqrel-cached` = 22.0× [20.1, 24.1]; `seqcst` / `acqrel` = 0.996× [0.946, 1.088] (no
  difference); `acqrel` / `acqrel-cached` = 1.21× [1.11, 1.35].

### Step 2: index padding (CPUs 2 → 3)

| variant | pad | latency p50 | throughput ns/msg |
|---|---|---|---|
| `acqrel` | 0 | 326.1 [312.8, 329.4] | 56.5 [53.9, 56.6] |
| `acqrel` | 64 | 327.2 [315.0, 328.3] | 53.9 [52.9, 55.4] |
| `acqrel` | 128 | 317.2 [313.9, 323.9] | **35.3** [34.7, 36.1] |
| `acqrel-cached` | 0 | 346.1 [326.1, 350.6] | 46.1 [39.7, 51.6] |
| `acqrel-cached` | 64 | **246.1** [238.9, 271.7] | **11.1** [10.8, 11.5] |
| `acqrel-cached` | 128 | 284.4 [246.1, 316.1] | 12.3 [11.8, 12.7] |

Throughput ratios: `acqrel` pad 64 / pad 128 = **1.53×** [1.49, 1.57], pad 0 / pad 64 =
1.05× [0.996, 1.07] (not resolved); `acqrel-cached` pad 0 / pad 64 = **4.13×** [3.56, 4.67],
pad 128 / pad 64 = 1.10× [1.05, 1.16]. Latency p50, `acqrel-cached` pad 0 / pad 64 = 1.41×
[1.26, 1.45]. Best configuration: `acqrel-cached`, pad 64.

### Step 3: placement (`acqrel-cached`, pad 64)

| placement | latency p50 | p99 | p99.9 | throughput ns/msg |
|---|---|---|---|---|
| separate cores (2 → 3) | 248.9 [243.3, 262.8] | 2 835 | 44.2 µs | 11.0 [10.6, 11.2] |
| SMT siblings (2 → 6) | **112.2** [112.2, 112.8] | 964 | 36.8 µs | 12.8 [12.7, 12.9] |
| unpinned | 261.7 [248.3, 301.7] | 2 871 | 44.7 µs | 11.2 [10.7, 12.1] |

SMT siblings: latency p50 **2.22× lower** [2.16, 2.38], throughput 1.17× slower [1.14, 1.21].
Unpinned vs pinned 2 → 3: p50 1.05× [0.96, 1.21], p99 1.01× [0.72, 1.59], throughput
1.03× [0.97, 1.11]: no measurable difference.

(Step 1 and step 3 ran in separate sessions; the same configuration, `acqrel-cached` pad 64
on CPUs 2 → 3, measured 259 / 246 / 249 ns p50 and 11.0 / 11.1 / 11.0 ns/msg in the three
sessions, so the sessions agree.)

**Correctness.** Every message carries a sequence number; every run checks it, and a run with
any lost, duplicated or reordered message would have failed (none did). Unit tests and a
two-thread stress test with random delays pass for all variants under ThreadSanitizer
(1 M messages per variant in the default test; a local run with **10^8 messages for each of
the 6 lock-free configurations** passed with 0 ThreadSanitizer reports in 11.7 minutes,
[`tsan_stress_1e8.log`](../results/E3/tsan_stress_1e8.log)).

## Counter evidence

Per message, consumer thread unless marked, timed region only (medians; full tables in
`docs/results/E3/*-counters.csv`). In latency mode both threads spin between messages, so
cycles and instructions per message there measure the 1 µs wait, not the queue; misses and
context switches are still meaningful.

**Throughput mode, step 1 (pad 64, 2 → 3):**

| variant | cycles | instructions | IPC | L1D load misses | context switches |
|---|---|---|---|---|---|
| `mutex-cv` | 506.5 | 210.9 | 0.42 | 2.55 | 3.2e-4 |
| `mutex-spin` | 419.0 | 165.5 | 0.40 | 1.89 | 1.4e-5 |
| `seqcst` | 280.0 | 38.9 | 0.14 | 4.36 | 7e-7 |
| `acqrel` | 86.4 | 46.6 | 0.54 | 2.50 | 2e-7 |
| `acqrel-cached` | **17.6** | 32.2 | **1.83** | **1.06** | 5e-8 |

- **The cached ring is down to about one L1D miss per message: the message itself.** Each
  message is one 64-byte slot line that the producer wrote, so the consumer must miss on it
  once; 1.06 misses per message means index traffic has almost disappeared. Without the
  cached index, 2.50 misses per message: the slot plus the other side's index line on most
  messages. Cycles per message fall 4.9× (86.4 → 17.6) with only 1.4× fewer instructions.
- **`seq_cst` costs ~194 extra cycles per message, not "tens".** Same instructions
  (38.9 vs 46.6), IPC 0.14 vs 0.54. The `xchg` itself is cheap; what it waits for is the
  store buffer to drain, and the store buffer holds the slot write, which misses (the line is
  in the consumer's cache). This is our reading of the counters, not a direct measurement. So every message pays a full cross-core miss *serially* instead
  of overlapping it with the next message's work.
- **Mutexes: the lock word is the bottleneck.** `mutex-spin` spends **29% of its CPU time in
  the kernel** while `acqrel` spends 0 (`docs/results/E3/diag_mutex_spin_systime.txt`, a
  single diagnostic run): glibc's `std::mutex` makes a `futex` system call whenever `lock()`
  finds the mutex held, even though the threads rarely actually sleep (1.4e-5 context
  switches per message). `mutex-cv`'s consumer really sleeps: in latency mode it ran at
  **83.6% CPU** with **0.023 context switches per message** (one sleep/wake per ~43
  messages); every other variant ran at 100%.

**Padding (throughput):** `acqrel-cached` pad 0 has 1.92 L1D misses per message vs 1.06 at
pad 64 and 73.4 vs 17.7 cycles: the private cached copies sit on the same line as the index
the other core writes, so reading one's *own* cached copy misses (false sharing). For `acqrel`,
pad 128 cuts cycles per message from 85.8 to 56.2 with almost the same L1D miss count
(2.27 vs 2.51): the misses got cheaper rather than fewer, consistent with the adjacent-line
prefetcher no longer pulling the other side's index line along; `perf c2c` is needed to
confirm it.

**Placement (latency mode):** consumer L1D misses per message **5.0 (separate cores) vs 0.57
(SMT siblings)**: siblings share the L1 and L2, so the message and the index never leave the
core. In throughput mode the siblings' producer has 0.002 L1D misses per message (vs 1.04),
but the two spinning threads share one core's execution resources (IPC 1.57 vs 1.85).

**Tails (all lock-free variants):** p99.9 is 36–48 µs regardless of variant, padding or
placement, and each thread sees a handful of context switches per 2-second run (3–14 for
`acqrel-cached` in step 1). These cores are not
isolated, so the tail is the operating system (timer ticks, other tasks preempting the
producer or consumer; a preempted producer delays every scheduled message behind it), not the
queue. One `acqrel-cached` run (step 1, run 5) had a single 16 ms stall; the merged percentile
plot shows it, the per-run medians do not. Core isolation is E6.

## Where the hypotheses were wrong

- **H1 (blocking costs µs): right, but fewer sleeps than expected.** p50 11.4 µs, 43.9× the
  best ring; consumer CPU 83.6%. Predicted "a context switch per few messages"; measured one
  per ~43 messages: the consumer usually finds the next message already there when it
  re-acquires the lock, so each sleep/wake likely covers a burst.
- **H2 (mutex + spin: a few hundred ns): wrong.** p50 **5.7 µs**, only 2× better than the
  condition variable. Removing the sleep did not remove the kernel: `std::mutex` enters the
  kernel (`futex`) on every contended `lock()`, 29% of CPU time in the diagnostic run. With a
  consumer that polls the lock continuously, the producer often finds it held, and every
  `futex` call is a kernel entry and exit (our reading; the per-call cost was not measured). "Spinning" around a sleeping lock is not a spin lock.
- **H3 (`seq_cst` same latency, lower throughput): right**, with a bigger throughput cost than
  predicted (3.24×, ~194 cycles per message, not tens), because the barrier waits for a
  store that is itself a cross-core miss.
- **H4 (cached index: throughput only, no latency change): half wrong.** Throughput 4.91×
  as predicted, but latency p50 also improved 1.21× [1.11, 1.35]. At low rate the producer
  without a cache reads `head` before every push, and the consumer has just written `head`, so
  that read is a cross-core miss *on the critical path* before the slot is written. The
  producer's L1D misses per message in latency mode confirm it: 3.98 without the cache vs 2.8–2.9
  with it.
- **H5 (padding: 0 worst, 64 vs 128 small): wrong for the uncached ring.** Without the cached
  index, 64 B was no better than no padding (1.05×, not resolved) and **128 B was 1.53×
  faster than 64 B**: separating the indices into adjacent lines is not enough on Intel,
  where the spatial prefetcher works on 128-byte pairs. With the cached index the indices are
  rarely touched, so 64 vs 128 hardly matters (pad 64 1.10× better), while pad 0 is 4.1× worse
  because of the private copies (above).
- **H6 (siblings: faster hand-off, slower throughput; unpinned: worse tails): first two
  right, last one not shown.** Siblings 112 vs 249 ns p50 (2.22×) and 1.17× slower throughput.
  Unpinned threads were indistinguishable from pinned ones on this quiet machine, including
  the tail; the scheduler kept the two busy threads on separate idle cores.
- Compared with E0's ping-pong (one-way ~77 ns siblings, ~188 ns cross-core), the queue's
  hand-off p50 (112 / 249 ns) is higher by the extra line (slot and `tail` are separate
  lines) and the producer's reaction to its schedule.

## Conclusion

On this CPU, handing a 64-byte message from one core to another costs about **250 ns
one-way at p50 (112 ns between SMT siblings)** with the best lock-free ring, and the ring
sustains **11 ns per message (~91 M messages/s)**. The textbook blocking queue is 44× slower
in latency and 30× slower in throughput. Every step between them is explained by cache-line
movement: `seq_cst` serializes a cross-core miss per message (3.2×), the cached remote index
removes the index lines from the per-message traffic (4.9×, down to ~1 L1D miss per message:
the message itself), and padding must be 128 B, not 64 B, when indices are read every
message. Busy-polling around `std::mutex` is not lock-free: it still enters the kernel. The
tail (p99.9 ~40 µs) belongs to the operating system on non-isolated cores, not to the queue.

## Threats to validity

- **Laptop CPU at 1.6 GHz, turbo off, no `isolcpus`.** Absolute ns do not transfer; the ratios
  and the counter mechanisms do. Timer interrupts land on the measured cores.
- **One message size and one ring size** (64 B, 1 024 slots). A smaller message would put
  several messages in one line and change the throughput picture.
- **Latency at one offered rate** (1 msg/µs). A rate sweep belongs to E4 (threading models).
- **Busy-spinning without `PAUSE`** burns a full core per waiting thread; that is the usual
  low-latency trade-off, but it hurts an SMT sibling. A `PAUSE` variant is not measured.
- **Tests explore only the interleavings that happened** (see `docs/concurrency.md` §7).

## Reproduce

```bash
sudo scripts/tune_machine.sh apply
cmake --preset release && cmake --build --preset release
B="{bin}/apps/e3_queue --queue {queue} --pad {pad} --mode {mode} --place {place} --out {out}"
python3 scripts/run_bench.py --name E3-variants --runs 10 --cmd "$B" \
  --param queue=mutex-cv,mutex-spin,seqcst,acqrel,acqrel-cached --param pad=64 \
  --param mode=latency,throughput --param place=2-3
python3 scripts/run_bench.py --name E3-padding --runs 10 --cmd "$B" \
  --param queue=acqrel,acqrel-cached --param pad=0,64,128 --param mode=latency,throughput --param place=2-3
python3 scripts/run_bench.py --name E3-placement --runs 10 --cmd "$B" \
  --param queue=acqrel-cached --param pad=64 --param mode=latency,throughput --param place=2-3,2-6,unpinned
python3 scripts/analyse.py summary results/E3-variants/latest   # likewise for the others
sudo scripts/tune_machine.sh restore
```
