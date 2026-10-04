#!/usr/bin/env python3
"""E4 analysis: closed-loop capacity and latency vs offered load, per model and work level.

Uses the same statistics as scripts/analyse.py (per-run percentiles from the histogram,
median across N runs, 95% bootstrap CI of the median; A-vs-B ratios with their own
bootstrap CI), but groups runs by E4's parameters instead of printing one row per variant.

  scripts/e4_analyse.py capacity results/E4-capacity/latest
      -> capacity.md / capacity.csv in that directory: msgs/s, ns/msg, cycles and
         instructions per message (summed over the engine threads), B/A ratio.
  scripts/e4_analyse.py plot OUTDIR/sweep.csv CAPDIR/capacity.csv OUT.png --metric p99
      -> the hero plot: latency vs offered load, one panel per work level.
  scripts/e4_analyse.py sweep OUTDIR results/E4-sweep-w0/latest results/E4-sweep-w200/latest ...
      -> OUTDIR/sweep.csv / sweep.md: p50, p99, p99.9, max latency per
         (work, arrival, model, rate), plus the A/B ratio at each point.

Why the capacity numbers come from extra_metrics.msgs_per_s and not from the histogram: in
throughput mode the histogram holds the time per block of 1 024 messages (value_divisor), so
its mean is ns/msg over blocks; msgs_per_s is the window's message count over the whole
window's wall time, which is what "capacity" means. Both are reported.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyse import bootstrap_median_ci, bootstrap_ratio_ci, load_run  # noqa: E402

RNG_SEED = 12345


def med_ci(x: list[float], rng: np.random.Generator) -> tuple[float, float, float]:
    a = np.asarray(x, dtype=float)
    lo, hi = bootstrap_median_ci(a, rng)
    return float(np.median(a)), lo, hi


def f3(m: float, lo: float, hi: float, nd: int = 0) -> str:
    if math.isnan(m):
        return ""
    fmt = f"{{:,.{nd}f}}"
    return f"{fmt.format(m)} [{fmt.format(lo)}, {fmt.format(hi)}]"


def iter_runs(result_dir: Path):
    for meta_path in sorted(result_dir.glob("*/run*.meta.json")):
        yield meta_path, json.loads(meta_path.read_text())


# ---------------------------------------------------------------- capacity
def capacity(result_dir: Path) -> None:
    rng = np.random.default_rng(RNG_SEED)
    groups: dict[tuple[str, float], dict[str, list[float]]] = {}
    for meta_path, meta in iter_runs(result_dir):
        p = meta["params"]
        if p["mode"] != "throughput":
            continue
        n = float(p["measured_msgs"])
        g = groups.setdefault((p["model"], float(p["work_ns"])), {k: [] for k in ("mps", "ns", "cyc", "ins", "llc")})
        g["mps"].append(float(p["extra_metrics"]["msgs_per_s"]))
        g["ns"].append(1e9 / float(p["extra_metrics"]["msgs_per_s"]))
        c = p.get("counters") or {}
        g["cyc"].append(c.get("cycles", math.nan) / n)
        g["ins"].append(c.get("instructions", math.nan) / n)
        g["llc"].append(c.get("cache-misses", math.nan) / n)
    if not groups:
        sys.exit(f"no throughput runs under {result_dir}")

    rows = []
    lines = [
        "| work ns | model | runs | capacity M msg/s | ns/msg | cycles/msg (all engine threads) | instr/msg | cache-misses/msg |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for (model, work) in sorted(groups, key=lambda k: (k[1], k[0])):
        g = groups[(model, work)]
        mps = med_ci([v / 1e6 for v in g["mps"]], rng)
        ns = med_ci(g["ns"], rng)
        cyc, ins, llc = med_ci(g["cyc"], rng), med_ci(g["ins"], rng), med_ci(g["llc"], rng)
        lines.append(f"| {work:g} | {model.upper()} | {len(g['mps'])} | {f3(*mps, nd=3)} | {f3(*ns, nd=1)} | "
                     f"{f3(*cyc)} | {f3(*ins)} | {f3(*llc, nd=1)} |")
        rows.append({"work_ns": work, "model": model, "runs": len(g["mps"]), "capacity_mps": mps[0],
                     "capacity_mps_lo": mps[1], "capacity_mps_hi": mps[2], "ns_per_msg": ns[0],
                     "cycles_per_msg": cyc[0], "instr_per_msg": ins[0], "cache_misses_per_msg": llc[0]})

    lines += ["", "B / A (ratio of medians, 95% bootstrap CI):", "",
              "| work ns | capacity B/A | cycles/msg B/A |", "|---|---|---|"]
    for work in sorted({w for _, w in groups}):
        a, b = groups.get(("a", work)), groups.get(("b", work))
        if not a or not b:
            continue
        cap = bootstrap_ratio_ci(np.asarray(b["mps"]), np.asarray(a["mps"]), rng)
        cyc = bootstrap_ratio_ci(np.asarray(b["cyc"]), np.asarray(a["cyc"]), rng)
        lines.append(f"| {work:g} | {cap[0]:.3f}× [{cap[1]:.3f}, {cap[2]:.3f}] | {cyc[0]:.2f}× [{cyc[1]:.2f}, {cyc[2]:.2f}] |")

    (result_dir / "capacity.md").write_text("\n".join(lines) + "\n")
    with open(result_dir / "capacity.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print("\n".join(lines))


# ---------------------------------------------------------------- sweep
METRICS = ("p50", "p99", "p99.9", "max")


def sweep(outdir: Path, result_dirs: list[Path]) -> None:
    rng = np.random.default_rng(RNG_SEED)
    groups: dict[tuple, dict[str, list[float]]] = {}
    for d in result_dirs:
        for meta_path, meta in iter_runs(d):
            p = meta["params"]
            if p["mode"] != "latency" or p.get("hops"):
                continue
            r = load_run(Path(str(meta_path).replace(".meta.json", ".hist.csv")))
            key = (float(p["work_ns"]), p["arrival"], p["model"], float(p["rate_msgs_per_s"]))
            g = groups.setdefault(key, {m: [] for m in METRICS + ("lag_p99",)})
            for m in METRICS:
                g[m].append(r.metrics_ns[m])
            g["lag_p99"].append(float(p["extra_metrics"].get("feeder_lag_p99_ns", math.nan)))
    if not groups:
        sys.exit("no latency runs found")

    outdir.mkdir(parents=True, exist_ok=True)
    rows = []
    for key in sorted(groups):
        work, arrival, model, rate = key
        g = groups[key]
        row = {"work_ns": work, "arrival": arrival, "model": model, "rate_mps": rate / 1e6, "runs": len(g["p50"])}
        for m in METRICS + ("lag_p99",):
            med, lo, hi = med_ci(g[m], rng)
            row[m], row[m + "_lo"], row[m + "_hi"] = med, lo, hi
        rows.append(row)
    with open(outdir / "sweep.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    lines = []
    for work in sorted({k[0] for k in groups}):
        for arrival in ("smooth", "bursty"):
            sub = [r for r in rows if r["work_ns"] == work and r["arrival"] == arrival]
            if not sub:
                continue
            lines += [f"### work {work:g} ns, {arrival} arrival", "",
                      "| rate M msg/s | model | p50 ns | p99 ns | p99.9 ns | max ns | A/B p50 | A/B p99 |",
                      "|---|---|---|---|---|---|---|---|"]
            for rate in sorted({r["rate_mps"] for r in sub}):
                ka = (work, arrival, "a", rate * 1e6)
                kb = (work, arrival, "b", rate * 1e6)
                ratio = {}
                if ka in groups and kb in groups:
                    for m in ("p50", "p99"):
                        q = bootstrap_ratio_ci(np.asarray(groups[ka][m]), np.asarray(groups[kb][m]), rng)
                        ratio[m] = f"{q[0]:.2f}× [{q[1]:.2f}, {q[2]:.2f}]"
                for r in sub:
                    if r["rate_mps"] != rate:
                        continue
                    cells = [f3(r[m], r[m + "_lo"], r[m + "_hi"]) for m in METRICS]
                    extra = [ratio.get("p50", ""), ratio.get("p99", "")] if r["model"] == "a" else ["", ""]
                    lines.append(f"| {rate:.3f} | {r['model'].upper()} | " + " | ".join(cells + extra) + " |")
            lines.append("")
    (outdir / "sweep.md").write_text("\n".join(lines))
    print("\n".join(lines))


# ---------------------------------------------------------------- plot
# Colors: Model A blue, Model B orange (validated with the dataviz palette checker: CVD
# separation 24.7, contrast >= 3:1 on the surface). Arrival is a second, non-color channel:
# smooth = solid line + circle, bursty = dashed line + square, so identity never rests on
# color alone.
COLOR = {"a": "#2a78d6", "b": "#eb6834"}
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
STYLE = {"smooth": ("-", "o"), "bursty": ("--", "s")}


def plot(sweep_csv: Path, capacity_csv: Path, out: Path, metric: str) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    rows = list(csv.DictReader(open(sweep_csv)))
    cap = {(r["model"], float(r["work_ns"])): float(r["capacity_mps"]) / 1e6 for r in csv.DictReader(open(capacity_csv))}
    works = sorted({float(r["work_ns"]) for r in rows})
    ncol = 2 if len(works) > 1 else 1
    nrow = math.ceil(len(works) / ncol)
    plt.rcParams.update({"font.size": 9, "axes.edgecolor": INK2, "axes.labelcolor": INK2,
                         "xtick.color": INK2, "ytick.color": INK2, "text.color": INK})
    fig, axes = plt.subplots(nrow, ncol, figsize=(5.2 * ncol, 3.6 * nrow), squeeze=False, facecolor=SURFACE)
    for ax, work in zip(axes.flat, works):
        ax.set_facecolor(SURFACE)
        ax.grid(True, which="major", color=GRID, linewidth=0.6)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for model in ("a", "b"):
            for arrival in ("smooth", "bursty"):
                sub = sorted((r for r in rows if float(r["work_ns"]) == work and r["model"] == model
                              and r["arrival"] == arrival), key=lambda r: float(r["rate_mps"]))
                if not sub:
                    continue
                x = [float(r["rate_mps"]) for r in sub]
                y = [float(r[metric]) / 1e3 for r in sub]
                lo = [max(0.0, (float(r[metric]) - float(r[metric + "_lo"])) / 1e3) for r in sub]
                hi = [max(0.0, (float(r[metric + "_hi"]) - float(r[metric])) / 1e3) for r in sub]
                ls, mk = STYLE[arrival]
                ax.errorbar(x, y, yerr=[lo, hi], color=COLOR[model], linestyle=ls, marker=mk, markersize=4.5,
                            linewidth=1.6, elinewidth=1.0, capsize=2, markeredgecolor=SURFACE, markeredgewidth=0.8,
                            label=f"Model {model.upper()} ({'run-to-completion' if model == 'a' else 'pipelined'}), {arrival}")
            if (model, work) in cap:
                ax.axvline(cap[(model, work)], color=COLOR[model], linestyle=":", linewidth=1.0)
                ax.annotate(f"{model.upper()} capacity", (cap[(model, work)], 1.0), xycoords=("data", "axes fraction"),
                            xytext=(-3, -3), textcoords="offset points", ha="right", va="top", rotation=90,
                            fontsize=7.5, color=INK2)
        ax.set_yscale("log")
        ax.set_title(f"strategy work {work:g} ns/msg", fontsize=9.5, color=INK, loc="left")
        ax.set_xlabel("offered load (M msg/s)")
        ax.set_ylabel(f"{metric} latency (µs, log)")
    for ax in list(axes.flat)[len(works):]:
        ax.set_visible(False)
    handles, labels = axes.flat[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False, fontsize=8.5,
               bbox_to_anchor=(0.5, 1.0))
    fig.suptitle(f"E4: {metric} end-to-end latency vs offered load (median of 10 runs, 95% CI)",
                 y=1.04, fontsize=10.5, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(out, dpi=150, bbox_inches="tight", facecolor=SURFACE)
    print(f"wrote {out}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capacity")
    c.add_argument("dir", type=Path)
    s = sub.add_parser("sweep")
    s.add_argument("outdir", type=Path)
    s.add_argument("dirs", type=Path, nargs="+")
    pl = sub.add_parser("plot")
    pl.add_argument("sweep_csv", type=Path)
    pl.add_argument("capacity_csv", type=Path)
    pl.add_argument("out", type=Path)
    pl.add_argument("--metric", default="p99", choices=METRICS)
    a = ap.parse_args()
    if a.cmd == "plot":
        plot(a.sweep_csv, a.capacity_csv, a.out, a.metric)
    elif a.cmd == "capacity":
        capacity(a.dir.resolve())
    else:
        sweep(a.outdir, [d.resolve() for d in a.dirs])
    return 0


if __name__ == "__main__":
    sys.exit(main())
