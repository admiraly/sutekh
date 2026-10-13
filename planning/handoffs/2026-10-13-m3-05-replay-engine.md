# Handoff: M3-05 slice, T-RTS-REPLAY-1..3 against the engine

Base: `5e77760`.

## Change
- `bench/rts/replay/engine_backend.py`: `EngineBackend` for the rts-replay/0.1 recorder and replayer, over a real
  `sutekh serve` with the full six-system RTS world. Scene load through `world.spawn_batch` in 1,024-row calls;
  commands are the supervisor methods; digest records: state = SHA-256 of the canonical snapshot bytes
  (`world.snapshot`), events = SHA-256 of canonical JSON lines of the retained `events.read` records, counts from
  `world.inspect` and `events.read`. Header: `operator_manifest_hash` over the host entries of the semantic manifest,
  `system_manifest_hash` over all entries, `schema_digest` over the schema list. Engine digests are the engine's;
  they are not byte-compatible with the model backend (Q8, still open).
- `bench/rts/replay/rts_session.py` moved here from `tests/rts/load/` (bench code must not import tests).
- `rts_replay.Recorder.header` takes `schema_digest` and `system_manifest_hash` from the backend when it has them.
- `worker.c` `world.inspect`: `entities.truncated_rows` when the schema has `truncated`.
- `sutekh_main.c`: `SUTEKH_CHECKPOINT_INTERVAL_TICKS`.
- `tests/rts/replay/test_replay_engine.py` (ctest `rts_replay_engine`): REPLAY-1 (identical replay; two tick workers
  chunked from one row), REPLAY-2 (changed command value reports t 6, the command's tick; dropped set_fields
  reports t 3; changed operator manifest and `control_events_counted = true` fail closed), REPLAY-3 (SIGKILL before
  the step of tick 37 in a run whose digests are at t 0 and 40 only; recovery from the interval checkpoint at 32
  with 4 logged operations replayed; final digest equals the uninterrupted recording).

## Evidence (WSL Ubuntu 26.04, gcc 15.2.0)
- `rts_replay_engine` 15/0. Full ctest and native lane: see the commit message.

## Findings
- A digest record costs a `world.snapshot`, which the supervisor also keeps as a checkpoint. A replay that digests
  every tick therefore never exercises log replay; REPLAY-3 uses a sparse recording for that reason. The contract's
  digest_every K should be chosen with this in mind for recovery evidence.

## Unrun / gaps
- REPLAY-3 fault seams inside `rts.spatial` and `rts.scatter` returning `SU_E_INTERNAL` (needs test-only seams in
  the host operators).
- Engine-versus-model digest equality (Q8): engine digests are compared only with the engine.
- T-RTS-LOAD-1 (e)/(f), T-RTS-CAPS-3 world-scale acceptance, T-RTS-CHURN-1.
