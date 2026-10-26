#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Configure, build and test the minimal add_subdirectory consumer in tests/consumer/ against a Sutekh checkout.

Checks (each prints PASS or FAIL; the exit status is 1 when any check fails):
  defaults      in the consumer's cache SUTEKH_BUILD_TESTS, _BENCH, _EXAMPLES and SUTEKH_WERROR are OFF
  ctest-list    ctest lists exactly the consumer's tests (consumer_host, consumer_public_headers)
  no-engine-tests  the build produced none of Sutekh's test, bench or example programs
  internal-headers each probe that includes an engine-internal header (src/, src/platform/,
                profiles/space3d/native/) fails to compile because the header is not found
  system-headers   (GCC and Clang) consumer sources see the Sutekh tree only as -isystem <sutekh>/include (and
                -isystem for fetched third_party/ headers such as Vulkan's), and the
                same public-headers compile with -I instead of -isystem fails under -Wpadded -Werror (so the
                SYSTEM property is what keeps the consumer's warnings off engine headers)
  run           ctest passes: Sutekh::host links and runs (world, 120 ticks, rollback digest check) and every public
                header compiles under the consumer's -Wall -Wextra -Wpedantic -Werror
  tree-unchanged   no file in the Sutekh checkout was created, modified or removed (build*/, out/, .venv/, .cache/
                and .git are not compared)

usage: check_consumer.py [--sutekh DIR] [--build DIR] [--cmake CMAKE] [--config Debug] [-- extra cmake args]
The build directory must be outside the Sutekh checkout (default: a new temporary directory, removed on success).
Stdlib only; every subprocess gets an argument list.
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED_TESTS = ["consumer_host", "consumer_public_headers"]
PROBES = {
    "src": "world/su_world.h",
    "src_platform": "su_platform.h",
    "space3d": "space3d_geom.h",
}
# Programs that only Sutekh's tests, bench or examples build (a consumer build must produce none of them).
ENGINE_ONLY_PROGRAMS = ["test_world", "test_host", "worker_stub", "recovery_worker", "bench_host", "bench_audio_system",
                        "racer_microcosm", "minimal_host", "arena_host", "space_demo", "test_audio"]
SKIP_TOP = ("build", "out", ".venv", ".cache", ".git")

failures = []


def report(name, ok, detail=""):
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (": " + detail) if detail else ""))
    if not ok:
        failures.append(name)


def run(args, cwd=None):
    res = subprocess.run(args, cwd=cwd, shell=False, capture_output=True, text=True, check=False)
    return res.returncode, res.stdout + res.stderr


def tree_state(root):
    state = {}
    for top in os.listdir(root):
        if top.startswith(SKIP_TOP):
            continue
        path = os.path.join(root, top)
        if os.path.isfile(path) or os.path.islink(path):
            st = os.lstat(path)
            state[top] = (st.st_size, st.st_mtime_ns)
            continue
        for dirpath, _dirs, files in os.walk(path):
            for f in files:
                p = os.path.join(dirpath, f)
                st = os.lstat(p)
                state[os.path.relpath(p, root)] = (st.st_size, st.st_mtime_ns)
    return state


def inside(path, root):
    path, root = os.path.realpath(path), os.path.realpath(root)
    return path == root or path.startswith(root + os.sep)


def include_flags(tokens):
    """Yields (flag, path) for -I, -isystem and MSVC /I, -external:I include options."""
    i = 0
    while i < len(tokens):
        t = tokens[i]
        for flag in ("-isystem", "-I", "/I", "-external:I", "/external:I"):
            if t == flag and i + 1 < len(tokens):
                yield flag, tokens[i + 1]
                i += 1
                break
            if t.startswith(flag) and len(t) > len(flag) and not (flag == "-I" and t.startswith("-isystem")):
                yield flag, t[len(flag):]
                break
        i += 1


def main(argv):
    if "--" in argv:
        split = argv.index("--")
        argv, extra = argv[:split], argv[split + 1:]
    else:
        extra = []
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sutekh", default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument("--build", default=None)
    ap.add_argument("--cmake", default="cmake")
    ap.add_argument("--config", default="Debug")
    ap.add_argument("--keep", action="store_true", help="keep a temporary build directory")
    args = ap.parse_args(argv)
    sutekh = os.path.realpath(args.sutekh)
    cmake = args.cmake
    ctest = os.path.join(os.path.dirname(cmake), "ctest") if os.path.dirname(cmake) else "ctest"
    build = args.build or tempfile.mkdtemp(prefix="sutekh-consumer-")
    if inside(build, sutekh):
        print("FAIL: the build directory %s is inside the Sutekh checkout" % build)
        return 1
    os.makedirs(build, exist_ok=True)
    print("sutekh %s\nbuild  %s\ncmake  %s" % (sutekh, build, run([cmake, "--version"])[1].splitlines()[0]))

    before = tree_state(sutekh)
    rc, out = run([cmake, "-S", HERE, "-B", build, "-DSUTEKH_SOURCE_DIR=" + sutekh, "-DCMAKE_BUILD_TYPE=" + args.config,
                   "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *extra])
    if rc != 0:
        print(out)
        report("configure", False)
        return 1
    report("configure", True)
    for line in out.splitlines():
        if "Sutekh" in line:
            print("  " + line.strip())

    facts = dict(line.split("=", 1) for line in open(os.path.join(build, "consumer_facts.txt"), encoding="utf-8")
                 .read().splitlines() if "=" in line)
    print("  public targets: " + facts.get("sutekh_public_targets", "").replace(";", " "))
    defaults = {k: facts.get(k) for k in ("sutekh_build_tests", "sutekh_build_bench", "sutekh_build_examples",
                                          "sutekh_werror")}
    report("defaults", all(v == "OFF" for v in defaults.values()), " ".join("%s=%s" % kv for kv in defaults.items()))

    rc, out = run([cmake, "--build", build, "--config", args.config, "--parallel", str(min(os.cpu_count() or 2, 6))])
    if rc != 0:
        print(out[-6000:])
    report("build", rc == 0)  # the remaining checks still run and report

    rc, out = run([ctest, "--test-dir", build, "-C", args.config, "--show-only=json-v1"])
    names = sorted(t["name"] for t in json.loads(out)["tests"]) if rc == 0 else []
    report("ctest-list", names == sorted(EXPECTED_TESTS), "listed %s" % (names if rc == 0 else out.strip()))

    sutekh_build = os.path.join(build, "sutekh")
    present = [p for p in ENGINE_ONLY_PROGRAMS
               if any(os.path.exists(os.path.join(sutekh_build, p + ext)) for ext in ("", ".exe"))]
    report("no-engine-tests", not present, "built: %s" % present if present else "")

    for probe, header in PROBES.items():
        rc, out = run([cmake, "--build", build, "--config", args.config, "--target", "consumer_probe_" + probe])
        missing = header in out and any(s in out for s in ("No such file", "not found", "cannot open", "Cannot open"))
        report("internal-headers[%s]" % probe, rc != 0 and missing,
               "compiled" if rc == 0 else ("" if missing else "failed for another reason:\n" + out[-3000:]))

    compiler = facts.get("c_compiler_id", "")
    db = os.path.join(build, "compile_commands.json")
    if compiler in ("GNU", "Clang", "AppleClang") and os.path.isfile(db):
        entries = {os.path.basename(e["file"]): e for e in json.load(open(db, encoding="utf-8"))}
        bad = []
        for src in ("consumer_host.c", "consumer_public_headers.c"):
            e = entries[src]
            tokens = shlex.split(e["command"]) if "command" in e else e["arguments"]
            seen = [(f, os.path.realpath(os.path.join(e["directory"], p))) for f, p in include_flags(tokens)]
            for f, p in seen:
                # Allowed from the Sutekh tree: include/ and the fetched third-party headers, both as -isystem.
                public = p == os.path.join(sutekh, "include") or inside(p, os.path.join(sutekh, "third_party"))
                if inside(p, sutekh) and (f != "-isystem" or not public):
                    bad.append("%s: %s %s" % (src, f, p))
            if ("-isystem", os.path.join(sutekh, "include")) not in seen:
                bad.append("%s: no -isystem %s/include" % (src, sutekh))
        report("system-headers", not bad, "; ".join(bad))
        # Non-vacuous: the same compile with the engine headers as -I must trip -Wpadded -Werror.
        e = entries["consumer_public_headers.c"]
        tokens = shlex.split(e["command"]) if "command" in e else list(e["arguments"])
        tokens = ["-I" if t == "-isystem" else t for t in tokens]
        obj = os.path.join(build, "consumer_public_headers_nonsystem.o")
        if "-o" in tokens:
            tokens[tokens.index("-o") + 1] = obj
        rc, out = run(tokens, cwd=e["directory"])
        report("system-headers[control]", rc != 0 and "padding" in out,
               "with -I the strict compile %s" % ("fails on engine headers (expected)" if rc != 0 else "passed"))
    else:
        print("SKIP system-headers: compiler %s (checked for GCC and Clang only)" % compiler)

    rc, out = run([ctest, "--test-dir", build, "-C", args.config, "--output-on-failure", "-V"])
    for line in out.splitlines():
        if "host api" in line or "tests passed" in line or "Failed" in line:
            print("  " + line.split(": ", 1)[-1].strip())
    report("run", rc == 0, "" if rc == 0 else out[-3000:])

    after = tree_state(sutekh)
    changed = sorted(set(before) ^ set(after) | {k for k in before if k in after and before[k] != after[k]})
    report("tree-unchanged", not changed, "changed: %s" % changed[:20] if changed else "")

    if failures:
        print("FAILED: %s (build tree kept at %s)" % (", ".join(failures), build))
        return 1
    print("consumer checks passed")
    if args.build is None and not args.keep:
        shutil.rmtree(build, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
