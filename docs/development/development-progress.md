# Development Progress

Updated: 2026-11-19

## Phase 0 — source and system audit

**Status: Complete for the inspected checkout and host.** Evidence and limits
are recorded in:

- `docs/architecture/current-system-audit.md`
- `docs/architecture/current-source-map.md`
- `docs/architecture/current-build-process.md`
- `docs/architecture/current-risk-report.md`

The audit describes the Debian development host separately from the AILinux
repository and does not claim that the repository builds a distribution.
The planned language boundaries are documented in
`docs/architecture/language-strategy.md`.

## Phase 1 — three-self foundation

**Status: State policy and local persistence slice implemented; phase
incomplete.** The `ailinux_core` library models Base Self, Real Self, Super Self,
and deployment as independent state axes, including the required simultaneous
`REAL_ACTIVE`/`SUPER_ACTIVE` state. Cross-axis guards reject boot, development,
and deployment without their prerequisites. A versioned, SHA-256 chained
journal provides owner-only local persistence, process locking, fsync, replay
validation, and fail-closed corruption handling. CTest covers state transition
graphs, replay, stale writers, unsafe paths, symlink rejection, and corrupted
records.

The hash chain is not a signature and does not protect against its directory
owner. There is no runtime isolation, verified release, rescue image, actual
deployment, or rollback execution.

## Phase 2 — AI system core

**Status: Partial C++ foundation only.** `system/ai/reputation.cpp` provides a
version-scoped Agent identity registry, per-Agent concurrency limits,
evidence-backed review records,
domain-specific rolling performance/error rates, critical-incident suspension,
Domain Leader election, Inspector qualification/rotation, and transactional
role revocation. Its private SQLite database migrates schemas through version
9, including existing Python-generated Agent fingerprints, hashed tool-call
audit events, Provider usage, estimate-based per-Agent USD budgets, per-Agent
concurrency limits, and cumulative Provider request quotas.
`system/ai/providers.cpp`
provides non-streaming OpenAI/DeepSeek/Kimi adapters with systemd credential
file reads, redirect refusal, bounded responses, redacted errors, configured
completion-token caps, per-instance model routing (including multiple models
for one Provider), and bounded parallel requests, tested against local HTTP
mocks. `system/ai/runtime.cpp`
connects the registry to those providers with SQLite-persisted task states,
transition events, restart interruption recovery, active HTTP cancellation,
and global/per-Agent concurrency bounds plus a default 256 outstanding-task
cap, further bounded by each Agent's persisted concurrency setting. Queued
work uses stable priorities and optional deadlines that cover queue
wait and interrupt timed-out HTTP transfers. Cancellation interrupts libcurl
transfers and is honored while waiting for a provider concurrency slot.
Provider requests enforce a per-model
completion-token ceiling and persist returned prompt/completion usage with each
task. Queue saturation rejects submissions before persistence. Budgeted tasks
reserve an estimated maximum before Provider dispatch and settle against
returned usage; Provider prices are administrator-configured and the estimate
is not an invoice-level guarantee. No live credentials/API calls, complete
resident scheduler service, or automatic Council execution/inspection is
implemented. Agents whose
rolling global error rate exceeds 10% are restricted to difficulty-1
assignments; critical security findings suspend assignment eligibility pending
explicit resolution.

## Phase 5 — performance and Inspector foundations

**Status: Partial foundation only.** Error rates, critical incident suspension,
low-risk-only task assignment after the 10% limit, and rolling independent
Inspector assessments (precision/recall/specificity), role separation,
immediate role revocation when qualification is lost, and global Inspector
rotation are implemented in C++. `health_monitor.cpp` adds read-only CPU,
memory, swap, disk, load, aggregate interface receive/transmit rates,
OOM-kill deltas, and configured-process snapshots. `ailinux-healthd`
periodically emits the snapshots as JSON under a hardened, unprivileged
systemd unit. Installation does not enable the unit. It does not monitor
systemd or disk I/O. Network rates are informational and have no alert
thresholds; a newly observed OOM kill is critical. The same serialized
snapshots are available to running Agents through the read-only
`system.get_health` tool. `health_monitor.cpp` now also provides a bounded
in-memory alert manager that emits state-transition warning, critical,
unknown, and recovery records, deduplicating unchanged states. Optional
owner-only SQLite persistence is implemented for explicit `--alert-db` use;
external notification routing remains unimplemented. A deterministic,
read-only Inspector fallback is now emitted with each snapshot and explicitly
labels warning, critical, and unknown metrics; it does not claim to be an
elected or qualified AI Inspector.
Domain Leader election does not implement all three leadership tiers or all
requested competency rubrics.

## Phase 3 — permissioned system-operation foundation

**Status: Partial in-process foundation only.** `system/ai/tool_broker.cpp`
dispatches only handlers explicitly registered by trusted C++ code. It validates
bounded JSON schemas/arguments, checks the local Agent/task state and
caller-supplied tool allowlist, requires an explicit confirmation flag for
state-changing handlers, and always denies privileged handlers. Hashed
arguments and decision/outcome categories are written to the local SQLite
audit history, including a `started` event before a handler runs. The caller-supplied
authorization context is not authenticated: this is not a security boundary,
does not provide IPC or privilege separation, and cannot safely authorize
system operations on its own. No arbitrary-shell tool is provided.
The read-only `process.list`, `process.inspect`, and `thread.list` tools now
expose procfs data with PID/TID separation, CPU ticks, state, start ticks, and
virtual/resident memory. They do not signal or otherwise control processes.
`system.get_status` reports `uname(2)` identity and online CPU count without
claiming the host kernel is built or modified by AILinux.

## Phase 4 — Council workflow

**Status: Provider-backed deliberation and persistent phase-policy foundation.**
`system/ai/council.cpp` stores Council participants, independent opinions,
cross-reviews, Leader decisions, execution reports, Inspector outcomes, and
phase history in a private SQLite database. `council_manager.cpp` dispatches
parallel independent-analysis and peer-review requests through the configured
`AgentScheduler`, so model calls receive durable task states and share scheduler
backpressure and error categories. It validates structured outputs and asks
the elected Leader to synthesize the evidence into a decision. After explicit
execution reports are recorded, it can dispatch the current cycle's decision
and reports to the elected Inspector and persist the validated verdict with an
evidence digest. Failed Provider calls or malformed model output do not advance
the affected phase; persisted records permit deliberation resume. The workflow
enforces elected Leader and Inspector roles, role separation, sequential phase
gates, peer-review participation, and a return to execution when the Inspector
requests changes. It does not yet isolate contexts, assign or execute implementation work, independently
verify reported artifacts, or provide a user-facing Council center. Stored
records are not independently authenticated.

## Phase 6 — Super Self workspace

**Status: Initial disposable workspace foundation implemented.**
`system/core/super_self_workspace.cpp` provides owner-private detached Git
worktrees outside the source checkout, with no shell command interface and
explicit create/build/test/remove operations. Builds and tests run only inside
the disposable workspace and no deployment or release operation is exposed.
The worktree test is part of CTest and verifies private workspace creation plus
a successful workspace build.

## Phase 7 — release and recovery foundation

**Status: Initial local release-state foundation implemented.**
`system/core/release_recovery.cpp` maintains an owner-private, atomic JSON
release state and append-only JSONL audit history. It records stable/candidate
SHA-256 identities, denies activation until verification passes, switches the
recorded current/rollback slots, and rolls back explicitly. It has no bootloader,
partition, kernel-install, reboot, watchdog, or real deployment capability;
those remain out of scope until an isolated VM/recovery test environment exists.

## Phase 8

**Status: Initial host integration regression implemented.** The
`phase8_integration_test` CTest target verifies unverified-candidate rejection,
verified activation, stable rollback, malformed-state fail-closed behavior,
Super Self worktree isolation/cleanup, and health JSON output in one process.
Live multi-Agent, VM boot, privileged authorization, fault-injection, and
long-duration testing remain not implemented. Host CTest now also exercises
unauthorized and privileged tool denial, confirmation requirements, and
1,000 bounded read invocations with complete audit evidence.

## Verification for this slice

Run:

```sh
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

OpenSSL Crypto, SQLite3, libcurl, and nlohmann-json development headers and
libraries are required. There is no AILinux-owned Python runtime or Python test
dependency. The upstream kernel submodule retains its own source/build-tool
requirements.
