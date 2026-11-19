# Module Dependencies

## Current dependency graph

```text
ailinuxctl ──> host Linux read-only interfaces

self_state_test ─────┐
                     ├──> ailinux_core ──> OpenSSL Crypto (SHA-256)
self_state_store_test┘

ai_system_test ──────> ailinux_ai
                       ├── system/ai/providers.cpp ──> libcurl / nlohmann-json
                       ├── system/ai/reputation.cpp ─> SQLite / OpenSSL Crypto
                       ├── system/ai/runtime.cpp ────> ailinux_ai provider + reputation APIs
                       ├── system/ai/health_monitor.cpp ─> procfs / filesystem usage
                       ├── system/ai/process_monitor.cpp ─> procfs process/thread inspection
                       └── system/ai/tool_broker.cpp ──> reputation API / registered handlers
```

The AI components are C++ libraries, not long-running installed services.
There is not yet a running AI core, scheduled health service, trusted system
bridge, privilege-separated broker, IPC transport, or deployment manager. The
current tool broker is an in-process policy foundation; its caller-supplied
authorization context is not authenticated. A local Council workflow records
the phase sequence and outcomes, but does not dispatch provider calls or tasks.

## Ownership and boundary rules

- `ailinux_core` contains deterministic policy and private local state
  persistence; it must not execute OS mutations or access provider credentials.
- The state journal's hash chain detects accidental edits and incomplete
  writes but does not authenticate data against its owning user.
- `ailinuxctl` remains an unprivileged presentation/diagnostic tool.
- System mutations may only be exposed through explicitly registered C++
  handlers, never arbitrary shell execution. The current broker requires a
  separately authenticated caller and privilege-separated deployment before
  it can authorize real system changes.
- Future persistent state must have one authoritative writer and append-only
  audit evidence; agents must not update their own performance records.
- `kernel/linux` remains upstream source unless a documented capability gap
  requires a kernel change and the change is separately justified and tested.

## Planned dependency order

1. State schema, transition policy, tests.
2. Local persistence and audit format, including corruption and migration tests.
3. Read-only system health interfaces and tool registry metadata.
4. Permission broker and narrowly scoped system operations (in-process
   foundation only; trusted authorization is outstanding).
5. Provider adapters and isolated credential handling.
6. Agent lifecycle, scheduler, task records, and performance evaluation.
7. Council, elections, and independent Inspector (persisted phase foundation;
   Provider/task orchestration remains outstanding).
8. Isolated Super Self builds, release validation, and deployment/recovery.

These are dependencies, not completed features.
