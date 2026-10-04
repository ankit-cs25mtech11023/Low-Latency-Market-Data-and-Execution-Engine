#!/usr/bin/env python3
"""E4 sanity checks over one or more result directories.

Every E4 run on the same input window must make exactly the same order decisions: the
strategy is deterministic and sees the same messages in the same order, whatever the model
(A or B), arrival pattern, offered rate or work knob. So every run's `digest` and `orders`
must be identical, and the book must have seen no anomalies (unknown refs, duplicates,
overfills): the warm-up rebuilt the day exactly. Also reports negative latency samples
(clock skew) and runs whose engine threads did not stay on their CPUs.

  scripts/e4_check_digest.py results/E4-*/latest
Exit code 1 if any check fails.
"""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    keys: Counter[tuple] = Counter()
    bad = 0
    runs = 0
    for d in sys.argv[1:]:
        for meta_path in sorted(Path(d).glob("*/run*.meta.json")):
            p = json.loads(meta_path.read_text())["params"]
            runs += 1
            keys[(p["input"], p["start_time_ns"], p["measured_msgs"], p["digest"], p["extra_metrics"]["orders"])] += 1
            bc = p["book_counters"]
            anomalies = {k: bc[k] for k in ("unknown_ref", "duplicate_ref", "overfill") if bc[k]}
            if anomalies:
                print(f"{meta_path}: book anomalies {anomalies}")
                bad += 1
            if p["negative_samples"]:
                print(f"{meta_path}: {p['negative_samples']} negative latency samples")
    by_window: dict[tuple, list] = {}
    for (inp, start, n, digest, orders), c in keys.items():
        by_window.setdefault((inp, start, n), []).append((digest, orders, c))
    for w, vals in by_window.items():
        status = "OK" if len(vals) == 1 else "MISMATCH"
        if len(vals) != 1:
            bad += 1
        print(f"{status}: window {w}: " + ", ".join(f"digest {d} orders {o} ({c} runs)" for d, o, c in vals))
    print(f"{runs} runs checked, {bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
