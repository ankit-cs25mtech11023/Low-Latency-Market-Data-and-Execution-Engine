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

(filled in from `env.json` after the runs)

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

(filled in after the runs)

## Counter evidence

(filled in after the runs)

## Where the hypotheses were wrong

(filled in after the runs)

## Conclusion

(filled in after the runs)

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
  --param queue=<best> --param pad=<best> --param mode=latency,throughput --param place=2-3,2-6,unpinned
python3 scripts/analyse.py summary results/E3-variants/latest   # likewise for the others
sudo scripts/tune_machine.sh restore
```
