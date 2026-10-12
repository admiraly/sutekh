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
(default 512 MiB in the `sutekh` binary, so a 1,000,000-row world fits; the library default of
`SuAgentConfig.spool_bytes_max` is 32 MiB; `SUTEKH_SPOOL_BYTES_MAX` overrides the binary's value).
A world's checkpoint reservation is bounded from its row and field counts (11 bytes per
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

## 9. Native specialization (`world.specialize`, M1-02b; SUTEKH_NATIVE builds only)

Present only in builds configured with `-DSUTEKH_NATIVE=ON`. In the default build the method is not
advertised in `engine.info` and answers `SU_E_UNSUPPORTED_METHOD`.

`world.specialize {world_id, expected_world_revision, system_id, backend, expected_semantic_hash?}`
where `backend` is `scalar`, `avx2` or `auto`. `expected_semantic_hash` is optional and must equal the
installed system's semantic hash when given.

Result: `{world_id, specialization_id, state: "installed", system_id, backend, requested_backend,
execution_tier, artifact_hash, semantic_hash, fallback, source: {kind, admission: {cases, rows_checked,
mismatches}, recompile_hash_equal}, world_revision, tick, worker_pid}`.

- Pipeline (M1-02c-1; the supervisor runs it, not the worker): (1) the active worker answers the
  installed system's identity (`internal.specialize.identity`: semantic hash, source and capsule hashes,
  layout generation, revision), read-only. (2) A DISPOSABLE worker is spawned for this request only
  (RLIMIT_AS and RLIMIT_CPU as configured, wall timeout, killed on timeout, reaped). It compiles the system
  from its project files (`internal.specialize.admit`) and runs differential admission against the
  reference VM over: the seeded corpus (row counts 0,1,7,8,9,17,1024; aligned and one-element-unaligned
  columns; edge and seeded random words; 42 cases) and the system's own test cases (`<stem>.test.json`
  beside `<stem>.capsule.json`, run by the kernel through the world fixture the file names; 3 cases for
  `movement`). A kernel that passes the corpus can still fail the own cases. (3) Only on an admitted
  result, the active worker recompiles (compilation only; it never executes this kernel), requires the
  admitted `artifact_hash`, rechecks the revision, the semantic hash and the layout generation, marks the
  kernel admitted by hash, and installs it into that schedule entry between requests (a tick boundary).
- Failures. A disposable worker that is killed, crashes, exceeds the time limit or answers malformed
  output is `SU_E_WORKER_FAILED` with `error.detail = {phase: "admission", end: "exited" | "signaled" |
  "supervisor_killed", exit_code | signal | timeout_ms}`. A mismatch is `SU_E_CANDIDATE_FAILED` with
  `error.detail.first_mismatch`: `{source: "corpus", case_index, case_rows, case_offset, field_id, row,
  expected, actual}` or `{source: "own_case", case, reason}`. A compile or load failure is
  `SU_E_CANDIDATE_FAILED`. A recompile whose `artifact_hash` differs from the admitted one is refused by
  the active worker with `SU_E_CANDIDATE_FAILED` and nothing is installed. In every failure case the active
  worker PID, the world revision and the canonical state are unchanged, and the supervisor still answers.
- Test-only fault seams exist only in the test worker (`tests/native/qualification/native_test_worker.c`,
  built with SU_TEST_FAULT_HOOKS). The production `sutekh` binary contains none of them
  (`tests/native/qualification/check_release_seams.py`, ctest `release_seams`).
- `world_revision` is not changed: a kernel is not canonical state. Worker PID is unchanged.
- `avx2` on a host without AVX2 is `SU_E_CAPABILITY_DENIED` with the scalar fallback named in the message.
  `auto` selects AVX2 when the host reports it, falls back to scalar on an AVX2 frame-bound failure
  (`fallback` names it), and is scalar otherwise.
- A stale `expected_world_revision` or `expected_semantic_hash` is `SU_E_STALE_REVISION`; an unknown
  `system_id` is `SU_E_INVALID_ARGUMENT`.
- `candidate.install` of a new semantic revision of the system replaces the entry: the kernel is released
  with the old module and the system runs on the VM tier from that boundary.
- `world.inspect` `semantic_manifest` entries carry `execution_tier` (`vm`, `native_scalar`,
  `native_avx2`). `engine.info` reports `native.specialization` with the backends, tiers, host caps and
  the pinned AsmJit commit.
- Kernels are not canonical state. Snapshot and restore return the system to the VM tier. Worker
  recovery (`SU_E_WORKER_FAILED`) rebuilds the worlds on the VM tier; section 10 says which kernels are
  quarantined and which are respecialized.
- Not yet implemented: the t03 numeric-edge programs in admission (the corpus and the own test cases are
  used).

## 10. Native fault recovery, quarantine and respecialization (M1-02c-2; SUTEKH_NATIVE builds only)

Native kernels are not canonical state. Nothing about them is logged or replayed, and the replacement
worker always starts on the VM tier. Recovery correctness does not depend on respecialization: after any
recovery the canonical digest, revision and tick equal an uninterrupted VM-only run, whether or not a
respecialization succeeds.

- Records. The supervisor keeps, per world, the native kernel installed for each system (artifact_hash,
  requested backend, tier). A successful `world.specialize` records it; `candidate.install`, `world.restore`
  and `world.close` clear the world's records (the worker drops the kernel at the same point).
- Attribution. A worker death while a `world.step` runs on a world with installed kernels is attributed to
  those kernels: every kernel installed on that world is quarantined. The crashed kernel is not identified
  per tick, so this is conservative. A death at any other time (killed from outside, idle, another request)
  quarantines nothing; its kernels are respecialized. Attributed deaths do not count toward the M1-02a
  candidate quarantine (repeated-crash rule).
- Quarantine. Keyed by `artifact_hash`. Each entry: world_id, system_id, backend, tier, reason,
  `crash_count`, `replaced_pid`, `tick`. A table of 16 entries (`AGENT_MAX_QUARANTINE`) is never evicted.
  If a crash cannot be recorded (table full), no entry is added and every native specialization is refused
  with reason `quarantine_table_full`: no crash loop is possible.
- Recovery object (the failing request, `SU_E_WORKER_FAILED`, top-level `recovery`). When a kernel was
  attributed it adds `native: {artifact_hash, world_id, system_id, backend, tier, quarantined, crash_count,
  replaced_pid}` for the first attributed kernel. `quarantined: false` means the table was full.
- Refusal. `world.specialize` of a quarantined artifact runs admission (its hash is known only after
  admission, in the disposable worker) and then answers `SU_E_CANDIDATE_FAILED` with
  `error.detail = {phase: "quarantine", reason: "artifact_quarantined", artifact_hash, crash_count}`. A
  different artifact (for example scalar after an avx2 crash) is admitted normally. The VM is the tier
  until an admitted, non-quarantined kernel is installed.
- Respecialization queue. Non-attributed kernels are queued (FIFO, `AGENT_MAX_RESPEC` = 16) and re-requested
  through the normal pipeline (identity, disposable admission, install) against the current revision.
  Queueing happens inside recovery; the admission never does. The serve loop services one queue entry after
  the reply to a request that is not `world.step` and whose reply carries no `recovery` member. So a
  `world.step` is never preceded by respecialization caused by recovery, and the answer that reports a
  recovery shows the request pending. A `SU_E_WORKER_FAILED` outcome is retried once
  (`AGENT_RESPEC_RETRIES` = 1); a refusal is final.
- Outcome. Reported per world on `world.inspect`, as a top-level member `specialization`:
  `{pending_respecialization, respecialization: "none" | "pending" | "respecialized" | "failed", reason,
  quarantined: [...], quarantine_table_full}`. `respecialization` is the last outcome for the world.
  `quarantined` lists the whole table.
- Replay is not the mechanism. Respecialization uses the normal admission, so a respecialized kernel passed the
  same corpus and own cases as a first-time one.
- Test seams: `SU_TEST_FAULT_NATIVE_TICK=N` (and `SU_TEST_FAULT_NATIVE_BACKEND=avx2|scalar`, default any) make
  the test worker die with SIGSEGV inside the installed kernel while computing tick N. They exist only in the
  test worker (`tests/native/qualification/native_test_worker`); `check_release_seams.py` checks the production
  binary for them.

## 11. In-flight ordering, admission corpus and no-spawn evidence (M1-02c-3; SUTEKH_NATIVE builds only)

- Admission corpus. A specialization whose system has its own test cases runs them in the disposable worker:
  `<stem>.test.json` beside `<stem>.capsule.json`, or `test.json` beside `capsule.json` (the t03 numeric-edge
  fixtures). `evidence.cases` counts corpus plus own cases (45 for `examples/movement`, 43 for a t03 fixture).
  An own-case mismatch reports `first_mismatch` with `source: "own_case"`, `case`, `reason` and, for a column
  mismatch, `field`, `row`, `expected`, `actual`.
- Ordering. The supervisor does not interleave requests of clients. The install of a specialization follows its
  admission in the same call; a candidate.install or world.close that lands between them changes the world
  revision or closes the world, so the install is refused (stale revision or unknown world_id) and nothing is
  installed for the old revision. Test-only seam, `SU_TEST_INTERLEAVE=candidate|close` (one shot per supervisor
  process), exists only in `su_agent_test` (built with `SU_TEST_FAULT_HOOKS`); `check_release_seams.py` checks
  the production binary for it.
- No-spawn evidence (TX4). `tests/e2e/native_spawn_probe.py --binary <sutekh>` runs a session (world.create,
  world.specialize scalar and avx2, capsule.submit, candidate.evaluate, candidate.install, world.specialize
  again, world.step, world.close) under `strace -f -e trace=execve,execveat`. Exit 0 only when the two positive
  controls are detected and no compiler, assembler, linker or build tool is executed. Exit 2 inconclusive, 3
  not a native build (TX4 reports unavailable).
- Churn. `test_specialize_churn` (ctest `specialize_churn`): 1000 specialize cycles alternating scalar and avx2,
  then six revision swaps; checks the worker's mappings (no writable and executable mapping, count flat), RSS
  bound, the VM tier after each swap and world.close releasing the world's mapping. Sanitizer runs need
  `ASAN_OPTIONS=quarantine_size_mb=1` for the RSS bound to be meaningful.

## 12. Tick workers, schedule checks and containment defaults (M1 closure B)

**Tick workers (opt-in).** `SUTEKH_TICK_WORKERS=N` (1..64, default 1) and `SUTEKH_TICK_CHUNK_MIN_ROWS=R`
(default 4096, tests may lower it) are read by the supervisor at start-up (invalid values stop the binary
with exit 2). The supervisor sends both to the active worker as the optional internal fields `tick_workers`
and `chunk_min_rows` of every `world.step` (including the replay of recovery). They are not client
parameters. `engine.info` reports them as `tick_workers: {workers, chunk_min_rows}`.

- N = 1 is the serial path, unchanged. N > 1 runs a system in a world of at least R rows through
  `su_jobs_run_system_chunked` (chunks of rows over N pool workers; chunk boundaries depend only on the row
  count, never on N). Results are canonical: identical committed columns for every N (`test_tick_workers`,
  harness TX5 against the reference evaluator). A failing chunk aborts the whole tick; the reported system is
  the first failing one and the committed state is unchanged.
- Native kernels (SUTEKH_NATIVE builds) are chunked the same way from M1 closure C (section 13).
- Candidate evaluation (`candidate.evaluate`, disposable workers) always runs serially.

**Schedule checks at `world.create`.** With more than one system the worker runs the effect analysis
(`su_effects_build`) and the capsule-order check. Every system is treated as declared parallel, so:
- an unordered pair with a write/write or read/write conflict is refused with `SU_E_SCHEMA_MISMATCH`
  (message names the pair and the field);
- an ordering cycle (`after`/`before`) is refused with `SU_E_INVALID_ARGUMENT` (`ordering cycle: a -> b -> a`);
- an `after` edge whose target is listed later in `capsules` is refused with `SU_E_INVALID_ARGUMENT`
  (the world runs systems in capsule order);
- an edge to a system outside the world is refused with `SU_E_INVALID_ARGUMENT`.
Installs (`candidate.install`) are not re-checked against the schedule in M1.

**Containment defaults of the `sutekh` binary (C07).** Workers and disposable children run with
RLIMIT_AS 4 GiB (`SUTEKH_WORKER_AS_BYTES`, 0 = unlimited for sanitizer builds) and receive only the
scrubbed environment (`SU_TEST_*`, `SUTEKH_*`, sanitizer options). Harness TX6 checks the limit in
`/proc/<pid>/limits`, the absence of `SECRET_X` in `/proc/<pid>/environ` of the active worker and of a
disposable worker observed during `candidate.evaluate`, and that the spool default admits a
1,000,000-row world with the default limits.

## 13. Containment of workers, chunked native kernels and safe retirement (M1 closure C)

**Containment (C07).** Every child of the supervisor gets RLIMIT_AS (4 GiB), RLIMIT_FSIZE (256 MiB; `SIGXFSZ` is
ignored, so an over-limit write fails with `EFBIG` and the child lives), RLIMIT_CORE 0, and network denial. The
active worker has RLIMIT_NOFILE 1024 and no CPU limit (long simulations). A disposable worker (candidate.evaluate,
world.specialize admission) has RLIMIT_NOFILE 64 and the CPU bound of its request. Config fields
`worker_file_bytes`, `worker_nofile` and `disposable_nofile` take 0 for these defaults.

Network denial is a raw seccomp-bpf filter on x86-64 Linux (no libseccomp): `PR_SET_NO_NEW_PRIVS`, then
`PR_SET_SECCOMP` with `socket`, `connect`, `accept`, `bind`, `listen`, `accept4`, `socketpair` and the three io_uring
calls returning `EPERM` (section 14 lists the full set). The child checks the filter by calling `socket(AF_INET)` before exec. Off x86-64, or when the filter cannot be
installed, the child tries `unshare(CLONE_NEWNET)`. Nothing is claimed that the child did not verify.

The child writes what it enforced (getrlimit read-back and the network mode) to a close-on-exec status pipe before
exec. The supervisor reads that record and reports it. `candidate.evaluate` and the `world.specialize` answer carry:

```json
"containment": {"reported": true, "rlimit_as": 4294967296, "rlimit_cpu": 12, "rlimit_fsize": 268435456,
                "rlimit_nofile": 64, "no_network": "seccomp", "reason": "seccomp filter installed; ..."}
```

`no_network` is `seccomp`, `netns` or `none`. A limit the child did not report is `null`. `unlimited` means RLIM_INFINITY.
Tests: `containment` (tests/recovery/test_containment.c) runs a probe in a child (test workers only, method
`internal.test.containment`) and asserts socket EPERM, EFBIG after exactly 1 MiB, EMFILE at 64 descriptors,
`Seccomp: 2`, and the `/proc/self/limits` values. A control child with nothing enabled shows the unrestricted behaviour.
Harness TX6 asserts the same facts from outside (`/proc/<pid>/limits` exactly 4 GiB, 256 MiB, 1024 or 64; `Seccomp: 2`
for the active worker and for an observed disposable child; the `candidate.evaluate` containment object).

**Chunked native kernels.** `su_nv_run_system_chunked` (src/runtime/native_versions.c) runs a native kernel over the
canonical chunks of su_jobs (boundaries depend only on the row count) on sliced column views, with N pool workers.
Chunks read working columns and write disjoint staging ranges, so they run concurrently; the commit happens once, after
every chunk returned `SU_OK`. The status is the lowest failing chunk's. Tests: `native_chunked`
(tests/native/qualification/test_native_chunked.c): N in {1, 2, 4} x {VM, native scalar, native AVX2 when the host has
AVX2}, 50 ticks on 100,001 rows and 3 ticks on 1,000,000 rows, each equal to the serial VM reference; the chunk seam
is called once per chunk and tick; one failing chunk leaves the committed columns unchanged.

**Decision: tick workers stay opt-in.** `SUTEKH_TICK_WORKERS` defaults to 1. Deterministic replay equality across worker
counts is proven by the tests above (VM and native, N = 1, 2, 4), but whether parallelism is the default is a product
decision for M2. It is not made here.

**Safe retirement.** The in-flight count of a native kernel is zero at every system boundary (`safe_retire`). Releasing
an artifact from inside its own kernel call defers the unmapping until the call returns; the call's result is
committed and equals the VM (`safe_retire`, seam `SU_NV_FAULT_RELEASE_IN_CALL`). The quarantine table compares whole
artifact hashes (`safe_retire`, `su_agent_test_quar_hit`).

**Digest comparison (M1 closure D).** `digest_matches` (src/agent/su_agent.c) calls `su_agent_digest_fields_match`,
which compares the revision, the tick and, when one is given, the snapshot SHA-256. Each field is tested directly by
`hardening` (tests/recovery/test_hardening.c). The closure C argument that the tick comparison is implied by the SHA
comparison still holds, and the tick is now tested as well.

## 14. C07 containment coverage (M1 closure D)

What each engine child enforces, where, and which test or harness check shows it. The filters are attack-surface
reduction for the worker's own processes. They are not a sandbox against hostile native code (C07).

**Network filter (every child, installed before exec; `src/agent/su_proc.c`).** Returns `EPERM`:
- socket creation and use: `socket`, `connect`, `accept`, `bind`, `listen`, `socketpair`, `accept4`;
- io_uring, which can open sockets without `socket(2)` (IORING_OP_SOCKET): `io_uring_setup`, `io_uring_enter`,
  `io_uring_register`;
- process inspection and injection, keys, BPF and perf, userfaultfd, roots, mounts, namespaces and handles, kernel
  images and modules, reboot, swap, accounting: `ptrace`, `process_vm_readv`, `process_vm_writev`, `kcmp`, `keyctl`,
  `add_key`, `request_key`, `bpf`, `perf_event_open`, `userfaultfd`, `pivot_root`, `chroot`, `mount`, `umount2`,
  `setns`, `unshare`, `open_by_handle_at`, `name_to_handle_at`, `kexec_load`, `init_module`, `finit_module`,
  `delete_module`, `reboot`, `swapon`, `swapoff`, `acct`.
Kills the process (`SIGSYS`): any architecture other than x86-64, and any x32 syscall (number >= 0x40000000).
Not denied, on purpose: `mmap` and `mprotect` (the AsmJit path maps code with PROT_EXEC after dropping PROT_WRITE)
and `personality` (glibc and the sanitizers need it).

**Worker self-sandbox (both worker roles, installed by the worker after startup; `src/agent/su_sandbox.c`).**
`execve` and `execveat` return `EPERM`; `fork` and `vfork` return `EPERM`; `clone` returns `EPERM` unless `CLONE_THREAD`
is set (the tick-worker pool creates threads); `clone3` returns `ENOSYS`, so glibc falls back to `clone`. The
architecture and x32 kills apply here too. The state is reported in `internal.stats` (`self_sandbox`) and as
`Seccomp_filters: 2` in `/proc/<pid>/status` (TX6). `SUTEKH_WORKER_SANDBOX=0` turns it off, and is honoured only in
AddressSanitizer or ThreadSanitizer builds (their runtimes fork and ptrace); the sanitizer lane in this document uses it.
On non-x86-64 hosts the self-sandbox is not installed and reports `unavailable` with the reason.

**Filesystem writes (disposable children, applied before exec; `src/agent/su_proc.c`).** A Landlock ruleset handles
every write-class access (write, truncate, create of files, directories, links and devices, remove, rename-out) and
grants none, so no path is writable. Reads are not handled and stay allowed. The child reports the ABI and the outcome
of `landlock_restrict_self`; the answer's containment object says `"fs_write_denied": "landlock"` when it applied and
`"none"` with `fs_reason` otherwise (for example on a kernel without Landlock). The active worker keeps write access
(`"none"`, reason stated), because the supervisor relies on it for persistent state.

**Limits (every child).** RLIMIT_AS 4 GiB, RLIMIT_FSIZE 256 MiB (over-limit writes fail with `EFBIG`), RLIMIT_CORE 0,
RLIMIT_NOFILE 1024 (active) or 64 (disposable), and a CPU bound for disposable children only. Environment: an
allow-list (`SU_TEST_`, `SUTEKH_`, sanitizer options).

**Evidence.** `containment` (network filter, Landlock, limits, control children), `hardening` (digest fields, spool
directory mode), `recovery`, `tick_workers`, `scheduler`, native suites under `SUTEKH_NATIVE=ON`, and harness TX6
(`/proc` limits, `Seccomp: 2`, `Seccomp_filters: 2`, the `candidate.evaluate` containment object, and the scrubbed
environment). The release binary is checked for test-only seam strings (`check_release_seams.py`). Mutants that the
tests kill: `tests/recovery/run_mutants.sh` (M-ARCH, M-X32, M-SOCKETPAIR, M-URING, M-EXECVE, M-CLONE, M-DIGEST-SHA,
M-DIGEST-REV, M-SPOOL-MODE).

**Contained.** Network creation and use by socket, socketpair and io_uring, including IORING_OP_SOCKET; the i386 and
x32 entries of the same calls; process creation and program execution by a worker; ptrace and the other listed calls;
file creation by a disposable child (project root and `/tmp` refused with `EACCES`); file size, open files, address space,
core files, and for disposable children CPU time.

**Not contained.**
- Filesystem reads: any file the uid can read, including the supervisor's own files, is readable by a child.
- Use of descriptors inherited across exec: the filters deny creating sockets, not using a socket already open.
  The supervisor keeps its descriptors close-on-exec, and a child keeps only its stdin and stdout pipes.
- CPU of the active worker: it has no CPU bound, by design, for long simulations.
- Writes by the active worker: it keeps write access, so it is not read-only.
- Side channels: timing, cache, and memory-layout observation are out of scope.
- Code the process maps executable (mmap PROT_EXEC is allowed), and kernel bugs reachable from the allowed calls.
- Non-x86-64 Linux: no seccomp filters and no self-sandbox (the network namespace fallback applies to networking only).
- Windows: not implemented.

## 15. Entity control plane (M2-00c3b; contract `planning/contracts/M2_ENTITIES_CONTRACT.md` r2, r2.1)

Every world created by `world.create` owns an entity table and a bounded event log. The methods below are
advertised in `engine.info` (`capabilities`) and are the only entity methods implemented. All of them are
recorded in the supervisor's replay log where they mutate state (section 8).

- **Handles.** An entity handle is the JSON integer `(generation << 32) | slot`, a u64. The engine reads
  integers exactly up to u64 (`su_json_get_uint`), so a handle round-trips without loss. Clients whose JSON
  numbers are IEEE doubles lose precision above 2^53 (generation 2^21 and up) and must parse integers as
  big integers. Generation 0 is always stale, and so is any handle whose slot is out of range, dead, retired
  or whose generation differs from the stored one. Stale handles are `SU_E_STALE_HANDLE` (21).
- **`world.create`** accepts, in addition to the M0 keys, optional `max_entities` (0..1,000,000, default 0:
  capacity = fixture rows), `max_struct_per_tick` (1..1,048,576, default 4096), `max_events_per_tick`
  (1..1,048,576, default 4096) and `event_log_max` (1..1,048,576, default 65536). Non-integers are
  `SU_E_TYPE` (the supervisor checks the type, the worker the range: `SU_E_RESOURCE_LIMIT`). The capacity is
  `max(rows, max_entities)`.
- **`world.spawn`** `{world_id, expected_world_revision, values: {<field>: u32}, external_id?: u32}` ->
  `{world_id, world_revision, entity, slot, generation}`. The slot is the lowest free slot (not alive, not
  retired); unset fields are 0. Unknown field: `SU_E_SCHEMA_MISMATCH`. No free slot: `SU_E_RESOURCE_LIMIT`.
  Emits an `SU_EVENT_SPAWNED` record (`a` = slot, `b` = generation). The values object of a spawn is kept
  in the replay log (1 KiB limit: larger objects are `SU_E_RESOURCE_LIMIT`).
- **`world.despawn`** `{world_id, expected_world_revision, entity}` -> `{world_id, world_revision, entity}`.
  Emits `SU_EVENT_DESPAWNED` (`a` = slot, `b` = old generation). The stored generation advances; at
  generation 0xFFFFFFFE the slot is retired and never reissued.
- **`events.read`** `{world_id, cursor, max: 1..256}` -> `{world_id, events: [{event_seq, tick, kind,
  system_index, slot, seq, a, b}], next_cursor, skipped, dropped}`. `cursor` is an `event_seq`. A cursor below
  the oldest retained record starts at the oldest one and reports `skipped`. A cursor above the next sequence
  is `SU_E_INVALID_ARGUMENT`. `max` outside 1..256 is `SU_E_TYPE` at the supervisor. Control-plane records
  have `system_index` 4294967295 (`SU_SYSTEM_INDEX_HOST`), `seq` 0. IR records use the VM's per-row sequence,
  shared by `despawn_self` and `emit_event`; a row's records follow instruction order. Refused IR despawns are
  `SU_EVENT_REJECTED` (`a` = struct kind 1, `b` = status).
- **`world.inspect`** always adds `entities: {capacity, alive, retired, high_water}`. With `entity` (a handle)
  it adds `entity: {handle, slot, generation, external_id, values{field: u32}}` for a live entity; a stale
  handle is `SU_E_STALE_HANDLE`. Columns and `entity_ids` still cover the fixture's initial rows (`row_count`);
  a spawned row is read with `entity`.
- **`world.snapshot` / `world.restore`** use snapshot format 0.2: the entity table and the event log (next
  sequence, dropped count, retained records) are part of the canonical bytes and the restore rebuilds both.
  The per-tick caps stay those of the world being restored.
- **`world.step`** on an entity world runs IR 0.2 systems with despawn and event records. A tick whose
  records exceed a cap (per-tick structs, per-tick events, per-row or buffer) fails with
  `SU_E_RESOURCE_LIMIT` and changes nothing (columns, entity table, log, revision).
- **Worker counts.** `SUTEKH_TICK_WORKERS=N` (section 12) does not change any output: the canonical state
  (`world.snapshot` `sha256`) is equal for every N.
- **Fixtures.** `tests/fixtures/entities/`: `actors_3.json` (world), `despawn_move` (IR 0.2 movement with
  despawn_self and emit_event), `guard` (second system ordered after movement; its despawn of an already
  despawned row is refused), `emit_then_despawn` (the emit precedes the despawn in one row).

## 16. Default caps and 1,000,000-row worlds (M2-00 audit, decision record; defaults unchanged)

The defaults of `world.create` (`max_struct_per_tick` 4096, `max_events_per_tick` 4096, section 15) are kept.
What they mean for a 1,000,000-row entity world with the fixture system `despawn_move` (one `despawn_self`
and one `emit_event` per row, declared per-row caps 4 and 4):

- **Creation succeeds.** Admission sums the buffers one tick would allocate: 4 x 1,000,000 = 4,000,000
  entries per class, within `SU_CMDQ_MAX_ENTRIES` (4,194,304). Two such systems (8,000,000) are refused at
  `world.create` with `SU_E_RESOURCE_LIMIT`; with per-row caps 4 that is every world above 524,288 rows.
- **A despawn-heavy tick fails.** In the first tick of the benchmark fixture (generator seed 8) about 272,700
  rows despawn, so the tick
  needs about 272,000 structure records and about 272,000 events. Against the default cap of 4096 the tick
  fails with `SU_E_RESOURCE_LIMIT` ("tick discarded, committed state unchanged"). The world then reports the
  full 1,000,000 alive rows: the failed tick changes nothing (columns, entity table, event log, revision).
- **How to run it.** Raise the caps when the world is created, for example
  `world.create {"fixture_path": ..., "capsule_paths": [...], "max_struct_per_tick": 1048576,
  "max_events_per_tick": 1048576}`. The maximum is 1,048,576, which covers one `despawn_self` and one
  `emit_event` for every one of the 1,000,000 rows. A system declaring more than one of either per row can
  produce up to rows x declared-cap records in one tick, so the caps must cover the records the tick
  actually produces, and the worst case must still fit 1,048,576 (otherwise the tick is refused).
  `event_log_max` (default 65536) only bounds the retained events: older records are dropped and counted,
  which is not a failure. The benchmark `bench/ir02_entities_bench.py` creates the world this way.
- **Buffer memory.** Buffers are preallocated at creation, per (system, storage chunk of 1024 rows):
  entries = min(chunk rows x declared per-row cap, per-tick cap), for each class. Structure entries are
  32 bytes and event entries 40 bytes. The total is
  `bytes = sum over systems and chunks of (min(rows_c x struct_cap_row, max_struct_per_tick) x 32
  + min(rows_c x event_cap_row, max_events_per_tick) x 40)`. For 1,000,000 rows, one system with per-row
  caps 4 and 4 and caps 1,048,576: 4,000,000 x (32 + 40) = 288,000,000 bytes (about 288 MB, measured as
  VmHWM 294 MB of the worker). The default caps do not reduce this memory for the fixture: a full chunk
  needs min(1024 x 4, 4096) = 4096 entries either way. The defaults only bound the number of records a tick
  may produce, which is what makes the despawn-heavy first tick fail.


## 17. Schema migrations (M2-01b; contract `planning/contracts/M2_MIGRATION_CONTRACT.md` r2, sections 3-7)

Methods (all in the capability list; the migration document is read from the project tree, its hash is
checked, and it is copied into the supervisor spool):

- **`world.migrate_plan`** `{world_id, expected_world_revision, migration_path, transform_path?, lir_path?,
  drop_systems?: [system_id]}` -> `{world_id, world_revision, plan_id, plan}`. `plan` carries `valid`, `reason`
  (`field_in_use`, `spool_budget`, `memory` or null), `message`, `migration_hash`, `plan_hash`, the schema
  before and after (`fields`, `digest`), `columns_added|removed|renamed`, `transform`, `systems[{system_id,
  impact, reasons}]` and `resource` with `estimate: true`. Impacts: `compatible_after_resubmit` (the fields
  the system uses are unchanged), `needs_resubmission` (a renamed or transformed field), `broken` (a removed
  field it uses). A plan changes no state. The plan is bound to the world revision it was made at.
- **`world.migrate_commit`** `{world_id, plan_id, expected_world_revision, resubmit: [{system_id,
  candidate_id}], drop_systems?}`. Every installed system must be replaced by one evaluated candidate
  (`passed`) for the plan's target generation, or be dropped; otherwise `SU_E_UNSUPPORTED_MIGRATION`. Stages:
  (1) a durable pre-migration checkpoint is taken and pinned (the rollback target); (2) the worker prepares
  the successor world and replacement schedule aside (`internal.world.migrate_prepare`); (3) the worker
  publishes them atomically (`internal.world.migrate_swap`, checked against the plan hash; the entity runtime
  moves and `SU_EVENT_MIGRATED` (0xFFFFFFFC; a = from generation, b = to) is appended); (4) the migration is
  logged (`LOG_MIGRATE`, which stores the document hashes, the replacement capsule hashes, the drops and the
  budget); (5) the post-commit checkpoint is taken and the reply is sent only after it is durable. Result:
  `{world_revision, tick, layout_generation, migration_hash, plan_id, rollback_available: true, timings:
  {plan_ns, prepare_ns, transform_ns, swap_ns, checkpoint_ns, total_ns}, bytes: {new_columns, peak}, systems}`.
  `plan_ns` is the worker time of the plan the commit uses; `bytes` are the plan's estimates. Timings are migration
  timings, not reload latency.
- **`world.migrate_rollback`** `{world_id, expected_world_revision}` -> `{world_revision, tick, restored_from,
  changes_lost: true, discarded_ticks, lineage: "retained"}`. Restores the pinned pre-migration checkpoint as a
  NEW revision (`LOG_ROLLBACK`). Changes made after the pin are lost, and the reply says how many ticks were
  discarded. `lineage` is "retained" only when the worker confirms that the IDs of the undone schema are retired
  (otherwise the rollback is refused and the world is marked unrecoverable). Without a pin: `SU_E_STALE_REVISION`
  with `rollback_unavailable`.

Failures: a stale plan or revision is `SU_E_STALE_REVISION`; an invalid plan or unresolved systems is
`SU_E_UNSUPPORTED_MIGRATION`; an unknown plan is `SU_E_INVALID_ARGUMENT`; a missing migration document is
`SU_E_IO`. An unvalidated candidate is `SU_E_UNSUPPORTED_MIGRATION`. If the post-commit checkpoint has no
room, the commit is undone: the logged migration is removed from the replay log, the world is rebuilt by a
replacement worker from the pinned pre-migration checkpoint, and the answer is `SU_E_RESOURCE_LIMIT` with
`error.detail = {reason: "post_commit_checkpoint", rolled_back_to: "pinned_pre_migration_checkpoint"}`. The
world keeps its pre-commit revision and digest; the pin is then no longer a rollback target. If the worker dies
before the commit is acknowledged, the answer is `SU_E_WORKER_FAILED` with a recovery object and the world is
the pre-migration state (retry the commit). If the worker dies while the post-commit checkpoint is being taken,
the recovery replays the logged migration, the checkpoint is taken again, and the commit is answered as a success
with the recovery object. If the checkpoint dies a second time, the commit is undone the same way as the no-room
case, and the answer is `SU_E_WORKER_FAILED` with `error.detail.reason = "post_commit_checkpoint_failed"` (a
retry is allowed; the migration is not in the replay log).

Recovery: a world whose newest checkpoint was taken after a migration or rollback (`migrated` flag) is rebuilt
with `internal.world.create_from_snapshot` (schema from the snapshot header, schedule from the checkpointed
capsules, row identities from the snapshot's `entity_ids`), not from the fixture; the log after it is replayed
(`LOG_MIGRATE` and `LOG_ROLLBACK` re-run the worker's operations from the spool and must reproduce the logged
digests).

Inspect (`world.inspect`) adds `layout_generation`, `migration_lineage` (hex hashes, oldest first), `schema`
(`[{id, name, type, default, used, access}]` for every field of the world, used or not) and
`needs_resubmission` (always `[]` after a successful commit).

Data rules: field IDs are never reused (retired IDs). A rollback keeps the retired IDs of the undone lineage
except those that are live in the restored schema. A replacement system may not need access the world does
not grant (access does not grow in a migration). Migration documents are limited to 256 KiB; transform and IR
documents to 64 KiB. At most four plans are kept per world (the oldest is replaced).

Test seams (test workers and the test supervisor only, `SU_TEST_FAULT_HOOKS`): `SUTEKH_TEST_KILL_STAGE`
(`migrate.prepare`, `migrate.swap`, `migrate.capture`, `migrate.step`, `migrate.rollback`) kills the worker at that
stage when `SUTEKH_TEST_KILL_TOKEN` names a file: the file is unlinked first (one kill), or, when its first byte
is a digit n >= 2, it is rewritten with n-1 and the kill happens (n kills in total, the later ones in replacement
workers); `SUTEKH_TEST_KILL_MODE=abort` aborts instead. `SUTEKH_TEST_POST_CK_NOROOM_TOKEN` names a file that
makes the next post-commit checkpoint report no room (the file is unlinked; read by the supervisor).

Not in this section (deviations, see the M2-01b handoff): inline migration documents (paths only); native
kernels admitted but not yet installed are not tracked per world (installed kernels of the old schedule are
released when the schedule is replaced); the native build lane is not exercised by the migration tests.
