# Persistent and Provider-Backed Council Workflow

`system/ai/council.cpp` provides a local SQLite-backed phase coordinator for
Council records, and `system/ai/council_manager.cpp` dispatches the deliberation
phases to configured Providers. A session can only be opened with the currently elected
Domain Leader and Inspector for its domain, and at least one additional ready
Agent. Leader and Inspector must be distinct. Role eligibility is checked
again before the Leader decides or the Inspector files a report; participant
readiness is checked before analysis, review, and execution records are
accepted. Each session stores a unique task ID and bounded task summary.

The enforced sequence is:

1. Intake.
2. Independent analysis by the Leader and Members (the Inspector is excluded).
3. Cross-review, with at least one peer review from every non-Inspector
   participant before the next phase.
4. Leader decision, including alternatives, risk, execution, test, and rollback
   criteria.
5. Explicit execution reports by the Leader and Members.
6. Independent Inspector review. A change request returns the session to
   execution; verification or rejection archives the session.

Execution reports and inspections are retained per cycle. When the Inspector
requests changes, the cycle advances and a fresh Leader execution report is
required before the next inspection; prior reports are never overwritten.

Participant opinions, cross-reviews, decisions, execution outcomes, inspection
reports, and phase transitions persist in a separate owner-only SQLite
database. The schema enables foreign-key checks and records all phase changes.
The database is local evidence only: its owning account can modify it, and it
does not authenticate Agent identities.

The Provider-backed manager makes independent analysis calls in parallel for
the Leader and Members, without sharing other participants' opinions. Each
participant then reviews one peer's analysis in parallel; the Leader receives
the full analysis and review record and must return a structured decision.
Responses are parsed and validated before persistence. Invalid output or a
Provider error is surfaced and does not advance the phase. Existing persisted
analysis and review records are used to resume a deliberation after interruption.
Every Provider call is submitted through `AgentScheduler` under a unique
Council task ID, so state transitions and error categories appear in the shared
task history and the scheduler's concurrency/backpressure limits apply. Failed
calls are drained from the current parallel batch before the phase is returned
as failed; no partial set of model outputs is written as completed analysis.

After explicit execution reports are recorded, the manager can dispatch the
current execution cycle's decision and reports to the elected Inspector. Its
structured verdict and report are validated, and a SHA-256 digest of the
reviewed evidence and verdict is persisted. Provider or parse failures leave
the session in inspection so the assessment can be retried.

This is not yet a full autonomous Council. The manager does not isolate
participant processes, automatically invoke tools, assign or execute
implementation work, inspect code changes, produce signed evidence, or route
Inspector findings to a trusted user interface. Execution reports remain
explicit inputs; provider-backed inspection does not independently verify the
truth of those reports. Council decisions never bypass the independent tool
authorization path or grant permission to execute system operations.

CTest exercises the full stored phase flow, rejects missing analyses/reviews,
prevents role conflicts and self-review, checks Leader/Inspector revocation,
verifies that a completed session survives database reopen, and runs an
end-to-end mock-provider deliberation across OpenAI, DeepSeek, and Kimi adapters.
