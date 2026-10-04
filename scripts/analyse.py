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
  4. Hardware counters (if the run recorded params.counters, measured around the timed
     region only) are divided by the number of operations (samples x value_divisor) and
     summarized the same way: cycles/op, instructions/op, IPC, effective core GHz.
  5. A-vs-B claims use the bootstrap CI of the difference of medians; a difference is only
     called real if that CI excludes 0 (Mann-Whitney U p-value is shown alongside). The ratio
     of medians (speedup) is printed with its own bootstrap CI.

Usage:
  scripts/analyse.py summary results/E0-timers/latest            # table + plots
  scripts/analyse.py summary DIR --panels 'TSC:rdtsc,rdtscp;OS:steady_clock'  # grouped plot panels
  scripts/analyse.py compare results/X/latest varA varB --metric p99
  scripts/analyse.py skew results/E0-skew/latest                 # cross-core TSC offsets
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
    counter_metrics: dict = field(default_factory=dict)

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
    r.counter_metrics = counter_metrics(meta["params"].get("counters"), r.total * r.divisor)
    if meta["params"].get("heap_allocations_measured") is not None and r.total > 0:
        # Counted by an operator-new shim around the timed region only (E2 driver).
        r.counter_metrics["heap allocs/op"] = meta["params"]["heap_allocations_measured"] / (r.total * r.divisor)
    return r


def counter_metrics(c: dict | None, ops: float) -> dict:
    """Per-operation counter values. The counts cover the whole timed region, so per-op values
    include the amortized loop and histogram-record overhead (1/value_divisor of it per op)."""
    if not c or ops <= 0:
        return {}
    out = {}
    if "cycles" in c:
        out["cycles/op"] = c["cycles"] / ops
        if c.get("time_running_ns"):
            out["core GHz"] = c["cycles"] / c["time_running_ns"]
    if "instructions" in c:
        out["instr/op"] = c["instructions"] / ops
        if c.get("cycles"):
            out["IPC"] = c["instructions"] / c["cycles"]
    if c.get("time_enabled_ns"):
        out["pmu running %"] = 100.0 * c["time_running_ns"] / c["time_enabled_ns"]
    for k in ("branch-misses", "cache-misses", "L1-dcache-load-misses", "dTLB-load-misses", "page-faults",
              "context-switches"):
        if k in c:
            out[f"{k}/op"] = c[k] / ops
    return out


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


def bootstrap_ratio_ci(a: np.ndarray, b: np.ndarray, rng: np.random.Generator) -> tuple[float, float, float]:
    """Ratio of medians (e.g. a speedup "A takes 2.6x as long as B") with a 95% bootstrap CI:
    each side's runs are resampled independently, as in bootstrap_diff_ci."""
    ma = np.median(rng.choice(a, size=(BOOT, len(a)), replace=True), axis=1)
    mb = np.median(rng.choice(b, size=(BOOT, len(b)), replace=True), axis=1)
    q = ma / mb
    return float(np.median(a) / np.median(b)), float(np.quantile(q, 0.025)), float(np.quantile(q, 0.975))


def fmt(v: float) -> str:
    if math.isnan(v):
        return ""
    if 0 < abs(v) < 0.001:
        return f"{v:.2e}"  # e.g. allocations per message: keep rare-but-nonzero visible
    return f"{v:.3f}" if abs(v) < 10 else f"{v:.1f}" if abs(v) < 1000 else f"{v:.0f}"


def summary(result_dir: Path, plot: bool, panels: str | None = None) -> None:
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
    ctable = counter_table(variants, rng, result_dir)
    if ctable:
        table += "\n\nHardware counters, timed region only (median across runs [95% CI]):\n\n" + ctable
    (result_dir / "summary.md").write_text(table + "\n")
    print(table)

    if plot:
        percentile_plot(variants, result_dir / "percentiles.png", panels)


def counter_table(variants: dict[str, list[Run]], rng: np.random.Generator, result_dir: Path) -> str:
    names: list[str] = []
    for runs in variants.values():
        for r in runs:
            names += [k for k in r.counter_metrics if k not in names]
    if not names:
        return ""
    rows, lines = [], ["| variant | " + " | ".join(names) + " |", "|---|" + "---|" * len(names)]
    for v, runs in variants.items():
        cells = []
        for m in names:
            x = np.array([r.counter_metrics.get(m, math.nan) for r in runs])
            if np.all(np.isnan(x)):
                cells.append("")
                continue
            lo, hi = bootstrap_median_ci(x, rng)
            med = float(np.nanmedian(x))
            rows.append({"variant": v, "metric": m, "median": med, "ci95_lo": lo, "ci95_hi": hi,
                         "runs": int(np.sum(~np.isnan(x)))})
            cells.append(f"{fmt(med)} [{fmt(lo)}, {fmt(hi)}]")
        lines.append(f"| {v} | " + " | ".join(cells) + " |")
    with open(result_dir / "counters.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    return "\n".join(lines)


# Fixed categorical order (validated for CVD separation on a light surface). Hues are assigned
# by position within a panel and never cycled: a panel holds at most len(SERIES_COLORS) lines.
# Line style is a second encoding so identity never depends on colour alone.
SERIES_COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
SERIES_STYLES = ["-", "--", "-.", ":", (0, (5, 1, 1, 1, 1, 1))]


def parse_panels(spec: str | None, names: list[str]) -> list[tuple[str, list[str]]]:
    """'Title:a,b;Title2:c' -> [(title, [variant names])]. Names match a variant either exactly
    or by the value after '=' (so 'rdtsc' selects 'variant=rdtsc'). Without a spec, variants
    are chunked in order into panels of at most len(SERIES_COLORS)."""
    cap = len(SERIES_COLORS)
    if not spec:
        return [("", names[i:i + cap]) for i in range(0, len(names), cap)]

    def find(key: str) -> str:
        hits = [n for n in names if n == key or n.split("=", 1)[-1] == key]
        if len(hits) != 1:
            sys.exit(f"--panels: '{key}' matches {hits or 'no variant'}")
        return hits[0]

    panels = []
    for part in spec.split(";"):
        title, _, keys = part.rpartition(":")
        sel = [find(k.strip()) for k in keys.split(",") if k.strip()]
        if len(sel) > cap:
            sys.exit(f"--panels: panel '{title}' has {len(sel)} series; the limit is {cap} (split it)")
        panels.append((title.strip(), sel))
    return panels


def percentile_plot(variants: dict[str, list[Run]], path: Path, panels_spec: str | None = None) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import ticker

    panels = parse_panels(panels_spec, list(variants))
    fig, axes = plt.subplots(len(panels), 1, figsize=(9, 3.6 * len(panels)), squeeze=False)
    qs = 1 - np.logspace(0, -6, 400)
    for ax, (title, names) in zip(axes[:, 0], panels):
        for i, v in enumerate(names):
            runs = variants[v]
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
            ax.plot(1 / (1 - qs_v), hi[idx] / div / tpn, label=v.split("=", 1)[-1], color=SERIES_COLORS[i],
                    linestyle=SERIES_STYLES[i], linewidth=2)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks([1, 2, 10, 100, 1e3, 1e4, 1e5, 1e6])
        ax.set_xticklabels(["0%", "50%", "90%", "99%", "99.9%", "99.99%", "99.999%", "99.9999%"])
        ax.set_ylabel("latency (ns)")
        # Label 1-2-5 steps so short log ranges (less than a decade) still have readable ticks.
        ax.yaxis.set_major_locator(ticker.LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
        ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, _: f"{y:g}"))
        ax.yaxis.set_minor_formatter(ticker.NullFormatter())
        if title:
            ax.set_title(title, loc="left", fontsize=11)
        ax.grid(True, which="major", color="#d8d7d3", linewidth=0.6)
        ax.grid(True, which="minor", color="#ecebe8", linewidth=0.4)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        ax.legend(fontsize=8, loc="upper left", frameon=False)
    axes[-1, 0].set_xlabel("percentile (all runs merged)")
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
    q, qlo, qhi = bootstrap_ratio_ci(xa, xb, rng)
    print(f"{metric}: median({a}) / median({b}) = {q:.3f}x, 95% CI [{qlo:.3f}, {qhi:.3f}]")


def skew(result_dir: Path) -> None:
    """Cross-core TSC offsets from e0_skew runs (<variant>/runNN.skew.csv).

    For each ordered pair A->B a run reports offset(B rel. A) estimated NTP-style from the 1%
    lowest-RTT round trips, and its bound (RTT/2). A one-direction estimate mixes two things:
      * real skew s:      TSC_B - TSC_A; it flips sign when the roles are swapped;
      * path asymmetry a: B's stamp sits off-centre in the round trip (polling, rdtsc latency);
                          it has the same sign in both directions.
    off(A->B) = s + a and off(B->A) = -s + a, so s = (off_AB - off_BA)/2, a = (off_AB + off_BA)/2.
    """
    rng = np.random.default_rng(12345)
    per_pair: dict[tuple[int, int], list[dict]] = {}
    for f in sorted(result_dir.glob("*/run*.skew.csv")):
        meta = json.loads(Path(str(f).replace(".skew.csv", ".meta.json")).read_text())
        tpn = float(meta["tsc_calibration"]["ticks_per_ns"])
        with open(f) as fh:
            for row in csv.DictReader(fh):
                key = (int(row["cpu_a"]), int(row["cpu_b"]))
                per_pair.setdefault(key, []).append({
                    "off": float(row["offset_median_best1pct"]) / tpn, "bound": float(row["bound_best1pct"]) / tpn,
                    "half_rtt_p50": float(row["p50_rtt"]) / 2 / tpn, "violations": int(row["violations"]),
                    "run": f.name})
    if not per_pair:
        sys.exit(f"no *.skew.csv under {result_dir}")
    env = result_dir / "env.json"
    ncpu = int(json.loads(env.read_text())["before"]["logical_cpus"]) if env.exists() else max(max(k) for k in per_pair) + 1
    rows = []
    for (a, b), ab in sorted(per_pair.items()):
        if a > b or (b, a) not in per_pair:
            continue
        ba = per_pair[(b, a)]
        # Pair the runs by run index (with --pairs all, both directions come from the same process).
        by_run = {r["run"]: r for r in ba}
        common = [(x, by_run[x["run"]]) for x in ab if x["run"] in by_run]
        s_ns = np.array([(x["off"] - y["off"]) / 2 for x, y in common])
        a_ns = np.array([(x["off"] + y["off"]) / 2 for x, y in common])
        bound = np.array([max(x["bound"], y["bound"]) for x, y in common])
        half_rtt = np.array([(x["half_rtt_p50"] + y["half_rtt_p50"]) / 2 for x, y in common])
        s_lo, s_hi = bootstrap_median_ci(s_ns, rng)
        rows.append({"cpu_a": a, "cpu_b": b, "relation": "SMT sibling" if abs(a - b) == ncpu // 2 else "cross-core",
                     "runs": len(common), "skew_ns": float(np.median(s_ns)), "skew_ci95_lo": s_lo,
                     "skew_ci95_hi": s_hi, "asymmetry_ns": float(np.median(a_ns)),
                     "bound_ns": float(np.median(bound)), "half_rtt_p50_ns": float(np.median(half_rtt)),
                     "violations": sum(x["violations"] + y["violations"] for x, y in common)})
    with open(result_dir / "skew.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    lines = ["| CPUs | relation | runs | skew ns (95% CI) | asymmetry ns | bound ns (RTT/2, best 1%) | "
             "one-way p50 ns | violations |", "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        lines.append(f"| {r['cpu_a']}-{r['cpu_b']} | {r['relation']} | {r['runs']} | {fmt(r['skew_ns'])} "
                     f"[{fmt(r['skew_ci95_lo'])}, {fmt(r['skew_ci95_hi'])}] | {fmt(r['asymmetry_ns'])} | "
                     f"{fmt(r['bound_ns'])} | {fmt(r['half_rtt_p50_ns'])} | {r['violations']} |")
    worst = max(rows, key=lambda r: abs(r["skew_ns"]))
    lines.append("")
    lines.append(f"Largest |skew| estimate: {fmt(worst['skew_ns'])} ns (CPUs {worst['cpu_a']}-{worst['cpu_b']}); "
                 f"largest bound: {fmt(max(r['bound_ns'] for r in rows))} ns.")
    table = "\n".join(lines)
    (result_dir / "skew.md").write_text(table + "\n")
    print(table)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary")
    s.add_argument("dir", type=Path)
    s.add_argument("--no-plot", action="store_true")
    s.add_argument("--panels", help="plot panels as 'Title:varA,varB;Title2:varC' (max 5 series per panel)")
    c = sub.add_parser("compare")
    c.add_argument("dir", type=Path)
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--metric", default="p50")
    k = sub.add_parser("skew")
    k.add_argument("dir", type=Path)
    args = ap.parse_args()
    d = args.dir.resolve()
    if args.cmd == "summary":
        summary(d, not args.no_plot, args.panels)
    elif args.cmd == "skew":
        skew(d)
    else:
        compare(d, args.a, args.b, args.metric)
    return 0


if __name__ == "__main__":
    sys.exit(main())
