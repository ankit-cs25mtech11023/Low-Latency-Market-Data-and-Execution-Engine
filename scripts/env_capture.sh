#!/usr/bin/env bash
# Captures everything about the machine state that can change a latency number, as JSON.
# Every result directory gets one of these (scripts/run_bench.py calls it automatically).
#
# Usage: scripts/env_capture.sh [build_dir] > env.json
set -euo pipefail

BUILD_DIR="${1:-}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

esc() {  # JSON-escape stdin
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/\\t/g' | tr -d '\n\r'
}
rd() {  # read a file, or "n/a" if missing/unreadable
    if [[ -r "$1" ]]; then esc < "$1"; else printf 'n/a'; fi
}
cmd() {  # run a command, or "n/a" if it fails
    local out
    if out="$("$@" 2>/dev/null)"; then printf '%s' "$out" | esc; else printf 'n/a'; fi
}

governors="$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | awk '{printf "%s%s:%s", (NR>1?",":""), $2, $1}')"
cur_mhz="$(awk -F: '/cpu MHz/{printf "%s%d", (n++?",":""), $2}' /proc/cpuinfo)"
ac_online="n/a"
for s in /sys/class/power_supply/*/; do
    [[ -r "$s/type" && "$(cat "$s/type")" == "Mains" ]] && ac_online="$(cat "$s/online")"
done
thermal="$(for z in /sys/class/thermal/thermal_zone*/; do
    [[ -r "$z/type" && -r "$z/temp" ]] && printf '%s=%s ' "$(cat "$z/type")" "$(cat "$z/temp")"; done)"
git_sha="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo n/a)"
# Any uncommitted or untracked source change means the binary may not match git_sha.
git_dirty="$([[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]] && echo true || echo false)"
cxx_flags="n/a"; cxx_compiler="n/a"; build_type="n/a"
if [[ -n "$BUILD_DIR" && -r "$BUILD_DIR/CMakeCache.txt" ]]; then
    cxx_compiler="$(grep -E '^CMAKE_CXX_COMPILER:' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2-)"
    build_type="$(grep -E '^CMAKE_BUILD_TYPE:' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2-)"
    bt_upper="$(echo "$build_type" | tr '[:lower:]' '[:upper:]')"
    cxx_flags="$(grep -E "^CMAKE_CXX_FLAGS_${bt_upper}:" "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2-)"
fi

cat <<EOF
{
  "captured_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "cpu_model": "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //' | esc)",
  "microcode": "$(grep -m1 microcode /proc/cpuinfo | cut -d: -f2- | tr -d ' ' | esc)",
  "logical_cpus": $(nproc --all),
  "smt_siblings_cpu0": "$(rd /sys/devices/system/cpu/cpu0/topology/thread_siblings_list)",
  "smt_active": "$(rd /sys/devices/system/cpu/smt/active)",
  "kernel": "$(uname -r | esc)",
  "kernel_cmdline": "$(rd /proc/cmdline)",
  "isolated_cpus": "$(rd /sys/devices/system/cpu/isolated)",
  "nohz_full": "$(rd /sys/devices/system/cpu/nohz_full | sed 's/^ *(null)$//')",
  "nmi_watchdog": "$(rd /proc/sys/kernel/nmi_watchdog)",
  "clocksource": "$(rd /sys/devices/system/clocksource/clocksource0/current_clocksource)",
  "scaling_driver": "$(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver)",
  "governors": "$governors",
  "energy_perf_preference": "$(rd /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference)",
  "intel_pstate_no_turbo": "$(rd /sys/devices/system/cpu/intel_pstate/no_turbo)",
  "intel_pstate_min_perf_pct": "$(rd /sys/devices/system/cpu/intel_pstate/min_perf_pct)",
  "intel_pstate_max_perf_pct": "$(rd /sys/devices/system/cpu/intel_pstate/max_perf_pct)",
  "cpu_mhz_now": "$cur_mhz",
  "thp_enabled": "$(rd /sys/kernel/mm/transparent_hugepage/enabled)",
  "perf_event_paranoid": "$(rd /proc/sys/kernel/perf_event_paranoid)",
  "irqbalance_active": "$( (systemctl is-active irqbalance 2>/dev/null || true) | head -1 | esc)",
  "ac_online": "$ac_online",
  "thermal_millideg": "$(printf '%s' "$thermal" | esc)",
  "loadavg": "$(rd /proc/loadavg)",
  "mem_available_kb": "$(awk '/MemAvailable/{print $2}' /proc/meminfo)",
  "gcc": "$( (gcc --version 2>/dev/null || echo n/a) | head -1 | esc)",
  "clang": "$( (clang --version 2>/dev/null || echo n/a) | head -1 | esc)",
  "build_dir": "$(printf '%s' "$BUILD_DIR" | esc)",
  "build_type": "$(printf '%s' "$build_type" | esc)",
  "cxx_compiler": "$(printf '%s' "$cxx_compiler" | esc)",
  "cxx_flags": "$(printf '%s' "$cxx_flags" | esc)",
  "git_sha": "$git_sha",
  "git_dirty": $git_dirty
}
EOF
