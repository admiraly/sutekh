# M3-00 contract r1: RTS microcosm (synthetic hordes, formations, movement, targeting, projectiles, damage, spatial load)

Status: DRAFT r1 for owner review. Design only; no product code is part of this contract. Builds on M2-00 (generational entities, IR 0.2, per-chunk struct/event buffers, snapshot 0.2) and M2-01 (migrations, snapshot 0.3, recovery log). Nothing here is accepted until the owner accepts this revision. Implementation of any task below is gated on M2 owner acceptance (`planning/status.json` shows M2 `implemented_audited_pending_owner_acceptance`).

Scope: a synthetic, non-BFME RTS profile. No original game assets, no BFME behaviour claim (C18, C19; M4 owns compatibility). Headers `include/sutekh/abi.h`, `ir.h`, `entities.h`, `status.h`, `src/runtime/schedule.h` are NOT changed by this contract. Every shared-interface change listed in section 13 requires its owner's approval.

---

## 0. Decisions at a glance

| # | Decision | Why (short) |
|---|---|---|
| D1 | **No new IR opcodes for M3.** Boolean composition uses nested `select_u32`; materialize a bool from a 0/1 u32 flag with `eq_u32(flag, 1)`; min via `lt`+`select`; abs-difference via `lt`+`sub`+`select`. | Every M3 per-row rule is expressible in the current 0.1/0.2 table (section 6). New opcodes would be speculative (C18). Re-evaluate only if measured instruction counts or authoring error rates fail (section 6.4). |
| D2 | **Indexed gather, scatter-add and the spatial index are host-side RTS-profile phase operators** (`rts_ops_v1`), not IR. | IR loads read only the current row, so a runtime-indexed read or a write to another entity is inexpressible, not just awkward (LIVE_IR_SPEC section 1, 5). Operators have fixed, bounded, reference-modelled semantics (section 4). |
| D3 | **Death is `despawn_self` in an IR 0.2 system** (reference VM only). Movement, goal and combat are IR 0.1 (native-eligible). | Keeps the native lane (IR 0.1 only) useful for hot per-row math while death uses the M2 path. |
| D4 | **Projectiles are per-shooter embedded state with a fixed flight time** (no spawn from IR, no homing geometry, no division). | Spawn-from-IR is reserved/unsupported in M2. A fixed flight time avoids division and keeps the state a plain row. |
| D5 | **Cross-entity damage is `scatter_add_u32` keyed by a generational target handle**, sum modulo 2^32. | Wrap-add is commutative and associative, so the merge order cannot change the result; canonical order is still recorded (section 4.3). |
| D6 | **Two control-plane additions are needed**: `world.set_fields` (orders, formation membership) and `world.spawn_batch` (scene loading at scale). | Orders and formation changes mid-run are otherwise impossible; per-row `world.spawn` at 10^5-10^6 rows is one recovery-log entry per row. Owner decision OD-3. |
| D7 | **Spatial index**: dense 128 x 128 grid of 256-unit cells over the 32768^2 domain; bounded query of at most 25 cells and at most 64 examined entries per query, with an explicit `truncated` flag. | Bounded work per query (constitution C10 and LIVE_IR_SPEC section 7); truncation under congestion is a reported observation, not hidden. |
| D8 | **Numeric profile `rts_u32_v1`**: all fields u32; coordinates 0..32767; squared distances < 2^31; no division in IR; host operators use only compare, add, sub, shifts and an exact integer square root. | No float (C08); no overflow in distance math by construction. |
| D9 | **Six scheduled systems** (`rts.spatial`, `rts.goal`, `rts.combat`, `rts.move`, `rts.scatter`, `rts.damage`), under the existing `SU_SCHED_MAX_SYSTEMS` = 8. | No scheduler cap change needed. |

---

## 1. Entity model and identity

- One entity table, all rows are `u32` columns. Row kinds are data, not types:
  - `kind = 1` unit (participates in combat and the spatial index).
  - `kind = 2` formation anchor (moves, carries no weapon, is never targeted, never damaged).
  - `kind = 0` reserved; never valid for a live row (validation rejects it).
- Handles are the M2 `SuEntity` (`generation << 32 | slot`). Every generation-checked reference in the schema stores the slot (u32) and the generation (u32) as two fields. Initial spawns use generation 1.
- Capacity is fixed at `world.create` (`max_entities`). Scenes never grow capacity. Spawn and despawn churn are bounded by capacity (M2 lowest-free-slot reuse).
- Dead rows are skipped by IR systems (VM alive mask) and by host operators (explicit `alive` check). Dead rows are canonically zero after every system commit (M2-00 section 1).

## 2. Schema and column layout

All fields are `u32`, stored as SoA columns, chunked by `SU_CHUNK_SLOTS` (1024) in the storage layer. `NONE = 0xFFFFFFFF`. `BIAS = 16384`. Field ids are assigned in the order below, starting at 1, and are stable within a lineage (M2-01 rule). Access lists name the systems that read or write the field; the verifier derives IR effects from IR, and host operators declare theirs (section 4.4).

| id | name | role | written by (store) | read by |
|---|---|---|---|---|
| 1 | `kind` | 0 invalid, 1 unit, 2 anchor | control plane only | spatial, combat, scatter, damage (via grid filter) |
| 2 | `team` | 0 or 1 | control plane | spatial |
| 3 | `hp` | hit points, >= 1 at spawn for units; 0 for anchors | damage | damage, spatial |
| 4 | `pos_x` | 0..32767 | move; control | spatial, goal, move |
| 5 | `pos_y` | 0..32767 | move; control | spatial, goal, move |
| 6 | `speed` | units per tick, 0..1024 | control | move |
| 7 | `range_sq` | weapon range squared, 0 = unarmed, <= 262144 | control | spatial, combat |
| 8 | `reload` | cooldown ticks after firing, 1..1024 | control | combat |
| 9 | `cooldown` | remaining ticks | combat | combat |
| 10 | `damage` | damage per hit, 1..65535 | control | combat |
| 11 | `flight` | flight ticks, 1..64 (fixed per weapon) | control | combat |
| 12 | `order_mode` | 0 none, 1 move (ignores enemies), 2 attack-move | control | goal, move |
| 13 | `order_x` | 0..32767 | control | goal |
| 14 | `order_y` | 0..32767 | control | goal |
| 15 | `formation_slot` | anchor slot, or NONE | control | spatial (gather), goal |
| 16 | `formation_gen` | generation of anchor at assignment | control | spatial (gather) |
| 17 | `formation_ox` | offset x + BIAS, offset in [-16384, 16383] | control | goal |
| 18 | `formation_oy` | offset y + BIAS | control | goal |
| 19 | `goal_x` | derived goal | goal | move |
| 20 | `goal_y` | derived goal | goal | move |
| 21 | `anchor_x` | gathered anchor x (0 if invalid) | spatial | goal |
| 22 | `anchor_y` | gathered anchor y | spatial | goal |
| 23 | `anchor_valid` | 0/1 | spatial | goal |
| 24 | `target_slot` | nearest qualifying enemy slot, or NONE | spatial | combat, move |
| 25 | `target_gen` | generation of target | spatial | combat |
| 26 | `target_dist_sq` | squared distance to target (0 if none) | spatial | (diagnostic) |
| 27 | `target_valid` | 0/1, equals `target_slot != NONE` | spatial | combat, move |
| 28 | `target_x` | gathered target x | spatial | (diagnostic) |
| 29 | `target_y` | gathered target y | spatial | (diagnostic) |
| 30 | `truncated` | 1 if the query budget ended the scan early | spatial | (diagnostic, metrics) |
| 31 | `proj_active` | 0/1 projectile in flight | combat | combat |
| 32 | `proj_left` | ticks until arrival (0 when inactive) | combat | combat |
| 33 | `proj_target` | target slot captured at fire | combat | combat |
| 34 | `proj_target_gen` | target generation captured at fire | combat | combat |
| 35 | `proj_damage` | damage carried | combat | combat |
| 36 | `dmg_out` | damage emitted this tick (0 if none) | combat | scatter |
| 37 | `dmg_target` | target slot for this tick's arrival, or NONE | combat | scatter |
| 38 | `dmg_target_gen` | generation for this tick's arrival | combat | scatter |
| 39 | `incoming` | summed damage received this tick | combat (store 0), scatter (add) | damage |

Layout arithmetic: 39 fields x 4 bytes = 156 bytes per row per root. Memory accounting (section 9) uses this table.

Canonical state (M2-00 encoding v2) covers all 39 fields plus the entity table and event log. The schema descriptor hash (M2-01 section 6) covers the sorted `(id, type, name, default)` list. Field defaults are all 0 (M3 has no migration). Spawn requests therefore name every field whose intended value is not 0, and the scene generator writes `NONE` explicitly into `target_slot`, `dmg_target`, `proj_target` and `formation_slot` where NONE is intended; the rules in sections 4-5 never rely on a spawn-time default of NONE.

### 2.1 Numeric profile `rts_u32_v1`
- Domain: all positions and orders in `[0, 32767]`. Violations are rejected by the control plane (`SU_E_TYPE`, path = field name). Host operators treat out-of-domain as `SU_E_INTERNAL`.
- Squared distance: `dx = |x1 - x2|`, `dy = |y1 - y2|` each <= 32767; `dx*dx + dy*dy <= 2*32767^2 = 2147352578 < 2^31`. No intermediate wraps.
- u32 modular identities used in formation: `(a + ox - BIAS) mod 2^32 == a + offset` exactly when the true result is in `[0, 2^32)`. Anchor positions are in-domain, so `a + offset` may be negative; the clamp (section 5.2) handles that through the 2^31 sign threshold.
- No division in IR. Host operators use integer shifts and compares only, plus an exact integer square root implemented by the integer Newton method (reference in the Python model).

## 3. Scheduler and phases

Per tick, after control-plane commands for the boundary have been applied (M2-00 section 2), the tick runs these phases in this order. Each IR system runs as one job per storage chunk (reference VM). Host operators run as listed in section 4.

| idx | system id | kind | native-eligible | reads (summary) | writes (summary) |
|---|---|---|---|---|---|
| 0 | `rts.spatial` | host operator, 4 sub-steps (4.1) | no | alive, kind, team, hp, pos, formation_*, range_sq | 21-30 |
| 1 | `rts.goal` | IR 0.1 | yes | order_*, anchor_*, formation_o*, pos | 19, 20 |
| 2 | `rts.combat` | IR 0.1 | yes | target_valid, target_slot, target_gen, cooldown, reload, damage, flight, proj_* | 9, 31-39 (see 5.2) |
| 3 | `rts.move` | IR 0.1 | yes | pos, goal, speed, order_mode, target_valid | 4, 5 |
| 4 | `rts.scatter` | host operator (4.3) | no | dmg_*, alive, generation, kind | 39 (of target rows) |
| 5 | `rts.damage` | IR 0.2 (despawn_self) | no (reference VM only) | hp, incoming | 3 (and despawn) |

Order rationale: spatial reads tick-start state; goal and combat read spatial outputs; move reads goal and `target_valid`; combat zeroes `incoming` before scatter adds; scatter precedes damage; deaths take effect at the boundary merge.

Semantics fixed by this order: all loads see tick-start values (IR rule) and each host operator reads the state committed by the previous phase. Combat decides firing with the position from tick start; move then changes position. This is the documented M3 ordering, not a race.

Count: 6 systems <= `SU_SCHED_MAX_SYSTEMS` (8). Chunk parallelism (M1-style) is safe for spatial query and gather (each row writes only its own row, reads only frozen grid/tick-start state), for IR 0.1 systems, and for damage; scatter is serial in M3 (section 4.3).

Boundary (after phase 5): merge in canonical order `(system_index, slot, seq)`; apply `world.despawn` requests emitted by `rts.damage` (one `SU_EVENT_DESPAWNED` each); zero dead slots; publish. Tick abort cases follow M2-00 (`SU_E_RESOURCE_LIMIT` leaves nothing published).

## 4. Host-side phase operators (`rts_ops_v1`)

Common rules: integer-only; read only frozen inputs (tick-start state for `rts.spatial`, pre-scatter state for scatter); allocation only at admission (no allocation in the tick, C11); buffers sized from capacity and the declared bounds below; every operator is reference C (`profiles/rts/native/`) and a Python model (section 8).

### 4.1 `rts.spatial` (four sub-steps, run in this order)

Let `cap` = capacity, `alive(s)` = slot `s` alive at tick start.

(a) **Anchor gather**: for each alive row `r` with `formation_slot != NONE`, `anchor_valid(r) = gather_valid(formation_slot, formation_gen)`; if valid, `anchor_x/y = pos` of that slot, else 0. `gather_valid(s, g)`: `s != NONE && s < cap && alive(s) && generation(s) == g`. Rows not valid write `anchor_valid = 0`, `anchor_x = anchor_y = 0`. Rows with `formation_slot == NONE` write `anchor_valid = 0` too.

(b) **Grid build** (serial, global): cell `c(x,y) = (y >> 8) * 128 + (x >> 8)` for alive rows with `kind == 1 && hp > 0`. Counting sort into `cell_start[16385]` and `cell_items[N_idx]`, items in ascending slot order within each cell. Memory: 16384 counts plus 16385 starts (u32) plus item count (u32 per indexed row). Built once per tick.

(c) **Target query** (per alive row `r` with `kind == 1 && range_sq > 0`; others write `target_slot = NONE, target_gen = 0, target_dist_sq = 0, target_valid = 0, truncated = 0`):
1. `R = isqrt(range_sq)` (floor). Radius in cells `rc = (R + 255) >> 8`. Reject at profile validation if `rc > 2` (i.e., `range_sq > 262144`); the spatial phase treats it as `SU_E_INTERNAL` if reached.
2. Cell box: `cx = x >> 8`, `cy = y >> 8`; box `[max(0,cx-rc), min(127,cx+rc)] x [max(0,cy-rc), min(127,cy+rc)]`. Visit cells in row-major order (y ascending, then x ascending). At most 25 cells.
3. Budget `K = 64` examined entries (self and friendly entries count when examined). For each entry `e` in the cell (ascending slot) before budget exhaustion:
   - Count `examined += 1`. If `examined > K`, stop: `truncated = 1` only if at least one entry remained unexamined in the box, else 0.
   - Skip `e == r`. Skip if `team(e) == team(r)`.
   - `dist_sq = dx^2 + dy^2` (rts_u32_v1 domain). Qualify iff `dist_sq <= range_sq(r)`.
   - Keep best = minimum of `(dist_sq, slot)` lexicographically (lower slot breaks ties).
4. Result: `target_slot = best.slot or NONE`, `target_gen = generation(best.slot)`, `target_dist_sq = best.dist_sq or 0`, `target_valid = (best exists)`.
5. Work bound per query: at most 25 cell visits and 64 entry examinations plus constant work. Per tick worst case is `N_armed * (25 + 64)` basic steps.

(d) **Target gather**: `target_x/y` = pos of `target_slot` when `target_valid` (else 0).

Note: `target_valid` implies `dist_sq <= range_sq`; combat need not re-check range.

### 4.2 Truncation policy
`truncated = 1` means the nearest-enemy answer is the best found within budget and may not be the true nearest. It is deterministic. It is an observable scene statistic (section 9), never silently ignored. A high truncation rate in congested scenes is a finding to report, not a bug.

### 4.3 `rts.scatter` (`scatter_add_u32`)
For each alive row `r` in ascending slot order with `dmg_target(r) != NONE` and `dmg_out(r) != 0`:
- Let `t = dmg_target(r)`. Apply iff `t < cap && alive(t) && generation(t) == dmg_target_gen(r) && kind(t) == 1`.
- Then `incoming(t) = (incoming(t) + dmg_out(r)) mod 2^32`.
- Writes only field 39 of target rows. Reads: fields 36-38, `alive`, `generation`, `kind`.
- Determinism: wrap-add is commutative and associative, so the result is independent of order. The canonical row order is still used so that a future saturating or non-commutative combine is not silently order-dependent (equivalent mutant M3-M09b in section 15).
- Serial in M3. A later parallel version uses per-chunk partial sums merged in canonical chunk order.

### 4.4 Declared effects (C04 gap, owner decision OD-5)
Host operators are not verified from IR. Each operator declares its effects in its registration record (read set, write set, per-row or global). An instrumented debug build records every field store per phase and asserts the store set equals the declaration (M3-04 test). Native or release builds skip the instrumentation; the claim is then "declared and tested", not "verified".

## 5. IR systems (IR 0.1 unless noted)

All systems below are row-local and use only `load_u32`, `const_u32`, `add/sub/mul_u32`, `eq/lt_u32`, `select_u32`, `store_u32` (IR 0.1) plus `despawn_self` (IR 0.2 for damage). Each declared writable field is stored exactly once per row (verifier rule). Pseudocode uses bool/flag names; the JSON realization of each flag is in section 6.

### 5.1 `rts.goal` (IR 0.1)
Reads: `order_mode, order_x, order_y, anchor_valid, anchor_x, anchor_y, formation_ox, formation_oy, pos_x, pos_y`. Writes: `goal_x, goal_y`.

```
has_order  = order_mode != 0                  (flag u32 = select(eq(order_mode,0), 0, 1))
av         = anchor_valid == 1
raw_x      = select(has_order, order_x, select(av, anchor_x + formation_ox - BIAS, pos_x))
raw_y      = same with y fields
goal_x     = clamp_domain(raw_x)
clamp_domain(v) = select(lt(2147483647, v), 0, select(lt(32767, v), 32767, v))
```
Priority: an explicit order (mode 1 or 2) wins over formation membership; a formation member with no order follows its anchor; otherwise the goal is the current position (hold). `lt(2147483647, v)` detects wrapped negatives because `|true value| < 2^31`.

### 5.2 `rts.combat` (IR 0.1)
Reads: `target_valid, target_slot, target_gen, cooldown, reload, damage, flight, proj_active, proj_left, proj_target, proj_target_gen, proj_damage`. Writes: `cooldown, proj_active, proj_left, proj_target, proj_target_gen, proj_damage, dmg_out, dmg_target, dmg_target_gen, incoming`.

```
tv        = target_valid == 1
cd0       = cooldown == 0
pa0       = proj_active == 0
fire_flag = select(tv, select(cd0, select(pa0, 1, 0), 0), 0)     (u32 0/1)
fire      = eq(fire_flag, 1)
arrive    = proj_active == 1 && proj_left == 1  -> flag = select(eq(proj_active,1), select(eq(proj_left,1),1,0), 0), arrive = eq(flag,1)

cooldown'     = select(fire, reload, select(lt(0,cooldown), cooldown-1, 0))
proj_active'  = select(fire, 1, select(arrive, 0, proj_active))
proj_left'    = select(fire, flight, select(arrive, 0, select(lt(0,proj_left), proj_left-1, 0)))
proj_target'  = select(fire, target_slot, select(arrive, NONE, proj_target))
proj_target_gen' = select(fire, target_gen, select(arrive, 0, proj_target_gen))
proj_damage'  = select(fire, damage, select(arrive, 0, proj_damage))
dmg_out'      = select(arrive, proj_damage, 0)
dmg_target'   = select(arrive, proj_target, NONE)
dmg_target_gen' = select(arrive, proj_target_gen, 0)
incoming'     = 0
```
Timing: a shot fired at tick `t` arrives at tick `t + flight` (fire sets `proj_left = flight`; each later tick decrements; arrival when the value before decrement is 1). `flight >= 1` is validated. A unit with an active projectile cannot fire again (`pa0`), so shot rate is bounded by `max(reload, flight+1)`. Projectile state belongs to the shooter row: a despawned shooter's in-flight projectile disappears with it (no damage; M3-T-PROJ-02).

`incoming' = 0` is the zeroing that makes scatter additive per tick.

### 5.3 `rts.move` (IR 0.1)
Reads: `goal_x, goal_y, pos_x, pos_y, speed, order_mode, target_valid`. Writes: `pos_x, pos_y`.

```
mv     = select(eq(order_mode,1), 1, select(target_valid==1, 0, 1))  -> moving = eq(mv,1)
right  = lt(pos_x, goal_x);  left = lt(goal_x, pos_x)
ax     = select(right, goal_x - pos_x, pos_x - goal_x)             (abs difference, 0 if equal)
m      = select(lt(ax, speed), ax, speed)                           (min)
step_x = select(right, pos_x + m, select(left, pos_x - m, pos_x))
pos_x' = select(moving, step_x, pos_x)
(same for y)
```
Mode 1 (move) ignores enemies. Mode 2 (attack-move) and mode 0 stop when a target is valid, so engagement is "stop and fight"; no stop-and-fight exists for anchors (no weapon). Position stays in domain because it moves monotonically toward an in-domain goal.

### 5.4 `rts.damage` (IR 0.2, `despawn_self`)
Reads: `hp, incoming`. Writes: `hp`. Emits: `despawn_self` (cond).

```
m        = select(lt(incoming, hp), incoming, hp)          (min)
hp'      = hp - m                                           (saturating by construction)
kill_flg = select(eq(hp', 0), select(lt(0, hp), 1, 0), 0)   (hp was > 0, now 0)
despawn_self if eq(kill_flg, 1)
```
Anchors have `hp = 0` and `incoming = 0`, so they never die by damage. Declared caps: `max_struct_per_row = 1`, `max_events_per_row = 0`. The reference VM runs this system only (native refuses 0.2, M2 rule).

## 6. IR expressiveness: why no new opcode is needed (D1)

### 6.1 Realization rules
- Boolean composition of conditions is done by nested selection on values, never by materializing bool AND/OR. Example: `fire = AND(tv, cd0, pa0)` is `fire_flag = select(tv, select(cd0, select(pa0,1,0), 0), 0)` then `fire = eq(fire_flag, 1)`.
- A bool from a flag: `eq_u32(flag, 1)`. A flag from a bool: `select_u32(b, 1, 0)`.
- `min(a,b) = select(lt(a,b), a, b)`. Abs difference: `select(lt(a,b), b-a, a-b)`. Clamp: section 5.1.
- `le(a,b) = not lt(b,a)` via `select(lt(b,a), 0, 1)`.
- All constants are `const_u32` registers; a const 0 for `lt(0, x)` uses one register.

### 6.2 Instruction budget
Estimates (to be replaced by measured counts in M3-03): `rts.goal` ~ 60 instructions, `rts.combat` ~ 110, `rts.move` ~ 50, `rts.damage` ~ 25. All far below `SU_LIMIT_INSTRUCTIONS` (4096) and `SU_LIMIT_REGISTERS` (4096). Registers are per row and freshly defined; no register survives a row.

### 6.3 What is not expressible in IR and why host operators are required
- Indexed read (`gather`): `load_u32` reads only the current row. Formation anchors and targets need another row's data.
- Indexed write (`scatter`): a row writing another entity's field is cross-entity addressing, forbidden by LIVE_IR_SPEC section 1.
- Neighbour search: needs iteration over a variable-length bucket. A K-unrolled IR version would need around 64 candidate steps of about 20 instructions each per system plus cross-row gathers that IR cannot express, so it is not viable.

### 6.4 Re-evaluation trigger
If M3-03 measures any system above 2048 instructions, or the M3-07 agent experiment shows more than a fixed rate of verifier rejections caused by bool-flag encoding, the owner reviews a narrow IR 0.3 proposal: `bool_and`, `bool_or`, `bool_not`, `min_u32`. Indexed gather and scatter remain host operators regardless.

## 7. Scene generators (seeded, deterministic, committed)

### 7.1 PRNG (`splitmix64_v1`)
State is u64. `next()`: `state = state + 0x9E3779B97F4A7C15 (mod 2^64)`; `z = state`; `z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9 (mod 2^64)`; `z = (z ^ (z >> 27)) * 0x94D049BB133111EB (mod 2^64)`; `z = z ^ (z >> 31)`; `u32 = z >> 32`. `uniform(n) = (u32 * n) >> 32` (multiply-high, no modulo bias, `n <= 2^32`). Seeds are u64.

### 7.2 Canonical scene encoding (digest input)
`rts-scene-canon/0.1`: line 1 is canonical JSON (keys sorted, no whitespace) of `{ "format":"rts-scene-canon/0.1", "generator":<id>, "params":<object>, "seed":<decimal>, "schema_digest":"sha256:<hex>", "capacity":N, "rows":N }`. Then for each slot `0..N-1` the 39 field values as u32 little-endian (156 bytes per row, in the field-id order of section 2). The scene digest is SHA-256 of these bytes. Python and C generators must produce identical digests.

### 7.3 Generators and scenes

| id | generator | purpose | N (committed) |
|---|---|---|---|
| `duel.fixture.v1` | hand-specified rows (no PRNG) | edge fixtures with exact expected outcomes (section 10.2) | 8 to 64 |
| `mixed.battle.v1` | seeded; two bands on x, formations of 16 members (anchor + 16 per group), 3 weapon kinds (table 7.4) | representative mixed scene | 1,024; 16,384 |
| `congested.blob.v1` | seeded; all units inside a 2048 x 2048 box in the middle, two teams interleaved, formations | congestion, truncation, grid density | 1,024; 16,384 |
| `sparse.field.v1` | seeded; uniform positions over the domain, sparse teams | spatial baseline, few queries hit | 16,384; 65,536 |

Scene slot order: for each group `g` in index order: the anchor slot, then its members in member index order. Initial generations are 1. Formation offsets: member `i` in a group has column `i mod 4` and row `i div 4`; `offset_x = col*32 - 48`, `offset_y = row*32 - 48`, stored as `formation_ox = offset + BIAS`.

### 7.4 Weapon table (synthetic, not BFME)

| kind | range_sq | reload | flight | damage | speed | hp |
|---|---|---|---|---|---|---|
| rifle | 65536 (256) | 6 | 2 | 3 | 96 | 12 |
| bow | 262144 (512) | 10 | 5 | 5 | 64 | 10 |
| melee | 256 (16) | 2 | 1 | 2 | 128 | 20 |

Team placement in `mixed.battle.v1`: team 0 x in `[1024, 8191]`, team 1 x in `[24576, 31743]`, y uniform in `[0, 32767]`; group orders attack-move (mode 2) to the opposite band centre. Congested: positions uniform in the central box; orders attack-move to the box centre.

### 7.5 Committed lock
`bench/rts/scenes.lock.json` lists per scene: generator id, `params`, `seed`, `capacity`, the scene digest, and the schema digest. Regeneration must reproduce every digest; a mismatch fails the lock check (`bench/rts/gen/check_lock.py`). Large scenes (>= 65,536 rows) are generated in process and never stored; only their spec and digest are committed.

## 8. Independent Python reference model

- Location: `bench/rts/ref/` (`model.py`, `ir_eval.py`, `isqrt.py`); tests in `tests/rts/ref/`. Python 3 standard library only; no import from the engine or from `profiles/rts/native/`.
- Written from this contract only. The author of `model.py` must not read the C operator sources (M3-04) before the model's golden outputs are fixed. A different agent or person reviews the model against the contract text.
- Two parts:
  1. **Semantic model** of phases 0-5 and merge, written directly from sections 4 and 5 (not from the IR JSON). It is the oracle for scene digests.
  2. **IR evaluator** (`ir_eval.py`): interprets only the committed IR 0.1 and 0.2 JSON with the M0/M2 opcode semantics. A differential test asserts that for random rows the evaluator output equals the semantic model's per-row outputs.
- Outputs per tick: per-phase field digests and the canonical state digest, using the same canonical layout as the engine (section 9.2).
- Scale: the Python oracle covers scenes up to 16,384 rows over a bounded tick count (time budget set by M3-02). Larger scenes are compared to the C engine by digest for the reference VM only where Python completed; otherwise the record says `oracle: not_run`.

## 9. Replay format, digests and memory accounting

### 9.1 Replay file `rts-replay/0.1` (JSON Lines)
- Line 1, header: `{"format":"rts-replay","version":"0.1","numeric_profile":"rts_u32_v1","scene":{"generator","params","seed","capacity","scene_digest"},"schema_digest","system_manifest_hash","engine":{"artifact_hashes":[...],"source_revision"},"caps":{"max_struct_per_tick","max_events_per_tick","event_log_max"},"digest_every":K,"ticks":T}`.
- Command records: `{"t":<tick>,"seq":<n>,"cmd":"world.set_fields","args":{...}}`, `"cmd":"world.spawn_batch"`, `"cmd":"world.despawn"`. `t` is the boundary tick at which the command is applied (before that tick runs). Within a boundary, commands are applied in `seq` order.
- Digest records: `{"t":<tick>,"state":"sha256:...","events":"sha256:...","counts":{"alive","retired","events","truncated_rows"}}` every `digest_every` ticks and on the final tick.
- Replay validation: re-run from the header and the commands; compare each digest record; on mismatch report the first differing tick, the first differing `(field_id, slot)`, and the expected and actual values. Exit status is nonzero on any mismatch or missing record.
- Replay across tick-worker counts 1 and 2 (`SUTEKH_TICK_WORKERS`) must give the same digests; this is a test of the IR 0.2 and host operator determinism, not a performance claim.

### 9.2 Canonical digests
State digest is the M2 recovery digest (`SHA-256(canonical_state || layout_generation_u32_le || schema_descriptor_sha256)`) plus the 0.2 canonical state: rows `0..high_water-1`, entity table, retired count, event counters. Event digest is SHA-256 over canonical JSON lines of the event records in `event_seq` order. Canonical JSON uses sorted keys, no whitespace, decimal integers.

### 9.3 Memory accounting (observations, not targets)
Accounted categories, all computed from capacity and configured limits, reported in bytes:
- columns: `capacity * 39 * 4 * roots`, `roots = 2` (committed and working) plus staging `capacity * 4 * max_fields_written_by_one_system` (combat writes 10 fields, so 40 bytes per row). Estimate ~352 bytes per row.
- entity table: `capacity * 12` (generation, alive, external id). Exact layout from M2-00.
- spatial: `16384*4` counts + `16385*4` starts + `N_idx*4` items.
- per-chunk struct and event buffers: admitted sizes from M2-00 admission formula, reported at admission.
- event log: `event_log_max * record_bytes`.
- checkpoint spool: canonical state bytes, present only while a checkpoint is retained.
Cross-check: process `VmHWM` (from `/proc/self/status`) sampled at the end of each run and the allocator-level count where available. Any category that cannot be measured is `null` with a reason (C10). The accounting API (for exact per-root bytes) is an owner-approved addition (OD-10); until then the harness reports the analytic figures and marks them as estimates.

## 10. Tests and acceptance

### 10.1 Reference-behaviour tests (per system, known answers from the Python model)
- `T-RTS-GOAL-*`: explicit order overrides formation; formation member follows anchor with offset; invalid anchor holds; clamp of wrapped negative and over-domain values to 0 and 32767.
- `T-RTS-MOVE-*`: arrival exactly at goal; step equals `min(distance, speed)` on each axis; mode 1 ignores target; mode 2 stops when `target_valid`; speed 0 never moves; no overshoot.
- `T-RTS-COMBAT-*`: fire at cooldown 0 with target; no fire with active projectile; arrival at `t + flight` exactly (flight 1 and flight 5 edges); cooldown reload; cooldown never underflows; `incoming` zeroed each tick.
- `T-RTS-DAMAGE-*`: partial damage; exact kill with `incoming == hp` (kill emitted once); overkill saturates; anchors never killed; kill emits exactly one `SU_EVENT_DESPAWNED` at the boundary.
- `T-RTS-SPATIAL-*`: nearest-enemy by `(dist_sq, slot)` with ties; dist exactly equal to `range_sq` qualifies, `range_sq + 1` does not; cell boundary at x=255/256 and y=255/256; box clipped at domain edges (0 and 32767); self excluded; same-team excluded; dead excluded; truncation at exactly K examined and at K+1 with the flag; generation mismatch reported as invalid gather.
- `T-RTS-GATHER-*`: stale anchor (generation mismatch, despawned, out of range, NONE) yields `anchor_valid = 0`; zero-value legitimate positions are distinguishable through the flag.
- `T-RTS-SCATTER-*`: sum wrap at 2^32; dead target ignored; stale generation ignored; non-unit target ignored; shooter despawned before arrival causes no damage; multiple shooters on one target summed.

### 10.2 Edge fixtures (`duel.fixture.v1`, exact expected outcomes from the model)
A minimal set: two units at distance exactly `range_sq`; one unit at x=0 and one at x=32767; equal-distance candidates (lower slot wins); a bow with `flight = 5` against a target that dies in flight; a target with `hp = 3` hit by two shooters in one tick (killed once, incoming 6); a formation whose anchor is despawned mid-run; slot reuse after despawn with a stale projectile target (the stale shot must not hit the new occupant).

### 10.3 Scene and replay tests
- `T-RTS-SCENE-1`: generator run twice gives identical bytes and digests; digest changes when any single PRNG constant or one field value changes (mutation).
- `T-RTS-SCENE-2`: Python and C generators produce identical digests for every committed scene up to 16,384 rows.
- `T-RTS-REPLAY-1`: record then replay gives identical digest records; replay with tick workers 1 and 2 identical.
- `T-RTS-REPLAY-2`: a single flipped command byte, or a dropped `world.set_fields`, reports the first divergent tick.
- `T-RTS-REPLAY-3`: kill the worker mid-run, recover from checkpoint plus log, continue, and compare digests with an uninterrupted run.
- `T-RTS-CHURN-1`: spawn and despawn churn of at least 100,000 operations on a 16k scene with reference model comparison (inherits M2 churn test on RTS fields).
- `T-RTS-CAPS-1`: deaths in one tick above `max_struct_per_tick` abort the tick with `SU_E_RESOURCE_LIMIT`; state unchanged (M2 rule), and the replay records the configured cap.

### 10.4 Verifier and IR negatives (M3-03)
Each code path: missing store (`SU_E_MISSING_STORE`), undeclared write (`SU_E_UNDECLARED_WRITE`), undeclared read (`SU_E_UNDECLARED_READ`), 0.2 op in a 0.1 module (`SU_E_UNKNOWN_OPCODE`), `despawn_self` count above `max_struct_per_row` (`SU_E_RESOURCE_LIMIT`), `emit_event` absent, `ir_version` mismatch (`SU_E_SCHEMA_MISMATCH`), duplicate register (`SU_E_DUPLICATE_REGISTER`), undefined register (`SU_E_UNDEFINED_REGISTER`), native lowering refused for 0.2 (`SU_E_CAPABILITY_DENIED`).

### 10.5 Agent-authored behaviours (M3-07)
Section 11.

### 10.6 Performance and stress (M3-08)
No timing targets are asserted. Tests check that output files exist, schema-valid, with hardware, OS, toolchain, source and artifact hashes present (C10).

## 11. Agent-authored behaviours evidence plan (M3-07)

Three behaviour tasks, each to be done by an agent through the existing candidate pipeline (verifier, reference tests, `world.candidate` path; AGENT_PROTOCOL), with hidden acceptance fixtures written by a different author from the Python model:
- **B1 low-hp retreat**: units with `hp <= 3` move toward their team's base point (a per-team constant in IR) and stop attacking. IR-only change to `rts.goal` and `rts.combat`.
- **B2 projectile retarget guard**: a projectile whose target generation changed while in flight is cancelled instead of applied. IR-only change to `rts.combat`; the scatter-side check must still hold.
- **B3 formation hold**: members with `anchor_valid = 0` and no order hold position instead of drifting. IR-only change to `rts.goal`.

Evidence record per task (JSON, stored in `bench/rts/agent_experiments/records/`): task id, agent and session identifier, model identifier, number of candidate submissions, verifier rejections by code, reference test failures by fixture id, hidden fixture result, wall time to accept (or failure), native-lane status (not applicable, as all three remain IR 0.1 or reference-only), null tokens and tool calls with reason `not_recorded` if the harness did not record them. Acceptance: the candidate passes hidden fixtures and the full existing suite. The record is not a claim of general agent capability.

## 12. Stress curves and memory methodology (M3-08)

- Harness: `bench/rts/stress/`. Sweep scene sizes over a geometric series of powers of two starting at 1,024 (sweep points are a methodology choice, not a unit-count claim). For each size, run `T` ticks (default 120) of `mixed.battle.v1` and of `congested.blob.v1`.
- Stop rule per curve: stop at the first `SU_E_RESOURCE_LIMIT` (record which limit), at an operator-set memory ceiling, or at an operator-set wall-clock ceiling per run. Record the largest size completed and the first failing size with its code. No size is predeclared as solved.
- Per tick, record per-phase monotonic nanoseconds (spatial sub-steps a-d, goal, combat, move, scatter, damage, merge, digest when taken). Per run: p50, p95, p99 and max over ticks, with sample counts. Also record the truncation fraction, deaths per tick, projectiles per tick, and `max_struct_per_tick` actually configured.
- Hardware and environment block: CPU model, vCPU count, memory, OS, compiler versions, build flags, `SUTEKH_TICK_WORKERS`, artifact hashes. Measurement mode `dev_checked` or `perf_trusted` is stated explicitly. Shared VM timings are observations (C10).
- Memory: section 9.3 analytic figures plus `VmHWM` per run; differences between the two are reported, not hidden.
- Actual limits found are recorded as data: configured `SU_LIMIT_ROWS` (default 1,000,000), capacity used, per-tick caps, and the largest completed size.

## 13. Interface changes and owner decisions

| id | item | change class | owner |
|---|---|---|---|
| OD-1 | Host-side operators (`rts_ops_v1`) instead of IR gather/scatter (D2) | design | orchestrator plus M3 owner |
| OD-2 | No IR opcode additions in M3 (D1); IR 0.3 proposal only if section 6.4 triggers | design | IR owner |
| OD-3 | `world.set_fields {world_id, expected_world_revision, entity, values}`: batch write of non-key fields between ticks, stale handle `SU_E_STALE_HANDLE`, stale revision `SU_E_STALE_REVISION`, one recovery-log entry, emits no event | shared contract (new control method) | ABI/control owner |
| OD-3b | `world.spawn_batch {world_id, expected_world_revision, rows:[{values, external_id?}], max 4096 per call}`: one recovery-log entry per call, slots in given order | shared contract (new control method) | ABI/control owner |
| OD-4 | `SU_SCHED_MAX_SYSTEMS` stays 8; M3 uses 6 | none | - |
| OD-5 | Declared-and-tested (not verified) effects for host operators (4.4) | design risk accepted | owner |
| OD-6 | Query budget K = 64, cells <= 25, cell 256 units, 128 x 128 grid | profile constant | RTS profile owner |
| OD-7 | No separation, avoidance, pathfinding or obstacle map in M3 | scope | owner |
| OD-8 | One projectile per shooter with fixed flight time | scope | owner |
| OD-9 | `rts_u32_v1` numeric profile and 15-bit domain | profile | owner |
| OD-10 | Exact per-root memory accounting API in `src/world` (for 9.3) | native_core | ABI/world owner |
| OD-11 | Native lane for goal/combat/move (M3-10), gated on M1 owner acceptance | native_core, later | owner |
| OD-12 | Bulk scene path for N > 65,536 (binary scene, or spawn_batch only) | scope, later | owner |
| OD-13 | Whether a Python model written by a different agent under the same orchestrator counts as independent | process | owner |
| OD-14 | Scatter remains serial in M3 | scope | owner |

Profile errors reuse existing codes: `SU_E_INVALID_ARGUMENT` (malformed scene or command), `SU_E_TYPE` (out-of-domain value, `kind = 0` on live row, weapon out of table), `SU_E_RESOURCE_LIMIT` (`range_sq > 262144`, capacity, per-tick caps, operator buffers), `SU_E_SCHEMA_MISMATCH` (generator schema digest differs from loaded schema), `SU_E_STALE_HANDLE` and `SU_E_STALE_REVISION` (control commands), `SU_E_INTERNAL` (invariant violated inside an operator). No new status codes are proposed; a new code would require a `status.h` change and owner approval.

## 14. Task split (proposed replacement for the single M3-00 entry)

Note for the orchestrator (do not treat this as an edit to `planning/tasks.json`, which is not changed by this contract): the current `M3-00` entry in `planning/tasks.json` is broad ("Build synthetic RTS microcosm"). Proposed: keep `M3-00` as this contract's freeze and split the rest as below. Every task is `status: planned` until the owner accepts this revision.

| id | title | depends_on | write scope (lease) | change class | native validation | acceptance (summary) |
|---|---|---|---|---|---|---|
| M3-00 | Contract r1 freeze (this file) | M2-01 (design only) | `planning/contracts/M3_RTS_CONTRACT.md` | design | no | owner accepts or rejects OD-1..OD-14 |
| M3-01 | Schema, scene generators, scene lock | M3-00 | `profiles/rts/schema/`, `bench/rts/gen/`, `bench/rts/scenes.lock.json` | profile | no | generator reproducible; digests stable; PRNG and field-order mutants change digests |
| M3-02 | Independent Python reference model and IR evaluator | M3-00 | `bench/rts/ref/`, `tests/rts/ref/` | profile | no | section 10.1 known answers; IR-vs-model differential; model author does not read M3-04 sources |
| M3-03 | IR systems goal/combat/move/damage and authoring generator | M3-01, M3-02, M2-01 (IR 0.2 verifier) | `capsules/rts/`, `bench/rts/gen_ir.py` | gameplay | no (0.1 systems are native-eligible but native is not run here) | section 10.4 negatives; regenerate-equality; measured instruction counts recorded |
| M3-04 | Host operators: spatial index, gather, scatter (C17) | M3-01, M3-03 | `profiles/rts/native/` | native_core | yes | section 10.1 SPATIAL/GATHER/SCATTER; ASan, UBSan; instrumented store-set check (4.4) |
| M3-05 | Scheduler integration, `world.set_fields`, `world.spawn_batch`, replay record and replay | M3-03, M3-04, OD-3 approved | `src/runtime/rts/` (lease), `tests/rts/replay/` | schema/runtime | yes | T-RTS-REPLAY-1..3, T-RTS-CAPS-1; recovery-log entries for the two new methods |
| M3-06 | End-to-end suite, golden scenes, edge fixtures, mutation campaign | M3-02, M3-05 | `tests/rts/e2e/`, `tests/rts/fixtures/` | tests | yes (with engine runs) | C == Python digests for all committed scenes within oracle limits; all listed mutants detected |
| M3-07 | Agent-authored behaviours experiment (B1-B3) | M3-06 | `bench/rts/agent_experiments/` | evidence | no | records per section 11; hidden fixtures authored by a different party |
| M3-08 | Stress curves and memory methodology | M3-06 (and OD-10 for exact accounting) | `bench/rts/stress/` | evidence | no | raw logs with hashes, hardware blocks, null reasons; no predeclared unit-count claims |
| M3-09 | Independent review of any performance optimization | M3-08 and the optimization | reviewer write-free | review | per change | C10 evidence present; reviewer differs from author; digests unchanged |
| M3-10 | Native lane for goal/combat/move (optional) | M1 owner acceptance, M3-03, M3-06 | `backends/native/` (lease) | native_core | yes | differential vs reference VM across row tails and dead rows; capability fallback |

Mapping to MILESTONES M3 closure evidence: deterministic replay (M3-05, M3-06); agent-authored behaviours (M3-07); end-to-end tick costs (M3-08); stress curves and memory accounting (M3-08); independently reviewed optimizations (M3-09). M3-10 is optional and does not block closure.

## 15. Mutation requirements (minimum set; each must be detected by a named test)

| id | mutant | detected by |
|---|---|---|
| M3-M01 | tie-break on slot reversed (higher slot wins) | T-RTS-SPATIAL tie case, edge fixture |
| M3-M02 | cell index off by one column (`(x>>8)+1`) | T-RTS-SPATIAL boundary x=255/256 |
| M3-M03 | range check uses `<` not `<=` | T-RTS-SPATIAL exact-equal case |
| M3-M04 | query budget ignored (no truncation) | T-RTS-SPATIAL K and K+1 cases; truncation counter |
| M3-M05 | generation check removed from gather | T-RTS-GATHER stale anchor; T-RTS-SCATTER stale projectile after slot reuse |
| M3-M06 | scatter applies to dead target | T-RTS-SCATTER dead-target case |
| M3-M07 | scatter applies to non-unit (kind 2) target | T-RTS-SCATTER non-unit case |
| M3-M08 | scatter uses saturating add | T-RTS-SCATTER sum-wrap case (2^32 boundary) |
| M3-M09 | scatter applies only the first shooter's damage per target per tick (drops later contributions) | T-RTS-SCATTER multiple-shooters case, two-shooter edge fixture |
| M3-M09b | scatter iterates in descending slot order | Equivalent under wrap-add (section 4.3): not detectable by sums or digests. Recorded as an equivalent mutant, not as detected; it is covered only by the canonical-order check if a non-commutative combine is ever added |
| M3-M10 | flight off by one (arrival at `t + flight - 1` or `+ 1`) | T-RTS-COMBAT flight 1 and 5 edges |
| M3-M11 | cooldown not zeroed when fire blocked or underflow | T-RTS-COMBAT reload and underflow |
| M3-M12 | `incoming` not zeroed each tick | T-RTS-COMBAT zeroing test; two-tick damage fixture |
| M3-M13 | damage kill emitted twice or on anchor | T-RTS-DAMAGE exact-kill, anchor-never-killed |
| M3-M14 | clamp removed from goal | T-RTS-GOAL wrapped-negative case |
| M3-M15 | move overshoots goal (no min) | T-RTS-MOVE no-overshoot case |
| M3-M16 | move ignores mode 2 engagement stop | T-RTS-MOVE mode-2 stop case |
| M3-M17 | generator PRNG constant or uniform(n) modulo variant | T-RTS-SCENE-1 digest |
| M3-M18 | replay skips a digest record or ignores `seq` order | T-RTS-REPLAY-1 and -2 |
| M3-M19 | `set_fields` without revision check | control-plane test with stale revision |
| M3-M20 | projectile state not cleared when shooter despawned (damage applied from dead shooter) | T-RTS-SCATTER shooter-despawned case |

Each mutant is applied to a copy of the tree, and the named test must fail. An unrun mutant is listed as unrun, not as passing.

## 16. Risks

1. **Per-tick caps at scale.** Mass deaths can exceed `max_struct_per_tick` (default 4096) and abort the whole tick with `SU_E_RESOURCE_LIMIT`. The cap is a configured value that the stress run records; it must not be raised silently.
2. **Host operators carry gameplay logic.** The spatial query, gather and scatter are profile code. Their effects are declared and tested, not verified (OD-5). A later verifier extension is the fix; it is not claimed here.
3. **Approximate nearest target under congestion.** Truncation makes targeting depend on budget and entry order. This is an accepted profile semantic (OD-6), but it must be reported with truncation rates and must not be described as exact nearest-neighbour behaviour.
4. **Overlap without separation.** Congested scenes will stack units. That is a synthetic choice (OD-7), not a physical model.
5. **IR verbosity.** Flag encoding makes IR JSON long. The authoring generator (`bench/rts/gen_ir.py`) is committed and regeneration equality is tested, so hand edits cannot drift from the source. Its output is authoritative only together with the committed JSON digest.
6. **Dead-row waste.** Native kernels process all rows and rely on dead-row zeroing (M2). In high-death scenes this costs time and must be measured in M3-10, not assumed.
7. **Digest cost.** Canonical SHA-256 of 39 columns per tick is O(N). `digest_every` is recorded; per-tick digests are used for small replay tests only.
8. **Oracle reach.** The Python model cannot run 10^5-10^6 row scenes in useful time. Large-scene correctness rests on the C reference and small-scene digest equality. Records must say so.
9. **Dependence on M2.** M3 uses M2-00 and M2-01 behaviour (entities, buffers, snapshot 0.2/0.3, recovery log). M2 is audited but not owner-accepted. Any M2 change reopens M3 section 2 and 13.
10. **Projectile model limits.** One projectile per shooter with fixed flight time is not a general weapon system (OD-8).
11. **Native lane timing.** Native execution of goal, combat and move changes performance only; the reference VM stays the oracle (M3-10).
12. **Authoring tool trust.** The generator is Python; it is a development helper and not part of the native build. Its outputs are committed and digested so the no-build guarantee (C01) is not affected by it.

## 17. Owner decisions requested

1. Accept D1 (no new IR opcodes for M3) and the re-evaluation trigger in 6.4 (OD-2).
2. Accept D2 (host operators `rts_ops_v1`) and the declared-and-tested effect model (OD-1, OD-5).
3. Approve `world.set_fields` and `world.spawn_batch` as new control-plane methods, each with recovery-log entries (OD-3, OD-3b).
4. Approve the profile constants: 256-unit cells, 128 x 128 grid, K = 64, 25-cell cap, `rts_u32_v1` domain (OD-6, OD-9).
5. Confirm scope exclusions: no separation, pathfinding, obstacles, multiple projectiles per shooter (OD-7, OD-8).
6. Decide whether exact memory accounting (OD-10) lands before M3-08 or whether analytic figures are reported as estimates until then.
7. Decide whether M3-10 native work is in scope for M3 closure (OD-11) or deferred.
8. Confirm whether the M3 Python model may be written by a different agent in the same orchestration (OD-13).
9. Decide scene load path for N above 65,536 (OD-12).
