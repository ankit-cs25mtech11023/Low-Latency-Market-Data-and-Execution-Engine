# Benchmark Methodology

How every number in this project is produced. If a result does not follow these rules it is
not reported. The rules come from master plan §5.2 and §9; this file explains the reasons.

## 1. Clock

All latencies are differences of two **TSC** (time-stamp counter) readings.

- **Start stamp:** `lfence; rdtsc`. `rdtsc` alone may execute before earlier instructions
  have finished (out-of-order execution), so the stamp could be taken "too early". `lfence`
  waits until all earlier instructions have completed locally before `rdtsc` runs.
- **End stamp:** `rdtscp; lfence`. `rdtscp` waits for all earlier instructions to finish
  before reading the counter. The trailing `lfence` stops *later* instructions (e.g. the
  code that stores the result) from starting before the stamp is taken.
- **Calibration:** ticks per nanosecond are measured against `CLOCK_MONOTONIC_RAW` over ~1 s
  at the start of every run, and written to that run's `meta.json`. The analysis converts
  ticks to ns with that run's own calibration.
- **Validity checks:** the program refuses to run without `constant_tsc` and `nonstop_tsc`
  in `/proc/cpuinfo` (rate independent of frequency changes and C-states).
- **Measurement floor:** a start stamp immediately followed by an end stamp does not read 0.
  E0 measures this floor; every stage latency includes it.
- **Cross-core comparisons** (stamp taken on core A, subtracted from a stamp on core B) are
  only valid within the skew bound measured in E0.
- The interface (`lle::Tsc`) hides the instruction set, so an AArch64 counter can replace it.

## 2. Units

- **TSC ticks** run at a fixed 1.8 GHz on this CPU. They measure wall-clock time.
- **Core cycles** run at the actual core frequency. With turbo on, the core may run at up
  to 3.4 GHz, so ticks and cycles differ. Core cycles come from hardware counters
  (`perf_event_open`), not from the TSC.
- **ns** = ticks / calibrated ticks-per-ns.
- **Instructions per operation** is a secondary metric that is almost free of noise. It
  shows whether a code change altered the work done, independent of timing noise.

Results state which unit they use and the TSC frequency.

## 3. Load model: open loop, no coordinated omission

A closed-loop benchmark ("send the next message when the previous one is done") hides
queueing. If the system stalls for 1 ms, the benchmark simply stops sending during that
millisecond, and the stall shows up as one slow sample instead of the hundreds of messages
that would really have waited behind it. This is *coordinated omission*.

From E4 onwards, every message has an **intended send time** from a fixed schedule
(replay at a speed factor, or a fixed rate), and latency is measured from that intended
time, not from when the sender actually managed to send it. Delays caused by the system
under test are then counted for every message they affect.

E0–E3 are micro-benchmarks of individual operations, not request/response systems, so this
does not apply to them.

## 4. Runs

1. `sudo scripts/tune_machine.sh apply` (or the named E6 configuration), AC power,
   browser closed. `scripts/env_capture.sh` records the state before and after the session.
2. Every thread is pinned to a named logical CPU (default: CPU 2, away from CPU 0, which
   handles most interrupts). The CPU map is recorded with the run.
3. **Warm-up:** each process first runs the workload and throws the result away (caches,
   branch predictors, page faults, frequency settling). `run_bench.py` also runs one full
   warm-up pass of every variant first and discards it.
4. **N ≥ 10 runs per configuration, interleaved.** Runs alternate between variants
   (A B C, B C A, …) instead of AAAA BBBB. Slow drifts (temperature, background activity)
   then affect all variants equally and become run-to-run noise that the confidence
   interval captures, instead of a false difference between variants.
5. Each run is a separate process, so process-level effects (memory layout, ASLR) vary
   between runs and are included in the noise.

## 5. Sample counts

A tail percentile needs enough samples beyond it to be meaningful. Per run:

| percentile | minimum samples per run |
|---|---|
| p99 | 10⁴ |
| p99.9 | 10⁵ |
| p99.99 | 10⁶ |

`analyse.py` leaves a percentile blank if a run has fewer samples. The **maximum** is one
event, not a statistic; it is reported separately and never used to compare variants.

## 6. Statistics

- **Per run:** percentiles from the run's histogram (nearest-rank definition; the
  log-linear histogram is accurate to < 0.79% relative error, validated against
  HdrHistogram_c in `tests/unit/test_histogram_vs_hdr.cpp`).
- **Across runs:** the **median** of the per-run values, with a **95% bootstrap confidence
  interval**: resample the N run values with replacement 10 000 times, take the median of
  each resample, and report the 2.5th and 97.5th percentiles of those medians. The median
  is used because one disturbed run should not move the result.
- **A vs B:** the bootstrap CI of the difference of medians (Mann-Whitney U alongside).
  An improvement is claimed only if that CI excludes 0.

## 7. Hardware counters

Counters are read with `perf_event_open` around the **timed region only**
(`lle::PerfCounters`), not around the whole process, so calibration, warm-up, file loading
and result writing are excluded. They are counted as one group, so all counters cover the
same time window. Result files record `time_enabled` and `time_running`; if the PMU had to
be time-shared (multiplexing), the run shows a running fraction below 100% and its counts
are scaled estimates.

Counters explain results: "faster" is not a result, "faster because LLC misses per message
fell from X to Y" is.

## 8. Builds

Performance numbers come only from the `release` preset (GCC, `-O2 -DNDEBUG`). Debug,
sanitizer and instrumented builds are used for testing only. Profiling uses
`relwithdebinfo` (`-O2 -g -fno-omit-frame-pointer`); its timings are not reported.

## 9. Reproducibility

`scripts/run_bench.py` refuses to run from a tree with uncommitted changes (unless
`--allow-dirty` is given for development, and such results are never reported). Each
session writes:

```
results/<experiment>/<UTC timestamp>/
    env.json         machine state before and after (CPU, microcode, kernel, cmdline,
                     governor, turbo, isolated CPUs, compiler, flags, git SHA)
    manifest.json    exact command template, variants, run order, git SHA
    <variant>/runNN.hist.csv   raw histogram (TSC ticks)
    <variant>/runNN.meta.json  build info, TSC calibration, parameters, counters
```

`scripts/analyse.py summary <dir>` regenerates every table and plot from these raw files.
Curated result sets are copied under `docs/results/` when an experiment is written up.

## 10. What is not claimed

- No NIC, wire or kernel-bypass latency: the laptop has Wi-Fi only, and networking
  experiments run over veth pairs between network namespaces.
- No numbers from other machines. All results are specific to this CPU, kernel and workload.
