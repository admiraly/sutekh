#!/usr/bin/env python3
"""Generates the M0-07 reload fixtures in this directory.

Run: python3 -I tests/reload/make_fixtures.py   (from any directory; writes beside this file)

Each system is a capsule (metadata and field bindings) with its SU-LIR source. Expected
results come from the tiny arithmetic model below, written independently of the C
engine, so an engine bug cannot define its own expectation. All arithmetic is u32 modulo
2^32, the numeric profile u32_mod_v1.

Systems:
  sys_a       demo.movement  position_x += velocity_x            (v1, layout gen 1)
  sys_a_v2    demo.movement  position_x += 2 * velocity_x        (v2 behaviour, same ID)
  sys_a_bad   demo.movement  position_x -= velocity_x            (wrong behaviour)
  sys_a_gen2  demo.movement  same program as sys_a, layout gen 2 (schema change)
  sys_b       demo.drift     velocity_x += 1                      (second system)
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
M = 1 << 32
ROWS = {"entity_ids": [1, 2, 3, 4], "position_x": [0, 1, 4294967295, 7], "velocity_x": [1, 2, 1, 0]}


def write(name, obj):
    with open(os.path.join(HERE, name), "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2)
        fh.write("\n")


def ir(system_id, instructions):
    return {
        "ir_version": "0.1",
        "system_id": system_id,
        "numeric_profile": "u32_mod_v1",
        "query": "demo.actor",
        "instructions": instructions,
    }


PROGRAMS = {
    "sys_a": ("demo.movement", [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]),
    "sys_a_v2": ("demo.movement", [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "const_u32", "dst": "two", "value": 2},
        {"op": "mul_u32", "dst": "twice_v", "a": "v", "b": "two"},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "twice_v"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]),
    "sys_a_bad": ("demo.movement", [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "sub_u32", "dst": "next", "a": "p", "b": "v"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]),
    "sys_a_gen2": ("demo.movement", [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]),
    # Refusal fixtures (audit item 2): each is a candidate that a code-only install must refuse.
    #   sys_b_conflict  demo.drift writes position_x with no ordering edge to demo.movement (write/write)
    #   sys_a_cycle     demo.movement declares after demo.drift, which already runs after it (cycle)
    #   sys_b_overdecl  demo.drift declares a READ of position_x that its IR never loads (declared != derived)
    "sys_b_conflict": ("demo.drift", [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "const_u32", "dst": "one", "value": 1},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "one"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]),
    "sys_a_cycle": ("demo.movement", [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]),
    "sys_b_overdecl": ("demo.drift", [
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "const_u32", "dst": "one", "value": 1},
        {"op": "add_u32", "dst": "next", "a": "v", "b": "one"},
        {"op": "store_u32", "field": "velocity_x", "src": "next"},
    ]),
    # Gen-2 refusal fixtures for the migration commit (layout generation 2 is the migration target).
    "sys_a_gen2_cycle": ("demo.movement", [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]),
    "sys_b_gen2_conflict": ("demo.drift", [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "const_u32", "dst": "one", "value": 1},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "one"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]),
    "sys_b_gen2": ("demo.drift", [
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "const_u32", "dst": "one", "value": 1},
        {"op": "add_u32", "dst": "next", "a": "v", "b": "one"},
        {"op": "store_u32", "field": "velocity_x", "src": "next"},
    ]),
    "sys_b": ("demo.drift", [
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "const_u32", "dst": "one", "value": 1},
        {"op": "add_u32", "dst": "next", "a": "v", "b": "one"},
        {"op": "store_u32", "field": "velocity_x", "src": "next"},
    ]),
}

# Field bindings per capsule: (id, name, read, write).
MOVE_FIELDS = [(1, "position_x", True, True), (2, "velocity_x", True, False)]
DRIFT_FIELDS = [(2, "velocity_x", True, True)]


def capsule(stem, system_id, gen, fields, tests, after=()):
    return {
        "capsule_format": "0.1",
        "system_id": system_id,
        "source": stem + ".lir.json",
        "schema": {"id": "demo.actor", "version": 1, "layout_generation": gen},
        "numeric_profile": "u32_mod_v1",
        "fields": [
            {"id": fid, "name": name, "type": "u32", "read": rd, "write": wr}
            for (fid, name, rd, wr) in fields
        ],
        "schedule": {"phase": "simulate", "after": list(after), "before": [], "parallel": "serial"},
        "limits": {"max_instructions": 4096, "max_rows": 1000000, "allocation": "forbidden"},
        "tests": tests,
    }


def model(systems, ticks):
    """systems: list of 'a1'|'a2'|'bad'|'b' applied in order each tick."""
    pos = list(ROWS["position_x"])
    vel = list(ROWS["velocity_x"])
    for _ in range(ticks):
        for s in systems:
            if s == "a1":
                pos = [(p + v) % M for p, v in zip(pos, vel)]
            elif s == "a2":
                pos = [(p + 2 * v) % M for p, v in zip(pos, vel)]
            elif s == "bad":
                pos = [(p - v) % M for p, v in zip(pos, vel)]
            elif s == "b":
                vel = [(v + 1) % M for v in vel]
            elif s == "c":
                pos = [(p + 1) % M for p in pos]
            else:
                raise ValueError(s)
    return {"position_x": pos, "velocity_x": vel}


def test_file(stem, system_id, systems, ticks_list, cap_name, bound, world="world4.json"):
    """bound: the field names the system binds. A candidate is evaluated on a world built
    from its own field table, so expected columns may only name bound fields."""
    cases = []
    for t in ticks_list:
        full = model(systems, t)
        cases.append({
            "name": "%s_%d_ticks" % (stem, t),
            "ticks": t,
            "expected_columns": {k: v for k, v in full.items() if k in bound},
        })
    return {
        "test_format": "0.1",
        "system_id": system_id,
        "capsule": cap_name,
        "world": world,
        "cases": cases,
    }


def main():
    write("world4.json", {
        "fixture_format": "0.1",
        "schema_id": "demo.actor",
        "schema_version": 1,
        "tick": 0,
        "entity_ids": ROWS["entity_ids"],
        "columns": {"position_x": ROWS["position_x"], "velocity_x": ROWS["velocity_x"]},
    })
    for stem, (system_id, body) in PROGRAMS.items():
        write(stem + ".lir.json", ir(system_id, body))

    write("sys_a.capsule.json", capsule("sys_a", "demo.movement", 1, MOVE_FIELDS, ["sys_a.test.json"]))
    write("sys_a_v2.capsule.json", capsule("sys_a_v2", "demo.movement", 1, MOVE_FIELDS, ["sys_a_v2.test.json"]))
    write("sys_a_bad.capsule.json", capsule("sys_a_bad", "demo.movement", 1, MOVE_FIELDS, ["sys_a_bad.test.json"]))
    write("sys_a_gen2.capsule.json", capsule("sys_a_gen2", "demo.movement", 2, MOVE_FIELDS, ["sys_a_gen2.test.json"]))
    write("sys_b.capsule.json", capsule("sys_b", "demo.drift", 1, DRIFT_FIELDS, ["sys_b.test.json"], after=["demo.movement"]))
    # Refusal candidates. The effect analysis refuses them before any evaluation, so they name the
    # existing test file of their system; it is never run for them.
    write("sys_b_conflict.capsule.json", capsule("sys_b_conflict", "demo.drift", 1, [(1, "position_x", True, True)], ["sys_b.test.json"]))
    write("sys_a_cycle.capsule.json", capsule("sys_a_cycle", "demo.movement", 1, MOVE_FIELDS, ["sys_a.test.json"], after=["demo.drift"]))
    write("sys_a_gen2_cycle.capsule.json", capsule("sys_a_gen2_cycle", "demo.movement", 2, MOVE_FIELDS, ["sys_a_gen2_cycle.test.json"], after=["demo.drift"]))
    write("sys_b_gen2_conflict.capsule.json", capsule("sys_b_gen2_conflict", "demo.drift", 2, [(1, "position_x", True, True)], ["sys_b_gen2_conflict.test.json"]))
    write("sys_b_gen2.capsule.json", capsule("sys_b_gen2", "demo.drift", 2, DRIFT_FIELDS, ["sys_b_gen2.test.json"], after=["demo.movement"]))
    write("sys_b_overdecl.capsule.json", capsule(
        "sys_b_overdecl", "demo.drift", 1,
        [(2, "velocity_x", True, True), (1, "position_x", True, False)], ["sys_b.test.json"], after=["demo.movement"]))

    # Evaluations: each candidate runs alone against world4.json. sys_a_bad is checked
    # against the v1 expectations, so its evaluation must fail.
    both = ["position_x", "velocity_x"]
    write("sys_a.test.json", test_file("sys_a", "demo.movement", ["a1"], [0, 1, 3], "sys_a.capsule.json", both))
    write("sys_a_v2.test.json", test_file("sys_a_v2", "demo.movement", ["a2"], [0, 1, 3], "sys_a_v2.capsule.json", both))
    write("sys_a_bad.test.json", test_file("sys_a_bad", "demo.movement", ["a1"], [1, 3], "sys_a_bad.capsule.json", both))
    write("sys_a_gen2.test.json", test_file("sys_a_gen2", "demo.movement", ["a1"], [1, 3], "sys_a_gen2.capsule.json", both))
    write("sys_b.test.json", test_file("sys_b", "demo.drift", ["b"], [1, 3], "sys_b.capsule.json", ["velocity_x"]))
    write("sys_a_gen2_cycle.test.json", test_file("sys_a_gen2_cycle", "demo.movement", ["a1"], [1, 3], "sys_a_gen2_cycle.capsule.json", both))
    write("sys_b_gen2_conflict.test.json", test_file("sys_b_gen2_conflict", "demo.drift", ["c"], [1, 3], "sys_b_gen2_conflict.capsule.json", ["position_x"]))
    write("sys_b_gen2.test.json", test_file("sys_b_gen2", "demo.drift", ["b"], [1, 3], "sys_b_gen2.capsule.json", ["velocity_x"]))


if __name__ == "__main__":
    main()
