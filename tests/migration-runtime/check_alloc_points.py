#!/usr/bin/env python3
"""Static check for M2-01 fixes2 (N2): every allocation in the migration engine is an injection point.

The allocation-failure sweep in test_migrate_runtime counts only the points the engine passes. An allocation made with
the raw allocator would never be counted, so it would never be tested. This check fails when src/world/migration/
su_migrate.c calls malloc, calloc or realloc directly anywhere except the three wrapper bodies (mig_malloc,
mig_calloc, mig_realloc), each of which checks the injection point before it allocates.

Usage: check_alloc_points.py <path to su_migrate.c>
Exit status 0 when every raw allocator call is one of the wrapper bodies, 1 otherwise.
"""
import re
import sys

WRAPPER_BODIES = {
    "return inject_point(g_alloc_hooks) ? NULL : malloc(n);",
    "return inject_point(g_alloc_hooks) ? NULL : calloc(n, m);",
    "return inject_point(g_alloc_hooks) ? NULL : realloc(p, n);",
}


def main(argv):
    if len(argv) != 2:
        print("usage: check_alloc_points.py <su_migrate.c>", file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        lines = f.read().splitlines()
    raw = re.compile(r"(?<![A-Za-z0-9_])(malloc|calloc|realloc)\s*\(")
    sites = []
    for number, line in enumerate(lines, 1):
        if raw.search(line):
            sites.append((number, line.strip()))
    bodies_found = {text for _, text in sites if text in WRAPPER_BODIES}
    unwrapped = [(n, t) for n, t in sites if t not in WRAPPER_BODIES]
    missing = sorted(WRAPPER_BODIES - bodies_found)
    for number, text in unwrapped:
        print(f"{argv[1]}:{number}: raw allocator call outside the counted wrappers: {text}", file=sys.stderr)
    for text in missing:
        print(f"wrapper body not found (the seam is missing): {text}", file=sys.stderr)
    if unwrapped or missing:
        return 1
    wrapped = re.compile(r"(?<![A-Za-z0-9_])mig_(malloc|calloc|realloc)\s*\(")
    uses = sum(1 for line in lines if wrapped.search(line) and not line.lstrip().startswith("static "))
    print(f"check_alloc_points: {uses} wrapped allocation sites, 0 raw")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
