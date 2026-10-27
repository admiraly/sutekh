<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# space3d: deterministic 3D queries (`space3d_ops_v1`)

Status: implemented and tested (2026-10-18; amendment `planning/amendments/2026-10-18-numeric-3d.md`). Code: `profiles/space3d/native/` (`space3d_geom.c` narrow phase, `space3d_ops.c` operator). Numeric semantics: [NUMERIC_FIXED_Q32.md](NUMERIC_FIXED_Q32.md).

## 1. Capability model

Swept collision and gate crossing need cross-row reads, which IR 0.x does not have. They are therefore a **host operator** (the M3 mechanism used by `rts.spatial`): a phase system compiled into the engine whose effects are a declared field table. The declaration is checked twice: the host capsule record (`profiles/space3d/space3d_query.capsule.json`) must equal the descriptor exactly (operator `space3d_ops_v1`, constants hash, every lane's ID, name, type and access; `su_host_load`), and the schedule's union table requires every IR system that binds one of those lanes to agree on its ID, name, storage type and **value type** (`q32` lanes are q32 for every system). Capsules consume the results as ordinary declared reads in a later system; the operator never writes a field it did not declare and stores every declared write for every row.

**Registered operators (2026-10-26, issue 137).** A host operator does not have to be compiled into the engine. A
game that embeds Sutekh through `include/sutekh/host.h` can define one in its own code (for example a cross-entity
damage or targeting pass) and add it with `su_host_register_operator(const SuHostOperatorDesc *, SuHostError *)`
before it creates a world that lists the operator's record. The descriptor gives the system ID, schema ID, operator
name and version, constants hash, the field table (IDs, names, value types, access), a scratch size as a function of
the world's capacity, an optional operator-data parser (`su_host_set_operator_data`) and the `run` callback. From
then on the operator is checked and run exactly as a built-in one: the record must equal the descriptor (operator
name, version, constants hash, every lane; or a schema-relative binding of it, section 2), the schedule's union table
and the effect analysis use its declaration, and it runs in capsule order, so an IR system with an `after` edge to it
consumes its results in the same tick. At run time `run` sees only its declared lanes (no world pointer); its
read lanes are re-hashed after the call and a change fails the tick with `SU_E_UNDECLARED_WRITE` (unless the
descriptor sets `SU_HOST_OPERATOR_TRUST_INPUTS`); write lanes are pre-filled (zero for write-only lanes, the input
value for read-write lanes), so every row is defined; the scratch is zeroed before every call, always (it is not in
snapshots); scratch, column views and alive/generation views are allocated at world creation, so the tick allocates
nothing. Names (system, schema, operator, lanes) are identifiers (`[A-Za-z0-9_.-]`). The system's semantic hash
covers the operator name, version, constants hash, lanes, binding, the descriptor flags and the scratch size for the
world's capacity, and is part of the world identity, so snapshots and replays only load into a world whose process
registered the same operator the same way. Registering an ID twice, an ID built into the library, an ID with a
reserved prefix (`rts.`, `space3d.`, `sutekh.`, any letter case) or a reserved operator name (`rts_ops*`,
`space3d_ops*`, `sutekh*`) is `SU_E_DUPLICATE_KEY`; a record whose operator is neither built in nor registered stays
`SU_E_CAPABILITY_DENIED`. The static library and the DLL each have their own registry.
A process that registers nothing behaves exactly as
before (records, identities and digests unchanged). Registration is in-process `host.h` only: `sutekh serve`, the
worker and `world.create` do not see registered operators (their records are refused there as unknown); a game that
needs one in a worker would have to build its own worker, which is not provided. Contract and limits: `host.h`,
"registered host operators". Evidence: `host_registered`, `host_registered_dll` and the cross-process
`host_registered_record` / `host_registered_verify_*` tests (`tests/host/test_host_registered.c`).

**Operator state (2026-10-26, issue 139).** A registered operator may also own authoritative variable-size state that
does not fit fixed columns (for example sparse per-entity overlays keyed by entity handle). It declares maximum
entries and bytes in its descriptor (version 2 fields); the engine owns the bytes as a sorted (entity, sub) -> value
map that `run` changes through `run->state` (`su_host_state_put`, `alloc`, `modify`, `erase`, `erase_entity`). The
state is canonical (its bytes depend only on its content), commits or rolls back with the tick, is in snapshots
(version 2, section `SUOPSTAT`), both digests, both restores and replay keyframes, and its declaration joins the
world identity. Nothing is allocated after world creation; a change past a cap is refused (`SU_E_RESOURCE_LIMIT`).
With `SU_HOST_STATE_DROP_DEAD` the entries of dead entities are erased when a tick publishes. Built-in operators have
no state, and worlds without operator state keep version 1 snapshots byte for byte. Contract: `host.h`, "operator
state". Evidence: `host_opstate*` tests (`tests/host/test_host_opstate.c`).

**Events from operators (2026-10-27, issue 172).** A registered operator may append events to the world's event log
while it runs, through `run->emit_event` (descriptor version 3: `max_events_per_tick`), for example one record per
resolved hit, which per-row columns cannot express (they keep only the last occurrence in a tick). The records are
the IR `emit_event` records: same 40-byte layout, same kind range (below `SU_EVENT_KIND_RESERVED_MIN`), the
operator's schedule position as `system_index`, and `seq` numbering its emissions in the run from 0. A tick's log is
ordered by schedule position and then by the order each system produced its records, so an operator's records appear
in its emission order between the IR systems before and after it; nothing depends on thread timing, because `run` is
one serial call per tick. Past `max_events_per_tick` emissions the append is refused with `SU_E_RESOURCE_LIMIT` and
the refusal is sticky: the tick fails in that system and nothing of it is committed, the same outcome the IR path
gives for an over-cap emission; the world's own `max_events_per_tick` bounds the tick's total, host records
included. Because the records are the log, they are in the canonical snapshot, both digests, both restores and
replay keyframes, and re-simulation re-emits them with the same sequence numbers. The operator's buffer is allocated
at world creation; the entity tick's merge buffer (40 bytes times the sum of the declared caps) is allocated on the
first step that builds the entity cache, and the declared cap joins the world identity. An operator that declares no
cap gets `emit_event == NULL` and behaves exactly as before. Contract: `host.h`, "events from host operators".
Evidence: `host_events`, `host_events_dll` and the cross-process `host_events_record` / `host_events_verify_*` tests
(`tests/host/test_host_events.c`).

Typed host lanes: `SuHostField.type` holds the lane's value type (`SU_TYPE_U32`, or `SU_TYPE_I32`/`I64`/`Q32` for a typed lane; a 64-bit field is two adjacent entries `I`, `I+1` named `<field>.lo`, `<field>.hi`). A descriptor with typed lanes has numeric profile `fixed_q32_v1` and operator family `<schema_id>_ops`. Descriptors with only u32 lanes (the RTS operators and their tests) keep `rts_ops_v<N>`, `u32_mod_v1`, and byte-identical declaration digests.

**Cross-table movers and targets (2026-10-27, issue 140, slice 2).** A world with several tables (`host.h`, "tables")
keeps projectiles and ships apart; a `space3d.query` entry can take its **movers** from its own table and its
**collidables and gates** from another. Three additive pieces, no change to a single-table operator:

- the descriptor declares the target lanes (`SuHostOperatorDesc.target_field_count/target_fields`, descriptor
  version 4): the read-only lanes a target row is queried by. For this family they are `kind`, `gate_index`, the
  pose (`pos_*`, `rot_*`), `radius` and the half extents; a target row is never swept, so `prev_*`,
  `next_gate`, `prox_margin`, `z0/z1`, `compound` and `rot0_*` are not part of the set. The names are the
  target table's **declared** lane names (`pos_x.lo`, `pos_x.hi`, ...), resolved at world creation.
- `SuHostWorldDesc.capsule_target_table` names, per capsule, the table whose rows the entry reads as targets
  (descriptor version 5); it must differ from the capsule's own table.
- `SuHostOperatorRun.target_*` gives the operator the target table, its rows and base, its columns under the
  descriptor's IDs, and its alive/generation views. The target columns are the target table's working values
  during a tick (a target system earlier in the schedule is visible) or its committed values when the table has
  no system; the operator only reads them.

Movers are the operator's own table's rows; a target row is offered as a candidate to a mover and ranked by its
**world** slot, so the candidate cap and contact order are the same rule as the single-table form. A mover
table's non-mover rows are neither movers nor targets, so a query in the projectile table does not collide
projectiles with each other unless the game puts a row in the target set too. A target set is bound once at
world creation, joins the world identity, and needs one instance scratch for both tables' rows, so the tick
still allocates nothing. The cross-table result lanes (`c0_slot`, `c0_gen`, `gate_slot`, ...) carry **world**
slots and generations, so a game's handle check (`handle_live`) matches the host's handles. A world without a
target set is byte-identical to the single-table form: the released operator path runs unchanged. Evidence:
`host_tables_entities` (`tests/host/test_host_tables_entities.c`) runs the issue's example (ships in table 0,
projectiles in table 1, spawns, events, despawns, cross-table queries, snapshot and replay) and a target table
with no system; `space3d_ops`, `space3d_ops_v2_grid` and the determinism matrix are unchanged.

## 2. Schema `space3d` (profile `profiles/space3d`)

A world that uses the operator has schema ID `space3d` and reserves these field IDs; games add their own fields at other IDs (100 and above by convention). Every q32 field occupies `ID` and `ID+1`.

| ID | Field | Type | Operator access | Meaning |
|---|---|---|---|---|
| 1 | `kind` | u32 | read | 0 none, 1 mover (swept sphere; a capsule under `space3d.query_shape`; a compound under `space3d.query_compound`), 2 sphere, 3 oriented box, 4 capsule, 5 rectangular gate, 6 disc gate, 7 elliptical gate, 8 static mesh (see "Static meshes") |
| 2 | `next_gate` | u32 | read | mover: `gate_index` of the gate it must cross next |
| 3 | `gate_index` | u32 | read | gate: its index in the course; static mesh (kind 8): its mesh index in the operator's mesh set |
| 10, 12, 14 | `pos_x/y/z` | q32 | read | position at the end of the tick (movers), centre (others) |
| 16, 18, 20 | `prev_x/y/z` | q32 | read | mover position at the start of the tick |
| 22..28 | `rot_w/x/y/z` | q32 | read | unit orientation, local to world (boxes, capsules, gates) |
| 30 | `radius` | q32 | read | sphere/mover/capsule radius, disc gate radius |
| 32, 34, 36 | `half_x/y/z` | q32 | read | box half extents; capsule half length along local z (`half_z`); gate rectangle half width/height; ellipse semi-axes along local x and y |
| 40 | `hit_count` | u32 | write | contacts reported (0..2) |
| 41 | `hit_flags` | u32 | write | 1 candidates truncated, 2 contacts truncated, 4 pair budget (v1: the tick's sweep budget ran out, set on every mover; v2: this mover's broadphase was cut, section 3 "Broadphase v2") |
| 42 + 10k | `ck_slot`, `ck_gen` | u32 | write | contact k (k = 0, 1): other row's slot and generation (slot 0xFFFFFFFF when absent) |
| 44 + 10k | `ck_toi` | q32 | write | time of impact within the tick, [0, 1] |
| 46 + 10k, 48 + 10k, 50 + 10k | `ck_nx/ny/nz` | q32 | write | unit contact normal from the other shape towards the mover |
| 62 | `gate_dir` | u32 | write | 0 none, 1 forward, 2 backward |
| 63 | `gate_slot` | u32 | write | slot of the crossed gate (0xFFFFFFFF when none) |
| 64 | `gate_toi` | q32 | write | crossing time within the tick |

Non-mover rows store zero counts and NONE slots; dead rows are zeroed by the world at publish.

**Schema-relative binding (2026-10-18).** A game does not have to use schema `space3d` or these IDs. Its host record
can bind the operator to its own schema: `schema.id` names the game schema, and every entry of `host.fields` carries
the game lane's own `id`, `name`, `type` and `access` plus `"binds": "<operator lane name>"` (for example
`{"id": 1010, "name": "g_pos_x.lo", "type": "q32", "access": "read", "binds": "pos_x.lo"}`). The record is checked by
`su_host_load_bound` (`src/runtime/rts/host_decl.h`): binds on every entry or on none; every operator lane bound
exactly once to an existing name; type and access equal to the bound lane's; IDs nonzero and strictly ascending;
names unique and 1..64 bytes; the two lanes of a 64-bit operator field bound by game IDs `J`, `J + 1` in that order.
Any violation refuses the record with `SU_E_SCHEMA_MISMATCH` and a reason. The accepted declaration carries the
game's IDs, names and schema, so the schedule's union table checks the game's IR systems against it as usual, and its
semantic hash covers the binding. At run time the schedule hands the operator the game's columns under the operator's
own lane IDs (`su_sched_add_host_bound`), so the operator's code and results are unchanged. `world.create` accepts a
bound record as a `{capsule_path, host_operator}` item, and checkpoints, restore and recovery reload it the same way.
Evidence: `space3d_bind` (one refusal per rule, each for its intended reason, and the schedule's mapping check) and
`space3d_serve` check 5 (the racer microcosm rewritten under the game schema `racer_game` with every ID + 1000 and every
name prefixed `g_`, run through `sutekh serve`, VM and native: every lane equals the original run's).

**In-process host API (2026-10-18).** A game that embeds Sutekh through `include/sutekh/host.h` passes the record's
bytes as one of the world's capsules (`SuHostCapsule.lir_json = NULL`, `lir_len = 0`); exact and bound records are
checked by `su_host_load_bound_bytes`, the same checks as the file loader. `su_host_operator_count`/`_info` list the
operators built into the library. Every world gets its own operator scratch, sized for its capacity at creation
(`space3d_query_instance_create`, `SuSysEntry.host_inst`), so worlds in one process are independent and may step on
different threads; the worker keeps the static context. The operator's results are world columns, so snapshots,
digests, in-place restore and replays carry them. Evidence: `host_ops` / `host_ops_dll` (the racer microcosm through
the host API reproduces the pinned in-process digests; rollback re-emits events; two worlds on two threads) and
`example_arena_host` (`examples/host_space3d`: a game schema `arena` with a bound record, gates and bumps, rollback
drills, pinned digest on the VM and native tiers).

**Binding rules for IR systems (2026-10-18).** (1) An IR 0.3 field of type `q32` or `i64` named `x` with ID `J`
binds two storage lanes: `x.lo` at ID `J` and `x.hi` at ID `J + 1` (LIVE_IR_SPEC.md, "Fields and storage lanes"). A
host record names lanes, so an IR system that consumes the record's `pos_x.lo`/`pos_x.hi` declares one q32 field
`pos_x` at the `.lo` lane's ID, and IR operands name `pos_x`. A disagreement is refused with
`SU_E_SCHEMA_MISMATCH` and a message naming both systems, both IDs and names and the lane rule (for example
`field mismatch: racer.move has id 10 "posx_.lo" (q32), space3d.query has id 10 "pos_x.lo" (q32); a q32/i64 IR field
"x" at ID J binds lanes "x.lo" (J), "x.hi" (J+1)`; `su_sched_explain_mismatch`, used by `world.create` and
`su_host_world_create`). (2) A capsule's `read`/`write` flags must equal what its IR actually loads and stores: a
field declared `read: true` that no instruction loads is refused at world creation with `SU_E_SCHEMA_MISMATCH` and
the message `... declared read=1 write=0 but IR reads=0 writes=0 (the capsule's read/write flags must equal what the IR
actually loads and stores: set read:false or load the field)`.

**Post-query ordering: same-tick consumption (2026-10-18).** Systems run in capsule order; phases only group
systems for effect analysis (phases sort by name, so `query` sorts before `simulate`). An `after` edge across
phases is refused, with one exception: an IR system may name a **host operator of an earlier phase** in its
`after` list. The edge is verified like any other (the target must exist, be a host entry, sort in an earlier
phase, and come earlier in the capsule order; ordering cycles are refused) and makes the IR system run after the
operator in the same tick, so it can respond to this tick's contacts before the state is committed. Other
cross-phase edges (a `before` edge, an edge from a host operator, an edge to an operator of a later phase) are
still refused with a message saying so. Nothing changes for schedules without such an edge. The pattern
(`examples/host_space3d/arena_respond.*`, `arena_host --respond`): the operator in phase `query` after the motion
system, then a response system in phase `simulate` with `"after": ["<motion system>", "space3d.query_prox"]` that
puts a mover that moved into its first contact back to `prev + toi * (pos - prev)` and reflects its velocity.
Evidence: `example_arena_host_respond` (900 ticks, 30 rollback drills with 0 differences, pinned digest; the deepest
committed penetration of a ship into the sphere or another ship is at most 1e-6 m, measured each tick; the same run
without the response system commits 0.209 m), and `space3d_serve` check 6 (the same capsules through
`sutekh serve`, including a snapshot/restore, equal the in-process host API on every lane).

**Proximity: `space3d.query_prox` (2026-10-18).** A second operator, record
`profiles/space3d/space3d_query_prox.capsule.json`, with every lane and result of `space3d.query` plus:

| ID | Field | Type | Access | Meaning |
|---|---|---|---|---|
| 66 | `prox_margin` | q32 | read | mover: proximity margin; <= 0 turns proximity off for the mover |
| 68 | `prox_slot` | u32 | write | closest other collidable within the margin (0xFFFFFFFF when none) |
| 69 | `prox_gen` | u32 | write | its generation (0 when none) |
| 70 | `prox_dist` | q32 | write | its surface distance in [0, margin] (0 when none) |

The distance is taken at the end positions: centre-to-shape distance (sphere and mover: centre distance; capsule:
distance to the clamped closest point of the segment, `t = clamp(dot(p - a, b - a) / dot(b - a, b - a), 0, 1)`;
box: length of the box-frame offset minus its clamp to the half extents) minus the other shape's radius, minus the
mover's radius, clamped at 0; every step a profile operation. The kept collidable is the one with the smallest
distance, ties to the lowest slot. A mover with a positive margin has its broadphase bounds widened by the margin,
and proximity is chosen among the same kept candidates as contacts (so flag 1 also covers proximity). With every
margin <= 0 the base lanes equal `space3d.query`'s bit for bit. Constants hash `79204fc0...` (SHA-256 of
`space3d_prox_constants_text()`). Evidence: `space3d_ops` (known answers for sphere, capsule and box distances, ties
and margins; 30 random scenes: margin 0 equals the base operator on every base lane, random margins equal brute force
on every lane when nothing is truncated; a tie-break mutant fails).

**Capsule movers: `space3d.query_shape` (2026-10-18).** A sphere fits long or flat craft poorly (one measured hull:
bounding sphere 40.9 m about the origin for half extents 18.3 x 13.7 x 32.8 m, so a side contact fires up to 22.6 m
from the hull; a capsule along local z fits it at r 24.3 m). A third operator, record
`profiles/space3d/space3d_query_shape.capsule.json`, has every lane and result of `space3d.query_prox` plus:

| ID | Field | Type | Access | Meaning |
|---|---|---|---|---|
| 72 | `z0` | q32 | read | mover: start of the core segment on its local +z axis |
| 74 | `z1` | q32 | read | mover: end of the core segment on its local +z axis |

A mover with `z0 = z1 = 0` is a sphere and takes exactly the sphere code path, so its lanes equal
`space3d.query_prox`'s bit for bit; worlds that use `space3d.query` or `space3d.query_prox` are untouched (their
records, constants hashes and digests are unchanged). Otherwise the mover is the capsule of radius `radius` about the
segment from `rot * (0, 0, z0)` to `rot * (0, 0, z1)` relative to its position (`z0 > z1` is allowed; `z0 = z1 != 0` is
an offset sphere).

*Rotation during the tick.* The schema has one orientation per row, so the segment offsets are computed once from the
mover's `rot` lanes, which hold its orientation **at the end of the tick**, and held for the whole tick: the sweep is
pure translation from `prev` to `pos`. This is an approximation, not a bound: a mover turning by an angle a during
the tick has a segment end at distance |z| from the reference point displaced by up to 2 |z| sin(a / 2) from where
the true motion has it at the start of the tick (about 0.35 m for |z| = 20 m and 1 degree per tick), with the error
shrinking to zero at the end of the tick. Contacts can therefore be early, late or missed by that much at the ends of
the segment. A game that needs a conservative result can add that amount to `radius` (or to the obstacle) for fast
turners; a game that needs the start orientation can store it in the `rot` lanes before the query.

*Narrow phase* (`s3_sweep_capsule_*`, `space3d_geom.h`). The mover's reference point c sweeps against the Minkowski
sum of the obstacle core and the reversed mover segment: for a segment core a-b (capsule, sphere with a = b, another
mover's segment at its start position with the relative displacement, as for spheres) that sum is the parallelogram
`{a - e0 + alpha (b - a) + beta (e0 - e1)}` rounded by the radius sum. Its earliest contact is the smallest s among the
face slab (entry through the plane at distance R, with the hit point's parallelogram coordinates, computed with
cross products against the unit normal, in [0, 1]) and the four edge capsules (`ray_capsule`, the existing
sphere-against-capsule root); equal s keeps the earlier feature (face, then edges in fixed order). Start contact gives
toi 0. The normal is taken from the closest point of the parallelogram to the contact centre (face first, then the
edges, smallest squared distance, ties to the earlier), i.e. from the closest pair of core points, pointing from the
obstacle to the mover. Against an oriented box: a core segment that crosses the box at the start (slab clip) reports
toi 0 with the box normal at the middle of the crossing chord; otherwise the earliest of the two end-sphere sweeps
(`s3_sweep_sphere_obb`) and the twelve box-edge segment sweeps (radius r), ties to the earlier in that order. This is
exact: the first touch of a segment and a box is at an end point of the segment or between the segment and a box
edge. A sphere mover against a capsule mover uses `s3_sweep_sphere_capsule` against the other's segment.
Same toi/normal contract and accuracy domain as the sphere sweeps.

*Broadphase and proximity.* A capsule mover's bounds are the union of its two end spheres at `prev` and at `pos` (the
swept shape is their convex hull), plus the margin and slack as before. `prox_dist` uses core segment distances:
segment to point or segment (closest point of the same parallelogram), segment to box (0 when the segment crosses it,
else the minimum over the end points and the twelve edges). Gate crossings stay on the reference point's segment
`prev -> pos`. Constants hash `fb8287e3...` (SHA-256 of `space3d_shape_constants_text()`).

Evidence: `test_space3d_geom` (hand-worked cases: end-sphere contact at toi (10 - sqrt 3) / 20, crossed capsules
(face) and parallel capsules (edges) at 0.3, box edge at (4 - sqrt 0.75) / 10 with normal (-sqrt 0.75, 0, 0.5), box
face via an end sphere, start crossing, segment distances; 6,000 random sweeps each against sphere, capsule and box
checked against a double-precision reference that minimises the convex core distance by nested ternary search and
bisects the first root: 0 mismatches, worst toi error 7.3e-7 m, worst normal error 3.4e-7; a mutant without the
face slab fails 762 cases). `test_space3d_ops` (descriptor, hash and record; a long mover hits a sphere a sphere
mover misses; the axis follows `rot`; two capsule movers report each other at one toi with opposite normals; a
two-sphere tie at one toi orders by slot and proximity ties go to the lower slot, with slots swapped; 30 random
scenes with z lanes zero equal `space3d.query_prox` on every lane; the same scenes with random capsule movers: broadphase
equals brute force on every lane in the 22 untruncated scenes, repeat bit-identical; a mutant with sphere bounds for
capsule movers fails). `host_ops` check 8 (host API: z lanes zero give every lane of the `space3d.query` world; capsule
movers of +/-20 m: rollback over 120 ticks in place and allocating, and replay seek + play, bit-identical).
`space3d_serve` check 7 (`sutekh serve`: z lanes zero equal the `space3d.query` run on every lane; capsule movers:
snapshot at T/2, restore and re-step to T equal the straight run on every lane). Native tier: the operator is compiled
C run identically under every tier (it is not lowered), so there is no separate native path to compare.

**Static meshes (2026-10-20).** Analytic shapes fit real models poorly (a battleship wreck's box set holds 3.5 to
7.6 times its solid volume, so contacts fire far from the visible hull). Every space3d operator (`space3d.query`,
`space3d.query_prox`, `space3d.query_shape`) can also collide movers against triangle meshes, with no new lanes and no
new records:

- *Data.* A mesh set is immutable operator data handed to a world before its first step:
  `su_host_set_operator_data(w, system_id or NULL, bytes, len, &err)` (`include/sutekh/host.h`). NULL gives it to every
  host entry whose operator takes data (one parse, shared; a game with a second binding of the operator, for example
  a look-ahead sensor, gets the same set). Format `S3MESH01` (`profiles/space3d/native/space3d_mesh.h`): triangle
  geometries (q32 vertices, u32 indices), parts (a geometry placed in the mesh frame by position, unit rotation and
  uniform q32 scale) and meshes (lists of parts; empty meshes allowed). Shared geometry with per-part scale keeps
  instanced content (many scaled copies of one rock) small. Limits: 65,536 geometries, 2^20 parts and meshes, 2^24
  vertices and triangles, coordinates within 2^20 m, scale in [2^-16, 2^16], rotations unit within 2^-16. Malformed
  data is refused with `SU_E_PARSE` (or `SU_E_RESOURCE_LIMIT`) and a reason naming the table and index; nothing is
  attached then.
- *Identity.* The data is not world state (not in snapshots or digests) but it is part of the world identity:
  `SHA-256("SUOPDATA\0" || identity || u32 entries || per entry: u32 schedule index || SHA-256(data))`. Snapshots and
  replays only load into a world given the same bytes, so a replay cannot be re-simulated against other geometry.
  Data is accepted once per entry and only before the first `su_host_step`.
- *Rows.* A row of kind 8 is mesh `gate_index` placed at `pos` with orientation `rot` (mesh frame to world). It is a
  collidable exactly when its operator instance holds a set with that mesh and the mesh has parts; otherwise (no set:
  the `sutekh serve` worker, a world without data, an unknown index, an empty mesh) it is not collidable, and every
  lane equals the result of the same world without the row, bit for bit. A world that never uses kind 8 is
  unchanged: no record, constants hash or digest moves. Broadphase bounds are the mesh's bounds (union of its parts'
  oriented bounds, padded 1/64 m) as an oriented box at the row's pose.
- *Narrow phase.* The mover (sphere, or capsule under `space3d.query_shape`) is taken into the mesh frame, then into
  each part's frame (`conj(rot) * (x - pos) / scale`; radius / scale) and swept against the triangles
  (`space3d_geom.h`): a sphere against the face slab and the three edge capsules (`s3_sweep_sphere_tri`); a capsule
  against its two end spheres and the rounded parallelograms of the three edges, or toi 0 when its core segment
  crosses the triangle at the start (`s3_sweep_capsule_tri`). Triangles are two-sided surfaces, not solids: a mover
  already inside a closed mesh without touching it does not collide (a game keeps movers out of closed meshes;
  swept contacts never let one in). The result is the hit with the smallest (toi, part index, triangle index) over
  every triangle of the mesh; its normal is that triangle's closest-feature normal rotated back to the world. A mesh
  row contributes at most one contact per mover, in the usual (toi, slot) order with the other contacts.
- *Proximity* (`space3d.query_prox`, `space3d.query_shape`): the core distance (point or segment to triangle,
  `s3_dist_seg_tri`) to the nearest triangle, part distances multiplied by the part scale, minus the mover radius.
  Only triangles within the mover's margin are searched; beyond it the row is not within the margin.
- *Culling.* Geometry and part BVHs are built at load by a median split on the axis of largest centroid spread (stable
  order by centroid key and index; leaves of at most 4), from exact integer bounds. Sweeps visit nodes whose bounds,
  swept back by the mover's extent and padded by 1/64 m, the path enters no later than the best toi so far; each
  triangle first passes a plane test (the swept core more than r + pad from its plane on one side is skipped).
  Distances visit nodes whose box gap is within the best distance + pad (nearer child first), skip triangles whose
  plane is farther than that from both segment ends, and are searched only below the best proximity distance found
  so far among the mover's candidates (a strict < keeps the tie rule). Every skipped triangle provably cannot
  reach the reported minimum, so results do not depend on the traversal: tests compare culled and brute-force runs
  bit for bit.
- *Cost.* Per mover and mesh row: two frame changes, then a BVH walk; an exact triangle test is a fixed sequence of at
  most 2 face and 15 edge root solves. Load: O(T log^2 T) integer work per geometry, at set time, never in a tick;
  memory about 100 bytes per triangle plus 56 bytes per BVH node (at most 2 nodes per triangle).

Evidence: `test_space3d_geom` (triangle known answers: face from both sides, edge, start contact, degenerate triangle
equal to its segment, capsule face, start crossing and edge contacts, distances; 20,000 random sphere sweeps and
6,000 random capsule sweeps against the double-precision reference (Ericson's closest point on a triangle, nested
ternary search and bisection): 0 mismatches, worst toi error 6.6e-7 m and normal error 6.2e-7; 1,500 random
segment-triangle distances within 1e-5 m; a mutant without the face slab fails 1,039 cases). `test_space3d_mesh`
(blob refusals: every truncated prefix, magic, trailing bytes, index ranges, coordinates, part geometry, scale and
rotation, mesh ranges, empty geometry; a box of 12 triangles at random poses, part rotations and scales against
`s3_sweep_*_obb` for 5,927 sphere and capsule sweeps starting outside it: 0 mismatches, worst toi error 2.5e-7 m;
3,600 sweeps and 3,600 distances on random soups of 800 triangles in 4 rotated, scaled parts: culled equals brute
force bit for bit with 23 times fewer triangle tests; 4,000 crossings of a zero-thickness wall at 1,820 m/s (15.17 m
per 120 Hz tick) by spheres of 0.25 m and ship-sized capsules (r 24 m, 18 m core) at up to 75 degrees: every one
reports the first contact, worst error 2.2e-7 m). `test_space3d_ops` (an instance holds one set; the static context
none; box and scaled triangle meshes give the known toi, normal and proximity; empty and unknown meshes never collide;
24 random scenes with mesh rows: without a set every lane equals the scene with those rows removed, with a set
broadphase equals brute force in the 18 untruncated scenes and a repeat is bit-identical). `host_ops` check 9
(refusals leave the identity unchanged; same data, same identity, other data, other identity; data after a step or
twice refused; a snapshot of a world without data does not load into one with it; the racer microcosm with its 48
obstacles as triangle boxes collides only with the set, rolls back bit-identically, and its replay re-simulates in a
world given the same data and is refused by one without).

**Compound movers: `space3d.query_compound` (2026-10-20).** One capsule fits a winged or tall craft poorly (a measured
wide hull as one capsule: radius 89.9 m, so contacts fire up to 81 m from the visible hull), and a capsule held at the
end-of-tick orientation is not a bound for a turning craft. A fourth operator, record
`profiles/space3d/space3d_query_compound.capsule.json`, has every lane and result of `space3d.query_shape` plus:

| ID | Field | Type | Access | Meaning |
|---|---|---|---|---|
| 76 | `compound` | u32 | read | mover: index of its compound shape in the instance's set (0xFFFFFFFF: none) |
| 77 | `c0_sub` | u32 | write | contact 0: low 16 bits the mover's sub-shape, high 16 bits the other mover's sub-shape (0xFFFF for a side that is not a compound mover); 0xFFFFFFFF when there is no contact |
| 78 | `c1_sub` | u32 | write | contact 1, as `c0_sub` |
| 80..86 | `rot0_w/x/y/z` | q32 | read | mover: its orientation at the start of the tick; all four zero: not given |

- *Data.* Compound shapes are operator data, in the same set as the static meshes: blob format `S3MESH02`
  (`space3d_mesh.h`) is `S3MESH01` with two more header counts and two more tables, sub-shapes (`a`, `b`, `radius`
  in the mover's local frame; `a = b` is a sphere) and compounds (a range of 1 to 8 sub-shapes). Limits: 65,536
  compounds, 2^20 sub-shapes, coordinates and radii within 4,096 m. A set may hold compounds only. The data enters
  the world identity as before (`su_host_set_operator_data`), so replays only load with the same shapes.
- *Shape.* A mover whose `compound` lane names a compound of its instance's set is the union of the sub-shapes,
  each rotated by the mover's `rot` lanes (end of tick) about its reference point; its `radius`, `z0` and `z1`
  lanes are not used for contacts: they are its proximity envelope (below). Any other
  mover (`compound` 0xFFFFFFFF, an unknown index, or no set: the `sutekh serve` worker) is the sphere or capsule
  mover of `space3d.query_shape`.
- *Turning.* When a mover's `rot0` lanes are nonzero, s = sin(a / 2) is taken for the angle a of the shortest
  rotation from `rot0` to `rot` (|vector part| / |quaternion| of conj(rot0) * rot, plus 2^-20 for rounding), and
  every sub-shape (or the capsule mover's segment) is swept with its radius padded by `2 reach s` plus one raw
  unit, `reach` an upper bound of its core's distance from the reference point (max(|a|, |b|), rounded up at load;
  max(|z0|, |z1|) for a capsule mover). A core point at distance d moves at most 2 d sin(a / 2) between any
  orientation on that arc and the end orientation, so the padded translation-only sweep contains the shape at every
  instant of a motion that interpolates the position linearly and the orientation along the shortest arc: a turning
  craft cannot pass through anything within the tick. The pad is a bound, so contacts can come early by up to it
  (0.8 m for a point 8 m out turning 0.1 rad). Proximity is measured at the end pose without the pad. A sphere
  mover is never padded (its core is the reference point).
- *Narrow phase.* Mover pieces (sub-shapes, or the one sphere/capsule of a non-compound mover) against the other
  row: static shapes and meshes per mover piece (`s3_sweep_capsule_*`, the sphere functions for a sphere piece at
  its offset centre, `s3mesh_sweep_lim` with the best toi so far as its limit); another mover per pair of pieces,
  that mover held at its start position with the relative displacement. The contact is the earliest over the
  pieces; equal toi keeps the lower mover piece, then the lower other piece. `c*_sub` name them. Contacts per
  mover and their (toi, slot) order are unchanged: a row still contributes at most one contact.
- *Broadphase and proximity.* A compound or padded mover's bounds are the union of every piece's end spheres at
  `prev` and at `pos` with the padded radii and of its `radius`/`z0`/`z1` shape at both positions (plus the margin
  and slack as before). Proximity measures every mover by its `radius`/`z0`/`z1` sphere or capsule, unpadded, exactly
  as `space3d.query_shape` does (since 2026-10-20 evening; the first version took the minimum over the sub-shapes):
  a game keeps an enveloping sphere or capsule there. An exact distance search per sub-shape costs about n times the
  single search against meshes (measured in a downstream pack race: proximity per sub-shape raised the 99th
  percentile step from about 2 ms to 4 ms), while a proximity margin is coarse by nature. Gate crossings stay on the
  reference point's segment.
- *Compatibility.* A mover with `compound` 0xFFFFFFFF and `rot0` all zero takes exactly the `space3d.query_shape`
  code (its lanes equal that operator's bit for bit, and `c*_sub` are 0xFFFFFFFF); worlds using the other three
  operators are untouched (their records, constants hashes and digests are unchanged). Constants hash
  `ced24aed...` (SHA-256 of `space3d_compound_constants_text()`).
- *Cost.* Per mover and candidate: n pieces against a static row (n mesh walks for a mesh, each limited by the best
  toi so far), n x m piece pairs against another mover (at most 64 rounded-parallelogram sweeps), one quaternion
  product and a square root for the turn per mover row.

Evidence: `test_space3d_mesh` (S3MESH02: accessors and reach, every truncated prefix, trailing bytes, compounds with
0 or 9 sub-shapes or a range beyond the table, coordinates beyond 4,096 m, negative radius, compounds-only sets;
`s3mesh_sweep_lim` against the unlimited sweep on 3,600 random soup sweeps: a limit at or after the first hit finds
it bit for bit, an earlier limit finds nothing earlier). `test_space3d_ops` (descriptor, hash, record loading and
refusal across records; known answers: the sub-shape that touches first (toi 0.45 vs 0.9 for the other piece, sub
ids), two compound movers head-on (one toi, opposite normals, swapped sub pairs), a compound against a box mesh
(hull piece at toi 0.75; rotated 90 degrees, a wing piece at 0.375), a turn of 0.1 rad padding a piece 8 m out by
0.79967 m (toi conservative by the 2^-20 rounding term only); 12 random scenes with `compound` none and `rot0`
zero: every `space3d.query_shape` lane bit-identical; a one-capsule compound equal to the capsule mover on every
contact lane; 24 random scenes of compounds, unknown indices, capsule movers, turns up to 0.3 rad and meshes:
broadphase equals brute force on every lane in the 20 untruncated scenes, repeat bit-identical; no tunnelling:
4,000 cases of a 4-piece craft or a twin capsule moving 15.17 m (1,820 m/s at 120 Hz) and turning up to 0.06 rad
within the tick against a sphere, a capsule, a zero-thickness wall mesh or another such compound coming the other
way, checked against a double-precision oracle of the true motion (linear position, slerp orientation, 257
samples): every one of the 1,409 overlapping motions reports a contact no later than the first sampled overlap
(0 missed, 0 late); with the turn pad disabled the same check finds 5 missed and 211 late). `host_ops` check 10
(host API with compounds, meshes and a constant turn: one 720-tick call equals 720 single steps, two worlds on two
threads equal the single-thread run, rollback in place and allocating over 120 ticks and replay seek + play are
bit-identical). Native tier: the operator is compiled C under every tier (not lowered), as for the others.

## 3. Semantics

**Broadphase.** Collidables are alive rows of kinds 1..4. Bounds: a mover's start and end spheres; a sphere; a box's rotated extents (plus one raw unit); a capsule's two end spheres; every bound widened by 4096 raw units (about 1 micrometre) so that rounding in the narrow phase's frame changes can never make it report a pair the broadphase dropped. Sweep axis: the axis with the largest spread of bound centres, ties to x, then y. Collidables are ordered by (minimum on that axis, slot) with a stable merge sort and swept; a pair whose intervals overlap on all three axes and that contains at least one mover becomes a candidate of each mover in it. At most 32 candidates are kept per mover (the first 32 in sweep order; flag 1). At most 2^22 overlapping-interval pairs are examined per tick; when that budget runs out every mover gets flag 4 and the remaining pairs are not examined. Both limits are deterministic functions of the world state.

**Narrow phase** (`space3d_geom.h`; every value q32, every step a profile operation). The mover's centre moves from `prev` to `pos` (displacement v, length L = v3_len(v), direction u = v3_normalize(v)); sweeps are solved in distance s along u and reported as toi = s / L.
- Sphere (and mover against mover, using the relative displacement v_a - v_b from the start positions): the earliest root of |m + s u| = R with m the start separation and R the radius sum: c = m.m - R^2; c <= 0 gives s = 0; otherwise b = m.u, no hit when b >= 0 or b^2 - c < 0, else s = -b - sqrt(b^2 - c), no hit when s > L. Normal: normalize(m + u s), +z for the zero vector.
- Capsule (segment a-b, radius rc): the earliest of the infinite-cylinder root (in the plane perpendicular to the unit axis, accepted when its axial coordinate is within [0, |b - a|]) and the two end-sphere roots; normal from the closest point on the segment.
- Oriented box: the problem is rotated into the box frame by the conjugate orientation. A sphere touching or overlapping the box at the start has toi 0 and the normal from the closest box point (centre inside the box: the axis of least penetration, ties to the lower axis). Otherwise the slab test against the box expanded by r gives the entry point; by its Voronoi region (face, edge, vertex; Ericson, RTCD 5.5.7) the hit is the face entry or the earliest root among the edge capsules at that edge or corner. The normal is always taken from the closest box point to the contact centre, so a near-tie between an edge and its corner sphere cannot tilt it.
- Contacts per mover: candidates are tested in ascending slot order; hits are kept by (toi, slot), at most 2 (flag 2 when more were found).

**Gate crossing.** The mover's centre segment `prev -> pos` against the gate whose `gate_index` equals the mover's `next_gate` (lowest slot among equal indices). n = rotate(q, +z), x and y axes likewise; d0, d1 are the signed distances of the end points. Forward when d0 < 0 <= d1, backward when d0 >= 0 > d1; t = d0 / (d0 - d1) clamped to [0, 1]; the crossing point's gate coordinates (lx, ly) must satisfy |lx| <= half_x and |ly| <= half_y (rectangle), lx^2 + ly^2 <= radius^2 (disc), or u^2 + v^2 <= 1 with u = div(lx, half_x), v = div(ly, half_y) (ellipse; q32 operations in that order, no crossing when a semi-axis is <= 0). A disc is the ellipse with equal semi-axes up to rounding; tests check that they agree on a grid. The operator reports the crossing; advancing `next_gate`, laps and wrong-way handling are capsule logic.

**Accuracy domain.** Results are bit-exact for every input. Geometric accuracy holds while the relative coordinates of a tested pair stay within about 16 km and radii and extents within about 4 km (squares stay inside Q32.32); outside that range intermediate values saturate deterministically.

**Broadphase v2 (`space3d_ops_v2`, 2026-10-26, issue 163).** In v1 the 2^22 sweep budget is world-wide: when a dense
scene exhausts it, every mover gets flag 4 and the pairs after that point are never examined, so collision stops for
most of the world (a 15,000-mover converging scene in `space3d_dense` loses 7,552 of 15,000 certain contacts that
way). The v1 descriptors keep that behaviour, and their constants hashes, records and digests, byte for byte. A world
opts into v2 by listing the v2 records instead: `space3d.query2`, `space3d.query2_prox`, `space3d.query2_shape` and
`space3d.query2_compound` (`profiles/space3d/space3d_query2*.capsule.json`; operator `space3d_ops_v2`, operator
version 2). Their lanes, narrow phase, proximity, capsule and compound movers, meshes, gates and contact order are
those of the v1 variant of the same suffix. Only candidate selection and the budget differ:

- *Candidates.* Every mover takes the collidables whose bounds overlap its own on all three axes (with the same
  bounds and slack as v1; a row whose bounds are empty on an axis, lo > hi, is not collidable). It keeps the 32 best
  by (rank, slot), whatever the order in which they were found: rank = (63 - level) in the top 6 bits, then the L1
  distance from the centre of the mover's bounds to the other's bounds (saturated at 2^58 - 1), where level is the
  other's grid level below. So larger bodies come first and then the nearest: in a swarm converging on a ship or a
  station, the ship is kept and the excess neighbours are dropped (flag 1). v1 keeps the first 32 in sweep order.
- *Finding them.* Sort and sweep, as v1, when the number of interval-overlap pairs on the sweep axis is at most 512
  per collidable (counted exactly before sweeping, one binary search per row). Otherwise a hierarchical grid: a row's
  level is the smallest l with every bounds extent at most 2^l raw units (cells of 2^l raw, so one to three cells per
  axis per query); one entry per row at the cell of its lower corner; a mover visits its own level and every coarser
  level that has entries, a static row only the coarser levels that have movers, and a pair across levels is given to
  both sides by the finer row's query. Cells are looked up by a 64-bit hash with an exact cell check. Both methods
  give the same result unless the grid's budget cuts.
- *Budget (declared capacity, part of the identity).* The pair budget is max(2^22, 1024 x capacity) grid entries
  visited per tick (capacity: the world's row count; 67,108,864 at 65,536 rows). When the grid's per-row entry counts
  sum to more than the budget, every row's query visits at most `cut` entries, the largest value with
  sum(min(count, cut)) <= budget, in a fixed order (coarser levels first, so large bodies are kept; in each level the
  cell of the row's centre first, then by z, y, x; entries in slot order). Only the most crowded rows are cut. A cut
  mover gets flag 4, and so does every mover with an entry in a coarser-level cell that a cut query did not finish
  (it may have lost the pair that query would have given it). Movers outside the crowded region keep every contact.
- *Counters.* `su_host_operator_counters` (host API, both families) returns the last run's movers with flag 1, 2, 4 or
  any, pairs examined, the pair budget, entries skipped (v2) and whether the budget was hit, plus totals since world
  creation, so a host sees truncation without scanning `hit_flags`.

Evidence: `space3d_ops_v2` runs every scene test of `space3d_ops` on the v2 descriptors, and `space3d_ops_v2_grid` with the grid
forced (`space3d_test_force_grid`): equal to the brute force on every lane wherever no flag 1 or 4 is set, as v1.
`space3d_dense` (host API): the converging scene above (15,000 movers of 0.25 m, 3 m per tick, on four 8 m targets,
one of them a mover) loses no contact in v2 (10,992 found and 4,008 displaced by two earlier contacts, as the brute
force reports them; 0 lost; no shell mover flagged 4), and a pile-up of 3,000 movers fired from one point beside
1,000 converging ones cuts only the piled movers (all 3,000 flag 4, 0 of the converging ones); counters equal the
lanes, no allocation in the tick, pinned digests. Determinism matrix workload `s3_dense` (cut, grid and sweep ticks).

## 4. Resource bounds

Static meshes (section 2): the set is allocated once at `su_host_set_operator_data`, never in a tick; traversal uses
fixed stacks of 128 entries (BVH depth is at most 25 for 2^24 triangles).

Worlds above 65,536 rows are refused (`SU_E_RESOURCE_LIMIT`, before any store). Scratch is static (about 12 MiB, admitted at process start; no allocation in the tick). Work per tick: O(n log n) sort, at most 2^22 sweep pair examinations, at most 32 narrow-phase tests per mover, one gate test per mover. The operator is not reentrant (one world per worker process, as for the RTS operators).

v2 (2026-10-26): the scratch holds v2's grid arrays as well, about 132 bytes per row more (cell key, level, count,
bounds centre, worst kept rank, sorted bounds copy, entry index, and a bucket table of a power of two of at least 2 x
rows slots at 17 bytes each): about 335 bytes per row in all, 21 MiB for the static context's 65,536 rows (virtual;
pages a v1-only process never touches are not committed) and the same per instance row. The pair budget costs no
memory: it bounds time. Work per tick in v2: O(n log n) sorts and binary searches; either at most 512 sweep pairs
per row, or the grid's lookups (at most 27 per level per row) plus at most max(2^22, 1024 x capacity) entry visits;
at most 32 narrow-phase tests per mover. Measured (i7-14700K, one core, GCC 16.2 -O3, `test_space3d_dense --bench`,
2026-10-26; observations, not budgets): about 6 ns per grid entry visited and 10 to 15 ns per entry when nearly
every visit is an overlapping pair; a full 2^22 budget about 40 ms. Dense scenes of `space3d_dense` (ms per tick,
median, v1 / v2): converge 31.6 / 82.3 (v1 drops half the contacts; v1 with its budget lifted takes about 84),
pile-up 19.5 / 43.4, sparse 4.4 / 5.2. Worst case at full capacity: the budget is about 66.5 million entries
for a 65,536-row world, and an independent review measured about 60,000 co-located movers reaching it at about
543 ms per tick (v1: 12 ms, because it stops after 2^22 pairs and flags every mover). The budget bounds work, so a
world this crowded is slow rather than wrong; it cannot be lowered in this version.

## 5. Evidence

`test_space3d_geom`: hand-derived known answers for every query and the gate rules, and 60,000 seeded random sweeps against an independent double-precision oracle (sampling plus bisection of the signed distance; cases within the oracle's grazing band of 1e-4 m are counted and skipped): 0 mismatches, worst TOI error 3.5e-6 m, worst normal error 1.1e-5. `test_space3d_ops`: descriptor validation, typed declaration module, pinned constants hash, the committed record loads and four altered records are refused, hand-built scenes (contacts, symmetric mover pairs, ordering and truncation, gates forward/backward/disc/miss, dead rows, zero rows), 40 random scenes of 64 to 960 rows where the broadphase output equals the brute-force output on every output lane whenever no truncation flag is set (34 scenes; the 6 truncated scenes are checked for repeatability), the candidate cap and the row limit.
