#!/usr/bin/env python3
"""Splits an E2 result directory recorded with `e2_book --mode both`.

In that mode every run writes two result sets next to each other in its variant directory:
  <variant>/run<r>.hist.csv / .meta.json           batch timing (throughput)
  <variant>/run<r>_permsg.hist.csv / .meta.json    per-message timing (latency distribution)
analyse.py treats every run*.hist.csv in a variant directory as one run of that variant, so
the per-message files are moved into a sibling variant directory "<variant>__method=permsg"
(and the batch files' directory is renamed "<variant>__method=batch"). Logs stay with the
batch files.

  scripts/e2_split.py results/E2-book/<timestamp>
"""
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    for vdir in sorted(p for p in root.iterdir() if p.is_dir() and "__method=" not in p.name):
        permsg = sorted(vdir.glob("run*_permsg.*"))
        if not permsg:
            continue
        pdir = root / f"{vdir.name}__method=permsg"
        pdir.mkdir(exist_ok=True)
        for f in permsg:
            f.rename(pdir / f.name.replace("_permsg", ""))
        vdir.rename(root / f"{vdir.name}__method=batch")
        print(f"{vdir.name}: {len(permsg) // 2} per-message runs split off")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
