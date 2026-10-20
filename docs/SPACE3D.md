<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# space3d: deterministic 3D queries (`space3d_ops_v1`)

Status: implemented and tested (2026-10-18; amendment `planning/amendments/2026-10-18-numeric-3d.md`). Code: `profiles/space3d/native/` (`space3d_geom.c` narrow phase, `space3d_ops.c` operator). Numeric semantics: [NUMERIC_FIXED_Q32.md](NUMERIC_FIXED_Q32.md).

## 1. Capability model

Swept collision and gate crossing need cross-row reads, which IR 0.x does not have. They are therefore a **host operator** (the M3 mechanism used by `rts.spatial`): a phase system compiled into the engine whose effects are a declared field table. The declaration is checked twice: the host capsule record (`profiles/space3d/space3d_query.capsule.json`) must equal the descriptor exactly (operator `space3d_ops_v1`, constants hash, every lane's ID, name, type and access; `su_host_load`), and the schedule's union table requires every IR system that binds one of those lanes to agree on its ID, name, storage type and **value type** (`q32` lanes are q32 for every system). Capsules consume the results as ordinary declared reads in a later system; the operator never writes a field it did not declare and stores every declared write for every row.

Typed host lanes: `SuHostField.type` holds the lane's value type (`SU_TYPE_U32`, or `SU_TYPE_I32`/`I64`/`Q32` for a typed lane; a 64-bit field is two adjacent entries `I`, `I+1` named `<field>.lo`, `<field>.hi`). A descriptor with typed lanes has numeric profile `fixed_q32_v1` and operator family `<schema_id>_ops`. Descriptors with only u32 lanes (the RTS operators and their tests) keep `rts_ops_v<N>`, `u32_mod_v1`, and byte-identical declaration digests.

## 2. Schema `space3d` (profile `profiles/space3d`)

A world that uses the operator has schema ID `space3d` and reserves these field IDs; games add their own fields at other IDs (100 and above by convention). Every q32 field occupies `ID` and `ID+1`.

| ID | Field | Type | Operator access | Meaning |
|---|---|---|---|---|
| 1 | `kind` | u32 | read | 0 none, 1 mover (swept sphere; a capsule under `space3d.query_shape`), 2 sphere, 3 oriented box, 4 capsule, 5 rectangular gate, 6 disc gate, 7 elliptical gate, 8 static mesh (see "Static meshes") |
| 2 | `next_gate` | u32 | read | mover: `gate_index` of the gate it must cross next |
| 3 | `gate_index` | u32 | read | gate: its index in the course; static mesh (kind 8): its mesh index in the operator's mesh set |
| 10, 12, 14 | `pos_x/y/z` | q32 | read | position at the end of the tick (movers), centre (others) |
| 16, 18, 20 | `prev_x/y/z` | q32 | read | mover position at the start of the tick |
| 22..28 | `rot_w/x/y/z` | q32 | read | unit orientation, local to world (boxes, capsules, gates) |
| 30 | `radius` | q32 | read | sphere/mover/capsule radius, disc gate radius |
| 32, 34, 36 | `half_x/y/z` | q32 | read | box half extents; capsule half length along local z (`half_z`); gate rectangle half width/height; ellipse semi-axes along local x and y |
| 40 | `hit_count` | u32 | write | contacts reported (0..2) |
| 41 | `hit_flags` | u32 | write | 1 candidates truncated, 2 contacts truncated, 4 sweep budget exhausted |
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

## 3. Semantics

**Broadphase.** Collidables are alive rows of kinds 1..4. Bounds: a mover's start and end spheres; a sphere; a box's rotated extents (plus one raw unit); a capsule's two end spheres; every bound widened by 4096 raw units (about 1 micrometre) so that rounding in the narrow phase's frame changes can never make it report a pair the broadphase dropped. Sweep axis: the axis with the largest spread of bound centres, ties to x, then y. Collidables are ordered by (minimum on that axis, slot) with a stable merge sort and swept; a pair whose intervals overlap on all three axes and that contains at least one mover becomes a candidate of each mover in it. At most 32 candidates are kept per mover (the first 32 in sweep order; flag 1). At most 2^22 overlapping-interval pairs are examined per tick; when that budget runs out every mover gets flag 4 and the remaining pairs are not examined. Both limits are deterministic functions of the world state.

**Narrow phase** (`space3d_geom.h`; every value q32, every step a profile operation). The mover's centre moves from `prev` to `pos` (displacement v, length L = v3_len(v), direction u = v3_normalize(v)); sweeps are solved in distance s along u and reported as toi = s / L.
- Sphere (and mover against mover, using the relative displacement v_a - v_b from the start positions): the earliest root of |m + s u| = R with m the start separation and R the radius sum: c = m.m - R^2; c <= 0 gives s = 0; otherwise b = m.u, no hit when b >= 0 or b^2 - c < 0, else s = -b - sqrt(b^2 - c), no hit when s > L. Normal: normalize(m + u s), +z for the zero vector.
- Capsule (segment a-b, radius rc): the earliest of the infinite-cylinder root (in the plane perpendicular to the unit axis, accepted when its axial coordinate is within [0, |b - a|]) and the two end-sphere roots; normal from the closest point on the segment.
- Oriented box: the problem is rotated into the box frame by the conjugate orientation. A sphere touching or overlapping the box at the start has toi 0 and the normal from the closest box point (centre inside the box: the axis of least penetration, ties to the lower axis). Otherwise the slab test against the box expanded by r gives the entry point; by its Voronoi region (face, edge, vertex; Ericson, RTCD 5.5.7) the hit is the face entry or the earliest root among the edge capsules at that edge or corner. The normal is always taken from the closest box point to the contact centre, so a near-tie between an edge and its corner sphere cannot tilt it.
- Contacts per mover: candidates are tested in ascending slot order; hits are kept by (toi, slot), at most 2 (flag 2 when more were found).

**Gate crossing.** The mover's centre segment `prev -> pos` against the gate whose `gate_index` equals the mover's `next_gate` (lowest slot among equal indices). n = rotate(q, +z), x and y axes likewise; d0, d1 are the signed distances of the end points. Forward when d0 < 0 <= d1, backward when d0 >= 0 > d1; t = d0 / (d0 - d1) clamped to [0, 1]; the crossing point's gate coordinates (lx, ly) must satisfy |lx| <= half_x and |ly| <= half_y (rectangle), lx^2 + ly^2 <= radius^2 (disc), or u^2 + v^2 <= 1 with u = div(lx, half_x), v = div(ly, half_y) (ellipse; q32 operations in that order, no crossing when a semi-axis is <= 0). A disc is the ellipse with equal semi-axes up to rounding; tests check that they agree on a grid. The operator reports the crossing; advancing `next_gate`, laps and wrong-way handling are capsule logic.

**Accuracy domain.** Results are bit-exact for every input. Geometric accuracy holds while the relative coordinates of a tested pair stay within about 16 km and radii and extents within about 4 km (squares stay inside Q32.32); outside that range intermediate values saturate deterministically.

## 4. Resource bounds

Static meshes (section 2): the set is allocated once at `su_host_set_operator_data`, never in a tick; traversal uses
fixed stacks of 128 entries (BVH depth is at most 25 for 2^24 triangles).

Worlds above 65,536 rows are refused (`SU_E_RESOURCE_LIMIT`, before any store). Scratch is static (about 12 MiB, admitted at process start; no allocation in the tick). Work per tick: O(n log n) sort, at most 2^22 sweep pair examinations, at most 32 narrow-phase tests per mover, one gate test per mover. The operator is not reentrant (one world per worker process, as for the RTS operators).

## 5. Evidence

`test_space3d_geom`: hand-derived known answers for every query and the gate rules, and 60,000 seeded random sweeps against an independent double-precision oracle (sampling plus bisection of the signed distance; cases within the oracle's grazing band of 1e-4 m are counted and skipped): 0 mismatches, worst TOI error 3.5e-6 m, worst normal error 1.1e-5. `test_space3d_ops`: descriptor validation, typed declaration module, pinned constants hash, the committed record loads and four altered records are refused, hand-built scenes (contacts, symmetric mover pairs, ordering and truncation, gates forward/backward/disc/miss, dead rows, zero rows), 40 random scenes of 64 to 960 rows where the broadphase output equals the brute-force output on every output lane whenever no truncation flag is set (34 scenes; the 6 truncated scenes are checked for repeatability), the candidate cap and the row limit.
