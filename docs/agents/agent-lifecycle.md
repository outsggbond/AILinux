# Agent Scheduler — Initial Runtime Slice

`system/ai/runtime.cpp` connects persisted Agent identity and eligibility to the
provider pool. `AgentScheduler::submit()` validates the chat request, records a
task assignment and queued state transactionally, and schedules a provider
call. It enforces global, configurable per-Agent limits (default 2; each Agent
can persist a limit from 1 to 256), and configurable outstanding-task limits
(256 by default). The scheduler's per-Agent constructor value remains a hard
upper bound. At capacity, submission throws `SchedulerCapacityError`
before recording a task, so rejected work does not silently accumulate in
SQLite. Each Agent may also have a cumulative Provider request limit; zero
means unlimited. A request attempt is consumed immediately before Provider
dispatch, including attempts that fail or are cancelled while waiting for a
Provider slot. Tasks accept priorities from -1000 to 1000; larger values run first,
with submission order preserved among equal priorities. An optional per-task
timeout (1 ms to 24 hours) covers queue wait and Provider execution, and
interrupts an in-flight HTTP request when it expires. It persists queued,
running, completed, failed, cancelled, and interrupted states in the local
SQLite database. `runtime_snapshot()` aggregates these persisted records into
READY, WAITING, BUSY, OFFLINE, SUSPENDED, or DISABLED with queued/running task
counts. The most recent failed/interrupted terminal task reports ERROR with its
category and occurrence timestamp; queued/running work takes precedence, and a
later successful or cancelled task clears that runtime error. Provider failures affect this
operational snapshot only and do not by themselves count as Agent reputation
errors. State changes are also recorded as task events. Tool calls
are returned as response data; the scheduler does not execute them or grant
operating-system permissions.

An Agent must be registered and `ready` when submitted and is checked again
before execution. The reputation store enforces the over-10% low-risk task
restriction when recording assignments. Provider failures produce a
failure category and do not automatically count as Agent errors; a separate
evidence-backed review is required.

On scheduler startup, persisted queued/running tasks are marked interrupted
with a `process_restart` reason; provider requests are not automatically
replayed. An active cost reservation is conservatively charged at its reserved
estimate during recovery. Task lifecycle state and transition history persist,
but request payloads and response bodies are deliberately not stored. This
remains a library, not a complete lifecycle service: there is no persisted
retry policy, resource isolation, full Agent status machine, or user-facing
control interface. Cancelling a task interrupts an in-flight
libcurl transfer and a wait for provider capacity; an in-flight cancellation
is conservatively charged at its reserved estimate. No systemd unit or IPC
service has been installed.

An optional per-Agent USD budget reserves an estimated maximum cost before
Provider dispatch, rejects work that cannot reserve that amount, and settles
successful calls using returned token usage. Provider model input/output
prices must be configured for budgeted Agents. If usage is absent or a call
fails after dispatch, the reservation is charged conservatively. Price and
prompt-byte estimates are approximate and are not an invoice-level spending
guarantee; see the Provider adapter limits.
