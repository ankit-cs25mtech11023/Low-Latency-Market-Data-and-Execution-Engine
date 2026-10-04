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
  **Window: M = 1 000 000 messages starting at feed time 09:30:00.000** (the first ~14 s of
  continuous trading after the opening cross), after a warm-up of the **10 411 212** messages
  from 04:00 to 09:30. Input file: the first 12.5 M messages of the day
  (`slice_itch --first 12500000`, plain, so loading avoids gzip). Why 09:30 and not a midday
  window: the warm-up must replay every earlier message (books are rebuilt exactly), and a
  10:00 window needs 44 M warm-up messages (1.29 GB of bodies, ~1.8 GB resident on a machine
  with ~3 GB free) and 09:45 one needs 30 M (19 s per run, 6.7 h for the grid); 09:30 needs
  10.4 M (5–8 s per run). The cost: the window is the busiest part of the day, not a typical
  one (see threats).
- Feeder: a pinned thread on CPU 0 replaying from memory. Engine: Model A on CPU 2; Model B's
  decode / decision / sink stages on CPUs 1 / 2 / 3. All four physical cores are busy in
  Model B; CPU 0 also serves interrupts, which hits the feeder (both models alike; feeder
  lateness counts as latency because latency is measured from the intended send time).
- Rings: E3's best variant (acquire/release, cached remote index, 128-byte padding),
  1 024 slots of one 64-byte record each, for the feeder ring and both internal rings.
- Identical work: both models call the same decode, decision (book + imbalance strategy +
  cool-down and position-limit risk + optional spin of k ns) and sink functions. The spin
  runs only for measured messages (it changes no state, so the warm-up skips it). Every run
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

### Capacity (closed loop)

N = 10 interleaved runs per row, median [95% bootstrap CI]; the window is 1 M messages, so
capacity = messages / window wall time. Counters are summed over the engine threads (one for
A, three for B) and divided by the window's messages. Files:
[`capacity.md`](../results/E4/capacity.md), [`capacity.csv`](../results/E4/capacity.csv).

| work (ns/msg) | A capacity (M msg/s) | B capacity (M msg/s) | **B / A** | cycles/msg A | cycles/msg B | B / A cycles |
|---|---|---|---|---|---|---|
| 0 | 1.940 [1.898, 1.954] | 2.083 [1.998, 2.134] | **1.074×** [1.040, 1.113] | 821 | 2 282 | 2.78× [2.68, 2.88] |
| 200 | 1.358 [1.345, 1.383] | 1.413 [1.381, 1.423] | **1.040×** [1.011, 1.052] | 1 173 | 3 367 | 2.87× [2.84, 2.95] |
| 500 | 0.945 [0.932, 0.952] | 0.962 [0.953, 0.991] | **1.019×** [1.006, 1.051] | 1 685 | 4 940 | 2.93× [2.85, 2.97] |
| 1000 | 0.627 [0.625, 0.641] | 0.635 [0.623, 0.641] | **1.013×** [0.988, 1.024] | 2 538 | 7 448 | 2.93× [2.91, 3.01] |

All 80 runs made the same 100 290 order decisions (digest `f81da7945b24d496`) with 0 unknown
refs, duplicates or overfills (`scripts/e4_check_digest.py`).

**Where B's time goes** (work 0, per stage, median of 10 runs): the decide stage (book +
strategy) has **17.9 LLC misses/msg at IPC 0.77**, i.e. all of the cache misses (A has 17.8 in
total). Decode and sink run at the same 764 cycles/msg only because they spin waiting for the
decide stage (their instruction counts, 978 and 1 355 per message, are mostly the spin loop).
So the pipeline's throughput is the decide stage's, and the only work B moves off that
critical path is decode + sink: A spends 821 cycles/msg in total, B's bottleneck stage 764,
a difference of ~57 cycles (7%) — exactly the measured 1.074×. Added work lands in the same
stage, so the ratio falls toward 1.0 (1.013× at 1000 ns, CI includes 1).

### Latency vs offered load (open loop)

*(sweep running; filled in from the N = 10 sweep only)*

## Where the hypotheses were wrong

*(pending)*

## Threats to validity

*(pending)*

## Reproduce

*(pending)*
