#!/usr/bin/env python3
"""Generate capsule copies bound to the negative-case sources in tests/fixtures (T04).

capsule.submit takes only capsule_path. The engine binds a capsule to the source named by its
own `source` field, and su_path_sibling (src/agent/su_path.c) accepts only a bare file name
that sits beside the capsule. So a negative case whose source is not its capsule's own source
can be sent only through a capsule copy placed in the source's directory.

For each such pair under tests/fixtures/ this writes `bound_<name>.capsule.json` beside the
source, with `source` set to the bare file name, and records the copy in
tests/fixtures/invalid/bound_manifest.json. Every other field is copied unchanged, so the
rejection the engine returns is still the one the case expects.

Sources outside tests/fixtures (examples/invalid/*) are not touched: they stay not expressible
and T04 reports them unavailable.

Run from the repository root:  python3 -I tests/harness/make_negative_bindings.py
"""
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MANIFESTS = ["tests/fixtures/negative_cases.json", "examples/negative_cases.json"]
MAP_REL = "tests/fixtures/invalid/bound_manifest.json"


def norm(rel):
    return os.path.normpath(rel).replace(os.sep, "/")


def main() -> int:
    os.chdir(REPO)
    mapping, written, skipped = {}, [], []
    for manifest_rel in MANIFESTS:
        with open(manifest_rel, encoding="utf-8") as fh:
            doc = json.load(fh)
        base = os.path.dirname(manifest_rel)
        for case in doc["cases"]:
            cap_rel = norm(os.path.join(base, case.get("capsule", doc["capsule"])))
            src_rel = norm(os.path.join(base, case["path"]))
            with open(cap_rel, encoding="utf-8") as fh:
                cap = json.load(fh)
            if norm(os.path.join(os.path.dirname(cap_rel), cap["source"])) == src_rel:
                continue  # already bound by its own capsule
            if not src_rel.startswith("tests/fixtures/"):
                skipped.append(src_rel)  # outside this harness's write scope
                continue
            src_dir, src_name = os.path.split(src_rel)
            out_rel = norm(os.path.join(src_dir, "bound_" + src_name[: -len(".lir.json")] + ".capsule.json"))
            cap["source"] = src_name
            with open(os.path.join(REPO, out_rel), "w", encoding="utf-8") as fh:
                json.dump(cap, fh, indent=2)
                fh.write("\n")
            mapping[src_rel] = out_rel
            written.append(out_rel)
    with open(os.path.join(REPO, MAP_REL), "w", encoding="utf-8") as fh:
        json.dump(dict(sorted(mapping.items())), fh, indent=2)
        fh.write("\n")
    print(f"wrote {len(written)} bound capsules; {len(skipped)} sources outside tests/fixtures left unbound")
    return 0


if __name__ == "__main__":
    sys.exit(main())
