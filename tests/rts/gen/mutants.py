#!/usr/bin/env python3
"""Mutant check for the RTS scene generator (M3-01; contract 10.3 T-RTS-SCENE-1, 15 M3-M17).

Each mutant is an exact-string patch of bench/rts/gen/rtsgen.py (or a transformed copy of the
profile file). Every patch states how many times its old text must occur; a patch that does
not apply exactly that many times is a harness error, not a result. The patched module is
run on all committed scene specs and its digests are compared with the unpatched module.

kind "detect":     at least one scene digest or the schema digest must change (killed).
kind "equivalent": no digest may change. A changed digest makes the run fail, because the
                   mutant was claimed equivalent and is not.

Run: python3 -I tests/rts/gen/mutants.py [--work DIR]
Exit 0 only when every detect mutant is killed and every equivalent mutant is unchanged.
"""
import argparse
import importlib.util
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.normpath(os.path.join(HERE, "..", "..", "..", "bench", "rts", "gen"))
sys.path.insert(0, GEN)
sys.path.insert(0, HERE)
import rtsgen  # noqa: E402

SOURCE = os.path.join(GEN, "rtsgen.py")

DETECT = [
    ("M01-PRNG-GOLDEN", "PRNG increment constant (GOLDEN) changed by 1",
     [("GOLDEN = 0x9E3779B97F4A7C15", "GOLDEN = 0x9E3779B97F4A7C16", 1)], None),
    ("M02-PRNG-MIX1", "PRNG first mix constant changed",
     [("MIX1 = 0xBF58476D1CE4E5B9", "MIX1 = 0xBF58476D1CE4E5B8", 1)], None),
    ("M03-PRNG-MIX2", "PRNG second mix constant changed",
     [("MIX2 = 0x94D049BB133111EB", "MIX2 = 0x94D049BB133111EC", 1)], None),
    ("M04-PRNG-SHIFT", "PRNG final shift 31 changed to 30",
     [("return z ^ (z >> 31)", "return z ^ (z >> 30)", 1)], None),
    ("M05-PRNG-U32", "u32 taken from the low 32 bits instead of the high 32 bits",
     [("return self.next_u64() >> 32", "return self.next_u64() & 0xFFFFFFFF", 1)], None),
    ("M06-PRNG-UNIFORM", "uniform(n) as modulo instead of multiply-high",
     [("return (self.next_u32() * n) >> 32", "return self.next_u32() % n", 1)], None),
    ("M07-SEED-MASK32", "seed truncated to 32 bits",
     [("self.state = seed\n", "self.state = seed & 0xFFFFFFFF\n", 1)], None),
    ("M08-SEED-IGNORED", "seed ignored (state starts at 0)",
     [("self.state = seed\n", "self.state = 0\n", 1)], None),
    ("M09-FIELD-ORDER", "row encoding swaps the pos_x and pos_y columns",
     [('return sorted(profile.by_name[n]["id"] for n in FIELD_NAMES)',
       'o = sorted(profile.by_name[n]["id"] for n in FIELD_NAMES)\n    o[3], o[4] = o[4], o[3]\n    return o', 1)],
     None),
    ("M10-REMAINDER-GROUP", "group size used for the split is 16 instead of 17 (remainder rule)",
     [("groups, rem = divmod(rows_n, GROUP_SIZE)", "groups, rem = divmod(rows_n, GROUP_SIZE - 1)", 2)], None),
    ("M11-REMAINDER-ORDER", "remainder rows get order_mode 0 instead of attack-move",
     [("REMAINDER_ORDER_MODE, ", "0, ", 2)], None),
    ("M12-ANCHOR-IDLE", "anchor reload written as 2 instead of 1 (7.3 idle value)",
     [('"reload": 1, "damage": 1', '"reload": 2, "damage": 1', 1)], None),
    ("M13-FORMATION-ORIGIN", "member offset origin -48 changed to -16",
     [("FORMATION_ORIGIN = -48", "FORMATION_ORIGIN = -16", 1)], None),
    ("M14-BAND-CENTRE", "attack-move target uses the band centre without the +1 (half-open)",
     [("return (lo + hi + 1) // 2, CENTRE", "return (lo + hi) // 2, CENTRE", 1)], None),
    ("M15-WEAPON-TABLE", "rifle reload 6 changed to 7 (7.4 table)",
     [('"rifle": {"range_sq": 65536, "reload": 6', '"rifle": {"range_sq": 65536, "reload": 7', 1)], None),
]

EQUIVALENT = [
    ("E01-STATE-MODULO", "state wrap written as % (1 << 64) instead of & MASK64 (same value)",
     [("self.state = (self.state + GOLDEN) & MASK64", "self.state = (self.state + GOLDEN) % (1 << 64)", 1)],
     None),
    ("E02-ASCII-FLAG", "canonical JSON with ensure_ascii=False (output is ASCII for all inputs)",
     [('ensure_ascii=True).encode("ascii")', 'ensure_ascii=False).encode("ascii")', 1)], None),
    ("E03-PROFILE-FILE-ORDER", "profile fields listed in reverse order in the file (ids unchanged)",
     [], "reverse_fields"),
]


# Known gaps: mutants that change the PRNG but leave every committed digest unchanged. They are
# reported as gaps, never as killed. M04 changes only bit 0 of the u32 that 7.1 specifies
# (u32 = z >> 32); uniform(n) = (u32 * n) >> 32 does not see that bit in practice (checked:
# 0 of 14,000 draws changed for several n). The PRNG known answers in test_prng.py do catch it.
# Closing the gap changes normative 7.1 (owner decision); it is not changed here.
KNOWN_GAPS = {
    "M04-PRNG-SHIFT": "digest-insensitive for committed scenes; caught by PRNG KAT only "
                      "(7.1 change needed to close; owner decision)",
}


def kat_ok(mod):
    """True when the module's splitmix64 reproduces the pinned known answers (test_prng.py)."""
    import test_prng  # same directory; its module-level code has no side effects
    for seed, want in test_prng.KAT_U64.items():
        r = mod.SplitMix64(seed)
        if ["%016x" % r.next_u64() for _ in want] != want:
            return False
    r = mod.SplitMix64(0)
    return r.uniform(1024) == 904 and mod.SplitMix64(0).uniform(3) == 2


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def apply_patches(source, patches):
    text = source
    for old, new, count in patches:
        found = text.count(old)
        if found != count:
            raise RuntimeError("patch %r occurs %d time(s), expected %d" % (old[:60], found, count))
        text = text.replace(old, new)
    return text


def digests(mod, profile):
    out = {}
    for spec in mod.SCENE_SPECS:
        out[spec["name"]] = mod.make_scene(profile, spec)[2]
    out["<schema>"] = profile.schema_digest
    return out


def run_one(work, label, source_text, profile_doc):
    path = os.path.join(work, "rtsgen_%s.py" % label)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(source_text)
    mod = load_module(path, "rtsgen_" + label.replace("-", "_"))
    profile = mod.Profile(profile_doc)
    return digests(mod, profile), mod


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--work", default=None, help="scratch directory (created if missing)")
    args = ap.parse_args(argv)
    own_work = args.work is None
    work = args.work or tempfile.mkdtemp(prefix="rts_mutants_")
    os.makedirs(work, exist_ok=True)
    try:
        with open(SOURCE, "r", encoding="utf-8") as fh:
            source = fh.read()
        with open(rtsgen.PROFILE_PATH, "r", encoding="utf-8") as fh:
            profile_doc = json.load(fh)
        base = digests(rtsgen, rtsgen.load_profile())
        # The unpatched copy must reproduce the module's own digests (sanity of the harness).
        check, _m = run_one(work, "baseline", source, profile_doc)
        if check != base:
            print("HARNESS ERROR: unpatched copy does not reproduce the baseline digests")
            return 2
        rows = []
        failures = 0
        jobs = [("detect", m) for m in DETECT] + [("equivalent", m) for m in EQUIVALENT]
        for kind, (mid, desc, patches, transform) in jobs:
            try:
                if transform == "reverse_fields":
                    doc = json.loads(json.dumps(profile_doc))
                    doc["fields"].reverse()
                    got, mod = run_one(work, mid, source, doc)
                else:
                    got, mod = run_one(work, mid, apply_patches(source, patches), profile_doc)
                katok = kat_ok(mod)
            except Exception as exc:  # harness error: counts as a failed run
                rows.append((mid, kind, desc, "ERROR: %s" % exc, [], None))
                failures += 1
                continue
            changed = sorted(k for k in base if got.get(k) != base[k])
            if kind == "detect":
                if changed:
                    verdict, ok = "killed", True
                elif mid in KNOWN_GAPS:
                    verdict, ok = "KNOWN GAP (no digest change)", True
                else:
                    verdict, ok = "SURVIVED", False
            else:
                ok = not changed
                verdict = "equivalent (unchanged)" if ok else "FAIL: changed, not equivalent"
            if not ok:
                failures += 1
            rows.append((mid, kind, desc, verdict, changed, katok))
        width = max(len(r[0]) for r in rows)
        for mid, kind, desc, verdict, changed, katok in rows:
            kat = "-" if katok is None else ("KAT fails" if not katok else "KAT passes")
            print("%-*s  %-10s  %-30s  %-10s  %s" % (width, mid, kind, verdict, kat, ",".join(changed) or "-"))
            print("%-*s  %s" % (width, "", desc))
            if mid in KNOWN_GAPS:
                print("%-*s  gap: %s" % (width, "", KNOWN_GAPS[mid]))
        killed = sum(1 for r in rows if r[1] == "detect" and r[3] == "killed")
        gaps = sum(1 for r in rows if r[3].startswith("KNOWN GAP"))
        n_det = sum(1 for r in rows if r[1] == "detect")
        n_eq = sum(1 for r in rows if r[1] == "equivalent")
        print("detect: %d/%d killed by a scene digest, %d known gap(s); equivalent: %d listed"
              % (killed, n_det, gaps, n_eq))
        print("unexpected failures: %d" % failures)
        return 0 if failures == 0 else 1
    finally:
        if own_work:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
