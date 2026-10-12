#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-03 IR authoring tool: writes the RTS IR capsule and LIR record pairs (contract r4, 3.2, 5, 5.5).

Systems: rts.goal, rts.combat, rts.move (IR 0.1) and rts.damage (IR 0.2, despawn_self). Each
program is typed from the section 5 pseudocode with the M0 instruction names. The instruction
list is the LIR JSON read by the engine verifier (src/ir/su_lir.c). The capsule `fields` table is
derived from the instructions (loads give read, stores give write); the field ids and names come
from profiles/rts/schema/rts_u32_v1.json.

This file does not import the reference programs in bench/rts/ref/ on purpose: the differential in
tests/rts/ir compares two independent typings of section 5 against the model and against the engine.

Recorded deviations (see planning/handoffs/2026-10-12-m3-03.md): numeric_profile is written as
"u32_mod_v1" (the only profile the engine and the capsule schema accept; contract OD-9 names
"rts_u32_v1"), and system_rev and capabilities are written as contract 5.5 requires although the
engine verifier does not accept those two keys yet.

Usage:
  python3 -I bench/rts/gen_ir.py               write the records into capsules/rts/
  python3 -I bench/rts/gen_ir.py --check       regenerate in memory; exit 1 if a committed record differs
  python3 -I bench/rts/gen_ir.py --out DIR     write the records into DIR
"""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
PROFILE_PATH = os.path.join(REPO, "profiles", "rts", "schema", "rts_u32_v1.json")
RECORD_DIR = os.path.join(REPO, "capsules", "rts")

BIAS = 16384
NONE = 0xFFFFFFFF
NUMERIC_PROFILE = "u32_mod_v1"
SCHEMA_ID = "rts"
SCHEMA_VERSION = 1
LAYOUT_GENERATION = 1
SYSTEM_REV = 1
PHASE = "rts"
MAX_INSTRUCTIONS = 4096


class Ir:
    """Straight-line LIR builder. Each helper returns the register it defines. Constants and field
    loads are defined once and reused (a load sees tick-start values, so reuse is exact)."""

    def __init__(self):
        self.ins = []
        self._n = 0
        self._consts = {}
        self._loads = {}

    def _new(self, hint):
        self._n += 1
        return f"{hint}{self._n}"

    def const(self, value):
        if value not in self._consts:
            r = self._new("k")
            self.ins.append({"op": "const_u32", "dst": r, "value": value})
            self._consts[value] = r
        return self._consts[value]

    def load(self, field):
        if field not in self._loads:
            r = self._new("v")
            self.ins.append({"op": "load_u32", "dst": r, "field": field})
            self._loads[field] = r
        return self._loads[field]

    def _bin(self, op, a, b):
        r = self._new("t")
        self.ins.append({"op": op, "dst": r, "a": a, "b": b})
        return r

    def add(self, a, b):
        return self._bin("add_u32", a, b)

    def sub(self, a, b):
        return self._bin("sub_u32", a, b)

    def eq(self, a, b):
        return self._bin("eq_u32", a, b)

    def lt(self, a, b):
        return self._bin("lt_u32", a, b)

    def select(self, cond, on_true, on_false):
        r = self._new("s")
        self.ins.append({"op": "select_u32", "dst": r, "cond": cond,
                         "on_true": on_true, "on_false": on_false})
        return r

    def store(self, field, src):
        self.ins.append({"op": "store_u32", "field": field, "src": src})

    def despawn_self(self, cond):
        self.ins.append({"op": "despawn_self", "cond": cond})


def program_goal():
    """5.1 rts.goal. Explicit order (mode 1 or 2) wins; a formation member follows its anchor with
    its offset; otherwise the goal is the current position. clamp_domain wraps negatives to 0."""
    p = Ir()
    zero, one = p.const(0), p.const(1)
    bias, big, top = p.const(BIAS), p.const(2147483647), p.const(32767)
    has_order_flag = p.select(p.eq(p.load("order_mode"), zero), zero, one)
    has_order = p.eq(has_order_flag, one)
    av = p.eq(p.load("anchor_valid"), one)

    def axis(anchor, offset, order, pos):
        anchor_goal = p.sub(p.add(p.load(anchor), p.load(offset)), bias)
        raw = p.select(has_order, p.load(order), p.select(av, anchor_goal, p.load(pos)))
        return p.select(p.lt(big, raw), zero, p.select(p.lt(top, raw), top, raw))

    p.store("goal_x", axis("anchor_x", "formation_ox", "order_x", "pos_x"))
    p.store("goal_y", axis("anchor_y", "formation_oy", "order_y", "pos_y"))
    return "0.1", p.ins


def program_combat():
    """5.2 rts.combat. Fire when a target is valid, cooldown is 0 and no projectile is in flight.
    Arrival when proj_left is 1 at tick start. incoming is stored as 0 (scatter adds to it later)."""
    p = Ir()
    zero, one = p.const(0), p.const(1)
    none = p.const(NONE)
    tv = p.eq(p.load("target_valid"), one)
    cd0 = p.eq(p.load("cooldown"), zero)
    pa0 = p.eq(p.load("proj_active"), zero)
    fire_flag = p.select(tv, p.select(cd0, p.select(pa0, one, zero), zero), zero)
    fire = p.eq(fire_flag, one)
    arrive_flag = p.select(p.eq(p.load("proj_active"), one),
                           p.select(p.eq(p.load("proj_left"), one), one, zero), zero)
    arrive = p.eq(arrive_flag, one)

    cooldown = p.select(fire, p.sub(p.load("reload"), one),
                        p.select(p.lt(zero, p.load("cooldown")), p.sub(p.load("cooldown"), one), zero))
    proj_active = p.select(fire, one, p.select(arrive, zero, p.load("proj_active")))
    dec_left = p.select(p.lt(zero, p.load("proj_left")), p.sub(p.load("proj_left"), one), zero)
    proj_left = p.select(fire, p.load("flight"), p.select(arrive, zero, dec_left))
    proj_target = p.select(fire, p.load("target_slot"), p.select(arrive, none, p.load("proj_target")))
    proj_target_gen = p.select(fire, p.load("target_gen"), p.select(arrive, zero, p.load("proj_target_gen")))
    proj_damage = p.select(fire, p.load("damage"), p.select(arrive, zero, p.load("proj_damage")))
    dmg_out = p.select(arrive, p.load("proj_damage"), zero)
    dmg_target = p.select(arrive, p.load("proj_target"), none)
    dmg_target_gen = p.select(arrive, p.load("proj_target_gen"), zero)

    p.store("cooldown", cooldown)
    p.store("proj_active", proj_active)
    p.store("proj_left", proj_left)
    p.store("proj_target", proj_target)
    p.store("proj_target_gen", proj_target_gen)
    p.store("proj_damage", proj_damage)
    p.store("dmg_out", dmg_out)
    p.store("dmg_target", dmg_target)
    p.store("dmg_target_gen", dmg_target_gen)
    p.store("incoming", zero)
    return "0.1", p.ins


def program_move():
    """5.3 rts.move. Mode 1 always moves; otherwise a valid target stops the unit. Each axis steps
    min(distance, speed) toward the goal, never overshooting."""
    p = Ir()
    zero, one = p.const(0), p.const(1)
    mode_move = p.eq(p.load("order_mode"), one)
    has_tgt = p.eq(p.load("target_valid"), one)
    mv_flag = p.select(mode_move, one, p.select(has_tgt, zero, one))
    moving = p.eq(mv_flag, one)
    speed = p.load("speed")

    def axis(pos, goal):
        cur, g = p.load(pos), p.load(goal)
        right = p.lt(cur, g)
        left = p.lt(g, cur)
        ax = p.select(right, p.sub(g, cur), p.sub(cur, g))
        m = p.select(p.lt(ax, speed), ax, speed)
        step = p.select(right, p.add(cur, m), p.select(left, p.sub(cur, m), cur))
        return p.select(moving, step, cur)

    p.store("pos_x", axis("pos_x", "goal_x"))
    p.store("pos_y", axis("pos_y", "goal_y"))
    return "0.1", p.ins


def program_damage():
    """5.4 rts.damage (IR 0.2). Subtract min(incoming, hp). despawn_self when hp goes from above 0
    to exactly 0 in this tick. Anchors (hp 0) never fire it."""
    p = Ir()
    zero, one = p.const(0), p.const(1)
    hp = p.load("hp")
    incoming = p.load("incoming")
    m = p.select(p.lt(incoming, hp), incoming, hp)
    hp_new = p.sub(hp, m)
    kill_flag = p.select(p.eq(hp_new, zero), p.select(p.lt(zero, hp), one, zero), zero)
    p.store("hp", hp_new)
    p.despawn_self(p.eq(kill_flag, one))
    return "0.2", p.ins


# name -> (program, schedule.after per contract 3.2, capsule tests, capabilities, max_struct_per_row)
SYSTEMS = {
    "goal": (program_goal, ["rts.spatial"], ["T-RTS-GOAL-*"], [], None),
    "combat": (program_combat, ["rts.spatial"], ["T-RTS-COMBAT-*"], [], None),
    "move": (program_move, ["rts.spatial", "rts.goal", "rts.combat"], ["T-RTS-MOVE-*"], [], None),
    "damage": (program_damage, ["rts.spatial", "rts.scatter"], ["T-RTS-DAMAGE-*"], ["despawn_self"], 1),
}


def load_profile_ids(path=PROFILE_PATH):
    with open(path, "r", encoding="utf-8") as fh:
        prof = json.load(fh)
    return {f["name"]: f["id"] for f in prof["fields"]}


def derive_fields(instructions, ids):
    """Field table from the instructions: a load is a read, a store is a write. Ascending id."""
    read, write = set(), set()
    for ins in instructions:
        if ins["op"] == "load_u32":
            read.add(ins["field"])
        elif ins["op"] == "store_u32":
            write.add(ins["field"])
    names = sorted(read | write, key=lambda n: ids[n])
    return [{"id": ids[n], "name": n, "type": "u32", "read": n in read, "write": n in write}
            for n in names]


def build_records(ids=None):
    """Returns {filename: text} for every record, deterministic bytes (JSON, 2-space indent, LF)."""
    ids = load_profile_ids() if ids is None else ids
    out = {}
    for name, (program, after, tests, caps, max_struct) in SYSTEMS.items():
        ir_version, instructions = program()
        system_id = f"rts.{name}"
        lir = {
            "ir_version": ir_version,
            "system_id": system_id,
            "numeric_profile": NUMERIC_PROFILE,
            "query": SCHEMA_ID,
            "instructions": instructions,
        }
        limits = {"max_instructions": MAX_INSTRUCTIONS, "max_rows": 0, "allocation": "forbidden"}
        if max_struct is not None:
            limits["max_struct_per_row"] = max_struct
            limits["max_events_per_row"] = 0
        capsule = {
            "capsule_format": "0.1",
            "system_id": system_id,
            "system_rev": SYSTEM_REV,
            "capabilities": list(caps),
            "ir_version": ir_version,
            "source": f"{name}.lir.json",
            "schema": {"id": SCHEMA_ID, "version": SCHEMA_VERSION, "layout_generation": LAYOUT_GENERATION},
            "numeric_profile": NUMERIC_PROFILE,
            "fields": derive_fields(instructions, ids),
            "schedule": {"phase": PHASE, "after": list(after), "before": [], "parallel": "serial"},
            "limits": limits,
            "tests": list(tests),
        }
        out[f"{name}.capsule.json"] = json.dumps(capsule, indent=2) + "\n"
        out[f"{name}.lir.json"] = json.dumps(lir, indent=2) + "\n"
    return out


def check(dest=RECORD_DIR):
    """Compares the regenerated bytes with the files in dest. Returns a list of problems."""
    problems = []
    for fname, text in build_records().items():
        path = os.path.join(dest, fname)
        try:
            with open(path, "r", encoding="utf-8", newline="") as fh:
                current = fh.read()
        except OSError:
            problems.append(f"missing {fname}")
            continue
        if current != text:
            problems.append(f"differs {fname}")
    return problems


def write(dest):
    os.makedirs(dest, exist_ok=True)
    for fname, text in build_records().items():
        with open(os.path.join(dest, fname), "w", encoding="utf-8", newline="") as fh:
            fh.write(text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--check", action="store_true", help="compare regenerated records with capsules/rts/")
    ap.add_argument("--out", default=None, help="write the records into this directory")
    args = ap.parse_args(argv)
    if args.check:
        problems = check()
        for p in problems:
            print(p, file=sys.stderr)
        if problems:
            return 1
        print("regenerate-equality: ok (%d records)" % len(build_records()))
        return 0
    dest = args.out if args.out is not None else RECORD_DIR
    write(dest)
    print("wrote %d records to %s" % (len(build_records()), dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
