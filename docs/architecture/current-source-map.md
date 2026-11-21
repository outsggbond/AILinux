# Current Source Map

## Repository-owned files

| Path | Responsibility | Current status |
|---|---|---|
| `CMakeLists.txt` | C++20 project configuration and build targets | Builds the host utility, core state library, AI library, health service, and CTest targets; installs the service and its systemd unit. |
| `system/tools/ailinuxctl/main.cpp` | User-facing system control CLI | Prints the host OS, kernel release, and architecture using `uname(2)`. |
| `system/core/self_state.hpp/.cpp` | Three-self state model | Enforces the explicit state transition graph without system side effects. |
| `system/core/self_state_store.hpp/.cpp` | Persistent state journal | Uses OpenSSL SHA-256, POSIX file permissions, `flock`, and `fsync`; it does not perform OS deployment. |
| `system/core/super_self_workspace.hpp/.cpp` | Disposable Super Self workspace | Creates an owner-private detached Git worktree outside the source checkout and provides restricted workspace-relative build/test/remove operations. It has no deployment or Real/Base mutation capability. |
| `system/core/ailinux_ipc.hpp/.cpp` | Authenticated Unix-domain IPC foundation | 4 MB-bounded framed JSON with magic/version/CRC32, SO_PEERCRED UID/GID/PID validation, and pidfd locking. No privileged gateway or independent audit daemon yet. |
| `system/core/release_recovery.hpp/.cpp` | Local release/recovery state foundation | Maintains private stable/current/candidate/rollback JSON state and JSONL audit history. It verifies explicit candidate approval and explicit rollback but does not modify boot, disks, kernels, or services. |
| `system/ai/providers.hpp/.cpp` | OpenAI, DeepSeek, and Kimi Chat Completions adapters | C++/libcurl implementation with named systemd credential reads, TLS verification, per-instance model routing and completion-token ceilings, bounded prompt/response sizes, configured price estimates, bounded concurrency, cancellation-aware transfers, redacted errors, and local mock tests. Tool calls are returned as data only. |
| `system/ai/reputation.hpp/.cpp` | Agent reputation and governance | C++/SQLite implementation with version-scoped evidence reviews, per-Agent runtime snapshots, concurrency and Provider request quotas, error eligibility, critical-error suspension, Domain Leader elections, Inspector quality/rotation, role revocation, schema migration, persistent task/token/cost records, and transactionally bounded per-Agent USD budgets with crash recovery settlement. |
| `system/ai/runtime.hpp/.cpp` | Provider-backed Agent scheduler | C++ bounded worker scheduler with stable task priorities, total-task deadlines, global/per-Agent configured concurrency limits, capped outstanding work, persistent task state/events, active Provider request cancellation, pre-dispatch budget reservations and usage settlement, process lease, and restart interruption recovery. |
| `system/ai/health_monitor.hpp/.cpp` | Deterministic local health sampler and alert fallback | Read-only procfs CPU/memory/swap/load/network interface rates/OOM-kill deltas, filesystem capacity, and configured process sampling; bounded state-transition alerts and owner-only SQLite alert history; and an explicit deterministic Inspector fallback classification. Shared JSON output is used by `ailinux-healthd` and the read-only `system.get_health` broker tool. Network rates are informational, not alert thresholds. |
| `system/ai/healthd.cpp` | Periodic health-monitor executable | Emits versioned JSON-line snapshots, state-transition alerts, optional SQLite-persisted alert history, and deterministic Inspector fallback records; supports one-shot checks, validates sampling intervals, and exits on SIGTERM/SIGINT; installed as an unprivileged, sandboxed systemd service template. |
| `system/ai/process_monitor.hpp/.cpp` | Read-only process/thread inspection | Enumerates visible processes and per-process threads from procfs, distinguishing PID and TID; exposes only `process.list`, `process.inspect`, and `thread.list` through the in-process tool broker. |
| `system/ai/system_tools.hpp/.cpp` | Read-only host status tools | Registers `system.get_status`, reporting kernel identity and online CPU count through the broker; it does not assert that the host kernel is an AILinux-built kernel. |
| `system/ai/tool_broker.hpp/.cpp` | Process-local tool dispatch policy | Validates structured JSON arguments, Agent/task state, and caller-supplied tool allowlist; state-changing calls require confirmation and privileged calls are denied. Not an authenticated authorization boundary. |
| `system/ai/council.hpp/.cpp` | Persistent Council workflow coordinator | Stores role-checked session participants, independent opinions, cross-reviews, Leader decisions, execution reports, Inspector outcomes, and phase events. The manager dispatches deliberation only; execution remains explicit and does not authorize system operations. |
| `system/ai/council_manager.hpp/.cpp` | Scheduler-backed Council deliberation and inspection | Concurrently submits independent opinions and peer reviews as persistent AgentScheduler tasks, obtains the elected Leader's decision, and dispatches explicit execution reports to the elected Inspector with an evidence digest. Does not execute work or invoke tools. |
| `tests/self_state_test.cpp` | State model regression test | Tests state transitions and guards. |
| `tests/self_state_store_test.cpp` | State persistence regression test | Tests replay, permissions, symlink rejection, corruption detection, and stale writers. |
| `tests/phase8_integration_test.cpp` | Host integration regression | Exercises release rejection/activation/rollback, malformed release-state failure, Super Self worktree lifecycle, and health JSON output. It does not provide VM boot, live Provider, privileged authorization or long-duration validation. |
| `tests/ai_system_test.cpp` | C++ AI-module integration tests | Tests provider transport/errors/concurrency, reputation/election/Inspector rules, scheduler backpressure/state/recovery, Council phase/rework persistence, scheduler-backed multi-provider deliberation/inspection and failure audit, schema migration/fingerprint compatibility, procfs health data, process/thread inspection, and tool-broker policy/audit. |
| `.gitmodules` | Kernel submodule origin and path | Registers `kernel/linux` as the upstream Linux stable submodule. |
| `README.md` | Project goals and execution requirements | Defines the multi-phase target; it is not evidence that the specified features already exist. |
| `docs/architecture/language-strategy.md` | Language and trust boundaries | Records the C++20 requirement for AILinux-owned user-space runtime code and the upstream kernel C boundary. |
| `.gitignore` | Generated build output exclusions | Excludes `/build/` and common CMake/compiler output. |

## External source

`kernel/linux` is a Git submodule pinned by the superproject gitlink to
`b869a5313cbfe7102873d3de318ddb68675146d0` (Linux `v6.12.118`). It is upstream
kernel source, not a directory of AILinux modifications.

## Not yet implemented

There is no distribution image, package definition, authenticated or
privilege-separated system-operation broker, complete Council workflow,
continuous health-monitor service, isolated Super Self build environment,
recovery environment, or deployment/rollback pipeline. A successful host
build is not a bootable AILinux system.
