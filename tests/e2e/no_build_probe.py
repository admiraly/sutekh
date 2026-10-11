#!/usr/bin/env python3
"""T12 instrument: prove no compiler, assembler, linker or build tool is executed during a
capsule-only edit and install (stdlib only).

Run:  python3 tests/e2e/no_build_probe.py [--out PATH] [--demo-arg ARG ...]

Method (the only one used here; a fallback is not needed because strace is installed):
  * `strace -f -ttt -e trace=execve,execveat -s 4096` wraps the whole run of
    tools/demo_zero_wait.py. -f follows every fork and thread of the supervisor and its
    worker children; -ttt stamps each syscall with CLOCK_REALTIME epoch seconds, the same
    clock the demo writes into its evidence (edit_window.start_epoch_ns / end_epoch_ns).
  * A successful exec (return value 0) of a build tool counts as a violation. A failed
    execve (for example a PATH probe that returns ENOENT) is recorded but is not a process
    start. The name matching is by basename, including versioned names (gcc-13, clang-18).
  * Violations inside the edit window (capsule.submit .. candidate.install) and violations
    anywhere in the run are reported separately. Any violation anywhere fails the probe.

Self-check (two positive controls): before the demo is trusted, the same detector must catch
(1) `cc --version` resolved through PATH and (2) `<absolute path to cc> --version`, which a PATH
shim cannot see (this is the form the M13 mutant uses). Either one missed means the probe exits 2
(inconclusive), never 0. With no compiler installed, a stub named cc is used for both controls.
The demo is run with --binary (default build/sutekh) so the probe drives the binary under test.

Static scan: the linked binary's printable strings and dynamic imports are checked for
build-tool names and for exec/popen/system calls; src/ is scanned for the same calls and for
build-tool literals. Results are reported; a build-tool literal in the static scan is listed
as a finding.

Limits (stated, not hidden):
  * It sees exec-family syscalls only. A process that runs with a build tool's code inside
    the same address space (in-process compilation) would not be seen. The static scan and
    the source review cover that case only partially.
  * strace slows the traced tree. Timings in the demo evidence are from the untraced run.
  * strace can miss a process if it ptrace-detached or if the kernel denies ptrace; a denied
    attach fails the run with a non-zero strace status, which the probe reports as
    inconclusive instead of passing.

Exit status: 0 pass (control detected, no build-tool exec, demo passed);
             1 violation (a build tool was executed by the traced tree);
             2 inconclusive (no strace, control not detected, demo failed, or unreadable log).
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DEMO = os.path.join(REPO, "tools", "demo_zero_wait.py")
BINARY = os.path.join(REPO, "build", "sutekh")
SRC = os.path.join(REPO, "src")
PROBE_FORMAT = "m0-no-build-probe/0.1"

BUILD_TOOL_RE = re.compile(
    r"^(cc|c\+\+|gcc|g\+\+|clang|clang\+\+|cpp|cc1|cc1plus|collect2|as|ld|ld\.(bfd|gold|lld)|lld|"
    r"ld64|ar|ranlib|nasm|yasm|cmake|ninja|make|gmake|meson|rustc|tcc|mold|"
    r"(x86_64|aarch64)-linux-gnu-(gcc|g\+\+|as|ld))(-?[0-9.]+)?$")
EXEC_CALL_RE = re.compile(r"\b(system|popen|execl|execlp|execle|execv|execve|execvp|execvpe|posix_spawnp?|fork|vfork)\s*\(")
TOOL_LITERAL_RE = re.compile(r'"(cc|c\+\+|gcc|g\+\+|clang|as|ld|lld|cmake|ninja|make)(\s[^"]*)?"')
EXEC_SYMBOL_RE = re.compile(r"^(system|popen|execv|execve|execvp|execl|posix_spawn|posix_spawnp|fork|vfork)$")
STRACE_LINE_RE = re.compile(r"^(?P<pid>\d+)\s+(?P<ts>\d+\.\d+)\s+(?P<body>.*)$")
EXECVE_RE = re.compile(r'^(?P<sc>execve(?:at)?)\((?:[^"]*?,\s*)?"(?P<path>(?:[^"\\]|\\.)*)"')
RESULT_RE = re.compile(r"\)\s*=\s*(?P<rc>-?\d+)")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def is_build_tool(path: str) -> bool:
    return bool(BUILD_TOOL_RE.match(os.path.basename(path)))


def parse_strace(text: str) -> list:
    """Return one record per exec-family call: pid, epoch ts, path, return code (None if unknown)."""
    records = []
    pending: dict = {}
    for raw in text.splitlines():
        m = STRACE_LINE_RE.match(raw)
        if not m:
            continue
        pid, ts, body = int(m.group("pid")), float(m.group("ts")), m.group("body")
        if "resumed>" in body and pid in pending:
            rc = RESULT_RE.search(body)
            if rc is None:
                continue
            path_info = pending.pop(pid)
            records.append({"pid": pid, "ts": path_info["ts"], "path": path_info["path"],
                            "rc": int(rc.group("rc")), "syscall": path_info["sc"]})
            continue
        em = EXECVE_RE.match(body)
        if not em:
            continue
        if "unfinished" in body:
            pending[pid] = {"ts": ts, "path": em.group("path"), "sc": em.group("sc")}
            continue
        rc = RESULT_RE.search(body)
        if rc is None:
            continue
        records.append({"pid": pid, "ts": ts, "path": em.group("path"),
                        "rc": int(rc.group("rc")), "syscall": em.group("sc")})
    return records


def strace_run(argv: list, log_path: str, cwd: str, env=None):
    cmd = ["strace", "-f", "-ttt", "-e", "trace=execve,execveat", "-s", "4096", "-o", log_path, "--", *argv]
    proc = subprocess.run(cmd, cwd=cwd, shell=False, capture_output=True, text=True, env=env, check=False)
    text = ""
    if os.path.exists(log_path):
        with open(log_path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    return {"argv": cmd, "returncode": proc.returncode, "stderr_tail": (proc.stderr or "")[-600:],
            "records": parse_strace(text), "log_lines": text.count("\n")}


def summarise(records: list, window=None) -> dict:
    successful = [r for r in records if r["rc"] == 0]
    builds = [r for r in successful if is_build_tool(r["path"])]
    in_window = []
    if window is not None:
        lo, hi = window
        in_window = [r for r in builds if lo <= r["ts"] <= hi]
    return {
        "exec_attempts": len(records),
        "successful_execs": len(successful),
        "failed_exec_attempts": len(records) - len(successful),
        "distinct_executed_basenames": sorted({os.path.basename(r["path"]) for r in successful}),
        "build_tool_execs_total": [{"pid": r["pid"], "ts": r["ts"], "path": r["path"]} for r in builds],
        "build_tool_execs_in_window": [{"pid": r["pid"], "ts": r["ts"], "path": r["path"]} for r in in_window],
    }


def _cc_location(tmp: str):
    """(absolute path of a C compiler driver, how it was obtained). Falls back to a stub named cc."""
    cc = shutil.which("cc")
    if cc is not None:
        return os.path.abspath(cc), "installed cc"
    stubdir = os.path.join(tmp, "stub")
    os.makedirs(stubdir, exist_ok=True)
    stub = os.path.join(stubdir, "cc")
    with open(stub, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\necho stub-cc\n")
    os.chmod(stub, 0o755)
    return stub, "stub cc (no compiler installed)"


def _control(name: str, argv: list, tmp: str, env, log_name: str) -> dict:
    """Run one deliberate build-tool invocation under strace; the detector must report it."""
    run = strace_run(argv, os.path.join(tmp, log_name), REPO, env=env)
    summary = summarise(run["records"])
    detected = any(os.path.basename(r["path"]) == "cc" for r in summary["build_tool_execs_total"])
    return {"name": name, "argv": argv, "detected": detected, "strace_returncode": run["returncode"],
            "summary": summary}


def positive_controls(tmp: str) -> list:
    """Two deliberate invocations, both of which the detector must catch before any absence claim:
      bare   `cc` resolved through PATH (the only form a PATH shim can see)
      abs    `cc` invoked by its absolute path (the form M13 uses; a PATH shim cannot see it)
    """
    cc_path, how = _cc_location(tmp)
    stub_env = None
    if shutil.which("cc") is None:
        stub_env = dict(os.environ, PATH=os.path.dirname(cc_path) + os.pathsep + os.environ.get("PATH", ""))
    bare = _control(f"bare cc via PATH ({how})", ["sh", "-c", "cc --version >/dev/null 2>&1; exit 0"],
                    tmp, stub_env, "control_bare.strace")
    absolute = _control(f"absolute path {cc_path} ({how})",
                        ["sh", "-c", f"{cc_path} --version >/dev/null 2>&1; exit 0"],
                        tmp, None, "control_abs.strace")
    return [bare, absolute]


def positive_control(tmp: str) -> dict:
    """Backward-compatible view of the first (bare-name) control."""
    return positive_controls(tmp)[0]


def static_scan() -> dict:
    out: dict = {"binary": {"path": os.path.relpath(BINARY, REPO)}}
    if os.path.isfile(BINARY):
        with open(BINARY, "rb") as fh:
            data = fh.read()
        strings = {m.decode("ascii") for m in re.findall(rb"[\x20-\x7e]{4,}", data)}
        tool_strings = sorted(s for s in strings if BUILD_TOOL_RE.match(os.path.basename(s))
                              or re.search(r"(^|/)(cc|gcc|g\+\+|clang|as|ld|lld|cmake|ninja|make)(-[0-9.]+)?$", s))
        exec_strings = sorted(s for s in strings if re.search(r"\b(popen|system)\b", s))
        out["strings_naming_build_tools"] = tool_strings
        out["strings_mentioning_popen_or_system"] = exec_strings
        nm = shutil.which("nm")
        if nm is None:
            out["dynamic_imports"] = "unavailable (nm not installed)"
        else:
            proc = subprocess.run([nm, "-D", "--undefined-only", BINARY], capture_output=True,
                                  text=True, shell=False, check=False)
            names = [ln.split()[-1].split("@")[0] for ln in proc.stdout.splitlines() if ln.strip()]
            out["dynamic_imports"] = sorted({n for n in names if EXEC_SYMBOL_RE.match(n)})
            out["dynamic_imports_all_count"] = len(names)
    else:
        out["binary_error"] = "build/sutekh is missing; run python3 tools/dev.py build"
    src_exec, src_tool = [], []
    for root, _dirs, files in os.walk(SRC):
        for name in sorted(files):
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, REPO).replace(os.sep, "/")
            with open(path, encoding="utf-8", errors="replace") as fh:
                for no, line in enumerate(fh, 1):
                    if EXEC_CALL_RE.search(line):
                        src_exec.append({"at": f"{rel}:{no}", "text": line.strip()[:160]})
                    if TOOL_LITERAL_RE.search(line):
                        src_tool.append({"at": f"{rel}:{no}", "text": line.strip()[:160]})
    out["src_exec_call_sites"] = src_exec
    out["src_build_tool_literals"] = src_tool
    return out


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=None, help="probe evidence JSON (default outside the repository)")
    parser.add_argument("--demo-arg", action="append", default=[], help="extra argument for the demo")
    parser.add_argument("--binary", default=BINARY, help="executable the demo drives (default build/sutekh)")
    args = parser.parse_args(argv)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out_path = args.out or os.path.join(os.path.expanduser("~"), ".cache", "sutekh", "no_build_probe",
                                        f"{stamp}-{os.getpid()}.json")
    evidence = {"probe_format": PROBE_FORMAT, "started_utc": stamp,
                "method": "strace -f -ttt -e trace=execve,execveat -s 4096 (exec-family syscalls, all children)",
                "build_tool_pattern": BUILD_TOOL_RE.pattern, "verdict": None, "reason": None}
    if shutil.which("strace") is None:
        evidence.update(verdict="inconclusive", reason="strace is not installed")
        print("inconclusive: strace is not installed (no fallback was run)")
        write(out_path, evidence)
        return 2

    tmp = tempfile.mkdtemp(prefix="no-build-probe-")
    try:
        controls = positive_controls(tmp)
        evidence["positive_controls"] = controls
        evidence["positive_control"] = controls[0]
        for control in controls:
            print(f"positive control ({control['name']}): detected={control['detected']}")
        missed = [c["name"] for c in controls if not c["detected"]]
        if missed:
            evidence.update(verdict="inconclusive",
                            reason="positive control was not detected (" + "; ".join(missed)
                                   + "); the probe cannot show absence")
            print("inconclusive: the detector missed a deliberate cc invocation")
            write(out_path, evidence)
            return 2

        demo_out = os.path.join(tmp, "demo_evidence.json")
        demo_argv = [sys.executable, "-I", DEMO, "--binary", os.path.abspath(args.binary), "--out", demo_out,
                     *args.demo_arg]
        print("running: " + " ".join(demo_argv) + " (under strace)", flush=True)
        run = strace_run(demo_argv, os.path.join(tmp, "run.strace"), REPO)
        evidence["traced_run"] = {"argv": run["argv"], "strace_returncode": run["returncode"],
                                  "strace_stderr_tail": run["stderr_tail"], "log_lines": run["log_lines"]}
        demo = None
        if os.path.isfile(demo_out):
            with open(demo_out, encoding="utf-8") as fh:
                demo = json.load(fh)
        evidence["demo"] = {"status": demo.get("status") if demo else None,
                            "failure": demo.get("failure") if demo else None,
                            "binary_sha256": (demo or {}).get("binary", {}).get("sha256")}
        window = None
        if demo and "edit_window" in demo:
            w = demo["edit_window"]
            window = (w["start_epoch_ns"] / 1e9, w["end_epoch_ns"] / 1e9)
            evidence["edit_window_epoch_s"] = {"start": window[0], "end": window[1],
                                               "duration_ms": round((window[1] - window[0]) * 1000, 3)}
        summary = summarise(run["records"], window)
        evidence["run_summary"] = summary
        evidence["static_scan"] = static_scan()
        violations = summary["build_tool_execs_total"]
        window_violations = summary["build_tool_execs_in_window"]
        print(f"traced exec attempts: {summary['exec_attempts']} (successful {summary['successful_execs']}); "
              f"executed basenames: {', '.join(summary['distinct_executed_basenames'])}")
        if window is not None:
            print(f"edit window {evidence['edit_window_epoch_s']['duration_ms']} ms: "
                  f"build-tool execs in window = {len(window_violations)}")
        print(f"build-tool execs in whole run = {len(violations)}")
        if violations:
            evidence.update(verdict="fail", reason="a build tool was executed by the traced tree")
            code = 1
        elif demo is None or demo.get("status") != "passed":
            evidence.update(verdict="inconclusive",
                            reason="the demo did not complete with status passed; no conclusion about the edit")
            code = 2
        elif run["returncode"] != 0:
            evidence.update(verdict="inconclusive", reason=f"strace returned {run['returncode']}")
            code = 2
        else:
            evidence.update(verdict="pass",
                            reason="both positive controls (bare and absolute-path cc) detected; zero build-tool "
                                   "execs in the whole traced run, including the capsule edit/install window")
            code = 0
        print(f"verdict: {evidence['verdict']} ({evidence['reason']})")
        write(out_path, evidence)
        print(f"evidence: {out_path}")
        return code
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def write(path: str, obj) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2, sort_keys=True, ensure_ascii=False)
        fh.write("\n")


if __name__ == "__main__":
    sys.exit(main())
