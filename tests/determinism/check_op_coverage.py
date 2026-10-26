# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Every IR 0.3/0.4 operation must run in a determinism-matrix workload, with its result reaching state.

The operation list comes from the table itself (det_op_list prints src/ir/su_ops03.c). For every operation the
test looks for an instruction in the matrix workloads' IR (MATRIX_LIR below) whose result is live: transitively
used by a store, an emit_event, a despawn_self or a select/condition that feeds one. Stores, which define no
register, count when the instruction exists. An operation missing from the workloads, or present only as dead code,
fails the test and is named.

    python3 -I tests/determinism/check_op_coverage.py <det_op_list binary>
    python3 -I tests/determinism/check_op_coverage.py --self-test   (the liveness rule on hand-made programs)
"""
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# The IR of every IR-driven matrix workload (tests/determinism/det_matrix.c).
MATRIX_LIR = [
    "capsules/rts/damage.lir.json", "capsules/rts/goal.lir.json", "capsules/rts/combat.lir.json",
    "capsules/rts/move.lir.json",
    "capsules/space3d_racer/race.lir.json", "capsules/space3d_racer/move.lir.json",
    "tests/determinism/sixdof.lir.json", "tests/determinism/churn.lir.json",
    "tests/determinism/s3move.lir.json", "tests/determinism/s3move_c.lir.json",
    "tests/determinism/bits04.lir.json", "tests/determinism/gather04.lir.json",
    "tests/determinism/handle04.lir.json",
]

SINKS = ("store_", "emit_event", "despawn_self")


def live_ops(ins):
    """Opcode names whose results are live (reach a sink)."""
    by_dst = {i["dst"]: i for i in ins if "dst" in i and not i["op"].startswith("store_")}
    live = set()
    work = []
    used = set()
    for i in ins:
        if i["op"].startswith(SINKS):
            used.add(i["op"])
            for k, v in i.items():
                if k not in ("op", "field", "x", "y", "z", "kind") and isinstance(v, str) and v in by_dst:
                    work.append(v)
            if i["op"].startswith("store_") and "dst" in i and i["dst"] in by_dst:  # store_v3 keeps src in dst
                work.append(i["dst"])
    while work:
        r = work.pop()
        if r in live:
            continue
        live.add(r)
        i = by_dst[r]
        used.add(i["op"])
        for k, v in i.items():
            if k not in ("op", "dst", "field") and isinstance(v, str) and v in by_dst:
                work.append(v)
    return used


def self_test():
    prog = [
        {"op": "load_i32", "dst": "a", "field": "x"},
        {"op": "abs_i32", "dst": "dead", "a": "a"},             # result unused: must not count
        {"op": "neg_i32", "dst": "n", "a": "a"},
        {"op": "lt_i32", "dst": "c", "a": "n", "b": "a"},       # reaches state through the select
        {"op": "select_i32", "dst": "s", "cond": "c", "on_true": "n", "on_false": "a"},
        {"op": "store_i32", "field": "y", "src": "s"},
        {"op": "make_v3", "dst": "v", "x": "q", "y": "q", "z": "q"},
        {"op": "const_q32", "dst": "q", "num": 1, "den": 1},
        {"op": "store_v3", "x": "f1", "y": "f2", "z": "f3", "src": "v"},
    ]
    got = live_ops(prog)
    want = {"load_i32", "neg_i32", "lt_i32", "select_i32", "store_i32", "make_v3", "const_q32", "store_v3"}
    if got != want:
        print("self-test FAILED: live %s, expected %s" % (sorted(got), sorted(want)))
        return 1
    print("check_op_coverage self-test: ok (dead abs_i32 not counted)")
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    out = subprocess.run([argv[0]], check=True, capture_output=True, text=True).stdout
    table = [ln.split()[0] for ln in out.splitlines() if ln.strip()]
    covered = {}
    for rel in MATRIX_LIR:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print("MISSING FILE: %s (MATRIX_LIR must name the matrix workloads' IR)" % rel)
            return 1
        with open(path, encoding="utf-8") as f:
            for op in live_ops(json.load(f)["instructions"]):
                covered.setdefault(op, rel)
    missing = [op for op in table if op not in covered]
    print("check_op_coverage: %d table operations, %d live in matrix workloads"
          % (len(table), len(table) - len(missing)))
    for op in missing:
        print("MISSING: %s (not run with a live result by any matrix workload)" % op)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
