#!/usr/bin/env bash
# Downloads NASDAQ ITCH 5.0 sample day files into data/ and verifies them.
# Nasdaq data is never committed (see data/README.md).
#
#   scripts/fetch_itch.sh                 # default day
#   scripts/fetch_itch.sh 07302019        # explicit day (MMDDYYYY)
#   scripts/fetch_itch.sh --verify-only   # re-check an existing download
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
DATA="$REPO/data"
declare -A SIZES=( [07302019]=3662140094 )

verify_only=0
day=07302019
for a in "$@"; do
    case "$a" in
        --verify-only) verify_only=1 ;;
        *) day="$a" ;;
    esac
done
file="$day.NASDAQ_ITCH50.gz"
path="$DATA/$file"
mkdir -p "$DATA"

if [[ $verify_only -eq 0 ]]; then
    echo "downloading $file (resumable; re-run to continue after an interruption)"
    wget -c -O "$path" "$BASE/$file"
fi

[[ -f "$path" ]] || { echo "missing $path" >&2; exit 1; }
size=$(stat -c %s "$path")
if [[ -n "${SIZES[$day]:-}" && "$size" != "${SIZES[$day]}" ]]; then
    echo "size mismatch: got $size, expected ${SIZES[$day]} (incomplete download? re-run to resume)" >&2
    exit 1
fi
echo "size ok ($size bytes); checking gzip integrity (takes a few minutes)..."
if command -v pigz >/dev/null; then pigz -t "$path"; else gzip -t "$path"; fi
echo "gzip ok"

sums="$DATA/checksums.sha256"
if grep -q " $file\$" "$sums" 2>/dev/null; then
    (cd "$DATA" && grep " $file\$" checksums.sha256 | sha256sum -c -)
else
    echo "no pinned sha256 for $file yet; pinning it now"
    (cd "$DATA" && sha256sum "$file" >> checksums.sha256)
    echo "added to data/checksums.sha256; commit that file"
fi
