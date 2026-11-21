# Read-Only Health Monitor

`system/ai/health_monitor.cpp` is a local, deterministic sampler independent of
provider APIs. It reads Linux procfs CPU counters, memory and swap statistics,
load average, aggregate network interface receive/transmit counters, filesystem
usage, and caller-supplied critical process IDs. Network rates aggregate all
visible interfaces (including loopback) and are informational; they do not
have alert thresholds. OOM kills are reported as a counter delta and any newly
observed kill is critical. CPU/network/OOM first samples establish a baseline
and are explicitly marked unknown; CPU and network samples less than 100 ms
apart also remain unknown. Subsequent samples calculate utilization and
network rates from counter deltas. Baselines are in memory and restart with
the sampler. Malformed or decreasing counters fail the sample instead of
being reported as healthy.

`ailinux-healthd` runs this sampler as an unprivileged, periodic service. It
emits one JSON snapshot per line to standard output (journal under systemd),
supports a bounded sampling interval and one-shot mode, and exits cleanly on
SIGTERM/SIGINT. It also emits state-transition alert records, including
warning/critical/unknown transitions and recovery events, as separate JSON
lines. Repeated unchanged metric states are suppressed in memory. The alert
manager is bounded and has no external notification backend; journal output is
the current routing boundary. Sampling/output failures are written to standard
error and cause a nonzero exit so systemd can apply its bounded restart policy. The
provided systemd unit uses a dynamic unprivileged identity and filesystem,
device, capability, and network restrictions. Build and install it with CMake;
installation does not enable or start the service.

For a local, non-installing check:

```sh
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
./build/cmake/ailinux-healthd --once
```

The default CMake install prefix is `/usr/local`; set
`-DCMAKE_INSTALL_PREFIX=...` when configuring if a different prefix is needed,
so the generated unit's executable path matches the install destination.
Review the generated unit before installing it on a host. Enabling or starting
the service is an explicit administrator action and is not part of the build
or install steps.

The monitor does not take corrective action or execute privileged operations.
It provides state-transition alert records but not paging, email, D-Bus, or
other external notification routing. An explicit `--alert-db PATH` option
can additionally persist state-transition records in an owner-only SQLite
database whose directory must be private and record count is bounded to
10,000 entries. Process checks require trusted PID input. A missing process
is reported as critical; malformed or inaccessible required metrics raise
`HealthMonitorError` instead of being silently treated as healthy.

This is not yet a full Inspector or system health service. Disk I/O,
temperatures, kernel/systemd logs and services, thread health, Agent health,
alert routing, persistent health history, and authenticated administrative
interfaces remain unimplemented.
