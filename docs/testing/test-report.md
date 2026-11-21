# Test Report

Date: 2026-11-19
Scope: host build, C++ self-state foundation, and C++ AI-module prototype

## Environment

- Debian GNU/Linux 13 development host
- Linux host kernel `6.12.113+deb13-amd64`
- CMake 3.31.6
- Ninja 1.12.1
- GCC/G++ 14.2.0
- C++20

## Executed checks

| Check | Result | Evidence |
|---|---|---|
| C++ health-monitor service | PASS | `ailinux-healthd --once` emits a schema-versioned JSON snapshot; periodic mode exits cleanly on SIGTERM, and sampler/output failures are nonzero. |
| Super Self workspace | PASS | CTest creates an owner-private detached Git worktree outside the source tree and performs a workspace-local CMake/Ninja build; removal is tested as a cleanup operation without deployment. |
| Release/recovery state | PASS | CTest denies an unverified candidate, activates a verified candidate, records rollback/current slots, returns to the stable slot, and verifies private state/audit records without touching boot or partitions. |
| Phase 8 integration regression | PASS | CTest integrates release rejection/activation/rollback, malformed-state fail closed behavior, Super Self worktree lifecycle, and health JSON contract checks. Live boot, Provider, privilege and long-duration tests remain outside host unit scope. |
| Phase 8 security regression | PASS | CTest verifies unauthorized tool denial, state-change confirmation, privileged-operation denial, 1,000 authorized read invocations, and complete hashed audit event recording. It does not establish process authentication or privilege separation. |
| Unix-domain IPC peer authentication | PASS | CTest verifies a bounded framed JSON exchange, SO_PEERCRED UID/GID/PID identity, and successful pidfd locking without shell transport. |
| CMake configure (`cmake -S . -B build/cmake-cpp -G Ninja`) | PASS | Configured with local dependency paths for OpenSSL Crypto, SQLite3, libcurl, and nlohmann-json. |
| CMake build (`cmake --build build/cpp-only-check`) | PASS | `ailinuxctl`, `ailinux-healthd`, `ailinux_core`, `ailinux_ai`, and all test executables built. |
| CTest (`ctest --test-dir build/cpp-only-check --output-on-failure`) | PASS | 4/4 C++ test targets passed: state transitions, persistent state journal, AI modules (including Council workflow), and one-shot healthd JSON output. |
| Existing CLI smoke test (`./build/cmake-cpp/ailinuxctl`) | PASS | Prints host OS, kernel release, and architecture. |
| Persistent state journal | PASS | Persists/replays valid lifecycle transitions; rejects stale writers, symlink journals, unsafe directory permissions, and a modified hash-chain record; verifies journal mode is `0600`. |
| Agent reputation/governance | PASS | Tests independent evidence, the 10% boundary and low-risk-only assignment restriction, domain separation, critical suspension/resolution, SQLite v1-to-v9, v4-to-v9, v7-to-v9, and v8-to-v9 migrations, Python fingerprint compatibility, per-Agent concurrency and Provider request quota persistence, Domain Leader election/revocation, Inspector quality evidence, role separation, provider token-usage and cost persistence, atomic budget reservations, restart settlement, and hashed tool-audit persistence. |
| Deterministic health monitor | PASS | Tests CPU and network counter-delta sampling, aggregate interface counters, OOM-kill baselines and critical deltas, memory/swap/disk/load health classification, missing/zombie processes, threshold validation, malformed procfs network/OOM/memory data, and meminfo lines without units. |
| Health alert transitions | PASS | Tests initial warning/critical alert emission, unchanged-alert deduplication, healthy recovery events, JSON alert records, private SQLite persistence/retrieval, record-bounding support, and bounded-state validation. |
| Deterministic Inspector fallback | PASS | Tests classification of warning, critical, and unknown metrics and the explicit `deterministic_inspector_fallback` JSON contract without representing the result as a qualified AI Inspector. |
| System status/process/thread/health tools | PASS | Tests `uname(2)` status, procfs process/thread stat parsing (including names containing parentheses), PID/TID separation, CPU ticks, start time, virtual/resident memory, thread counts, invalid/missing PIDs, and the shared JSON health snapshot through the read-only broker. |
| In-process tool broker | PASS | Tests explicit registration, structured input rejection, task/Agent binding, allowlist denial, confirmation for state changes, privileged-operation denial, handler-failure auditing, and schema migration. This does not test authenticated IPC or privilege separation, which are not implemented. |
| Council workflow | PASS (mock providers) | Tests elected-role validation, independent analyses and cross-reviews dispatched concurrently across OpenAI/DeepSeek/Kimi adapters as persisted AgentScheduler tasks, structured Leader decisions, Provider-backed Inspector verification and archive, Provider authentication failure with all failed calls audited and no false phase advancement or fabricated partial analyses, peer-review phase gates, explicit execution reports, Inspector independence, rework loop, role revocation, and persistence across reopen. Autonomous execution dispatch, context isolation, and authenticated identities remain unimplemented. |
| Provider adapters | PASS (mock only) | Tests default endpoint selection, multiple models per Provider, enforced completion-token caps and per-request ceilings, request/tool-call parsing, secret redaction, auth/rate-limit/retry metadata, redirect refusal, response-size limits, credential symlink rejection, and bounded concurrent calls against a local mock server. No live API credentials/calls. |
| Agent scheduler | PASS (mock only) | Tests bounded task execution, stable priority ordering, queued and in-flight task deadlines, per-Agent configured concurrency and Provider request quotas, READY/WAITING/BUSY/ERROR/OFFLINE runtime snapshots (including latest error category/time) and recovery, outstanding-task backpressure and non-persistence on rejection, provider failure categorization without Agent blame, cancellation while waiting for provider capacity, in-flight HTTP cancellation and transport disconnect, persisted task state, interrupted-task recovery, Provider usage cost settlement, and pre-dispatch budget rejection. Task request/response payloads are not persisted. |
| Source/submodule check | PASS | Kernel submodule remains at the pinned Linux `v6.12.118` commit; no kernel source files were changed. |

## Not tested / not implemented

- Power-loss behavior during an append and full durable-backup procedures.
- Authentication against an attacker with the same account, backups, and
  journal recovery after a torn append.
- Separate Base/Real/Super runtime environments or crash isolation.
- Release signing, stable-version verification, deployment, or rollback.
- Bootloader, partition, kernel installation, or recovery-media behavior.
- Live provider APIs/credentials, autonomous Council execution, authenticated IPC, or
  privilege-separated system-operation broker.
- VM fault-injection, hardware compatibility, long-duration testing, or
  distribution boot testing.

The passing host build and unit test are not evidence that AILinux is a
bootable distribution or that recovery/deployment requirements have been met.
