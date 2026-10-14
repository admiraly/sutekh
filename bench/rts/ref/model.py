# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Semantic RTS reference model, M3-02 (contract planning/contracts/M3_RTS_CONTRACT.md r4).

Written from sections 2, 4, 5, 8 and 9 of the contract, not from the IR JSON and not from any
engine source (M3-04 operator sources are not read). Standard library only.

Scope of this module:
  - the 39-field column layout (section 2) and the control-plane spawn/despawn rules of 2.2 that
    the fixtures need (set_fields, spawn_batch and replay commands are not implemented here);
  - the per-tick phases 0-5 and the boundary merge (sections 3, 4, 5, 9.4);
  - the per-row functions of the four IR systems (section 5), which the IR evaluator is compared
    against in tests/rts/ref/test_ir.py.

Conventions:
  - Values are Python ints. "u32" means the contract's domains and the wrap rules of section 2.1.
    Positions and orders are domain-checked (0..32767) at spawn; the per-row functions are only
    defined for domain-valid rows, as the contract requires (control plane refuses others).
  - Host phases (spatial index, scatter) are plain Python here. They are the reference for the
    host operators of M3-04, not an engine implementation.
"""
from __future__ import annotations

from collections import deque

from isqrt import isqrt

MASK = 0xFFFFFFFF
NONE = 0xFFFFFFFF
DOMAIN_MAX = 32767
BIAS = 16384
K_BUDGET = 128
CELL_SHIFT = 6
GRID = 512
CELLS = GRID * GRID
RC_MAX = 8
KIND_UNIT = 1
KIND_ANCHOR = 2
ORDER_NONE = 0
ORDER_MOVE = 1
ORDER_ATTACK_MOVE = 2
RANGE_SQ_MAX = 262144
CAP_MAX = 1000000  # SU_LIMIT_ROWS default (OD-21 keeps it)
EVENT_LOG_MAX_DEFAULT = 65536
DESPAWN_KEY = "_despawn"  # pseudo-output of rts.damage: 1 when despawn_self fires

# Section 2 field table: id and name, in id order.
FIELDS = (
    (1, "kind"), (2, "team"), (3, "hp"), (4, "pos_x"), (5, "pos_y"), (6, "speed"), (7, "range_sq"),
    (8, "reload"), (9, "cooldown"), (10, "damage"), (11, "flight"), (12, "order_mode"),
    (13, "order_x"), (14, "order_y"), (15, "formation_slot"), (16, "formation_gen"),
    (17, "formation_ox"), (18, "formation_oy"), (19, "goal_x"), (20, "goal_y"), (21, "anchor_x"),
    (22, "anchor_y"), (23, "anchor_valid"), (24, "target_slot"), (25, "target_gen"),
    (26, "target_dist_sq"), (27, "target_valid"), (28, "target_x"), (29, "target_y"),
    (30, "truncated"), (31, "proj_active"), (32, "proj_left"), (33, "proj_target"),
    (34, "proj_target_gen"), (35, "proj_damage"), (36, "dmg_out"), (37, "dmg_target"),
    (38, "dmg_target_gen"), (39, "incoming"),
)
FIELD_NAMES = tuple(name for _, name in FIELDS)
assert len(FIELD_NAMES) == 39 and len(set(FIELD_NAMES)) == 39

# Spawn classes (2.2 and D1). The three groups partition the 39 fields.
SPAWN_NAMED = (
    "kind", "team", "hp", "pos_x", "pos_y", "speed", "range_sq", "reload", "damage", "flight",
    "order_mode", "order_x", "order_y", "formation_slot", "formation_gen", "formation_ox",
    "formation_oy",
)
SPAWN_IDLE_ZERO = (
    "cooldown", "goal_x", "goal_y", "anchor_x", "anchor_y", "anchor_valid", "target_gen",
    "target_dist_sq", "target_valid", "target_x", "target_y", "truncated", "proj_active",
    "proj_left", "proj_target_gen", "proj_damage", "dmg_out", "dmg_target_gen", "incoming",
)
SPAWN_IDLE_NONE = ("target_slot", "dmg_target", "proj_target")
assert len(SPAWN_NAMED) + len(SPAWN_IDLE_ZERO) + len(SPAWN_IDLE_NONE) == 39

# Declared writes per system (section 2 "written by", sections 4.3 and 5.x). Used as a guard.
GOAL_WRITES = ("goal_x", "goal_y")
COMBAT_WRITES = ("cooldown", "proj_active", "proj_left", "proj_target", "proj_target_gen",
                 "proj_damage", "dmg_out", "dmg_target", "dmg_target_gen", "incoming")
MOVE_WRITES = ("pos_x", "pos_y")
DAMAGE_WRITES = ("hp",)
SPATIAL_WRITES = ("anchor_x", "anchor_y", "anchor_valid", "target_slot", "target_gen",
                  "target_dist_sq", "target_valid", "target_x", "target_y", "truncated")
SCATTER_WRITES = ("incoming",)


class ModelError(Exception):
    """A refusal or an out-of-contract input. code is the contract error name, path a field name."""

    def __init__(self, code: str, path: str = "", detail: str = ""):
        super().__init__(f"{code} {path} {detail}".strip())
        self.code = code
        self.path = path
        self.detail = detail


# ---- u32 helpers (section 2.1, 4.3) -----------------------------------------------------------

def sat_add(a: int, b: int) -> int:
    """sat_add(a, b) = (a + b < a) ? 0xFFFFFFFF : a + b, in u32 (4.3)."""
    s = (a + b) & MASK
    return MASK if s < a else s


def clamp_domain(v: int) -> int:
    """Clamp a true integer to the position domain [0, 32767] (5.1 clamp, in exact arithmetic)."""
    if v < 0:
        return 0
    if v > DOMAIN_MAX:
        return DOMAIN_MAX
    return v


def cell_of(x: int, y: int) -> int:
    """c(x, y) = (y >> 6) * 512 + (x >> 6), 4.1(b)."""
    return (y >> CELL_SHIFT) * GRID + (x >> CELL_SHIFT)


def ring_cells(cx: int, cy: int, d: int):
    """Cells at Chebyshev distance exactly d from (cx, cy), clipped to the grid, row-major (4.1(c)2)."""
    for cyp in range(max(0, cy - d), min(GRID - 1, cy + d) + 1):
        if abs(cyp - cy) == d:
            xs = range(max(0, cx - d), min(GRID - 1, cx + d) + 1)
        else:
            xs = [c for c in (cx - d, cx + d) if 0 <= c < GRID]
        for cxp in xs:
            yield cyp * GRID + cxp


# ---- world state ------------------------------------------------------------------------------

class World:
    """Entity table, columns, event log. Slots 0..cap-1; generation starts at 1 and despawn bumps it."""

    def __init__(self, capacity: int, event_log_max: int = EVENT_LOG_MAX_DEFAULT):
        if type(capacity) is not int or not (1 <= capacity <= CAP_MAX):
            raise ModelError("SU_E_RESOURCE_LIMIT", "capacity")
        if type(event_log_max) is not int or event_log_max < 1:
            raise ModelError("SU_E_TYPE", "event_log_max")
        self.cap = capacity
        self.alive = [0] * capacity
        self.gen = [1] * capacity
        self.cols = {name: [0] * capacity for name in FIELD_NAMES}
        self.hw = 0
        self.retired = 0
        self.tick_no = 0
        self.event_log_max = event_log_max
        self.events = deque()  # retained (seq, kind, slot, gen)
        self.event_next_seq = 0
        self.dropped = 0
        self.last_truncated_rows = 0

    # -- events --
    def _log(self, kind: str, slot: int, gen: int) -> None:
        if len(self.events) == self.event_log_max:
            self.events.popleft()
            self.dropped += 1
        self.events.append((self.event_next_seq, kind, slot, gen))
        self.event_next_seq += 1

    # -- control plane (idle phase only; section 2.2, 9.4) --
    def handle(self, slot: int) -> int:
        """SuEntity handle for the live row at slot: generation << 32 | slot."""
        return (self.gen[slot] << 32) | slot

    def check_formation_live(self, formation_slot: int) -> None:
        """Q2 (owner decision, M3-05): a formation_slot other than NONE must name a live row of the committed
        state, so a member never names a dead slot at its current generation (M3-M24)."""
        if formation_slot != NONE and not self.alive[formation_slot]:
            raise ModelError("SU_E_TYPE", "formation_slot", "must name a live row")

    def spawn(self, spec: dict) -> int:
        """Spawn one row from named fields; returns the handle. Omitted fields take the 2.2 defaults."""
        vals = validate_spawn(spec, self.cap)
        self.check_formation_live(vals["formation_slot"])
        slot = next((s for s in range(self.cap) if not self.alive[s] and self.gen[s] != MASK), None)
        if slot is None:
            raise ModelError("SU_E_RESOURCE_LIMIT", "world", "no free slot")
        for name in FIELD_NAMES:
            self.cols[name][slot] = vals[name]
        self.alive[slot] = 1
        self.hw = max(self.hw, slot + 1)
        self._log("SU_EVENT_SPAWNED", slot, self.gen[slot])
        return self.handle(slot)

    def despawn(self, handle: int) -> None:
        """Control-plane despawn of a live handle (stale or dead handles refused)."""
        slot, gen = handle & MASK, handle >> 32
        if slot >= self.cap or not self.alive[slot] or self.gen[slot] != gen:
            raise ModelError("SU_E_STALE_HANDLE", "entity")
        self._despawn_slot(slot)

    def _despawn_slot(self, slot: int) -> None:
        # Boundary merge: mark dead, bump generation, zero the row (M2-00 section 1, 9.2).
        self._log("SU_EVENT_DESPAWNED", slot, self.gen[slot])
        self.alive[slot] = 0
        self.gen[slot] = (self.gen[slot] + 1) & MASK
        if self.gen[slot] == MASK:
            self.retired += 1
        for name in FIELD_NAMES:
            self.cols[name][slot] = 0

    def row(self, slot: int) -> dict:
        return {name: self.cols[name][slot] for name in FIELD_NAMES}

    def live_slots(self) -> list:
        return [s for s in range(self.cap) if self.alive[s]]


def _is_int(v) -> bool:
    return type(v) is int


def _in(v, lo: int, hi: int) -> bool:
    return _is_int(v) and lo <= v <= hi


def validate_spawn(spec: dict, cap: int) -> dict:
    """Apply the 2.2 spawn domains and the D1 defaults. Returns the full 39-field row or raises."""
    for name in spec:
        if name not in FIELD_NAMES:
            raise ModelError("SU_E_SCHEMA_MISMATCH", name)
    for name in SPAWN_IDLE_ZERO:
        if name in spec and spec[name] != 0:
            raise ModelError("SU_E_TYPE", name, "system-owned, idle value is 0 at spawn")
    for name in SPAWN_IDLE_NONE:
        if name in spec and spec[name] != NONE:
            raise ModelError("SU_E_TYPE", name, "must be NONE at spawn")
    for name in ("kind", "reload", "damage", "flight"):
        if name not in spec:
            raise ModelError("SU_E_TYPE", name, "required at spawn (D1)")
    kind = spec["kind"]
    if kind not in (KIND_UNIT, KIND_ANCHOR) or not _is_int(kind):
        raise ModelError("SU_E_TYPE", "kind")
    team = spec.get("team", 0)
    if not _in(team, 0, 1):
        raise ModelError("SU_E_TYPE", "team")
    hp = spec.get("hp", 0)
    if kind == KIND_UNIT and not _in(hp, 1, 65535):
        raise ModelError("SU_E_TYPE", "hp", "kind 1 needs 1..65535")
    if kind == KIND_ANCHOR and hp != 0:
        raise ModelError("SU_E_TYPE", "hp", "kind 2 needs 0")
    for name in ("pos_x", "pos_y", "order_x", "order_y"):
        if not _in(spec.get(name, 0), 0, DOMAIN_MAX):
            raise ModelError("SU_E_TYPE", name)
    if not _in(spec.get("speed", 0), 0, 1024):
        raise ModelError("SU_E_TYPE", "speed")
    range_sq = spec.get("range_sq", 0)
    if not _in(range_sq, 0, RANGE_SQ_MAX) or (kind == KIND_ANCHOR and range_sq != 0):
        raise ModelError("SU_E_TYPE", "range_sq")
    if not _in(spec["reload"], 1, 1024):
        raise ModelError("SU_E_TYPE", "reload")
    if not _in(spec["damage"], 1, 65535):
        raise ModelError("SU_E_TYPE", "damage")
    if not _in(spec["flight"], 1, 64):
        raise ModelError("SU_E_TYPE", "flight")
    if spec.get("order_mode", 0) not in (ORDER_NONE, ORDER_MOVE, ORDER_ATTACK_MOVE) or not _is_int(spec.get("order_mode", 0)):
        raise ModelError("SU_E_TYPE", "order_mode")
    fslot = spec.get("formation_slot", NONE)
    if not (fslot == NONE or _in(fslot, 0, cap - 1)):
        raise ModelError("SU_E_TYPE", "formation_slot")
    if fslot == NONE:
        if spec.get("formation_gen", 0) != 0:
            raise ModelError("SU_E_TYPE", "formation_gen", "must be 0 when formation_slot is NONE")
        fgen = 0
    else:
        if "formation_gen" not in spec:
            raise ModelError("SU_E_TYPE", "formation_gen", "required when formation_slot is set")
        fgen = spec["formation_gen"]
        if not _in(fgen, 1, MASK):
            raise ModelError("SU_E_TYPE", "formation_gen", "must be a nonzero u32")
        for name in ("formation_ox", "formation_oy"):
            if name not in spec:
                raise ModelError("SU_E_TYPE", name, "required when formation_slot is set (D1)")
            if not _in(spec[name], 0, DOMAIN_MAX):
                raise ModelError("SU_E_TYPE", name)
    vals = {name: 0 for name in FIELD_NAMES}
    for name in SPAWN_IDLE_NONE:
        vals[name] = NONE
    vals["formation_slot"] = fslot
    vals["formation_gen"] = fgen
    for name in SPAWN_NAMED:
        if name in spec:
            vals[name] = spec[name]
    vals["team"] = team
    vals["hp"] = hp
    vals["range_sq"] = range_sq
    vals["order_mode"] = spec.get("order_mode", 0)
    return vals


# ---- per-row system functions (section 5), written from the prose and pseudocode -----------------
# Each takes a dict row (all 39 fields at tick start, or the state the phase sees) and returns the
# new values of its declared writes. Inputs are domain-valid (control plane guarantees it).

def goal_row(row: dict) -> dict:
    """5.1: explicit order wins; formation member follows anchor plus offset; else hold."""
    if row["order_mode"] != ORDER_NONE:
        return {"goal_x": clamp_domain(row["order_x"]), "goal_y": clamp_domain(row["order_y"])}
    if row["anchor_valid"] == 1:
        gx = clamp_domain(row["anchor_x"] + row["formation_ox"] - BIAS)
        gy = clamp_domain(row["anchor_y"] + row["formation_oy"] - BIAS)
        return {"goal_x": gx, "goal_y": gy}
    return {"goal_x": row["pos_x"], "goal_y": row["pos_y"]}


def combat_row(row: dict) -> dict:
    """5.2: fire when a target is valid, cooldown is 0 and no projectile is in flight; arrival at
    the start-of-tick proj_left == 1; reload - 1 and flight are stored (N1)."""
    tv = row["target_valid"] == 1
    cd0 = row["cooldown"] == 0
    pa0 = row["proj_active"] == 0
    fire = tv and cd0 and pa0
    arrive = row["proj_active"] == 1 and row["proj_left"] == 1
    if fire:
        cooldown = row["reload"] - 1
    elif row["cooldown"] > 0:
        cooldown = row["cooldown"] - 1
    else:
        cooldown = 0
    if fire:
        proj_active, proj_left = 1, row["flight"]
        proj_target, proj_target_gen = row["target_slot"], row["target_gen"]
        proj_damage = row["damage"]
    elif arrive:
        proj_active, proj_left = 0, 0
        proj_target, proj_target_gen = NONE, 0
        proj_damage = 0
    else:
        proj_active = row["proj_active"]
        proj_left = row["proj_left"] - 1 if row["proj_left"] > 0 else 0
        proj_target, proj_target_gen = row["proj_target"], row["proj_target_gen"]
        proj_damage = row["proj_damage"]
    return {
        "cooldown": cooldown,
        "proj_active": proj_active,
        "proj_left": proj_left,
        "proj_target": proj_target,
        "proj_target_gen": proj_target_gen,
        "proj_damage": proj_damage,
        "dmg_out": row["proj_damage"] if arrive else 0,
        "dmg_target": row["proj_target"] if arrive else NONE,
        "dmg_target_gen": row["proj_target_gen"] if arrive else 0,
        "incoming": 0,
    }


def move_row(row: dict) -> dict:
    """5.3: mode 1 always moves; otherwise a valid target stops the unit; step = min(distance, speed)."""
    moving = row["order_mode"] == ORDER_MOVE or row["target_valid"] != 1

    def step(pos: int, goal: int) -> int:
        if goal > pos:
            return pos + min(goal - pos, row["speed"])
        if goal < pos:
            return pos - min(pos - goal, row["speed"])
        return pos

    if not moving:
        return {"pos_x": row["pos_x"], "pos_y": row["pos_y"]}
    return {"pos_x": step(row["pos_x"], row["goal_x"]), "pos_y": step(row["pos_y"], row["goal_y"])}


def damage_row(row: dict) -> dict:
    """5.4: subtract min(incoming, hp); despawn_self when hp goes from above 0 to exactly 0."""
    m = min(row["incoming"], row["hp"])
    hp2 = row["hp"] - m
    kill = hp2 == 0 and row["hp"] > 0
    return {"hp": hp2, DESPAWN_KEY: 1 if kill else 0}


MODEL_SYSTEMS = {"goal": goal_row, "combat": combat_row, "move": move_row, "damage": damage_row}
SYSTEM_WRITES = {"goal": GOAL_WRITES, "combat": COMBAT_WRITES, "move": MOVE_WRITES, "damage": DAMAGE_WRITES}


# ---- host phases (section 4) -------------------------------------------------------------------

def _gather_valid(world: World, fslot: int, fgen: int) -> bool:
    """gather_valid(s, g) = s != NONE && s < cap && alive(s) && generation(s) == g (4.1(a))."""
    return fslot != NONE and fslot < world.cap and world.alive[fslot] == 1 and world.gen[fslot] == fgen


def spatial_phase(world: World) -> dict:
    """Phase 0 (4.1 a-d). Reads tick-start state only; writes the spatial fields of section 2."""
    cap, alive, gen, cols = world.cap, world.alive, world.gen, world.cols
    pos_x, pos_y = cols["pos_x"], cols["pos_y"]
    kind, team, hp = cols["kind"], cols["team"], cols["hp"]
    range_sq_col = cols["range_sq"]

    # (a) anchor gather
    for s in range(cap):
        if not alive[s]:
            continue
        fs = cols["formation_slot"][s]
        if _gather_valid(world, fs, cols["formation_gen"][s]):
            cols["anchor_valid"][s] = 1
            cols["anchor_x"][s] = pos_x[fs]
            cols["anchor_y"][s] = pos_y[fs]
        else:
            cols["anchor_valid"][s] = 0
            cols["anchor_x"][s] = 0
            cols["anchor_y"][s] = 0

    # (b) grid: alive units with hp > 0, ascending slot order within a cell (counting sort result)
    grid = {}
    for s in range(cap):
        if alive[s] and kind[s] == KIND_UNIT and hp[s] > 0:
            grid.setdefault(cell_of(pos_x[s], pos_y[s]), []).append(s)

    # (c) target query
    truncated_rows = 0
    for r in range(cap):
        if not alive[r]:
            continue  # dead committed rows remain canonically zero
        if not (kind[r] == KIND_UNIT and range_sq_col[r] > 0):
            cols["target_slot"][r] = NONE
            cols["target_gen"][r] = 0
            cols["target_dist_sq"][r] = 0
            cols["target_valid"][r] = 0
            cols["truncated"][r] = 0
            continue
        best, trunc = _target_query(world, grid, r)
        if best is None:
            cols["target_slot"][r] = NONE
            cols["target_gen"][r] = 0
            cols["target_dist_sq"][r] = 0
            cols["target_valid"][r] = 0
        else:
            cols["target_slot"][r] = best[1]
            cols["target_gen"][r] = gen[best[1]]
            cols["target_dist_sq"][r] = best[0]
            cols["target_valid"][r] = 1
        cols["truncated"][r] = trunc
        truncated_rows += trunc

    # (d) target gather
    for r in range(cap):
        if alive[r] and cols["target_valid"][r] == 1:
            t = cols["target_slot"][r]
            cols["target_x"][r] = pos_x[t]
            cols["target_y"][r] = pos_y[t]
        else:
            cols["target_x"][r] = 0
            cols["target_y"][r] = 0

    world.last_truncated_rows = truncated_rows
    return {"truncated_rows": truncated_rows, "grid_cells": len(grid),
            "grid_items": sum(len(v) for v in grid.values())}


def _target_query(world: World, grid: dict, r: int):
    """4.1(c): ring-ordered scan with the K = 128 examined-entry budget. Returns (best, truncated),
    best = (dist_sq, slot) or None."""
    cols = world.cols
    x, y = cols["pos_x"][r], cols["pos_y"][r]
    range_sq = cols["range_sq"][r]
    my_team = cols["team"][r]
    R = isqrt(range_sq)
    rc = (R + 63) >> 6
    if rc > RC_MAX:
        raise ModelError("SU_E_INTERNAL", "range_sq", "rc above 8")
    cx, cy = x >> CELL_SHIFT, y >> CELL_SHIFT
    pos_x, pos_y, team = cols["pos_x"], cols["pos_y"], cols["team"]
    box_total = 0
    examined = 0
    stop = False
    best = None
    for d in range(rc + 1):
        for cell in ring_cells(cx, cy, d):
            entries = grid.get(cell, ())
            box_total += len(entries)
            if stop:
                continue
            for e in entries:
                if examined == K_BUDGET:
                    stop = True
                    break
                examined += 1
                if e == r or team[e] == my_team:
                    continue
                dx = abs(pos_x[e] - x)
                dy = abs(pos_y[e] - y)
                dist_sq = dx * dx + dy * dy
                if dist_sq <= range_sq:
                    cand = (dist_sq, e)
                    if best is None or cand < best:
                        best = cand
    truncated = 1 if box_total > K_BUDGET else 0
    return best, truncated


def scatter_phase(world: World) -> dict:
    """Phase 4 (4.3): ascending alive senders with dmg_target != NONE and dmg_out != 0 add into targets."""
    cap, alive, gen, cols = world.cap, world.alive, world.gen, world.cols
    applied = 0
    for r in range(cap):
        if not alive[r]:
            continue
        t = cols["dmg_target"][r]
        amount = cols["dmg_out"][r]
        if t == NONE or amount == 0:
            continue
        if t < cap and alive[t] == 1 and gen[t] == cols["dmg_target_gen"][r] and cols["kind"][t] == KIND_UNIT:
            cols["incoming"][t] = sat_add(cols["incoming"][t], amount)
            applied += 1
    return {"applied": applied}


# ---- tick (section 3, 9.4) ----------------------------------------------------------------------

def _run_row_system(world: World, name: str, fn) -> list:
    """Run a per-row system over alive rows. Each row sees its own tick-start values (rows do not
    read each other), and its outputs are committed after the row. Returns despawn slots ascending."""
    cols = world.cols
    deaths = []
    allowed = set(SYSTEM_WRITES[name]) | {DESPAWN_KEY}
    for s in range(world.cap):
        if not world.alive[s]:
            continue
        out = fn(world.row(s))
        if not set(out) <= allowed:
            raise ModelError("SU_E_UNDECLARED_WRITE", name, str(sorted(set(out) - allowed)))
        for key, value in out.items():
            if key == DESPAWN_KEY:
                if value:
                    deaths.append(s)
            else:
                cols[key][s] = value
    return deaths


def tick(world: World, systems: dict = None) -> dict:
    """One tick: spatial, goal, combat, move, scatter, damage, then the boundary merge (9.4).
    systems maps 'goal', 'combat', 'move', 'damage' to row functions (MODEL_SYSTEMS or the IR wrappers)."""
    systems = MODEL_SYSTEMS if systems is None else systems
    info = {}
    info["spatial"] = spatial_phase(world)
    _run_row_system(world, "goal", systems["goal"])
    _run_row_system(world, "combat", systems["combat"])
    _run_row_system(world, "move", systems["move"])
    info["scatter"] = scatter_phase(world)
    deaths = _run_row_system(world, "damage", systems["damage"])
    for s in deaths:  # canonical merge order: ascending slot
        world._despawn_slot(s)
    info["deaths"] = len(deaths)
    world.last_truncated_rows = sum(1 for s in range(world.hw)
                                    if world.alive[s] and world.cols["truncated"][s])
    world.tick_no += 1
    return info
