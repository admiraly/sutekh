#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M1 native build latency with -DSUTEKH_NATIVE=ON (stdlib only).

Every sample uses a fresh build directory OUTSIDE the repository (--scratch). For each compiler
(gcc always; clang when clang and clang++ are on PATH) and build type (Release, Debug) it records,
for n samples each (default n = 7):

  cold  configure_s          cmake configure into an empty build directory
  cold  clean_build_s        cmake --build of the full tree, including the pinned AsmJit sources
                             (third_party/asmjit, core and x86 translation units)
  warm  noop_build_s         cmake --build again with nothing changed
  warm  incremental_s[file]  os.utime(mtime only, content unchanged) on each of five files, then
                             cmake --build; the other build products are reused

Compile and link steps are counted from `ninja -v` output (one "[k/N] command" line per executed
command): a command containing " -c " is a compile step, any other command is a link/archive step.
The "other" count covers commands that are neither (for example CMake regeneration).

The live gameplay edit path does not run any of these builds (ENGINE_CONSTITUTION C01); see
the note written to the output file, which is checked by grep over src/ at run time.

  python3 -I bench/native/build_latency.py --scratch <dir outside the repository> [--n 7]

Output: planning/evidence/m1/native-build-latency.json. Each value is a measurement of this run.
Percentiles are nearest-rank. Missing tools are recorded as null with a reason.
"""

import argparse
import datetime
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
EVIDENCE = os.path.join(REPO, "planning", "evidence", "m1", "native-build-latency.json")
TOUCH = [
    "backends/native/su_native_asmjit.cpp",
    "backends/native/su_native.c",
    "src/vm/su_vm.c",
    "include/sutekh/native/abi.h",
    "include/sutekh/abi.h",
]
STEP_RE = re.compile(r"^\[(\d+)/(\d+)\] (.*)$")
COMPILE_RE = re.compile(r" -c ")


class BuildError(Exception):
    pass


def run(argv, cwd=REPO, timeout=3600):
    return subprocess.run(argv, cwd=cwd, capture_output=True, text=True, shell=False, timeout=timeout, check=False)


def timed(argv):
    t0 = time.monotonic()
    r = run(argv)
    dt = time.monotonic() - t0
    if r.returncode != 0:
        raise BuildError(f"{' '.join(argv[:4])} ... rc={r.returncode}: {(r.stderr or r.stdout)[-1500:]}")
    return dt, r.stdout + r.stderr


def count_steps(text):
    compile_n = link_n = other_n = 0
    for line in text.splitlines():
        m = STEP_RE.match(line)
        if not m:
            continue
        cmd = m.group(3)
        if COMPILE_RE.search(cmd):
            compile_n += 1
        elif re.search(r"\bar qc\b| -o ", cmd):
            link_n += 1  # linker or archiver (ar qc ... && ranlib)
        elif "cmake" in cmd:
            other_n += 1  # cmake -P (glob verification), regeneration, cmake -E without an archive
        else:
            link_n += 1
    return {"compile_steps": compile_n, "link_steps": link_n, "other_steps": other_n}


def stats(values):
    if not values:
        return {"n": 0, "min": None, "median": None, "p95": None, "max": None, "percentile_method": "nearest-rank"}
    s = sorted(values)
    n = len(s)
    p95 = s[max(1, math.ceil(0.95 * n)) - 1]
    return {"n": n, "min": round(s[0], 4), "median": round(statistics.median(s), 4), "p95": round(p95, 4),
            "max": round(s[-1], 4), "percentile_method": "nearest-rank"}


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return None


def machine():
    cpu = None
    cpuinfo = read_text("/proc/cpuinfo") or ""
    for line in cpuinfo.splitlines():
        if line.startswith("model name"):
            cpu = line.split(":", 1)[1].strip()
            break
    mem = re.search(r"^MemTotal:\s+(\d+)\s+kB", read_text("/proc/meminfo") or "", re.M)
    cg = read_text("/proc/1/cgroup") or ""
    if os.path.exists("/.dockerenv"):
        is_c, why = True, "/.dockerenv present"
    elif re.search(r"docker|kubepods|containerd|lxc", cg):
        is_c, why = True, "PID 1 cgroup names a container runtime"
    else:
        is_c, why = False, "no container marker (no /.dockerenv; PID 1 cgroup has no container runtime)"

    def ver(argv):
        if shutil.which(argv[0]) is None:
            return None
        r = run(argv, timeout=60)
        out = (r.stdout or r.stderr).strip().splitlines()
        return out[0] if out else None

    return {
        "cpu_model": cpu, "logical_cpus": os.cpu_count(),
        "memory_total_kib": int(mem.group(1)) if mem else None,
        "kernel": platform.release(), "os": platform.platform(),
        "container": {"is_container": is_c, "reason": why},
        "versions": {"gcc": ver(["gcc", "--version"]), "g++": ver(["g++", "--version"]),
                     "clang": ver(["clang", "--version"]), "clang++": ver(["clang++", "--version"]),
                     "cmake": ver(["cmake", "--version"]), "ninja": ver(["ninja", "--version"])},
        "ninja_default_parallelism": (os.cpu_count() or 1) + 2,
        "python": platform.python_version(),
    }


def git_state():
    head = run(["git", "rev-parse", "HEAD"]).stdout.strip()
    dirty = run(["git", "status", "--porcelain", "--untracked-files=no"]).stdout.strip() != ""
    return head, dirty


def live_edit_grep():
    """Checks that src/ (the engine, which runs the live edit path) names no build tool or compiler."""
    r = run(["grep", "-rnE", r"\b(cmake|ninja|gcc|clang)\b|execv|posix_spawn|system\(", "src"], timeout=60)
    hits = [h for h in r.stdout.splitlines() if h.strip()]
    return {"pattern": r"\b(cmake|ninja|gcc|clang)\b|execv|posix_spawn|system\(", "scope": "src/",
            "hits": hits[:20], "hit_count": len(hits),
            "meaning": "hits listed for review; a build-tool spawn in the live edit path would appear here"}


def run_case(scratch, cname, cc, cxx, btype, n, log):
    bdir = os.path.join(scratch, f"{cname}-{btype}")
    cold = {"configure_s": [], "clean_build_s": [], "clean_steps": []}
    warm = {"noop_build_s": [], "noop_steps": [], "incremental": {f: {"s": [], "steps": []} for f in TOUCH}}
    for i in range(n):
        if os.path.isdir(bdir):
            shutil.rmtree(bdir)
        dt, _ = timed(["cmake", "-S", REPO, "-B", bdir, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={btype}",
                       "-DSUTEKH_NATIVE=ON", f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"])
        cold["configure_s"].append(dt)
        dt, out = timed(["cmake", "--build", bdir, "--", "-v"])
        cold["clean_build_s"].append(dt)
        cold["clean_steps"].append(count_steps(out))
        log(f"{cname} {btype} sample {i + 1}/{n}: configure+clean done")
        dt, out = timed(["cmake", "--build", bdir, "--", "-v"])
        warm["noop_build_s"].append(dt)
        warm["noop_steps"].append(count_steps(out))
        for f in TOUCH:
            path = os.path.join(REPO, f)
            os.utime(path, None)  # mtime only; content and git state are unchanged
            dt, out = timed(["cmake", "--build", bdir, "--", "-v"])
            warm["incremental"][f]["s"].append(dt)
            warm["incremental"][f]["steps"].append(count_steps(out))
    shutil.rmtree(bdir, ignore_errors=True)

    def summarize_steps(rows):
        keys = ("compile_steps", "link_steps", "other_steps")
        return {k: sorted({r[k] for r in rows}) for k in keys}

    return {
        "compiler": {"cc": cc, "cxx": cxx},
        "build_type": btype,
        "cold_first_build_with_asmjit": {
            "definition": "fresh build directory: configure, then full build compiling AsmJit core/x86/axl and all first-party targets",
            "configure_s": stats(cold["configure_s"]), "configure_samples_s": [round(x, 4) for x in cold["configure_s"]],
            "clean_full_build_s": stats(cold["clean_build_s"]),
            "clean_full_build_samples_s": [round(x, 4) for x in cold["clean_build_s"]],
            "steps_per_clean_build": summarize_steps(cold["clean_steps"]),
        },
        "warm_iteration_with_asmjit_already_built": {
            "definition": "same build directory as the cold build, no source change (noop) or mtime-only touch of one file",
            "noop_build_s": stats(warm["noop_build_s"]),
            "noop_build_samples_s": [round(x, 4) for x in warm["noop_build_s"]],
            "noop_steps": summarize_steps(warm["noop_steps"]),
            "incremental": {
                f: {"seconds": stats(v["s"]), "samples_s": [round(x, 4) for x in v["s"]],
                    "steps": summarize_steps(v["steps"])}
                for f, v in warm["incremental"].items()
            },
        },
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch", required=True, help="scratch directory outside the repository")
    ap.add_argument("--n", type=int, default=7, help="samples per case (>= 7)")
    ap.add_argument("--types", default="Release,Debug")
    ap.add_argument("--out", default=EVIDENCE)
    args = ap.parse_args(argv)
    if args.n < 7:
        raise SystemExit("n must be at least 7")
    scratch = os.path.abspath(args.scratch)
    if os.path.commonpath([scratch, REPO]) == REPO:
        raise SystemExit("--scratch must be outside the repository")
    os.makedirs(scratch, exist_ok=True)
    head, dirty = git_state()
    if dirty:
        raise SystemExit("tracked files are modified; commit the sources first")

    compilers = [("gcc", "gcc", "g++")]
    if shutil.which("clang") and shutil.which("clang++"):
        compilers.append(("clang", "clang", "clang++"))
    types = [t.strip() for t in args.types.split(",") if t.strip()]

    def log(msg):
        print(msg, flush=True)

    cases, failures = [], []
    started = time.monotonic()
    for cname, cc, cxx in compilers:
        for btype in types:
            try:
                cases.append({"name": f"{cname}-{btype}", **run_case(scratch, cname, cc, cxx, btype, args.n, log)})
            except BuildError as exc:
                failures.append({"case": f"{cname}-{btype}", "error": str(exc)})
                log(f"FAILED {cname}-{btype}: {exc}")
    head2, dirty2 = git_state()
    doc = {
        "report_format": "0.1",
        "kind": "native_build_latency",
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "git": {"head_at_start": head, "dirty_at_start": dirty, "head_at_end": head2, "dirty_at_end": dirty2,
                "note": "recorded tree is the commit above; sources were committed before this run; mtime-only "
                        "touches do not change git content"},
        "configuration": {"option": "-DSUTEKH_NATIVE=ON", "generator": "Ninja", "samples_per_case": args.n,
                          "build_dir": "fresh per sample, under --scratch (outside the repository; path not recorded)",
                          "touched_files_mtime_only": TOUCH,
                          "step_count_source": "ninja -v output of each cmake --build (one line per executed command)"},
        "machine": machine(),
        "cases": cases,
        "failures": failures,
        "cold_vs_warm": ("cold = fresh build directory (configure, then full build including AsmJit). "
                         "warm = same directory with AsmJit already built (no-op or mtime-only touch). "
                         "Warm numbers are not cold numbers and must not be reported as first-build cost."),
        "live_edit_path": {
            "claim": "The live gameplay edit path spawns none of these builds (ENGINE_CONSTITUTION C01). "
                     "Gameplay IR edits run without a native project build.",
            "source_scan": live_edit_grep(),
        },
        "not_measured": [
            "Windows native build (MSVC or clang-cl) was not run",
            "ccache/sccache not used; no caching-layer measurements",
            "scratch build directories were deleted after each sample",
            "time for the first-party build alone with AsmJit prebuilt is covered only by the incremental rows",
            "wall-clock includes the cmake and ninja process start; no CPU-time breakdown",
        ],
        "total_wall_s": round(time.monotonic() - started, 1),
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")
    log(f"wrote {args.out}; cases {len(cases)}; failures {len(failures)}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
