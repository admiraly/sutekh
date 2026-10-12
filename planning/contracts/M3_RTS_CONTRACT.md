# M3-00 contract r3: RTS microcosm (synthetic hordes, formations, movement, targeting, projectiles, damage, spatial load)

Status: DRAFT r3 for owner review. Design only; no product code is part of this contract. Builds on M2-00 (generational entities, IR 0.2, per-chunk struct/event buffers, snapshot 0.2) and M2-01 (migrations, snapshot 0.3, recovery log). Nothing here is accepted until the owner accepts this revision. Implementation of any task below is gated on M2 owner acceptance (`planning/status.json` shows M2 `implemented_audited_pending_owner_acceptance`).

Scope: a synthetic, non-BFME RTS profile. No original game assets, no BFME behaviour claim (C18, C19; M4 owns compatibility). Headers `include/sutekh/abi.h`, `ir.h`, `entities.h`, `status.h` are NOT changed by this contract. `src/runtime/schedule.h` is changed only by the additive host-entry kind in OD-1b, and only after owner approval. `src/ir/su_lir.h` is NOT changed (the host declaration module is built with the existing public struct, section 3.1). Every shared-interface change listed in section 13 requires its owner's approval.

Review input: `planning/audit/M3-00-contract-review.md` (r1, accept after fixes) and `planning/audit/M3-00-contract-rereview.md` (r2, accept after fixes; blocking items A-D). Section R lists how each item is resolved in r3.

---

## R. Changelog

### R.1 r2 to r3 (this revision)

Code facts used, checked against the tree at r3 time:
- `src/runtime/schedule.h`: `SuSysEntry` holds `SuLirModule *mod` and `SuExecContext *ctx`; `su_sched_add` builds the VM context through `su_vm_context_create` (`schedule.c:114`). `su_vm_context_create` (`src/vm/su_vm.c:178`) rejects any field with `access == 0` (`su_vm.c:203`), so a host declaration cannot be given to it.
- `src/runtime/jobs/su_effects.c`: `su_effects_derive` (`:39`) derives reads and writes from LOAD/STORE only; `su_effects_build` step 1 (`:254` area) calls `su_effects_check_declared` on every entry, which fails with `SU_E_SCHEMA_MISMATCH` when declared access differs from derived access. A module with no instructions therefore fails for any declared access. Derivation consumes `e->mod->fields[k].access` for the union.
- `src/runtime/worker.c:514` (`read_schedule_facts`) reads `phase`, `after`, `before` from the **capsule file** of each system (`paths[i].capsule`), called from `check_system_effects` (`:594-660`). `in[i].declared_parallel = 1` for every system (`:638`), so every same-phase conflicting pair needs an ordering path. `worker.c:700-733` loads each system with `su_reload_load(capsule, source)`, which requires a `.lir.json`.
- `schemas/capsule.schema.json`: top-level `additionalProperties: false`; keys present are `capsule_format, fields, ir_version, limits, numeric_profile, schedule, schema, source, system_id, tests`. `system_rev` and `capabilities` are absent.
- `src/runtime/entity_tick.c:546` runs non-entity systems through `su_sched_run_system`, which reaches `run_one_system` (`schedule.c:246`). `entity_tick.c:483-515` creates per-chunk contexts only for systems with `uses_entity_ops`.
- `src/world/entities/su_cmdq.c:641-644`: `su_cmdq_commit` refuses when `v.n_events + v.n_cmds > per_tick_e` and counts only records of the cmdq merge (system-emitted). Control-plane spawn (`worker.c` `h_world_spawn`, `:1072-1079`) calls `su_world_spawn` (no cap check) and appends its record with `ent_log_one`; it never enters the cmdq counter.
- `src/runtime/worker.c` `h_world_spawn` parses `external_id` as a u32 (`external_id must be a u32 integer`). The control plane therefore already has a 10-digit bound on the id.
- `src/agent/su_proto.h:5` and `su_proto.c:38/183`: the request line excluding the newline is capped at `SU_LIMIT_PROTOCOL_MESSAGE` = 1,048,576 bytes, refused with `SU_E_RESOURCE_LIMIT`.
- `src/agent/su_agent.c:57` `AGENT_DEFAULT_RETAIN` = 4; `:62` `AGENT_DEFAULT_SPOOL` = 32 MiB; `:978-984` `snapshot_bound` = `(fields + 1) * 11` = 440 bytes per row; `:5722-5726` reads `checkpoint_retain` and `spool_bytes_max`.
- `src/platform/sutekh_main.c:81-83`: the 512 MiB spool default and the comment claiming a 1,000,000-row world is about 28 MB per checkpoint.
- `src/world/snapshot.c:643`: capture refused when `hw * (field_count + 1) > SU_SNAPSHOT_MAX_BYTES / 2`, the 838,860-row guard at 39 fields. `hw` is `su_ents_high_water` (`src/world/entities/su_ents.h:56`).
- `src/world/su_world.h:157-173`: `su_world_build_invocation(SuWorld*, SuInvocation*)`, `su_world_build_outputs(SuWorld*, SuOutputs*)`, `su_world_commit_staging(SuWorld*, const uint32_t *field_ids, uint32_t count)`, `su_world_zero_dead`, `su_world_publish`, `su_world_abort`. `su_world.h:176-178`: `su_world_alive_view` and `su_world_generation_view` (base, n, buffer).
- `src/runtime/schedule.c:200` `su_sched_check_candidate` is the single candidate gate; `src/runtime/reload.c:162` calls it before any swap.
- `tests/scheduler/test_scheduler.c`, `tests/scheduler/test_tick_workers.c`, `tests/entities/test_entity_tick.c`, `tests/reload/`, `tests/native/qualification/test_native_chunked.c` exist and are the existing scheduler, worker, entity and native-refusal tests named below.

| id | finding | resolution in r3 | where |
|---|---|---|---|
| A (F1, blocking) | Host declaration module, effect path, capsule record, dispatch, worker phase reading and VM context were not specified | Exact mechanism in 3.1: `SuHostOpDesc` (fields, version, constants hash, run function); `su_sched_add_host` (new, additive); declaration module built by `su_host_decl_build` into an ordinary zeroed `SuLirModule` (no instructions, no header change); `SuEffectInput.host_declared` makes `su_effects_build` take the declared access bits and skip derive and check for host entries, IR path unchanged; host capsule record (`capsule.host` object, schedule block as today) loaded by `su_host_load` from a manifest item with `host_operator` instead of `source_path`; no VM context for host entries (`ctx == NULL`); dispatch branch first in `run_one_system`; candidate and native refusal. Required ordering edges written out in 3.2. Schema change (OD-16) isolated in lease M3-03a. Complete M3-04a lease and tests T-HOST-01..10 in section 14 and 10.7 | 3, 3.1, 3.2, 10.7, 13 OD-1b, OD-16, 14, 15 |
| B (F6/D11, blocking) | "No stall by construction" contradicted OD-3c (control events counted against the per-tick cap) | Chosen rule (i): control-plane records are appended to the event log at the boundary and are **not** counted by `max_events_per_tick`; the per-tick cap counts only the system-emitted records of the tick merge. Invariant: for RTS worlds the tick merge count is at most the number of alive rows at tick start, which is at most capacity, so `max_struct_per_tick = max_events_per_tick = capacity` cannot be exceeded. The claim is restricted to the RTS configuration (only `rts.damage` emits structural requests, `max_struct_per_row = 1`; no system emits events). Alternative (iii) (admission bound on pending control records) recorded but not chosen, with its cost. OD-3c changed to "excluded" (owner approval). Spawn-then-kill-all test T-RTS-CAPS-2 added | 0 D11, 9.1, 10.3, 13 OD-3c, 16 risk 15 |
| C (F4, blocking) | 1,024-row bound not safe because `external_id` was unbounded | `external_id` is a **u32** (as in `world.spawn`, `worker.c`), at most 10 digits. Computed worst case with every field and the id at 10 digits, compact JSON: 925 bytes per row, 948,225 bytes for 1,024 rows, 100,351 bytes headroom under 1,048,576 for the envelope. Byte-level check on the request line (excluding newline), before parsing rows, refuses over 1,048,576 with `SU_E_RESOURCE_LIMIT`. "All-worst-case" defined in T-RTS-LOAD-1 | D6, 10.3, 13 OD-3b |
| D (blocking) | Domains and writable set of control fields undefined; `flight = 0` deadlocks the shooter | Table 2.2: domain per control field, error `SU_E_TYPE`, `flight` in 1..64 for every row (`flight = 0` refused), writable set of `world.set_fields` defined (14 fields), system-owned and immutable fields refused, anchor and idle-value rules, `hp` spawn-only | 1, 2, 2.2, 10.3, 13 OD-3, OD-9 |
| audit N-a | 2% gate margin thin (1.5-1.7% measured on a probe) | Same 1,024 seeded queriers in 4.2 and 10.6. Gate unchanged. Margin rule added: a pass with point estimate above 1.5% is recorded as "thin margin" and raised as OD-6 in the M3-06 report; no silent pass | 4.2, 10.6 |
| audit N-b | Spool config keys unnamed; reservation formula; 440 vs 446 | RTS profile sets `checkpoint_retain = 1` (code default is 4) and `spool_bytes_max`. Reservation is `(retain + 1) * bound`. The 440 in `su_agent.c` is below the 446 worst case, so the RTS admission precheck (12, 9.3) uses 446 per row and 90 per retained event, independent of the agent-internal figure | 9.3, 12 |
| audit N-c | Periodic capture refused by the row guard became SU_E_INTERNAL (unrecoverable) | M3-05 admission precheck refuses `world.spawn` and `spawn_batch` that would push high water above 838,860 (`su_ents_high_water`) with `SU_E_RESOURCE_LIMIT`, no state change, before admission. A capture refused by the guard after this is an invariant break (SU_E_INTERNAL, a finding). Spool precheck added. Note only for the stale comment in `sutekh_main.c:81-82` | 9.3, 12 |
| audit N-d | B1-B3 candidate order and fixture baseline | Order B3, B2, B1 with reasons; each fixture fails on the r2 baseline and on every earlier accepted candidate; each accepted candidate is re-run against all earlier fixtures. B1 needs extra ordering edges (3.2) | 11 |
| audit N-e | 9.3 total (440 per row) did not match its table (400 per row) | Total is 400 bytes per row (352 + 12 + 4 + 32) plus 2 MiB fixed plus event log and spool, stated separately | 9.3 |
| audit N-f | N7 check before phase 0 impossible | Check after phase 1 of tick 0 (after the commit of `rts.goal`, before `rts.combat`) | 7.3 |
| audit N-g | M3-M27 not detectable by T-RTS-REPLAY-1 (same tree on both sides) | Detector is the Python-model comparison of M3-06 (state digest per tick against the independent model) | 15 |
| audit N-h | OD-16 left conditional | OD-16 invoked: `capsule.schema.json` gains `system_rev`, `capabilities`, and the host variant (`host` object; `source`, `ir_version`, `fields` required only for the IR variant). Single schema writer M3-03a | 5.5, 13 OD-16, 14 |
| audit N-i | Ring fixture expectation not deterministic | Exact outcome: `target_valid = 0`, `truncated = 1` (10.1) | 10.1 |
| audit N-j | Section 17 omitted OD-14, OD-16, OD-17 | Added | 17 |
| audit N-k | Dispatch site for host entries not stated | `entity_tick.c:546` reaches `run_one_system` through `su_sched_run_system`; the host branch there covers it, and the entity path is not used for host entries | 3.1 |
| audit N-l | `order_mode` values outside 0..2 | Refused with `SU_E_TYPE` (2.2) | 2.2 |
| audit N-m | Gameplay tuning of constants needs a native build | Recorded: K, cell size, scatter rule and the three host constants are native (`rts_ops_v1`, OD-5, N8). Capsule-only edits do not change them | 4.4, 13 |
| audit N-n | `tasks.json` M3-00 scope differs from this table | Orchestrator item; this contract does not edit `planning/tasks.json` | 14 |

Further r3 changes found while verifying the above (not in the audit):
- The r2 weapon-table rule in section 13 ("weapon out of table") had no rule behind it. The control plane does not enforce the weapon table; it enforces the domains of 2.2 only. The table is a scene-generator convention (7.4).
- `hp` is written only by `rts.damage` (table 2) and by spawn. It is not writable by `set_fields`, so the N11 rule "kind 1 with hp 0 refused in set_fields" is replaced by the spawn rule and the domain in 2.2.
- The `set_fields` entity payload is 354 bytes at most (14 writable fields with 10-digit values, 20-digit handle, compact JSON), not 886 (which counted all 39 fields). 1,024 entities is 362,496 bytes, under the cap.
- The spool precheck, not the row guard, binds first at default settings: with `retain = 1` and 512 MiB, the reservation `2 * (446 N + 90 * 65,536)` fits up to about 588,000 rows (worst-case estimate, unmeasured). This is recorded in 9.3 and in the 12 stop rule.
- The candidate and native refusal of host entries uses the existing `SU_E_CAPABILITY_DENIED` code (no new code).

Renumbering: none of the existing ids is renumbered. New ids: M3-03a (schema-only, OD-16), M3-04a (host-entry kind), M3-08a (exact accounting), T-HOST-01..10, T-RTS-CAPS-2, M3-M29, M3-M30, M3-M31. Meaning changes a reader could misread: M3-M08 (wrapping add must be detected), B2 and B3 (redefined), OD-3c (now "excluded", was "counted"), OD-3 and OD-3b (new signatures, u32 external id), T-RTS-SCENE-2 (engine-load check), M3-M27 (detector is M3-06).

### R.2 r1 to r2 (unchanged from r2)

Resolutions F1-F9 and N1-N16 of the r1 review, as written in r2, stand except where r3 changes them as listed in R.1. The r2 changelog (F1 to F9: host-entry kind, targeting metrics and 64-unit cells, saturating add, 1,024-row cap, memory limits, control caps, `set_fields` batch, scene generator, B1-B3 redefinition, N1 to N16) is kept in the history of this file (git) and is not repeated here.

---

## 0. Decisions at a glance

| # | Decision | Why (short) |
|---|---|---|
| D1 | **No new IR opcodes for M3.** Boolean composition uses nested `select_u32`; a bool comes from `eq_u32(flag, 1)`; min via `lt`+`select`; abs-difference via `lt`+`sub`+`select`. | Every M3 per-row rule is expressible in the current 0.1/0.2 table (section 6). New opcodes would be speculative (C18). Re-evaluate only if measured counts or authoring error rates fail (6.4). |
| D2 | **Indexed gather, scatter-add and the spatial index are host-side RTS-profile phase operators** (`rts_ops_v1`), not IR. | IR loads read only the current row, so a runtime-indexed read or a write to another entity is inexpressible (LIVE_IR_SPEC sections 1, 5). Operators have fixed, bounded, reference-modelled semantics (section 4). |
| D3 | **Death is `despawn_self` in an IR 0.2 system** (reference VM only). Movement, goal and combat are IR 0.1 (native-eligible). | Keeps the native lane (IR 0.1 only) useful for per-row math; death uses the M2 path. |
| D4 | **Projectiles are per-shooter embedded state with a fixed flight time** (no spawn from IR, no homing, no division). | Spawn-from-IR is reserved and unsupported in M2. A fixed flight time avoids division and keeps the state a plain row. |
| D5 | **Cross-entity damage is `scatter_sat_add_u32` keyed by a generational target handle**, saturating at 2^32 - 1. | Saturating add of unsigned values is commutative and associative, so merge order cannot change the result. Canonical order is still recorded (4.3). |
| D6 | **Two control-plane additions**: `world.set_fields` (batch of entities, writable fields only, 2.2) and `world.spawn_batch` (scene load), each at most 1,024 rows or entities per call under the 1 MiB request cap; `external_id` is a u32. | Orders and formation changes mid-run are otherwise one call per unit. The byte bound is 948,225 bytes for 1,024 worst-case rows (10.3). OD-3, OD-3b. |
| D7 | **Spatial index**: dense 512 x 512 grid of 64-unit cells over the 32768^2 domain; ring-ordered query over at most 17 x 17 cells (rc <= 8) and at most 128 examined entries, with an explicit `truncated` flag and reported `valid_but_not_nearest` metric. | Probe 1 (section R.2, F2): 256-unit cells leave most congested targets not nearest; 64-unit cells with K=128 leave about 1.5%. Bounded work per query (C10, LIVE_IR_SPEC section 7). OD-6. |
| D8 | **Numeric profile `rts_u32_v1`**: all fields u32; coordinates 0..32767; squared distances < 2^31; no division in IR; host operators use compare, add, sub, shifts and an exact integer square root. | No float (C08); no overflow in distance math by construction. |
| D9 | **Six scheduled systems** under the existing `SU_SCHED_MAX_SYSTEMS` = 8: `rts.spatial` and `rts.scatter` are host entries (new kind, OD-1b); `rts.goal`, `rts.combat`, `rts.move` are IR 0.1; `rts.damage` is IR 0.2. | Schedule capacity unchanged. Only the entry kind is added, additively. |
| D10 | **One normative scene generator**: Python `bench/rts/gen/` (M3-01). No C scene generator in M3. | Removes the two-generator contradiction (F8). The engine-loaded state is checked against the generator digest. |
| D11 | **RTS world caps and the stall rule**: `max_struct_per_tick = max_events_per_tick = capacity`, requiring capacity <= 1,048,576. Control-plane records are excluded from `max_events_per_tick` (OD-3c). Under this configuration the tick merge cannot exceed the cap (proof in 9.4). | Deaths per tick are at most the alive rows at tick start (at most capacity), and control records never enter the tick-merge count. The earlier "no stall by construction" wording was wrong under OD-3c (audit B); this is the corrected rule. |

---

## 1. Entity model and identity

- One entity table, all rows are `u32` columns. Row kinds are data, not types:
  - `kind = 1` unit (participates in combat and the spatial index). Requires `hp >= 1` at spawn (2.2). `hp` is never written by `set_fields`.
  - `kind = 2` formation anchor (moves, carries no weapon, is never targeted, never damaged). Requires `hp = 0` and `range_sq = 0` (2.2).
  - `kind = 0` reserved; never valid for a live row (validation rejects it with `SU_E_TYPE`).
- Handles are the M2 `SuEntity` (`generation << 32 | slot`). Every generation-checked reference in the schema stores the slot (u32) and the generation (u32) as two fields. Initial spawns use generation 1.
- Capacity is fixed at `world.create` (`max_entities`). Scenes never grow capacity. Spawn and despawn churn are bounded by capacity (M2 lowest-free-slot reuse).
- Dead rows are skipped by IR systems (VM alive mask) and by host operators (explicit `alive` check). Dead rows are canonically zero after every system commit (M2-00 section 1; `su_world_zero_dead`).

## 2. Schema and column layout

All fields are `u32`, stored as SoA columns, chunked by `SU_CHUNK_SLOTS` (1024). `NONE = 0xFFFFFFFF`. `BIAS = 16384`. Field ids are assigned in the order below, starting at 1, and are stable within a lineage (M2-01 rule). The reads column lists the systems whose formulas (sections 4 and 5) read the field; the verifier derives IR effects from IR, and host operators declare theirs (4.4).

| id | name | role | written by (store) | read by |
|---|---|---|---|---|
| 1 | `kind` | 0 invalid, 1 unit, 2 anchor; immutable after spawn | control plane only (spawn) | spatial, scatter |
| 2 | `team` | 0 or 1; immutable after spawn | control plane only (spawn) | spatial |
| 3 | `hp` | hit points; >= 1 for units, 0 for anchors; set by spawn only | damage (store); spawn | damage, spatial |
| 4 | `pos_x` | 0..32767 | move; control | spatial, goal, move |
| 5 | `pos_y` | 0..32767 | move; control | spatial, goal, move |
| 6 | `speed` | units per tick, 0..1024 | control | move |
| 7 | `range_sq` | weapon range squared, 0 = unarmed, <= 262144 | control | spatial |
| 8 | `reload` | cooldown ticks, 1..1024 (every row) | control | combat |
| 9 | `cooldown` | remaining ticks | combat | combat |
| 10 | `damage` | damage per hit, 1..65535 (every row) | control | combat |
| 11 | `flight` | flight ticks, 1..64 (every row) | control | combat |
| 12 | `order_mode` | 0 none, 1 move (ignores enemies), 2 attack-move | control | goal, move |
| 13 | `order_x` | 0..32767 | control | goal |
| 14 | `order_y` | 0..32767 | control | goal |
| 15 | `formation_slot` | anchor slot, or NONE | control | spatial (gather) |
| 16 | `formation_gen` | generation of anchor at assignment; 0 when `formation_slot` = NONE | control | spatial (gather) |
| 17 | `formation_ox` | offset x + BIAS, offset in [-16384, 16383] (stored 0..32767) | control | goal |
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

Canonical state (M2-00 encoding v2) covers all 39 fields plus the entity table and event log. The schema descriptor hash (M2-01 section 6) covers the sorted `(id, type, name, default)` list. Field defaults are all 0 (M3 has no migration). Spawn requests name every field whose intended value is not 0; the scene generator writes `NONE` explicitly into `target_slot`, `dmg_target`, `proj_target` and `formation_slot` where NONE is intended (2.2 idle-value rule).

### 2.1 Numeric profile `rts_u32_v1`
- Domain: all positions and orders in `[0, 32767]`. Violations are rejected by the control plane (`SU_E_TYPE`, path = field name; 2.2). Host operators treat out-of-domain as `SU_E_INTERNAL`.
- Squared distance: `dx = |x1 - x2|`, `dy = |y1 - y2|` each <= 32767; `dx*dx + dy*dy <= 2*32767^2 = 2147352578 < 2^31`. No intermediate wraps.
- u32 modular identities used in formation: `(a + ox - BIAS) mod 2^32 == a + offset` exactly when the true result is in `[0, 2^32)`. Anchor positions are in-domain, so `a + offset` may be negative; the clamp (5.1) handles that through the 2^31 sign threshold.
- No division in IR. Host operators use integer shifts and compares only, plus an exact integer square root (Newton method; reference in the Python model).

### 2.2 Control-plane domains, writable sets and idle values (D, r3)

Applies to `world.spawn`, `world.spawn_batch` and `world.set_fields`. Every violation refuses the whole call with no state change. Values are JSON integers within u32; a non-integer or out-of-range JSON value is `SU_E_TYPE` (existing behaviour, `worker.c` "field values must be u32 integers").

| field | at spawn | by `set_fields` | domain | error on violation |
|---|---|---|---|---|
| `kind` | yes | no (immutable) | {1, 2} (0 never valid) | `SU_E_TYPE` |
| `team` | yes | no (immutable) | {0, 1} | `SU_E_TYPE` |
| `hp` | yes | no (written only by damage and spawn) | kind 1: 1..65535; kind 2: 0 (**proposed bound 65535, OD-9**) | `SU_E_TYPE` |
| `pos_x`, `pos_y` | yes | yes | 0..32767 | `SU_E_TYPE` |
| `speed` | yes | yes | 0..1024 | `SU_E_TYPE` |
| `range_sq` | yes | yes | 0 or 1..262144; kind 2 requires 0 | `SU_E_TYPE` |
| `reload` | yes | yes | 1..1024, every row (anchors included, so N1 holds for all rows) | `SU_E_TYPE` |
| `damage` | yes | yes | 1..65535, every row | `SU_E_TYPE` |
| `flight` | yes | yes | 1..64, every row (`flight = 0` would leave `proj_active` set forever) | `SU_E_TYPE` |
| `order_mode` | yes | yes | 0, 1, 2 (other values fall through to attack-move in the r2 formulas, so they are refused) | `SU_E_TYPE` |
| `order_x`, `order_y` | yes | yes | 0..32767 (checked even when `order_mode = 0`) | `SU_E_TYPE` |
| `formation_slot` | yes | yes | NONE, or 0..capacity-1 | `SU_E_TYPE` |
| `formation_gen` | yes | yes | u32; must be 0 when `formation_slot` = NONE | `SU_E_TYPE` |
| `formation_ox`, `formation_oy` | yes | yes | 0..32767 (offset + BIAS) | `SU_E_TYPE` |
| `cooldown`, `goal_*`, `anchor_*`, `target_*`, `truncated`, `proj_*`, `dmg_*`, `incoming` (system-owned) | idle value only: 0, except `target_slot`, `dmg_target`, `proj_target` which accept 0 or NONE | **no** | not writable | `SU_E_TYPE` |

Rules that depend on other fields:
- **Writable set of `world.set_fields`** (exactly these 14 names): `pos_x, pos_y, speed, range_sq, reload, damage, flight, order_mode, order_x, order_y, formation_slot, formation_gen, formation_ox, formation_oy`. Each of these is written by control in the table of section 2. A name in the table but not in this list (`kind`, `team`, `hp`, every system-owned field) is `SU_E_TYPE`. A name that is not a field of the schema is `SU_E_SCHEMA_MISMATCH` (existing behaviour, `worker.c` "values names an unknown field").
- `range_sq` on a kind-2 row must remain 0; `set_fields` reads `kind` from the committed row to check it. A kind-2 row with `hp != 0` is refused at spawn.
- Duplicate entity in one `set_fields` call: `SU_E_INVALID_ARGUMENT` (10.3). Stale handle or dead entity: `SU_E_STALE_HANDLE`.
- The table is a set of profile constants. The bounds of `damage`, `flight`, `reload`, `speed`, `hp` and `order_mode` are `rts_u32_v1` profile constants (OD-9). Changing them is a profile change with a new digest.

## 3. Scheduler and phases

Per tick, after control-plane commands for the boundary have been applied (M2-00 section 2), the tick runs these phases in this order.

| idx | system id | kind | native-eligible | reads (summary) | writes (summary) |
|---|---|---|---|---|---|
| 0 | `rts.spatial` | host entry (OD-1b), 4 sub-steps (4.1) | no | alive, kind, team, hp, pos, formation_*, range_sq | 21-30 |
| 1 | `rts.goal` | IR 0.1 | yes | order_*, anchor_*, formation_o*, pos (B1 adds hp, team) | 19, 20 |
| 2 | `rts.combat` | IR 0.1 | yes | target_valid, target_slot, target_gen, cooldown, reload, damage, flight, proj_* | 9, 31-39 (5.2; 39 is the store of 0) |
| 3 | `rts.move` | IR 0.1 | yes | pos, goal, speed, order_mode, target_valid (B2, B3 add cooldown, anchor_valid) | 4, 5 |
| 4 | `rts.scatter` | host entry (OD-1b), 4.3 | no | dmg_*, alive, generation, kind | 39 (of target rows) |
| 5 | `rts.damage` | IR 0.2 (despawn_self) | no (reference VM only) | hp, incoming | 3 (and despawn) |

Order rationale: spatial reads tick-start state; goal and combat read spatial outputs; move reads goal and `target_valid`; combat zeroes `incoming` (through its store of 0) before scatter adds; scatter precedes damage; deaths take effect at the boundary merge.

Semantics fixed by this order: all IR loads see tick-start values and each phase reads the state committed by the previous phase. Combat decides firing with the position from tick start; move then changes position. This is the documented M3 ordering, not a race.

Boundary (after phase 5): merge in canonical order `(system_index, slot, seq)`; apply `world.despawn` requests emitted by `rts.damage` (one `SU_EVENT_DESPAWNED` each); zero dead slots; publish. Tick abort cases follow M2-00 (`SU_E_RESOURCE_LIMIT` leaves nothing published).

Count: 6 systems <= `SU_SCHED_MAX_SYSTEMS` (8). Chunk parallelism is safe for IR 0.1 systems and damage (rows write only their own row). Spatial and scatter are serial in M3 (OD-14); with `workers > 1`, host entries run serially in their phase.

### 3.1 Host-entry kind (OD-1b, M3-04a): mechanism

Required change to `src/runtime/schedule.h`, additive. `src/ir/su_lir.h` is not changed.

**Descriptor (new types in `schedule.h`):**
```c
#define SU_SYS_KIND_IR   0u
#define SU_SYS_KIND_HOST 1u

typedef struct SuHostField {
    uint32_t id;
    uint32_t type;        /* SU_TYPE_U32 */
    uint32_t access;      /* SU_ACCESS_READ and/or SU_ACCESS_WRITE; never 0 */
    const char *name;
} SuHostField;

typedef struct SuHostOpDesc {
    const char *system_id;          /* "rts.spatial", "rts.scatter" */
    const char *schema_id;          /* must equal the world schema ("rts") */
    uint32_t operator_version;      /* 1 for rts_ops_v1 */
    uint8_t constants_hash[32];     /* sha256 of the operator constants (4.4) */
    uint32_t field_count;
    const SuHostField *fields;      /* declared set, ascending id */
    uint32_t (*run)(const SuInvocation *in, SuOutputs *out, void *host_ctx);
    void *host_ctx;                 /* operator scratch, admitted at creation, never allocated in the tick */
} SuHostOpDesc;
```
`SuSysEntry` gains `uint32_t kind;` (`SU_SYS_KIND_IR` = 0, so every existing entry is unchanged) and `const SuHostOpDesc *host;` (NULL for IR entries).

**Declaration module (no header change):** `uint32_t su_host_decl_build(const SuHostOpDesc *d, SuLirModule *out)` in new file `src/runtime/rts/host_decl.c`. It zeroes `*out` and fills the public struct fields: `artifact_format = SU_ARTIFACT_FORMAT_VERSION`; `system_id`, `query` (= `schema_id`), `numeric_profile = "rts_u32_v1"` as `malloc`ed copies; `fields` as a `calloc`ed table in ascending id with `name` copied; `field_count` from the descriptor; `register_count = 0`, `insn_count = 0`, `insns = NULL`, `max_rows = 0`, `ir_version_minor = 0`, `max_struct_per_row = 0`, `max_events_per_row = 0`, `uses_entity_ops = 0`, hash strings empty. All of these are freed by the existing `su_lir_module_free`, so `su_sched_entry_destroy` needs no change for the module. The declaration is never passed to `su_lir_verify`, `su_vm_context_create` or native lowering.

**Schedule entry:** `uint32_t su_sched_add_host(SuSchedule *s, const SuHostOpDesc *d, SuLirModule *decl, uint32_t schema_version, uint32_t gen, uint64_t txid, uint64_t system_rev, uint64_t tick)`. It checks `decl` against `d` (same id, query, field table), appends an entry with `kind = SU_SYS_KIND_HOST`, `host = d`, `mod = decl`, `ctx = NULL`, and takes ownership of `decl` on `SU_OK` only. It never calls `su_vm_context_create`. `su_sched_add` (IR) is unchanged, and the `schedule.c:114` context creation runs only on the IR path.

**Effect path (`src/runtime/jobs/su_effects.{c,h}`):** `SuEffectInput` gains `uint32_t host_declared;` (appended at the end). In `su_effects_build` step 1, when `host_declared != 0`, the read and write sets of the entry are taken from `mod->fields[k].access` (bits READ and WRITE) and `su_effects_check_declared` is skipped; `su_effects_derive` is not called for that entry. For IR entries the code path is unchanged, so `su_effects_check_declared` still rejects a declared/derived mismatch. Steps 2 to 5 (edges, closure, conflicts, canonical order) are unchanged and apply to host entries.

**Worker load (`src/runtime/worker.c`):**
- The world's `capsules` manifest item for a host system is `{"capsule_path": "...", "host_operator": "rts.spatial"}` and has no `source_path`. The loader branch for items with `host_operator` calls `su_host_load(capsule_path, host_operator, &desc, &decl, msg)` (in `src/runtime/rts/host_decl.c`), then `su_sched_add_host`. The IR branch (`su_reload_load`) is unchanged.
- `su_host_load` parses the capsule's `schedule` block and `host` object, checks that `system_id` equals the descriptor's, that `host.operator_version` and `host.constants_hash` equal the descriptor's (else refuse), and that `host.fields` equals the descriptor's field table (ids, names, types, access) exactly (else `SU_E_SCHEMA_MISMATCH`). Then it builds the declaration.
- **How the worker reads phase and ordering:** `check_system_effects` calls `read_schedule_facts(paths[i].capsule, ...)` for every system, including host systems. Phase, `after` and `before` come from the capsule file's `schedule` block, not from the descriptor. Therefore every host capsule record must carry a `schedule` block (5.5). `check_system_effects` sets `in[i].host_declared = (sched->sys[i].kind == SU_SYS_KIND_HOST)`. The `after` target must be an earlier index (`worker.c:644-660`), so the capsule order is the schedule order (3.2).

**Dispatch (`src/runtime/schedule.c`, `run_one_system`):** the first branch is `if (s->sys[i].kind == SU_SYS_KIND_HOST) return run_host(w, &s->sys[i]);`, before the native branch and before any chunked branch. `run_host`:
1. `su_world_build_invocation(w, &inv)` and `su_world_build_outputs(w, &out)` (working reads, staging writes);
2. `st = d->run(&inv, &out, d->host_ctx)`;
3. on `SU_OK`, `su_world_commit_staging(w, write_ids, write_count)` with the descriptor's write ids in ascending order;
4. return `st`. The caller already runs `su_world_zero_dead` after every system (`schedule.c:308`, `entity_tick.c:549`); that is the host zeroing step, so no zeroing code is added.
A host entry ignores `opt->workers` (serial, 3.2 and OD-14). `entity_tick.c:546` reaches this branch through `su_sched_run_system`, and host entries get no per-chunk entity context (`entity_tick.c:483-515` creates contexts only for `uses_entity_ops`).

**Refusals:**
- `su_sched_check_candidate` (`schedule.c:200`) looks up the entry named by `m->system_id`; if it is a host entry, it returns `SU_E_CAPABILITY_DENIED` with a message. `reload.c:162` calls this before any swap, so the old entry stays.
- Native lowering: the entry point that lowers an entry to native (`native_versions.c`, the path that today calls `su_vm_context_create(a->mod, ...)` at line 477 and its caller) refuses a host entry with `SU_E_CAPABILITY_DENIED` before reading `mod`. Compiled under `SU_HAVE_NATIVE`.
- Rejected alternatives (unchanged from r2): `mod == NULL` (37 dereference sites, 5 files); an IR no-op module with stores (fakes IR); running host operators outside the schedule (the tick loop would fork and the effect and manifest machinery would not apply).

**Production descriptors** for `rts.spatial` and `rts.scatter` are written in M3-04 (`profiles/rts/native/`), not in M3-04a. M3-04a ships the mechanism with test-only descriptors (T-HOST tests). M3-04a also writes the two host capsule records (`capsules/rts/host/`), whose declared sets are the 4.1 and 4.3 sets. M3-04 must produce descriptors equal to those records, which T-HOST-05 and the M3-04 load test check. No stub operator is registered in the product build.

### 3.2 Required ordering edges (r3)

Worker declares every system `parallel` (`worker.c:638`), so every same-phase pair whose derived or declared effects conflict must be ordered. All six systems use phase `rts`. Conflicts that need an edge, from the sets of section 2:

| pair | conflict | edge needed |
|---|---|---|
| spatial, goal | spatial writes 21-23; goal reads 21-23 | spatial before goal |
| spatial, combat | spatial writes 24, 25, 27; combat reads them | spatial before combat |
| spatial, move | spatial writes 27 (target_valid), which move reads; spatial reads 4, 5, which move writes | spatial before move |
| spatial, damage | spatial reads 3 (hp); damage writes 3 | spatial before damage |
| goal, move | goal writes 19, 20; move reads them; move and goal read 4, 5 vs move write | goal before move |
| combat, scatter | combat writes 39 (store 0) and 36-38; scatter reads 36-38 and writes 39 | combat before scatter |
| scatter, damage | scatter writes 39; damage reads 39 | scatter before damage |
| combat, damage | combat writes 39; damage reads 39 | implied by combat before scatter before damage (transitive order is accepted by `su_effects_build`) |

Capsule `schedule.after` lists (capsule order 0 spatial, 1 goal, 2 combat, 3 move, 4 scatter, 5 damage, every target earlier in this order):

- `rts.spatial`: none
- `rts.goal`: `[rts.spatial]`
- `rts.combat`: `[rts.spatial]`
- `rts.move`: `[rts.spatial, rts.goal, rts.combat]`
- `rts.scatter`: `[rts.combat]`
- `rts.damage`: `[rts.spatial, rts.scatter]`

These are the complete edges for the r3 field sets. The B candidates of section 11 add edges (B1 needs `rts.damage` after `rts.goal` and `rts.move`, because B1 makes them read `hp`; B2 reads `cooldown`, already ordered after combat; B3 reads `anchor_valid`, already ordered after spatial). A candidate that adds an unordered conflict is refused by `su_effects_build` with `SU_E_SCHEMA_MISMATCH`, so the effect check is the gate.

## 4. Host-side phase operators (`rts_ops_v1`)

Common rules: integer-only; read only frozen inputs (tick-start state for `rts.spatial`, pre-scatter state for scatter); allocation only at admission (no allocation in the tick, C11); buffers sized from capacity and the declared bounds below; every operator is reference C (`profiles/rts/native/`) and a Python model (section 8). Each operator has a fault seam (N9).

### 4.1 `rts.spatial` (four sub-steps, run in this order)

Let `cap` = capacity, `alive(s)` = slot `s` alive at tick start.

(a) **Anchor gather**: for each alive row `r` with `formation_slot != NONE`, `anchor_valid(r) = gather_valid(formation_slot, formation_gen)`; if valid, `anchor_x/y = pos` of that slot, else 0. `gather_valid(s, g)`: `s != NONE && s < cap && alive(s) && generation(s) == g`. Rows not valid write `anchor_valid = 0`, `anchor_x = anchor_y = 0`. Rows with `formation_slot == NONE` write `anchor_valid = 0` too. (The `alive(s)` term is redundant because despawn bumps the generation; see M3-M24.)

(b) **Grid build** (serial, global): cell `c(x,y) = (y >> 6) * 512 + (x >> 6)` for alive rows with `kind == 1 && hp > 0`. Counting sort into `cell_start[262145]` and `cell_items[N_idx]`, items in ascending slot order within each cell. Memory: 262144 counts plus 262145 starts (u32), i.e. 2 MiB fixed, plus 4 bytes per indexed row. Built once per tick.

(c) **Target query** (per alive row `r` with `kind == 1 && range_sq > 0`; others write `target_slot = NONE, target_gen = 0, target_dist_sq = 0, target_valid = 0, truncated = 0`):
1. `R = isqrt(range_sq)` (floor). Cell radius `rc = (R + 63) >> 6`. Since `range_sq <= 262144` is validated (2.2, N5), `R <= 512` and `rc <= 8`. The operator treats `rc > 8` as `SU_E_INTERNAL`.
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

Gates (M3-06, applied to the committed `congested.blob.v1` at 1,024 and 16,384 rows, over the same 1,024 seeded queriers as 10.6): `missed <= 0.1%`, `valid_but_not_nearest <= 2%`, ratio p95 `<= 3.0`. A gate failure is an owner decision on K (OD-6); the cell size is not changed silently.

Margin rule (audit N-a): the probe (non-generator scene, 600 queriers, section R.2 of r2) measured 1.5 to 1.7% not-nearest at 16k rows. A committed-scene pass with a point estimate above 1.5% is recorded as `thin_margin: true` in the M3-06 report and raised as an OD-6 item in the same report. It is not a silent pass, and the gate itself is not changed.

### 4.3 `rts.scatter` (`scatter_sat_add_u32`)
For each alive row `r` in ascending slot order with `dmg_target(r) != NONE` and `dmg_out(r) != 0`:
- Let `t = dmg_target(r)`. Apply iff `t < cap && alive(t) && generation(t) == dmg_target_gen(r) && kind(t) == 1`.
- Then `incoming(t) = sat_add(incoming(t), dmg_out(r))`, where `sat_add(a, b) = (a + b < a) ? 0xFFFFFFFF : a + b` in u32.
- Writes only field 39 of target rows. Reads: fields 36-38, `alive`, `generation`, `kind`.
- Determinism: saturating add of unsigned values is commutative and associative, because the saturated sum of any multiset of inputs is `min(sum, 0xFFFFFFFF)`. The result is therefore independent of order, and the canonical row order is kept for any future non-commutative combine.
- Lethality: a saturated `incoming` remains `>= hp`, so the damage rule (5.4) still kills exactly once.
- Serial in M3 (OD-14). A later parallel version uses per-chunk partial sums merged in canonical chunk order with the same saturating combine.

### 4.4 Declared effects (C04 gap, owner decision OD-5) and versioning (N8)
Host operators are not verified from IR. Each operator's descriptor (3.1) declares its read set, write set and `operator_version`. An instrumented debug build records every field store per phase and asserts the store set equals the declaration (M3-04 test). Native or release builds skip the instrumentation; the claim is then "declared and tested", not "verified". The effect graph uses the declared sets (3.1, `host_declared`), so a wrong declaration changes ordering checks and is caught by T-HOST-04 and by the instrumented test, not by the verifier.

Constants of `rts_ops_v1` are native gameplay semantics: `K = 128`, cell size 64, grid 512, tie-break rule, saturating add, the `alive`/`generation` gate. Changing any of them is a native build plus a new `operator_version` (audit N-m: tuning them is not a capsule-only edit). The sha256 of the constants and version is `operator_manifest_hash`, recorded in the replay header (9.1). Replay fails closed on mismatch, as M2-01 section 6 does for schema.

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
Reload (N1): `reload >= 1` is validated for every row (2.2), so `reload - 1` does not wrap. Derivation: fire at tick `t` sets `cooldown' = reload - 1` at the end of `t`. Each later tick decrements it until 0, so `cooldown = 0` at the start of tick `t + reload` and the earliest re-fire is `t + reload`. Projectile: fire sets `proj_left = flight`; arrival happens in tick `t + flight` (the tick whose start value of `proj_left` is 1), and `proj_active` clears at that tick's end, so the earliest next fire is `t + flight + 1`. Shot period is therefore `max(reload, flight + 1)`. Known answers (10.1): reload 6, flight 2 gives re-fire at `t + 6`; reload 1, flight 1 gives fires at `t`, `t + 2`, `t + 4`; reload 2, flight 5 gives re-fire at `t + 6`. `flight >= 1` (2.2) is what makes `proj_left` reach 1.

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
Anchors have `hp = 0` and `incoming = 0`, so they never die by damage. A saturated `incoming` (4.3) still gives the correct kill. The reference VM runs this system only (native refuses 0.2, M2 rule). `max_struct_per_row = 1`, `max_events_per_row = 0` (5.5), and this is the only structural producer in the schedule (9.4).

### 5.5 Capsule records (N15, C04, OD-16)
Each capsule in `capsules/rts/` (IR variant) declares, using `schemas/capsule.schema.json`:
- identity: `system_id`, `system_rev`, `ir_version` (0.1 or 0.2);
- schema dependencies: `schema_id`, `schema_version`, `layout_generation`, field table digest;
- field reads and writes exactly as in section 2 (per-system lists in 5.1 to 5.4);
- schedule block: `phase` (`rts`) and `after` exactly as 3.2;
- numeric profile `rts_u32_v1`;
- capabilities: none, except `rts.damage` (`despawn_self`, IR 0.2);
- resource bounds: `max_instructions` 4096, `max_registers` 4096, and for `rts.damage` `max_struct_per_row = 1`, `max_events_per_row = 0`;
- test obligations: the T-RTS ids of section 10 that exercise the system.

Each host capsule in `capsules/rts/host/` declares, under the host variant of the schema: identity (`system_id`, `system_rev`); the `schedule` block as above; `numeric_profile` `rts_u32_v1`; a `host` object with `operator` (`rts_ops_v1`), `operator_version`, `constants_hash` (sha256 hex), and `fields` (id, name, type, access) equal to the 4.1 or 4.3 declared sets; and test obligations. A host capsule has no `source`, `ir_version` or `fields` key.

**OD-16 is invoked.** `schemas/capsule.schema.json` lacks `system_rev`, `capabilities` and the host variant. The single schema edit is task M3-03a (section 14), before any M3-03 or M3-04a record is validated. The edit adds those keys and makes `source`, `ir_version` and `fields` required only for the IR variant. Other keys are unchanged.

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

Slot order: for each group `g` in index order, the anchor slot then its 16 members in member index order; remainder rows (`N mod 17` after the groups; for N = 1,024 the remainder is 4, for 16,384 it is 13) are free units at the end, with `formation_slot = NONE` and their own attack-move order. Initial generations are 1. Anchors are written with `reload = 1, damage = 1, flight = 1, range_sq = 0, hp = 0` to satisfy 2.2.

Formation offsets: member `i` in a group has column `i mod 4` and row `i div 4`; `offset_x = col*32 - 48`, `offset_y = row*32 - 48`, stored as `formation_ox = offset + BIAS`.

Orders (N7): only the anchor receives the group order (mode 2, attack-move to the opposite band centre or the box centre). Members have `order_mode = 0` and follow anchor plus offset until a target is valid. Acceptance check (audit N-f): after phase 1 of tick 0 (after the commit of `rts.goal`, before `rts.combat`), each member with `anchor_valid = 1` has `goal = anchor + offset`, so formation-follow is exercised in every committed scene.

### 7.4 Weapon table (synthetic, not BFME)

| kind | range_sq | reload | flight | damage | speed | hp |
|---|---|---|---|---|---|---|
| rifle | 65536 (256) | 6 | 2 | 3 | 96 | 12 |
| bow | 262144 (512) | 10 | 5 | 5 | 64 | 10 |
| melee | 256 (16) | 2 | 1 | 2 | 128 | 20 |

The table is a generator convention. The control plane enforces the domains of 2.2 only, not this table (r3 change).

Team placement in `mixed.battle.v1`: team 0 x in `[1024, 8191]`, team 1 x in `[24576, 31743]`, y uniform in `[0, 32767]`. Congested: positions uniform in the central 2048 x 2048 box, orders attack-move to the box centre.

### 7.5 Committed lock and load path (F8)
- `bench/rts/scenes.lock.json` lists per scene: generator id, `params`, `seed`, `capacity`, the scene digest, and the schema digest. Regeneration must reproduce every digest; a mismatch fails the lock check (`bench/rts/gen/check_lock.py`, M3-01).
- Scenes up to 65,536 rows may be generated in memory by the Python generator and loaded in the test harness. Scenes larger than that are generated to a local, uncommitted row file (never committed; only spec and digest are committed) and loaded with `world.spawn_batch` in calls of at most 1,024 rows (OD-3b). The M3-05 harness refuses a load whose accepted row count differs from the generator's.
- Load-check: for N up to the snapshot cap (12, the 838,860 high-water precheck and the spool precheck of 9.3), the loaded state is read back through a snapshot capture and its columns are hashed with the 7.2 canonical encoding; the check must equal the lock digest (T-RTS-SCENE-2). Above that cap no full read-back path is specified in M3; the record says `scene_check: not_available` and reports the accepted row count and the per-call count only. Adding a bulk read path is OD-12.

## 8. Independent Python reference model

- Location: `bench/rts/ref/` (`model.py`, `ir_eval.py`, `isqrt.py`); tests in `tests/rts/ref/`. Python 3 standard library only; no import from the engine or from `profiles/rts/native/`.
- Written from this contract only. The author of `model.py` must not read the C operator sources (M3-04) before the model's golden outputs are fixed. A different agent or person reviews the model against the contract text (OD-13).
- Two parts:
  1. **Semantic model** of phases 0-5 and merge, written from sections 4 and 5 (not from the IR JSON). It is the oracle for scene digests and for targeting metrics (4.2). It is also the independent comparison for the state digest per tick (M3-M27 detector, section 15).
  2. **IR evaluator** (`ir_eval.py`): interprets only the committed IR 0.1 and 0.2 JSON with the M0/M2 opcode semantics. A differential test asserts that for random rows the evaluator output equals the semantic model's per-row outputs.
- Outputs per tick: per-phase field digests and the canonical state digest, using the same canonical layout as the engine (9.2).
- Oracle budget (N13, estimate, not measured): in CI, scenes up to 4,096 rows and up to 120 ticks. Larger runs record `oracle: not_run` with `reason: budget`. M3-02 measures the budget on the reference machine and fixes the numbers before M3-06.
- The exact brute-force oracle for targeting metrics (4.2) is part of this model; it scans all enemies, not the grid.

## 9. Replay format, digests and memory accounting

### 9.1 Replay file `rts-replay/0.1` (JSON Lines)
- Line 1, header: `{"format":"rts-replay","version":"0.1","numeric_profile":"rts_u32_v1","scene":{"generator","params","seed","capacity","scene_digest"},"schema_digest","system_manifest_hash","operator_manifest_hash","engine":{"artifact_hashes":[...],"source_revision"},"caps":{"max_struct_per_tick","max_events_per_tick","event_log_max","control_events_counted":false},"spool":{"checkpoint_retain","spool_bytes_max"},"checkpoint_every":32,"digest_every":K,"ticks":T}`.
- `operator_manifest_hash` covers the `rts_ops_v1` versions, constants and host descriptors (3.1, 4.4); `system_manifest_hash` covers the IR systems and the host capsule records.
- `caps.control_events_counted` is `false` (OD-3c, r3): control-plane records are excluded from `max_events_per_tick` (9.4). A replay with `true` for an RTS world is a configuration error.
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

| category | formula | estimate (per row unless stated) |
|---|---|---|
| columns | `capacity * 39 * 4 * 2` (committed and working roots) plus staging `capacity * 4 * 10` (combat writes 10 fields) | 312 + 40 = 352 bytes per row |
| entity table | `capacity * 12` (generation, alive, external id); exact layout from M2-00 | 12 bytes per row |
| spatial grid | `262144 * 4` counts + `262145 * 4` starts + `N_idx * 4` items | 2 MiB fixed + 4 bytes per indexed row |
| structural and event buffers | `min(chunk_size * per_row, per_tick)` per (system, chunk), admitted at create (su_cmdq.h) | 32 bytes per row, unmeasured estimate, for `rts.damage` only |
| event log | `event_log_max * 40` | 40 bytes per retained event (not per row) |
| checkpoint spool | `(checkpoint_retain + 1) * (446 * hw + 90 * retained_events + 16384)` reserved at admission | 446 per row and 90 per event: contract worst case, unmeasured (M3-08) |

**Per-row total, excluding the event log and the spool:** 352 + 12 + 4 + 32 = **400 bytes per row**, plus 2 MiB fixed. (r2 said "about 440"; that figure was the agent-internal spool bound and is not part of this total.)

**Spool note (audit N-b, F5):** `checkpoint_retain` = 1 for the RTS profile (the agent default is 4, `su_agent.c:57`). The create path holds the new checkpoint while the old one is retained, so the reservation is `(retain + 1)` times the bound. The RTS precheck (12) uses 446 per row and 90 per event, not the agent's internal 440 (`su_agent.c:978-984`), because 440 is below the contract worst case. With the 512 MiB binary spool, the precheck binds at about 588,000 rows (worst case, unmeasured): `2 * (446 N + 90 * 65,536) <= 536,870,912` gives `N <= 588,650`. The row guard (838,860) binds later. The 65,536-row recovery claim (12) fits the spool comfortably.

Cross-check: process `VmHWM` (from `/proc/self/status`) sampled at the end of each run and the allocator-level count where available. Any category that cannot be measured is `null` with a reason (C10). Exact per-root accounting is OD-10 (M3-08a). Until it lands, the harness reports the analytic figures and marks them as estimates.

### 9.4 Stall rule for RTS worlds (B, r3; replaces the r2 "no stall by construction" claim)

Definitions. The tick merge (`su_cmdq_commit`, `su_cmdq.c:641-644`) refuses when `n_events + n_cmds > max_events_per_tick`, where `n_cmds` is the number of structural requests of the tick and `n_events` the number of system-emitted events of the tick. Control-plane records are not in this count: `world.spawn` and `world.spawn_batch` append their `SU_EVENT_SPAWNED` records to the event log at the boundary (`worker.c:1072-1079`), and `world.despawn` appends its record there too. They never pass through the cmdq buffers.

Rule (OD-3c, r3). Control-plane records are excluded from `max_events_per_tick`. This is the existing code path, and the replay header records it (`caps.control_events_counted = false`).

Claim. For an RTS world with `max_struct_per_tick = max_events_per_tick = capacity` and the schedule of section 3 (the only structural producer is `rts.damage` with `max_struct_per_row = 1`; no system emits events, `max_events_per_row = 0`):
- `n_events = 0` for every tick;
- `n_cmds` is at most the number of alive rows at tick start, because each alive row produces at most one `despawn_self` per tick and dead rows produce none (dead rows are zeroed, 9.4 proof step 2);
- alive rows at tick start are at most `capacity`, so `n_cmds <= capacity = max_events_per_tick`.
Therefore the merge check never refuses in an RTS world, whatever the control-plane history. A spawn at the boundary adds rows that are alive at tick start, and it is counted in the alive bound, not in a second counter.

Proof step 2 (dead rows produce no requests): `rts.damage` runs only on alive rows (VM alive mask, section 1). A row that dies in the tick emits its one request in that tick; it cannot emit again in a later tick because it is dead then.

Consequence for the r2 F6 example (capacity 16, spawn 16, all die next tick): the 16 spawn records are control records and are not counted; the next tick has `n_cmds = 16 <= 16` and completes. Test T-RTS-CAPS-2.

Alternative not chosen (for the owner's record): count control records in the window and bound admission with `alive_after + pending_control_records_after <= capacity`. That keeps the same invariant but halves the spawnable count per window (16 spawns into an empty 16-slot world become impossible, since 16 + 16 > 16). Rejected because it cuts scene load capacity for no safety gain under the proof above.

Config check: M3-05 refuses `world.create` for an RTS profile whose `max_struct_per_tick` or `max_events_per_tick` is below capacity (M3-M28). Any configuration that breaks the claim (other producers, `max_struct_per_row > 1`, events from a system) is outside this claim and uses the general per-tick refusal with `SU_E_RESOURCE_LIMIT` (M2 rule).

## 10. Tests and acceptance

### 10.1 Reference-behaviour tests (per system, known answers from the Python model)
- `T-RTS-GOAL-*`: explicit order overrides formation; formation member follows anchor with offset; invalid anchor holds; clamp of wrapped negative and over-domain values to 0 and 32767.
- `T-RTS-MOVE-*`: arrival exactly at goal; step equals `min(distance, speed)` on each axis; mode 1 ignores target; mode 2 stops when `target_valid`; speed 0 never moves; no overshoot.
- `T-RTS-COMBAT-*`: fire at cooldown 0 with target; no fire with active projectile; arrival at `t + flight` exactly (flight 1 and flight 5 edges); re-fire period `max(reload, flight + 1)` with the three known answers of 5.2 (reload 6 flight 2; reload 1 flight 1; reload 2 flight 5); cooldown never underflows; `incoming` zeroed each tick.
- `T-RTS-DAMAGE-*`: partial damage; exact kill with `incoming == hp` (kill emitted once); overkill saturates; anchors never killed; kill emits exactly one `SU_EVENT_DESPAWNED` at the boundary.
- `T-RTS-SPATIAL-*`: nearest-enemy by `(dist_sq, slot)` with ties for queries whose budget is not exhausted (`truncated = 0`); dist exactly equal to `range_sq` qualifies, `range_sq + 1` does not; cell boundary at x = 63/64 and y = 63/64 and 255/256; box clipped at domain edges (0 and 32767); self excluded; same-team excluded; dead excluded; truncation at exactly K = 128 examined and at K + 1 with the flag (box_total = 128 gives 0; box_total = 129 gives 1); a generation mismatch reported as invalid gather; ring order fixture: the querier's ring-0 cell holds 200 friendly entries and a single enemy sits in ring 1 within range. **Exact expected outcome (audit N-i):** `target_valid = 0` and `truncated = 1`. The ring-0 scan spends all K = 128 examinations on friendly entries (and self), so the ring-1 enemy is never examined, and `box_total > 128` makes `truncated = 1`.
- `T-RTS-GATHER-*`: stale anchor (generation mismatch, despawned, out of range, NONE) yields `anchor_valid = 0`; zero-value legitimate positions are distinguishable through the flag.
- `T-RTS-SCATTER-*`: saturation at the operator level: two arrivals of `0xFFFFFFF0` leave `incoming = 0xFFFFFFFF` (wrapping add would give `0xFFFFFFE0`); saturated lethal case kills exactly once; dead target ignored; stale generation ignored; non-unit target ignored; shooter despawned before arrival causes no damage; multiple shooters on one target summed.

### 10.2 Edge fixtures (`duel.fixture.v1`, exact expected outcomes from the model)
A minimal set: two units at distance exactly `range_sq`; one unit at x=0 and one at x=32767; equal-distance candidates (lower slot wins); a bow with `flight = 5` against a target that dies in flight; a target with `hp = 3` hit by two shooters in one tick (killed once, incoming 6); a formation whose anchor is despawned mid-run; slot reuse after despawn with a stale projectile target (the stale shot must not hit the new occupant).

### 10.3 Scene, load, control and replay tests
- `T-RTS-SCENE-1`: generator run twice gives identical bytes and digests; digest changes when any single PRNG constant or one field value changes (mutation).
- `T-RTS-SCENE-2`: for each committed scene up to the cap of 12 (838,860 high-water and the spool precheck), the engine-loaded state, read back through a snapshot and hashed with the 7.2 encoding, equals the lock digest.
- `T-RTS-LOAD-1` (C, F4; r3 definitions):
  - **"Maximum row bytes"** (the byte bound used for every size claim): every row names the 39 fields and `external_id`, every value is a u32 written with 10 decimal digits (4294967295), compact JSON with no whitespace. Measured by the generator as 925 bytes per row (`{"values":{...39 pairs...},"external_id":4294967295}` plus one separator). For 1,024 rows the array is 948,225 bytes.
  - (a) **Accepted near-maximum request:** 1,024 domain-valid rows, each with the longest domain-valid value per field (positions 32767, speed 1024, range_sq 262144, reload 1024, damage 65535, flight 64, `formation_gen` 4294967295, `external_id` 4294967295, idle system fields 0 or NONE), request line at most 1,048,576 bytes. Expected: `SU_OK`, 1,024 rows, one revision bump, one recovery entry, 1,024 `SU_EVENT_SPAWNED` records (dropped beyond `event_log_max`).
  - (b) **Byte refusal:** the same request padded with spaces to 1,048,577 bytes is refused with `SU_E_RESOURCE_LIMIT` by the request-size check (`su_proto.c`, before row parsing), no state change.
  - (c) **Boundary:** a request line of exactly 1,048,576 bytes passes the size check (its refusal, if any, is a domain code, never `SU_E_RESOURCE_LIMIT` from size).
  - (d) **Bound check:** 1,024 rows in the maximum-row-bytes form are at most 948,225 bytes (plus envelope, under 100 KB), so no valid 1,024-row call can hit (b) without padding.
  - (e) **Scale:** a 1,000,000-row load is 977 calls; each call is 1,024 rows except the last; the event count matches the M2 spawn rule (one `SU_EVENT_SPAWNED` per row, with `dropped` for rows beyond `event_log_max`).
  - (f) **High-water precheck (N-c):** a spawn batch that would push `hw` above 838,860 is refused with `SU_E_RESOURCE_LIMIT` with no state change; capture at 838,860 rows succeeds (M3-M31).
- `T-RTS-CTRL-1` (D, N11): `set_fields` with 1,024 entities, each with the 14 writable fields at maximum domain values and a 20-digit handle (at most 354 bytes per entity, 362,496 bytes per call), is accepted and gives one revision bump; a stale handle among the entities refuses the whole call with `SU_E_STALE_HANDLE`; a duplicate entity refuses with `SU_E_INVALID_ARGUMENT`; writes to `kind`, `team`, `hp` or any system-owned field refuse with `SU_E_TYPE`; an unknown field name refuses with `SU_E_SCHEMA_MISMATCH`; each domain of 2.2 has a refused value at its boundary + 1 (and 0 where 0 is outside): `flight = 0` and `flight = 65` refuse with `SU_E_TYPE` in `spawn`, `spawn_batch` and `set_fields`; `reload = 0`; `damage = 0`; `order_mode = 3`; `formation_gen != 0` with `formation_slot = NONE`; `kind = 1` with `hp = 0` refuses with `SU_E_TYPE` at spawn; `kind = 2` with `hp != 0` or `range_sq != 0` refuses; a stale `expected_world_revision` refuses with `SU_E_STALE_REVISION` (M3-M19).
- `T-RTS-REPLAY-1`: record then replay gives identical digest records; replay with tick workers 1 and 2 identical.
- `T-RTS-REPLAY-2`: a single flipped command byte, or a dropped `world.set_fields`, reports the first divergent tick; a changed `operator_manifest_hash` fails closed; `caps.control_events_counted = true` on an RTS world fails closed.
- `T-RTS-REPLAY-3` (N9): kill the worker mid-run, recover from checkpoint plus log in a replacement worker, continue, and compare digests with an uninterrupted run; recovery replays at most `checkpoint_every` = 32 ticks. Fault seams inside `rts.spatial` and `rts.scatter` return `SU_E_INTERNAL`, and each is recovered the same way (AGENTS.md: in-process native faults require checkpoint recovery, not pointer rollback).
- `T-RTS-CHURN-1`: spawn and despawn churn of at least 100,000 operations on a 16k scene with reference model comparison (inherits M2 churn test on RTS fields).
- `T-RTS-CAPS-1` (B, F6): with an explicitly smaller configured `max_struct_per_tick`, deaths in one tick above it abort the tick with `SU_E_RESOURCE_LIMIT`, state unchanged (M2 rule), and the replay records the configured cap. With the RTS setting (`= capacity`), a tick where every alive unit dies completes.
- `T-RTS-CAPS-2` (B, r3): spawn to capacity, then kill every row in the next tick, must complete. Setup: an empty RTS world of capacity 16 with `max_struct = max_events = 16`; `spawn_batch` of 16 rows (accepted; 16 control records appended to the event log, not counted); one tick in which all 16 units die (`n_cmds = 16`, `n_events = 0`). Expected: the tick commits, 16 `SU_EVENT_DESPAWNED` records, `dropped` correct if `event_log_max` is small. Repeat with `event_log_max` = 1 and with `SUTEKH_TICK_WORKERS` = 2. Also: spawn 16, kill 8, spawn 8, kill 16 across two ticks completes.

### 10.4 Verifier and IR negatives (M3-03)
Each code path: missing store (`SU_E_MISSING_STORE`), undeclared write (`SU_E_UNDECLARED_WRITE`), undeclared read (`SU_E_UNDECLARED_READ`), 0.2 op in a 0.1 module (`SU_E_UNKNOWN_OPCODE`), `despawn_self` count above `max_struct_per_row` (`SU_E_RESOURCE_LIMIT`), `emit_event` absent, `ir_version` mismatch (`SU_E_SCHEMA_MISMATCH`), duplicate register (`SU_E_DUPLICATE_REGISTER`), undefined register (`SU_E_UNDEFINED_REGISTER`), native lowering refused for 0.2 (`SU_E_CAPABILITY_DENIED`), and a non-bool condition in `select` (a raw u32 used as the selector; N2). Capsule schedule keys: an `after` edge to a later or unknown system is refused (`worker.c` schedule-order check).

### 10.5 Agent-authored behaviours (M3-07)
Section 11.

### 10.6 Performance, stress and targeting metrics (M3-06, M3-08)
- Targeting metrics (4.2) are reported for every congested scene at 1,024 and 16,384 rows over 1,024 seeded queriers, with the exact oracle (the same count as 4.2). Gates: `missed <= 0.1%`, `valid_but_not_nearest <= 2%`, ratio p95 `<= 3.0`, with the thin-margin rule of 4.2.
- No timing targets are asserted. Tests check that output files exist, are schema-valid, and carry hardware, OS, toolchain, source and artifact hashes (C10).

### 10.7 Host-entry tests (M3-04a, T-HOST, r3)
Run in a targeted build of the runtime; the existing scheduler, worker and entity tests are run unchanged (`tests/scheduler/test_scheduler.c`, `tests/scheduler/test_tick_workers.c`, `tests/entities/test_entity_tick.c`, `tests/reload/`). New tests in `tests/scheduler/test_host_entry.c` and `tests/rts/host/` use test-only descriptors.
- **T-HOST-01** IR entries unchanged: the existing scheduler, tick-worker, entity-tick and reload tests pass unchanged after the change.
- **T-HOST-02** Dispatch and commit: `su_sched_run_ticks` over `[IR, host, IR]` runs the host `run` once per tick, in index order; the committed state contains only the declared writes; a dead row written by the host is zeroed after its entry (`su_world_zero_dead`).
- **T-HOST-03** Failure: a host `run` that returns `SU_E_INTERNAL` (injected) aborts the tick, `failed_index` is the host index, the committed root is unchanged.
- **T-HOST-04** Declared effects are used: an entry with `host_declared = 1` that declares a write to field F, and an IR entry in the same phase that writes F with no ordering edge, makes `su_effects_build` return `SU_E_SCHEMA_MISMATCH`; with the edge, `SU_OK`. An IR entry with `host_declared = 0` and a declared/derived mismatch is still refused (`su_effects_check_declared` unchanged). Mutant M3-M29 (host effects derived as empty) must fail this test.
- **T-HOST-05** Load checks: a host capsule whose `host.fields` differs from the descriptor (extra write, missing read, wrong access), whose `constants_hash` or `operator_version` differs, or whose `system_id` differs, is refused at world load with `SU_E_SCHEMA_MISMATCH` (or `SU_E_INVALID_ARGUMENT` for the id); the world is not created. A capsule without a `schedule` block is refused. The M3-04 production descriptors must equal the M3-04a capsule records (checked in M3-04).
- **T-HOST-06** Candidate refusal: a candidate module whose `system_id` names a host entry is refused by `su_sched_check_candidate` and by the reload path with `SU_E_CAPABILITY_DENIED`; the old entry is unchanged and still runs.
- **T-HOST-07** Native refusal: a native lowering request for a host entry returns `SU_E_CAPABILITY_DENIED` before `mod` is read (built under `SU_HAVE_NATIVE`; the existing `tests/native/qualification/test_native_chunked.c` build is the targeted compile).
- **T-HOST-08** No VM context: a host entry has `ctx == NULL` and `mod->insn_count == 0`; `su_sched_entry_destroy` frees the declaration with no double free (ASan build).
- **T-HOST-09** Entity-op schedule: a schedule containing an IR 0.2 entry (`uses_entity_ops`) and a host entry runs both through `su_ent_run_ticks`; the host entry runs through `su_sched_run_system` and gets no per-chunk entity context.
- **T-HOST-10** Workers: the T-HOST-02 schedule gives identical digests with `workers` 1 and 2 (the host entry is serial).

---

## 11. Agent-authored behaviours evidence plan (M3-07)

Three behaviour tasks, each to be done by an agent through the existing candidate pipeline (verifier, reference tests, `world.candidate` path; AGENT_PROTOCOL), with hidden acceptance fixtures written by a different author from the Python model. All three are IR 0.1 changes to `rts.goal`, `rts.combat` and `rts.move`, and each fixture fails on the r2 baseline (F9):

- **B1 low-hp retreat**: a unit with `hp <= 3` retreats to its team's base point and does not fire. Write scope: `rts.goal` (goal toward base), `rts.combat` (no fire gate on `hp > 3`), `rts.move` (moving when retreating even with `target_valid`). Extra declared reads: `goal` reads `hp` and `team`; `combat` reads `hp`; `move` reads `hp`. Base points are constants per team (team 0 at x = 2048, team 1 at x = 30720, y = 16384). Retreat overrides explicit orders. Baseline fixture: team 0 unit, `hp = 3`, at (4000, 16384), rifle; team 1 unit, `hp = 12`, at (4200, 16384). Baseline: the unit does not move (valid target) and fires. Expected: it moves 96 units toward x = 2048 and does not fire. Edges needed: `rts.damage` after `rts.goal` and `rts.move` (3.2), since both now read `hp`, which damage writes.
- **B2 kiting**: a unit with `cooldown > 0` and a valid target keeps moving toward its goal instead of stopping. Write scope: `rts.move` (stop gate becomes "target valid and cooldown = 0"); extra declared read: `move` reads `cooldown`. Baseline fixture: rifle unit with `cooldown = 4`, a valid target at distance 200, mode 2 order 2000 units away. Baseline: no movement. Expected: moves `speed` (96) per tick. The old r1 projectile retarget guard is removed: scatter already rejects a target whose generation changed (4.3, T-RTS-SCATTER), so it is base behaviour and has no failing baseline. Edge `combat` before `move` already exists (3.2).
- **B3 formation catch-up**: a formation member (`anchor_valid = 1`, mode 0) farther than 64 units from its goal moves at twice `speed`, capped at the remaining distance. Write scope: `rts.move`; extra declared read: `move` reads `anchor_valid`. Baseline fixture: member at distance 200 from goal, speed 96. Baseline: moves 96. Expected: moves 192. The r1 "formation hold" rule is removed: the r1 rules already hold when `anchor_valid = 0` and no order (`goal = pos`), so it had no failing baseline. Edge `spatial` before `move` already exists (3.2).

**Candidate order (audit N-d): B3, then B2, then B1.** B3 and B2 are the smallest changes and both touch only `rts.move`. B1 changes three systems and adds the `hp` reads that need the new edges, so it is last. Rules for every candidate:
- Each behaviour's fixture must fail on the r2 baseline **and** on the last accepted candidate (so B2's fixture must still fail with B3 accepted, and B1's with B2 and B3 accepted).
- Each accepted candidate is re-run against the fixtures of every earlier behaviour and the full existing suite.
- The candidate's declared sets and new edges pass `su_effects_build` (3.2); a candidate that adds an unordered conflict is refused by that check.

Evidence record per task (JSON, stored in `bench/rts/agent_experiments/records/`): task id, agent and session identifier, model identifier, number of candidate submissions, verifier rejections by code, reference test failures by fixture id, hidden fixture result, wall time to accept (or failure), native-lane status (not applicable, as all three remain IR 0.1), null tokens and tool calls with reason `not_recorded` if the harness did not record them. Acceptance: the candidate passes hidden fixtures and the full existing suite. The record is not a claim of general agent capability. Behaviour set change is OD-19.

## 12. Stress curves and memory methodology (M3-08)

- Harness: `bench/rts/stress/`. Sweep scene sizes over a geometric series of powers of two starting at 1,024 (sweep points are a methodology choice, not a unit-count claim). For each size, run `T` ticks (default 120) of `mixed.battle.v1` and of `congested.blob.v1`.
- **Admission prechecks (M3-05, r3):** before admitting a `world.spawn` or `world.spawn_batch`, the handler computes `hw_after` conservatively as `hw + rows` (`su_ents_high_water`, `su_ents.c:249`) and refuses with `SU_E_RESOURCE_LIMIT`, no state change, if either:
  - `hw_after > 838,860` (the snapshot row guard, so periodic capture is never refused by it; audit N-c), or
  - `(checkpoint_retain + 1) * (446 * hw_after + 90 * event_log_max) + 16384 > spool_bytes_max` (spool, 9.3).
  A capture refused by the row guard after this precheck is an invariant break and is reported as `SU_E_INTERNAL` (a finding, not a limit).
- Stop rule per curve: stop at the first failing limit, which is recorded by name:
  - `capacity` (`SU_LIMIT_ROWS`, default 1,000,000; RTS capacity must also be <= 1,048,576, F6);
  - `tick_cap` (`SU_E_RESOURCE_LIMIT` from `max_struct_per_tick` or `max_events_per_tick`; by 9.4 this should never fire for RTS worlds, so any occurrence is a finding and a bug report, not a memory limit);
  - `operator_buffer` (`SU_E_RESOURCE_LIMIT` from an operator's admitted buffers);
  - `snapshot_cap` (`SU_E_RESOURCE_LIMIT` from the admission precheck of 12, or from capture at 64 MiB output);
  - `spool_budget` (`SU_E_RESOURCE_LIMIT` from the spool precheck, `spool_bytes_max`);
  - `memory_ceiling` (operator-set);
  - `wall_clock_ceiling` (operator-set, per run).
  Record the largest size completed and the first failing size with its named limit and code. No size is predeclared as solved.
- Checkpointing during the sweep: a checkpoint every `checkpoint_every` = 32 ticks, so the spool and snapshot limits are exercised within a 120-tick run.
- Per tick, record per-phase monotonic nanoseconds (spatial sub-steps a-d, goal, combat, move, scatter, damage, merge, digest when taken). Per run: p50, p95, p99 and max over ticks, with sample counts. Also record the truncation fraction, the targeting metrics of 10.6, deaths per tick, projectiles per tick, and the configured `max_struct_per_tick` and `max_events_per_tick`.
- Hardware and environment block: CPU model, vCPU count, memory, OS, compiler versions, build flags, `SUTEKH_TICK_WORKERS`, artifact hashes. Measurement mode `dev_checked` or `perf_trusted` is stated explicitly. Shared VM timings are observations (C10).
- Memory: section 9.3 analytic figures plus `VmHWM` per run; differences between the two are reported, not hidden.
- Recovery claim (F5): checkpoint recovery is claimed for N <= 65,536 only, with the binary spool default (512 MiB) or with a library spool that holds the measured checkpoint (the library default is 32 MiB and does not hold a 65,536-row checkpoint with a full event log). N = 262,144 is claimed only if the measured canonical bytes per row (including the event log) give a checkpoint of at most half the spool, and at most 64 MiB. Above 838,860 rows the admission precheck refuses; 1M is not claimed. A larger claim needs OD-15.
- Actual limits found are recorded as data: configured `SU_LIMIT_ROWS`, capacity used, per-tick caps, and the largest completed size.

## 13. Interface changes and owner decisions

| id | item | change class | owner |
|---|---|---|---|
| OD-1 | Host-side operators (`rts_ops_v1`) instead of IR gather/scatter (D2) | design | orchestrator plus M3 owner |
| OD-1b | Additive host-entry kind in `schedule.h` (3.1): `SU_SYS_KIND_*`, `SuHostField`, `SuHostOpDesc`, `su_sched_add_host`, `SuSysEntry.kind/host`; declaration module in `src/runtime/rts/host_decl.c` (no `su_lir.h` change); `SuEffectInput.host_declared` (`su_effects.*`); dispatch branch; candidate and native refusal | shared contract (runtime header) and runtime sources | scheduler/runtime owner |
| OD-2 | No IR opcode additions in M3 (D1); IR 0.3 proposal only if 6.4 triggers | design | IR owner |
| OD-3 | `world.set_fields {world_id, expected_world_revision, entities:[{entity, values}]}`: at most 1,024 entities per call; `values` restricted to the 14 writable fields of 2.2; `kind`, `team`, `hp` and system-owned fields rejected with `SU_E_TYPE`; duplicate or stale entity refuses the whole call; one revision check and one recovery-log entry per call; emits no event | shared contract (new control method) | ABI/control owner |
| OD-3b | `world.spawn_batch {world_id, expected_world_revision, rows:[{values, external_id?}]}`: at most 1,024 rows per call; `external_id` is a u32 (as `world.spawn`); request line at most 1,048,576 bytes (`su_proto.h:5`), checked before row parsing; one recovery-log entry per call; slots in given order; one `SU_EVENT_SPAWNED` per row, appended to the event log outside the tick merge (M2 rule, `worker.c:1077`) | shared contract (new control method) | ABI/control owner |
| OD-3c | Whether control-plane records count toward `max_events_per_tick`. **r3 proposal: excluded** (9.4). The r2 "required" rule (counted) is withdrawn because it breaks the stall-freedom claim with no safety gain under the 9.4 proof. Recorded in the replay header (`caps.control_events_counted = false`). Alternative (pending-record admission bound) recorded in 9.4 and not chosen | shared contract (runtime) | runtime/control owner (approval required) |
| OD-4 | `SU_SCHED_MAX_SYSTEMS` stays 8; M3 uses 6 | none | - |
| OD-5 | Declared-and-tested (not verified) effects for host operators (4.4) | design risk accepted | owner |
| OD-6 | Query grid 64-unit cells (512 x 512), K = 128, rc <= 8, ring order, `truncated = [box_total > K]`; gates of 4.2 with the thin-margin rule | profile constant | RTS profile owner |
| OD-7 | No separation, avoidance, pathfinding or obstacle map in M3 | scope | owner |
| OD-8 | One projectile per shooter with fixed flight time | scope | owner |
| OD-9 | `rts_u32_v1` numeric profile and 15-bit domain; control domains of 2.2, including the proposed `hp` bound 1..65535 for kind 1 and `flight` 1..64 for every row | profile | owner |
| OD-10 | Exact per-root memory accounting API in `src/world` (for 9.3), M3-08a | native_core | ABI/world owner |
| OD-11 | Native lane for goal/combat/move (M3-10), gated on M1 owner acceptance | native_core, later | owner |
| OD-12 | Bulk scene read-back above the snapshot cap; spawn_batch is the only load path in M3 | scope, later | owner |
| OD-13 | Whether a Python model written by a different agent under the same orchestrator counts as independent | process | owner |
| OD-14 | Scatter and spatial remain serial in M3 | scope | owner |
| OD-15 | Snapshot limit: accept the N <= 65,536 recovery claim (F5), or raise `SU_SNAPSHOT_MAX_BYTES` or the row guard, or adopt a binary snapshot encoding (new format, not in this contract). Note: with `retain = 1` and 512 MiB the spool precheck binds first (about 588,000 rows, 9.3) | shared contract (`src/world/snapshot.h`) | snapshot owner |
| OD-16 | Capsule schema change, invoked in r3: `system_rev`, `capabilities`, and the host variant (5.5); single writer M3-03a | shared schema | schema owner |
| OD-17 | Replay header 0.1 additions (`checkpoint_every`, `operator_manifest_hash`, `dropped` count, `caps.control_events_counted`, `spool`) | file format | replay owner |
| OD-18 | `checkpoint_every` = 32 ticks as an RTS profile constant (N9) | profile constant | RTS profile owner |
| OD-19 | Redefinition of M3-07 behaviours B2 and B3, removal of r1 B2 and B3 (F9); candidate order B3, B2, B1 (11) | scope | owner |

Profile errors reuse existing codes: `SU_E_INVALID_ARGUMENT` (malformed scene or command, duplicate entity in a call), `SU_E_TYPE` (out-of-domain value, `kind = 0` on live row, `kind = 1` with `hp = 0`, `kind = 2` with `hp` or `range_sq` nonzero, immutable or system-owned field write, `range_sq > 262144`, idle-value violation), `SU_E_RESOURCE_LIMIT` (capacity, per-tick caps, operator buffers, request size, high-water precheck, spool precheck), `SU_E_SCHEMA_MISMATCH` (generator schema digest differs from loaded schema, unknown field name, host capsule field table differs from the descriptor), `SU_E_STALE_HANDLE` and `SU_E_STALE_REVISION` (control commands), `SU_E_CAPABILITY_DENIED` (host entry replaced by a candidate or lowered to native), `SU_E_INTERNAL` (invariant violated inside an operator, including `rc > 8`, and a capture refused after the precheck). No new status codes are proposed; a new code would require a `status.h` change and owner approval.

## 14. Task split (proposed replacement for the single M3-00 entry)

Note for the orchestrator (not an edit to `planning/tasks.json`, which this contract does not change): the current `M3-00` entry is broad ("Build synthetic RTS microcosm"). Proposed: keep `M3-00` as this contract's freeze and split the rest as below. Every task is `status: planned` until the owner accepts this revision.

| id | title | depends_on | write scope (lease) | change class | native validation | acceptance (summary) |
|---|---|---|---|---|---|---|
| M3-00 | Contract r3 freeze (this file) | M2-01 (design only) | `planning/contracts/M3_RTS_CONTRACT.md` | design | no | owner accepts or rejects OD-1 to OD-19 |
| M3-01 | Schema, Python scene generator (single normative), scene lock | M3-00 | `profiles/rts/schema/`, `bench/rts/gen/`, `bench/rts/scenes.lock.json` | profile | no | generator reproducible; digests stable; PRNG and field-order mutants change digests; remainder rule (7.3); anchor idle values (7.3) |
| M3-02 | Independent Python reference model, IR evaluator, targeting oracle | M3-00 | `bench/rts/ref/`, `tests/rts/ref/` | profile | no | 10.1 known answers (including the exact ring fixture of 10.1); IR-vs-model differential; oracle budget (8); targeting metrics (4.2) at 1,024 queriers; model author does not read M3-04 sources |
| M3-03a | Capsule schema edit (OD-16): `system_rev`, `capabilities`, host variant (5.5) | M3-00 and OD-16 approval | `schemas/capsule.schema.json` only | shared_schema | no | existing IR capsules still validate unchanged; IR and host variants validate; an unknown top-level key is refused; no other key changes |
| M3-03 | IR systems goal/combat/move/damage, authoring generator, IR capsule records | M3-01, M3-02, M3-03a, M2-01 (IR 0.2 verifier) | `capsules/rts/` (IR records only), `bench/rts/gen_ir.py` | gameplay | no (0.1 systems are native-eligible but native is not run here) | 10.4 negatives; regenerate-equality; measured instruction counts recorded; 5.5 record fields and 3.2 `after` lists |
| M3-04a | Scheduler host-entry kind (OD-1b) and host capsule records | M3-00, OD-1b approval, M3-03a | `src/runtime/schedule.h`, `src/runtime/schedule.c`, `src/runtime/jobs/su_effects.h`, `src/runtime/jobs/su_effects.c`, `src/runtime/worker.c` (host loader branch in the world schedule loader and the `host_declared` line in `check_system_effects` only), `src/runtime/native_versions.c` (refusal guard only), `src/runtime/rts/host_decl.c` and `src/runtime/rts/host_decl.h` (new), `capsules/rts/host/` (2 new records, `spatial` and `scatter`), `tests/scheduler/test_host_entry.c` (new), `tests/rts/host/` (new) | shared_contract, native_core | yes (targeted compile of the runtime with and without `SU_HAVE_NATIVE`; existing scheduler, tick-worker, entity-tick and reload tests; T-HOST-01..10) | IR entries unchanged; host dispatch, declared-set use in effects, load checks, candidate and native refusal, no VM context for host entries |
| M3-04 | Host operators: spatial index, gather, scatter (C17); production descriptors | M3-01, M3-02, M3-04a | `profiles/rts/native/`, `tests/rts/native/` | native_core | yes | 10.1 SPATIAL, GATHER, SCATTER; ASan, UBSan; instrumented store-set check (4.4); fault seams (N9); descriptors equal the M3-04a host records (T-HOST-05). Depends on M3-02 known answers, not on M3-03 |
| M3-05 | `world.set_fields`, `world.spawn_batch`, admission prechecks (12), replay record and replay, checkpoint recovery | M3-03, M3-04, M3-04a, OD-3, OD-3b, OD-3c approved | `src/agent/su_agent.c` (method handlers only), `schemas/` (no capsule schema), `docs/AGENT_RPC.md` (with approval), `tests/rts/replay/`, `bench/rts/load/` | schema/runtime | yes | T-RTS-LOAD-1 (a)-(f), T-RTS-CTRL-1, T-RTS-REPLAY-1..3, T-RTS-CAPS-1, T-RTS-CAPS-2; the high-water and spool precheck; config check (9.4); recovery-log entries for the two new methods; OD-3c rule (exclusion) implemented and recorded |
| M3-06 | End-to-end suite, golden scenes, edge fixtures, mutation campaign, targeting gates | M3-02, M3-05 | `tests/rts/e2e/`, `tests/rts/fixtures/` | tests | yes (with engine runs) | C == Python digests for committed scenes within oracle limits; 10.6 gates with the thin-margin rule; all listed mutants detected or recorded as equivalent; M3-M27 detected by the per-tick model comparison (15) |
| M3-07 | Agent-authored behaviours experiment (B1-B3, redefined) | M3-06 | `bench/rts/agent_experiments/`; candidates only through the candidate pipeline (no direct edits to `capsules/rts/` on the canonical branch) | evidence | no | records per section 11; hidden fixtures authored by a different party; candidate order B3, B2, B1; each fixture fails on the baseline and on the last accepted candidate |
| M3-08 | Stress curves and memory methodology | M3-06; M3-08a for exact accounting | `bench/rts/stress/` | evidence | no | raw logs with hashes, hardware blocks, named limits, null reasons; no predeclared unit-count claims; recovery claim per 12 |
| M3-08a | Exact per-root memory accounting (OD-10) | owner approval | `src/world/` (accounting API only) | native_core | yes | per-category bytes match the 9.3 table within stated tolerance; optional: without it, 9.3 figures stay estimates |
| M3-09 | Independent review of any performance optimization | M3-08 and the optimization | reviewer write-free | review | per change | C10 evidence present; reviewer differs from author; digests unchanged |
| M3-10 | Native lane for goal/combat/move (optional) | M1 owner acceptance, M3-03, M3-06 | `backends/native/` (lease) | native_core | yes | differential vs reference VM across row tails and dead rows; capability fallback |

Notes on the split (r3):
- The only writer of `schemas/capsule.schema.json` for the M3 capsule keys is M3-03a. M3-03 and M3-04a depend on it. M3-04 does not depend on M3-03.
- M3-04a writes the two host capsule records; M3-03 writes only IR records. The `capsules/rts/` lease is split by subdirectory (`capsules/rts/host/` for M3-04a).
- M3-05 has no lease on `src/runtime/`; the OD-3c exclusion is already the existing code path, so M3-05 adds no tick-merge counter.

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
| M3-M27 | `proj_target_gen` not cleared on arrival | **Detected by the Python-model comparison of M3-06** (per-tick state digest against the independent model, 8). Not by T-RTS-REPLAY-1, which replays the same (mutated) tree on both sides and so agrees with itself (audit N-g) |
| M3-M28 | per-tick cap set below capacity for an RTS world | T-RTS-CAPS-1 with the RTS setting (all units die in one tick); config check in M3-05 (9.4) |
| M3-M29 | host entry effects derived as empty (declared set ignored by `su_effects_build`) | T-HOST-04 (a host write conflicts with an unordered IR entry and must be refused) |
| M3-M30 | control domain check missing for `flight` (0 accepted) or a system-owned field accepted by `set_fields` | T-RTS-CTRL-1 domain and writable-set cases |
| M3-M31 | spawn admission without the high-water precheck (`hw_after > 838,860` accepted) | T-RTS-LOAD-1 (f) |

Each mutant is applied to a copy of the tree, and the named test must fail. An unrun mutant is listed as unrun, not as passing. Equivalent mutants are listed as equivalent, not as detected.

## 16. Risks

1. **Per-tick caps at scale.** Mass deaths cannot exceed alive rows per tick, and alive rows cannot exceed capacity; control records are outside the tick merge (9.4). The cap is still a configured value that the stress run records; it must not be raised silently, and capacity above 1,048,576 is out of scope.
2. **Host operators carry gameplay logic.** The spatial query, gather and scatter are profile code. Their effects are declared and tested, not verified (OD-5). A later verifier extension is the fix; it is not claimed here.
3. **Approximate target under congestion.** Truncation makes targeting depend on budget and ring order. This is an accepted profile semantic (OD-6). Measured with the metrics of 4.2 and gated; not described as exact nearest-neighbour behaviour. The probe numbers in section R.2 are from a non-generator scene and are not a claim about the committed scenes.
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
14. **Snapshot and spool limits.** Recovery claims stop at N = 65,536 (F5). The high-water precheck (838,860) and the spool precheck (about 588,000 rows at default settings, worst-case estimate) bind before 1M. Changing either is OD-15.
15. **Host event accounting.** Resolved in the r3 proposal by OD-3c "excluded" (9.4), which needs owner approval. If the owner rejects it, the pending-record admission bound in 9.4 is the fallback and must be implemented in M3-05 with its test before M3-05 closes.
16. **Host capsule schema.** The host variant depends on OD-16 (M3-03a). Until that task lands, M3-04a cannot validate its records; this is a sequencing risk, not a design risk.

## 17. Owner decisions requested

1. Accept D1 (no new IR opcodes for M3) and the re-evaluation trigger in 6.4 (OD-2).
2. Accept D2 (host operators `rts_ops_v1`) and the declared-and-tested effect model (OD-1, OD-5).
3. Approve the additive host-entry kind in `src/runtime/schedule.h` and the `SuEffectInput.host_declared` change in `src/runtime/jobs/su_effects.*`, with the mechanism of 3.1 and its rejected alternatives (OD-1b, F1, audit A).
4. Approve `world.set_fields` (entities batch, 1,024 per call, writable set of 2.2) and `world.spawn_batch` (1,024 rows per call, u32 `external_id`, 1,048,576-byte request cap), each with recovery-log entries (OD-3, OD-3b, F4, F7, audit C and D).
5. Decide the boundary event-accounting rule (OD-3c): the r3 proposal excludes control-plane records from `max_events_per_tick` (9.4). The alternative is the pending-record admission bound, which halves spawn throughput per window (9.4). The r2 "counted" rule is withdrawn (audit B).
6. Approve the profile constants: 64-unit cells, 512 x 512 grid, K = 128, rc <= 8, ring order, `rts_u32_v1` domain, the 2.2 control domains (including the proposed `hp` bound 65535 for units), and the targeting gates of 4.2 with the thin-margin rule (OD-6, OD-9, F2).
7. Approve saturating add for `incoming` (D5, F3).
8. Decide the snapshot limit: accept the N <= 65,536 recovery claim, or fund OD-15. Note that the spool precheck binds first at default settings (9.3).
9. Confirm scope exclusions: no separation, pathfinding, obstacles, multiple projectiles per shooter (OD-7, OD-8).
10. Decide whether exact memory accounting (OD-10, M3-08a) lands before M3-08 or whether analytic figures are reported as estimates until then.
11. Decide whether M3-10 native work is in scope for M3 closure (OD-11) or deferred.
12. Confirm whether the M3 Python model may be written by a different agent in the same orchestration (OD-13).
13. Confirm the single Python generator and the 1,024-row load path (D10, OD-12, F8), and checkpoint cadence (OD-18).
14. Approve the redefined M3-07 behaviours B1, B2 and B3, the removal of the r1 B2 and B3 (OD-19, F9), and the candidate order B3, B2, B1 (11).
15. Approve the capsule schema change (OD-16, M3-03a): `system_rev`, `capabilities`, and the host variant (5.5).
16. Approve the replay header additions (OD-17): `checkpoint_every`, `operator_manifest_hash`, `dropped`, `caps.control_events_counted`, `spool`.
