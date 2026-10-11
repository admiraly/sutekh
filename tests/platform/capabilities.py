#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Structured report of what this host can do for Sutekh platform checks (M1-05).

Prints one JSON object on stdout. It only describes the host: nothing here is a test result.
Each tool is found on PATH (or at a known install path) and its version is read with a bounded,
argument-list subprocess call (shell=False). Missing tools are reported as missing, with a reason.

Usage: python3 -I tests/platform/capabilities.py [--pretty]
Stdlib only.
"""

import argparse
import datetime
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
SCHEMA = "sutekh.platform_capabilities.v1"
TIMEOUT_S = 60

# Tool name -> extra fallback paths tried when the name is not on PATH.
TOOL_FALLBACKS = {
    "wine64": ["/usr/lib/wine/wine64"],
    "wine": ["/usr/bin/wine"],
}


def find_tool(name):
    """Return the absolute path of an executable, or None."""
    found = shutil.which(name)
    if found:
        return found
    for path in TOOL_FALLBACKS.get(name, []):
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def run_bounded(argv, env=None, timeout=TIMEOUT_S):
    """Run an argument vector without a shell. Never raises for a failed command."""
    try:
        proc = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout, env=env, check=False)
        return {"returncode": proc.returncode,
                "stdout": proc.stdout.decode("utf-8", "replace"),
                "stderr": proc.stderr.decode("utf-8", "replace")}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"returncode": None, "stdout": "", "stderr": str(exc)}


def first_line(text):
    for line in text.splitlines():
        if line.strip():
            return line.strip()
    return ""


def tool_entry(name, version_args=("--version",)):
    path = find_tool(name)
    if path is None:
        return {"found": False, "path": None, "version": None, "reason": f"{name} not found on PATH"}
    res = run_bounded([path, *version_args])
    text = res["stdout"] or res["stderr"]
    return {"found": True, "path": path, "version": first_line(text) or "unknown",
            "version_returncode": res["returncode"]}


def compiler_inventory():
    names = ["cc", "gcc", "g++", "clang", "clang++"]
    out = {name: tool_entry(name) for name in names}
    out["cl"] = tool_entry("cl", version_args=())
    out["clang-cl"] = tool_entry("clang-cl")
    return out


def cross_inventory():
    names = ["x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++", "aarch64-linux-gnu-gcc"]
    return {name: tool_entry(name) for name in names}


def build_tools():
    return {name: tool_entry(name) for name in ("cmake", "ninja", "make", "python3")}


def read_cpu_flags():
    """Feature flags from /proc/cpuinfo (Linux only)."""
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("flags"):
                    flags = set(line.split(":", 1)[1].split())
                    return {"source": "/proc/cpuinfo",
                            "avx": "avx" in flags, "avx2": "avx2" in flags,
                            "avx512f": "avx512f" in flags, "xsave": "xsave" in flags,
                            "osxsave": "osxsave" in flags}
    except OSError as exc:
        return {"source": "/proc/cpuinfo", "error": str(exc)}
    return {"source": "/proc/cpuinfo", "error": "no flags line"}


def cpu_state_via_helper():
    """Compile tests/platform/cpu_state.c with the host C compiler and run it (CPUID + XGETBV)."""
    if platform.machine().lower() not in ("x86_64", "amd64"):
        return {"available": False, "reason": f"not an x86-64 host (machine={platform.machine()})"}
    cc = find_tool("cc") or find_tool("gcc") or find_tool("clang")
    if cc is None:
        return {"available": False, "reason": "no C compiler to build tests/platform/cpu_state.c"}
    with tempfile.TemporaryDirectory(prefix="sutekh-cpustate-") as tmp:
        exe = os.path.join(tmp, "cpu_state")
        src = os.path.join(HERE, "cpu_state.c")
        comp = run_bounded([cc, "-std=c99", "-O1", "-Wall", "-Werror", "-o", exe, src])
        if comp["returncode"] != 0:
            return {"available": False, "reason": "cpu_state.c did not compile: " + first_line(comp["stderr"])}
        res = run_bounded([exe])
        if res["returncode"] != 0:
            return {"available": False, "reason": f"cpu_state exited {res['returncode']}"}
        try:
            data = json.loads(first_line(res["stdout"]))
        except ValueError:
            return {"available": False, "reason": "cpu_state printed no JSON"}
        data["available"] = True
        data["compiler"] = cc
        return data


def ptrace_report():
    report = {}
    scope_path = "/proc/sys/kernel/yama/ptrace_scope"
    if os.path.exists(scope_path):
        try:
            with open(scope_path, "r", encoding="utf-8") as fh:
                report["yama_ptrace_scope"] = int(fh.read().strip())
        except (OSError, ValueError) as exc:
            report["yama_ptrace_scope"] = None
            report["yama_error"] = str(exc)
    else:
        report["yama_ptrace_scope"] = None
    strace = find_tool("strace")
    if strace is None:
        report["strace_attach"] = "unavailable"
        report["reason"] = "strace not installed"
        return report
    with tempfile.TemporaryDirectory(prefix="sutekh-ptrace-") as tmp:
        log = os.path.join(tmp, "log.txt")
        res = run_bounded([strace, "-f", "-o", log, "--", "/bin/true"])
    if res["returncode"] == 0:
        report["strace_attach"] = "available"
    else:
        report["strace_attach"] = "denied"
        report["reason"] = first_line(res["stderr"]) or f"strace exited {res['returncode']}"
    return report


def build_report():
    system = platform.system()
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    host = {
        "system": system,
        "release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "generated_utc": now,
    }
    report = {
        "schema": SCHEMA,
        "host": host,
        "is_windows_native": system == "Windows",
        "is_linux_native": system == "Linux",
        "compilers": compiler_inventory(),
        "cross_compilers": cross_inventory(),
        "build_tools": build_tools(),
        "wine": tool_entry("wine64") if find_tool("wine64") else tool_entry("wine"),
        "strace": tool_entry("strace"),
        "ptrace": ptrace_report(),
        "cpu": {"flags": read_cpu_flags() if system == "Linux" else {"source": None,
                                                                       "error": "not Linux"},
                "cpuid_xgetbv": cpu_state_via_helper()},
        "asmjit_checkout_present": os.path.isfile(
            os.path.join(REPO_ROOT, "third_party", "asmjit", "asmjit", "core", "code_holder.cpp")),
    }
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--pretty", action="store_true", help="indent the JSON output")
    args = parser.parse_args(argv)
    report = build_report()
    json.dump(report, sys.stdout, indent=2 if args.pretty else None, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
