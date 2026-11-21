# Phase 8 Integration Regression

## Current scope

`phase8_integration_test` is a host-process C++ regression that exercises the
integration boundaries already implemented:

- unverified release candidates remain inactive;
- verified candidates activate and can be rolled back;
- malformed release-state data fails closed;
- Super Self creates and removes a detached, owner-private worktree;
- the deterministic health monitor emits the documented JSON schema;
- unauthorized tool use, missing confirmation, and privileged-tool access are denied and hashed into the audit store;
- 1,000 authorized read invocations produce bounded, complete started/executed audit events;
- Unix-domain IPC validates bounded framed JSON, SO_PEERCRED UID/GID/PID identity and pidfd lock acquisition.

This test does not start the AI core, boot a distribution, modify partitions or
bootloader configuration, install a kernel, enable a systemd unit, or invoke a
live Provider.

## Run

```sh
cmake -S . -B build/cmake-cpp -G Ninja
cmake --build build/cmake-cpp
ctest --test-dir build/cmake-cpp --output-on-failure
```

## Remaining validation

- Multi-Agent live concurrency and Provider fault injection.
- Real Leader re-election and Inspector rotation under process loss.
- Privileged or authenticated IPC authorization testing.
- VM boot, disk fault injection, A/B start validation, watchdog and rollback.
- Long-duration memory, queue, log-growth, API-disconnect and disk-growth tests.

These items require an isolated VM or recoverable test environment and must not
be performed against the project's only bootable host system.
