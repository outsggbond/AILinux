# Agent Performance Evaluation — Initial Implementation

## Persisted identity and version isolation

The C++ `AgentReputationStore` records provider, model, model version, prompt
version, and tool-configuration version. Reviews are tied to the SHA-256
fingerprint of all five fields. Any configuration change therefore starts a
new evidence window; old results remain stored but do not qualify the changed
Agent configuration.

The implementation recognizes the twelve professional domains listed in the
project requirements. Scores for one domain never contribute to another
domain's sample or score.

## Evidence and attribution

Every review requires a prior assignment, a reviewer ID different from the
Agent, and a lowercase SHA-256 digest identifying the evidence. Supported
verdicts are `correct`, `agent-error`, `external-failure`, and `undetermined`.
Only `correct` and `agent-error` count toward the effective sample/error rate;
external and undetermined outcomes do not blame the Agent or inflate its
sample. Critical security findings must be attributed to an Agent error,
suspend the Agent immediately, and block return to `ready` until the incident
is explicitly resolved.

The store enforces these constraints locally, but it does not authenticate
reviewer identities or prove that a supplied evidence digest refers to a
trusted artifact. A privileged service and independently protected evidence
source are required before these records can be treated as trustworthy
governance input.

## Current calculations and gates

- Effective history is the most recent 100 independently recorded,
  current-configuration reviews with `correct` or `agent-error` verdicts.
- Error rate is `agent-error / effective reviewed tasks`.
- Task difficulty is 1–5 and weights the reported accuracy score.
- Governance qualification requires at least 20 effective global reviews,
  `ready` status, no unresolved critical security finding, and a global error
  rate no greater than 10%.
- Domain Leader qualification additionally requires at least 20 effective
  reviews in that domain and a domain error rate no greater than 10%.
- If the global error rate exceeds 10%, assignment is restricted to difficulty
  1 tasks; suspended Agents cannot receive assignments.
- More than 10% is disqualifying; exactly 10% does not bypass the sample,
  status, incident, or domain-specific gates.

## Domain Leader election

`elect_domain_leader(domain)` considers only Agents with the Leader permission
and passing global/domain gates. It ranks by difficulty-weighted domain
accuracy. Within five percentage points of the top score, the Agent with fewer
recorded task opportunities in that domain is preferred, followed by fewer
prior appointments in that domain, score, and Agent ID. Up to two challengers
are recorded, preferring other providers when available. Each election and its
rationale are persisted.

This is an initial deterministic election policy, not the complete governance
system. It does not yet calculate the full per-domain competency rubrics,
Chief/Integration Leader roles, model-family independence, signed reviewer
identities, or user-facing administrative APIs. A caller with write access to
this local database remains trusted; this module is not a security boundary by
itself.

## Inspector performance and rotation

Independent Inspector assessments are stored separately from task reviews as
true-positive, false-positive, true-negative, or false-negative outcomes. Each
requires an assigned task, an Inspector distinct from the inspected Agent, an
independent evaluator distinct from both, and an evidence digest. The rolling
Inspector score exposes precision, recall, specificity, and the false
positive/negative rate. Qualification requires the Agent's global task
performance gates, at least 20 current-version inspections, evidence of both a
correct finding and a correctly rejected non-finding, and an inspection error
rate no greater than 10%.

Rotation excludes the current global Inspector and every active Domain Leader,
prefers a different Provider than that domain's Leader, and otherwise picks
the least-served qualified Inspector. Only one Inspector may be active at a
time across all domains. If there is no qualified alternate,
the rotation result is empty and explicitly requires deterministic monitoring
to continue; the deterministic monitor itself is not implemented here.

## Storage and migration

The database uses SQLite schema version 9, transactions, foreign keys, and
owner-only file permissions. Version 1 and 2 databases are upgraded
transactionally.
The database directory must already exist, be owned by the current user, and
not be group/world writable.

Tool audit entries retain a request/task/Agent/tool identifier, a SHA-256
digest of canonicalized arguments, a bounded decision category, and a
timestamp. Raw tool arguments and handler output are not stored. Tool-audit
history is local SQLite data and is not tamper-proof against the database
owner; it must not be presented as an independently trusted audit service.
