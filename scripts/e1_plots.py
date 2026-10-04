#!/usr/bin/env python3
"""E1 plots and tables from an e1_workload output directory.

    scripts/e1_plots.py results/E1/07302019            # writes e1-*.png and e1-tables.md there

Every figure answers one Phase 2 design question (see docs/experiments/E1-workload.md):
  e1-distance.png   how wide must the tick ladder be?   (fraction of events within W ticks of best)
  e1-lifetimes.png  how long do orders live?            (CDF of add -> death, per way of dying)
  e1-burstiness.png how far above the mean do peaks go? (msgs/s per 1 ms / 10 ms / 1 s window)
  e1-symbol-skew.png how concentrated is activity?      (share of book messages in the top-K symbols)
"""
from __future__ import annotations

import csv
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyse import SERIES_COLORS, SERIES_STYLES  # noqa: E402  same validated palette as E0

GRID_MAJOR, GRID_MINOR = "#d8d7d3", "#ecebe8"
REFERENCE = "#6f6e6a"  # neutral ink for reference lines (never a series colour)
REGULAR_HOURS_S = 6.5 * 3600


def load_hist(path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Returns (bucket upper bound, count). Upper bounds make CDFs conservative (never early)."""
    hi, cnt = [], []
    with path.open() as f:
        for row in csv.DictReader(f):
            hi.append(int(row["high"]))
            cnt.append(int(row["count"]))
    return np.array(hi, dtype=float), np.array(cnt, dtype=float)


def quantile(hi: np.ndarray, cnt: np.ndarray, q: float) -> float:
    cum = np.cumsum(cnt)
    return float(hi[np.searchsorted(cum, max(1.0, np.ceil(q * cum[-1])))])


def style(ax, title: str) -> None:
    ax.set_title(title, loc="left", fontsize=11)
    ax.grid(True, which="major", color=GRID_MAJOR, linewidth=0.6)
    ax.grid(True, which="minor", color=GRID_MINOR, linewidth=0.4)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)


def series(ax, i: int, x, y, label: str) -> None:
    ax.plot(x, y, label=label, color=SERIES_COLORS[i], linestyle=SERIES_STYLES[i], linewidth=2)


def plot_distance(summary: dict, out: Path, plt) -> None:
    d = summary["distance_ticks"]
    keys = [("coverage_add", "add (new order)"), ("coverage_replace_new", "replace (new price)"),
            ("coverage_cancelled", "partial cancel"), ("coverage_deleted", "delete"),
            ("coverage_executed", "execution")]
    fig, ax = plt.subplots(figsize=(9, 4))
    for i, (k, label) in enumerate(keys):
        cov = d[k]
        w = np.array([int(x) for x in cov])
        series(ax, i, w + 1, np.array(list(cov.values())) * 100, label)
    ax.set_xscale("log")
    ticks = [0, 1, 4, 16, 64, 256, 1024, 4096, 16384]
    ax.set_xticks([t + 1 for t in ticks], [str(t) for t in ticks])
    ax.set_xlabel("W = distance from the best price on the same side (ticks)")
    ax.set_ylabel("% of events within W ticks")
    ax.set_ylim(0, 100)
    style(ax, "Where book events happen relative to the best price")
    ax.legend(fontsize=8, loc="lower right", frameon=False)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def plot_lifetimes(rd: Path, out: Path, plt) -> None:
    fig, ax = plt.subplots(figsize=(9, 4))
    for i, name in enumerate(["executed", "cancelled", "deleted", "replaced"]):
        hi, cnt = load_hist(rd / f"lifetime_{name}.hist.csv")
        if cnt.sum() == 0:  # e.g. no X ever removed a whole order on 2019-07-30; keep colours stable
            continue
        series(ax, i, np.maximum(hi, 1), np.cumsum(cnt) / cnt.sum() * 100, f"{name} (n={int(cnt.sum()):,})")
    ax.set_xscale("log")
    ticks = [1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11]
    ax.set_xticks(ticks, ["1 µs", "10 µs", "100 µs", "1 ms", "10 ms", "100 ms", "1 s", "10 s", "100 s"])
    ax.set_xlim(1e2, 3e13)
    ax.set_xlabel("order lifetime: add (or replace) until the message that removed it")
    ax.set_ylabel("cumulative % of orders")
    ax.set_ylim(0, 100)
    style(ax, "Order lifetime by how the order ended")
    ax.legend(fontsize=8, loc="upper left", frameon=False)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def plot_burstiness(rd: Path, summary: dict, out: Path, plt) -> dict:
    from matplotlib import ticker

    fig, ax = plt.subplots(figsize=(9, 4))
    # Start at the median: below it many 1 ms windows are empty (rate 0), which a log axis cannot show.
    qs = 1 - np.logspace(np.log10(0.5), -5, 300)
    stats = {}
    for i, ms in enumerate([1, 10, 1000]):
        hi, cnt = load_hist(rd / f"rate_window_{ms}ms.hist.csv")
        n = cnt.sum()
        if n == 0:  # stream without regular-hours messages (e.g. a synthetic fixture)
            continue
        qv = qs[qs <= 1 - 1 / n]
        rates = np.array([quantile(hi, cnt, q) for q in qv]) * 1000.0 / ms
        series(ax, i, 1 / (1 - qv), rates, f"{ms} ms windows")
        mean = summary["rate_per_window_regular_hours"][f"{ms}ms"]["mean"]  # exact (histogram keeps the sum)
        stats[ms] = {"windows": int(n), "p50": quantile(hi, cnt, 0.5) * 1000 / ms,
                     "p99": quantile(hi, cnt, 0.99) * 1000 / ms, "p99.9": quantile(hi, cnt, 0.999) * 1000 / ms,
                     "max": float(hi.max()) * 1000 / ms, "mean": mean * 1000 / ms}
    if not stats:
        plt.close(fig)
        return stats
    mean_rate = stats[1000]["mean"]
    ax.axhline(mean_rate, color=REFERENCE, linewidth=1, linestyle="--")
    ax.text(1e5, mean_rate * 0.85, f"mean over regular hours ({mean_rate:,.0f} msgs/s)", color=REFERENCE,
            fontsize=8, ha="right", va="top")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xticks([2, 10, 100, 1e3, 1e4, 1e5], ["50%", "90%", "99%", "99.9%", "99.99%", "99.999%"])
    ax.yaxis.set_major_locator(ticker.LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, _: f"{y:,.0f}"))
    ax.yaxis.set_minor_formatter(ticker.NullFormatter())
    ax.set_xlabel("percentile of windows (09:30-16:00, empty windows counted)")
    ax.set_ylabel("message rate in the window (msgs/s)")
    style(ax, "Burstiness: message rate per window, by window width")
    ax.legend(fontsize=8, loc="upper left", frameon=False)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)
    return stats


def load_symbols(rd: Path) -> list[dict]:
    with (rd / "symbols.csv").open() as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        r["book_messages"] = int(r["book_messages"])
    rows.sort(key=lambda r: r["book_messages"], reverse=True)
    return rows


def plot_skew(rows: list[dict], out: Path, plt) -> None:
    msgs = np.array([r["book_messages"] for r in rows], dtype=float)
    share = np.cumsum(msgs) / msgs.sum() * 100
    k = np.arange(1, len(msgs) + 1)
    fig, ax = plt.subplots(figsize=(9, 4))
    series(ax, 0, k, share, "cumulative share")
    for top in (10, 100, 1000):
        if top <= len(share):
            ax.annotate(f"top {top}: {share[top - 1]:.1f}%", (top, share[top - 1]), xytext=(-6, 8),
                        textcoords="offset points", fontsize=8, color="#3d3c39", ha="right")
            ax.plot([top], [share[top - 1]], "o", color=SERIES_COLORS[0], markersize=5)
    ax.set_xscale("log")
    ax.set_xlabel(f"top-K symbols by book messages (K, {len(msgs):,} symbols had book activity)")
    ax.set_ylabel("% of all book messages")
    ax.set_ylim(0, 100)
    style(ax, "Symbol skew: share of book messages in the busiest K symbols")
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def fmt_ns(v: float) -> str:
    for unit, scale in (("s", 1e9), ("ms", 1e6), ("us", 1e3)):
        if v >= scale:
            x = v / scale
            return f"{x:,.0f} {unit}" if x >= 100 else f"{x:.3g} {unit}"
    return f"{v:.0f} ns"


def tables(rd: Path, summary: dict, rows: list[dict], burst: dict) -> str:
    out = []
    mix = list(csv.DictReader((rd / "message_mix.csv").open()))
    total = sum(int(m["count"]) for m in mix)
    out.append("### Message mix\n\n| type | count | share |\n|---|---:|---:|")
    for m in sorted(mix, key=lambda m: -int(m["count"])):
        out.append(f"| `{m['type']}` | {int(m['count']):,} | {int(m['count']) / total * 100:.3f}% |")
    out.append(f"| **total** | **{total:,}** | |\n")

    out.append("### Burstiness (09:30-16:00; rates in msgs/s)\n\n| window | windows | mean | p50 | p99 | p99.9 | max | max / mean |\n|---|---:|---:|---:|---:|---:|---:|---:|")
    for ms, s in burst.items():
        out.append(f"| {ms} ms | {s['windows']:,} | {s['mean']:,.0f} | {s['p50']:,.0f} | {s['p99']:,.0f} | "
                   f"{s['p99.9']:,.0f} | {s['max']:,.0f} | {s['max'] / s['mean']:.1f}x |")
    out.append("")

    out.append("### Order lifetimes (bucket upper bounds; shares count only buckets entirely below the limit)\n\n"
               "| ended by | orders | <= 100 us | <= 1 ms | <= 1 s | p50 | p90 | p99 | max |\n"
               "|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for name in ["executed", "cancelled", "deleted", "replaced"]:
        hi, cnt = load_hist(rd / f"lifetime_{name}.hist.csv")
        n = cnt.sum()
        if n == 0:
            out.append(f"| {name} | 0 | | | | | | | |")
            continue
        within = [f"{cnt[hi <= lim].sum() / n * 100:.1f}%" for lim in (1e5, 1e6, 1e9)]
        qs = [fmt_ns(quantile(hi, cnt, q)) for q in (0.5, 0.9, 0.99)]
        out.append(f"| {name} | {int(n):,} | " + " | ".join(within + qs) + f" | {fmt_ns(hi[cnt > 0].max())} |")
    out.append("")

    out.append("### Busiest symbols\n\n| rank | symbol | book messages | price range (ticks) |\n|---:|---|---:|---:|")
    for i, r in enumerate(rows[:10], 1):
        out.append(f"| {i} | {r['symbol']} | {r['book_messages']:,} | {int(r['range_ticks']):,} |")
    out.append("")

    rng = np.array([int(r["range_ticks"]) for r in rows if int(r["range_ticks"]) > 0])
    out.append("### Day price range per symbol (ticks, symbols with adds; includes far-away stub quotes)\n\n| p50 | p90 | p99 | max |\n|---:|---:|---:|---:|")
    out.append(" | ".join(["", *[f"{np.percentile(rng, p):,.0f}" for p in (50, 90, 99)], f"{rng.max():,}", ""]).strip())
    out.append("")

    snaps = list(csv.DictReader((rd / "depth_snapshots.csv").open()))
    cols = list(snaps[0].keys())
    out.append("### Depth snapshots (every 30 min of feed time, all non-empty book sides)\n")
    out.append("| " + " | ".join(cols) + " |")
    out.append("|" + "---:|" * len(cols))
    for s in snaps:
        out.append("| " + " | ".join(f"{int(float(s[c])):,}" for c in cols) + " |")
    out.append("")
    return "\n".join(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    rd = Path(sys.argv[1])
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    summary = json.loads((rd / "summary.json").read_text())
    rows = load_symbols(rd)
    plot_distance(summary, rd / "e1-distance.png", plt)
    plot_lifetimes(rd, rd / "e1-lifetimes.png", plt)
    burst = plot_burstiness(rd, summary, rd / "e1-burstiness.png", plt)
    plot_skew(rows, rd / "e1-symbol-skew.png", plt)
    (rd / "e1-tables.md").write_text(tables(rd, summary, rows, burst))
    print(f"wrote e1-*.png and e1-tables.md in {rd}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
