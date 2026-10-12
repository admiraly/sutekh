# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Differential test of the RTS host operators (M3-04): C operators against the Python model (M3-02).

For each scene the driver:
  1. generates the scene with bench/rts/gen (rtsgen.make_scene, the committed generator);
  2. applies a seeded overlay that the generator does not produce: dead rows, stale formation generations,
     damage arrivals (some stale, some aimed at dead or anchor rows, some saturating), and preset incoming;
  3. runs test_rts_diff (C: rts.spatial and rts.scatter through the real scheduler on a 39-field world);
  4. runs the model phases (bench/rts/ref/model.py: spatial_phase then scatter_phase) on the same rows;
  5. compares every slot of the outputs and a SHA-256 digest of the output columns.

Dead rows: the engine zeroes dead rows at every commit (su_world_zero_dead). The model's 4.1(c) writes NONE
into dead rows' target_slot before that rule applies, so for dead rows both sides are compared after the
zeroing rule (all outputs zero). Alive rows are compared exactly.

Usage: python3 -I diff_driver.py --driver <test_rts_diff> --work <scratch dir> [--quick]
Exit status 0 only when every scene matches. Standard library only.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "bench", "rts", "gen"))
sys.path.insert(0, os.path.join(REPO, "bench", "rts", "ref"))

import rtsgen  # noqa: E402  (bench/rts/gen, normative scene generator)
import model  # noqa: E402  (bench/rts/ref, independent reference model)

NONE = 0xFFFFFFFF
MASK64 = (1 << 64) - 1
OUT_FIELDS = ("anchor_x", "anchor_y", "anchor_valid", "target_slot", "target_gen", "target_dist_sq",
              "target_valid", "target_x", "target_y", "truncated", "incoming")
FIELD_NAMES = rtsgen.FIELD_NAMES
IX = rtsgen.IX


def overlay(arr, cap, seed):
    """Seeded overlay (returns per-slot alive flags and the 39-value rows, dead rows zeroed)."""
    rng = rtsgen.SplitMix64(seed)
    rows = []
    alive = []
    for s in range(cap):
        v = [int(arr[s * 39 + i]) for i in range(39)]
        a = 0 if rng.uniform(8) == 0 else 1
        alive.append(a)
        rows.append(v)
    for s in range(cap):
        v = rows[s]
        if v[IX["formation_slot"]] != NONE and rng.uniform(5) == 0:
            v[IX["formation_gen"]] = 2  # stale: live anchors have generation 1
    for s in range(cap):
        if not alive[s]:
            continue
        v = rows[s]
        if rng.uniform(4) == 0:
            pick = rng.uniform(3)
            v[IX["dmg_out"]] = (rng.next_u32() if pick == 0 else 1 + rng.uniform(100)) if rng.uniform(10) else 0
            v[IX["dmg_target"]] = rng.uniform(cap) if rng.uniform(10) else NONE
            v[IX["dmg_target_gen"]] = 1 if rng.uniform(5) else 2
        if rng.uniform(4) == 0:
            v[IX["incoming"]] = rng.uniform(50)
    for s in range(cap):
        if not alive[s]:
            rows[s] = [0] * 39
    return alive, rows


def model_world(cap, alive, rows):
    w = model.World(cap)
    for s in range(cap):
        w.alive[s] = alive[s]
        w.gen[s] = 1 if alive[s] else 2
        for i, name in enumerate(FIELD_NAMES):
            w.cols[name][s] = rows[s][i] if alive[s] else 0
    return w


def digest_columns(cols, cap):
    h = hashlib.sha256()
    for name in OUT_FIELDS:
        h.update(struct.pack("<%dI" % cap, *cols[name][:cap]))
    return h.hexdigest()


def run_scene(driver, work, label, arr, cap, seed):
    alive, rows = overlay(arr, cap, seed)
    inp = os.path.join(work, label + ".in")
    outp = os.path.join(work, label + ".out")
    with open(inp, "w", encoding="ascii") as fh:
        fh.write("cap %d\n" % cap)
        for s in range(cap):
            fh.write("%d %s\n" % (alive[s], " ".join(str(x) for x in rows[s])))
    proc = subprocess.run([driver, inp, outp], capture_output=True, text=True)
    if proc.returncode != 0:
        return False, "driver exit %d: %s" % (proc.returncode, proc.stderr.strip())
    with open(outp, encoding="ascii") as fh:
        lines = [ln.split() for ln in fh if ln.strip()]
    if len(lines) != cap:
        return False, "driver wrote %d lines for %d slots" % (len(lines), cap)

    # model
    w = model_world(cap, alive, rows)
    model.spatial_phase(w)
    model.scatter_phase(w)
    truncated_rows = w.last_truncated_rows

    mismatches = []
    c_cols = {name: [0] * cap for name in OUT_FIELDS}
    m_cols = {name: [0] * cap for name in OUT_FIELDS}
    for s in range(cap):
        vals = [int(x) for x in lines[s]]
        if len(vals) != len(OUT_FIELDS):
            return False, "driver line %d has %d values" % (s, len(vals))
        for k, name in enumerate(OUT_FIELDS):
            c_cols[name][s] = vals[k]
            mv = w.cols[name][s]
            if not alive[s]:
                mv = 0  # engine zero-dead rule (see module docstring)
            m_cols[name][s] = mv
            if vals[k] != mv and len(mismatches) < 8:
                mismatches.append("slot %d %s: C %d model %d (alive %d)" % (s, name, vals[k], mv, alive[s]))
    c_digest = digest_columns(c_cols, cap)
    m_digest = digest_columns(m_cols, cap)
    ok = not mismatches and c_digest == m_digest
    n_alive = sum(alive)
    n_targets = sum(1 for s in range(cap) if m_cols["target_valid"][s] == 1)
    n_trunc = sum(1 for s in range(cap) if m_cols["truncated"][s] == 1)
    n_scatter = sum(1 for s in range(cap) if m_cols["incoming"][s] != 0)
    detail = ("alive %d/%d, valid targets %d, truncated %d (model truncated_rows %d), nonzero incoming %d, "
              "digest C %s model %s" % (n_alive, cap, n_targets, n_trunc, truncated_rows, n_scatter,
                                        c_digest[:16], m_digest[:16]))
    if mismatches:
        detail += "; " + "; ".join(mismatches)
    return ok, detail


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--driver", required=True, help="path to the test_rts_diff executable")
    ap.add_argument("--work", required=True, help="scratch directory (created if missing)")
    ap.add_argument("--quick", action="store_true", help="skip the 16384-row scenes")
    args = ap.parse_args(argv)
    os.makedirs(args.work, exist_ok=True)
    profile = rtsgen.load_profile()
    specs = {s["name"]: s for s in rtsgen.SCENE_SPECS}
    names = ["duel16", "mixed-1024", "congested-1024"]
    if not args.quick:
        names += ["congested-16384", "sparse-16384"]
    overlays = [1, 2]
    failures = 0
    runs = 0
    for name in names:
        spec = specs[name]
        arr, _params, scene_digest = rtsgen.make_scene(profile, spec)
        for k in overlays:
            seed = (spec["seed"] ^ (k * 0x9E3779B97F4A7C15)) & MASK64
            label = "%s-o%d" % (name, k)
            ok, detail = run_scene(args.driver, args.work, label, arr, spec["rows"], seed)
            runs += 1
            if not ok:
                failures += 1
            print("%s %s: %s" % ("PASS" if ok else "FAIL", label, detail))
            sys.stdout.flush()
    print("rts_native_diff: %d runs, %d failures" % (runs, failures))
    return 0 if failures == 0 and runs > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
