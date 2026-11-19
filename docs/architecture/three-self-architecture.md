# Three-Self Architecture: Initial Boundary

This document records the first executable model, not a claim that the three
environments already exist as separate bootable systems.

## Responsibilities

- **Base Self (本我):** eventual minimal recovery environment and trusted
  recovery tools. It must not depend on AI APIs. No separate Base Self image or
  recovery mechanism exists yet.
- **Real Self (真我):** eventual verified, user-facing operating system. The
  current Debian host is only the development host; it is not an AILinux
  release. No AILinux runtime service is installed.
- **Super Self (超我):** isolated development, build, and test environment. The
  current Git worktree and CMake build are development artifacts, not a
  sandboxed Super Self.

## Initial state model

The `ailinux_core` library stores Base Self, Real Self, Super Self, and
deployment as separate state axes. This allows `REAL_ACTIVE` and
`SUPER_ACTIVE` to coexist, as required by the README.

- Base Self: `BOOTSTRAP`, `BASE_READY`, `RECOVERY`, `FAILED`
- Real Self: `REAL_OFFLINE`, `REAL_BOOTING`, `REAL_ACTIVE`, `DEGRADED`,
  `REAL_RECOVERY`, `REAL_FAILED`
- Super Self: `SUPER_INACTIVE`, `SUPER_ACTIVE`, `SUPER_TESTING`, `SUPER_READY`,
  `SUPER_FAILED`
- Deployment: `DEPLOYMENT_IDLE`, `DEPLOYING`, `ROLLBACK`

Each axis has an explicit transition graph. Cross-axis guards require Base Self
to be ready before Real Self boots, Real Self to be active before Super Self
work begins, and both `REAL_ACTIVE` and `SUPER_READY` before entering
`DEPLOYING`. Invalid transitions preserve the prior snapshot and return a
diagnostic result.

## Deliberate limitations

The lifecycle policy is deterministic, and `PersistentSelfStateManager` can
persist state changes in a versioned, SHA-256 chained journal in a
user-owned, non-group/world-writable directory. The journal and lock file are
created with owner-only permissions; file symlinks, invalid permissions,
broken chains, invalid state transitions, and stale writers are rejected.
Each successful transition is fsynced before the in-memory state advances.

The journal is not a trusted security audit store: an attacker with the same
user identity and write access can rewrite the journal and recompute its hashes.
It is not signed or anchored outside the state directory. A torn final append
fails closed and requires explicit operator recovery; it is not automatically
repaired. The manager does not verify a release, execute a deployment, reboot,
or invoke recovery. It is not thread-safe as an in-process object; separate
processes are serialized through `flock`, and stale managers must be reopened
after a concurrent update.

## Next boundary

Before adding state persistence or a service, define a versioned state record,
single-writer ownership, atomic update semantics, event audit format, and
failure behavior. Before deployment, build a disposable VM/image test target
and an independently verifiable recovery artifact.
