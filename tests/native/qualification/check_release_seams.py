#!/usr/bin/env python3
"""Fails when a test-only fault seam is present in a production binary (M1-02c-1, C07).

The seams live under SU_TEST_FAULT_HOOKS (src/runtime/worker.c, src/runtime/native_versions.c). Their
environment names and mode names are string literals there, so they appear in a binary only when the
seam code was compiled in. Usage: check_release_seams.py <binary>. Exit 0: none found; 1: found.
"""
import sys

SEAM_STRINGS = [
    b"SU_TEST_FAULT",          # environment selectors (also matches SU_TEST_FAULT_MS)
    b"SU_TEST_FAULT_NATIVE_TICK",     # M1-02c-2: installed-kernel crash seam (tick)
    b"SU_TEST_FAULT_NATIVE_BACKEND",  # M1-02c-2: installed-kernel crash seam (backend)
    b"perturb_recompile",      # active-worker install seam
    b"wrong_kernel",           # disposable admission seam
    b"wrong_world",            # disposable own-case seam
]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_release_seams.py <binary>", file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    found = [s.decode() for s in SEAM_STRINGS if s in data]
    if found:
        print("test-only fault seam strings present in %s: %s" % (sys.argv[1], ", ".join(found)))
        return 1
    print("no test-only fault seam strings in %s" % sys.argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
