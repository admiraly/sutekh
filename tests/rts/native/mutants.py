# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Mutation campaign for the RTS host operators (M3-04; contract 15 M3-M01..M09b, M21..M24, M32 shapes).

Each mutant is one exact text replacement in profiles/rts/native/*.c (the expected number of occurrences is
checked first). For each mutant the script:
  1. writes the mutated source in place (the original text is kept in memory and restored in a finally block);
  2. rebuilds the test targets with the given CMake build directory (a build failure is reported as INVALID,
     not as a kill);
  3. runs test_rts_ops and the quick differential driver; a nonzero exit of either is a kill;
  4. restores the original file and checks that the restored text equals the original.

A mutant that survives is either recorded as EQUIVALENT (with the reason in the table) or is a gap.
Usage: python3 -I tests/rts/native/mutants.py --build <cmake build dir> --work <scratch dir>
Standard library only. Exit status 0 when every mutant is killed or recorded as equivalent.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SPATIAL = "profiles/rts/native/rts_spatial.c"
SCATTER = "profiles/rts/native/rts_scatter.c"

# (id, file, old, new, expected occurrences, status, note)
# status: "kill" expected to be killed; "equivalent" expected to survive (reason in note).
MUTANTS = [
    ("M3-M01", SPATIAL, "(dd == q->best_dist && e < q->best_slot)", "(dd == q->best_dist && e > q->best_slot)", 1,
     "kill", "tie-break: higher slot wins"),
    ("M3-M02", SPATIAL, "uint32_t cell = (py[r] >> RTS_OPS_CELL_SHIFT) * RTS_OPS_GRID + (px[r] >> RTS_OPS_CELL_SHIFT);",
     "uint32_t cell = (py[r] >> RTS_OPS_CELL_SHIFT) * RTS_OPS_GRID + ((px[r] + 64u) >> RTS_OPS_CELL_SHIFT);", 2,
     "kill", "grid cell column off by one (both grid passes)"),
    ("M3-M03", SPATIAL, "if (dd <= q->range_sq) {", "if (dd < q->range_sq) {", 1, "kill", "range check uses <"),
    ("M3-M04", SPATIAL, "if (q->examined == RTS_OPS_BUDGET_K) {", "if (q->examined == 0xFFFFFFFFu) {", 1, "kill",
     "budget ignored: unbounded scan"),
    ("M3-M04b", SPATIAL, "q.box_total > RTS_OPS_BUDGET_K", "q.box_total >= RTS_OPS_BUDGET_K", 1, "kill",
     "truncated flag at box_total = K"),
    ("M3-M05", SPATIAL, "if (g == fgen[r]) {", "if (g == fgen[r] || g != RTS_OPS_NONE) {", 1, "kill",
     "generation check removed from gather"),
    ("M3-M24", SPATIAL, "fslot[r] < cap && g_alive[fslot[r]] != 0u", "fslot[r] < cap", 1, "kill",
     "gather without alive check (contract says equivalent; not equivalent under forged formation_gen)"),
    ("M3-M21", SPATIAL, "if (g_alive[r] != 0u && kind[r] == RTS_OPS_KIND_UNIT && hp[r] > 0u) {",
     "if (g_alive[r] != 0u) {", 2, "kill", "grid indexes every alive row (anchors included)"),
    ("M3-M22", SPATIAL, "if (e == q->r || team[e] == q->team) {",
     "if (e == q->r || (team[e] == q->team && q->range_sq == RTS_OPS_NONE)) {", 1, "kill",
     "team filter removed (the extra term is never true: range_sq <= 262144)"),
    ("M3-M23a", SPATIAL, "uint32_t x0 = cx >= d ? cx - d : 0u;", "uint32_t x0 = cx - d;", 1, "kill",
     "column clip removed (unsigned wrap at x < d)"),
    ("M3-M23b", SPATIAL, "y0 = cy >= d ? cy - d : 0u;", "y0 = cy - d;", 1, "kill",
     "row clip removed (unsigned wrap at y < d)"),
    ("M3-R0", SPATIAL, "for (d = 0; d <= rc; d++) {", "for (d = 1; d <= rc; d++) {", 1, "kill",
     "ring 0 (own cell) skipped"),
    ("M3-G", SPATIAL, "ax = px[fslot[r]];", "ax = px[r];", 1, "kill", "anchor position read from the member"),
    ("M3-DOM", SPATIAL, "(px[r] > RTS_OPS_DOMAIN_MAX || py[r] > RTS_OPS_DOMAIN_MAX)", "(0)", 1, "kill",
     "position domain check removed"),
    ("M3-RANGE", SPATIAL, "if (q->range_sq > RTS_OPS_RANGE_SQ_MAX) {", "if (0) {", 1, "kill",
     "range_sq domain check removed"),
    ("M3-FAULT-S", SPATIAL, "            if (seam(c)) {\n                return SU_E_INTERNAL;\n            }\n            memset(&q",
     "            memset(&q", 1, "kill", "spatial query fault seam disabled"),
    ("M3-NOTE-S", SPATIAL, "rts_ops_note(c, SP_W_TRUNCATED, r);", "", 1, "kill",
     "store hook missing for truncated (instrumentation coverage)"),
    ("M3-ADD", SCATTER, "inc[t] = rts_ops_sat_add(inc[t], amount);", "inc[t] = inc[t] + amount;", 1, "kill",
     "wrapping add instead of saturating add (M3-M08)"),
    ("M3-M07", SCATTER, "kind[t] == RTS_OPS_KIND_UNIT) {", "kind[t] != 0u) {", 1, "kill",
     "non-unit (anchor) target accepted (M3-M07)"),
    ("M3-M09", SCATTER, "inc[t] = rts_ops_sat_add(inc[t], amount);",
     "if (inc[t] == 0u) { inc[t] = rts_ops_sat_add(inc[t], amount); }", 1, "kill",
     "only the first contribution to a target applies (M3-M09)"),
    ("M3-M05s", SCATTER, "if (g == dgen[r]) {", "if (g == dgen[r] || g != RTS_OPS_NONE) {", 1, "kill",
     "generation check removed from scatter (M3-M05)"),
    ("M3-M06", SCATTER, "if (t < cap && g_alive[t] != 0u && kind[t] == RTS_OPS_KIND_UNIT) {",
     "if (t < cap && kind[t] == RTS_OPS_KIND_UNIT) {", 1, "equivalent",
     "dead target accepted: the write lands in a dead row and su_world_zero_dead erases it before the commit is read; "
     "the alive check is kept as defence (a defensive dead-unit state does not detect it)"),
    ("M3-M09b", SCATTER, "for (r = 0; r < cap; r++) {", "for (r = cap; r-- > 0u;) {", 1, "equivalent",
     "descending shooter order: equivalent under saturating add (M3-M09b, 4.3)"),
    ("M3-FAULT-C", SCATTER, "        if (seam(c)) {\n            return SU_E_INTERNAL;\n        }\n        if (t < cap",
     "        if (seam(c) && 0) {\n            return SU_E_INTERNAL;\n        }\n        if (t < cap", 1, "kill",
     "scatter fault seam disabled (the call stays so the helper is still used)"),
    ("M3-NOTE-C", SCATTER, "rts_ops_note(c, SC_W_INCOMING, t);", "", 1, "kill",
     "store hook missing for incoming (instrumentation coverage)"),
]


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", required=True, help="CMake build directory configured for this tree")
    ap.add_argument("--work", required=True, help="scratch directory for the differential driver")
    args = ap.parse_args(argv)
    test_ops = os.path.join(args.build, "test_rts_ops")
    test_diff = os.path.join(args.build, "test_rts_diff")
    diff_driver = os.path.join(HERE, "diff_driver.py")
    rows = []
    bad = 0
    for mid, rel, old, new, count, status, note in MUTANTS:
        path = os.path.join(REPO, rel)
        with open(path, encoding="utf-8") as fh:
            original = fh.read()
        found = original.count(old)
        if found != count:
            rows.append((mid, "BAD-SPEC", "expected %d occurrence(s), found %d" % (count, found), note))
            bad += 1
            continue
        mutated = original.replace(old, new)
        result = None
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(mutated)
            build = run(["cmake", "--build", args.build, "-j8", "--target", "test_rts_ops", "test_rts_diff"], REPO)
            if build.returncode != 0:
                result = ("INVALID", "build failed (not a kill)")
            else:
                ops = run([test_ops], REPO)
                diff = run([sys.executable, "-I", diff_driver, "--driver", test_diff, "--work", args.work, "--quick"],
                           REPO)
                if ops.returncode != 0 or diff.returncode != 0:
                    by = []
                    if ops.returncode != 0:
                        by.append("test_rts_ops")
                    if diff.returncode != 0:
                        by.append("diff_driver")
                    result = ("KILLED", "by " + " and ".join(by))
                else:
                    result = ("SURVIVED", "no test failed")
        finally:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(original)
            with open(path, encoding="utf-8") as fh:
                if fh.read() != original:
                    print("RESTORE FAILED for %s" % rel, file=sys.stderr)
                    return 3
        verdict, why = result
        if verdict == "SURVIVED" and status == "equivalent":
            verdict = "EQUIVALENT"
        elif verdict == "KILLED" and status == "equivalent":
            verdict = "UNEXPECTED-KILL"
            bad += 1
        elif verdict in ("SURVIVED", "INVALID"):
            bad += 1
        rows.append((mid, verdict, why, note))
    # Rebuild with the original sources so the build directory matches the tree again.
    run(["cmake", "--build", args.build, "-j8", "--target", "test_rts_ops", "test_rts_diff"], REPO)
    for mid, verdict, why, note in rows:
        print("%-12s %-14s %-34s %s" % (mid, verdict, why, note))
    killed = sum(1 for r in rows if r[1] == "KILLED")
    print("mutants: %d total, %d killed, %d equivalent, %d not accepted" % (
        len(rows), killed, sum(1 for r in rows if r[1] == "EQUIVALENT"), bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
