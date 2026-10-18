#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Generates the capsules of the embedded space3d example (examples/host_space3d).

The game schema "arena" has its own field IDs and names. space3d.query is bound to it schema-relatively: every
operator lane of profiles/space3d/space3d_query.capsule.json is bound to the game lane "a_<lane>" at ID 100 + k
(k = the lane's position in the operator's table), so the record carries "binds" on every entry.

Systems, in schedule order:
  arena.score  IR 0.3  gate progress and bump counters; emit_event kind 1 (gate passed: a = gate index,
                       b = new score) and kind 2 (bump: a = other slot, b = contact count)
  arena.move   IR 0.3  bounce off last tick's first contact, input acceleration (Q16.16 i32 axes), integration
  space3d.query host   swept contacts and gate crossings of this tick's motion, consumed next tick

  gen_arena.py            write the files
  gen_arena.py --check    exit 1 when a committed file differs from what this script writes
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SCHEMA = {"id": "arena", "version": 1, "layout_generation": 1}
BASE_ID = 100
GATES = 3


def operator_lanes(record="space3d_query.capsule.json"):
    with open(os.path.join(ROOT, "profiles", "space3d", record), encoding="utf-8") as f:
        rec = json.load(f)
    return rec, rec["host"]["fields"]


def game_id(lanes, lane_name):
    for k, f in enumerate(lanes):
        if f["name"] == lane_name:
            return BASE_ID + k
    raise KeyError(lane_name)


def bound_record(rec, lanes):
    out = {
        "capsule_format": "0.1",
        "system_id": rec["system_id"],
        "system_rev": 1,
        "capabilities": [],
        "schema": SCHEMA,
        "numeric_profile": rec["numeric_profile"],
        "host": {
            "operator": rec["host"]["operator"],
            "operator_version": rec["host"]["operator_version"],
            "constants_hash": rec["host"]["constants_hash"],
            "fields": [
                {"id": BASE_ID + k, "name": "a_" + f["name"], "type": f["type"], "access": f["access"],
                 "binds": f["name"]}
                for k, f in enumerate(lanes)
            ],
        },
        "schedule": {"phase": "query", "after": [], "before": [], "parallel": "serial"},
        "limits": {"max_instructions": 1, "max_rows": 65536, "allocation": "forbidden"},
        "tests": ["examples/host_space3d/arena_host.c"],
    }
    return out


def field(fid, name, typ, read, write):
    return {"id": fid, "name": name, "type": typ, "read": read, "write": write}


def op_field(lanes, name, typ, read, write):
    """A game field bound to an operator lane (q32 fields by their low lane)."""
    lane = name + ".lo" if typ == "q32" else name
    return field(game_id(lanes, lane), "a_" + name, typ, read, write)


def capsule(system_id, source, fields, phase, events):
    c = {
        "capsule_format": "0.1",
        "ir_version": "0.3",
        "system_id": system_id,
        "source": source,
        "schema": SCHEMA,
        "numeric_profile": "fixed_q32_v1",
        "fields": sorted(fields, key=lambda f: f["id"]),
        "schedule": {"phase": phase, "after": [], "before": [], "parallel": "serial"},
        "limits": {"max_instructions": 4096, "max_rows": 65536, "allocation": "forbidden"},
        "tests": ["examples/host_space3d/arena_host.c"],
    }
    if events:
        c["limits"]["max_events_per_row"] = 2
        c["capabilities"] = ["emit_event"]
    return c


def lir(system_id, insns):
    return {"ir_version": "0.3", "system_id": system_id, "numeric_profile": "fixed_q32_v1", "query": SCHEMA["id"],
            "instructions": insns}


def score_system(lanes):
    fields = [
        field(1, "score", "u32", True, True),
        field(2, "bumps", "u32", True, True),
        op_field(lanes, "next_gate", "u32", True, True),
        op_field(lanes, "hit_count", "u32", True, False),
        op_field(lanes, "c0_slot", "u32", True, False),
        op_field(lanes, "gate_dir", "u32", True, False),
    ]
    ins = [
        {"op": "const_u32", "dst": "one", "value": 1},
        {"op": "const_u32", "dst": "zero", "value": 0},
        {"op": "const_u32", "dst": "ngates", "value": GATES},
        {"op": "load_u32", "dst": "gd", "field": "a_gate_dir"},
        {"op": "eq_u32", "dst": "fwd", "a": "gd", "b": "one"},
        {"op": "load_u32", "dst": "ng", "field": "a_next_gate"},
        {"op": "add_u32", "dst": "ng1", "a": "ng", "b": "one"},
        {"op": "eq_u32", "dst": "wrap", "a": "ng1", "b": "ngates"},
        {"op": "select_u32", "dst": "ng2", "cond": "wrap", "on_true": "zero", "on_false": "ng1"},
        {"op": "select_u32", "dst": "ngn", "cond": "fwd", "on_true": "ng2", "on_false": "ng"},
        {"op": "load_u32", "dst": "sc", "field": "score"},
        {"op": "add_u32", "dst": "sc1", "a": "sc", "b": "one"},
        {"op": "select_u32", "dst": "scn", "cond": "fwd", "on_true": "sc1", "on_false": "sc"},
        {"op": "load_u32", "dst": "hc", "field": "a_hit_count"},
        {"op": "lt_u32", "dst": "hit", "a": "zero", "b": "hc"},
        {"op": "load_u32", "dst": "bp", "field": "bumps"},
        {"op": "add_u32", "dst": "bp1", "a": "bp", "b": "one"},
        {"op": "select_u32", "dst": "bpn", "cond": "hit", "on_true": "bp1", "on_false": "bp"},
        {"op": "load_u32", "dst": "other", "field": "a_c0_slot"},
        {"op": "emit_event", "cond": "fwd", "kind": 1, "a": "ng", "b": "scn"},
        {"op": "emit_event", "cond": "hit", "kind": 2, "a": "other", "b": "hc"},
        {"op": "store_u32", "field": "a_next_gate", "src": "ngn"},
        {"op": "store_u32", "field": "score", "src": "scn"},
        {"op": "store_u32", "field": "bumps", "src": "bpn"},
    ]
    return capsule("arena.score", "arena_score.lir.json", fields, "simulate", True), lir("arena.score", ins)


def q_input(axis):
    """Q16.16 i32 input axis -> q32 register."""
    return [
        {"op": "load_i32", "dst": "i_" + axis, "field": "in_" + axis},
        {"op": "i64_from_i32", "dst": "w_" + axis, "a": "i_" + axis},
        {"op": "shl_i64", "dst": "s_" + axis, "a": "w_" + axis, "n": "sixteen"},
        {"op": "q32_from_raw", "dst": "q_" + axis, "a": "s_" + axis},
    ]


def move_system(lanes):
    fields = [
        field(3, "in_ax", "i32", True, False),
        field(4, "in_ay", "i32", True, False),
        field(5, "in_az", "i32", True, False),
        field(10, "vel_x", "q32", True, True),
        field(12, "vel_y", "q32", True, True),
        field(14, "vel_z", "q32", True, True),
        op_field(lanes, "kind", "u32", True, False),
        op_field(lanes, "pos_x", "q32", True, True),
        op_field(lanes, "pos_y", "q32", True, True),
        op_field(lanes, "pos_z", "q32", True, True),
        op_field(lanes, "prev_x", "q32", False, True),
        op_field(lanes, "prev_y", "q32", False, True),
        op_field(lanes, "prev_z", "q32", False, True),
        op_field(lanes, "hit_count", "u32", True, False),
        op_field(lanes, "c0_nx", "q32", True, False),
        op_field(lanes, "c0_ny", "q32", True, False),
        op_field(lanes, "c0_nz", "q32", True, False),
    ]
    ins = [
        {"op": "const_q32", "dst": "dt", "num": 1, "den": 120},
        {"op": "const_q32", "dst": "qzero", "num": 0, "den": 1},
        {"op": "const_q32", "dst": "restitution1", "num": 18, "den": 10},
        {"op": "const_u32", "dst": "sixteen", "value": 16},
        {"op": "const_u32", "dst": "one", "value": 1},
        {"op": "const_u32", "dst": "zero", "value": 0},
        {"op": "load_u32", "dst": "kind", "field": "a_kind"},
        {"op": "eq_u32", "dst": "mover", "a": "kind", "b": "one"},
        {"op": "load_v3", "dst": "p", "x": "a_pos_x", "y": "a_pos_y", "z": "a_pos_z"},
        {"op": "load_v3", "dst": "v", "x": "vel_x", "y": "vel_y", "z": "vel_z"},
        # Bounce off last tick's first contact when moving into it: v -= (1 + e) (v.n) n, e = 0.8.
        {"op": "load_u32", "dst": "hc", "field": "a_hit_count"},
        {"op": "lt_u32", "dst": "hit", "a": "zero", "b": "hc"},
        {"op": "load_v3", "dst": "n", "x": "a_c0_nx", "y": "a_c0_ny", "z": "a_c0_nz"},
        {"op": "v3_dot", "dst": "vn", "a": "v", "b": "n"},
        {"op": "lt_q32", "dst": "into", "a": "vn", "b": "qzero"},
        {"op": "and_bool", "dst": "bounce", "a": "hit", "b": "into"},
        {"op": "mul_q32", "dst": "k", "a": "vn", "b": "restitution1"},
        {"op": "v3_scale", "dst": "dvn", "v": "n", "s": "k"},
        {"op": "v3_sub", "dst": "vb", "a": "v", "b": "dvn"},
        {"op": "select_v3", "dst": "v1", "cond": "bounce", "on_true": "vb", "on_false": "v"},
    ]
    for axis in ("ax", "ay", "az"):
        ins += q_input(axis)
    ins += [
        {"op": "make_v3", "dst": "acc", "x": "q_ax", "y": "q_ay", "z": "q_az"},
        {"op": "v3_scale", "dst": "dv", "v": "acc", "s": "dt"},
        {"op": "v3_add", "dst": "v2", "a": "v1", "b": "dv"},
        {"op": "v3_scale", "dst": "dp", "v": "v2", "s": "dt"},
        {"op": "v3_add", "dst": "p2", "a": "p", "b": "dp"},
        {"op": "select_v3", "dst": "pn", "cond": "mover", "on_true": "p2", "on_false": "p"},
        {"op": "select_v3", "dst": "vnew", "cond": "mover", "on_true": "v2", "on_false": "v"},
        {"op": "store_v3", "src": "p", "x": "a_prev_x", "y": "a_prev_y", "z": "a_prev_z"},
        {"op": "store_v3", "src": "pn", "x": "a_pos_x", "y": "a_pos_y", "z": "a_pos_z"},
        {"op": "store_v3", "src": "vnew", "x": "vel_x", "y": "vel_y", "z": "vel_z"},
    ]
    return capsule("arena.move", "arena_move.lir.json", fields, "simulate", False), lir("arena.move", ins)


def respond_system(lanes):
    """Same-tick response (post-query ordering): runs after space3d.query in the same tick. A mover that moved into
    its first contact is put back to the contact point (prev + toi * (pos - prev)) and its velocity reflected, so the
    committed state of the contact tick does not penetrate. A mover with a proximity hit and no contact emits a
    near-miss event (kind 3: a = the other slot, b = 0)."""
    fields = [
        field(10, "vel_x", "q32", True, True),
        field(12, "vel_y", "q32", True, True),
        field(14, "vel_z", "q32", True, True),
        op_field(lanes, "kind", "u32", True, False),
        op_field(lanes, "pos_x", "q32", True, True),
        op_field(lanes, "pos_y", "q32", True, True),
        op_field(lanes, "pos_z", "q32", True, True),
        op_field(lanes, "prev_x", "q32", True, False),
        op_field(lanes, "prev_y", "q32", True, False),
        op_field(lanes, "prev_z", "q32", True, False),
        op_field(lanes, "hit_count", "u32", True, False),
        op_field(lanes, "c0_toi", "q32", True, False),
        op_field(lanes, "c0_nx", "q32", True, False),
        op_field(lanes, "c0_ny", "q32", True, False),
        op_field(lanes, "c0_nz", "q32", True, False),
        op_field(lanes, "prox_slot", "u32", True, False),
    ]
    ins = [
        {"op": "const_q32", "dst": "qzero", "num": 0, "den": 1},
        {"op": "const_q32", "dst": "restitution1", "num": 18, "den": 10},
        {"op": "const_u32", "dst": "one", "value": 1},
        {"op": "const_u32", "dst": "zero", "value": 0},
        {"op": "const_u32", "dst": "none", "value": 4294967295},
        {"op": "load_u32", "dst": "kind", "field": "a_kind"},
        {"op": "eq_u32", "dst": "mover", "a": "kind", "b": "one"},
        {"op": "load_v3", "dst": "p", "x": "a_pos_x", "y": "a_pos_y", "z": "a_pos_z"},
        {"op": "load_v3", "dst": "p0", "x": "a_prev_x", "y": "a_prev_y", "z": "a_prev_z"},
        {"op": "load_v3", "dst": "v", "x": "vel_x", "y": "vel_y", "z": "vel_z"},
        {"op": "load_u32", "dst": "hc", "field": "a_hit_count"},
        {"op": "lt_u32", "dst": "hit", "a": "zero", "b": "hc"},
        {"op": "load_q32", "dst": "toi", "field": "a_c0_toi"},
        {"op": "load_v3", "dst": "n", "x": "a_c0_nx", "y": "a_c0_ny", "z": "a_c0_nz"},
        {"op": "v3_sub", "dst": "d", "a": "p", "b": "p0"},
        {"op": "v3_dot", "dst": "dn", "a": "d", "b": "n"},
        {"op": "lt_q32", "dst": "into", "a": "dn", "b": "qzero"},
        {"op": "and_bool", "dst": "hit_into", "a": "hit", "b": "into"},
        {"op": "and_bool", "dst": "clamp", "a": "hit_into", "b": "mover"},
        {"op": "v3_scale", "dst": "dt", "v": "d", "s": "toi"},
        {"op": "v3_add", "dst": "pc", "a": "p0", "b": "dt"},
        {"op": "v3_dot", "dst": "vn", "a": "v", "b": "n"},
        {"op": "lt_q32", "dst": "vinto", "a": "vn", "b": "qzero"},
        {"op": "and_bool", "dst": "bounce", "a": "clamp", "b": "vinto"},
        {"op": "mul_q32", "dst": "k", "a": "vn", "b": "restitution1"},
        {"op": "v3_scale", "dst": "dvn", "v": "n", "s": "k"},
        {"op": "v3_sub", "dst": "vb", "a": "v", "b": "dvn"},
        {"op": "select_v3", "dst": "pn", "cond": "clamp", "on_true": "pc", "on_false": "p"},
        {"op": "select_v3", "dst": "vnew", "cond": "bounce", "on_true": "vb", "on_false": "v"},
        {"op": "load_u32", "dst": "ps", "field": "a_prox_slot"},
        {"op": "eq_u32", "dst": "far", "a": "ps", "b": "none"},
        {"op": "not_bool", "dst": "near", "a": "far"},
        {"op": "not_bool", "dst": "nohit", "a": "hit"},
        {"op": "and_bool", "dst": "near_nohit", "a": "near", "b": "nohit"},
        {"op": "and_bool", "dst": "near_miss", "a": "near_nohit", "b": "mover"},
        {"op": "emit_event", "cond": "near_miss", "kind": 3, "a": "ps", "b": "zero"},
        {"op": "store_v3", "src": "pn", "x": "a_pos_x", "y": "a_pos_y", "z": "a_pos_z"},
        {"op": "store_v3", "src": "vnew", "x": "vel_x", "y": "vel_y", "z": "vel_z"},
    ]
    cap = capsule("arena.respond", "arena_respond.lir.json", fields, "simulate", True)
    # Post-query ordering: an IR system may name a host operator of an earlier phase ("query" < "simulate").
    cap["schedule"]["after"] = ["arena.move", "space3d.query_prox"]
    return cap, lir("arena.respond", ins)


def outputs():
    rec, lanes = operator_lanes()
    prec, planes = operator_lanes("space3d_query_prox.capsule.json")
    score_cap, score_lir = score_system(lanes)
    move_cap, move_lir = move_system(lanes)
    respond_cap, respond_lir = respond_system(planes)
    return {
        "arena_respond.capsule.json": respond_cap,
        "arena_respond.lir.json": respond_lir,
        "arena_query_prox.capsule.json": bound_record(prec, planes),
        "arena_score.capsule.json": score_cap,
        "arena_score.lir.json": score_lir,
        "arena_move.capsule.json": move_cap,
        "arena_move.lir.json": move_lir,
        "arena_query.capsule.json": bound_record(rec, lanes),
    }


def main():
    check = "--check" in sys.argv[1:]
    bad = 0
    for name, obj in outputs().items():
        text = json.dumps(obj, indent=1) + "\n"
        path = os.path.join(HERE, name)
        if check:
            try:
                with open(path, encoding="utf-8", newline="") as f:
                    same = f.read() == text
            except OSError:
                same = False
            if not same:
                print("stale: %s" % name)
                bad += 1
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    print("gen_arena: %s" % ("%d stale" % bad if bad else ("up to date" if check else "written")))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
