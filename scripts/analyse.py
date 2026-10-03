#!/usr/bin/env python3
"""Turns raw run_bench.py results into percentiles with confidence intervals and plots.

Statistics (master plan §9):
  1. Per run: percentiles from that run's histogram (nearest-rank, same definition as the
     C++ histogram), converted ticks -> ns with that run's own TSC calibration.
  2. Across runs: the median of the per-run values, with a 95% bootstrap confidence
     interval (resample the runs with replacement many times, take the median each time,
     report the 2.5th..97.5th percentile of those medians).
  3. A percentile is only reported if every run has enough samples for it
     (p99 >= 1e4, p99.9 >= 1e5, p99.99 >= 1e6); otherwise it is left blank.
  4. A-vs-B claims use the bootstrap CI of the difference of medians; a difference is only
     called real if that CI excludes 0 (Mann-Whitney U p-value is shown alongside).

Usage:
  scripts/analyse.py summary results/E0-timers/latest            # table + plots
  scripts/analyse.py compare results/X/latest varA varB --metric p99
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

QUANTILES = [("p50", 0.50), ("p90", 0.90), ("p99", 0.99), ("p99.9", 0.999), ("p99.99", 0.9999)]
MIN_SAMPLES = {"p50": 1, "p90": 1, "p99": 10_000, "p99.9": 100_000, "p99.99": 1_000_000}
BOOT = 10_000


@dataclass
class Run:
    name: str
    low: np.ndarray
    high: np.ndarray
    count: np.ndarray
    meta: dict
    divisor: float = 1.0
    ticks_per_ns: float = 1.0
    total: int = 0
    metrics_ns: dict = field(default_factory=dict)

    def quantile_ticks(self, q: float) -> float:
        """Nearest-rank quantile: smallest bucket whose cumulative count reaches ceil(q*N)."""
        if q >= 1.0:
            return float(self.high[-1])
        rank = max(1, math.ceil(q * self.total))
        idx = int(np.searchsorted(np.cumsum(self.count), rank))
        return float(self.high[idx])


def load_run(hist_csv: Path) -> Run:
    meta = json.loads(Path(str(hist_csv).replace(".hist.csv", ".meta.json")).read_text())
    data = np.loadtxt(hist_csv, delimiter=",", skiprows=1, dtype=np.float64, ndmin=2)
    r = Run(hist_csv.name.split(".")[0], data[:, 0], data[:, 1], data[:, 2].astype(np.int64), meta)
    r.divisor = float(meta["params"].get("value_divisor", 1))
    r.ticks_per_ns = float(meta["tsc_calibration"]["ticks_per_ns"])
    r.total = int(r.count.sum())
    to_ns = lambda t: t / r.divisor / r.ticks_per_ns  # noqa: E731
    for name, q in QUANTILES:
        r.metrics_ns[name] = to_ns(r.quantile_ticks(q)) if r.total >= MIN_SAMPLES[name] else math.nan
    r.metrics_ns["max"] = to_ns(float(meta["summary_ticks"]["max"]))
    r.metrics_ns["mean"] = to_ns(float(meta["summary_ticks"]["mean"]))
    r.metrics_ns["min"] = to_ns(float(meta["summary_ticks"]["min"]))
    return r


def load_variants(result_dir: Path) -> dict[str, list[Run]]:
    out: dict[str, list[Run]] = {}
    for vdir in sorted(p for p in result_dir.iterdir() if p.is_dir()):
        runs = [load_run(f) for f in sorted(vdir.glob("run*.hist.csv"))]
        if runs:
            out[vdir.name] = runs
    return out


def bootstrap_median_ci(x: np.ndarray, rng: np.random.Generator, level: float = 0.95) -> tuple[float, float]:
    x = x[~np.isnan(x)]
    if len(x) < 2:
        return (math.nan, math.nan)
    meds = np.median(rng.choice(x, size=(BOOT, len(x)), replace=True), axis=1)
    a = (1 - level) / 2
    return float(np.quantile(meds, a)), float(np.quantile(meds, 1 - a))


def bootstrap_diff_ci(a: np.ndarray, b: np.ndarray, rng: np.random.Generator) -> tuple[float, float, float]:
    ma = np.median(rng.choice(a, size=(BOOT, len(a)), replace=True), axis=1)
    mb = np.median(rng.choice(b, size=(BOOT, len(b)), replace=True), axis=1)
    d = ma - mb
    return float(np.median(a) - np.median(b)), float(np.quantile(d, 0.025)), float(np.quantile(d, 0.975))


def fmt(v: float) -> str:
    if math.isnan(v):
        return ""
    return f"{v:.3f}" if abs(v) < 10 else f"{v:.1f}" if abs(v) < 1000 else f"{v:.0f}"


def summary(result_dir: Path, plot: bool) -> None:
    rng = np.random.default_rng(12345)
    variants = load_variants(result_dir)
    if not variants:
        sys.exit(f"no runs found under {result_dir}")
    metrics = ["min"] + [n for n, _ in QUANTILES] + ["max", "mean"]
    rows = []
    for v, runs in variants.items():
        for m in metrics:
            x = np.array([r.metrics_ns[m] for r in runs])
            if np.all(np.isnan(x)):
                continue
            lo, hi = bootstrap_median_ci(x, rng)
            rows.append({"variant": v, "metric": m, "median_ns": float(np.nanmedian(x)), "ci95_lo_ns": lo,
                         "ci95_hi_ns": hi, "runs": int(np.sum(~np.isnan(x))),
                         "samples_per_run": int(np.median([r.total for r in runs])),
                         "ticks_per_ns": float(np.median([r.ticks_per_ns for r in runs]))})
    with open(result_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    # Markdown table: one row per variant, median [CI] per metric.
    show = ["min", "p50", "p99", "p99.9", "max", "mean"]
    lines = ["| variant | runs | samples/run | " + " | ".join(f"{m} ns (95% CI)" for m in show) + " |",
             "|---|---|---|" + "---|" * len(show)]
    for v in variants:
        cells = []
        for m in show:
            r = next((x for x in rows if x["variant"] == v and x["metric"] == m), None)
            cells.append("" if r is None else f"{fmt(r['median_ns'])} [{fmt(r['ci95_lo_ns'])}, {fmt(r['ci95_hi_ns'])}]")
        any_row = next(x for x in rows if x["variant"] == v)
        lines.append(f"| {v} | {any_row['runs']} | {any_row['samples_per_run']} | " + " | ".join(cells) + " |")
    table = "\n".join(lines)
    (result_dir / "summary.md").write_text(table + "\n")
    print(table)

    if plot:
        percentile_plot(variants, result_dir / "percentiles.png")


def percentile_plot(variants: dict[str, list[Run]], path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(9, 5.5))
    qs = 1 - np.logspace(0, -6, 400)
    for v, runs in variants.items():
        # Merge all runs of a variant (same binary, same calibration within ~ppm).
        tpn = float(np.median([r.ticks_per_ns for r in runs]))
        div = runs[0].divisor
        highs = np.concatenate([r.high for r in runs])
        counts = np.concatenate([r.count for r in runs])
        order = np.argsort(highs, kind="stable")
        hi, cnt = highs[order], counts[order]
        cum = np.cumsum(cnt)
        n = cum[-1]
        qs_v = qs[qs <= 1 - 1 / n] if n > 1 else qs[:1]
        idx = np.searchsorted(cum, np.maximum(1, np.ceil(qs_v * n)))
        ax.plot(1 / (1 - qs_v), hi[idx] / div / tpn, label=v)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xticks([1, 2, 10, 100, 1e3, 1e4, 1e5, 1e6])
    ax.set_xticklabels(["0%", "50%", "90%", "99%", "99.9%", "99.99%", "99.999%", "99.9999%"])
    ax.set_xlabel("percentile")
    ax.set_ylabel("latency (ns)")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    print(f"plot: {path}")


def compare(result_dir: Path, a: str, b: str, metric: str) -> None:
    rng = np.random.default_rng(12345)
    variants = load_variants(result_dir)
    xa = np.array([r.metrics_ns[metric] for r in variants[a]])
    xb = np.array([r.metrics_ns[metric] for r in variants[b]])
    d, lo, hi = bootstrap_diff_ci(xa, xb, rng)
    try:
        from scipy.stats import mannwhitneyu
        p = float(mannwhitneyu(xa, xb, alternative="two-sided").pvalue)
    except ImportError:
        p = math.nan
    verdict = "significant" if (lo > 0 or hi < 0) else "NOT significant (CI includes 0)"
    print(f"{metric}: median({a}) - median({b}) = {d:.3f} ns, 95% CI [{lo:.3f}, {hi:.3f}], "
          f"Mann-Whitney p = {p:.4g} -> {verdict}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary")
    s.add_argument("dir", type=Path)
    s.add_argument("--no-plot", action="store_true")
    c = sub.add_parser("compare")
    c.add_argument("dir", type=Path)
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--metric", default="p50")
    args = ap.parse_args()
    d = args.dir.resolve()
    if args.cmd == "summary":
        summary(d, not args.no_plot)
    else:
        compare(d, args.a, args.b, args.metric)
    return 0


if __name__ == "__main__":
    sys.exit(main())
