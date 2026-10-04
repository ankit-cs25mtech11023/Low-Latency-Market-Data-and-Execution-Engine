# E1 curated results

Copies of the result files behind `docs/experiments/E1-workload.md`, from the run
`results/E1/07302019` (git `a4102bd` (now `ff265fa`, see [`docs/git-history.md`](../../git-history.md)), `release` preset, clean tree; see `env.json`).

| file | what |
|---|---|
| `summary.json` | every headline number: message and book counters, invariants, order refs, peak live orders, lifetime / distance / burst percentiles, peak RSS |
| `message_mix.csv` | count per ITCH message type |
| `depth_snapshots.csv` | levels per side and orders per level, every 30 min of feed time |
| `lifetime_*.hist.csv` | order lifetime histograms (ns), by how the order ended |
| `dist_add_behind.hist.csv`, `dist_add_improve.hist.csv` | distance of adds from the best on the same side (ticks) |
| `rate_window_{1,10,1000}ms.hist.csv` | messages per window, 09:30–16:00, empty windows included |
| `e1-*.png`, `e1-tables.md` | plots and tables made by `scripts/e1_plots.py` |
| `env.json` | machine, kernel, compiler and git state at the start of the run |

Only aggregate statistics are kept here. **No Nasdaq messages or slices are committed.** Two
files of the run directory stay local: `symbols.csv` (per-symbol counts and price ranges for
all 8 841 symbols; the doc shows the top 10) and `crossed_events.csv` (the 989 crossed/locked
tops with their prices; the doc summarizes them and walks through the two resume sequences).
To regenerate the plots, which need `symbols.csv`, re-run the tool (see "Reproduce" in the doc).

Histogram CSV columns are `low,high,count`: every value in `[low, high]` falls in that bucket.
Percentiles taken from them are bucket upper bounds.
