# E2 curated results

Copies of the result files behind `docs/experiments/E2-orderbook.md`. The raw per-run
histograms stay under `results/` (git-ignored). No Nasdaq messages are stored here; every
file is an aggregate (percentiles, counters, counts).

| files | source | what |
|---|---|---|
| `summary.md`, `summary.csv` | `results/E2-book/20261004T105758Z` (session A) | `ref` vs `fast-fib`, N = 10 interleaved full-day runs each; `__method=batch` = throughput (ns/msg over blocks of 1 024), `__method=permsg` = per-message latency; median across runs with 95% bootstrap CI |
| `counters.csv` | same | hardware counters per message (timed region only) and heap allocations per message |
| `percentiles.png` | same | percentile curves, all runs merged |
| `env.json`, `manifest.json` | same | machine state before/after, command, run order, git SHA |
| `diff_fullday_fast-fib.json` | `results/E2/diff_fullday_fast-fib.json` | full-day differential test of `fast-fib` against the reference book |
| `sessionB-summary.md`, `sessionB-summary.csv`, `sessionB-counters.csv`, `sessionB-percentiles.png`, `sessionB-env.json`, `sessionB-manifest.json` | `results/E2-book/20261004T131348Z` (session B) | identity-hash `fast` book, N = 10 full-day runs, same command and preset as session A, run afterwards (not interleaved with A) |
| `identity-profile/*.txt` | `perf record` on the `fast` (identity-hash) book | `perf report` by symbol and `perf annotate` of the hottest functions: mechanism evidence for H3, not a timing result |

Session A was built from git `7e2a7a2` (clean tree) with the `release` preset on the tuned
machine (`env.json`). The differential run was built from `ad2b905` with uncommitted E3
files in the tree (new queue headers and the E3 driver, not compiled into `diff_books`; the
book code is identical to `7e2a7a2`).

Session B was built from git `ba5daf0` (clean tree), same preset and command.

The identity-hash profile was recorded with `perf record -e cpu-clock` for 5 s on the
aborted run `results/E2-book/20261004T102616Z` (binary from `f7e0ad1`). That run was
stopped because `pigz` shared the measured CPU; the profile shows *where* the book thread
spent its own samples (the probe loop), which that problem does not change. It is not used
for any timing number.
