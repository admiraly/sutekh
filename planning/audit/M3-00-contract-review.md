# M3-00 contract r1 independent review

Reviewed: `planning/contracts/M3_RTS_CONTRACT.md` (r1, commit 9183c77). The contract was not edited.
Checked against: `AGENTS.md`, `ENGINE_CONSTITUTION.md`, `MILESTONES.md`, `ARCHITECTURE.md`, `LIVE_IR_SPEC.md`, `AGENT_PROTOCOL.md`, `planning/contracts/M2_ENTITIES_CONTRACT.md`, `planning/contracts/M2_MIGRATION_CONTRACT.md`, `planning/tasks.json`, `planning/status.json`, and code at `include/sutekh/{ir,status,entities}.h`, `src/runtime/schedule.{h,c}`, `src/world/snapshot.{h,c}`, `src/agent/su_proto.c`, `src/agent/su_agent.c`, `src/platform/sutekh_main.c`.

Probe scripts (scratch, not committed): a Python re-implementation of the section 4.1(c) query on a congested blob, and JSON size estimates for spawn_batch rows and snapshots.

## Verdict: accept after fixes

r1 must not be owner-accepted as written. The design direction is sound: no new IR opcodes is consistent with the 0.1 table, row-local loads force host operators for gather/scatter/index, generational gating of stale references is sound under M2 semantics, and the numeric bounds in `rts_u32_v1` are correct. The blocking items below are contradictions with the current code or unimplementable/unsafe semantics. Each has a concrete r2 fix. Nothing here needs a redesign of the M3 scope.

Classification: **BLOCKING** = must be fixed in r2 before owner acceptance. **BLOCKING (M3-07)** = does not block the freeze, but blocks starting M3-07. **NON-BLOCKING** = fix in r2 or in the owning task's first patch.

---

## Blocking findings

### F1. Host operators cannot be scheduled with the current scheduler (BLOCKING)
- Contract: sections 3, 4, 14 and 0 D9 say `src/runtime/schedule.h` is NOT changed; six systems, four of them host operators or IR 0.1/0.2, share the schedule with canonical `system_index`.
- Code: `SuSysEntry` (`src/runtime/schedule.h`) holds `SuLirModule *mod` and `SuExecContext *ctx`. `su_sched_add` takes only an `SuLirModule`. Effects (`src/runtime/jobs/su_effects.c`) and the union field table are derived from modules. Chunked execution (`su_jobs_run_system_chunked`) is IR-only. A host operator therefore has no entry type, no effect derivation, no dead-row zeroing hook (M2-00 section 1 zeroes after each system's commit) and no manifest hash.
- Fix: approve an explicit `schedule.h` change that adds a host-operator entry kind (name, registration record with declared read/write sets, `operator_version`, and a `commit` step that runs `su_world_zero_dead` as IR systems do). Add `src/runtime/schedule.*` and `src/runtime/rts/` to the leases in section 14 and to OD-list. State that the 6-entry schedule and `system_index` order are unchanged.

### F2. Congested targeting is not "nearest" and is almost always truncated (BLOCKING)
- Contract: section 4.1(c), section 7.4 (`congested.blob.v1`), section 10.1 `T-RTS-SPATIAL`, OD-6.
- Cause: the 64-entry budget is spent in row-major cell order from the box corner. Congested density is about 256 units per 256-unit cell, so the budget is exhausted inside one or two cells, and the nearest enemy is usually never examined.
- Measured (scratch probe, uniform 2048 x 2048 centre box, teams interleaved, plain PRNG, not the contract generator):

  | N | range_sq | target_valid | truncated | oracle: enemy in range exists | valid but not nearest |
  |---|---|---|---|---|---|
  | 1024 | 65536 | 1005 / 1024 | 965 / 1024 | 1024 | 561 |
  | 1024 | 262144 | 951 / 1024 | 1024 / 1024 | 1024 | 808 |
  | 4096 | 65536 | 3031 / 4096 | 4096 / 4096 | 4096 | 2938 |
  | 4096 | 262144 | 2325 / 4096 | 4096 / 4096 | 4096 | 2267 |

  So the spec's nearest-enemy rule is wrong for most queries in the scene it is meant to exercise. Section 16 risk 3 calls this "approximate", but 55 to 97 percent of found targets are not the nearest, and the truncated fraction is about 100 percent.
- Fix (owner decision, OD-6 with numbers): choose one and re-measure with the probe before freezing the constants.
  (a) Visit cells in Chebyshev rings from the querier's own cell outward, and use a finer grid or per-cell item ordering so that the nearest candidates fall within the budget; or
  (b) make the budget per ring or per cell instead of global; or
  (c) keep K and define a bounded exact query for congested scenes.
  Add an acceptance metric to M3-06/M3-08: per scene, report `truncated` fraction and `valid_but_not_nearest` against the Python exact oracle on a 1,024-row congested scene. Remove "nearest-enemy" wording from any query that can truncate.

### F3. `incoming` wraps mod 2^32, so a lethal hit can vanish (BLOCKING)
- Contract: D5, section 4.3, section 10.1 `T-RTS-SCATTER` "sum wrap at 2^32", M3-M08 (saturating add mutant).
- Reachability: `damage` is 1..65535 and set by control (`set_fields`), so the weapon table (max 5) is not the bound. About 65,537 arrivals at one target in one tick with damage 65535 wraps the sum. With N up to 1M rows this is reachable. A wrapped `incoming` makes `min(incoming, hp)` small and the unit survives a lethal volley. The test currently asserts the wrong behaviour.
- Fix: use a saturating add, `incoming = min(incoming + dmg, 0xFFFFFFFF)`, under a distinct op name. It is still commutative and associative, so D5's order argument holds. Change the sum-wrap test to saturation and keep M3-M08 as the mutant that detects wrap. Alternatively bound `damage * max_arrivals` in the profile and reject violations.

### F4. `world.spawn_batch` with 4096 rows exceeds the 1 MiB protocol message cap (BLOCKING)
- Contract: OD-3b (`max 4096 per call`), section 14 M3-05, section 7.5 scene load.
- Code: requests are limited to `SU_LIMIT_PROTOCOL_MESSAGE` (1 MiB), enforced in `src/agent/su_proto.c` (around line 322).
- Measured: a 39-field named row encodes to about 714 bytes of JSON, so one request holds about 1,468 rows. 4096 rows is about 2.9 MB and is rejected. Field-id keys would give about 3,000 rows at most.
- Fix: set the per-call maximum from the real encoder with a stated margin (e.g. 1,024 rows, or 1,024 with field ids), and state the 1 MiB bound in OD-3b. Add a test that a maximum-size batch is accepted and one byte more is rejected. Update the scene-load arithmetic (1M rows then needs about 1,000 calls, each one recovery-log entry).

### F5. Checkpoint recovery and the stress sweep cannot reach the claimed N (BLOCKING)
- Contract: section 9.3 (checkpoint spool), section 12 (sweep to the 1M default), T-RTS-REPLAY-3 (kill worker, recover from checkpoint plus log), section 16 risk 7.
- Code: `src/world/snapshot.h` sets `SU_SNAPSHOT_MAX_BYTES = 64 MiB` for both input and output, and `snapshot.c` refuses capture when `hw * (field_count + 1) > 32 MiB` (about 838k rows at 39 fields). Snapshots are canonical JSON.
- Estimate: about 6 bytes per value gives about 234 bytes per row, so the 64 MiB output cap binds at roughly 250k to 290k rows. A 1M-row checkpoint is about 220 MiB. The spool default is 32 MiB in the library and 512 MiB in the `sutekh` binary (`src/platform/sutekh_main.c`). These estimates are not measurements; measure them with the real encoder.
- Fix: (i) state the largest N for which checkpoint recovery is supported at the measured bytes per row; (ii) add `snapshot_cap` and `spool_budget` to the section 12 stop rule as named first-failing limits, so they are not reported as memory limits; (iii) either make N above the cap a recorded limit (no recovery claim) or propose an owner-approved binary snapshot encoding (new snapshot format, not in this contract). Do not claim 1M-row recovery in M3-05 or M3-08 closure evidence without it.

### F6. Per-tick caps can deterministically stall a valid world (BLOCKING)
- Contract: section 3 boundary, section 16 risk 1, T-RTS-CAPS-1 (abort with `SU_E_RESOURCE_LIMIT`, state unchanged).
- Code and M2: a tick over `max_struct_per_tick` (default 4096) aborts and retries identically. `SU_EVENT_DESPAWNED` also counts against `max_events_per_tick` (default 4096). The caps are `world.create` configuration (M2-00 section 5), not changeable on an existing world. Lockstep volleys at N = 16k can plausibly kill more than 4096 units in one tick, so the world is stuck at that tick.
- Structural fact that resolves it: each row despawns at most once per tick, so deaths per tick are at most alive units, which is at most capacity.
- Fix: for RTS worlds set `max_struct_per_tick >= capacity` and `max_events_per_tick >= capacity` (the cap is then unreachable by construction), and keep T-RTS-CAPS-1 as a test with an explicitly smaller configured cap. Record the configured values in the replay header (already listed). Add to section 9.2 that the event digest covers only retained events after ring eviction, and that `dropped` is part of the canonical state.

### F7. `world.set_fields` is single-entity but is justified as a batch (BLOCKING, interface)
- Contract: D6 and OD-3 (`entity` singular, "batch write of non-key fields").
- Effect: orders and formation changes for a 16k-unit army become 16k calls, 16k recovery-log entries and 16k revision bumps. This is the exact problem D6 is meant to solve.
- Fix: `world.set_fields {world_id, expected_world_revision, entities:[{entity, values}]}` with a row cap derived from the 1 MiB message bound (F4), one recovery-log entry per call, one revision bump per call. Use field names as in `world.spawn`. State event emission: `set_fields` emits none; state explicitly what `spawn_batch` emits (M2 `world.spawn` emits `SU_EVENT_SPAWNED`, so 1M events would overflow the log and the per-tick cap).

### F8. Python and C scene generators are both required, but no task owns the C generator (BLOCKING)
- Contract: section 7.2 and 7.3 ("generated in process" for N >= 65,536), T-RTS-SCENE-2 ("Python and C generators produce identical digests"), section 8.
- Tasks: M3-01 scope is `bench/rts/gen/` (Python). No task writes C code for scene generation.
- Fix: choose one. Either (a) the Python generator is the only generator: scenes are loaded through `spawn_batch`, and the test compares the Python canonical digest with the engine state digest after load; or (b) assign a C generator to a named task with its own lease. Remove the "in process" wording unless (b) is chosen.

### F9. Agent behaviours B1 to B3 cannot be implemented in their stated scope (BLOCKING (M3-07))
- Contract: section 11. This does not block the freeze, but M3-07 must not start from this text.
- B1 (retreat): `rts.move` stops whenever `target_valid` (section 5.3), so a retreating unit with an enemy in range cannot move. Changing `rts.goal` and `rts.combat` alone cannot override that. `goal` also needs `hp` and `team` in its declared reads, which section 5.1 does not list.
- B2 (projectile retarget guard): needs the current generation of the projectile's target. IR loads read only the current row (LIVE_IR_SPEC section 1), so combat cannot read it. This needs a host operator change (spatial or scatter), so it is not "IR-only" and it changes a native-free host path.
- B3 (formation hold): the current rules already hold. With `anchor_valid = 0` and no order, `goal = pos`, and `move` does not move. There is no failing baseline, so the hidden fixture would pass before the change. The task is either invalid or needs a real defect.
- Fix: rewrite B1 with `rts.move` in its write scope and the extra reads declared; rewrite B2 as a host-operator change with its own lease, or drop it; replace B3 with a measured baseline failure or drop it.

---

## Non-blocking findings

### N1. Cooldown and reload off-by-one (NON-BLOCKING)
- Contract: section 5.2 says `cooldown' = select(fire, reload, ...)`, the prose says "cooldown ticks after firing", and the shot-rate bound is `max(reload, flight+1)`.
- Derivation from the IR as written: fire at tick t sets cooldown = reload. The check uses the tick-start value, so `cd0` first holds at tick t + reload + 1. The projectile is freed at t + flight, so the earliest re-fire is `t + max(reload + 1, flight + 1)`. The stated bound is wrong by one for reload.
- Fix: decide the intended period. Either set `cooldown' = reload - 1` (for reload >= 1) so that re-fire is at t + reload, or change the prose and bound to `max(reload + 1, flight + 1)`. Make the `T-RTS-COMBAT` known answers and the Python model follow the chosen rule.

### N2. Flag used as a select condition (NON-BLOCKING)
- Section 5.1: `has_order` is defined as a u32 flag but used as `select(has_order, ...)`. `select` needs a bool, so the verifier would reject it. Section 6.1 gives the rule (`eq_u32(flag, 1)`), but 5.1 does not follow it. `le(a,b)` in 6.1 has the same problem.
- Fix: write `select(eq(has_order_flag,1), ...)` and make every flag-to-bool step explicit. Run the generator's type check on every pseudocode line before freezing.

### N3. Truncation definition (NON-BLOCKING)
- Section 4.1(c).3 counts `examined` before the budget test and then says "truncated = 1 only if at least one entry remained unexamined". The wording is ambiguous.
- Fix: define `truncated = 1` iff a (K+1)-th candidate entry exists in the box, counting self and friendly entries in visit order. Give the K and K+1 fixtures the exact expected flags.

### N4. Memory estimate omits categories (NON-BLOCKING)
- Section 9.3 gives about 352 bytes per row. Adding the entity table (12 bytes) and spatial items (4 bytes per indexed row) gives about 368 bytes per row before the fixed grid (about 128 KiB), per-chunk structural and event buffers, the event log, and the checkpoint spool (F5).
- Fix: state the formula per category, and state that the 352 figure covers columns only. Mark all as estimates until OD-10.

### N5. Error code for out-of-range `range_sq` is inconsistent (NON-BLOCKING)
- Section 4.1(c).1 says "reject at profile validation" and "SU_E_INTERNAL if reached". Section 13 says `SU_E_RESOURCE_LIMIT`.
- Fix: the control plane returns `SU_E_TYPE` (domain violation, like positions). The operator returns `SU_E_INTERNAL` only if an invariant is broken.

### N6. Task split does not match leases or dependencies (NON-BLOCKING, must fix before orchestration)
- Section 14 writes to `src/runtime/rts/`, `tests/rts/`, `bench/rts/`, `profiles/rts/native/`, `backends/native/` and `planning/` only. It also needs `src/runtime/schedule.*` (F1), `src/agent/` routing and `schemas/` and `docs/AGENT_RPC.md` for the two control methods (F4, F7), and `src/world/` for OD-10. None of these is in `planning/tasks.json`, and `tasks.json` M3-00 has only `profiles/rts/`, `capsules/rts/` and `bench/rts/`.
- M3-04 (host operators) needs M3-02 as a dependency, because section 10.1 known answers come from the Python model. M3-08 depends on OD-10 (a `src/world` task that does not exist).
- Fix: add the leases and tasks listed above to `tasks.json` through the orchestrator, and map the old M3-00 acceptance text to its new home (movement and formation tests to M3-03, end-to-end and stress to M3-06 and M3-08). Do not edit `tasks.json` from the contract.

### N7. Formation behaviour is never exercised in the committed scenes (NON-BLOCKING)
- Section 5.1: an explicit order overrides formation membership. Section 7.4: group orders are attack-move for the whole group. If the order goes to every member, formation-follow never runs in `mixed.battle.v1` or `congested.blob.v1`.
- Scene sizing is also unspecified: a group is 1 anchor plus 16 members (17 rows), and 1024 mod 17 is not zero, so the remainder rule is missing.
- Fix: order the anchor only, so members with mode 0 follow anchor plus offset. Add an acceptance check that at least one member in each formation has `anchor_valid = 1` and no order on a tick before the order is issued. Define the group count for each N and how remainder rows are filled.

### N8. Operator constants are native gameplay semantics (NON-BLOCKING, design statement)
- Section 4.4 and risk 2: `K`, cell size, tie-break and the scatter rule live in native C. Changing them is a native build outside C01's guarantee, and ARCHITECTURE section 5 says a gameplay rebalance must change the semantic revision that replays carry. Nothing in section 9.1 records operator versions. `system_manifest_hash` covers IR only.
- Fix: state that these are profile constants with a native build, add `rts_ops_v1` version and constants to `system_manifest_hash` and to the replay header, and make replay fail closed on mismatch (as M2-01 section 6 does for schema).

### N9. Recovery of host-operator effects (answer to the review question, NON-BLOCKING)
- Are `rts_ops_v1` effects replayable from the op log? Yes, by re-execution, not by logging effects. The recovery log stores control commands and tick deltas (M2-00 section 5). The operators are pure functions of tick-start state plus the schedule. The spatial grid is rebuilt every tick and is not state, so a checkpoint needs no operator state. Scatter applies from `incoming` which is canonical state. This is sound only if the operator version is identical (N8).
- In-process native faults (AGENTS.md) must be recovered by checkpoint replay in a replacement worker. Section 10.3 T-RTS-REPLAY-3 kills a worker but does not inject a fault inside `rts.spatial` or `rts.scatter`. Checkpoint cadence is not stated.
- Fix: add a fault seam inside each host operator (returns `SU_E_INTERNAL` or aborts the worker) and extend T-RTS-REPLAY-3 to cover it. State checkpoint cadence and the maximum ticks replayed on recovery.

### N10. Read sets in the table do not match the IR formulas (NON-BLOCKING)
- Section 2 table versus section 5 formulas: `kind` is listed as read by combat and damage (neither reads it). `range_sq` is listed as read by combat (not used; `target_valid` implies range). `formation_slot` is listed as read by goal (not used). `target_slot` is listed as read by move (move uses `target_valid` only).
- Fix: make each table reads column equal the IR's declared reads. C04 requires the declaration to match what the verifier derives.

### N11. Profile validation gaps (NON-BLOCKING)
- Section 2 says units need `hp >= 1` at spawn, but only `kind = 0` is rejected. A `kind = 1` row with `hp = 0` is invisible to spatial and never dies.
- Fix: reject `kind = 1` with `hp = 0` as `SU_E_TYPE` in `spawn`, `spawn_batch` and `set_fields`.

### N12. Replay content and scene identity (NON-BLOCKING)
- Section 9.1 does not say whether the initial scene is re-generated from the header or carried as spawn commands. For 1M rows, embedding spawn records in a replay is not practical.
- Fix: state that the initial scene is regenerated from `generator`, `params` and `seed`, and that the loaded state digest must match `scene_digest`, failing closed otherwise.

### N13. Cost estimates need explicit ceilings (NON-BLOCKING)
- Section 6.2 sums to about 245 IR instructions per row per tick (goal 60, combat 110, move 50, damage 25). At 1M rows that is about 2.5e8 interpreted instructions per tick, and a 120-tick run at that size is hours per scene on the reference VM. The pure-Python oracle at 16,384 rows over 120 ticks is about 5e8 interpreted instructions, which is also hours. These are estimates, not measurements (C10).
- Fix: M3-02 sets an explicit oracle budget (for example 16,384 rows for N ticks) and records `oracle: not_run` when it is exceeded. M3-08 sets wall-clock ceilings per point before the sweep.

### N14. Mutation coverage gaps (NON-BLOCKING)
- Add mutants for: grid indexing `kind = 2` entries; team filter removed; domain-edge box clipping removed; gather without the `alive` check; `dmg_out` set on non-arrival ticks; `incoming` not zeroed on a dead row in the native path; `proj_target_gen` not cleared on arrival; per-tick cap default too low for the scene (should fail a stress fixture).
- Fix: add to section 15 with named tests.

### N15. Capsule contract for IR systems (NON-BLOCKING)
- C04 requires each capsule to declare identity, schema deps, field reads and writes, ordering edges, numeric profile, capabilities, resource bounds and test obligations. Section 14 M3-03 writes `capsules/rts/` but does not specify those files or the `limits` keys for `rts.damage` (`max_struct_per_row = 1`).
- Fix: list the capsule contract fields and the `capsule.schema.json` changes in M3-03.

### N16. Rounding in the scene PRNG (NON-BLOCKING)
- Section 7.1 says `uniform(n) = (u32 * n) >> 32` has "no modulo bias". Multiply-high is not exactly unbiased; the bias is at most about n / 2^32.
- Fix: say "bias below n / 2^32, fixed by the lock". The digest test does not change.

---

## Confirmed correct (checked, no change needed)

- **IR expressibility (D1):** `ir.h` 0.1 ops are `CONST, LOAD, ADD, SUB, MUL, EQ, LT, SELECT, STORE`. Every formula in sections 5.1 to 5.4 uses only these, apart from the flag-to-bool issue in N2. `despawn_self` is 0.2 (opcode 13) and is listed as such.
- **Row-local loads:** LIVE_IR_SPEC section 1 and section 5 forbid cross-entity addressing, so D2 is necessary, not optional.
- **Scheduler cap:** `SU_SCHED_MAX_SYSTEMS` is 8 (`schedule.h`), so 6 entries fit. The cap itself is not the blocker; F1 is about entry type.
- **Spatial bounds:** `rc = (R+255)>>8` with `R = floor(sqrt(range_sq))` is the exact cell radius for a distance of at most R. The box holds at most 25 cells for `range_sq <= 262144`. The grid index `(y>>8)*128 + (x>>8)` is in `0..16383`. `dx^2 + dy^2 <= 2*32767^2 = 2147352578 < 2^31`, so no u32 overflow in the distance test.
- **Anchor and clamp math:** `a + ox - BIAS` with `ox` in `0..32767` and the `2^31` sign threshold clamp are correct for in-domain anchors.
- **Stale references:** M2 despawn increments the stored generation, and generation 0 is stale (M2-00 section 1). So the `(slot, generation)` gates in gather and scatter reject stale anchors and stale projectile targets after slot reuse. Scatter after a same-tick kill is handled by the `alive` check. Projectiles of a despawned shooter disappear with its row.
- **Death in the scatter/damage order:** deaths happen at the boundary, after scatter, so same-tick arrivals on a dying unit are counted and then the kill applies once. This is correct.
- **Status codes:** every code used in sections 10 and 13 exists in `include/sutekh/status.h` (`SU_E_STALE_REVISION` 10, `SU_E_STALE_HANDLE` 21, `SU_E_MISSING_STORE` 20, `SU_E_UNDECLARED_*` 7 and 8, and the others).
- **PRNG:** the constants (`0x9E3779B97F4A7C15`, `0xBF58476D1CE4E5B9`, `0x94D049BB133111EB`) and the mixing order are standard splitmix64.
- **Control names:** `world.spawn`, `world.despawn`, `events.read` and `world.inspect` exist. `world.set_fields` and `world.spawn_batch` do not exist yet, which the contract correctly marks as proposals.
- **Memory arithmetic:** 39 fields times 4 bytes times 2 roots plus 40 bytes of staging is 352 bytes per row, as stated (N4 is about omissions, not arithmetic).

## Blocking summary for r2

| id | fix in one line |
|---|---|
| F1 | Approve a `schedule.h` host-operator entry; add leases. |
| F2 | Ring-order or per-ring budget; measure with the probe; add `valid_but_not_nearest` metric. |
| F3 | Saturating `incoming`; change the sum-wrap test and M3-M08. |
| F4 | Cap `spawn_batch` by the 1 MiB message bound (about 1,024 rows). |
| F5 | State the checkpoint-recoverable N from measured bytes; add `snapshot_cap` and `spool_budget` to the stop rule. |
| F6 | Set per-tick death and event caps to at least capacity for RTS worlds; add the ring-eviction digest rule. |
| F7 | `set_fields` takes `entities[]`, one recovery entry per call. |
| F8 | Pick one scene generator and assign it. |
| F9 (M3-07 only) | Rewrite B1 scope to include `rts.move`; make B2 a host-operator change or drop it; replace B3. |

## Owner decisions this review adds

1. F1: approve a host-operator entry kind in `schedule.h`.
2. F2: choose the query budget design, using the probe numbers.
3. F3: approve saturating add for `incoming`.
4. F5: accept a checkpoint-recovery N limit, or propose a binary snapshot encoding.
5. F8: which scene generator is authoritative.

No measured performance claim is made in this review. All byte and instruction figures are analytic estimates or scratch-probe results, labelled as such.
