# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Compares determinism-matrix manifests (det_matrix --out) from different builds, OSes and tiers.

Every manifest must equal the first one line for line (comment lines starting with '#' and CR are ignored). The
first differing line is printed with both values, which names the workload and the first differing keyframe.

    python3 -I tests/determinism/compare_manifests.py tests/determinism/expected_digests.txt m1.txt m2.txt ...
"""
import sys


def load(path):
    with open(path, "rb") as f:
        text = f.read().decode("ascii")
    return [ln.rstrip("\r") for ln in text.split("\n") if ln.strip() and not ln.startswith("#")]


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    ref = load(argv[0])
    bad = 0
    for path in argv[1:]:
        got = load(path)
        diff = next((i for i in range(max(len(ref), len(got)))
                     if i >= len(ref) or i >= len(got) or ref[i] != got[i]), None)
        if diff is None:
            print("same  %s (%d lines)" % (path, len(got)))
            continue
        bad += 1
        print("DIFF  %s at line %d\n  %s: %s\n  %s: %s" % (
            path, diff + 1, argv[0], ref[diff] if diff < len(ref) else "(end)",
            path, got[diff] if diff < len(got) else "(end)"))
    print("%d of %d manifests equal %s" % (len(argv) - 1 - bad, len(argv) - 1, argv[0]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
