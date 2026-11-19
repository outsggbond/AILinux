# Current Risk Report

Audit date: 2026-11-19

## High-priority gaps

1. **No distribution or bootable image definition.** There is no project-owned
   root filesystem, installer, bootloader configuration, or target image.
2. **No recovery foundation.** No verified stable release, separate recovery
   environment, A/B deployment, or rollback mechanism exists. Automated system
   deployment is therefore unsafe and must remain disabled.
3. **No trusted system-operation authorization layer.** A process-local
   `ToolBroker` registers only explicit C++ handlers, validates structured
   arguments, checks local task/Agent state, requires confirmation for
   state-changing handlers, denies privileged handlers, and writes hashed
   argument audit events to local SQLite. Its caller-supplied allowlist and
   confirmation flag are unauthenticated, and the database is not independent
   of the process owner. It provides no IPC, privilege separation, or safe
   privileged operation path. Never pass model-generated shell commands to a
   privileged shell.
4. **No deployed credential service.** Provider adapters read named files from
   systemd's `CREDENTIALS_DIRECTORY`, but the project has no installed systemd
   unit, `LoadCredential=` setup, or managed credential provisioning workflow.

## Additional gaps

- No long-running AI core or IPC protocol. Read-only health sampling now has an
  installable unprivileged systemd service template; installation does not
  enable it. It reports CPU/memory/swap/load, aggregate network interface
  receive/transmit rates, OOM-kill deltas, filesystem capacity, and
  configured process presence only. There is no alert routing or corrective
  action, and it does not monitor systemd, disk I/O, temperatures, or
  system services. Process/thread inspection remains a separate library/tool.
- No Agent isolation or resource budgets. Assignments, reviews, and task
  lifecycle transitions have local SQLite records, but the database is not an
  independently protected audit store and does not authenticate reviewers.
- Provider adapters cover non-streaming OpenAI, DeepSeek, and Kimi chat
  completions, but have no live API validation or automatic retries. Optional
  per-Agent budgets are estimates from configured prices and prompt bytes, not
  hard invoice limits.
- No packaged test suite, CI, security scan, or release verification.
- No AILinux-specific kernel configuration or patches.
- The project has no declared target hardware or supported installation path.
- The existing CLI reports the host kernel, which may be different from the
  kernel source tracked as a submodule.

## Current safeguards

- The existing utility only queries `uname(2)` and does not request privilege.
- The initial self-state component is a pure transition-policy library and
  performs no filesystem, process, bootloader, or service operations.
- The upstream kernel source is not modified or built as part of this phase.
- No API secrets are present in project code.

## Release gate

Do not install a kernel, alter partitions or boot configuration, enable an
automatic privileged service, or deploy experimental changes to a host until a
target image, explicit authorization design, recovery path, and isolated
integration-test environment exist and have been validated.
