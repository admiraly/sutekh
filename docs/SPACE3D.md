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
| 1 | `kind` | u32 | read | 0 none, 1 mover (swept sphere), 2 sphere, 3 oriented box, 4 capsule, 5 rectangular gate, 6 disc gate, 7 elliptical gate |
| 2 | `next_gate` | u32 | read | mover: `gate_index` of the gate it must cross next |
| 3 | `gate_index` | u32 | read | gate: its index in the course |
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

Worlds above 65,536 rows are refused (`SU_E_RESOURCE_LIMIT`, before any store). Scratch is static (about 12 MiB, admitted at process start; no allocation in the tick). Work per tick: O(n log n) sort, at most 2^22 sweep pair examinations, at most 32 narrow-phase tests per mover, one gate test per mover. The operator is not reentrant (one world per worker process, as for the RTS operators).

## 5. Evidence

`test_space3d_geom`: hand-derived known answers for every query and the gate rules, and 60,000 seeded random sweeps against an independent double-precision oracle (sampling plus bisection of the signed distance; cases within the oracle's grazing band of 1e-4 m are counted and skipped): 0 mismatches, worst TOI error 3.5e-6 m, worst normal error 1.1e-5. `test_space3d_ops`: descriptor validation, typed declaration module, pinned constants hash, the committed record loads and four altered records are refused, hand-built scenes (contacts, symmetric mover pairs, ordering and truncation, gates forward/backward/disc/miss, dead rows, zero rows), 40 random scenes of 64 to 960 rows where the broadphase output equals the brute-force output on every output lane whenever no truncation flag is set (34 scenes; the 6 truncated scenes are checked for repeatability), the candidate cap and the row limit.
