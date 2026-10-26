# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Mutation check of the determinism matrix (maintainer tool; it edits sources, so it is not a ctest).

Each mutant changes one authoritative computation in a way a different compiler, libm or library could plausibly
differ. The matrix must catch it, so det_matrix --expect must fail. For each mutant the tool applies the edit,
rebuilds det_matrix in BUILD, runs the matrix on the given tier, and restores the source (also on error or
interrupt). A mutant that is not caught is reported, and the exit status is 1.

    python3 -I tests/determinism/run_mutants.py --build <cmake build dir> [--only NAME]
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# name, file, exact old text, new text, tier, workloads that must catch it (--only), why
MUTANTS = [
    ("ray_sphere_libm_sqrt", "profiles/space3d/native/space3d_geom.c",
     "s = su_q32_subf(su_q32_neg(b), su_q32_sqrtf(disc));",
     "s = su_q32_subf(su_q32_neg(b), (int64_t)sqrt((double)disc * 4294967296.0));",
     "planned", ["s3_query", "s3_prox", "s3_shape", "s3_compound"],
     "libm double sqrt instead of the exact integer root in the sphere sweep"),
    ("fast_rne_ties_up", "src/ir/su_num_fast.h",
     "uint32_t inc = (uint32_t)(frac > 0x80000000u) | "
     "((uint32_t)(frac == 0x80000000u) & (uint32_t)((uint64_t)q & 1u));",
     "uint32_t inc = (uint32_t)(frac >= 0x80000000u);",
     "planned", ["sixdof", "probe04", "racer16"],
     "round half up instead of half to even in the fast 128-bit rounding (planned and native tiers)"),
    ("reference_rne_ties_up", "src/ir/su_num.c",
     "if (frac > 0x80000000u || (frac == 0x80000000u && (q.lo & 1u) != 0u)) {",
     "if (frac >= 0x80000000u) {",
     "reference", ["sixdof", "probe04"],
     "round half up in the reference library's 128-bit rounding (reference tier)"),
    ("u32_shl_masks_count", "src/ir/su_ops03.h",
     "return n >= 32u ? 0u : a << n;",
     "return a << (n & 31u);",
     "planned", ["probe04"],
     "x86 shift-count masking instead of the IR 0.4 rule (count >= 32 gives 0)"),
    ("find_eq_last_match", "src/vm/su_vm.c",
     "                found = (uint32_t)s;\n                break;",
     "                found = (uint32_t)s;",
     "reference", ["probe04"],
     "find_eq_u32 returns the last match instead of the first (reference tier)"),
    ("handle_live_ignores_generation", "src/ir/su_ops03.h",
     "return (generations != NULL ? generations[slot] : 1u) == gen ? 1u : 0u;",
     "return (generations != NULL ? generations[slot] : 1u) != 0u ? 1u : 0u;",
     "planned", ["handles04"],
     "handle_live answers from alive alone, so a stale handle to a reused slot reads as live (both VM tiers)"),
    ("event_merge_reversed", "src/world/entities/su_cmdq.c",
     "    if (r != 0) {\n        return r < 0;\n    }\n    return b1 < b2;",
     "    if (r != 0) {\n        return r > 0;\n    }\n    return b1 < b2;",
     "planned", ["churn3k"],
     "per-chunk command/event buffers merged in reverse canonical order; only churn3k has more than one chunk "
     "(churn, one chunk, does not reach this merge: observed 2026-10-26)"),
]


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--only")
    args = ap.parse_args(argv)
    build = os.path.abspath(args.build)
    exe = os.path.join(build, "det_matrix")
    expect = os.path.join(ROOT, "tests", "determinism", "expected_digests.txt")
    fixtures = os.path.join(build, "det_fixtures")
    missed = 0
    for name, rel, old, new, tier, works, why in MUTANTS:
        if args.only and args.only != name:
            continue
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8", newline="") as f:
            src = f.read()
        if src.count(old) != 1:
            print("%-24s SETUP ERROR: the original text occurs %d times in %s" % (name, src.count(old), rel))
            missed += 1
            continue
        try:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(src.replace(old, new))
            b = run(["cmake", "--build", build, "-j", "6", "--target", "det_matrix"])
            if b.returncode != 0:
                print("%-24s BUILD FAILED\n%s" % (name, b.stdout[-2000:] + b.stderr[-2000:]))
                missed += 1
                continue
            caught = []
            for wl in works:
                r = run([exe, "--fixtures", fixtures, "--tier", tier, "--rollback", "0", "--only", wl,
                         "--expect", expect])
                first = next((ln.strip() for ln in r.stderr.splitlines() if ln.strip().startswith("expected:")), "")
                caught.append((wl, r.returncode != 0 and "MISMATCH" in r.stderr, first))
            ok = all(c for _, c, _ in caught)
            missed += 0 if ok else 1
            print("%-24s %s  (%s; tier %s)" % (name, "caught" if ok else "NOT CAUGHT", why, tier))
            for wl, c, first in caught:
                print("    %-14s %s %s" % (wl, "mismatch" if c else "MATCHED (not caught)", first[:100]))
        finally:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(src)
    b = run(["cmake", "--build", build, "-j", "6", "--target", "det_matrix"])  # back to the unmutated binary
    print("%d mutant(s) not caught" % missed)
    return 1 if missed or b.returncode != 0 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
