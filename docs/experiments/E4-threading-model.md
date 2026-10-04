# E4: Run-to-Completion vs Pipelined (Threading-Model Study)

> Status: **in progress.** Hypotheses written 2026-10-04 before any E4 run. Results,
> counter evidence and conclusions are filled in from measured runs only.

## Question

A trading engine's market-data path is decode → book update → strategy/risk → order out.
Should one core run the whole path for each message before taking the next (**Model A,
run-to-completion**), or should each stage get its own core, with messages handed between
cores through lock-free rings (**Model B, pipelined**)? How does the answer change with the
offered load, the per-message strategy cost, and bursty (real) vs smooth arrival?

## Hypotheses (expected mechanism), written before any E4 run

Inputs from earlier experiments:
- E2: decode + book update on the full day costs ~311 ns/msg (batch, `fast-fib`), per-message
  p50 302 ns.
- E3: one cross-core hand-off through the acquire/release cached ring costs ~250 ns one-way
  p50 when the ring is mostly empty (the consumer waits on a cold line), but only ~11 ns/msg
  when streaming (many messages in flight, line transfers overlap).
- E1: the real feed's 1 ms peaks reach 175× the regular-hours mean (2.0 M msg/s).

**H1. Below saturation, A has lower latency than B.** B adds two cross-core hops
(decode → decision, decision → sink). At low load each hop finds an empty ring, so each costs
close to E3's ~250 ns, and B's p50 should be roughly 300–500 ns above A's. As load rises,
B's hop cost should shrink (more messages in flight, E3's streaming case), narrowing the gap.

**H2. B's capacity gain is small and shrinks as strategy work grows.** A's capacity is
1 / (decode + book + strategy + work + sink); B's is 1 / (its slowest stage). The book and the
work knob are in the same (middle) stage, so B only offloads decode and the sink:
capacity ratio B/A ≈ (decode + book + work + sink) / (book + work + hop overhead). At work 0
that might be 1.2–1.4×; at 1000 ns of work it should approach 1.0×. (The master plan's
hypothesis, "B extends the saturation point once per-message work exceeds the inter-arrival
time", assumed the work can be split across stages; with a single heavy stage it cannot.)

**H3. Bursty arrival saturates the engine far below its average-rate capacity.** At the same
mean rate, the real arrival pattern contains bursts well above the mean, so queues build in
bursts and the tail (p99, p99.9) of the bursty runs rises long before the smooth runs'
tail does. Whichever model has more capacity (H2: B, slightly) drains bursts faster, so B
might win the bursty tail at loads where A wins the smooth p50.

**H4. B spends more total CPU per message.** Three spinning cores instead of one, and every
message's records cross two cache-line transfers; summed cycles per message (throughput mode)
should be higher for B than for A even where B's throughput is higher.

## Setup

- Intel i5-8250U, turbo off, `performance` governor (same machine and settings as E0–E3),
  GCC 15.2 `release` preset (`-O2 -DNDEBUG`).
- Input: the full NASDAQ ITCH day of 2019-07-30 (all symbols; E1 showed a top-symbol slice is
  the favourable, everything-cached case). Every message before the measured window is
  pushed through the engine unmeasured, rebuilding all books exactly as in the real day;
  then a window of M messages starting at a fixed feed time is replayed open loop.
- Feeder: a pinned thread on CPU 0 replaying from memory. Engine: Model A on CPU 2; Model B's
  decode / decision / sink stages on CPUs 1 / 2 / 3. All four physical cores are busy in
  Model B; CPU 0 also serves interrupts, which hits the feeder (both models alike; feeder
  lateness counts as latency because latency is measured from the intended send time).
- Rings: E3's best variant (acquire/release, cached remote index, 128-byte padding),
  1 024 slots of one 64-byte record each, for the feeder ring and both internal rings.
- Identical work: both models call the same decode, decision (book + imbalance strategy +
  cool-down and position-limit risk + optional spin of k ns) and sink functions. Every run
  writes an order digest; all runs on the same window must have the same digest.

## Method

- **Capacity (closed loop):** the feeder sends the window as fast as the engine accepts;
  the sink times every block of 1 024 messages → ns/msg and msgs/s, per model × work
  k ∈ {0, 200, 500, 1000} ns. Hardware counters summed over the engine threads.
- **Latency vs offered load (open loop):** for each work level, a grid of offered rates from
  ~10% of A's measured capacity to past B's; arrival smooth (fixed interval) and bursty (the
  window's original feed timestamps scaled to the same mean rate). Latency = sink TSC −
  intended send TSC, every message of the window.
- N = 10 interleaved runs per configuration (`run_bench.py`), median across runs with 95%
  bootstrap CI (`analyse.py`), as in E0–E3.
- **Per-hop breakdown:** separate runs with TSC stamps between the stages (`--hops 1`) at a
  few loads; not used for the main curves (each stamp costs ~16 ns, E0).

## Results

*(pending)*

## Where the hypotheses were wrong

*(pending)*

## Threats to validity

*(pending)*

## Reproduce

*(pending)*
