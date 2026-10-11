# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Write the M0-06 fixtures: edit swap set, candidate manifest, t09 failure set, protocol corpus.

Standard library only. Expected columns come from tests/harness/reference_eval.py,
never from an engine. Run: python3 -I tests/harness/make_m0_06_fixtures.py
The generator refuses to write anything if a known-answer or consistency check fails.
"""
from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import reference_eval as ref  # noqa: E402

FIX = os.path.join(REPO, "tests", "fixtures")
U32_MAX = ref.U32_MAX


def dump(path: str, doc) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(doc, indent=2) + "\n")


def write_bytes(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


def capsule(source: str, *, pos_write: bool = True, vel_read: bool = True,
            system_id: str = "demo.movement", max_rows: int = 1000000,
            after: list | None = None, tests: list | None = None) -> dict:
    return {
        "capsule_format": "0.1",
        "system_id": system_id,
        "source": source,
        "schema": {"id": "demo.actor", "version": 1, "layout_generation": 1},
        "numeric_profile": "u32_mod_v1",
        "fields": [
            {"id": 1, "name": "position_x", "type": "u32", "read": True, "write": pos_write},
            {"id": 2, "name": "velocity_x", "type": "u32", "read": vel_read, "write": False},
        ],
        "schedule": {"phase": "simulate", "after": after or [], "before": [], "parallel": "serial"},
        "limits": {"max_instructions": 4096, "max_rows": max_rows, "allocation": "forbidden"},
        "tests": tests or [],
    }


def lir(instructions: list[dict], system_id: str = "demo.movement") -> dict:
    return {
        "ir_version": "0.1",
        "system_id": system_id,
        "numeric_profile": "u32_mod_v1",
        "query": "demo.actor",
        "instructions": instructions,
    }


def world(columns: dict) -> dict:
    n = len(next(iter(columns.values())))
    return {
        "fixture_format": "0.1",
        "schema_id": "demo.actor",
        "schema_version": 1,
        "tick": 0,
        "entity_ids": list(range(1, n + 1)),
        "columns": columns,
    }


V1 = [
    {"op": "load_u32", "dst": "p", "field": "position_x"},
    {"op": "load_u32", "dst": "v", "field": "velocity_x"},
    {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
    {"op": "store_u32", "field": "position_x", "src": "next"},
]
V2 = [
    {"op": "load_u32", "dst": "p", "field": "position_x"},
    {"op": "load_u32", "dst": "v", "field": "velocity_x"},
    {"op": "const_u32", "dst": "two", "value": 2},
    {"op": "mul_u32", "dst": "twice_v", "a": "v", "b": "two"},
    {"op": "add_u32", "dst": "next", "a": "p", "b": "twice_v"},
    {"op": "store_u32", "field": "position_x", "src": "next"},
]
POSITIONS = [0, 1, U32_MAX, 7]
VELOCITIES = [1, 2, 1, 0]
EDIT_COLUMNS = {"position_x": POSITIONS, "velocity_x": VELOCITIES}


def check_inputs_against_examples() -> None:
    """Edit inputs must equal the checked-in examples, so the two sets cannot drift."""
    ex = os.path.join(REPO, "examples")
    with open(os.path.join(ex, "movement_v2.lir.json"), encoding="utf-8") as fh:
        if json.load(fh) != lir(V2):
            raise SystemExit("V2 program differs from examples/movement_v2.lir.json")
    with open(os.path.join(ex, "world.json"), encoding="utf-8") as fh:
        if json.load(fh)["columns"] != EDIT_COLUMNS:
            raise SystemExit("world columns differ from examples/world.json")


def build_edit(files: dict) -> dict:
    """Edit set: v1 for ticks 1-2, v2 from effective tick 2 (steps 3-4). Expected by tick."""
    v1_cap = capsule("movement.lir.json", tests=["swap_schedule.json"])
    v2_cap = capsule("movement_v2.lir.json", tests=["swap_schedule.json"])
    prog_v1, prog_v2 = lir(V1), lir(V2)

    expected = {}
    cols = EDIT_COLUMNS
    expected["0"] = {"columns": cols, "last_step_capsule": None}
    for t in range(1, 5):
        prog, cap, name = (prog_v1, v1_cap, "movement.capsule.json") if t <= 2 else (prog_v2, v2_cap, "movement_v2.capsule.json")
        cols = ref.run_tick(prog, cap, cols)
        expected[str(t)] = {"columns": cols, "last_step_capsule": name}

    # Cross-check: the first two steps are v1, and the last two are v2 from the start state.
    v1_two = ref.run_ticks(prog_v1, v1_cap, EDIT_COLUMNS, 2)
    if expected["2"]["columns"] != v1_two:
        raise SystemExit("edit expectation at tick 2 disagrees with run_ticks(v1, 2)")
    v2_from_two = ref.run_ticks(prog_v2, v2_cap, v1_two, 2)
    if expected["4"]["columns"] != v2_from_two:
        raise SystemExit("edit expectation at tick 4 disagrees with v2 continued from tick 2")
    if expected["2"]["columns"]["position_x"] != [2, 5, 1, 7]:
        raise SystemExit("hand check: v1 two ticks should be [2,5,1,7]")
    if expected["4"]["columns"]["position_x"] != [6, 13, 5, 7]:
        raise SystemExit("hand check: tick 4 should be [6,13,5,7]")

    files["edit/world.json"] = world(EDIT_COLUMNS)
    files["edit/movement.lir.json"] = prog_v1
    files["edit/movement.capsule.json"] = v1_cap
    files["edit/movement_v2.lir.json"] = prog_v2
    files["edit/movement_v2.capsule.json"] = v2_cap
    files["edit/swap_schedule.json"] = {
        "test_format": "0.1",
        "system_id": "demo.movement",
        "world": "world.json",
        "base_capsule": "movement.capsule.json",
        "candidate_capsule": "movement_v2.capsule.json",
        "total_ticks": 4,
        "swap": {
            "effective_tick": 2,
            "note": "candidate is installed while the world is at tick 2; the first step run with "
                    "it starts from tick 2 and produces tick 3",
        },
        "expected_by_tick": expected,
    }
    return expected


def build_candidates(files: dict) -> None:
    """Candidates for T06 (permission) and T07 (stale revision); see candidates/manifest.json."""
    # Declares position_x write:false but the program stores it: wrong field permission.
    files["edit/candidates/wrong_permission/capsule.json"] = capsule(
        "program.lir.json", pos_write=False, tests=["../../swap_schedule.json"])
    files["edit/candidates/wrong_permission/program.lir.json"] = lir(V1)
    # Paths in the manifest are relative to the manifest's own directory (edit/candidates/).
    files["edit/candidates/manifest.json"] = [
        {
            "id": "wrong_permission",
            "kind": "permission",
            "capsule": "wrong_permission/capsule.json",
            "expected_code": "SU_E_UNDECLARED_WRITE",
            "note": "capsule declares position_x write:false while the body stores position_x",
        },
        {
            "id": "stale_revision",
            "kind": "stale_revision",
            "capsule": "../movement_v2.capsule.json",
            "expected_world_revision": "@captured_before_mutation",
            "expected_code": "SU_E_STALE_REVISION",
            "note": "the harness substitutes the world revision captured before the first step; "
                    "the candidate itself is valid, so only the revision is wrong",
        },
    ]


def build_t09(files: dict) -> None:
    """Two systems: A writes position_x; B (after A) has max_rows 2 and fails on a 4-row world."""
    files["t09/world.json"] = world(EDIT_COLUMNS)
    files["t09/movement_a.lir.json"] = lir(V1)
    files["t09/movement_a.capsule.json"] = capsule("movement_a.lir.json")
    files["t09/movement_b.lir.json"] = lir(V1, system_id="demo.movement_b")
    files["t09/movement_b.capsule.json"] = capsule(
        "movement_b.lir.json", system_id="demo.movement_b", max_rows=2, after=["demo.movement"])
    files["t09/manifest.json"] = {
        "test_format": "0.1",
        "world": "world.json",
        "systems_in_order": ["movement_a.capsule.json", "movement_b.capsule.json"],
        "expected_failure_code": "SU_E_RESOURCE_LIMIT",
        "expected_columns_after_failed_step": EDIT_COLUMNS,
        "expected_tick_after_failed_step": 0,
        "note": "B exceeds its own limits.max_rows after A has written staging; the published "
                "tick root must not change. If the engine rejects this at admission instead, the "
                "harness reports not_run rather than passing.",
    }


PROTOCOL_CASES = [
    {"id": "duplicate_key", "file": "duplicate_key.jsonl", "expected_code": "SU_E_PARSE",
     "note": "same key 'method' twice in one object (contract section 4: duplicate keys -> SU_E_PARSE)",
     "bytes": b'{"protocol":"0.1","id":"dup-1","method":"engine.info","method":"engine.info","params":{}}\n'},
    {"id": "invalid_utf8", "file": "invalid_utf8.jsonl", "expected_code": "SU_E_PARSE",
     "note": "bytes 0xff 0xfe are not UTF-8 inside a string value",
     "bytes": b'{"protocol":"0.1","id":"utf-1","method":"engine.info","params":{"name":"\xff\xfe"}}\n'},
    {"id": "wrong_protocol", "file": "wrong_protocol.jsonl", "expected_code": "SU_E_TYPE",
     "note": "protocol 9.9 is not 0.1. No dedicated code exists; SU_E_TYPE is a judgment call for "
             "the spec owner to confirm",
     "bytes": b'{"protocol":"9.9","id":"proto-1","method":"engine.info","params":{}}\n'},
    {"id": "unknown_method", "file": "unknown_method.jsonl", "expected_code": "SU_E_UNSUPPORTED_METHOD",
     "note": "method name is not in AGENT_RPC.md",
     "bytes": b'{"protocol":"0.1","id":"m-1","method":"world.teleport","params":{}}\n'},
    {"id": "path_traversal", "file": "path_traversal.jsonl", "expected_code": "SU_E_CAPABILITY_DENIED",
     "note": "capsule_path escapes the project root with ..",
     "bytes": (b'{"protocol":"0.1","id":"p-1","method":"capsule.submit","params":'
               b'{"capsule_path":"../../../../etc/passwd","expected_world_revision":"world-0"}}\n')},
    {"id": "absolute_path", "file": "absolute_path.jsonl", "expected_code": "SU_E_CAPABILITY_DENIED",
     "note": "capsule_path is absolute",
     "bytes": (b'{"protocol":"0.1","id":"p-2","method":"capsule.submit","params":'
               b'{"capsule_path":"/etc/passwd","expected_world_revision":"world-0"}}\n')},
    {"id": "oversize_1mib", "generated": {"kind": "oversize", "bytes": 1048577},
     "expected_code": "SU_E_RESOURCE_LIMIT",
     "note": "generated by the harness at runtime: one line of 1 MiB + 1 byte (AGENT_RPC section 1 limit)"},
]


def build_protocol() -> None:
    for case in PROTOCOL_CASES:
        if "file" in case:
            write_bytes(os.path.join(FIX, "protocol", case["file"]), case["bytes"])
            # Each file must be exactly one line so the harness sends one message per session.
            if case["bytes"].count(b"\n") != 1:
                raise SystemExit(f"{case['id']} must contain exactly one newline")
    dump(os.path.join(FIX, "protocol", "cases.json"), {
        "format": "0.1",
        "maximum_message_bytes": 1048576,
        "cases": [{k: v for k, v in c.items() if k != "bytes"} for c in PROTOCOL_CASES],
    })


def main() -> int:
    check_inputs_against_examples()
    if ref.self_check_known_answers():
        raise SystemExit("reference evaluator fails its known-answer table")
    files: dict = {}
    build_edit(files)
    build_candidates(files)
    build_t09(files)
    for rel, doc in files.items():
        dump(os.path.join(FIX, rel), doc)
    build_protocol()
    print(f"wrote {len(files)} JSON files and {len(PROTOCOL_CASES)} protocol cases under tests/fixtures/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
