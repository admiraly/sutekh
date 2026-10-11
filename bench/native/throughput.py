#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M1 isolated native throughput: reference VM vs scalar native vs AVX2 native (stdlib only).

Builds bench/native/su_native_throughput.c WITHOUT CMake, against the static libraries of an
existing native build tree (configure it first, outside the repository):

  cmake -S . -B <scratch>/gcc-Release -G Ninja -DSUTEKH_NATIVE=ON -DCMAKE_BUILD_TYPE=Release
  cmake --build <scratch>/gcc-Release
  python3 -I bench/native/throughput.py --build-dir <scratch>/gcc-Release --scratch <scratch>/tp

Compile line (C17, same flags CMake used for the test objects in the native tree):
  gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror -pedantic -DASMJIT_STATIC -DASMJIT_NO_AARCH64
      -DASMJIT_NO_UJIT -I<repo> -I<repo>/src -I<repo>/include -c bench/native/su_native_throughput.c
Link line (the same archives and order CMake used for test_native_avx2):
  g++ <obj> -o su_native_throughput libsu_native.a libsu_vm.a libsu_world.a libsu_ir.a libsu_asmjit.a

The C driver verifies every engine against the reference VM (exact output equality) before timing.
This script refuses to report if any verification or timing step fails.

Timed quantities (monotonic clock, one process):
  * vm_wrapper, scalar_wrapper, avx2_wrapper: the full checked call (su_vm_invoke_v1 or
    su_native_run_checked, including wrapper checks).
  * scalar_kernel, avx2_kernel: the kernel function pointer call only. The reference VM has no
    separate kernel, so no vm kernel figure exists (null).
Each sample is the total time of K back-to-back invocations divided by K. K is 1 for 1,000,000
rows; for 1,024 rows K is calibrated so a sample lasts at least about 2 ms.

Derived quantities (stated per row and per byte):
  ns/row            = median sample (ns per invocation) / rows
  GB/s-equivalent   = rows * bytes_per_row / median sample (ns); bytes/ns = GB/s (10^9 bytes/s).
                      bytes_per_row is the nominal element bytes of the bound columns
                      (3 u32 columns = 12 bytes per row for both programs). It is not a measured
                      memory bandwidth and ignores caches.
  p95               = nearest-rank on the 21 sorted samples.

The CPU frequency is not pinned or read as a controlled value. This is an observation on a shared
VM. It is not qualification (ENGINE_CONSTITUTION.md C10).

Output: planning/evidence/m1/native-throughput.json (unless --out is given).
"""

import argparse
import datetime
import hashlib
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SOURCE = os.path.join("bench", "native", "su_native_throughput.c")
LIBS = ["libsu_native.a", "libsu_vm.a", "libsu_world.a", "libsu_ir.a", "libsu_asmjit.a"]
HASHED = [
    "bench/native/su_native_throughput.c",
    "backends/native/su_native.c",
    "backends/native/su_native_asmjit.cpp",
    "backends/native/avx2/su_native_avx2.cpp",
    "backends/native/avx2/su_native_caps.c",
    "src/vm/su_vm.c",
    "include/sutekh/native/abi.h",
    "include/sutekh/abi.h",
]
EVIDENCE = os.path.join(REPO, "planning", "evidence", "m1", "native-throughput.json")


class Fail(Exception):
    pass


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(argv, cwd=REPO, timeout=600):
    r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, shell=False, timeout=timeout, check=False)
    return r


def first_line(argv):
    try:
        r = run(argv, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    out = (r.stdout or r.stderr).strip().splitlines()
    return out[0] if out else None


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return None


def cpu_info():
    text = read_text("/proc/cpuinfo") or ""
    model = None
    for line in text.splitlines():
        if line.startswith("model name"):
            model = line.split(":", 1)[1].strip()
            break
    flags = set()
    for line in text.splitlines():
        if line.startswith("flags"):
            flags = set(line.split(":", 1)[1].split())
            break
    gov = read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    cur = read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")
    return {
        "model_name": model,
        "logical_cpus": os.cpu_count(),
        "avx2_flag": "avx2" in flags,
        "avx512f_flag": "avx512f" in flags,
        "scaling_governor": gov.strip() if gov else None,
        "scaling_cur_freq_khz": int(cur.strip()) if cur and cur.strip().isdigit() else None,
        "frequency_pinning": "not pinned; no frequency control; the shared VM host frequency is unknown",
    }


def mem_total_kib():
    text = read_text("/proc/meminfo") or ""
    m = re.search(r"^MemTotal:\s+(\d+)\s+kB", text, re.M)
    return int(m.group(1)) if m else None


def container_hint():
    cg = read_text("/proc/1/cgroup") or ""
    if os.path.exists("/.dockerenv"):
        return True, "/.dockerenv present"
    if re.search(r"docker|kubepods|containerd|lxc", cg):
        return True, "cgroup of PID 1 names a container runtime"
    return False, "no container marker found (/.dockerenv absent; PID 1 cgroup has no container runtime)"


def git_state():
    head = run(["git", "rev-parse", "HEAD"]).stdout.strip()
    dirty = run(["git", "status", "--porcelain", "--untracked-files=no"]).stdout.strip() != ""
    return head, dirty


def percentile_nearest_rank(sorted_vals, p):
    n = len(sorted_vals)
    return sorted_vals[max(1, math.ceil(p / 100.0 * n)) - 1]


def stats(values):
    s = sorted(values)
    return {
        "n": len(s), "min": s[0], "median": statistics.median(s), "p95": percentile_nearest_rank(s, 95),
        "max": s[-1], "percentile_method": "nearest-rank",
    }


def build(args, scratch):
    build_dir = os.path.abspath(args.build_dir)
    libs = []
    for name in LIBS:
        p = os.path.join(build_dir, name)
        if not os.path.isfile(p):
            raise Fail(f"missing library {p}: configure and build the native tree first (see module docstring)")
        libs.append(p)
    os.makedirs(scratch, exist_ok=True)
    obj = os.path.join(scratch, "su_native_throughput.o")
    exe = os.path.join(scratch, "su_native_throughput")
    cc = [args.cc, "-std=c17", "-O3", "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-pedantic",
          "-DASMJIT_STATIC", "-DASMJIT_NO_AARCH64", "-DASMJIT_NO_UJIT",
          f"-I{REPO}", f"-I{os.path.join(REPO, 'src')}", f"-I{os.path.join(REPO, 'include')}",
          "-c", os.path.join(REPO, SOURCE), "-o", obj]
    ld = [args.cxx, obj, "-o", exe] + libs
    r1 = run(cc)
    if r1.returncode != 0:
        raise Fail("compile failed: " + (r1.stderr or r1.stdout)[-2000:])
    r2 = run(ld)
    if r2.returncode != 0:
        raise Fail("link failed: " + (r2.stderr or r2.stdout)[-2000:])
    return exe, cc, ld, {name: sha256_file(p) for name, p in zip(LIBS, libs)}


def parse(text):
    host = {}
    cfgs = {}
    verify = []
    samples = []
    result = None
    for line in text.splitlines():
        f = line.split("\t")
        if f[0] == "HOST":
            host["avx2"] = f[2] == "1"
        elif f[0] == "ASMJIT":
            host["asmjit_commit"] = f[1]
        elif f[0] == "CONFIG":
            key = (f[1], f[2], int(f[3]))
            ks = {}
            for item in f[5:]:
                name, k = item.split(":")
                ks[name] = int(k)
            cfgs[key] = {"bytes_per_row": int(f[4]), "k": ks}
        elif f[0] == "VERIFY":
            verify.append({"program": f[1], "alignment": f[2], "rows": int(f[3]), "slot": f[4], "result": f[5]})
        elif f[0] == "SAMPLE":
            samples.append({"program": f[1], "alignment": f[2], "rows": int(f[3]), "slot": f[4], "K": int(f[5]),
                            "index": int(f[6]), "total_ns": int(f[7])})
        elif f[0] == "RESULT":
            result = f[1]
    return host, cfgs, verify, samples, result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True, help="native CMake build tree (SUTEKH_NATIVE=ON) outside the repo")
    ap.add_argument("--scratch", required=True, help="scratch directory outside the repository")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--out", default=EVIDENCE)
    args = ap.parse_args(argv)

    scratch = os.path.abspath(args.scratch)
    if os.path.commonpath([scratch, REPO]) == REPO:
        raise SystemExit("--scratch must be outside the repository")
    head, dirty = git_state()
    if dirty:
        raise SystemExit("tracked files are modified; commit the sources first so the recorded tree is clean")

    exe, cc, ld, lib_hashes = build(args, scratch)
    r = run([exe], timeout=3600)
    raw_path = os.path.join(scratch, "su_native_throughput.out")
    with open(raw_path, "w", encoding="utf-8") as fh:
        fh.write(r.stdout)
    host, cfgs, verify, samples, result = parse(r.stdout)
    if r.returncode != 0 or result != "pass":
        raise Fail(f"benchmark driver rc={r.returncode} result={result}: {r.stderr[-2000:]}")

    # group samples: (program, alignment, rows, slot) -> per-invocation ns
    series = {}
    for s in samples:
        key = (s["program"], s["alignment"], s["rows"], s["slot"])
        series.setdefault(key, []).append(s["total_ns"] / s["K"])
    results = []
    for (prog, align, rows, slot), vals in sorted(series.items()):
        cfg = cfgs[(prog, align, rows)]
        med = statistics.median(vals)
        bpr = cfg["bytes_per_row"]
        results.append({
            "program": prog, "alignment": align, "rows": rows, "slot": slot,
            "scope": "kernel call only (no wrapper checks)" if slot.endswith("_kernel")
            else "full checked wrapper call",
            "K_invocations_per_sample": cfg["k"][slot], "unit": "ns per invocation",
            "samples_ns": [round(v, 3) for v in vals], "stats_ns": {k: (round(v, 3) if isinstance(v, float) else v)
                                                                    for k, v in stats(vals).items()},
            "ns_per_row_median": round(med / rows, 6),
            "GBps_equiv_median": round(bpr * rows / med, 4),
            "bytes_per_row": bpr,
        })
    # kernel-only and wrapper medians for comparison; the VM has no kernel slot
    by = {(r_["program"], r_["alignment"], r_["rows"], r_["slot"]): r_ for r_ in results}
    ratios = []
    for (prog, align, rows, slot), r_ in sorted(by.items()):
        if slot == "vm_wrapper":
            continue
        base = by[(prog, align, rows, "vm_wrapper")]["stats_ns"]["median"]
        ratios.append({"program": prog, "alignment": align, "rows": rows, "slot": slot,
                       "median_speedup_vs_vm_wrapper": round(base / r_["stats_ns"]["median"], 3)})

    head2, dirty2 = git_state()
    tracked = {p: sha256_file(os.path.join(REPO, p)) for p in HASHED}
    ccv = first_line([args.cc, "--version"])
    cxxv = first_line([args.cxx, "--version"])
    cmv = first_line(["cmake", "--version"])
    nv = first_line(["ninja", "--version"])
    doc = {
        "report_format": "0.1",
        "kind": "native_throughput_observation",
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "claim_scope": ("Observation on a shared 4-vCPU VM (Xeon @ 2.10 GHz per /proc/cpuinfo). Not a "
                        "qualification, not a gate, not a fine-grained hardware decision (ENGINE_CONSTITUTION C10)."),
        "git": {"head": head2, "dirty_tracked_files": dirty2, "head_at_start": head, "dirty_at_start": dirty},
        "machine": {
            "cpu": cpu_info(), "mem_total_kib": mem_total_kib(), "kernel": platform.release(),
            "os": platform.platform(), "container": dict(zip(("is_container", "reason"), container_hint())),
        },
        "toolchain": {"cc": args.cc, "cc_version": ccv, "cxx": args.cxx, "cxx_version": cxxv,
                      "cmake": cmv, "ninja": nv, "build_type": "Release (-O3 -DNDEBUG)",
                      "asmjit_commit": host.get("asmjit_commit"), "avx2_reported_by_host_caps": host.get("avx2")},
        "build": {
            "build_dir": "outside repository (path not recorded; see library hashes)",
            "compile_command": cc, "link_command": ld,
            "library_sha256": lib_hashes,
            "source_sha256": tracked,
        },
        "method": {
            "programs": {
                "movement": "examples/movement.lir.json shape: load p, load v, add, store p (2 loads, 1 add, 1 store; "
                            "u32, 2 read columns, 1 write column, 12 bytes/row). The brief said '3 loads/adds/store'; "
                            "this is the shipped movement example and was used instead.",
                "arith20": "20 instructions: 2 loads, 17 add/mul/sub/lt/eq/select ops, 1 store; 19 registers; "
                           "2 read columns, 1 write column; 12 bytes/row.",
            },
            "alignment": {"aligned": "all bound columns start on a 64-byte boundary",
                          "unaligned1": "all bound columns start 4 bytes (one u32 element) past a 64-byte boundary"},
            "rows": [1000000, 1024],
            "samples_per_series": 21, "warm_up": "3 invocations per slot before sampling; K calibrated per slot",
            "order": "interleaved; the start slot rotates per sample index",
            "verification": "before timing, every slot's outputs are compared byte-for-byte with the reference VM "
                            "output (all statuses SU_OK); the outputs are compared again after timing",
            "statistics": "min, median, p95 (nearest rank), max over per-invocation samples",
            "formulas": {"ns_per_row": "median ns per invocation / rows",
                         "GBps_equiv": "rows * bytes_per_row / median ns (= 10^9 bytes/s)"},
            "clock": "CLOCK_MONOTONIC",
        },
        "driver": {"returncode": r.returncode, "result_line": result, "raw_output_scratch_only": True,
                   "verifications": verify},
        "results": results,
        "median_speedups": ratios,
        "not_done": [
            "no pinned frequency, no isolated core, no background-load control, no PMU counters",
            "no Windows measurement (Win64 native path not run)",
            "no AVX-512 measurement; the host reports AVX-512F but the backend does not use it",
            "one run only; no repetition across reboots or hosts",
            "1-element-unaligned is the only misalignment tested",
        ],
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, sort_keys=False)
        fh.write("\n")
    for r_ in results:
        print(f"{r_['program']:8s} {r_['alignment']:10s} {r_['rows']:>8d} {r_['slot']:15s} "
              f"median {r_['stats_ns']['median']:14.1f} ns  ns/row {r_['ns_per_row_median']:.4f}  "
              f"GB/s~ {r_['GBps_equiv_median']:.2f}  n={r_['stats_ns']['n']}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as exc:
        print("FAILED:", exc, file=sys.stderr)
        sys.exit(1)
