# E0: Measuring the Measurement

> Status: **hypotheses written, tuned runs pending.** Sections below marked *(pending)* are
> filled only from measured result files. A short untuned smoke run was made while
> developing the drivers; its numbers are not used here.

## Question

Every later latency number is a difference of two timestamps. Before trusting any of them:

1. What does reading the time cost, for each available clock, in core cycles and ns?
2. What is the smallest interval the project's start/stop stamp pair can report (the
   measurement floor that every stage latency includes)?
3. Can a TSC stamp taken on one core be subtracted from a stamp taken on another core,
   and to what accuracy?
4. What does recording one sample into the log-linear histogram cost?

## Hypotheses (expected mechanism), written before the tuned runs

**H1. Raw TSC reads.** `rdtsc` is a microcoded instruction (about 20 µops on Skylake-family
cores), so back-to-back calls are limited by microcode throughput, roughly 25 core cycles
each (Agner Fog's instruction tables list a reciprocal throughput of ~25 for Skylake).
`rdtscp` additionally waits for all earlier instructions to finish and reads `IA32_TSC_AUX`,
so it should cost more (~30–40 cycles). Adding `lfence` makes each read wait for the
previous one to complete; we expect `lfence;rdtsc` and `rdtscp;lfence` to cost about the
same as `rdtscp`, and `lfence;rdtsc;lfence` slightly more.

**H2. OS clocks.** On this kernel `clock_gettime(CLOCK_MONOTONIC / CLOCK_MONOTONIC_RAW /
CLOCK_REALTIME)` and `std::chrono::steady_clock::now()` (which calls
`clock_gettime(CLOCK_MONOTONIC)`) are served by the **vDSO** in user space, without a
system call. A vDSO read is: read a sequence counter, an ordered TSC read, a multiply and
shift to convert ticks to ns, re-check the sequence counter. We expect about 2× the cost of
an ordered TSC read (~50–70 cycles), all four clocks within a few cycles of each other, and
`steady_clock` equal to `CLOCK_MONOTONIC` (it is a thin wrapper).

**H3. Measurement floor.** An empty start/stop region costs the latency of the fenced pair,
not the throughput: about 30–50 core cycles. The floor in **TSC ticks** depends on the core
frequency, because the work is done in core cycles but counted at the fixed 1.8 GHz TSC rate.

**H4. Core frequency under the baseline tuning.** With `no_turbo=1` and the `performance`
governor, the core should run at its maximum non-turbo ratio. For the i5-8250U the
marketed base clock is 1.6 GHz, but the TSC runs at 1.8 GHz; we expect the core to run at
1.8 GHz (the platform's P1 ratio), so that TSC ticks ≈ core cycles in tuned runs. The
measured "core GHz" (cycles / time in the region) tests this.

**H5. Cross-core TSC skew.** The kernel synchronizes the TSCs of all cores at boot and the
`TSC_ADJUST` MSR keeps them aligned, so the true offset between any two logical CPUs should
be 0 within the measurement bound. The bound (RTT/2 of a cache-line ping-pong) should be
smallest for SMT siblings (n, n+4: the line stays in the shared L1/L2) and larger for
different physical cores (the line moves through the shared L3 / ring).
A single-direction NTP estimate can show a non-zero "offset" caused by an asymmetric
measurement path rather than real skew. Measuring both directions (A→B and B→A) separates
the two: real skew flips sign with the direction, asymmetry does not.

**H6. Histogram record.** `record()` is a `lzcnt`, a shift, an add, an increment of one
bucket in memory, a count, a sum and min/max updates: about 10–15 instructions. Throughput
should be limited by the loop-carried dependency through memory on `count_`/`sum_` (store,
then reload in the next iteration via store-to-load forwarding, ~4–5 cycles), so we expect
roughly 5–8 cycles per record over the `hist_baseline` loop.

## Setup *(pending)*

Hardware, kernel cmdline, governor/turbo state, compiler and flags, git SHA: from the
session's `env.json`. Measurement CPU: 2 (not CPU 0, which handles most interrupts).

## Variants

| driver | variant | what one operation is |
|---|---|---|
| `e0_timers` | `rdtsc` | `rdtsc` |
| | `lfence_rdtsc` | `lfence; rdtsc` (the project's start stamp) |
| | `rdtscp` | `rdtscp` |
| | `rdtscp_lfence` | `rdtscp; lfence` (the project's end stamp) |
| | `lfence_rdtsc_lfence` | `lfence; rdtsc; lfence` |
| | `steady_clock` | `std::chrono::steady_clock::now()` |
| | `clock_monotonic` | `clock_gettime(CLOCK_MONOTONIC)` |
| | `clock_monotonic_raw` | `clock_gettime(CLOCK_MONOTONIC_RAW)` |
| | `clock_realtime` | `clock_gettime(CLOCK_REALTIME)` |
| | `empty_loop` | the loop with no clock call (overhead baseline) |
| | `empty_region` | one start stamp then one end stamp (single sample, not batched) |
| | `hist_record` | `Histogram::record(v)`, v from a precomputed log-normal array |
| | `hist_baseline` | same loop, summing v instead of recording it |
| `e0_skew` | all ordered CPU pairs | one ping-pong round trip |

## Method

- Per-call variants: 200 000 batches of K = 1 000 back-to-back calls per run, each batch
  bracketed by one start/stop pair; the batch time divided by K is one sample. Batching
  amortizes the bracket overhead, so this measures the **throughput cost** of a call
  (how much a call slows down a loop that makes many of them), not the latency of a single
  isolated call. The single-call cost is what `empty_region` shows.
- Hardware counters (cycles, instructions, ref-cycles) around the measured batches only.
- 10 runs per variant, interleaved, plus one discarded warm-up pass; median across runs
  with 95% bootstrap CI (`docs/methodology.md`).
- Skew: 200 000 round trips per ordered pair per run (after 10 000 warm-up), 10 runs.
  The offset estimate uses the 1% of samples with the smallest RTT (tightest bound).

## Results *(pending)*

## Counter evidence *(pending)*

## Where the hypothesis was wrong *(pending)*

## Conclusion *(pending)*

## Threats to validity

- Batched per-call costs are throughput numbers; a single call in cold code can cost more
  (instruction-cache and microcode-sequencer warm-up).
- The batch average hides per-call variation, so tail percentiles of the batched variants
  describe 1 000-call windows, not single calls. Single-call tails come from `empty_region`.
- vDSO behaviour depends on the kernel's clocksource (`tsc` here, recorded in `env.json`).
  With a different clocksource (e.g. `hpet`) `clock_gettime` becomes a system call and
  costs far more.
- Results are for this CPU (Kaby Lake R microcode `0xf6`), this kernel and this compiler.

## Reproduce *(pending exact commands)*
