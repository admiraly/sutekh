#!/usr/bin/env python3
"""Static check for M2-01 (N2): every allocation in the migration engine is an injection point.

The allocation-failure sweeps in test_migrate and test_migrate_runtime count only the points the engine passes. An
allocation made with a raw allocator is never counted, so it is never tested. This check fails when su_migrate.c calls
any allocator directly (malloc, calloc, realloc, reallocarray, aligned_alloc, posix_memalign, memalign, valloc,
pvalloc, strdup, strndup, asprintf, vasprintf) anywhere except the three wrapper bodies (mig_malloc, mig_calloc,
mig_realloc), each of which checks the injection point before it allocates.

Usage: check_alloc_points.py <path to su_migrate.c>
       check_alloc_points.py --self-test
Exit status 0 when every raw allocator call is one of the wrapper bodies, 1 otherwise.
"""
import re
import sys

RAW_ALLOCATORS = (
    "malloc", "calloc", "realloc", "reallocarray", "aligned_alloc", "posix_memalign", "memalign", "valloc",
    "pvalloc", "strdup", "strndup", "asprintf", "vasprintf",
)

WRAPPER_BODIES = (
    "return inject_point(g_alloc_hooks) ? NULL : malloc(n);",
    "return inject_point(g_alloc_hooks) ? NULL : calloc(n, m);",
    "return inject_point(g_alloc_hooks) ? NULL : realloc(p, n);",
)

RAW = re.compile(r"(?<![A-Za-z0-9_])(" + "|".join(RAW_ALLOCATORS) + r")\s*\(")
WRAPPED = re.compile(r"(?<![A-Za-z0-9_])mig_(malloc|calloc|realloc)\s*\(")


def check_lines(name, lines):
    """Returns (status, messages): status 0 when clean, 1 on a raw allocator call or a missing wrapper body."""
    sites = []
    for number, line in enumerate(lines, 1):
        if RAW.search(line):
            sites.append((number, line.strip()))
    bodies_found = {text for _, text in sites if text in WRAPPER_BODIES}
    unwrapped = [(n, t) for n, t in sites if t not in WRAPPER_BODIES]
    missing = [text for text in WRAPPER_BODIES if text not in bodies_found]
    messages = []
    for number, text in unwrapped:
        messages.append(f"{name}:{number}: raw allocator call outside the counted wrappers: {text}")
    for text in missing:
        messages.append(f"wrapper body not found (the seam is missing): {text}")
    if unwrapped or missing:
        return 1, messages
    uses = sum(1 for line in lines if WRAPPED.search(line) and not line.lstrip().startswith("static "))
    return 0, [f"check_alloc_points: {uses} wrapped allocation sites, 0 raw"]


def self_test():
    """Synthetic inputs: every raw allocator name must be caught; the clean input and a missing body are judged."""
    clean = list(WRAPPER_BODIES) + ["    c = mig_calloc(1u, sizeof *c);", "    p = mig_realloc(p, n);", "    free(p);"]
    fails = 0
    if check_lines("clean", clean)[0] != 0:
        print("self-test: clean input rejected", file=sys.stderr)
        fails += 1
    for name in RAW_ALLOCATORS:
        if check_lines("synthetic", clean + [f"    x = {name}(16u, 32u);"])[0] != 1:
            print(f"self-test: raw {name} not caught", file=sys.stderr)
            fails += 1
    if check_lines("missing", clean[1:])[0] != 1:
        print("self-test: a missing wrapper body not caught", file=sys.stderr)
        fails += 1
    print(f"check_alloc_points self-test: {len(RAW_ALLOCATORS)} allocator names, {fails} failures")
    return 0 if fails == 0 else 1


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) != 2:
        print("usage: check_alloc_points.py <su_migrate.c> | --self-test", file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        lines = f.read().splitlines()
    status, messages = check_lines(argv[1], lines)
    stream = sys.stderr if status != 0 else sys.stdout
    for m in messages:
        print(m, file=stream)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
