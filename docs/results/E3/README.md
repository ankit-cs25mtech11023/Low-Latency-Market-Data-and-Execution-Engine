# E3 curated results

Copies of the result files behind `docs/experiments/E3-queues.md`. The raw per-run
histograms stay under `results/` (git-ignored).

| files | source run (UTC) | what |
|---|---|---|
| `variants-*` | `results/E3-variants/20261004T124409Z` (git `a495e87`) | 5 queue variants × {latency, throughput}, pad 64 B, CPUs 2 → 3, N = 10 |
| `padding-*` | `results/E3-padding/20261004T125136Z` (git `f14b9b4`) | `acqrel`, `acqrel-cached` × pad {0, 64, 128} × {latency, throughput}, CPUs 2 → 3, N = 10 |
| `placement-*` | `results/E3-placement/20261004T125721Z` (git `f14b9b4`) | `acqrel-cached` pad 64 × {2 → 3, 2 → 6, unpinned} × {latency, throughput}, N = 10 |
| `diag_mutex_spin_systime.txt` | single diagnostic runs | user vs system CPU time (`perf stat`) of `mutex-spin` and `acqrel`: mechanism evidence, not a timing result |
| `tsan_stress_1e8.log` | `results/E3/tsan_stress_1e8.log` | ThreadSanitizer two-thread stress test, 10^8 messages per lock-free variant |

Each session: `*-summary.md` / `.csv` (percentiles, median across runs with 95% bootstrap
CI), `*-counters.csv` (per-message counters for the consumer and the producer, CPU% per
thread), `*-percentiles.png`, `*-env.json` (machine state before/after), `*-manifest.json`
(command, run order, git SHA). Latency variants are in ns one-way; throughput variants in
ns per message (`value_divisor` 1 024). `a495e87` and `f14b9b4` differ only in documentation.
