# E0 curated results

Copies of the result files behind `docs/experiments/E0-measurement.md`. The raw per-run
histograms stay under `results/` (git-ignored); these are the summaries, environment
captures and run manifests needed to check every number in the write-up.

| files | source run (UTC) | what |
|---|---|---|
| `timers-*` | `results/E0-timers/20261003T215259Z` | 13 timer variants, N = 10, counters cycles/instructions/ref-cycles |
| `hist-counters-*` | `results/E0-hist-counters/20261003T221418Z` | histogram record, N = 10, counters incl. branch-misses and L1D load misses |
| `skew*` | `results/E0-skew/20261003T221107Z` | cross-core TSC skew, all 56 ordered CPU pairs, N = 10 |

All three were built from git `7291ce5` with the `release` preset on the tuned machine
(see the `*-env.json` files).
