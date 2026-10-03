#!/usr/bin/env bash
# Puts the laptop into the benchmark baseline state, and restores it afterwards.
#
#   sudo scripts/tune_machine.sh apply     # before a benchmark session
#   sudo scripts/tune_machine.sh restore   # afterwards
#   scripts/tune_machine.sh status         # no root needed
#
# Why each knob (repeatability matters more than peak speed):
#   * governor=performance + EPP=performance: the core does not ramp its frequency up and
#     down with load, so the cycles->time relation of the code stays stable during a run.
#   * no_turbo=1: turbo frequency depends on temperature, power budget and how many cores
#     are busy, which makes numbers drift between runs. Disabling it trades peak speed for
#     stable, comparable results. (The TSC itself is unaffected: it ticks at a constant rate.)
#   * nmi_watchdog=0: the kernel's hard-lockup detector permanently occupies one of the 4
#     general-purpose performance counters per core. Top-down metric groups (perf stat -M
#     TopdownL1, toplev) need all of them; with the watchdog on, events are multiplexed or
#     reported as "<not counted>". It also removes a periodic NMI interrupt from the cores.
# The saved state is written to /run/lle-tune.state so restore puts back exactly what was there.
set -euo pipefail

STATE=/run/lle-tune.state
CPUFREQ=/sys/devices/system/cpu/cpu*/cpufreq
PSTATE=/sys/devices/system/cpu/intel_pstate

status() {
    echo "governor:   $(cat $CPUFREQ/scaling_governor | sort | uniq -c | tr '\n' ' ')"
    echo "epp:        $(cat $CPUFREQ/energy_performance_preference 2>/dev/null | sort | uniq -c | tr '\n' ' ')"
    echo "no_turbo:   $(cat $PSTATE/no_turbo 2>/dev/null || echo n/a)"
    echo "AC online:  $(cat /sys/class/power_supply/A*/online 2>/dev/null || echo n/a)"
    echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
    echo "nmi_watchdog: $(cat /proc/sys/kernel/nmi_watchdog)"
}

need_root() {
    if [[ $EUID -ne 0 ]]; then echo "run with sudo" >&2; exit 1; fi
}

apply() {
    need_root
    if [[ ! -f $STATE ]]; then
        {
            echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
            echo "epp=$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null || echo)"
            echo "no_turbo=$(cat $PSTATE/no_turbo 2>/dev/null || echo)"
            echo "nmi_watchdog=$(cat /proc/sys/kernel/nmi_watchdog)"
        } > $STATE
    fi
    for g in $CPUFREQ/scaling_governor; do echo performance > "$g"; done
    for e in $CPUFREQ/energy_performance_preference; do [[ -w $e ]] && echo performance > "$e" || true; done
    [[ -w $PSTATE/no_turbo ]] && echo 1 > $PSTATE/no_turbo
    echo 0 > /proc/sys/kernel/nmi_watchdog
    if [[ "$(cat /sys/class/power_supply/A*/online 2>/dev/null || echo 1)" != "1" ]]; then
        echo "WARNING: not on AC power; results will not be comparable" >&2
    fi
    status
}

restore() {
    need_root
    if [[ ! -f $STATE ]]; then echo "nothing to restore ($STATE missing)"; status; return; fi
    # shellcheck disable=SC1090
    source $STATE
    for g in $CPUFREQ/scaling_governor; do echo "$governor" > "$g"; done
    if [[ -n "${epp:-}" ]]; then
        for e in $CPUFREQ/energy_performance_preference; do [[ -w $e ]] && echo "$epp" > "$e" || true; done
    fi
    [[ -n "${no_turbo:-}" && -w $PSTATE/no_turbo ]] && echo "$no_turbo" > $PSTATE/no_turbo
    [[ -n "${nmi_watchdog:-}" ]] && echo "$nmi_watchdog" > /proc/sys/kernel/nmi_watchdog
    rm -f $STATE
    status
}

case "${1:-status}" in
    apply) apply ;;
    restore) restore ;;
    status) status ;;
    *) echo "usage: $0 apply|restore|status" >&2; exit 2 ;;
esac
