# Hot replacement, state transactions, and failure recovery

## 1. Guarantees by change class

| Change | Supported first | Required behavior |
|---|---|---|
| Code body; identical schema | M0 | Same active worker PID; preserve state; activate at tick boundary |
| Invalid source/effects | M0 | Reject; no active code/state change |
| Restore an explicit snapshot | M0 | Restore exact schema, semantics, tick, and state lineage |
| Native implementation of identical IR | M1 | Admit after differential tests; retain old code until quiescent |
| Add field with default | M2 | Preallocate, migrate, rebind, commit atomically |
| Remove/retype field or relationship rewrite | M2+ | Explicit migration; dependency checks; reject unsupported cases |
| Resident core ABI change | Any stage | Targeted rebuild and controlled worker replacement may be necessary |
| Native crash | M1+ | Replace worker; restore checkpoint; replay committed inputs |

The entire game does not magically remain uninterrupted after arbitrary native corruption. Ordinary supported code replacement does not require a process restart; fault recovery is a different operation.

## 2. Candidate lifecycle

```text
RECEIVED
  → SOURCE_VALIDATED
  → EFFECTS_VERIFIED
  → PREPARED
  → PREVIEW_TESTED
  → ADMITTED
  → PENDING_BOUNDARY
  → INSTALLED
  → RETIRED
```

Any pre-install stage can end in `REJECTED`, `STALE`, or `CANCELLED`. An installed revision can be superseded or involved in recovery. A revision receives a unique transaction ID and immutable source/dependency fingerprints. A later save does not mutate an existing candidate in place.

A file watcher is only a convenience around `capsule.submit`. It must handle partial writes through a stable-byte capture or atomic manifest publication, coalesce bursts, and discard superseded preparation. Explicit submission is the authoritative test path and avoids file-system notification timing ambiguity.

## 3. Code-only installation

Prepare the new IR, effects, field bindings, scratch capacity, and replacement schedule outside the tick-critical section. Recheck expected active revision and layout generation before installation. At a boundary, stop dispatching the old schedule, finish already-dispatched jobs, atomically publish the complete new manifest/schedule generation, and resume.

M0 has a serial scheduler; this is an inexpensive safe point, not a generalized lock-free publication mechanism. M1 can use generation/epoch retirement once parallel jobs exist. No worker may observe half old and half new dependencies inside one semantic tick.

A behavior-changing swap is recorded in the replay log with an exact effective tick and semantic hash. A native-only optimization retaining semantic hash is an artifact change, but still requires valid bindings and numeric equivalence.

## 4. Committed versus working state

The **committed root** is the last accepted full simulation tick. A **working root** is the state being computed for the next tick. Candidate previews operate on independent roots. A system's staged writes become visible to subsequent systems in that working tick only after the system succeeds. External readers see only committed roots.

For M0, choose the simplest auditable implementation: copy committed columns into a reusable working buffer at tick start; preallocate per-system output scratch; publish a new committed root only after the tick succeeds. This has measurable copying overhead and is not the release-performance design. Optimize it into dirty chunks, undo logs, or page-level copy-on-write only after tests demonstrate equivalent behavior.

M0 has no external effects, so discarding a failed working root is enough for logical rollback. If the worker process dies, in-memory roots die with it: recover from a supervisor-held checkpoint/log, not from a pointer the process can no longer access.

## 5. Checkpoints and logs

A checkpoint contains format version, world revision, schema definitions and generations, ordered entities, canonical field values, tick, command queues, PRNG state when introduced, semantic manifest, and effect commit index. Exclude addresses, padding bytes, file handles, GPU handles, and wall-clock state.

The supervisor stores an immutable checkpoint plus accepted input and semantic-revision events after that checkpoint. Checkpoint materialization and retention are resource-bounded. Recovery recreates the worker with the checkpoint's manifest, replays inputs under the recorded semantic revisions, then resumes at the last accepted boundary. Repeated failure at a tick quarantines the offending revision instead of looping forever.

Do not claim constant-time cloning. M0 snapshots are bounded deep copies. Large-world costs are measured as bytes copied, memory overhead, and time, not hidden inside “world fork” latency.

## 6. State migration

An additive field migration allocates the new column, writes defaults, validates values, prepares new bindings/artifacts, and publishes a new schema/world generation. Existing fields keep stable IDs. A rename preserves field ID and changes only naming metadata. Removing a field that any admitted capsule still reads is rejected.

Destructive migrations declare source and destination schema versions, pure transform logic, bounds, resource reservation, and recovery policy. A noninvertible change cannot be rolled back by running a guessed inverse; retain the old checkpoint/root until acceptance and retention policy allow disposal.

Large migrations are not required to fit the small code-reload budget. The default is to leave the old world active, prepare the new representation independently where possible, and perform a controlled commit only when consistent. Background migration must reconcile intervening writes explicitly. Without that machinery, pause at a checkpoint and report the operation as a controlled migration, not zero-latency reload.

## 7. Native code lifetime

Generation-tag every entry point and binding. Never reclaim a page while an invocation, queued job, stack frame, callback, or deferred task may use it. Track retiring generations and wait for a quiescent epoch. Cranelift's API similarly requires no executing functions or live callable pointers before releasing its code memory [S02].

Prepare code in writable nonexecutable pages, finish relocations, transition to executable nonwritable permissions, handle platform instruction-cache requirements, then publish. Windows explicitly requires the application to establish instruction-cache coherency for generated executable regions [S03]. Failure at any stage rejects the artifact; do not leave RWX memory as a fallback.

## 8. Multiplayer policy

Live code editing is development-only by default. A lockstep session pins the semantic manifest and content set. Changing semantics requires a coordinated development-session barrier with all peers acknowledging the same version, or a new session. Native artifacts may differ by hardware only after profile-specific equivalence testing; semantic identity remains the same.

Do not install arbitrary local balance changes in a multiplayer match. Network packets and replays carry version identity so divergence due to code mismatch is diagnosed separately from simulation defects.

## 9. Required failure tests

Reject parse/type/effect errors, stale expected revision, stale schema generation, unavailable memory, preparation cancellation, and unsupported migration without touching active state. Kill a candidate worker and confirm the active worker and supervisor continue. Later kill a native active worker and verify checkpoint/log recovery, explicitly recording its changed PID.

Test repeated code revisions and retirement for memory leaks. Test a failed second system after a successful first system to ensure the published tick root remains unchanged. Test a schema change while an old native generation still has in-flight work; reclamation must wait.

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).
