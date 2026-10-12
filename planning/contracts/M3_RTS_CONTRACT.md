# M3-00 contract r2: RTS microcosm (synthetic hordes, formations, movement, targeting, projectiles, damage, spatial load)

Status: DRAFT r2 for owner review. Design only; no product code is part of this contract. Builds on M2-00 (generational entities, IR 0.2, per-chunk struct/event buffers, snapshot 0.2) and M2-01 (migrations, snapshot 0.3, recovery log). Nothing here is accepted until the owner accepts this revision. Implementation of any task below is gated on M2 owner acceptance (`planning/status.json` shows M2 `implemented_audited_pending_owner_acceptance`).

Scope: a synthetic, non-BFME RTS profile. No original game assets, no BFME behaviour claim (C18, C19; M4 owns compatibility). Headers `include/sutekh/abi.h`, `ir.h`, `entities.h`, `status.h` are NOT changed by this contract. `src/runtime/schedule.h` is changed only by the additive host-entry kind in OD-1b, and only after owner approval. Every shared-interface change listed in section 13 requires its owner's approval.

Review input: `planning/audit/M3-00-contract-review.md` (r1 verdict: accept after fixes). Every finding F1-F9 and N1-N16 is resolved in section R below.

---

## R. Changelog r1 to r2

Code facts used for the resolutions, checked against the tree at r2 time (commit base 34fc03c):
- `src/runtime/schedule.h`: `SuSysEntry` holds `SuLirModule *mod` and `SuExecContext *ctx`; `su_sched_add` takes an `SuLirModule`. `mod` is dereferenced at 37 sites in `schedule.c`, `entity_tick.c`, `native_versions.c`, `jobs/su_effects.c` and `worker.c`.
- `src/agent/su_proto.c:322`: a request line over `SU_LIMIT_PROTOCOL_MESSAGE` (1 MiB, `ir.h:21`) is refused with `SU_E_RESOURCE_LIMIT`. The check covers the whole line, envelope included.
- `src/world/snapshot.h`: `SU_SNAPSHOT_MAX_BYTES` = 64 MiB for input and output. `snapshot.c:643` refuses capture when `hw * (field_count + 1) > 32 MiB`, which is a row guard of 838,860 rows at 39 fields.
- `src/agent/su_agent.c:62`: library spool default `AGENT_DEFAULT_SPOOL` = 32 MiB. `src/platform/sutekh_main.c:83`: binary default 512 MiB.
- `src/world/entities/su_cmdq.h:50,55`: `SU_CMDQ_DEFAULT_PER_TICK_CAP` = 4096, `SU_CMDQ_MAX_TICK_CAP` = 1,048,576 (2^20). Buffers are allocated as `min(chunk_size * per_row_cap, per_tick_cap)` (su_cmdq.h header), so raising a per-tick cap does not raise memory beyond `chunk_size * per_row_cap`.
- `src/runtime/worker.c:1077`: `world.spawn` emits `SU_EVENT_SPAWNED` per row.
- `src/runtime/schedule.c:246-265`: `run_one_system` dispatches native, chunked-VM or serial VM. `schedule.c:308` and `entity_tick.c:549` run `su_world_zero_dead` after every system commit.
- `su_world.h`: `su_world_build_invocation` (reads, working), `su_world_build_outputs` (writes, staging), `su_world_commit_staging`, `su_world_zero_dead` already exist.

Probe evidence (scratch scripts, outside the repo, not committed; M3-02 must reproduce them with the committed generator). Probe 1 uses a uniform 2048 x 2048 box, teams interleaved, plain PRNG, 600 seeded queriers per row, exact brute-force oracle. Probe 2 measures encoded row sizes with the 39 contract field names.

| id | finding | resolution in r2 | where |
|---|---|---|---|
| F1 | Host operators cannot be scheduled (`SuSysEntry`, `su_sched_add`) | Owner-approved additive host-entry kind in `schedule.h` (OD-1b): `kind` field, `SuHostOpDesc` (operator id, `operator_version`, declared read/write set, run function, constants hash). `mod` stays non-NULL as a declaration module with no IR body, so the 37 dereference sites keep working unchanged. Dispatch branch in `run_one_system`. Commit reuses the existing staging commit and `su_world_zero_dead`. Native lowering and IR candidates refuse host entries. Schedule remains 6 entries; `system_index` order unchanged. Leases added in section 14 (new task M3-04a). Rejected alternative: `mod == NULL` (37 sites, 5 files) and an IR stub with stores (fakes IR). | 3, 4.4, 14, 13 OD-1b |
| F2 | Congested targeting is not nearest and is truncated | Probe 1 (16k bow, r1 layout): 419 of 424 found targets not nearest; 176 to 294 of 600 queriers missed an existing in-range enemy. Probe 1 variants (600 queriers each, same seed): 256-unit cells, ring order, K=64: 465/600 not nearest. 64-unit cells, ring order, K=64: 89/600 not nearest at 16k and 0 at 1k and 4k. 64-unit cells, ring order, K=128: 9/600 not nearest at 16k (ratio p50 1.38, p95 2.51), 0 missed. 32-unit cells, K=64: 0/600 at 16k but a 1M-cell grid. Chosen: 64-unit cells (512 x 512 grid), ring order, K=128. New metrics `missed`, `valid_but_not_nearest` and distance ratio, with gates (section 10.6). "Nearest" wording removed from every query that can truncate. | 4.1, 4.2, 9.3, 10.6, 13 OD-6, 16 risk 3 |
| F3 | `incoming` wraps mod 2^32 | `incoming' = sat_add(incoming, dmg_out)` with `sat_add` saturating at 0xFFFFFFFF. Saturating add of unsigned values is commutative and associative (the sum of any multiset saturates to min(sum, MAX)), so D5's order argument holds. Wrap needs 65,538 arrivals at damage 65,535 (65,537 arrivals sum to exactly 0xFFFFFFFF), reachable at N up to 1M. A saturated `incoming` is still >= hp, so a lethal volley still kills. Sum test changed to saturation. M3-M08 keeps its id and flips meaning (renumber note, section 15). | D5, 4.3, 5.4, 10.1, 15 |
| F4 | `spawn_batch` 4096 rows exceeds the 1 MiB cap | Cap set to 1,024 rows per call (OD-3b). Probe 2 with the 39 field names and 10-digit worst-case values: 926 bytes per row, 948,380 bytes per 1,024-row request (9.5% margin under 1,048,576). Representative rows: about 654 bytes per row. A 1,132-row worst-case request is the absolute ceiling, so 1,024 is set with margin. Scene load of 1M rows is 977 calls. Maximum-size and one-byte-over tests added (T-RTS-LOAD-1). | D6, 7.5, 10.3, 13 OD-3b |
| F5 | Checkpoint recovery and sweep cannot reach 1M | Measured-form estimate (probe 2, representative and worst-case values): canonical snapshot 0.2 is about 175 bytes per row representative and 446 worst case, plus up to about 90 bytes per retained event. Recovery claim limited to N <= 65,536 (worst case 29 MB plus a full 65,536-event log, about 35 MB, which needs the 512 MiB binary spool, not the 32 MiB library spool). N = 262,144 is conditional on measured bytes per row. N >= 838,861 is refused by the row guard; 1M is not claimed. `snapshot_cap` and `spool_budget` are named first-failing limits in the section 12 stop rule. Snapshot-limit change or binary snapshot encoding is an owner decision (OD-15). | 9.3, 12, 13 OD-15, 16 risk 14 |
| F6 | Per-tick caps stall a valid world | Each row produces at most one structural request per tick and at most one death event, so deaths per tick <= capacity. RTS worlds set `max_struct_per_tick = max_events_per_tick = capacity`. Capacity must be <= `SU_CMDQ_MAX_TICK_CAP` (1,048,576), which covers the default 1M. Memory is unchanged (buffers are `min(chunk*per_row, per_tick)`). Ring-eviction digest rule added (9.2). Open sub-item: whether control-plane events applied at a boundary share the per-tick counter is not resolved by the code read; the rule is owner decision OD-3c and must be settled in M3-05. | 3, 9.1, 9.2, 10.3, 13 OD-3c |
| F7 | `set_fields` is single-entity but justified as a batch | `world.set_fields {world_id, expected_world_revision, entities:[{entity, values}]}`, at most 1,024 entities per call (worst case 886 bytes per entity, 907 KB per call). One revision check, one recovery entry and one revision bump per call. Emits no event. `kind` and `team` are immutable after spawn and rejected. Duplicate entities or any stale handle refuse the whole call with no state change. | D6, 13 OD-3, 10.3 |
| F8 | Two scene generators, no owner for C | Chosen option (a): the Python generator `bench/rts/gen/` is the single normative generator (owner M3-01). No C scene generator in M3. Large scenes are generated to a local, uncommitted row file and loaded through `world.spawn_batch` by the M3-05 harness. "Generated in process" removed. T-RTS-SCENE-2 now checks the engine-loaded state against the Python digest (where the snapshot cap allows the read-back). | 7.2, 7.5, 10.3, 14 |
| F9 | B1 to B3 not implementable or no failing baseline | B1 rewritten with `rts.goal`, `rts.combat` and `rts.move` in write scope, with the extra reads declared. B2 (projectile retarget guard) dropped: scatter already rejects a target whose generation changed (4.3, T-RTS-SCATTER), so the behaviour is base behaviour and has no failing baseline. B2 redefined as a kiting rule in `rts.move` (IR-only). B3 (formation hold) dropped: the r1 rules already hold, so there was no baseline failure. B3 redefined as formation catch-up (IR-only). Every behaviour has a fixture that fails on the r2 baseline. Owner decision OD-19. | 11, 13 OD-19 |
| N1 | Cooldown off by one | Fire sets `cooldown' = reload - 1` (reload >= 1 is validated, so no wrap). Re-fire is at `t + max(reload, flight + 1)`, matching the prose and the bound. Derivation in 5.2. | 5.2, 10.1 |
| N2 | Flags used directly as conditions | Every condition is a bool from `eq`/`lt`. A flag tests as `eq(flag, 1)`. Pseudocode in 5.1 and 5.3 rewritten. Rule in 6.1. | 5, 6.1 |
| N3 | Truncation definition ambiguous | `truncated = 1` iff `box_total > K`, where `box_total` is the number of indexed entries (self included) in the ring-box. Derived from cell counts, exact, and equal to "a (K+1)-th candidate exists". | 4.1(c) |
| N4 | Memory categories omitted | Per-category formulas in 9.3, with the 352 figure labelled columns-only. Spatial grid 2 MiB fixed (grid change). Structural buffers, event log and spool itemised. | 9.3 |
| N5 | Error code for out-of-range `range_sq` | Control plane returns `SU_E_TYPE` (domain violation). Operator returns `SU_E_INTERNAL` only on a broken invariant. | 4.1(c), 13 |
| N6 | Leases and tasks do not match | Section 14 rewritten with leases for every write path: `src/runtime/schedule.*`, `src/runtime/rts/`, `src/agent/su_agent.c` (method handlers), `schemas/`, `docs/AGENT_RPC.md`, `src/world/` (M3-08a only). M3-04 depends on M3-02 (known answers) and M3-04a, not M3-03. Ids M3-04a and M3-08a added; no existing id renumbered. | 14 |
| N7 | Formation never exercised; remainder rule missing | Orders go to the anchor only. Members with mode 0 follow anchor plus offset. Groups are 17 rows (anchor + 16 members). Remainder rows (N mod 17) are free units with no formation and their own attack-move order. Acceptance check added. | 7.3, 7.4, 10.1 |
| N8 | Operator constants not versioned | `rts_ops_v1` version and constants hash are part of the declaration (OD-1b) and of `operator_manifest_hash` in the replay header. Replay fails closed on mismatch. | 9.1, 4.4 |
| N9 | Host-operator recovery and cadence | Replay by re-execution (no effect log, spatial grid is not state). Fault seam in `rts.spatial` and `rts.scatter`. T-RTS-REPLAY-3 extended to inject faults there. Checkpoint cadence `checkpoint_every` = 32 ticks, so at most 32 ticks are replayed on recovery (OD-18). | 9.1, 10.3 |
| N10 | Read sets in table disagree with formulas | Table in section 2 corrected: `kind` read by spatial and scatter only; `range_sq` read by spatial only; `formation_slot` read by spatial only; `target_slot` read by combat only. | 2 |
| N11 | `kind = 1, hp = 0` accepted | Rejected with `SU_E_TYPE` in `spawn`, `spawn_batch` and `set_fields`. `kind` and `team` immutable after spawn. | 2, 10.3 |
| N12 | Replay scene identity | Initial scene regenerated from `generator`, `params`, `seed`; loaded state must match `scene_digest` or the replay fails closed. | 9.1 |
| N13 | Cost estimates without ceilings | M3-02 oracle budget set: scenes up to 4,096 rows and 120 ticks in CI; larger reports `oracle: not_run`. Figures are estimates, not measured. | 8, 16 risk 8 |
| N14 | Mutation gaps | Added M3-M21 to M3-M28. Gather without the `alive` check is recorded as an equivalent mutant (M3-M24): despawn bumps the generation, so the generation check already rejects dead anchors. | 15 |
| N15 | Capsule contract fields not listed | Capsule record fields, resource bounds, `rts.damage` limits and the test obligations listed in 5.5. `capsule.schema.json` change only with OD-16. | 5.5, 13 |
| N16 | PRNG bias wording | "Bias below n / 2^32; fixed by the committed lock." Digest test unchanged. | 7.1 |

Renumbering: none of the existing ids is renumbered. `planning/tasks.json` contains only `M3-00`, so ids M3-01 to M3-10 exist only in this contract. New ids: M3-04a (host-entry kind) and M3-08a (exact accounting). Meaning changes that a reader could misread: M3-M08 (now wrapping add must be detected), B2 and B3 (redefined), OD-6 (new grid and budget), OD-3 and OD-3b (new signatures), T-RTS-SCENE-2 (now engine-load check).

---

## 0. Decisions at a glance

| # | Decision | Why (short) |
|---|---|---|
| D1 | **No new IR opcodes for M3.** Boolean composition uses nested `select_u32`; a bool comes from `eq_u32(flag, 1)`; min via `lt`+`select`; abs-difference via `lt`+`sub`+`select`. | Every M3 per-row rule is expressible in the current 0.1/0.2 table (section 6). New opcodes would be speculative (C18). Re-evaluate only if measured counts or authoring error rates fail (6.4). |
| D2 | **Indexed gather, scatter-add and the spatial index are host-side RTS-profile phase operators** (`rts_ops_v1`), not IR. | IR loads read only the current row, so a runtime-indexed read or a write to another entity is inexpressible (LIVE_IR_SPEC sections 1, 5). Operators have fixed, bounded, reference-modelled semantics (section 4). |
| D3 | **Death is `despawn_self` in an IR 0.2 system** (reference VM only). Movement, goal and combat are IR 0.1 (native-eligible). | Keeps the native lane (IR 0.1 only) useful for per-row math; death uses the M2 path. |
| D4 | **Projectiles are per-shooter embedded state with a fixed flight time** (no spawn from IR, no homing, no division). | Spawn-from-IR is reserved and unsupported in M2. A fixed flight time avoids division and keeps the state a plain row. |
| D5 | **Cross-entity damage is `scatter_sat_add_u32` keyed by a generational target handle**, saturating at 2^32 - 1. | Saturating add of unsigned values is commutative and associative, so merge order cannot change the result. Canonical order is still recorded (4.3). Replaces the r1 wrapping add (F3). |
| D6 | **Two control-plane additions**: `world.set_fields` (batch of entities, non-key fields) and `world.spawn_batch` (scene load), each at most 1,024 rows per call under the 1 MiB request cap. | Orders and formation changes mid-run are otherwise one call per unit; a 16k army becomes 16k recovery entries (F7). Scene load of 1M rows is 977 calls (F4). OD-3, OD-3b. |
| D7 | **Spatial index**: dense 512 x 512 grid of 64-unit cells over the 32768^2 domain; ring-ordered query over at most 17 x 17 cells (rc <= 8) and at most 128 examined entries, with an explicit `truncated` flag and reported `valid_but_not_nearest` metric. | Probe 1 (section R, F2): 256-unit cells leave 78% of congested targets not nearest; 64-unit cells with K=128 leave 1.5%. Bounded work per query (C10, LIVE_IR_SPEC section 7). OD-6. |
| D8 | **Numeric profile `rts_u32_v1`**: all fields u32; coordinates 0..32767; squared distances < 2^31; no division in IR; host operators use compare, add, sub, shifts and an exact integer square root. | No float (C08); no overflow in distance math by construction. |
| D9 | **Six scheduled systems** under the existing `SU_SCHED_MAX_SYSTEMS` = 8: `rts.spatial` and `rts.scatter` are host entries (new kind, OD-1b); `rts.goal`, `rts.combat`, `rts.move` are IR 0.1; `rts.damage` is IR 0.2. | Schedule capacity unchanged. Only the entry kind is added, additively. |
| D10 | **One normative scene generator**: Python `bench/rts/gen/` (M3-01). No C scene generator in M3. | Removes the two-generator contradiction (F8). The engine-loaded state is checked against the generator digest. |
| D11 | **RTS world caps**: `max_struct_per_tick = max_events_per_tick = capacity`, requiring capacity <= 1,048,576. | Deaths per tick cannot exceed capacity by construction (F6). |

---

## 1. Entity model and identity

- One entity table, all rows are `u32` columns. Row kinds are data, not types:
  - `kind = 1` unit (participates in combat and the spatial index). Requires `hp >= 1` at spawn and in every write (N11).
  - `kind = 2` formation anchor (moves, carries no weapon, is never targeted, never damaged, `hp = 0`).
  - `kind = 0` reserved; never valid for a live row (validation rejects it with `SU_E_TYPE`).
- Handles are the M2 `SuEntity` (`generation << 32 | slot`). Every generation-checked reference in the schema stores the slot (u32) and the generation (u32) as two fields. Initial spawns use generation 1.
- Capacity is fixed at `world.create` (`max_entities`). Scenes never grow capacity. Spawn and despawn churn are bounded by capacity (M2 lowest-free-slot reuse).
- Dead rows are skipped by IR systems (VM alive mask) and by host operators (explicit `alive` check). Dead rows are canonically zero after every system commit (M2-00 section 1; `su_world_zero_dead`).

## 2. Schema and column layout

All fields are `u32`, stored as SoA columns, chunked by `SU_CHUNK_SLOTS` (1024). `NONE = 0xFFFFFFFF`. `BIAS = 16384`. Field ids are assigned in the order below, starting at 1, and are stable within a lineage (M2-01 rule). The reads column lists the systems whose formulas (sections 4 and 5) read the field; the verifier derives IR effects from IR, and host operators declare theirs (4.4).

| id | name | role | written by (store) | read by |
|---|---|---|---|---|
| 1 | `kind` | 0 invalid, 1 unit, 2 anchor; immutable after spawn | control plane only | spatial, scatter |
| 2 | `team` | 0 or 1; immutable after spawn | control plane only | spatial |
| 3 | `hp` | hit points; >= 1 for units at spawn and in writes; 0 for anchors | damage | damage, spatial |
| 4 | `pos_x` | 0..32767 | move; control | spatial, goal, move |
| 5 | `pos_y` | 0..32767 | move; control | spatial, goal, move |
| 6 | `speed` | units per tick, 0..1024 | control | move |
| 7 | `range_sq` | weapon range squared, 0 = unarmed, <= 262144 | control | spatial |
| 8 | `reload` | cooldown ticks, 1..1024 | control | combat |
| 9 | `cooldown` | remaining ticks | combat | combat |
| 10 | `damage` | damage per hit, 1..65535 | control | combat |
| 11 | `flight` | flight ticks, 1..64 | control | combat |
| 12 | `order_mode` | 0 none, 1 move (ignores enemies), 2 attack-move | control | goal, move |
| 13 | `order_x` | 0..32767 | control | goal |
| 14 | `order_y` | 0..32767 | control | goal |
| 15 | `formation_slot` | anchor slot, or NONE | control | spatial (gather) |
| 16 | `formation_gen` | generation of anchor at assignment | control | spatial (gather) |
| 17 | `formation_ox` | offset x + BIAS, offset in [-16384, 16383] | control | goal |
| 18 | `formation_oy` | offset y + BIAS | control | goal |
| 19 | `goal_x` | derived goal | goal | move |
| 20 | `goal_y` | derived goal | goal | move |
| 21 | `anchor_x` | gathered anchor x (0 if invalid) | spatial | goal |
| 22 | `anchor_y` | gathered anchor y | spatial | goal |
| 23 | `anchor_valid` | 0/1 | spatial | goal |
| 24 | `target_slot` | best examined qualifying enemy slot, or NONE | spatial | combat |
| 25 | `target_gen` | generation of target | spatial | combat |
| 26 | `target_dist_sq` | squared distance to target (0 if none) | spatial | (diagnostic) |
| 27 | `target_valid` | 0/1, equals `target_slot != NONE` | spatial | combat, move |
| 28 | `target_x` | gathered target x | spatial | (diagnostic) |
| 29 | `target_y` | gathered target y | spatial | (diagnostic) |
| 30 | `truncated` | 1 if the query budget ended the scan early (4.1(c)) | spatial | (diagnostic, metrics) |
| 31 | `proj_active` | 0/1 projectile in flight | combat | combat |
| 32 | `proj_left` | ticks until arrival (0 when inactive) | combat | combat |
| 33 | `proj_target` | target slot captured at fire | combat | combat |
| 34 | `proj_target_gen` | target generation captured at fire | combat | combat |
| 35 | `proj_damage` | damage carried | combat | combat |
| 36 | `dmg_out` | damage emitted this tick (0 if none) | combat | scatter |
| 37 | `dmg_target` | target slot for this tick's arrival, or NONE | combat | scatter |
| 38 | `dmg_target_gen` | generation for this tick's arrival | combat | scatter |
| 39 | `incoming` | saturating sum of damage received this tick | combat (store 0), scatter (saturating add) | damage |

Layout arithmetic: 39 fields x 4 bytes = 156 bytes per row per root. Memory accounting (9.3) uses this table.

Canonical state (M2-00 encoding v2) covers all 39 fields plus the entity table and event log. The schema descriptor hash (M2-01 section 6) covers the sorted `(id, type, name, default)` list. Field defaults are all 0 (M3 has no migration). Spawn requests therefore name every field whose intended value is not 0, and the scene generator writes `NONE` explicitly into `target_slot`, `dmg_target`, `proj_target` and `formation_slot` where NONE is intended.

### 2.1 Numeric profile `rts_u32_v1`
- Domain: all positions and orders in `[0, 32767]`. Violations are rejected by the control plane (`SU_E_TYPE`, path = field name). Host operators treat out-of-domain as `SU_E_INTERNAL`.
- Squared distance: `dx = |x1 - x2|`, `dy = |y1 - y2|` each <= 32767; `dx*dx + dy*dy <= 2*32767^2 = 2147352578 < 2^31`. No intermediate wraps.
- u32 modular identities used in formation: `(a + ox - BIAS) mod 2^32 == a + offset` exactly when the true result is in `[0, 2^32)`. Anchor positions are in-domain, so `a + offset` may be negative; the clamp (5.1) handles that through the 2^31 sign threshold.
- No division in IR. Host operators use integer shifts and compares only, plus an exact integer square root (Newton method; reference in the Python model).

## 3. Scheduler and phases

Per tick, after control-plane commands for the boundary have been applied (M2-00 section 2), the tick runs these phases in this order.

| idx | system id | kind | native-eligible | reads (summary) | writes (summary) |
|---|---|---|---|---|---|
| 0 | `rts.spatial` | host entry (OD-1b), 4 sub-steps (4.1) | no | alive, kind, team, hp, pos, formation_*, range_sq | 21-30 |
| 1 | `rts.goal` | IR 0.1 | yes | order_*, anchor_*, formation_o*, pos, hp, team (B1 only, M3-07) | 19, 20 |
| 2 | `rts.combat` | IR 0.1 | yes | target_valid, target_slot, target_gen, cooldown, reload, damage, flight, proj_* | 9, 31-39 (5.2; 39 is the store of 0) |
| 3 | `rts.move` | IR 0.1 | yes | pos, goal, speed, order_mode, target_valid | 4, 5 |
| 4 | `rts.scatter` | host entry (OD-1b), 4.3 | no | dmg_*, alive, generation, kind | 39 (of target rows) |
| 5 | `rts.damage` | IR 0.2 (despawn_self) | no (reference VM only) | hp, incoming | 3 (and despawn) |

Order rationale: spatial reads tick-start state; goal and combat read spatial outputs; move reads goal and `target_valid`; combat zeroes `incoming` (through its store of 0) before scatter adds; scatter precedes damage; deaths take effect at the boundary merge.

Semantics fixed by this order: all IR loads see tick-start values and each phase reads the state committed by the previous phase. Combat decides firing with the position from tick start; move then changes position. This is the documented M3 ordering, not a race.

Boundary (after phase 5): merge in canonical order `(system_index, slot, seq)`; apply `world.despawn` requests emitted by `rts.damage` (one `SU_EVENT_DESPAWNED` each); zero dead slots; publish. Tick abort cases follow M2-00 (`SU_E_RESOURCE_LIMIT` leaves nothing published).

Count: 6 systems <= `SU_SCHED_MAX_SYSTEMS` (8). Chunk parallelism is safe for IR 0.1 systems and damage (rows write only their own row). Spatial and scatter are serial in M3 (OD-14); with `workers > 1`, host entries run serially in their phase.

### 3.1 Host-entry kind (OD-1b, M3-04a)
Required change to `src/runtime/schedule.h`, additive:
- `SuSysEntry` gains `kind` (`SU_SYS_KIND_IR = 0`, the zero default, so every existing entry is unchanged; `SU_SYS_KIND_HOST = 1`) and `const SuHostOpDesc *host` (NULL for IR entries).
- `SuHostOpDesc` holds: `system_id`, `operator_version` (for `rts_ops_v1`, 1), `constants_hash` (sha256 of the operator constants, 4.4), the declared read set and write set as field ids with access, and `run(SuWorld *w, void *ctx)`.
- `mod` stays non-NULL. For a host entry it is a **declaration module**: built from the descriptor, with the field bindings and access, no instructions, and a `host` flag. It is never passed to the IR verifier or to native lowering. This keeps the 37 existing dereference sites valid and reuses the effect derivation (`su_effects.c`), the union field table (`su_sched_finalize`) and the manifest hash unchanged in code.
- `run_one_system` branches on `kind`. A host entry runs `run` with `su_world_build_invocation` (reads from working) and `su_world_build_outputs` (writes to staging), then `su_world_commit_staging` on `SU_OK`. The existing `su_world_zero_dead` call after every system (schedule.c:308, entity_tick.c:549) is the host commit step, so no new zeroing code is needed.
- `su_sched_check_candidate` refuses to replace a host entry with an IR candidate (`SU_E_CAPABILITY_DENIED`). `native_versions.c` refuses a native handle for a host entry.
- Rejected alternatives: (i) `mod == NULL` for host entries (37 sites in 5 files, and every site would need a branch); (ii) an IR no-op module with stores (fakes IR, and the verifier requires stores); (iii) running host operators outside the schedule (the tick loop would fork and the effect and manifest machinery would not apply).
- Leases: `src/runtime/schedule.h`, `src/runtime/schedule.c`, `src/runtime/native_versions.c` (refusal guard only), `src/runtime/rts/` (registration glue). Validation: targeted compile of the runtime, existing scheduler tests, and new host-entry tests (kind dispatch, declared-set check, refusal of candidates and native handles).

## 4. Host-side phase operators (`rts_ops_v1`)

Common rules: integer-only; read only frozen inputs (tick-start state for `rts.spatial`, pre-scatter state for scatter); allocation only at admission (no allocation in the tick, C11); buffers sized from capacity and the declared bounds below; every operator is reference C (`profiles/rts/native/`) and a Python model (section 8). Each operator has a fault seam (N9).

### 4.1 `rts.spatial` (four sub-steps, run in this order)

Let `cap` = capacity, `alive(s)` = slot `s` alive at tick start.

(a) **Anchor gather**: for each alive row `r` with `formation_slot != NONE`, `anchor_valid(r) = gather_valid(formation_slot, formation_gen)`; if valid, `anchor_x/y = pos` of that slot, else 0. `gather_valid(s, g)`: `s != NONE && s < cap && alive(s) && generation(s) == g`. Rows not valid write `anchor_valid = 0`, `anchor_x = anchor_y = 0`. Rows with `formation_slot == NONE` write `anchor_valid = 0` too. (The `alive(s)` term is redundant because despawn bumps the generation; see M3-M24.)

(b) **Grid build** (serial, global): cell `c(x,y) = (y >> 6) * 512 + (x >> 6)` for alive rows with `kind == 1 && hp > 0`. Counting sort into `cell_start[262145]` and `cell_items[N_idx]`, items in ascending slot order within each cell. Memory: 262144 counts plus 262145 starts (u32), i.e. 2 MiB fixed, plus 4 bytes per indexed row. Built once per tick.

(c) **Target query** (per alive row `r` with `kind == 1 && range_sq > 0`; others write `target_slot = NONE, target_gen = 0, target_dist_sq = 0, target_valid = 0, truncated = 0`):
1. `R = isqrt(range_sq)` (floor). Cell radius `rc = (R + 63) >> 6`. Since `range_sq <= 262144` is validated (`SU_E_TYPE` at the control plane, N5), `R <= 512` and `rc <= 8`. The operator treats `rc > 8` as `SU_E_INTERNAL`.
2. **Ring order**: ring `d` (d = 0..rc) is the set of cells `(cy', cx')` with `max(|cy' - cy|, |cx' - cx|) = d`, clipped to the grid `[0,511]^2`, where `cx = x >> 6`, `cy = y >> 6`. Ring 0 is the querier's own cell. Within a ring, cells are visited row-major (cy' ascending, then cx' ascending). Entries within a cell are visited in ascending slot order. At most 17 x 17 = 289 cells.
3. **Budget**: `K = 128` examined entries. Self and same-team entries count when examined. For each entry `e` in visit order: if `examined == K`, stop. Otherwise `examined += 1`; skip `e == r`; skip if `team(e) == team(r)`; `dist_sq = dx^2 + dy^2` (rts_u32_v1 domain); qualify iff `dist_sq <= range_sq(r)`; keep `best = min over qualifying (dist_sq, slot)` lexicographically (lower slot breaks ties).
4. **Truncation** (N3): `box_total` = number of indexed entries (self included) in all cells of the ring-box (rings 0..rc). `truncated = 1` iff `box_total > K`, otherwise 0. This is a pure function of the cell counts, and equals "the scan ended at the budget with an unexamined entry remaining" (a (K+1)-th candidate exists in the box).
5. Result: `target_slot = best.slot or NONE`, `target_gen = generation(best.slot)`, `target_dist_sq = best.dist_sq or 0`, `target_valid = (best exists)`.
6. Work bound per query: at most 289 cell visits and 128 entry examinations plus constant work. Per tick worst case is `N_armed * (289 + 128)` basic steps.

(d) **Target gather**: `target_x/y` = pos of `target_slot` when `target_valid` (else 0).

Note: `target_valid` implies `dist_sq <= range_sq`; combat need not re-check range.

### 4.2 Truncation and approximation policy (F2)
`target_slot` is the best qualifying enemy among the examined entries in ring order. It is deterministic. It is **not** guaranteed to be the nearest qualifying enemy when `truncated = 1`, and the spec does not call it "nearest" for any query that can truncate. The following are observations reported per scene (section 10.6):
- `truncated` fraction;
- `missed` rate: queriers for which the exact oracle has a qualifying enemy but `target_valid = 0`;
- `valid_but_not_nearest` rate: queriers with `target_valid = 1` whose target is not the exact `(dist_sq, slot)` minimum;
- distance ratio `sqrt(target_dist_sq) / sqrt(oracle_dist_sq)` over the not-nearest cases (p50, p95).

Gates (M3-06, applied to the committed `congested.blob.v1` at 1,024 and 16,384 rows): `missed <= 0.1%`, `valid_but_not_nearest <= 2%`, ratio p95 `<= 3.0`. A gate failure is an owner decision on K (OD-6); the cell size is not changed silently. Probe 1 (section R, non-generator scene, 600 queriers) gave 16k: missed 0, not-nearest 9/600 (1.5%), ratio p95 2.51 at 64-unit cells with K=128.

### 4.3 `rts.scatter` (`scatter_sat_add_u32`)
For each alive row `r` in ascending slot order with `dmg_target(r) != NONE` and `dmg_out(r) != 0`:
- Let `t = dmg_target(r)`. Apply iff `t < cap && alive(t) && generation(t) == dmg_target_gen(r) && kind(t) == 1`.
- Then `incoming(t) = sat_add(incoming(t), dmg_out(r))`, where `sat_add(a, b) = (a + b < a) ? 0xFFFFFFFF : a + b` in u32 (F3).
- Writes only field 39 of target rows. Reads: fields 36-38, `alive`, `generation`, `kind`.
- Determinism: saturating add of unsigned values is commutative and associative, because the saturated sum of any multiset of inputs is `min(sum, 0xFFFFFFFF)`. The result is therefore independent of order, and the canonical row order is kept for any future non-commutative combine.
- Lethality: a saturated `incoming` remains `>= hp`, so the damage rule (5.4) still kills exactly once.
- Serial in M3 (OD-14). A later parallel version uses per-chunk partial sums merged in canonical chunk order with the same saturating combine.

### 4.4 Declared effects (C04 gap, owner decision OD-5) and versioning (N8)
Host operators are not verified from IR. Each operator's descriptor (3.1) declares its read set, write set and `operator_version`. An instrumented debug build records every field store per phase and asserts the store set equals the declaration (M3-04 test). Native or release builds skip the instrumentation; the claim is then "declared and tested", not "verified".

Constants of `rts_ops_v1` are native gameplay semantics: `K = 128`, cell size 64, grid 512, tie-break rule, saturating add, the `alive`/`generation` gate. Changing any of them is a native build plus a new `operator_version`. The sha256 of the constants and version is `operator_manifest_hash`, recorded in the replay header (9.1). Replay fails closed on mismatch, as M2-01 section 6 does for schema.

## 5. IR systems (IR 0.1 unless noted)

All systems below are row-local and use only `load_u32`, `const_u32`, `add/sub/mul_u32`, `eq/lt_u32`, `select_u32`, `store_u32` (IR 0.1) plus `despawn_self` (IR 0.2, damage). Each declared writable field is stored exactly once per row (verifier rule). Pseudocode uses bool names; each bool is produced by `eq` or `lt`, and each flag is converted with `eq(flag, 1)` (N2). `select(b, x, y)` requires `b` to be a bool.

### 5.1 `rts.goal` (IR 0.1)
Reads: `order_mode, order_x, order_y, anchor_valid, anchor_x, anchor_y, formation_ox, formation_oy, pos_x, pos_y`. Writes: `goal_x, goal_y`.

```
has_order_flag = select(eq(order_mode, 0), 0, 1)           (u32 0/1)
has_order      = eq(has_order_flag, 1)
av             = eq(anchor_valid, 1)
raw_x      = select(has_order, order_x, select(av, anchor_x + formation_ox - BIAS, pos_x))
raw_y      = same with y fields
goal_x     = clamp_domain(raw_x)
clamp_domain(v) = select(lt(2147483647, v), 0, select(lt(32767, v), 32767, v))
```
Priority: an explicit order (mode 1 or 2) wins over formation membership; a formation member with no order follows its anchor; otherwise the goal is the current position (hold). `lt(2147483647, v)` detects wrapped negatives because `|true value| < 2^31`.

### 5.2 `rts.combat` (IR 0.1)
Reads: `target_valid, target_slot, target_gen, cooldown, reload, damage, flight, proj_active, proj_left, proj_target, proj_target_gen, proj_damage`. Writes: `cooldown, proj_active, proj_left, proj_target, proj_target_gen, proj_damage, dmg_out, dmg_target, dmg_target_gen, incoming`.

```
tv        = eq(target_valid, 1)
cd0       = eq(cooldown, 0)
pa0       = eq(proj_active, 0)
fire_flag = select(tv, select(cd0, select(pa0, 1, 0), 0), 0)     (u32 0/1)
fire      = eq(fire_flag, 1)
arrive_flag = select(eq(proj_active, 1), select(eq(proj_left, 1), 1, 0), 0)
arrive      = eq(arrive_flag, 1)

cooldown'        = select(fire, reload - 1, select(lt(0, cooldown), cooldown - 1, 0))
proj_active'     = select(fire, 1, select(arrive, 0, proj_active))
proj_left'       = select(fire, flight, select(arrive, 0, select(lt(0, proj_left), proj_left - 1, 0)))
proj_target'     = select(fire, target_slot, select(arrive, NONE, proj_target))
proj_target_gen' = select(fire, target_gen, select(arrive, 0, proj_target_gen))
proj_damage'    = select(fire, damage, select(arrive, 0, proj_damage))
dmg_out'         = select(arrive, proj_damage, 0)
dmg_target'      = select(arrive, proj_target, NONE)
dmg_target_gen'  = select(arrive, proj_target_gen, 0)
incoming'        = 0
```
Reload (N1): `reload >= 1` is validated, so `reload - 1` does not wrap. Derivation: fire at tick `t` sets `cooldown' = reload - 1` at the end of `t`. Each later tick decrements it until 0, so `cooldown = 0` at the start of tick `t + reload` and the earliest re-fire is `t + reload`. Projectile: fire sets `proj_left = flight`; arrival happens in tick `t + flight` (the tick whose start value of `proj_left` is 1), and `proj_active` clears at that tick's end, so the earliest next fire is `t + flight + 1`. Shot period is therefore `max(reload, flight + 1)`, as in 5.2's prose. Known answers (10.1): reload 6, flight 2 gives re-fire at `t + 6`; reload 1, flight 1 gives fires at `t`, `t + 2`, `t + 4`; reload 2, flight 5 gives re-fire at `t + 6`.

`incoming' = 0` is the zeroing that makes scatter additive per tick.

### 5.3 `rts.move` (IR 0.1)
Reads: `goal_x, goal_y, pos_x, pos_y, speed, order_mode, target_valid`. Writes: `pos_x, pos_y`.

```
mode_move = eq(order_mode, 1)
has_tgt   = eq(target_valid, 1)
mv_flag   = select(mode_move, 1, select(has_tgt, 0, 1))      (u32 0/1)
moving    = eq(mv_flag, 1)
right     = lt(pos_x, goal_x);  left = lt(goal_x, pos_x)
ax        = select(right, goal_x - pos_x, pos_x - goal_x)    (abs difference, 0 if equal)
m         = select(lt(ax, speed), ax, speed)                  (min)
step_x    = select(right, pos_x + m, select(left, pos_x - m, pos_x))
pos_x'    = select(moving, step_x, pos_x)
(same for y)
```
Mode 1 (move) ignores enemies. Mode 2 (attack-move) and mode 0 stop when a target is valid, so engagement is "stop and fight"; anchors have no weapon and never stop. Position stays in domain because it moves monotonically toward an in-domain goal.

### 5.4 `rts.damage` (IR 0.2, `despawn_self`)
Reads: `hp, incoming`. Writes: `hp`. Emits: `despawn_self` (cond).

```
m        = select(lt(incoming, hp), incoming, hp)          (min)
hp'      = hp - m                                           (saturating by construction)
kill_flg = select(eq(hp', 0), select(lt(0, hp), 1, 0), 0)   (hp was > 0, now 0)
despawn_self if eq(kill_flg, 1)
```
Anchors have `hp = 0` and `incoming = 0`, so they never die by damage. A saturated `incoming` (4.3) still gives the correct kill. The reference VM runs this system only (native refuses 0.2, M2 rule).

### 5.5 Capsule record (N15, C04)
Each IR capsule in `capsules/rts/` declares, using `schemas/capsule.schema.json`:
- identity: `system_id`, `system_rev`, `ir_version` (0.1 or 0.2);
- schema dependencies: `schema_id`, `schema_version`, `layout_generation`, field table digest;
- field reads and writes exactly as in section 2 (per-system lists in 5.1 to 5.4);
- ordering edges: spatial before goal and combat; goal and combat before move; scatter before damage;
- numeric profile `rts_u32_v1`;
- capabilities: none, except `rts.damage` (`despawn_self`, IR 0.2);
- resource bounds: `max_instructions` 4096, `max_registers` 4096, and for `rts.damage` `max_struct_per_row = 1`, `max_events_per_row = 0`;
- test obligations: the T-RTS ids of section 10 that exercise the system.
If `capsule.schema.json` lacks any of these keys, adding them is an owner-approved schema change (OD-16). M3-03 does not change it otherwise.

## 6. IR expressiveness: why no new opcode is needed (D1)

### 6.1 Realization rules
- Boolean composition is done by nested selection on values, never by materialized bool AND/OR. Example: `fire = AND(tv, cd0, pa0)` is `fire_flag = select(tv, select(cd0, select(pa0,1,0), 0), 0)` then `fire = eq(fire_flag, 1)`.
- A bool from a u32 flag is `eq_u32(flag, 1)`. A flag from a bool is `select_u32(b, 1, 0)`. A bool used as a condition must be a bool (`eq` or `lt` result), never a raw u32 (N2).
- `min(a,b) = select(lt(a,b), a, b)`. Abs difference: `select(lt(a,b), b-a, a-b)`. Clamp: 5.1.
- `le(a,b) = not lt(b,a)` via `select(lt(b,a), 0, 1)`.
- All constants are `const_u32` registers. A const 0 for `lt(0, x)` uses one register.
- Each pseudocode line in section 5 is type-checked by the M3-02 IR-evaluator generator before freezing (N2).

### 6.2 Instruction budget
Estimates (to be replaced by measured counts in M3-03): `rts.goal` ~ 60 instructions, `rts.combat` ~ 110, `rts.move` ~ 50, `rts.damage` ~ 25. All far below `SU_LIMIT_INSTRUCTIONS` (4096) and `SU_LIMIT_REGISTERS` (4096). Registers are per row and freshly defined; no register survives a row.

### 6.3 What is not expressible in IR and why host operators are required
- Indexed read (`gather`): `load_u32` reads only the current row. Formation anchors and targets need another row's data.
- Indexed write (`scatter`): a row writing another entity's field is cross-entity addressing, forbidden by LIVE_IR_SPEC section 1.
- Neighbour search: needs iteration over a variable-length bucket. A K-unrolled IR version would need about 128 candidate steps of about 20 instructions each per system plus cross-row gathers that IR cannot express, so it is not viable.

### 6.4 Re-evaluation trigger
If M3-03 measures any system above 2048 instructions, or the M3-07 agent experiment shows more than a fixed rate of verifier rejections caused by bool-flag encoding, the owner reviews a narrow IR 0.3 proposal: `bool_and`, `bool_or`, `bool_not`, `min_u32`. Indexed gather and scatter remain host operators regardless.

## 7. Scene generators (seeded, deterministic, committed)

### 7.1 PRNG (`splitmix64_v1`)
State is u64. `next()`: `state = state + 0x9E3779B97F4A7C15 (mod 2^64)`; `z = state`; `z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9 (mod 2^64)`; `z = (z ^ (z >> 27)) * 0x94D049BB133111EB (mod 2^64)`; `z = z ^ (z >> 31)`; `u32 = z >> 32`. `uniform(n) = (u32 * n) >> 32` (multiply-high). Its bias is below `n / 2^32`; the committed lock (7.5) fixes the exact outputs, so the digest test is unchanged (N16). Seeds are u64.

### 7.2 Canonical scene encoding (digest input)
`rts-scene-canon/0.1`: line 1 is canonical JSON (keys sorted, no whitespace) of `{ "format":"rts-scene-canon/0.1", "generator":<id>, "params":<object>, "seed":<decimal>, "schema_digest":"sha256:<hex>", "capacity":N, "rows":N }`. Then for each slot `0..N-1` the 39 field values as u32 little-endian (156 bytes per row, in the field-id order of section 2). The scene digest is SHA-256 of these bytes. The digest is computed by the Python generator only (D10); a second implementation is not required.

### 7.3 Generators and scenes

| id | generator | purpose | N (committed) |
|---|---|---|---|
| `duel.fixture.v1` | hand-specified rows (no PRNG) | edge fixtures with exact expected outcomes (10.2) | 8 to 64 |
| `mixed.battle.v1` | seeded; two bands on x; groups of 17 rows (1 anchor + 16 members); 3 weapon kinds (7.4) | representative mixed scene | 1,024; 16,384 |
| `congested.blob.v1` | seeded; all units inside a 2048 x 2048 box in the middle, two teams interleaved, formations | congestion, truncation, grid density, targeting gates (4.2) | 1,024; 16,384 |
| `sparse.field.v1` | seeded; uniform positions over the domain, sparse teams | spatial baseline, few queries hit | 16,384; 65,536 |

Slot order: for each group `g` in index order, the anchor slot then its 16 members in member index order; remainder rows (`N mod 17` after the groups; for N = 1,024 the remainder is 4, for 16,384 it is 13) are free units at the end, with `formation_slot = NONE` and their own attack-move order. Initial generations are 1.

Formation offsets: member `i` in a group has column `i mod 4` and row `i div 4`; `offset_x = col*32 - 48`, `offset_y = row*32 - 48`, stored as `formation_ox = offset + BIAS`.

Orders (N7): only the anchor receives the group order (mode 2, attack-move to the opposite band centre or the box centre). Members have `order_mode = 0` and follow anchor plus offset until a target is valid. Acceptance check: at tick 0, before any tick runs, each member with `anchor_valid = 1` has `goal = anchor + offset`, so formation-follow is exercised in every committed scene.

### 7.4 Weapon table (synthetic, not BFME)

| kind | range_sq | reload | flight | damage | speed | hp |
|---|---|---|---|---|---|---|
| rifle | 65536 (256) | 6 | 2 | 3 | 96 | 12 |
| bow | 262144 (512) | 10 | 5 | 5 | 64 | 10 |
| melee | 256 (16) | 2 | 1 | 2 | 128 | 20 |

Team placement in `mixed.battle.v1`: team 0 x in `[1024, 8191]`, team 1 x in `[24576, 31743]`, y uniform in `[0, 32767]`. Congested: positions uniform in the central 2048 x 2048 box, orders attack-move to the box centre.

### 7.5 Committed lock and load path (F8)
- `bench/rts/scenes.lock.json` lists per scene: generator id, `params`, `seed`, `capacity`, the scene digest, and the schema digest. Regeneration must reproduce every digest; a mismatch fails the lock check (`bench/rts/gen/check_lock.py`, M3-01).
- Scenes up to 65,536 rows may be generated in memory by the Python generator and loaded in the test harness. Scenes larger than that are generated to a local, uncommitted row file (never committed; only spec and digest are committed) and loaded with `world.spawn_batch` in calls of at most 1,024 rows (OD-3b). The M3-05 harness refuses a load whose accepted row count differs from the generator's.
- Load-check: for N up to the snapshot cap (9.3), the loaded state is read back through a snapshot capture and its columns are hashed with the 7.2 canonical encoding; the check must equal the lock digest (T-RTS-SCENE-2). Above the snapshot cap no full read-back path is specified in M3; the record says `scene_check: not_available` and reports the accepted row count and the per-call count only. Adding a bulk read path is OD-12.

## 8. Independent Python reference model

- Location: `bench/rts/ref/` (`model.py`, `ir_eval.py`, `isqrt.py`); tests in `tests/rts/ref/`. Python 3 standard library only; no import from the engine or from `profiles/rts/native/`.
- Written from this contract only. The author of `model.py` must not read the C operator sources (M3-04) before the model's golden outputs are fixed. A different agent or person reviews the model against the contract text (OD-13).
- Two parts:
  1. **Semantic model** of phases 0-5 and merge, written from sections 4 and 5 (not from the IR JSON). It is the oracle for scene digests and for targeting metrics (4.2).
  2. **IR evaluator** (`ir_eval.py`): interprets only the committed IR 0.1 and 0.2 JSON with the M0/M2 opcode semantics. A differential test asserts that for random rows the evaluator output equals the semantic model's per-row outputs.
- Outputs per tick: per-phase field digests and the canonical state digest, using the same canonical layout as the engine (9.2).
- Oracle budget (N13, estimate, not measured): in CI, scenes up to 4,096 rows and up to 120 ticks. Larger runs record `oracle: not_run` with `reason: budget`. M3-02 measures the budget on the reference machine and fixes the numbers before M3-06.
- The exact brute-force oracle for targeting metrics (4.2) is part of this model; it scans all enemies, not the grid.

## 9. Replay format, digests and memory accounting

### 9.1 Replay file `rts-replay/0.1` (JSON Lines)
- Line 1, header: `{"format":"rts-replay","version":"0.1","numeric_profile":"rts_u32_v1","scene":{"generator","params","seed","capacity","scene_digest"},"schema_digest","system_manifest_hash","operator_manifest_hash","engine":{"artifact_hashes":[...],"source_revision"},"caps":{"max_struct_per_tick","max_events_per_tick","event_log_max"},"checkpoint_every":32,"digest_every":K,"ticks":T}`.
- `operator_manifest_hash` covers the `rts_ops_v1` versions and constants (4.4); `system_manifest_hash` covers the IR systems only.
- Caps are recorded as configured: `max_struct_per_tick = max_events_per_tick = capacity` for RTS worlds (D11).
- Initial scene (N12): the scene is regenerated from `generator`, `params` and `seed`, and the loaded state must match `scene_digest`. A mismatch fails closed. Spawn records are not embedded in the replay.
- Command records: `{"t":<tick>,"seq":<n>,"cmd":"world.set_fields","args":{...}}`, `"cmd":"world.spawn_batch"`, `"cmd":"world.despawn"`. `t` is the boundary tick at which the command is applied (before that tick runs). Within a boundary, commands are applied in `seq` order.
- Digest records: `{"t":<tick>,"state":"sha256:...","events":"sha256:...","counts":{"alive","retired","events","dropped","truncated_rows"}}` every `digest_every` ticks and on the final tick.
- Replay validation: re-run from the header and the commands; compare each digest record; on mismatch report the first differing tick, the first differing `(field_id, slot)`, and the expected and actual values. Exit status is nonzero on any mismatch or missing record.
- Replay across tick-worker counts 1 and 2 (`SUTEKH_TICK_WORKERS`) must give the same digests; this tests IR 0.2 and host operator determinism, not performance.
- Recovery replay (N9): replay by re-execution. Operators are pure functions of tick-start state and the schedule; the spatial grid is rebuilt every tick and is not state; scatter applies from `incoming`, which is canonical state. A checkpoint therefore needs no operator state, provided `operator_manifest_hash` matches.

### 9.2 Canonical digests
State digest is the M2 recovery digest (`SHA-256(canonical_state || layout_generation_u32_le || schema_descriptor_sha256)`) plus the 0.2 canonical state: rows `0..high_water-1`, entity table, retired count, event counters including `event_next_seq` and `dropped`. Event digest is SHA-256 over canonical JSON lines of the **retained** event records in `event_seq` order. Canonical JSON uses sorted keys, no whitespace, decimal integers.

Ring eviction (F6): the event digest covers only retained events after the bounded log has evicted older ones. Evicted events are not in the event digest, but their count is in `dropped`, and `dropped` and `event_next_seq` are part of the canonical state digest. Two runs that differ only in evicted events therefore differ in the state digest.

### 9.3 Memory accounting (estimates until OD-10, not targets)
Accounted categories, computed from capacity and configured limits, reported in bytes:

| category | formula | estimate |
|---|---|---|
| columns | `capacity * 39 * 4 * 2` (committed and working roots) plus staging `capacity * 4 * 10` (combat writes 10 fields) | 352 bytes per row |
| entity table | `capacity * 12` (generation, alive, external id); exact layout from M2-00 | 12 bytes per row |
| spatial grid | `262144 * 4` counts + `262145 * 4` starts + `N_idx * 4` items | 2 MiB fixed + 4 bytes per indexed row |
| structural and event buffers | `min(chunk_size * per_row, per_tick)` per (system, chunk), admitted at create (su_cmdq.h) | reported at admission; expected about 32 bytes per row for `rts.damage` only |
| event log | `event_log_max * 40` | 40 bytes per retained event |
| checkpoint spool | canonical snapshot bytes (9.3 note) for each retained checkpoint | about 175 bytes per row representative, 446 worst case, plus about 90 bytes per retained event (probe 2) |

Note on the spool: the bound is the retained-checkpoint count times the snapshot bytes; M3 uses `retain_limit = 1`, and the create path must hold the new checkpoint while the old is retained (confirm the peak against `checkpoint.h` before M3-05).

Cross-check: process `VmHWM` (from `/proc/self/status`) sampled at the end of each run and the allocator-level count where available. Any category that cannot be measured is `null` with a reason (C10). Exact per-root accounting is OD-10 (M3-08a). Until it lands, the harness reports the analytic figures and marks them as estimates. Total analytic estimate: about 440 bytes per row plus 2 MiB fixed, before the event log and spool.

## 10. Tests and acceptance

### 10.1 Reference-behaviour tests (per system, known answers from the Python model)
- `T-RTS-GOAL-*`: explicit order overrides formation; formation member follows anchor with offset; invalid anchor holds; clamp of wrapped negative and over-domain values to 0 and 32767.
- `T-RTS-MOVE-*`: arrival exactly at goal; step equals `min(distance, speed)` on each axis; mode 1 ignores target; mode 2 stops when `target_valid`; speed 0 never moves; no overshoot.
- `T-RTS-COMBAT-*`: fire at cooldown 0 with target; no fire with active projectile; arrival at `t + flight` exactly (flight 1 and flight 5 edges); re-fire period `max(reload, flight + 1)` with the three known answers of 5.2 (reload 6 flight 2; reload 1 flight 1; reload 2 flight 5); cooldown never underflows; `incoming` zeroed each tick.
- `T-RTS-DAMAGE-*`: partial damage; exact kill with `incoming == hp` (kill emitted once); overkill saturates; anchors never killed; kill emits exactly one `SU_EVENT_DESPAWNED` at the boundary.
- `T-RTS-SPATIAL-*`: nearest-enemy by `(dist_sq, slot)` with ties for queries whose budget is not exhausted (`truncated = 0`); dist exactly equal to `range_sq` qualifies, `range_sq + 1` does not; cell boundary at x = 63/64 and y = 63/64 and 255/256; box clipped at domain edges (0 and 32767); self excluded; same-team excluded; dead excluded; truncation at exactly K = 128 examined and at K + 1 with the flag (box_total = 128 gives 0; box_total = 129 gives 1); a generation mismatch reported as invalid gather; ring order checked by a fixture whose nearest enemy is in ring 1 and whose ring-0 cell holds 200 friendly entries (the query must still find the ring-1 enemy within budget or report `truncated`).
- `T-RTS-GATHER-*`: stale anchor (generation mismatch, despawned, out of range, NONE) yields `anchor_valid = 0`; zero-value legitimate positions are distinguishable through the flag.
- `T-RTS-SCATTER-*`: saturation at the operator level: two arrivals of `0xFFFFFFF0` leave `incoming = 0xFFFFFFFF` (wrapping add would give `0xFFFFFFE0`); saturated lethal case kills exactly once; dead target ignored; stale generation ignored; non-unit target ignored; shooter despawned before arrival causes no damage; multiple shooters on one target summed.

### 10.2 Edge fixtures (`duel.fixture.v1`, exact expected outcomes from the model)
A minimal set: two units at distance exactly `range_sq`; one unit at x=0 and one at x=32767; equal-distance candidates (lower slot wins); a bow with `flight = 5` against a target that dies in flight; a target with `hp = 3` hit by two shooters in one tick (killed once, incoming 6); a formation whose anchor is despawned mid-run; slot reuse after despawn with a stale projectile target (the stale shot must not hit the new occupant).

### 10.3 Scene, load, control and replay tests
- `T-RTS-SCENE-1`: generator run twice gives identical bytes and digests; digest changes when any single PRNG constant or one field value changes (mutation).
- `T-RTS-SCENE-2`: for each committed scene up to the snapshot cap, the engine-loaded state, read back through a snapshot and hashed with the 7.2 encoding, equals the lock digest.
- `T-RTS-LOAD-1` (F4, F7): a 1,024-row all-worst-case `world.spawn_batch` is accepted; the same call padded to 1,048,577 bytes is refused with `SU_E_RESOURCE_LIMIT` and no state change; a 1,000,000-row load is 977 calls; the event count matches the M2 spawn rule (one `SU_EVENT_SPAWNED` per row, with `dropped` for rows beyond `event_log_max`).
- `T-RTS-CTRL-1` (F7, N11): `set_fields` with 1,024 worst-case entities is accepted and one revision bump results; a stale handle among the entities refuses the whole call; a duplicate entity refuses with `SU_E_INVALID_ARGUMENT`; writes to `kind` or `team` refuse with `SU_E_TYPE`; `kind = 1` with `hp = 0` refuses with `SU_E_TYPE` in `spawn`, `spawn_batch` and `set_fields`; a stale `expected_world_revision` refuses with `SU_E_STALE_REVISION` (M3-M19).
- `T-RTS-REPLAY-1`: record then replay gives identical digest records; replay with tick workers 1 and 2 identical.
- `T-RTS-REPLAY-2`: a single flipped command byte, or a dropped `world.set_fields`, reports the first divergent tick; a changed `operator_manifest_hash` fails closed.
- `T-RTS-REPLAY-3` (N9): kill the worker mid-run, recover from checkpoint plus log in a replacement worker, continue, and compare digests with an uninterrupted run; recovery replays at most `checkpoint_every` = 32 ticks. Fault seams inside `rts.spatial` and `rts.scatter` return `SU_E_INTERNAL`, and each is recovered the same way (AGENTS.md: in-process native faults require checkpoint recovery, not pointer rollback).
- `T-RTS-CHURN-1`: spawn and despawn churn of at least 100,000 operations on a 16k scene with reference model comparison (inherits M2 churn test on RTS fields).
- `T-RTS-CAPS-1` (F6): with an explicitly smaller configured `max_struct_per_tick`, deaths in one tick above it abort the tick with `SU_E_RESOURCE_LIMIT`, state unchanged (M2 rule), and the replay records the configured cap. With the RTS setting (`= capacity`), a tick where every alive unit dies completes.

### 10.4 Verifier and IR negatives (M3-03)
Each code path: missing store (`SU_E_MISSING_STORE`), undeclared write (`SU_E_UNDECLARED_WRITE`), undeclared read (`SU_E_UNDECLARED_READ`), 0.2 op in a 0.1 module (`SU_E_UNKNOWN_OPCODE`), `despawn_self` count above `max_struct_per_row` (`SU_E_RESOURCE_LIMIT`), `emit_event` absent, `ir_version` mismatch (`SU_E_SCHEMA_MISMATCH`), duplicate register (`SU_E_DUPLICATE_REGISTER`), undefined register (`SU_E_UNDEFINED_REGISTER`), native lowering refused for 0.2 (`SU_E_CAPABILITY_DENIED`), and a non-bool condition in `select` (a raw u32 used as the selector; N2).

### 10.5 Agent-authored behaviours (M3-07)
Section 11.

### 10.6 Performance, stress and targeting metrics (M3-06, M3-08)
- Targeting metrics (4.2) are reported for every congested scene at 1,024 and 16,384 rows over 1,024 seeded queriers, with the exact oracle. Gates: `missed <= 0.1%`, `valid_but_not_nearest <= 2%`, ratio p95 `<= 3.0`.
- No timing targets are asserted. Tests check that output files exist, are schema-valid, and carry hardware, OS, toolchain, source and artifact hashes (C10).

## 11. Agent-authored behaviours evidence plan (M3-07)

Three behaviour tasks, each to be done by an agent through the existing candidate pipeline (verifier, reference tests, `world.candidate` path; AGENT_PROTOCOL), with hidden acceptance fixtures written by a different author from the Python model. All three are IR 0.1 changes to `rts.goal`, `rts.combat` and `rts.move`, and each fixture fails on the r2 baseline (F9):

- **B1 low-hp retreat**: a unit with `hp <= 3` retreats to its team's base point and does not fire. Write scope: `rts.goal` (goal toward base), `rts.combat` (no fire gate on `hp > 3`), `rts.move` (moving when retreating even with `target_valid`). Extra declared reads: `goal` reads `hp` and `team`; `combat` reads `hp`; `move` reads `hp`. Base points are constants per team (team 0 at x = 2048, team 1 at x = 30720, y = 16384). Retreat overrides explicit orders. Baseline fixture: team 0 unit, `hp = 3`, at (4000, 16384), rifle; team 1 unit, `hp = 12`, at (4200, 16384). Baseline: the unit does not move (valid target) and fires. Expected: it moves 96 units toward x = 2048 and does not fire.
- **B2 kiting**: a unit with `cooldown > 0` and a valid target keeps moving toward its goal instead of stopping. Write scope: `rts.move` (stop gate becomes "target valid and cooldown = 0"); extra declared read: `move` reads `cooldown`. Baseline fixture: rifle unit with `cooldown = 4`, a valid target at distance 200, mode 2 order 2000 units away. Baseline: no movement. Expected: moves `speed` (96) per tick. The old r1 projectile retarget guard is removed: scatter already rejects a target whose generation changed (4.3, T-RTS-SCATTER), so it is base behaviour and has no failing baseline.
- **B3 formation catch-up**: a formation member (`anchor_valid = 1`, mode 0) farther than 64 units from its goal moves at twice `speed`, capped at the remaining distance. Write scope: `rts.move`; extra declared read: `move` reads `anchor_valid`. Baseline fixture: member at distance 200 from goal, speed 96. Baseline: moves 96. Expected: moves 192. The r1 "formation hold" rule is removed: the r1 rules already hold when `anchor_valid = 0` and no order (`goal = pos`), so it had no failing baseline.

Evidence record per task (JSON, stored in `bench/rts/agent_experiments/records/`): task id, agent and session identifier, model identifier, number of candidate submissions, verifier rejections by code, reference test failures by fixture id, hidden fixture result, wall time to accept (or failure), native-lane status (not applicable, as all three remain IR 0.1), null tokens and tool calls with reason `not_recorded` if the harness did not record them. Acceptance: the candidate passes hidden fixtures and the full existing suite. The record is not a claim of general agent capability. Behaviour set change is OD-19.

## 12. Stress curves and memory methodology (M3-08)

- Harness: `bench/rts/stress/`. Sweep scene sizes over a geometric series of powers of two starting at 1,024 (sweep points are a methodology choice, not a unit-count claim). For each size, run `T` ticks (default 120) of `mixed.battle.v1` and of `congested.blob.v1`.
- Stop rule per curve: stop at the first failing limit, which is recorded by name:
  - `capacity` (`SU_LIMIT_ROWS`, default 1,000,000; RTS capacity must also be <= 1,048,576, F6);
  - `tick_cap` (`SU_E_RESOURCE_LIMIT` from `max_struct_per_tick` or `max_events_per_tick`; by construction this should never fire for RTS worlds, so any occurrence is a finding and a bug report, not a memory limit);
  - `operator_buffer` (`SU_E_RESOURCE_LIMIT` from an operator's admitted buffers);
  - `snapshot_cap` (`SU_E_RESOURCE_LIMIT` from capture: 64 MiB output or the 838,860-row guard, F5);
  - `spool_budget` (`SU_E_RESOURCE_LIMIT` from the checkpoint spool, `spool_bytes_max`, F5);
  - `memory_ceiling` (operator-set);
  - `wall_clock_ceiling` (operator-set, per run).
  Record the largest size completed and the first failing size with its named limit and code. No size is predeclared as solved.
- Checkpointing during the sweep: a checkpoint every `checkpoint_every` = 32 ticks, so the spool and snapshot limits are exercised within a 120-tick run.
- Per tick, record per-phase monotonic nanoseconds (spatial sub-steps a-d, goal, combat, move, scatter, damage, merge, digest when taken). Per run: p50, p95, p99 and max over ticks, with sample counts. Also record the truncation fraction, the targeting metrics of 10.6, deaths per tick, projectiles per tick, and the configured `max_struct_per_tick` and `max_events_per_tick`.
- Hardware and environment block: CPU model, vCPU count, memory, OS, compiler versions, build flags, `SUTEKH_TICK_WORKERS`, artifact hashes. Measurement mode `dev_checked` or `perf_trusted` is stated explicitly. Shared VM timings are observations (C10).
- Memory: section 9.3 analytic figures plus `VmHWM` per run; differences between the two are reported, not hidden.
- Recovery claim (F5): checkpoint recovery is claimed for N <= 65,536 only, with the binary spool default (512 MiB) or with a library spool that holds the measured checkpoint. N = 262,144 is claimed only if the measured canonical bytes per row (including the event log) give a checkpoint of at most half the spool, and at most 64 MiB. Above 838,860 rows the row guard refuses capture; 1M is not claimed. A larger claim needs OD-15.
- Actual limits found are recorded as data: configured `SU_LIMIT_ROWS`, capacity used, per-tick caps, and the largest completed size.

## 13. Interface changes and owner decisions

| id | item | change class | owner |
|---|---|---|---|
| OD-1 | Host-side operators (`rts_ops_v1`) instead of IR gather/scatter (D2) | design | orchestrator plus M3 owner |
| OD-1b | Additive host-entry kind in `schedule.h` (3.1): `kind`, `SuHostOpDesc`, declaration module, dispatch branch, candidate and native refusal | shared contract (runtime header) | scheduler/runtime owner |
| OD-2 | No IR opcode additions in M3 (D1); IR 0.3 proposal only if 6.4 triggers | design | IR owner |
| OD-3 | `world.set_fields {world_id, expected_world_revision, entities:[{entity, values}]}`, at most 1,024 entities per call, non-key fields only (`kind`, `team` rejected), duplicate or stale entity refuses the whole call, one revision check and one recovery-log entry per call, emits no event | shared contract (new control method) | ABI/control owner |
| OD-3b | `world.spawn_batch {world_id, expected_world_revision, rows:[{values, external_id?}]}`, at most 1,024 rows per call under the 1 MiB whole-request cap (`su_proto.c:322`), one recovery-log entry per call, slots in given order, one `SU_EVENT_SPAWNED` per row (M2 rule, `worker.c:1077`) | shared contract (new control method) | ABI/control owner |
| OD-3c | Whether control-plane events applied at a boundary count toward `max_events_per_tick`. Required rule: the control plane counts host events per boundary against the cap and refuses an over-cap call with `SU_E_RESOURCE_LIMIT` and no state change; the rule is recorded in the replay header. Not resolved by the code read (F6 sub-item) | shared contract (runtime) | runtime/control owner |
| OD-4 | `SU_SCHED_MAX_SYSTEMS` stays 8; M3 uses 6 | none | - |
| OD-5 | Declared-and-tested (not verified) effects for host operators (4.4) | design risk accepted | owner |
| OD-6 | Query grid 64-unit cells (512 x 512), K = 128, rc <= 8, ring order, `truncated = [box_total > K]`; gates of 4.2 | profile constant | RTS profile owner |
| OD-7 | No separation, avoidance, pathfinding or obstacle map in M3 | scope | owner |
| OD-8 | One projectile per shooter with fixed flight time | scope | owner |
| OD-9 | `rts_u32_v1` numeric profile and 15-bit domain | profile | owner |
| OD-10 | Exact per-root memory accounting API in `src/world` (for 9.3), M3-08a | native_core | ABI/world owner |
| OD-11 | Native lane for goal/combat/move (M3-10), gated on M1 owner acceptance | native_core, later | owner |
| OD-12 | Bulk scene read-back for N above the snapshot cap; spawn_batch is the only load path in M3 | scope, later | owner |
| OD-13 | Whether a Python model written by a different agent under the same orchestrator counts as independent | process | owner |
| OD-14 | Scatter and spatial remain serial in M3 | scope | owner |
| OD-15 | Snapshot limit: accept the N <= 65,536 recovery claim (F5), or raise `SU_SNAPSHOT_MAX_BYTES` or the row guard, or adopt a binary snapshot encoding (new format, not in this contract) | shared contract (`src/world/snapshot.h`) | snapshot owner |
| OD-16 | Capsule schema keys of 5.5, only if `schemas/capsule.schema.json` lacks them | shared schema | schema owner |
| OD-17 | Replay header 0.1 additions (`checkpoint_every`, `operator_manifest_hash`, `dropped` count) | file format | replay owner |
| OD-18 | `checkpoint_every` = 32 ticks as an RTS profile constant (N9) | profile constant | RTS profile owner |
| OD-19 | Redefinition of M3-07 behaviours B2 and B3, removal of r1 B2 and B3 (F9) | scope | owner |

Profile errors reuse existing codes: `SU_E_INVALID_ARGUMENT` (malformed scene or command, duplicate entity in a call), `SU_E_TYPE` (out-of-domain value, `kind = 0` on live row, `kind = 1` with `hp = 0`, immutable field write, `range_sq > 262144`, weapon out of table), `SU_E_RESOURCE_LIMIT` (capacity, per-tick caps, operator buffers, request size, snapshot and spool limits), `SU_E_SCHEMA_MISMATCH` (generator schema digest differs from loaded schema), `SU_E_STALE_HANDLE` and `SU_E_STALE_REVISION` (control commands), `SU_E_CAPABILITY_DENIED` (host entry replaced by a candidate or lowered to native), `SU_E_INTERNAL` (invariant violated inside an operator, including `rc > 8`). No new status codes are proposed; a new code would require a `status.h` change and owner approval.

## 14. Task split (proposed replacement for the single M3-00 entry)

Note for the orchestrator (not an edit to `planning/tasks.json`, which this contract does not change): the current `M3-00` entry is broad ("Build synthetic RTS microcosm"). Proposed: keep `M3-00` as this contract's freeze and split the rest as below. Every task is `status: planned` until the owner accepts this revision.

| id | title | depends_on | write scope (lease) | change class | native validation | acceptance (summary) |
|---|---|---|---|---|---|---|
| M3-00 | Contract r2 freeze (this file) | M2-01 (design only) | `planning/contracts/M3_RTS_CONTRACT.md` | design | no | owner accepts or rejects OD-1 to OD-19 |
| M3-01 | Schema, Python scene generator (single normative), scene lock | M3-00 | `profiles/rts/schema/`, `bench/rts/gen/`, `bench/rts/scenes.lock.json` | profile | no | generator reproducible; digests stable; PRNG and field-order mutants change digests; remainder rule (7.3) |
| M3-02 | Independent Python reference model, IR evaluator, targeting oracle | M3-00 | `bench/rts/ref/`, `tests/rts/ref/` | profile | no | 10.1 known answers; IR-vs-model differential; oracle budget (8); targeting metrics (4.2); model author does not read M3-04 sources |
| M3-03 | IR systems goal/combat/move/damage, authoring generator, capsule records | M3-01, M3-02, M2-01 (IR 0.2 verifier) | `capsules/rts/`, `bench/rts/gen_ir.py`, `schemas/capsule.schema.json` (only with OD-16) | gameplay | no (0.1 systems are native-eligible but native is not run here) | 10.4 negatives; regenerate-equality; measured instruction counts recorded; 5.5 record fields |
| M3-04a | Scheduler host-entry kind (OD-1b) | M3-00 and owner approval of OD-1b | `src/runtime/schedule.h`, `src/runtime/schedule.c`, `src/runtime/native_versions.c` (refusal guard only), `src/runtime/rts/` (registration) | shared_contract, native_core | yes (targeted compile; existing scheduler tests; new host-entry tests) | IR entries unchanged (existing tests pass); host dispatch, declared set, refusal of candidates and native handles |
| M3-04 | Host operators: spatial index, gather, scatter (C17) | M3-01, M3-02, M3-04a | `profiles/rts/native/`, `tests/rts/native/` | native_core | yes | 10.1 SPATIAL, GATHER, SCATTER; ASan, UBSan; instrumented store-set check (4.4); fault seams (N9). Depends on M3-02 known answers, not on M3-03 |
| M3-05 | `world.set_fields`, `world.spawn_batch`, replay record and replay, checkpoint recovery | M3-03, M3-04, M3-04a, OD-3, OD-3b, OD-3c approved | `src/agent/su_agent.c` (method handlers only), `schemas/`, `docs/AGENT_RPC.md` (with approval), `tests/rts/replay/`, `bench/rts/load/` | schema/runtime | yes | T-RTS-LOAD-1, T-RTS-CTRL-1, T-RTS-REPLAY-1..3, T-RTS-CAPS-1; recovery-log entries for the two new methods; OD-3c rule implemented |
| M3-06 | End-to-end suite, golden scenes, edge fixtures, mutation campaign, targeting gates | M3-02, M3-05 | `tests/rts/e2e/`, `tests/rts/fixtures/` | tests | yes (with engine runs) | C == Python digests for committed scenes within oracle limits; 10.6 gates; all listed mutants detected or recorded as equivalent |
| M3-07 | Agent-authored behaviours experiment (B1-B3, redefined) | M3-06 | `bench/rts/agent_experiments/`; candidates only through the candidate pipeline (no direct edits to `capsules/rts/` on the canonical branch) | evidence | no | records per section 11; hidden fixtures authored by a different party; each fixture fails on the r2 baseline |
| M3-08 | Stress curves and memory methodology | M3-06; M3-08a for exact accounting | `bench/rts/stress/` | evidence | no | raw logs with hashes, hardware blocks, named limits, null reasons; no predeclared unit-count claims; recovery claim per 12 |
| M3-08a | Exact per-root memory accounting (OD-10) | owner approval | `src/world/` (accounting API only) | native_core | yes | per-category bytes match analytic 9.3 table within stated tolerance; optional: without it, 9.3 figures stay estimates |
| M3-09 | Independent review of any performance optimization | M3-08 and the optimization | reviewer write-free | review | per change | C10 evidence present; reviewer differs from author; digests unchanged |
| M3-10 | Native lane for goal/combat/move (optional) | M1 owner acceptance, M3-03, M3-06 | `backends/native/` (lease) | native_core | yes | differential vs reference VM across row tails and dead rows; capability fallback |

Mapping to MILESTONES M3 closure evidence: deterministic replay (M3-05, M3-06); agent-authored behaviours (M3-07); end-to-end tick costs (M3-08); stress curves and memory accounting (M3-08, M3-08a); independently reviewed optimizations (M3-09). M3-10 is optional and does not block closure.

## 15. Mutation requirements (minimum set; each must be detected by a named test)

| id | mutant | detected by |
|---|---|---|
| M3-M01 | tie-break on slot reversed (higher slot wins) | T-RTS-SPATIAL tie case, edge fixture |
| M3-M02 | cell index off by one column (`(x>>6)+1`) | T-RTS-SPATIAL boundary x = 63/64 |
| M3-M03 | range check uses `<` not `<=` | T-RTS-SPATIAL exact-equal case |
| M3-M04 | query budget ignored (no truncation) | T-RTS-SPATIAL K and K+1 cases; truncation counter |
| M3-M05 | generation check removed from gather | T-RTS-GATHER stale anchor; T-RTS-SCATTER stale projectile after slot reuse |
| M3-M06 | scatter applies to dead target | T-RTS-SCATTER dead-target case |
| M3-M07 | scatter applies to non-unit (kind 2) target | T-RTS-SCATTER non-unit case |
| M3-M08 | scatter uses **wrapping** add instead of saturating add (r1 meaning was saturating; flipped in r2, F3) | T-RTS-SCATTER saturation case (two arrivals of 0xFFFFFFF0) |
| M3-M09 | scatter applies only the first shooter's damage per target per tick (drops later contributions) | T-RTS-SCATTER multiple-shooters case, two-shooter edge fixture |
| M3-M09b | scatter iterates in descending slot order | Equivalent under saturating add (4.3): not detectable by sums or digests. Recorded as an equivalent mutant, not as detected |
| M3-M10 | flight off by one (arrival at `t + flight - 1` or `+ 1`) | T-RTS-COMBAT flight 1 and 5 edges |
| M3-M11 | cooldown not zeroed when fire blocked or underflow; or reload not reduced by one on fire (period off by one) | T-RTS-COMBAT reload known answers (reload 6 flight 2; reload 1 flight 1) and underflow |
| M3-M12 | `incoming` not zeroed each tick | T-RTS-COMBAT zeroing test; two-tick damage fixture |
| M3-M13 | damage kill emitted twice or on anchor | T-RTS-DAMAGE exact-kill, anchor-never-killed |
| M3-M14 | clamp removed from goal | T-RTS-GOAL wrapped-negative case |
| M3-M15 | move overshoots goal (no min) | T-RTS-MOVE no-overshoot case |
| M3-M16 | move ignores mode 2 engagement stop | T-RTS-MOVE mode-2 stop case |
| M3-M17 | generator PRNG constant or uniform(n) modulo variant | T-RTS-SCENE-1 digest |
| M3-M18 | replay skips a digest record or ignores `seq` order | T-RTS-REPLAY-1 and -2 |
| M3-M19 | `set_fields` without revision check | T-RTS-CTRL-1 stale revision |
| M3-M20 | projectile state not cleared when shooter despawned (damage applied from dead shooter) | T-RTS-SCATTER shooter-despawned case |
| M3-M21 | grid indexes kind 2 (anchor) entries | T-RTS-SPATIAL anchor-never-target case |
| M3-M22 | team filter removed from target query | T-RTS-SPATIAL same-team case |
| M3-M23 | domain-edge box clipping removed (index wraps or reads out of range) | T-RTS-SPATIAL x = 0 and x = 32767 cases |
| M3-M24 | gather without the `alive` check | **Equivalent mutant**: despawn bumps the generation, so the generation check already rejects dead anchors (M2-00 section 1). Recorded as equivalent, not detected. Kept so the equivalence stays explicit |
| M3-M25 | `dmg_out` set on non-arrival ticks | T-RTS-COMBAT two-tick damage fixture |
| M3-M26 | `incoming` not zeroed on a dead row in the native path | M3-04 native differential across dead rows; T-RTS-COMBAT zeroing |
| M3-M27 | `proj_target_gen` not cleared on arrival | T-RTS-REPLAY-1 state digest (field 34 is canonical state) |
| M3-M28 | per-tick cap set below capacity for an RTS world | T-RTS-CAPS-1 with the RTS setting (all units die in one tick) |

Each mutant is applied to a copy of the tree, and the named test must fail. An unrun mutant is listed as unrun, not as passing. Equivalent mutants are listed as equivalent, not as detected.

## 16. Risks

1. **Per-tick caps at scale.** Mass deaths cannot exceed capacity per tick by construction (D11, F6). The cap is still a configured value that the stress run records; it must not be raised silently, and capacity above 1,048,576 is out of scope.
2. **Host operators carry gameplay logic.** The spatial query, gather and scatter are profile code. Their effects are declared and tested, not verified (OD-5). A later verifier extension is the fix; it is not claimed here.
3. **Approximate target under congestion.** Truncation makes targeting depend on budget and ring order. This is an accepted profile semantic (OD-6). Measured with the metrics of 4.2 and gated; not described as exact nearest-neighbour behaviour. The probe numbers in section R are from a non-generator scene and are not a claim about the committed scenes.
4. **Overlap without separation.** Congested scenes will stack units. That is a synthetic choice (OD-7), not a physical model.
5. **IR verbosity.** Flag encoding makes IR JSON long. The authoring generator (`bench/rts/gen_ir.py`) is committed and regeneration equality is tested, so hand edits cannot drift from the source. Its output is authoritative only together with the committed JSON digest.
6. **Dead-row waste.** Native kernels process all rows and rely on dead-row zeroing (M2). In high-death scenes this costs time and must be measured in M3-10, not assumed.
7. **Digest cost.** Canonical SHA-256 of 39 columns per tick is O(N). `digest_every` is recorded; per-tick digests are used for small replay tests only.
8. **Oracle reach.** The Python model cannot run 10^5-10^6 row scenes in useful time (N13 budget). Large-scene correctness rests on the C reference and small-scene digest equality. Records must say so.
9. **Dependence on M2.** M3 uses M2-00 and M2-01 behaviour (entities, buffers, snapshot 0.2/0.3, recovery log, checkpoint store). M2 is audited but not owner-accepted. Any M2 change reopens M3 sections 2 and 13.
10. **Projectile model limits.** One projectile per shooter with fixed flight time is not a general weapon system (OD-8).
11. **Native lane timing.** Native execution of goal, combat and move changes performance only; the reference VM stays the oracle (M3-10).
12. **Authoring tool trust.** The generators are Python; they are development helpers and not part of the native build. Their outputs are committed and digested so the no-build guarantee (C01) is not affected by them.
13. **Spatial grid memory and per-tick cost.** The 512 x 512 grid costs 2 MiB and a 262,144-bucket counting sort per tick (F2 change). Measured in M3-08 against the 128 x 128 grid of r1; if the cost is not acceptable, the owner revisits cell size and K together (OD-6).
14. **Snapshot and spool limits.** Recovery claims stop at N = 65,536 (F5). The row guard (838,860) and the 64 MiB cap bind before 1M. Changing either is OD-15.
15. **Host event accounting.** Whether boundary control events share the per-tick event counter is open until OD-3c is settled in M3-05 (F6).

## 17. Owner decisions requested

1. Accept D1 (no new IR opcodes for M3) and the re-evaluation trigger in 6.4 (OD-2).
2. Accept D2 (host operators `rts_ops_v1`) and the declared-and-tested effect model (OD-1, OD-5).
3. Approve the additive host-entry kind in `src/runtime/schedule.h`, with the declaration-module design of 3.1 and its rejected alternatives (OD-1b, F1).
4. Approve `world.set_fields` (entities batch, 1,024 per call) and `world.spawn_batch` (1,024 rows per call), each with recovery-log entries (OD-3, OD-3b, F4, F7).
5. Decide the boundary event-accounting rule (OD-3c, F6).
6. Approve the profile constants: 64-unit cells, 512 x 512 grid, K = 128, rc <= 8, ring order, `rts_u32_v1` domain, and the targeting gates of 4.2 (OD-6, OD-9, F2).
7. Approve saturating add for `incoming` (D5, F3).
8. Decide the snapshot limit: accept the N <= 65,536 recovery claim, or fund OD-15 (F5).
9. Confirm scope exclusions: no separation, pathfinding, obstacles, multiple projectiles per shooter (OD-7, OD-8).
10. Decide whether exact memory accounting (OD-10, M3-08a) lands before M3-08 or whether analytic figures are reported as estimates until then.
11. Decide whether M3-10 native work is in scope for M3 closure (OD-11) or deferred.
12. Confirm whether the M3 Python model may be written by a different agent in the same orchestration (OD-13).
13. Confirm the single Python generator and the 1,024-row load path (D10, OD-12, F8), and checkpoint cadence (OD-18).
14. Approve the redefined M3-07 behaviours B1, B2 and B3 and the removal of the r1 B2 and B3 (OD-19, F9).
