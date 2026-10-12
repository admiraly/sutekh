#!/usr/bin/env python3
"""Generates the capability and system_rev verifier fixtures (audit item 4, C04).

Run: python3 -I tests/fixtures/caps/make_caps.py   (from any directory; writes beside this file)

Every case is ir02/ok_valid (IR 0.2: despawn_self and emit_event) with the capsule keys
"capabilities" and "system_rev" varied. The base IR is copied unchanged, so a case differs from
the base only in the capsule keys. The expected codes are the codes the verifier must return:
SU_OK for a granted module, SU_E_CAPABILITY_DENIED for an operation or name that is not granted,
and SU_E_TYPE for a malformed key.
"""
import copy
import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
IR02 = os.path.join(HERE, "..", "ir02")


def write(name, obj):
    with open(os.path.join(HERE, name), "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2)
        fh.write("\n")


def main():
    with open(os.path.join(IR02, "ok_valid.capsule.json"), encoding="utf-8") as fh:
        base = json.load(fh)
    shutil.copyfile(os.path.join(IR02, "ok_valid.lir.json"), os.path.join(HERE, "base.lir.json"))

    both = ["despawn_self", "emit_event"]
    cases = [
        # id, extra capsule keys, expected code, stage
        ("grant_both_ok", {"capabilities": both, "system_rev": 3}, "SU_OK", None),
        ("grant_both_no_rev_ok", {"capabilities": both}, "SU_OK", None),
        ("grant_despawn_only", {"capabilities": ["despawn_self"]}, "SU_E_CAPABILITY_DENIED", 5),
        ("grant_emit_only", {"capabilities": ["emit_event"]}, "SU_E_CAPABILITY_DENIED", 5),
        ("grant_empty", {"capabilities": []}, "SU_E_CAPABILITY_DENIED", 5),
        ("grant_unknown_name", {"capabilities": ["despawn_self", "teleport"]}, "SU_E_CAPABILITY_DENIED", 4),
        ("grant_duplicate", {"capabilities": ["despawn_self", "despawn_self"]}, "SU_E_TYPE", 4),
        ("grant_non_string", {"capabilities": [7]}, "SU_E_TYPE", 4),
        ("grant_not_array", {"capabilities": "despawn_self"}, "SU_E_TYPE", 4),
        ("system_rev_zero", {"system_rev": 0}, "SU_E_TYPE", 4),
        ("system_rev_string", {"system_rev": "2"}, "SU_E_TYPE", 4),
    ]

    manifest = {
        "description": "Audit item 4 (C04) verifier cases: capsule capabilities and system_rev over ir02/ok_valid. "
                       "A capsule without capabilities keeps the M0-M2 verdicts (ir02 manifest, unchanged).",
        "generator": "tests/fixtures/caps/make_caps.py",
        "cases": [],
    }
    for cid, extra, code, stage in cases:
        cap = copy.deepcopy(base)
        cap.update(extra)
        write(cid + ".capsule.json", cap)
        entry = {
            "id": cid,
            "path": "base.lir.json",
            "capsule": cid + ".capsule.json",
            "expected_code": code,
            "note": json.dumps(extra, sort_keys=True),
        }
        if stage is not None:
            entry["stage"] = stage
        manifest["cases"].append(entry)
    write("manifest.json", manifest)


if __name__ == "__main__":
    main()
