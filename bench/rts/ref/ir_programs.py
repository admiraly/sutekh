# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""IR 0.1 / 0.2 programs for rts.goal, rts.combat, rts.move, rts.damage (contract M3 section 5).

Each program is typed by hand from the section 5 pseudocode with the M0 instruction names
(const_u32, load_u32, add/sub/mul_u32, eq/lt_u32, select_u32, store_u32; despawn_self for 0.2).
These are not the committed M3-03 capsule records (M3-03 owns those and is not started). The
pseudocode's flag and bool conventions of section 6.1 are followed: a flag is select(b, 1, 0),
a bool is eq(flag, 1). Negative goal values wrap in u32 and are caught by lt(2147483647, v).
"""
from __future__ import annotations

from model import BIAS, DOMAIN_MAX, NONE


class _Builder:
    def __init__(self, system_id: str, ir_version: str, reads, writes, max_struct_per_row: int = 0):
        self.system_id = system_id
        self.ir_version = ir_version
        self.reads = tuple(reads)
        self.writes = tuple(writes)
        self.max_struct_per_row = max_struct_per_row
        self.instructions = []
        self._n = 0

    def _reg(self, hint: str) -> str:
        self._n += 1
        return f"{hint}{self._n}"

    def const(self, value: int, hint: str = "c") -> str:
        dst = self._reg(hint)
        self.instructions.append({"op": "const_u32", "dst": dst, "value": value})
        return dst

    def load(self, field: str) -> str:
        dst = self._reg("v")
        self.instructions.append({"op": "load_u32", "dst": dst, "field": field})
        return dst

    def arith(self, op: str, a: str, b: str) -> str:
        dst = self._reg("t")
        self.instructions.append({"op": op, "dst": dst, "a": a, "b": b})
        return dst

    def add(self, a, b):
        return self.arith("add_u32", a, b)

    def sub(self, a, b):
        return self.arith("sub_u32", a, b)

    def eq(self, a, b) -> str:
        return self.arith("eq_u32", a, b)

    def lt(self, a, b) -> str:
        return self.arith("lt_u32", a, b)

    def sel(self, cond: str, on_true: str, on_false: str) -> str:
        dst = self._reg("s")
        self.instructions.append({"op": "select_u32", "dst": dst, "cond": cond,
                                  "on_true": on_true, "on_false": on_false})
        return dst

    def store(self, field: str, src: str) -> None:
        self.instructions.append({"op": "store_u32", "field": field, "src": src})

    def despawn(self, cond: str) -> None:
        self.instructions.append({"op": "despawn_self", "cond": cond})

    def build(self) -> dict:
        fields = {}
        for name in self.reads:
            fields.setdefault(name, {"read": False, "write": False})["read"] = True
        for name in self.writes:
            fields.setdefault(name, {"read": False, "write": False})["write"] = True
        return {
            "ir_version": self.ir_version,
            "system_id": self.system_id,
            "fields": fields,
            "limits": {"max_struct_per_row": self.max_struct_per_row, "max_events_per_row": 0},
            "instructions": self.instructions,
        }


def goal_program() -> dict:
    """5.1 rts.goal (IR 0.1)."""
    b = _Builder("rts.goal", "0.1",
                 reads=["order_mode", "order_x", "order_y", "anchor_valid", "anchor_x", "anchor_y",
                        "formation_ox", "formation_oy", "pos_x", "pos_y"],
                 writes=["goal_x", "goal_y"])
    zero, one = b.const(0, "zero"), b.const(1, "one")
    bias = b.const(BIAS, "bias")
    big = b.const(2147483647, "big")
    top = b.const(DOMAIN_MAX, "top")
    has_order_flag = b.sel(b.eq(b.load("order_mode"), zero), zero, one)
    has_order = b.eq(has_order_flag, one)
    av = b.eq(b.load("anchor_valid"), one)

    def axis(anchor: str, offset: str, order: str, pos: str) -> str:
        anchor_goal = b.sub(b.add(b.load(anchor), b.load(offset)), bias)
        inner = b.sel(av, anchor_goal, b.load(pos))
        raw = b.sel(has_order, b.load(order), inner)
        # clamp_domain(v) = select(lt(2147483647, v), 0, select(lt(32767, v), 32767, v))
        return b.sel(b.lt(big, raw), zero, b.sel(b.lt(top, raw), top, raw))

    b.store("goal_x", axis("anchor_x", "formation_ox", "order_x", "pos_x"))
    b.store("goal_y", axis("anchor_y", "formation_oy", "order_y", "pos_y"))
    return b.build()


def combat_program() -> dict:
    """5.2 rts.combat (IR 0.1)."""
    b = _Builder("rts.combat", "0.1",
                 reads=["target_valid", "target_slot", "target_gen", "cooldown", "reload", "damage",
                        "flight", "proj_active", "proj_left", "proj_target", "proj_target_gen",
                        "proj_damage"],
                 writes=["cooldown", "proj_active", "proj_left", "proj_target", "proj_target_gen",
                         "proj_damage", "dmg_out", "dmg_target", "dmg_target_gen", "incoming"])
    zero, one = b.const(0, "zero"), b.const(1, "one")
    none = b.const(NONE, "none")
    tv = b.eq(b.load("target_valid"), one)
    cd0 = b.eq(b.load("cooldown"), zero)
    pa0 = b.eq(b.load("proj_active"), zero)
    fire_flag = b.sel(tv, b.sel(cd0, b.sel(pa0, one, zero), zero), zero)
    fire = b.eq(fire_flag, one)
    arrive_flag = b.sel(b.eq(b.load("proj_active"), one), b.sel(b.eq(b.load("proj_left"), one), one, zero), zero)
    arrive = b.eq(arrive_flag, one)

    cooldown_new = b.sel(fire, b.sub(b.load("reload"), one),
                         b.sel(b.lt(zero, b.load("cooldown")), b.sub(b.load("cooldown"), one), zero))
    proj_active_new = b.sel(fire, one, b.sel(arrive, zero, b.load("proj_active")))
    proj_left_dec = b.sel(b.lt(zero, b.load("proj_left")), b.sub(b.load("proj_left"), one), zero)
    proj_left_new = b.sel(fire, b.load("flight"), b.sel(arrive, zero, proj_left_dec))
    proj_target_new = b.sel(fire, b.load("target_slot"), b.sel(arrive, none, b.load("proj_target")))
    proj_target_gen_new = b.sel(fire, b.load("target_gen"), b.sel(arrive, zero, b.load("proj_target_gen")))
    proj_damage_new = b.sel(fire, b.load("damage"), b.sel(arrive, zero, b.load("proj_damage")))
    dmg_out_new = b.sel(arrive, b.load("proj_damage"), zero)
    dmg_target_new = b.sel(arrive, b.load("proj_target"), none)
    dmg_target_gen_new = b.sel(arrive, b.load("proj_target_gen"), zero)

    b.store("cooldown", cooldown_new)
    b.store("proj_active", proj_active_new)
    b.store("proj_left", proj_left_new)
    b.store("proj_target", proj_target_new)
    b.store("proj_target_gen", proj_target_gen_new)
    b.store("proj_damage", proj_damage_new)
    b.store("dmg_out", dmg_out_new)
    b.store("dmg_target", dmg_target_new)
    b.store("dmg_target_gen", dmg_target_gen_new)
    b.store("incoming", zero)  # incoming' = 0
    return b.build()


def move_program() -> dict:
    """5.3 rts.move (IR 0.1)."""
    b = _Builder("rts.move", "0.1",
                 reads=["goal_x", "goal_y", "pos_x", "pos_y", "speed", "order_mode", "target_valid"],
                 writes=["pos_x", "pos_y"])
    zero, one = b.const(0, "zero"), b.const(1, "one")
    mode_move = b.eq(b.load("order_mode"), one)
    has_tgt = b.eq(b.load("target_valid"), one)
    mv_flag = b.sel(mode_move, one, b.sel(has_tgt, zero, one))
    moving = b.eq(mv_flag, one)

    def axis(pos: str, goal: str) -> str:
        p = b.load(pos)
        g = b.load(goal)
        s = b.load("speed")
        right = b.lt(p, g)
        left = b.lt(g, p)
        ax = b.sel(right, b.sub(g, p), b.sub(p, g))
        m = b.sel(b.lt(ax, s), ax, s)
        step = b.sel(right, b.add(p, m), b.sel(left, b.sub(p, m), p))
        return b.sel(moving, step, p)

    b.store("pos_x", axis("pos_x", "goal_x"))
    b.store("pos_y", axis("pos_y", "goal_y"))
    return b.build()


def damage_program() -> dict:
    """5.4 rts.damage (IR 0.2, despawn_self; max_struct_per_row = 1)."""
    b = _Builder("rts.damage", "0.2", reads=["hp", "incoming"], writes=["hp"], max_struct_per_row=1)
    zero, one = b.const(0, "zero"), b.const(1, "one")
    hp = b.load("hp")
    incoming = b.load("incoming")
    m = b.sel(b.lt(incoming, hp), incoming, hp)
    hp2 = b.sub(hp, m)
    kill_flag = b.sel(b.eq(hp2, zero), b.sel(b.lt(zero, b.load("hp")), one, zero), zero)
    b.store("hp", hp2)
    b.despawn(b.eq(kill_flag, one))
    return b.build()


PROGRAM_BUILDERS = {
    "goal": goal_program,
    "combat": combat_program,
    "move": move_program,
    "damage": damage_program,
}


def all_programs() -> dict:
    return {name: build() for name, build in PROGRAM_BUILDERS.items()}
