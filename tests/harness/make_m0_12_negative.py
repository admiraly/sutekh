#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Generate the M0-12 negative fixtures for T04 and the T11 layout-mismatch candidate.

Writes tests/fixtures/m0_neg/ (a capsule and an IR file per case, a manifest, and one layout
candidate). Every expected error code is written here from M0_CONTRACT.md section 1, the fixed
verifier stage order, and is NOT read from the engine. Each case names its stage so the harness
self-test can check the code against an independent stage table.

Stage order (M0_CONTRACT section 1; the first failing stage wins):
  1 size/depth limits            -> SU_E_RESOURCE_LIMIT
  2 JSON syntax / UTF-8          -> SU_E_PARSE
  3 duplicate object keys        -> SU_E_DUPLICATE_KEY
  4 top-level shape / profile    -> SU_E_TYPE
  5 schema semantics (ids, names, system_id, query, profile between IR and capsule)
                                 -> SU_E_SCHEMA_MISMATCH
  6 instructions                 -> SU_E_UNDECLARED_READ / SU_E_UNDECLARED_WRITE / ...
  7 end of body                  -> SU_E_MISSING_STORE

Run from the repository root:  python3 -I tests/harness/make_m0_12_negative.py
"""
import copy
import json
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_REL = "tests/fixtures/m0_neg"
IR_LIMIT_BYTES = 256 * 1024  # SU_LIMIT_IR_SOURCE_BYTES, include/sutekh/ir.h

BASE_IR = "tests/fixtures/edit/movement.lir.json"
BASE_CAP = "tests/fixtures/invalid/movement.capsule.json"
LAYOUT_SRC_IR = "tests/fixtures/edit/movement_v2.lir.json"
LAYOUT_SRC_CAP = "tests/fixtures/edit/movement_v2.capsule.json"
LAYOUT_SRC_TEST = "tests/fixtures/edit/movement_v2.test.json"
LAYOUT_WORLD = "tests/fixtures/edit/world.json"


def load(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as fh:
        return json.load(fh)


def dump_text(obj) -> str:
    return json.dumps(obj, indent=2) + "\n"


def nested(depth: int):
    doc = 0
    for _ in range(depth):
        doc = [doc]
    return doc


def build_cases(base_ir: dict, base_cap: dict):
    """Return the case list. Each case: id, stage, code, ir_bytes, cap (dict), note."""
    cases = []

    def add(cid, stage, code, ir_bytes, cap, note):
        cases.append({"id": cid, "stage": stage, "code": code, "ir_bytes": ir_bytes, "cap": cap, "note": note})

    ir_text = dump_text(base_ir)
    ir_ok = ir_text.encode("utf-8")
    cap_ok = copy.deepcopy(base_cap)

    # Stage 1: size. Valid JSON padded with whitespace to one byte over the 256 KiB limit.
    body = ir_text.rstrip("\n")
    pad = IR_LIMIT_BYTES + 1 - len(body.encode("utf-8")) - 1
    if pad < 0:
        raise SystemExit("base IR is larger than the oversize target")
    padded = body[:1] + (" " * pad) + body[1:] + "\n"
    if len(padded.encode("utf-8")) != IR_LIMIT_BYTES + 1:
        raise SystemExit("oversize IR does not have the intended length")
    add("oversize_ir", 1, "SU_E_RESOURCE_LIMIT", padded.encode("utf-8"), cap_ok,
        "valid JSON of 262145 bytes (one byte over SU_LIMIT_IR_SOURCE_BYTES)")

    cap_deep = copy.deepcopy(cap_ok)
    cap_deep["extra"] = nested(40)
    add("capsule_depth_40", 1, "SU_E_RESOURCE_LIMIT", ir_ok, cap_deep,
        "capsule with an unknown key holding 40 nested arrays (cap 32)")

    ir_deep = copy.deepcopy(base_ir)
    ir_deep["extra"] = nested(40)
    add("ir_depth_40", 1, "SU_E_RESOURCE_LIMIT", dump_text(ir_deep).encode("utf-8"), cap_ok,
        "IR with an unknown key holding 40 nested arrays (cap 32)")

    # Stage 2: JSON syntax and UTF-8.
    add("malformed_json_truncated", 2, "SU_E_PARSE", ir_text[: len(ir_text) // 2].encode("utf-8"), cap_ok,
        "IR cut off in the middle of an instruction object")
    bad_utf8 = ir_text.replace('"dst": "p"', '"dst": "ÿ"').encode("latin-1")
    if b"\xff" not in bad_utf8:
        raise SystemExit("invalid UTF-8 substitution failed")
    add("invalid_utf8_in_ir", 2, "SU_E_PARSE", bad_utf8, cap_ok,
        "byte 0xff inside a string value (not UTF-8)")

    # Stage 3: a duplicate object key inside one instruction. The syntax is valid, so stage 2 passes.
    dup_text = ir_text.replace('"dst": "p"', '"dst": "p",\n      "dst": "p"', 1)
    if dup_text == ir_text:
        raise SystemExit("duplicate-key substitution failed")
    add("duplicate_key_in_ir", 3, "SU_E_DUPLICATE_KEY", dup_text.encode("utf-8"), cap_ok,
        "instruction 0 has the key dst twice")

    # Stage 4: top-level shape and profile support in the capsule.
    cap_profile = copy.deepcopy(cap_ok)
    cap_profile["numeric_profile"] = "u32_mod_v2"
    add("capsule_profile_unsupported", 4, "SU_E_TYPE", ir_ok, cap_profile,
        "capsule names a profile M0 does not support; stage 4 covers unsupported profiles")

    # Stage 5: schema and identity.
    ir_sys = copy.deepcopy(base_ir)
    ir_sys["system_id"] = "demo.other"
    add("schema_system_id_mismatch", 5, "SU_E_SCHEMA_MISMATCH", dump_text(ir_sys).encode("utf-8"), cap_ok,
        "IR system_id demo.other, capsule demo.movement")

    ir_query = copy.deepcopy(base_ir)
    ir_query["query"] = "demo.other"
    add("schema_query_mismatch", 5, "SU_E_SCHEMA_MISMATCH", dump_text(ir_query).encode("utf-8"), cap_ok,
        "IR query demo.other, capsule schema.id demo.actor")

    cap_dup_id = copy.deepcopy(cap_ok)
    cap_dup_id["fields"][1]["id"] = 1
    add("schema_duplicate_field_id", 5, "SU_E_SCHEMA_MISMATCH", ir_ok, cap_dup_id,
        "two fields with id 1 (position_x and velocity_x)")

    cap_dup_name = copy.deepcopy(cap_ok)
    cap_dup_name["fields"][1]["name"] = "position_x"
    add("schema_duplicate_field_name", 5, "SU_E_SCHEMA_MISMATCH", ir_ok, cap_dup_name,
        "two fields named position_x with ids 1 and 2")

    # Stage 6: instruction-level permission failures.
    ir_unknown_read = copy.deepcopy(base_ir)
    ir_unknown_read["instructions"][1]["field"] = "velocity_y"
    add("undeclared_read_unknown_field", 6, "SU_E_UNDECLARED_READ", dump_text(ir_unknown_read).encode("utf-8"),
        cap_ok, "load of velocity_y, which the capsule does not declare")

    # Stage 7: end of body.
    ir_nostore = copy.deepcopy(base_ir)
    ir_nostore["instructions"] = [i for i in ir_nostore["instructions"] if i["op"] != "store_u32"]
    add("missing_store", 7, "SU_E_MISSING_STORE", dump_text(ir_nostore).encode("utf-8"), cap_ok,
        "writable position_x is never stored")

    return cases


def write_case_files(out_dir: str, cases: list) -> list:
    entries = []
    for case in cases:
        ir_name = f"{case['id']}.lir.json"
        cap_name = f"{case['id']}.capsule.json"
        cap = copy.deepcopy(case["cap"])
        cap["source"] = ir_name
        with open(os.path.join(out_dir, ir_name), "wb") as fh:
            fh.write(case["ir_bytes"])
        with open(os.path.join(out_dir, cap_name), "w", encoding="utf-8") as fh:
            fh.write(dump_text(cap))
        entries.append({"id": case["id"], "path": ir_name, "capsule": cap_name,
                        "stage": case["stage"], "expected_code": case["code"], "note": case["note"]})
    return entries


def main() -> int:
    os.chdir(REPO)
    out_dir = os.path.join(REPO, OUT_REL)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    base_ir, base_cap = load(BASE_IR), load(BASE_CAP)
    cases = build_cases(base_ir, base_cap)
    entries = write_case_files(out_dir, cases)

    # Valid control for the matrix (must be accepted): same capsule as the base, its own IR.
    with open(os.path.join(out_dir, "valid.lir.json"), "w", encoding="utf-8") as fh:
        fh.write(dump_text(base_ir))
    valid_cap = copy.deepcopy(base_cap)
    valid_cap["source"] = "valid.lir.json"
    with open(os.path.join(out_dir, "valid.capsule.json"), "w", encoding="utf-8") as fh:
        fh.write(dump_text(valid_cap))

    # Layout candidate for T11: the v2 movement system declared with layout_generation 2. It
    # evaluates correctly, so only the install-time layout check can refuse it.
    shutil.copyfile(os.path.join(REPO, LAYOUT_SRC_IR), os.path.join(out_dir, "layout_gen2.lir.json"))
    layout_cap = load(LAYOUT_SRC_CAP)
    layout_cap["source"] = "layout_gen2.lir.json"
    layout_cap["schema"]["layout_generation"] = 2
    with open(os.path.join(out_dir, "layout_gen2.capsule.json"), "w", encoding="utf-8") as fh:
        fh.write(dump_text(layout_cap))
    # Its own test file: the engine requires the test file to name the candidate's capsule by basename, and
    # the expectations are the v2 expectations (same behaviour, different layout generation).
    layout_test = load(LAYOUT_SRC_TEST)
    layout_test["capsule"] = "layout_gen2.capsule.json"
    layout_test["world"] = "world.json"
    with open(os.path.join(out_dir, "layout_gen2.test.json"), "w", encoding="utf-8") as fh:
        fh.write(dump_text(layout_test))
    shutil.copyfile(os.path.join(REPO, LAYOUT_WORLD), os.path.join(out_dir, "world.json"))

    manifest = {
        "description": "M0-12 T04 negative matrix. Expected codes follow M0_CONTRACT section 1 stage order.",
        "generator": "tests/harness/make_m0_12_negative.py",
        "capsule": f"{OUT_REL}/valid.capsule.json",
        "cases": entries,
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as fh:
        fh.write(dump_text(manifest))
    print(f"wrote {len(entries)} negative cases, a valid control and the layout candidate to {OUT_REL}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
