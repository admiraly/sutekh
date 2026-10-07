# Agent control protocol
## Local JSON-lines v0.1

These are interfaces to implement, not APIs already present. M0 exposes a local stdio session owned by one CLI/orchestrator. It must not expose a public network listener. Later multi-client transport may use a permission-controlled local socket or named pipe with authentication and per-client capabilities.

## 1. Envelope

One strict UTF-8 JSON object per line, maximum 1 MiB per message initially. Larger state/artifacts are referenced by validated immutable IDs, never pasted as uncontrolled executable content. Reject duplicate keys, malformed UTF-8, unexpected keys, oversized collections, and path escapes before allocating large buffers.

Request:

```json
{"protocol":"0.1","id":"request-17","method":"capsule.submit","params":{"capsule_path":"capsules/movement","expected_world_revision":"world-3"}}
```

Response:

```json
{"protocol":"0.1","id":"request-17","ok":true,"result":{"candidate_id":"candidate-12","status":"received"}}
```

IDs in these snippets are illustrative, not current engine state. Response `ok:false` carries an error object; stdout remains parseable and diagnostic prose goes to stderr or structured events. An accepted queue request does not mean a candidate passed.

## 2. Core M0 methods

| Method | Inputs | Result |
|---|---|---|
| `engine.info` | None | Versions, implemented capabilities, runtime mode, process identity |
| `world.create` | Fixture/artifact ID, tick settings | New world ID and revision |
| `world.inspect` | World, entity/field selection, result limit | Bounded canonical state and tick |
| `world.step` | World, exact tick count | Completed tick range or explicit failure |
| `world.run` / `world.pause` | World, pacing policy | Running/paused status |
| `world.snapshot` | World, expected revision | Immutable snapshot ID and metadata |
| `world.restore` | World, snapshot, expected revision | Restored revision or mismatch error |
| `capsule.submit` | Immutable capsule source or allowed path, expected base | Candidate ID, captured source hashes |
| `candidate.status` | Candidate ID | State, diagnostics, evidence IDs |
| `candidate.evaluate` | Candidate, fixture/test IDs | Asynchronous test job ID |
| `candidate.install` | Candidate, expected revision, allowed effective tick | Pending/installed version event |
| `events.read` | Cursor, maximum count | Bounded ordered events plus next cursor |
| `tests.run` | Suite, source/candidate ID | Test job ID |
| `job.status` | Job ID | State, exact input fingerprints, artifacts |

`world.step` is valid for a paused world; reject it during free running unless the interface explicitly pauses at a boundary. `candidate.install` requires sufficient evidence for the chosen policy. A client cannot set its own “passed” flag. The supervisor resolves installed revisions against its evidence registry.

## 3. Later methods

`schema.plan_migration`, `schema.commit_migration`, `replay.compare`, `benchmark.run`, `dependency.query`, `artifact.inspect`, `native.specialize`, and `task.lease` are added only in their milestone. Unsupported methods return `SU_E_UNSUPPORTED_METHOD`; never return placeholder success.

A human-facing CLI can wrap these operations. A future MCP-style adapter is an optional wrapper, not the core protocol and not a bootstrap requirement.

## 4. Concurrency and idempotence

Every mutation includes an expected world/source revision or is explicitly creation-only. Stale expectations return `SU_E_STALE_REVISION`. Retrying the same client request ID with the same canonical payload returns the previous outcome while retained; retrying it with different payload is an error.

Jobs are immutable inputs with a mutable status record. Cancellation is best-effort before installation; report whether work was actually cancelled. A cancelled result cannot later be installed. Events use monotonic sequence numbers independent of simulation tick.

## 5. Errors

Initial error set: `SU_E_PARSE`, `SU_E_TYPE`, `SU_E_DUPLICATE_KEY`, `SU_E_UNKNOWN_OPCODE`, `SU_E_UNDEFINED_REGISTER`, `SU_E_DUPLICATE_REGISTER`, `SU_E_UNDECLARED_READ`, `SU_E_UNDECLARED_WRITE`, `SU_E_SCHEMA_MISMATCH`, `SU_E_STALE_REVISION`, `SU_E_RESOURCE_LIMIT`, `SU_E_UNSUPPORTED_METHOD`, `SU_E_UNSUPPORTED_MIGRATION`, `SU_E_CANDIDATE_FAILED`, `SU_E_WORKER_FAILED`, `SU_E_CAPABILITY_DENIED`, and `SU_E_IO`.

Errors identify request/candidate/source hash, relevant JSON pointer, expected/actual values, and whether retry is meaningful. Do not leak secrets or arbitrary filesystem contents through diagnostics.

## 6. Trust and filesystem policy

M0 methods can read only the selected project/artifact roots. Normalize and validate paths, reject traversal/symlink escapes according to a documented policy, and capture immutable content before evaluation. Importing an artifact does not execute its metadata as shell commands.

Network and native compilation requests are not exposed to ordinary gameplay capsules. Build tools are invoked by the broker using validated arguments, not interpolated untrusted shell strings. Avoid default system-wide credentials in worker environments.

## 7. Evidence and metrics

Report host monotonic durations and simulation ticks separately. `engine.info` exposes capability presence, not imaginary counters. `job.status` says `not_run` or `unavailable` when appropriate. Results include exact workload and artifact identities so a worker can safely act on them without scraping text logs.
