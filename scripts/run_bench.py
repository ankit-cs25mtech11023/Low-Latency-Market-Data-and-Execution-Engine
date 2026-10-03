#!/usr/bin/env python3
"""Runs an experiment as N interleaved runs per variant and stores raw results + environment.

Why interleave: slow drifts (temperature, background activity, frequency) affect all
variants equally when runs alternate A B C, B C A, ... instead of AAAA BBBB CCCC, so they
show up as run-to-run noise (which the confidence interval captures) instead of as a
fake difference between variants.

Each variant is one process invocation built from a command template:

  scripts/run_bench.py --name E0-timers --build build/release --runs 10 \
      --cmd "{bin}/e0_timers --variant {variant} --cpu 2 --out {out}" \
      --param variant=rdtsc,rdtscp,steady_clock

Several --param flags form a cross product (e.g. load x work for E4). The placeholders are
{bin} (build dir), {out} (output prefix for this run) and every --param name.

Output layout (results/ is git-ignored; curated results are copied under docs/ later):
  results/<name>/<UTC timestamp>/
      env.json         machine state (scripts/env_capture.sh), captured before and after
      manifest.json    command, variants, run order, git SHA
      <variant_id>/run<r>.hist.csv, run<r>.meta.json, run<r>.log  (+ run<r>.perf.csv)
"""
from __future__ import annotations

import argparse
import datetime as dt
import itertools
import json
import os
import random
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def sh(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, text=True, **kw)


def env_json(build: Path) -> dict:
    out = sh([str(REPO / "scripts/env_capture.sh"), str(build)], capture_output=True).stdout
    return json.loads(out)


def variant_id(params: dict[str, str]) -> str:
    return "__".join(f"{k}={v}" for k, v in params.items()) if params else "default"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True, help="experiment name, e.g. E0-timers")
    ap.add_argument("--build", default="build/release", help="CMake build directory (release preset)")
    ap.add_argument("--cmd", required=True, help="command template")
    ap.add_argument("--param", action="append", default=[], help="name=v1,v2,... (repeatable, cross product)")
    ap.add_argument("--runs", type=int, default=10, help="runs per variant (>= 10 for reported numbers)")
    ap.add_argument("--warmup-runs", type=int, default=1, help="full passes run first and discarded")
    ap.add_argument("--order", choices=["rotate", "shuffle"], default="rotate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--perf-stat", default="", help="comma-separated perf events; wraps each run in perf stat")
    ap.add_argument("--allow-dirty", action="store_true", help="allow running from an uncommitted tree")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--outdir", default="results")
    args = ap.parse_args()

    build = (REPO / args.build).resolve()
    if not args.no_build:
        sh(["cmake", "--build", str(build)])
    env_before = env_json(build)
    if env_before["git_dirty"] and not args.allow_dirty:
        print("refusing to run: working tree is dirty, results would not match a commit "
              "(commit first or pass --allow-dirty for exploratory runs)", file=sys.stderr)
        return 2
    if env_before.get("build_type") not in ("Release",):
        print(f"WARNING: build type is {env_before.get('build_type')}; reported numbers must use the release preset",
              file=sys.stderr)

    names, values = [], []
    for p in args.param:
        k, v = p.split("=", 1)
        names.append(k)
        values.append(v.split(","))
    variants = [dict(zip(names, combo)) for combo in itertools.product(*values)] if names else [{}]

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = REPO / args.outdir / args.name / stamp
    root.mkdir(parents=True)

    rng = random.Random(args.seed)
    order: list[tuple[int, int]] = []  # (run, variant index); negative run = warm-up
    for r in range(-args.warmup_runs, args.runs):
        idx = list(range(len(variants)))
        if args.order == "rotate":
            k = r % len(idx)
            idx = idx[k:] + idx[:k]
        else:
            rng.shuffle(idx)
        order += [(r, i) for i in idx]

    manifest = {
        "name": args.name, "cmd": args.cmd, "params": args.param, "runs": args.runs,
        "warmup_runs": args.warmup_runs, "order_mode": args.order, "seed": args.seed,
        "variants": [variant_id(v) for v in variants], "perf_stat": args.perf_stat,
        "git_sha": env_before["git_sha"], "git_dirty": env_before["git_dirty"],
        "started_at": env_before["captured_at"], "argv": sys.argv,
    }
    (root / "env.json").write_text(json.dumps({"before": env_before}, indent=2))

    total = len(order)
    for n, (r, i) in enumerate(order, 1):
        v = variants[i]
        vdir = root / variant_id(v)
        vdir.mkdir(exist_ok=True)
        run_name = f"warmup{-r}" if r < 0 else f"run{r:02d}"
        out = vdir / run_name
        cmd = shlex.split(args.cmd.format(bin=str(build), out=str(out), **v))
        if args.perf_stat:
            cmd = ["perf", "stat", "-x", ",", "-e", args.perf_stat, "-o", f"{out}.perf.csv", "--"] + cmd
        print(f"[{n}/{total}] {run_name} {variant_id(v)}", flush=True)
        with open(f"{out}.log", "w") as log:
            res = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, text=True)
        if res.returncode != 0:
            print(f"run failed (exit {res.returncode}); see {out}.log", file=sys.stderr)
            return 1

    env_after = env_json(build)
    (root / "env.json").write_text(json.dumps({"before": env_before, "after": env_after}, indent=2))
    manifest["finished_at"] = env_after["captured_at"]
    manifest["order"] = [[r, variant_id(variants[i])] for r, i in order]
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2))
    latest = REPO / args.outdir / args.name / "latest"
    if latest.is_symlink() or latest.exists():
        latest.unlink()
    latest.symlink_to(stamp)
    print(f"results: {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
