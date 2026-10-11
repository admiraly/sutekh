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
| `world.close` | World, expected revision | Frees the world, its schedule, checkpoints and registry records; the slot is reusable and closed IDs are never reused (implemented in M1) |
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

## 8. Worker failure and recovery (M1-02)

The supervisor holds, per world, the creation parameters (fixture path and sha256, capsule and source
sha256), canonical checkpoints, and an ordered log of accepted mutating operations since the newest
checkpoint. Each acknowledged operation is digested (sha256 of the canonical snapshot bytes, which
include the world revision) and the digest is recorded with the operation. Checkpoint bytes and every
capsule or source the supervisor depends on are kept in a supervisor-owned spool, named by sha256.
Recovery never reads project files that may have changed since they were captured.

**When it runs.** A request that finds the active worker dead (EOF, kill, timeout, or an exit noticed
before the request is sent) starts recovery. The replacement worker is started, each live world is
recreated from its creation parameters, its newest checkpoint is restored at the recorded revision, and
the log is replayed. After each replayed operation the digest must equal the one recorded when it was
acknowledged. A mismatch, or a rejected checkpoint or replayed operation, marks that world
`unrecoverable`; it is reported and never continued.

**The failed request is not executed.** It is answered with `SU_E_WORKER_FAILED` (retry flag true) and a
top-level `recovery` member. Read-only requests are not retried automatically. Mutating requests are never
retried by the supervisor, so the client retries them after reading the recovered revision.

`recovery` object (present only on the request that found the death, and on any response that reports a
quarantine):

| Field | Meaning |
|---|---|
| `recovered` | `true` only when every live world was rebuilt and verified in this recovery |
| `replaced_pid_old` | PID of the worker that died (0 if none) |
| `replaced_pid_new` | PID of the replacement worker (0 if none) |
| `worlds_recovered` | Worlds rebuilt and verified |
| `ops_replayed` | Logged operations re-applied to rebuild them |
| `duration_ms` | Wall-clock time of the recovery |
| `from_checkpoint_tick` | Tick of the oldest checkpoint a rebuilt world started from, or `null` |
| `worker_generation` | Worker processes started so far |
| `unrecoverable` | `[{world_id, reason}]` for worlds that could not be recovered |
| `quarantined` | `[{candidate_id, world_id, state, reason, rolled_back_to_tick?, discarded_ticks?}]` |

**Worker event.** The first response after a recovery that does not itself report one carries a top-level
`worker_event` member with the same fields as `recovery`. `engine.info` reports `worker_generation` and
`recoveries` (completed recoveries), and a `recovery` object with `limits`, `spool_bytes`,
`committed_bytes` and per-world `worlds[]` (`tick`, `revision`, `log_entries`, `checkpoints`,
`checkpoint_bytes`, `reserved_bytes`, `unrecoverable`).

**Checkpoints and the log.** A checkpoint is taken after `checkpoint_interval_ticks` accepted ticks
(default 64), after every accepted install (unless the test-only `no_install_checkpoint` is set), at
creation, on `world.snapshot`, after `world.restore` (the restored state starts a fresh log), and before
an operation that would overflow the log. Retained checkpoints per world: `checkpoint_retain` (default 4,
max 8). The log holds at most `log_max_entries` operations per world (default 256, max 1024). A checkpoint
clears the log it supersedes.

**Quarantine.** When the same operation kills the worker on `quarantine_after` consecutive recoveries
(default 2), the offending revision is quarantined. An install is refused from then on
(`SU_E_CANDIDATE_FAILED`; `candidate.status` reports `install.state` `quarantined` and `install.reason`),
and the world stays on its previous revision. Any other operation reverts the world to the newest
checkpoint that predates the installed revision. That state is recorded as a new revision, and operations
accepted after that checkpoint are discarded (`discarded_ticks` reports how many ticks).

**Limits and budgets.** The spool holds capsule, source and checkpoint bytes within `spool_bytes_max`
(default 32 MiB). A world's checkpoint reservation is bounded from its row and field counts (11 bytes per
value, plus fixed overhead) times `checkpoint_retain + 1`, and creation is refused with `SU_E_RESOURCE_LIMIT`
when the reservation or the spool budget does not fit. At most four worlds are held at once (the worker limit).

**Explicit restore and install** still re-check the project files against their recorded hashes (the M0
rule). Recovery uses the spool only. A client `world.restore` of a revision whose file changed on disk is
refused with `SU_E_SCHEMA_MISMATCH` when that revision's project files are still known.

**Limits of this milestone.** Registry transaction IDs (`txid`) and the worker's own counters are
worker-local, so a recovered world reports new transaction IDs. Canonical state, revisions, ticks and
columns are byte-identical to an uninterrupted run. A fixture file changed on disk after creation makes
its world unrecoverable (fail closed). Windows is not implemented.

**Test-only seam.** Workers built with `-DSU_TEST_FAULT_HOOKS` honour `SUTEKH_TEST_KILL_AT_TICK`,
`SUTEKH_TEST_KILL_SOURCE_SHA`, `SUTEKH_TEST_KILL_TOKEN`, `SUTEKH_TEST_KILL_MODE` and
`SUTEKH_TEST_SKEW_EXACT`. The production binary contains none of these strings (checked by
`tests/recovery/test_recovery`). They are not part of the protocol.

**Internal worker methods** (supervisor to worker only, never advertised): `internal.world.seed`,
`internal.world.capture`, `internal.world.digest`, `internal.world.restore_bytes`. `world.create` also
accepts `world_id`, `fixture_sha256`, and per-capsule `capsule_sha256` and `source_sha256`.
