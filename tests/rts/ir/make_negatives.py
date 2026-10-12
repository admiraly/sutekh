# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Writes the contract 10.4 negative fixtures into tests/rts/ir/fixtures/neg/ (run once; committed).

Each case is a capsule/LIR pair derived from the committed records (engine projection: system_rev and
capabilities removed, so the engine's own refusal is what each case exercises) with one defect. The
expected code is written to negative_cases.json. Cases the engine cannot reach from the IR verifier are
listed with status "not_run" and the reason; they are not reported as passing.

Usage: python3 -I tests/rts/ir/make_negatives.py
"""
from __future__ import annotations

import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ir_harness as h  # noqa: E402

OUT = os.path.join(HERE, "fixtures", "neg")


def base(name):
    cap, lir = h.load_record(name)
    return h.projection(cap), copy.deepcopy(lir)


def first_index(instructions, op, pred=lambda ins: True):
    for i, ins in enumerate(instructions):
        if ins["op"] == op and pred(ins):
            return i
    raise ValueError(f"no {op}")


def load_dst(instructions, field):
    for ins in instructions:
        if ins["op"] == "load_u32" and ins["field"] == field:
            return ins["dst"]
    raise ValueError(field)


def case_missing_store():
    cap, lir = base("goal")
    lir["instructions"] = [ins for ins in lir["instructions"]
                           if not (ins["op"] == "store_u32" and ins["field"] == "goal_y")]
    return cap, lir


def case_undeclared_write():
    cap, lir = base("goal")
    i = first_index(lir["instructions"], "store_u32", lambda ins: ins["field"] == "goal_y")
    lir["instructions"][i]["field"] = "pos_x"  # pos_x is declared read only
    return cap, lir


def case_undeclared_read():
    cap, lir = base("goal")
    # add a load of hp, which goal does not declare
    lir["instructions"].insert(0, {"op": "load_u32", "dst": "hp_probe", "field": "hp"})
    return cap, lir


def case_zero_one_ir_with_despawn():
    cap, lir = base("damage")
    cap["ir_version"] = "0.1"
    lir["ir_version"] = "0.1"
    cap["limits"].pop("max_struct_per_row")
    cap["limits"].pop("max_events_per_row")
    return cap, lir


def case_despawn_above_cap():
    cap, lir = base("damage")
    lir["instructions"].append({"op": "despawn_self", "cond": h_bool_reg(lir)})
    return cap, lir


def h_bool_reg(lir):
    for ins in lir["instructions"]:
        if ins["op"] in ("eq_u32", "lt_u32"):
            return ins["dst"]
    raise ValueError("no bool register")


def case_emit_event_without_cap():
    """10.4 'emit_event': a 0.2 module with emit_event and no declared event cap (max_events_per_row 0)."""
    cap, lir = base("damage")
    b = h_bool_reg(lir)
    lir["instructions"].append({"op": "emit_event", "cond": b, "kind": 1, "a": load_dst(lir["instructions"], "incoming"),
                                "b": load_dst(lir["instructions"], "hp")})
    return cap, lir


def case_ir_version_mismatch():
    cap, lir = base("goal")
    lir["ir_version"] = "0.2"  # capsule says 0.1
    return cap, lir


def case_duplicate_register():
    cap, lir = base("goal")
    lir["instructions"][1]["dst"] = lir["instructions"][0]["dst"]
    return cap, lir


def case_undefined_register():
    cap, lir = base("goal")
    i = first_index(lir["instructions"], "store_u32", lambda ins: ins["field"] == "goal_x")
    lir["instructions"][i]["src"] = "no_such_reg"
    return cap, lir


def case_select_raw_u32_condition():
    cap, lir = base("goal")
    i = first_index(lir["instructions"], "select_u32")
    # a u32 register that is already defined at the select (the first constant): not a bool (N2)
    const_reg = next(ins["dst"] for ins in lir["instructions"][:i] if ins["op"] == "const_u32")
    lir["instructions"][i]["cond"] = const_reg
    return cap, lir


def case_0_2_op_missing_cap_in_0_2():
    """Unknown opcode in 0.2 (despawn_self spelled differently) is refused as an unknown opcode."""
    cap, lir = base("damage")
    i = first_index(lir["instructions"], "despawn_self")
    lir["instructions"][i]["op"] = "despawn_me"
    return cap, lir


CASES = (
    # name, builder, expected code (engine), status
    ("missing_store_goal_y", case_missing_store, "SU_E_MISSING_STORE", "run"),
    ("undeclared_write_pos_x", case_undeclared_write, "SU_E_UNDECLARED_WRITE", "run"),
    ("undeclared_read_hp", case_undeclared_read, "SU_E_UNDECLARED_READ", "run"),
    ("ir_0_2_despawn_in_0_1_module", case_zero_one_ir_with_despawn, "SU_E_UNKNOWN_OPCODE", "run"),
    ("despawn_above_max_struct", case_despawn_above_cap, "SU_E_RESOURCE_LIMIT", "run"),
    ("emit_event_without_event_cap", case_emit_event_without_cap, "SU_E_RESOURCE_LIMIT", "run"),
    ("ir_version_mismatch", case_ir_version_mismatch, "SU_E_SCHEMA_MISMATCH", "run"),
    ("duplicate_register", case_duplicate_register, "SU_E_DUPLICATE_REGISTER", "run"),
    ("undefined_register", case_undefined_register, "SU_E_UNDEFINED_REGISTER", "run"),
    ("select_raw_u32_condition", case_select_raw_u32_condition, "SU_E_TYPE", "run"),
    ("unknown_opcode_in_0_2", case_0_2_op_missing_cap_in_0_2, "SU_E_UNKNOWN_OPCODE", "run"),
    ("native_lowering_refused_0_2", None, "SU_E_CAPABILITY_DENIED", "not_run"),
    ("schedule_after_later_or_unknown_system", None, "SU_E_SCHEMA_MISMATCH", "not_run"),
)

NOT_RUN_REASON = {
    "native_lowering_refused_0_2": "native lane needs SUTEKH_NATIVE=ON and the asmjit sources, which are not "
                                   "vendored (third_party holds only asmjit.lock.json) and are not fetched offline",
    "schedule_after_later_or_unknown_system": "the schedule-order check is in src/runtime/worker.c and needs a "
                                              "world.create with a manifest; the IR verifier does not read after lists",
}


def main():
    os.makedirs(OUT, exist_ok=True)
    manifest = []
    for name, builder, code, status in CASES:
        if status != "run":
            manifest.append({"name": name, "status": status, "expected_code": code,
                             "reason": NOT_RUN_REASON[name]})
            continue
        cap, lir = builder()
        cap["source"] = f"{name}.lir.json"  # the capsule binds its own fixture's LIR, not the record's
        cap_path = os.path.join(OUT, f"{name}.capsule.json")
        lir_path = os.path.join(OUT, f"{name}.lir.json")
        with open(cap_path, "w", encoding="utf-8") as fh:
            json.dump(cap, fh, indent=2)
            fh.write("\n")
        with open(lir_path, "w", encoding="utf-8") as fh:
            json.dump(lir, fh, indent=2)
            fh.write("\n")
        manifest.append({"name": name, "status": "run", "expected_code": code,
                         "capsule": f"{name}.capsule.json", "source": f"{name}.lir.json"})
    with open(os.path.join(HERE, "fixtures", "negative_cases.json"), "w", encoding="utf-8") as fh:
        json.dump({"format": "rts-ir-negatives/0.1", "cases": manifest}, fh, indent=2)
        fh.write("\n")
    print("wrote", len(manifest), "cases")


if __name__ == "__main__":
    main()
