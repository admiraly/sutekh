#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Run the M1-05 platform lanes that this host can execute and record every check's status.

Statuses (kept distinct on purpose):
  passed       the check ran and its own criterion held (exit code and output parsed)
  failed       the check ran and the criterion did not hold, or a required build step failed
  not_run      the lane is possible here but this check was not executed (reason given)
  unavailable  the lane or tool cannot exist on this host, or the platform is not supported

Lanes:
  linux-native   Linux x86-64 host: configure with SUTEKH_NATIVE=ON, run the M0 C unit tests and the
                 native ABI probe (SysV). Runs on Linux only.
  win64-cross    Linux host with x86_64-w64-mingw32 GCC: cross-compile the M0 core, the native
                 backend and the Win64 probe; run them under wine when wine is installed. wine is a
                 compatibility layer, so a wine result is labelled as wine evidence, never Windows.
  win64-native   Real Windows host. Not executed by this runner: always not_run here, because no
                 Windows host is available in this environment. Never inferred from wine or mingw.
  macos          unavailable: not a supported platform.
  arm64          unavailable: no ARM64 native backend (AsmJit ARM support is compiled out).

Output: planning/evidence/m1/platform-linux.json, platform-windows-cross.json and
platform-matrix.json (by default), plus logs in the work directory (build-platform/, ignored by git).
Exit status: 0 when no check failed, 1 when at least one failed, 2 on usage errors.
Stdlib only. Every subprocess is an argument vector (shell=False).

Usage: python3 -I tests/platform/run_platform.py [--lanes linux-native,win64-cross] [--skip-full-graph]
"""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import capabilities as caps  # noqa: E402  (same directory; stdlib only)

SCHEMA = "sutekh.platform_run.v1"
LANES = ("linux-native", "win64-cross", "win64-native", "macos", "arm64")
PASSED, FAILED, NOT_RUN, UNAVAILABLE = "passed", "failed", "not_run", "unavailable"
UNIT_TESTS = ("test_ir", "test_world", "test_vm", "test_snapshot")
NATIVE_LIBS = ("su_native", "su_asmjit", "su_ir")
PROBE_SRC = os.path.join(REPO_ROOT, "tests", "platform", "native_abi_probe.c")
TOOLCHAIN = os.path.join(REPO_ROOT, "tests", "platform", "toolchains", "mingw-w64-x86_64.cmake")
MINGW_CC = "x86_64-w64-mingw32-gcc"
MINGW_CXX = "x86_64-w64-mingw32-g++"
STRICT = ["-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic"]


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path):
    if not path or not os.path.isfile(path):
        return None
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rel(path):
    """Repository-relative spelling for evidence, absolute if outside the repository."""
    try:
        r = os.path.relpath(path, REPO_ROOT)
    except ValueError:
        return path
    return path if r.startswith("..") else r


class Runner:
    def __init__(self, work_dir, evidence_dir, wine_path):
        self.work_dir = work_dir
        self.evidence_dir = evidence_dir
        self.wine = wine_path
        self.records = []
        self.commands = []
        self.lane_meta = {}
        os.makedirs(self.work_dir, exist_ok=True)
        self.env = dict(os.environ)
        self.env["WINEDEBUG"] = "-all"
        self.env["WINEPREFIX"] = os.path.join(self.work_dir, "wineprefix")
        self.env.setdefault("LC_ALL", "C")

    def record(self, lane, check_id, status, reason=None, **extra):
        rec = {"lane": lane, "id": check_id, "status": status, "reason": reason}
        rec.update(extra)
        self.records.append(rec)
        print(f"[{lane}] {check_id}: {status}" + (f" - {reason}" if reason else ""), flush=True)
        return rec

    def run(self, lane, check_id, argv, cwd=REPO_ROOT, timeout=900, env=None, log_name=None):
        """Run one argument vector, keep its log in the work directory, and record the command."""
        log_path = os.path.join(self.work_dir, log_name or (check_id.replace(".", "_") + ".log"))
        start = time.monotonic()
        try:
            with open(log_path, "w", encoding="utf-8") as log:
                proc = subprocess.run(argv, cwd=cwd, env=env or self.env, stdin=subprocess.DEVNULL,
                                      stdout=log, stderr=subprocess.STDOUT, timeout=timeout,
                                      check=False)
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            rc = None
        except OSError as exc:
            rc = None
            with open(log_path, "a", encoding="utf-8") as log:
                log.write(f"\nOSError: {exc}\n")
        duration = round(time.monotonic() - start, 3)
        with open(log_path, "r", encoding="utf-8", errors="replace") as log:
            text = log.read()
        entry = {"check": check_id, "argv": [rel(a) if os.path.isabs(a) else a for a in argv],
                 "cwd": rel(cwd), "returncode": rc, "duration_s": duration,
                 "log": rel(log_path), "log_sha256": sha256_file(log_path)}
        self.commands.append(entry)
        return rc, text

    def meta(self, lane, key, value):
        self.lane_meta.setdefault(lane, {})[key] = value

    def write_lane(self, lane, filename, host, tools, inputs, artifacts):
        out = {
            "schema": SCHEMA,
            "lane": lane,
            "generated_utc": utc_now(),
            "host": host,
            "tools": tools,
            "inputs_sha256": inputs,
            "artifacts_sha256": artifacts,
            "checks": [r for r in self.records if r["lane"] == lane],
            "commands": [c for c in self.commands if c["check"].startswith(lane.split("-")[0])],
            "notes": self.lane_meta.get(lane, {}),
        }
        os.makedirs(self.evidence_dir, exist_ok=True)
        path = os.path.join(self.evidence_dir, filename)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print(f"wrote {rel(path)}", flush=True)
        return path


def tool_version(report, name):
    for group in ("compilers", "cross_compilers", "build_tools"):
        entry = report.get(group, {}).get(name)
        if entry and entry.get("found"):
            return {"path": entry["path"], "version": entry["version"]}
    return None


def configure_args(build_dir, extra):
    generator = "Ninja" if caps.find_tool("ninja") else "Unix Makefiles"
    return ["cmake", "-S", REPO_ROOT, "-B", build_dir, "-G", generator, "-DCMAKE_BUILD_TYPE=Release",
            *extra]


def parse_probe(text):
    """Return (checks, failures, host_abi) from the probe's summary lines, or None."""
    m = re.search(r"native_abi_probe: (\d+) checks, (\d+) failures", text)
    h = re.search(r"host_abi=(\w+) host_caps=0x([0-9a-f]+) backends=(\d+)", text)
    if not m:
        return None
    return {"checks": int(m.group(1)), "failures": int(m.group(2)),
            "host_abi": h.group(1) if h else None,
            "backends": int(h.group(3)) if h else None,
            "host_caps": int(h.group(2), 16) if h else None}


def failed_objects(text):
    return sorted({line.split("FAILED: ", 1)[1].strip() for line in text.splitlines()
                   if line.startswith("FAILED: ")})


# ---------------------------------------------------------------- linux-native

def lane_linux_native(R, report):
    lane = "linux-native"
    if platform.system() != "Linux":
        for cid in ("linux.configure", "linux.unit_tests", "linux.native_abi_probe"):
            R.record(lane, cid, UNAVAILABLE, f"host is {platform.system()}, not Linux")
        return
    cc = tool_version(report, "gcc") or tool_version(report, "cc")
    cxx = tool_version(report, "g++")
    cmake = caps.find_tool("cmake")
    if not (cc and cxx and cmake):
        R.record(lane, "linux.configure", UNAVAILABLE, "gcc, g++ or cmake not found")
        return
    build_dir = os.path.join(R.work_dir, "linux-native")
    native = report["asmjit_checkout_present"]
    extra = ["-DSUTEKH_NATIVE=ON"] if native else []
    rc, _ = R.run(lane, "linux.configure", configure_args(build_dir, extra), timeout=600)
    if rc != 0:
        R.record(lane, "linux.configure", FAILED, f"cmake configure returned {rc}")
        return
    R.record(lane, "linux.configure", PASSED, "native backend " + ("enabled" if native else
             "not configured: third_party/asmjit missing (run tools/bootstrap_asmjit.py)"))
    targets = list(UNIT_TESTS) + (list(NATIVE_LIBS) if native else [])
    rc, _ = R.run(lane, "linux.build", [cmake, "--build", build_dir, "--target", *targets], timeout=1800)
    if rc != 0:
        R.record(lane, "linux.build", FAILED, f"build of {' '.join(targets)} returned {rc}")
        return
    R.record(lane, "linux.build", PASSED, f"built {' '.join(targets)}")
    for test in UNIT_TESTS:
        exe = os.path.join(build_dir, test)
        rc, text = R.run(lane, f"linux.unit.{test}", [exe], timeout=600)
        if rc == 0:
            R.record(lane, f"linux.unit.{test}", PASSED, "exit 0", returncode=rc)
        else:
            R.record(lane, f"linux.unit.{test}", FAILED, f"exit {rc}", returncode=rc)
    if not native:
        R.record(lane, "linux.native_abi_probe", UNAVAILABLE, "AsmJit checkout missing; probe needs su_native")
        return
    probe_dir = os.path.join(build_dir, "probe")
    os.makedirs(probe_dir, exist_ok=True)
    probe_exe = os.path.join(probe_dir, "native_abi_probe")
    libs = [os.path.join(build_dir, "lib" + n + ".a") for n in NATIVE_LIBS]
    rc, text = R.run(lane, "linux.native_abi_probe.compile",
                     [caps.find_tool("gcc") or "gcc", *STRICT, "-pedantic", "-O2", "-I", REPO_ROOT,
                      "-I", os.path.join(REPO_ROOT, "include"), "-I", os.path.join(REPO_ROOT, "src"),
                      "-o", probe_exe, PROBE_SRC, *libs, "-lstdc++", "-lm"], timeout=600)
    if rc != 0:
        R.record(lane, "linux.native_abi_probe", FAILED, f"probe did not compile (exit {rc})")
        return
    rc, text = R.run(lane, "linux.native_abi_probe", [probe_exe], timeout=900)
    summary = parse_probe(text)
    if rc == 0 and summary and summary["failures"] == 0 and summary["host_abi"] == "sysv":
        R.record(lane, "linux.native_abi_probe", PASSED,
                 f"{summary['checks']} checks, 0 failures, SysV, backends={summary['backends']}")
        R.meta(lane, "native_abi_probe", summary)
    else:
        R.record(lane, "linux.native_abi_probe", FAILED, f"exit {rc}; summary {summary}")


# ---------------------------------------------------------------- win64-cross

def win64_native_not_run(R):
    lane = "win64-native"
    reason = ("no Windows host in this environment (host OS: " + platform.system() + "); "
              "Windows-native execution is not inferred from wine or the mingw cross build")
    if platform.system() == "Windows":
        reason = ("this runner has no Windows-native lane implementation; no Windows-native "
                  "evidence was collected by run_platform.py")
    for cid in ("win64native.build", "win64native.unit_tests", "win64native.native_abi_probe",
                "win64native.sutekh_version"):
        R.record(lane, cid, NOT_RUN, reason)


def lane_win64_cross(R, report, skip_full_graph):
    lane = "win64-cross"
    cc = find_mingw(MINGW_CC)
    cxx = find_mingw(MINGW_CXX)
    cmake = caps.find_tool("cmake")
    if not (cc and cxx and cmake):
        R.record(lane, "win64cross.toolchain", UNAVAILABLE,
                 f"{MINGW_CC}, {MINGW_CXX} or cmake not found (apt install mingw-w64)")
        return
    wine = R.wine
    wine_reason = None if wine else "wine64 not installed; Windows binaries were not executed"
    build_dir = os.path.join(R.work_dir, "win64-cross")
    native = report["asmjit_checkout_present"]
    extra = ["-DSUTEKH_NATIVE=ON"] if native else []
    rc, _ = R.run(lane, "win64cross.configure", [cmake, "-S", REPO_ROOT, "-B", build_dir, "-G",
                  "Ninja" if caps.find_tool("ninja") else "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Release",
                  "-DCMAKE_TOOLCHAIN_FILE=" + TOOLCHAIN, *extra], timeout=600)
    if rc != 0:
        R.record(lane, "win64cross.configure", FAILED, f"cmake configure returned {rc}")
        return
    R.record(lane, "win64cross.configure", PASSED, "toolchain " + rel(TOOLCHAIN))

    core = ["su_platform", "su_ir", "su_world", "su_vm", "su_snapshot", *UNIT_TESTS]
    rc, _ = R.run(lane, "win64cross.build_core", [cmake, "--build", build_dir, "--target", *core], timeout=1800)
    R.record(lane, "win64cross.build_core", PASSED if rc == 0 else FAILED,
             "platform layer, su_ir, su_world, su_vm, su_snapshot and the four M0 unit tests"
             if rc == 0 else f"build returned {rc}")
    for test in UNIT_TESTS:
        exe = os.path.join(build_dir, test + ".exe")
        cid = f"win64cross.unit.{test}"
        if rc != 0 or not os.path.isfile(exe):
            R.record(lane, cid, NOT_RUN, "not built")
        elif not wine:
            R.record(lane, cid, NOT_RUN, wine_reason)
        else:
            rc2, _ = R.run(lane, cid, [wine, exe], timeout=600)
            R.record(lane, cid, PASSED if rc2 == 0 else FAILED,
                     "run under wine, exit 0" if rc2 == 0 else f"wine run returned {rc2}", returncode=rc2)

    if not native:
        R.record(lane, "win64cross.build_native", UNAVAILABLE,
                 "AsmJit checkout missing; run tools/bootstrap_asmjit.py")
        R.record(lane, "win64cross.native_abi_probe", UNAVAILABLE, "needs the native backend")
    else:
        rc, _ = R.run(lane, "win64cross.build_native", [cmake, "--build", build_dir, "--target", *NATIVE_LIBS],
                      timeout=1800)
        R.record(lane, "win64cross.build_native", PASSED if rc == 0 else FAILED,
                 "su_native (Win64 prologue, VirtualAlloc/VirtualProtect), su_asmjit, su_ir"
                 if rc == 0 else f"build returned {rc}")
        if rc == 0:
            probe_dir = os.path.join(build_dir, "probe")
            os.makedirs(probe_dir, exist_ok=True)
            obj = os.path.join(probe_dir, "native_abi_probe.o")
            exe = os.path.join(probe_dir, "native_abi_probe.exe")
            libs = [os.path.join(build_dir, "lib" + n + ".a") for n in NATIVE_LIBS]
            rc2, _ = R.run(lane, "win64cross.native_abi_probe.compile",
                           [cc, *STRICT, "-O2", "-I", REPO_ROOT, "-I", os.path.join(REPO_ROOT, "include"),
                            "-I", os.path.join(REPO_ROOT, "src"), "-c", "-o", obj, PROBE_SRC], timeout=600)
            rc3 = None
            if rc2 == 0:
                rc3, _ = R.run(lane, "win64cross.native_abi_probe.link",
                               [cxx, "-static", "-o", exe, obj, *libs], timeout=600)
            if rc2 != 0 or rc3 != 0:
                R.record(lane, "win64cross.native_abi_probe", FAILED, "probe did not compile or link")
            elif not wine:
                R.record(lane, "win64cross.native_abi_probe", NOT_RUN, wine_reason)
            else:
                rc4, text = R.run(lane, "win64cross.native_abi_probe", [wine, exe], timeout=900)
                summary = parse_probe(text)
                if rc4 == 0 and summary and summary["failures"] == 0 and summary["host_abi"] == "win64":
                    R.record(lane, "win64cross.native_abi_probe", PASSED,
                             f"run under wine: {summary['checks']} checks, 0 failures, Win64 ABI, "
                             f"backends={summary['backends']}")
                    R.meta(lane, "native_abi_probe", summary)
                else:
                    R.record(lane, "win64cross.native_abi_probe", FAILED,
                             f"wine exit {rc4}; summary {summary}")

    # sutekh executable on Windows: only the platform layer is linked (CMake also links su_agent,
    # which is POSIX-only and does not build for Windows).
    defs = os.path.join(R.work_dir, "sutekh_defs.h")
    with open(defs, "w", encoding="utf-8") as fh:
        fh.write('#define SUTEKH_VERSION_STRING "0.1.0"\n#define SUTEKH_BUILD_TYPE "Release"\n'
                 '#define SUTEKH_C_COMPILER_ID "GNU"\n#define SUTEKH_C_COMPILER_VERSION "13.0.0"\n')
    sut = os.path.join(R.work_dir, "sutekh-win64.exe")
    rc, _ = R.run(lane, "win64cross.sutekh_platform_main",
                  [cc, *STRICT, "-O2", "-I", REPO_ROOT, "-I", os.path.join(REPO_ROOT, "include"),
                   "-I", os.path.join(REPO_ROOT, "src"), "-I", os.path.join(REPO_ROOT, "src", "platform"),
                   "-include", defs, "-o", sut, os.path.join(REPO_ROOT, "src", "platform", "sutekh_main.c"),
                   os.path.join(REPO_ROOT, "src", "platform", "su_platform.c")], timeout=600)
    if rc != 0:
        R.record(lane, "win64cross.sutekh_platform_main", FAILED, f"compile returned {rc}")
    elif not wine:
        R.record(lane, "win64cross.sutekh_platform_main", NOT_RUN, wine_reason)
    else:
        rc1, text1 = R.run(lane, "win64cross.sutekh_version", [wine, sut, "--version"], timeout=120)
        rc2, text2 = R.run(lane, "win64cross.sutekh_serve_unsupported", [wine, sut, "serve"], timeout=120)
        ok1 = rc1 == 0 and "sutekh 0.1.0" in text1
        ok2 = rc2 == 1 and "SU_E_UNSUPPORTED_METHOD" in text2
        R.record(lane, "win64cross.sutekh_version", PASSED if ok1 else FAILED,
                 "--version under wine exit 0" if ok1 else f"exit {rc1}")
        R.record(lane, "win64cross.sutekh_serve_unsupported", PASSED if ok2 else FAILED,
                 "serve returns SU_E_UNSUPPORTED_METHOD (exit 1) on Windows" if ok2 else f"exit {rc2}")

    if skip_full_graph:
        R.record(lane, "win64cross.full_graph", NOT_RUN, "skipped by --skip-full-graph")
    else:
        rc, text = R.run(lane, "win64cross.full_graph", [cmake, "--build", build_dir, "--", "-k", "0"],
                         timeout=2400)
        if rc == 0:
            R.record(lane, "win64cross.full_graph", PASSED, "every target in the graph built for Windows")
        else:
            failed = failed_objects(text)
            R.record(lane, "win64cross.full_graph", FAILED,
                     f"{len(failed)} object(s) fail for Windows; POSIX-only sources (see details)",
                     failed_objects=failed)


def find_mingw(name):
    return caps.find_tool(name)


# ---------------------------------------------------------------- unsupported lanes

def lane_macos(R):
    R.record("macos", "macos.build", UNAVAILABLE,
             "macOS is not a supported platform: no Mach-O code-publication path, not built or tested")


def lane_arm64(R, report):
    machine = report["host"]["machine"]
    R.record("arm64", "arm64.native_backend", UNAVAILABLE,
             "no ARM64 native backend; AsmJit ARM support is compiled out (ASMJIT_NO_AARCH64); "
             f"this host is {machine}")


# ---------------------------------------------------------------- main

def tools_for(report, names):
    return {n: tool_version(report, n) for n in names}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--lanes", default=",".join(LANES), help="comma-separated subset of " + ",".join(LANES))
    parser.add_argument("--work-dir", default=os.path.join(REPO_ROOT, "build-platform"))
    parser.add_argument("--evidence-dir", default=os.path.join(REPO_ROOT, "planning", "evidence", "m1"))
    parser.add_argument("--skip-full-graph", action="store_true",
                        help="do not build the whole win64-cross graph (the POSIX-only inventory)")
    args = parser.parse_args(argv)
    lanes = [x.strip() for x in args.lanes.split(",") if x.strip()]
    bad = [x for x in lanes if x not in LANES]
    if bad:
        print("unknown lane(s): " + ", ".join(bad), file=sys.stderr)
        return 2

    report = caps.build_report()
    wine = caps.find_tool("wine64") or caps.find_tool("wine")
    R = Runner(os.path.abspath(args.work_dir), os.path.abspath(args.evidence_dir), wine)
    R.meta("host", "capabilities", {"system": report["host"]["system"], "machine": report["host"]["machine"],
                                    "wine": wine, "asmjit_checkout_present": report["asmjit_checkout_present"]})

    if "linux-native" in lanes:
        lane_linux_native(R, report)
    if "win64-cross" in lanes:
        lane_win64_cross(R, report, args.skip_full_graph)
    if "win64-native" in lanes:
        win64_native_not_run(R)
    if "macos" in lanes:
        lane_macos(R)
    if "arm64" in lanes:
        lane_arm64(R, report)

    host = report["host"]
    cross_tools = tools_for(report, ["x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++", "cmake", "ninja"])
    cross_tools["wine"] = {"path": wine, "version": (caps.tool_entry("wine64").get("version")
                                                       if wine else None)}
    cross_inputs = {rel(p): sha256_file(p) for p in (TOOLCHAIN, PROBE_SRC,
                    os.path.join(REPO_ROOT, "tests", "platform", "run_platform.py"),
                    os.path.join(REPO_ROOT, "tests", "platform", "capabilities.py"))}
    linux_inputs = {rel(p): sha256_file(p) for p in (PROBE_SRC,
                    os.path.join(REPO_ROOT, "tests", "platform", "cpu_state.c"),
                    os.path.join(REPO_ROOT, "tests", "platform", "run_platform.py"),
                    os.path.join(REPO_ROOT, "tests", "platform", "capabilities.py"))}
    linux_tools = tools_for(report, ["gcc", "g++", "cmake", "ninja"])

    if "linux-native" in lanes:
        probe_bin = os.path.join(R.work_dir, "linux-native", "probe", "native_abi_probe")
        R.write_lane("linux-native", "platform-linux.json", host, linux_tools, linux_inputs,
                     {"native_abi_probe_sysv": sha256_file(probe_bin)})
    if "win64-cross" in lanes:
        art = {}
        build_dir = os.path.join(R.work_dir, "win64-cross")
        for n in NATIVE_LIBS:
            art["build-platform/win64-cross/lib" + n + ".a"] = sha256_file(os.path.join(build_dir, "lib" + n + ".a"))
        for n in UNIT_TESTS:
            art["build-platform/win64-cross/" + n + ".exe"] = sha256_file(os.path.join(build_dir, n + ".exe"))
        art["build-platform/win64-cross/probe/native_abi_probe.exe"] = sha256_file(
            os.path.join(build_dir, "probe", "native_abi_probe.exe"))
        art["build-platform/sutekh-win64.exe"] = sha256_file(os.path.join(R.work_dir, "sutekh-win64.exe"))
        R.write_lane("win64-cross", "platform-windows-cross.json", host, cross_tools, cross_inputs, art)
    matrix = {
        "schema": SCHEMA,
        "generated_utc": utc_now(),
        "host": host,
        "lanes": list(lanes),
        "counts": {s: sum(1 for r in R.records if r["status"] == s)
                   for s in (PASSED, FAILED, NOT_RUN, UNAVAILABLE)},
        "checks": R.records,
    }
    os.makedirs(R.evidence_dir, exist_ok=True)
    with open(os.path.join(R.evidence_dir, "platform-matrix.json"), "w", encoding="utf-8") as fh:
        json.dump(matrix, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("counts: " + json.dumps(matrix["counts"], sort_keys=True), flush=True)
    return 1 if matrix["counts"][FAILED] else 0


if __name__ == "__main__":
    sys.exit(main())
