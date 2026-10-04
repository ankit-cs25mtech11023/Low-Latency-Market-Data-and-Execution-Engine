#!/usr/bin/env bash
# E4 latency-vs-offered-load sweep: one run_bench.py call per work level.
#
#   scripts/e4_sweep.sh results/E4-capacity/<ts>/capacity.csv [work levels...]
#
# The offered rates are fractions of Model A's *measured* closed-loop capacity at the same
# work level (capacity.csv from `scripts/e4_analyse.py capacity`), from 10% to 120%. 120% is
# past B's capacity too (B/A capacity ratio was measured first), so every curve shows its
# knee. Fractions are denser near 1 because that is where the curves bend.
#
# Each configuration (model x arrival x rate) gets N = 10 interleaved runs; --warmup-runs 0
# because every run already warms its books on the 10.4 M messages before the window.
set -euo pipefail

cap_csv=${1:?usage: e4_sweep.sh CAPACITY_CSV [work ...]}
shift
works=("$@")
[[ ${#works[@]} -eq 0 ]] && works=(0 200 500 1000)
fractions="0.1 0.3 0.5 0.7 0.85 0.95 1.05 1.2"
input=${E4_INPUT:-data/slices/prefix_0930_w2m.itch}
runs=${E4_RUNS:-10}

for w in "${works[@]}"; do
    cap=$(awk -F, -v w="$w" 'NR > 1 && $2 == "a" && $1 + 0 == w + 0 { print $4 }' "$cap_csv")
    [[ -n "$cap" ]] || { echo "no Model A capacity for work $w in $cap_csv" >&2; exit 1; }
    # capacity.csv holds M msg/s; e4_threading --rate takes msg/s.
    rates=$(python3 -c "
import sys
c = float(sys.argv[1]) * 1e6
assert 1e4 < c < 1e8, f'implausible capacity {c} msg/s'
print(','.join(f'{round(c * float(f), -3):.0f}' for f in sys.argv[2:]))" "$cap" $fractions)
    echo "work $w ns: A capacity $cap M msg/s -> rates $rates msg/s"
    python3 scripts/run_bench.py --name "E4-sweep-w$w" --build build/release --runs "$runs" --warmup-runs 0 \
        --cmd "{bin}/apps/e4_threading --model {model} --mode latency --arrival {arrival} --rate {rate} --work-ns $w --input $input --start-time 09:30:00 --window 1000000 --out {out}" \
        --param model=a,b --param arrival=smooth,bursty --param "rate=$rates"
done
