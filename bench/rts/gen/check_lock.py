#!/usr/bin/env python3
"""Lock check for bench/rts/scenes.lock.json (M3-01, contract 7.5).

Regenerates every committed scene with rtsgen.py and compares generator id, params, seed,
capacity, row count, scene digest and schema digest with the lock. Exit 0 only when every
entry matches and every row passes the 2.2 / 7.3 checks. Python standard library only.

Run: python3 -I bench/rts/gen/check_lock.py [--lock PATH] [--profile PATH]
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rtsgen  # noqa: E402


def check_lock(lock, profile):
    """Returns a list of problem strings (empty when the lock matches the generator)."""
    problems = []
    if lock.get("format") != rtsgen.LOCK_FORMAT:
        problems.append("lock format is not %s" % rtsgen.LOCK_FORMAT)
    if lock.get("numeric_profile") != rtsgen.NUMERIC_PROFILE:
        problems.append("numeric_profile is not %s" % rtsgen.NUMERIC_PROFILE)
    if lock.get("prng") != rtsgen.PRNG_NAME:
        problems.append("prng is not %s" % rtsgen.PRNG_NAME)
    if lock.get("canon_format") != rtsgen.CANON_FORMAT:
        problems.append("canon_format is not %s" % rtsgen.CANON_FORMAT)
    want_schema = "sha256:" + profile.schema_digest
    if lock.get("schema_digest") != want_schema:
        problems.append("schema_digest %s != %s" % (lock.get("schema_digest"), want_schema))
    entries = lock.get("scenes")
    if not isinstance(entries, list):
        return problems + ["scenes is not a list"]
    by_name = {}
    for e in entries:
        name = e.get("name")
        if name in by_name:
            problems.append("duplicate scene %r" % name)
        by_name[name] = e
    want_names = [s["name"] for s in rtsgen.SCENE_SPECS]
    if sorted(by_name) != sorted(want_names):
        problems.append("scene set %s != committed specs %s" % (sorted(by_name), sorted(want_names)))
    for spec in rtsgen.SCENE_SPECS:
        e = by_name.get(spec["name"])
        if e is None:
            continue
        arr, params, digest = rtsgen.make_scene(profile, spec)
        if e.get("generator") != spec["generator"]:
            problems.append("%s: generator id differs" % spec["name"])
        if e.get("seed") != spec["seed"]:
            problems.append("%s: seed differs" % spec["name"])
        if e.get("capacity") != spec["rows"] or e.get("rows") != spec["rows"]:
            problems.append("%s: capacity/rows differ from the generator" % spec["name"])
        if e.get("params") != json.loads(rtsgen.canon_json(params)):
            problems.append("%s: params differ from the generator" % spec["name"])
        if e.get("scene_digest") != "sha256:" + digest:
            problems.append("%s: scene digest %s != regenerated sha256:%s"
                            % (spec["name"], e.get("scene_digest"), digest))
        if e.get("schema_digest") != want_schema:
            problems.append("%s: schema digest differs" % spec["name"])
        try:
            rtsgen.validate_scene(profile, arr, capacity=spec["rows"])
        except ValueError as exc:
            problems.append("%s: domain check: %s" % (spec["name"], exc))
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lock", default=rtsgen.LOCK_PATH)
    ap.add_argument("--profile", default=rtsgen.PROFILE_PATH)
    args = ap.parse_args(argv)
    profile = rtsgen.load_profile(args.profile)
    with open(args.lock, "r", encoding="utf-8") as fh:
        lock = json.load(fh)
    problems = check_lock(lock, profile)
    if problems:
        for p in problems:
            print("FAIL " + p)
        print("lock check FAILED (%d problem(s))" % len(problems))
        return 1
    print("lock check passed: %d scene(s), schema sha256:%s" % (len(lock["scenes"]), profile.schema_digest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
