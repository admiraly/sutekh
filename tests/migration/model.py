# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Independent reference model for the M2-01a migration tests.

Reads the same fixtures the C test reads (tests/fixtures/snapshot02/worked_0_2.json and
tests/fixtures/migration/*) and writes the expected destination schema, retired IDs, columns and the expected
spawn-after-migration row to a JSON file. The C test compares the engine's output against this file and never
computes expectations from engine code. Standard library only.

Semantics modelled (contract M2_MIGRATION_CONTRACT.md sections 2, 4):
  - a destination row at index r >= high_water is zero;
  - a dead slot (alive == 0) is zero in every column;
  - an added field that the transform does not write is its default on alive rows below high_water;
  - a kept field that the transform does not write is copied from the old column on alive rows;
  - a transform-written field takes the value the transform computes from the OLD values (read-old, write-new);
    the mixed transform below computes alpha' = alpha + b and d' = alpha * b, both wrapping at 2^32;
  - a spawn after migration writes each field's default and no transform value;
  - the new slot is the lowest index that is neither alive nor retired (free slots below high_water are
    reused first; slots at or above high_water are free).

Run: python3 -I tests/migration/model.py --fixtures tests/fixtures --out <path>
"""
from __future__ import annotations

import argparse
import json
import os
import sys

M32 = 0xFFFFFFFF
RETIRED_GEN = 0xFFFFFFFF


def load(path):
    with open(path, "rb") as fh:
        return json.loads(fh.read().decode("utf-8"))


def base_world(fixtures):
    snap = load(os.path.join(fixtures, "snapshot02", "worked_0_2.json"))
    fields = [(f["id"], f["name"]) for f in snap["fields"]]
    cap = snap["capacity"]
    hw = snap["high_water"]
    slots = snap["slots"]
    assert len(slots) == hw
    alive = [s[1] for s in slots]
    gens = [s[0] for s in slots]
    retired_ids = []
    columns = {name: [int(v) for v in snap["columns"][name]] for _, name in fields}
    for name in columns:
        assert len(columns[name]) == hw
    return {
        "fields": fields,
        "cap": cap,
        "hw": hw,
        "alive": alive,
        "gens": gens,
        "columns": columns,
        "retired": retired_ids,
        "tick": snap["tick"],
        "gen": snap["layout_generation"],
    }


def apply_ops(base, doc):
    """Returns (dst fields as dicts sorted by id, retired ids sorted, transform writes or None, source names)."""
    assert doc["from_layout_generation"] == base["gen"]
    assert doc["to_layout_generation"] == base["gen"] + 1
    schema = {fid: {"id": fid, "name": name, "default": 0, "src": True} for fid, name in base["fields"]}
    retired = set(base["retired"])
    writes = None
    for op in doc["ops"]:
        kind = op["op"]
        if kind == "add_field":
            f = op["field"]
            assert f["id"] not in schema and f["id"] not in retired
            schema[f["id"]] = {"id": f["id"], "name": f["name"], "default": f["default"], "src": False}
        elif kind == "rename_field":
            schema[op["id"]]["name"] = op["new_name"]
        elif kind == "remove_field":
            del schema[op["id"]]
            retired.add(op["id"])
        elif kind == "transform":
            writes = load(os.path.join(FIXTURES_DIR, "migration", op["document"]))["writes"]
        else:
            raise SystemExit("model: unknown op " + kind)
    dst = [schema[k] for k in sorted(schema)]
    return dst, sorted(retired), writes


def transform_values(kind, base, alive_row):
    """Values the mixed transform stores for alive row r, from OLD values."""
    a = base["columns"]["a"][alive_row]
    b = base["columns"]["b"][alive_row]
    return {10: (a + b) & M32, 30: (a * b) & M32}


def expected_for(base, dst, retired, writes, scenario):
    cap = base["cap"]
    hw = base["hw"]
    name_by_src = {fid: name for fid, name in base["fields"]}
    cols = {}
    for f in dst:
        fid = f["id"]
        col = [0] * cap
        for r in range(hw):
            if not base["alive"][r]:
                continue
            if writes is not None and fid in writes:
                col[r] = transform_values(scenario, base, r)[fid]
            elif f["src"]:
                # kept field: old value under its source name (the source column is keyed by source name)
                col[r] = base["columns"][name_by_src[fid]][r]
            else:
                col[r] = f["default"]
        cols[f["name"]] = col
    # spawn after migration: lowest slot neither alive nor retired (slots at or above high_water are free)
    slot = None
    for r in range(cap):
        if r < hw:
            if not base["alive"][r] and base["gens"][r] != RETIRED_GEN:
                slot = r
                break
        else:
            slot = r
            break
    spawn_values = {f["name"]: f["default"] for f in dst}
    return {
        "schema": [{"id": f["id"], "name": f["name"], "default": f["default"]} for f in dst],
        "retired": retired,
        "columns": cols,
        "spawn": {"slot": slot, "values": spawn_values},
    }


FIXTURES_DIR = ""


def main(argv):
    global FIXTURES_DIR
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    FIXTURES_DIR = args.fixtures
    base = base_world(args.fixtures)
    scenarios = {
        "add": "m_add.json",
        "rename": "m_rename.json",
        "remove": "m_remove.json",
        "mixed": "m_mixed.json",
    }
    out = {"model": "migration-model-0.1", "capacity": base["cap"], "high_water": base["hw"], "scenarios": {}}
    for name, fname in scenarios.items():
        doc = load(os.path.join(args.fixtures, "migration", fname))
        dst, retired, writes = apply_ops(base, doc)
        out["scenarios"][name] = expected_for(base, dst, retired, writes, name)
    # The mixed transform overwrites rows; the model checks the one independent identity it can state:
    # alpha' = a + b and d' = a * b on every alive row below high_water.
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1, sort_keys=True)
        fh.write("\n")
    print("model: wrote %s (%d scenarios)" % (args.out, len(scenarios)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
