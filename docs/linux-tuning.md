# Linux Tuning

What is changed on the machine, why, and how to undo it. Every result file records the state
of these knobs in its `env.json` (`scripts/env_capture.sh`), so a number can always be traced
back to the configuration that produced it.

Machine: Intel Core i5-8250U (Kaby Lake R, 4 cores / 8 threads, SMT siblings n and n+4),
7.6 GiB RAM, Kali Linux, kernel 6.16, bare metal, laptop on AC power.

## 1. Development settings (persistent)

### `kernel.perf_event_paranoid = 1`

`perf_event_paranoid` controls what an unprivileged user may measure with `perf`:

| value | allows |
|---|---|
| 2 (Debian default) | only user-space counting/sampling of your own processes |
| 1 | also kernel-space events for your own processes |
| 0 | also CPU-wide events (all processes), raw tracepoints |
| -1 | everything |

We need `1`: experiments count events (cycles, cache misses, page faults) for our own process
*including the time it spends in the kernel* (system calls in E0's `clock_gettime`, socket
receives in E5). We do not need system-wide access, so we do not go lower.

Set once, persistent across reboots:

```bash
echo 'kernel.perf_event_paranoid=1' | sudo tee /etc/sysctl.d/99-perf.conf
sudo sysctl --system
cat /proc/sys/kernel/perf_event_paranoid   # -> 1
```

Undo: `sudo rm /etc/sysctl.d/99-perf.conf && sudo sysctl kernel.perf_event_paranoid=2`.

## 2. Benchmark baseline (per session, `scripts/tune_machine.sh`)

```bash
sudo scripts/tune_machine.sh apply     # before a benchmark session
sudo scripts/tune_machine.sh restore   # afterwards (puts back exactly what was there)
scripts/tune_machine.sh status         # no root needed
```

| knob | baseline value | why |
|---|---|---|
| cpufreq governor | `performance` | The default `powersave` governor (intel_pstate) ramps frequency with load; a benchmark would then measure partly at a low frequency and partly at a high one. |
| energy_performance_preference | `performance` | Hint to the hardware P-state controller (HWP) to stay at high frequency instead of saving power. |
| `intel_pstate/no_turbo` | `1` | Turbo frequency depends on temperature, power budget and how many cores are busy. It changes during and between runs, so results drift. With turbo off, the core runs at its maximum non-turbo frequency, which is stable. Repeatability matters more than peak speed. |
| `kernel.nmi_watchdog` | `0` | The hard-lockup detector permanently uses one of the 4 general-purpose performance counters on every logical CPU and fires a periodic NMI. Top-down analysis (`perf stat -M TopdownL1`, `toplev`) needs all counters; with the watchdog on, those events are reported as `<not counted>`. |

The TSC is unaffected by all of these: it ticks at a constant rate (`constant_tsc`) even when
the core frequency changes, and keeps ticking in deep C-states (`nonstop_tsc`). That is why
it can be used as a clock. It also means TSC ticks are *not* core cycles; see
`docs/methodology.md`.

Manual checklist for a benchmark session (not scriptable):
- Laptop on AC power (`tune_machine.sh` warns otherwise; `env.json` records `ac_online`).
- Close the browser and other heavy applications (free RAM, fewer background wake-ups).
- Let the machine cool down for a minute after a large build.

## 3. Later phases (filled in when they are reached)

- **P2:** `ulimit -l` (memlock) for `mlockall`, 2 MiB huge pages (`vm.nr_hugepages`, THP modes).
- **P5:** network namespaces, veth, `tc netem` (all need `sudo`).
- **P7 / E6:** kernel command line (`isolcpus`, `nohz_full`, `rcu_nocbs`, `irqaffinity`),
  IRQ pinning with `irqbalance` off, C-state limits, `SCHED_FIFO`, measured one knob at a time.
