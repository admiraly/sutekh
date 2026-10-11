# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M0 acceptance harness for the Sutekh executable (docs/TEST_SPEC.md T00-T15).

Standard library only. Run with:  python3 -I tests/harness/run_m0.py [options]

What it does
  * Drives the REAL executable (default build/sutekh, override with --binary) over
    the JSON-lines control protocol (docs/AGENT_RPC.md) via `<binary> serve`.
  * Computes every expected value from fixture inputs with tests/harness/reference_eval.py
    (an evaluator independent of the engine). Fixture expectations are cross-checked
    against that evaluator first.
  * Never trusts an engine-provided "passed"/"ok" flag alone: results are compared with
    expected values, error codes are compared exactly, and negative cases also check
    that no state changed.
  * Reports each test as passed / failed / not_run / unavailable with reasons.
      passed       every check ran and matched expectations.
      failed       a check mismatched, or the engine answered in a malformed way.
      unavailable  the executable does not advertise (or answers SU_E_UNSUPPORTED_METHOD
                   for) a feature this test needs. The reason is the real reply.
      not_run      the harness did not execute the check: prerequisite missing, build
                   skipped, or a precondition that cannot be constructed.

A test body that records no check cannot be passed (NOT_RUN). Process identities (supervisor, worker,
killed children) are checked against OS facts from /proc (pid, parent, start time), not only against the
pids the engine reports. T13 compares its measurements with the proposed targets in docs/PERFORMANCE.md
section 2 and records a verdict (met / not_met / not_comparable, with reasons) per target; those verdicts
are provisional report fields, not the test status. T12's primary evidence is the strace probe
(tests/e2e/no_build_probe.py): an inconclusive probe is not_run, never passed.

Control-plane parameters are the ones the engine's handlers accept (src/agent/su_agent.c
and src/runtime/worker.c). They are documented once, in ENGINE_METHODS below. Anything the
engine does not implement is reported unavailable with the engine's own reply or the
advertised capability list, never as a pass.

Exit status: 0 when no test failed, 1 when at least one failed, 2 on harness usage errors.
"""
from __future__ import annotations

import argparse
import collections
import datetime
import hashlib
import importlib.util
import json
import math
import platform
import os
import queue
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import reference_eval as ref  # noqa: E402

HARNESS_FORMAT = "m0-harness/0.1"
PROTOCOL = "0.1"
MAX_MESSAGE = 1048576          # AGENT_RPC.md section 1: 1 MiB per message
READ_CAP = 4 * 1048576         # harness-side bound on one reply line
PASSED, FAILED, NOT_RUN, UNAVAILABLE = "passed", "failed", "not_run", "unavailable"
STATUSES = (PASSED, FAILED, NOT_RUN, UNAVAILABLE)
BUILD_COMMS = {"cc1", "cc1plus", "as", "ld", "ld.bfd", "ld.gold", "ld.lld", "gcc", "g++",
               "clang", "clang-18", "cmake", "ninja", "make"}
SHIM_TOOLS = ["cc", "gcc", "g++", "c++", "clang", "as", "ld", "cmake", "ninja", "make"]
UNSUPPORTED = "SU_E_UNSUPPORTED_METHOD"


class Outcome(Exception):
    status = FAILED


class Failed(Outcome):
    status = FAILED


class Unavailable(Outcome):
    status = UNAVAILABLE


class NotRun(Outcome):
    status = NOT_RUN


# ------------------------------------------------------------------ pure helpers

def _reject_duplicates(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate key {key!r}")
        out[key] = value
    return out


def _reject_constant(name):
    raise ValueError(f"non-JSON constant {name}")


def strict_loads(text: str):
    """JSON loader that rejects duplicate keys and NaN/Infinity."""
    return json.loads(text, object_pairs_hook=_reject_duplicates, parse_constant=_reject_constant)


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def is_u32(value) -> bool:
    return type(value) is int and 0 <= value <= 0xFFFFFFFF


def compare_columns(expected: dict, actual) -> list:
    """Exact comparison of column maps. Returns mismatch strings (empty means equal)."""
    if not isinstance(actual, dict):
        return ["columns is not a JSON object"]
    out = []
    if set(actual) != set(expected):
        out.append(f"column names: expected {sorted(expected)} actual {sorted(actual)}")
    for name in sorted(set(expected) & set(actual)):
        exp, act = expected[name], actual[name]
        if not isinstance(act, list):
            out.append(f"{name}: not a list")
            continue
        if len(act) != len(exp):
            out.append(f"{name}: row count expected {len(exp)} actual {len(act)}")
            continue
        for i, (e, a) in enumerate(zip(exp, act)):
            if not is_u32(a):
                out.append(f"{name}[{i}]: not a u32 JSON integer: {a!r}")
            elif a != e:
                out.append(f"{name}[{i}]: expected {e} actual {a}")
            if len(out) >= 20:
                return out
    return out


def verify_state(expected_tick: int, expected_columns: dict, inspect_result) -> list:
    """Compare an inspect result (tick + columns) with independently derived values."""
    out = []
    tick = inspect_result.get("tick") if isinstance(inspect_result, dict) else None
    if type(tick) is not int or tick != expected_tick:
        out.append(f"tick: expected {expected_tick} actual {tick!r}")
    out.extend(compare_columns(expected_columns, (inspect_result or {}).get("columns")))
    return out


def classify_error(envelope: dict, expected_code: str) -> list:
    """Mismatch list for an envelope that must be a rejection with exactly expected_code."""
    if envelope.get("ok") is True:
        return [f"accepted, expected rejection {expected_code}"]
    code = envelope["error"].get("code")
    if code != expected_code:
        return [f"error code: expected {expected_code} actual {code}"]
    return []


def parse_envelope(raw: bytes, expected_id) -> dict:
    """Validate one reply line. Raises Failed on any envelope violation."""
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise Failed(f"reply is not UTF-8: {exc}") from None
    try:
        obj = strict_loads(text)
    except ValueError as exc:
        raise Failed(f"reply is not strict JSON: {exc}") from None
    if not isinstance(obj, dict):
        raise Failed("reply is not a JSON object")
    if obj.get("protocol") != PROTOCOL:
        raise Failed(f"reply protocol is {obj.get('protocol')!r}, expected {PROTOCOL!r}")
    ok = obj.get("ok")
    if not isinstance(ok, bool):
        raise Failed("reply has no boolean ok")
    if expected_id is not None and obj.get("id") != expected_id \
            and not (ok is False and obj.get("id") is None):
        raise Failed(f"reply id {obj.get('id')!r} does not match request {expected_id!r}")
    if ok:
        if not isinstance(obj.get("result"), dict):
            raise Failed("ok reply has no result object")
        return {"ok": True, "result": obj["result"], "id": obj.get("id")}
    err = obj.get("error")
    if not isinstance(err, dict) or not isinstance(err.get("code"), str):
        raise Failed("error reply has no error.code")
    return {"ok": False, "error": err, "id": obj.get("id")}


def classify_engine_info(raw, probe_id: str) -> dict:
    """Classify the reply to engine.info sent to `serve`. Pure; used by the probe and self-test."""
    if raw is None:
        return {"state": "absent", "reason": "serve closed stdout without answering engine.info"}
    try:
        obj = strict_loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        return {"state": "malformed", "reason": f"engine.info reply is not strict UTF-8 JSON: {exc}"}
    if isinstance(obj, dict) and "protocol" not in obj and isinstance(obj.get("error"), dict) \
            and obj["error"].get("code") == UNSUPPORTED:
        return {"state": "absent",
                "reason": "serve role is not implemented: the binary answered without an envelope "
                          f"with {UNSUPPORTED} ({raw[:200].decode('utf-8', 'replace')})"}
    try:
        env = parse_envelope(raw, probe_id)
    except Failed as exc:
        return {"state": "malformed", "reason": f"engine.info reply malformed: {exc.args[0]}"}
    if not env["ok"]:
        if env["error"].get("code") == UNSUPPORTED:
            return {"state": "absent", "reason": "engine.info is not implemented (SU_E_UNSUPPORTED_METHOD)"}
        return {"state": "malformed",
                "reason": f"engine.info failed with {env['error'].get('code')}: "
                          f"{env['error'].get('message', '')}"}
    result = env["result"]
    caps = result.get("capabilities")
    if isinstance(caps, list) and all(isinstance(c, str) for c in caps):
        caps = set(caps)
    elif isinstance(caps, dict):
        caps = {k for k, v in caps.items() if v is True}
    else:
        caps = None
    return {"state": "present", "reason": "", "capabilities": caps, "info": result}


def verify_rejections_leave_state(before: dict, after: dict) -> list:
    """Negative cases: no collateral change to tick, columns or revision."""
    out = []
    for key in ("world_revision", "tick"):
        if before.get(key) != after.get(key):
            out.append(f"{key} changed from {before.get(key)!r} to {after.get(key)!r}")
    out.extend(compare_columns(before.get("columns", {}), after.get("columns")))
    return out


def compare_manifest(expected, actual) -> list:
    if not isinstance(actual, list):
        return ["semantic_manifest missing or not a list"]
    if expected != actual:
        return [f"semantic_manifest differs: expected {expected!r} actual {actual!r}"]
    return []


def read_header_constant(relpath: str, name: str):
    with open(os.path.join(REPO, relpath), encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r"#define\s+" + name + r"\s+(?:UINT32_C\((\d+)\)|\"([^\"]*)\")", text)
    if not m:
        raise SystemExit(f"{name} not found in {relpath}")
    return int(m.group(1)) if m.group(1) is not None else m.group(2)


def make_oversize_line(total_bytes: int, method: str = "engine.info") -> bytes:
    """One protocol line of exactly total_bytes that is valid JSON for `method` (padding in a string)."""
    head = ('{"protocol":"0.1","id":"oversize-1","method":"' + method + '","params":{"pad":"').encode("ascii")
    tail = b'"}}'
    pad = total_bytes - len(head) - len(tail)
    if pad < 0:
        raise ValueError("total_bytes too small")
    return head + b"x" * pad + tail


def _short(value, limit=200) -> str:
    text = repr(value)
    return text if len(text) <= limit else text[:limit] + "..."


def parse_build_tool_lines(out: str) -> dict:
    tools = {}
    for line in out.splitlines():
        m = re.match(r"^\s+(python|cmake|ninja|compiler): (.*)$", line)
        if m:
            tools[m.group(1)] = m.group(2).strip()
    return tools


def parse_version_output(out: str) -> dict:
    fields = {}
    for line in out.splitlines():
        parts = line.split(" ", 1)
        if len(parts) == 2:
            fields[parts[0]] = parts[1].strip()
    return fields


# Verifier stage -> codes that stage may report (M0_CONTRACT section 1, fixed stage order). Written
# separately from the negative-fixture generator's table so the self-test can cross-check them.
STAGE_CODES = {
    1: {"SU_E_RESOURCE_LIMIT"},
    2: {"SU_E_PARSE"},
    3: {"SU_E_DUPLICATE_KEY"},
    4: {"SU_E_TYPE"},
    5: {"SU_E_SCHEMA_MISMATCH"},
    6: {"SU_E_UNKNOWN_OPCODE", "SU_E_TYPE", "SU_E_UNDEFINED_REGISTER", "SU_E_DUPLICATE_REGISTER",
        "SU_E_UNDECLARED_READ", "SU_E_UNDECLARED_WRITE"},
    7: {"SU_E_MISSING_STORE"},
}


def stage_code_consistent(stage, code) -> bool:
    return code in STAGE_CODES.get(stage, set())


def parse_proc_stat(raw: str, pid=None) -> dict:
    """Parse /proc/<pid>/stat. comm may contain spaces and ')', so split at the last ')'.
    After it: field 3 (state) is index 0, field 4 (ppid) index 1, field 22 (starttime) index 19."""
    close = raw.rfind(")")
    comm = raw[raw.find("(") + 1:close]
    fields = raw[close + 2:].split()
    return {"pid": pid, "comm": comm, "state": fields[0], "ppid": int(fields[1]), "starttime": int(fields[19])}


def os_identity(pid):
    """OS facts for a pid: None when the process does not exist. Start time is in clock ticks since boot,
    so a pid that was recycled for a new process has a different identity."""
    if not isinstance(pid, int) or isinstance(pid, bool):
        return None
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8") as fh:
            raw = fh.read()
    except OSError:
        return None
    return parse_proc_stat(raw, pid)


def identity_key(ident):
    return None if ident is None else (ident["pid"], ident["starttime"], ident["ppid"])


def is_dead(ident) -> bool:
    return ident is None or ident["state"] in ("Z", "X")


def binary_sanitizers(path: str) -> list:
    """Sanitizer runtimes linked into a binary, from its symbol names. Empty for a plain build."""
    markers = {"asan": b"__asan_init", "ubsan": b"__ubsan_handle", "tsan": b"__tsan_init", "msan": b"__msan_init"}
    with open(path, "rb") as fh:
        data = fh.read()
    return sorted(name for name, marker in markers.items() if marker in data)


def with_asan_quarantine(env: dict, mb: int = 1) -> dict:
    """ASan keeps freed blocks in a quarantine (default 256 MB) before reuse, so VmRSS grows for reasons
    unrelated to the engine. For the RSS check only, cap it; the rest of ASAN_OPTIONS is preserved."""
    out = dict(env)
    prior = out.get("ASAN_OPTIONS", "")
    out["ASAN_OPTIONS"] = (prior + ":" if prior else "") + f"quarantine_size_mb={mb}"
    return out


def rss_window_median(samples, lo: int, hi: int):
    """Median VmRSS (KiB) of samples whose iteration index is in [lo, hi). None when the window is empty.
    A median over a window removes single-sample noise; comparing windows removes start-up growth."""
    vals = sorted(kib for it, kib in samples if lo <= it < hi and kib is not None)
    if not vals:
        return None
    mid = len(vals) // 2
    return vals[mid] if len(vals) % 2 else (vals[mid - 1] + vals[mid]) / 2


# Proposed targets from docs/PERFORMANCE.md section 2 (all unmeasured there; C10: shared VM, provisional).
# gates: conditions the measurement must satisfy for a verdict; caveats: reporting requirements that are
# recorded but do not change the verdict. proxy: the measured RPC is an upper bound on the target quantity.
TIMING_TARGETS = (
    {"id": "ir_prepare", "row": "IR prepare latency: p95 <= 20 ms (<=64 KiB source, fixed schema, warm process, 1,024-row preview)",
     "metric": "prepare_submit_and_evaluate", "p95_ms_max": 20.0, "proxy": False,
     "gates": ("source <= 64 KiB", "existing fixed schema", "warm process", "1,024-row preview"), "caveats": ()},
    {"id": "preview_response", "row": "Isolated preview response: p95 <= 100 ms (reusable worker, small fixture)",
     "metric": "evaluate_reusable_worker", "p95_ms_max": 100.0, "proxy": False,
     "gates": ("reusable worker", "small fixture", "excludes long qualification suites"), "caveats": ()},
    {"id": "install_critical_section", "row": "Code installation critical section: p95 <= 1 ms (code-only swap, prepared, no migration)",
     "metric": "install_call_round_trip", "p95_ms_max": 1.0, "proxy": True,
     "gates": ("code-only swap", "bindings already prepared", "no migration"),
     "caveats": ("the measured RPC contains the critical section; the section itself is not isolated",)},
    {"id": "warm_edit_to_first_effect", "row": "Warm edit to first preview effect: p95 <= 100 ms",
     "metric": "edit_to_first_effect", "p95_ms_max": 100.0, "proxy": False,
     "gates": ("warm process",),
     "caveats": ("queue and worker delays are inside one install RPC and are not separated",)},
    {"id": "native_incremental_check", "row": "Targeted native incremental check: aim p95 <= 2 s",
     "metric": None, "p95_ms_max": 2000.0, "proxy": False, "gates": (), "caveats": ()},
)
TIMING_VERDICTS = ("met", "not_met", "not_comparable")


def compare_target(target: dict, stats, gates: dict, caveats=()) -> dict:
    """Verdict for one proposed target. gates maps each gate name to (met: bool, reason: str).

    met            p95 <= target and every gate met (install: upper-bound proxy within target is also met)
    not_met        p95 > target and every gate met, not a proxy
    not_comparable a gate is unmet, the metric is not measured, or the proxy exceeds the target (the
                   section itself is not isolated); the numbers are still recorded
    A missing or non-finite p95 raises ValueError: a target without a measurement must not be silently skipped.
    """
    if set(gates) != set(target["gates"]):
        raise ValueError(f"{target['id']}: gate set {sorted(gates)} differs from declared {sorted(target['gates'])}")
    base = {"id": target["id"], "row": target["row"], "metric": target["metric"], "provisional": True,
            "target_p95_ms_max": target["p95_ms_max"], "gates": {k: {"met": bool(v[0]), "reason": v[1]}
                                                                   for k, v in gates.items()},
            "caveats": list(target["caveats"]) + list(caveats),
            "note": "provisional: shared VM, C10 (proposed budgets are not measurements)"}
    if target["metric"] is None:
        return {**base, "p95_ms": None, "n": 0, "verdict": "not_comparable",
                "reason": "not measured by T13 (native edit lane; no native measurement in this harness)"}
    if not isinstance(stats, dict) or not isinstance(stats.get("p95"), (int, float)) \
            or isinstance(stats.get("p95"), bool) or not math.isfinite(stats["p95"]) or not stats.get("n"):
        raise ValueError(f"{target['id']}: no finite measured p95 for {target['metric']}")
    p95 = float(stats["p95"])
    within = p95 <= target["p95_ms_max"]
    unmet = [name for name, (ok, _r) in gates.items() if not ok]
    if unmet:
        reasons = "; ".join(f"{name}: {gates[name][1]}" for name in unmet)
        verdict, reason = "not_comparable", f"unmet conditions ({reasons}); measured p95 {p95} ms"
    elif within:
        verdict = "met"
        reason = f"measured p95 {p95} ms <= {target['p95_ms_max']} ms"
    elif target["proxy"]:
        verdict = "not_comparable"
        reason = (f"measured RPC p95 {p95} ms exceeds {target['p95_ms_max']} ms, but the RPC is only an upper "
                  "bound on the critical section, which is not isolated: no conclusion about the section")
    else:
        verdict = "not_met"
        reason = f"measured p95 {p95} ms > {target['p95_ms_max']} ms with every gate met"
    return {**base, "p95_ms": p95, "n": stats["n"], "verdict": verdict, "reason": reason}


# ------------------------------------------------------------------ process I/O

class Session:
    """One `<binary> serve` process speaking JSON lines on stdio."""

    def __init__(self, tctx, label, env=None):
        self.tctx = tctx
        self.label = label
        self.argv = [tctx.binary, "serve"]
        self.proc = subprocess.Popen(self.argv, cwd=REPO, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     shell=False, env=env or tctx.env)
        self.cmd = {"label": label, "argv": self.argv, "cwd": REPO, "pid": self.proc.pid,
                    "returncode": None}
        tctx.commands.append(self.cmd)
        self._msgs: queue.Queue = queue.Queue()
        self._err = collections.deque(maxlen=40)
        self._seq = 0
        self._closed = False
        self.revs = {}  # world_id -> last world_revision seen in a reply
        threading.Thread(target=self._read_out, daemon=True).start()
        threading.Thread(target=self._read_err, daemon=True).start()

    def _read_out(self):
        buf = bytearray()
        while True:
            chunk = self.proc.stdout.readline(65536)
            if not chunk:
                if buf:
                    self._msgs.put(bytes(buf))
                self._msgs.put(None)
                return
            buf += chunk
            if chunk.endswith(b"\n"):
                self._msgs.put(bytes(buf[:-1]))
                buf = bytearray()
            elif len(buf) > READ_CAP:
                self._msgs.put(None)
                return

    def _read_err(self):
        for raw in iter(self.proc.stderr.readline, b""):
            self._err.append(raw.decode("utf-8", "replace"))

    def stderr_tail(self) -> str:
        return "".join(self._err)[-800:]

    def send(self, payload: bytes, tolerate_closed=False, timeout=30.0):
        errors = []

        def writer():
            try:
                self.proc.stdin.write(payload)
                self.proc.stdin.flush()
            except OSError as exc:
                errors.append(exc)

        thread = threading.Thread(target=writer, daemon=True)
        thread.start()
        thread.join(timeout)
        if thread.is_alive():
            raise Failed(f"{self.label}: engine did not accept request bytes within {timeout}s")
        if errors and not tolerate_closed:
            raise Failed(f"{self.label}: engine stdin closed: {errors[0]}")

    def read_message(self, timeout=30.0):
        try:
            return self._msgs.get(timeout=timeout)
        except queue.Empty:
            raise Failed(f"{self.label}: no reply within {timeout}s") from None

    def request(self, method: str, params: dict, timeout=60.0) -> dict:
        self._seq += 1
        rid = f"m0-{self._seq}"
        body = json.dumps({"protocol": PROTOCOL, "id": rid, "method": method, "params": params},
                          separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\n"
        self.send(body)
        raw = self.read_message(timeout)
        if raw is None:
            raise Failed(f"{self.label}: engine closed stdout during {method}; "
                         f"stderr tail: {self.stderr_tail()!r}")
        return parse_envelope(raw, rid)

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            rc = self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = self.proc.wait()
        self.cmd["returncode"] = rc
        self.cmd["stderr_tail"] = self.stderr_tail()


# ------------------------------------------------------------------ run context

# Detail records are capped to keep the report readable. The cap is never silent: every check
# is counted (checks_total / checks_failed) and the report states checks_recorded and checks_dropped.
MAX_RECORDED_CHECKS = 20000
DEFAULT_RELOAD_ITERATIONS = 1000  # TEST_SPEC T11 count


class RunCtx:
    def __init__(self, binary, no_build, reload_iterations, latency_samples):
        self.binary = binary
        self.no_build = no_build
        self.reload_iterations = reload_iterations
        self.latency_samples = latency_samples
        self.env = dict(os.environ)
        self.probe = None


class TestCtx:
    def __init__(self, run: RunCtx, spec_id: str):
        self.run = run
        self.binary = run.binary
        self.env = run.env
        self.spec_id = spec_id
        self.commands: list = []
        self.checks: list = []
        self.checks_total = 0
        self.checks_failed = 0
        self.checks_dropped = 0
        self.inputs: dict = {}
        self.details: dict = {}
        self.sessions: list = []

    def _record(self, entry):
        self.checks_total += 1
        if not entry.get("ok"):
            self.checks_failed += 1
        if len(self.checks) < MAX_RECORDED_CHECKS:
            self.checks.append(entry)
        else:
            self.checks_dropped += 1

    # inputs and fixtures
    def abspath(self, rel: str) -> str:
        return os.path.join(REPO, rel)

    def rel(self, abs_path: str) -> str:
        return os.path.relpath(abs_path, REPO).replace(os.sep, "/")

    def need_file(self, rel: str) -> str:
        path = self.abspath(rel)
        if not os.path.isfile(path):
            raise NotRun(f"input missing: {rel}")
        self.inputs[rel] = sha256_file(path)
        return path

    def load(self, rel: str):
        path = self.need_file(rel)
        with open(path, encoding="utf-8") as fh:
            try:
                return strict_loads(fh.read())
            except ValueError as exc:
                raise Failed(f"fixture {rel} is not strict JSON: {exc}") from None

    def load_system(self, cap_rel: str):
        """Return (capsule, lir, source_rel) for a capsule; the LIR is read from its source."""
        cap = self.load(cap_rel)
        cap_dir = os.path.dirname(cap_rel)
        source_rel = os.path.normpath(os.path.join(cap_dir, cap["source"])).replace(os.sep, "/")
        return cap, self.load(source_rel), source_rel

    # checks
    def check(self, name, expected, actual):
        ok = expected == actual
        self._record({"name": name, "expected": expected, "actual": actual, "ok": ok})
        if not ok:
            raise Failed(f"{name}: expected {_short(expected)} actual {_short(actual)}")

    def check_mismatches(self, name, mismatches):
        self._record({"name": name, "mismatches": mismatches[:20], "ok": not mismatches})
        if mismatches:
            raise Failed(f"{name}: " + "; ".join(mismatches[:5]))

    def session(self, label, env=None) -> Session:
        sess = Session(self, label, env=env)
        self.sessions.append(sess)
        return sess

    def close(self):
        for sess in self.sessions:
            sess.close()

    def run_cmd(self, argv, timeout=900):
        """Run a command (argv list, shell=False) inside the repo and record it exactly."""
        try:
            proc = subprocess.run(argv, cwd=REPO, capture_output=True, text=True,
                                  shell=False, timeout=timeout, env=self.env)
            rc, out = proc.returncode, (proc.stdout or "") + (proc.stderr or "")
        except OSError as exc:
            rc, out = None, f"could not start: {exc}"
        self.commands.append({"label": "command", "argv": argv, "cwd": REPO, "returncode": rc,
                              "stdout_sha256": sha256_bytes(out.encode("utf-8", "replace")),
                              "output_tail": out[-3000:]})
        return rc, out


def ok_result(tctx: TestCtx, method: str, env: dict) -> dict:
    if env["ok"]:
        return env["result"]
    code = env["error"].get("code")
    if code == UNSUPPORTED:
        raise Unavailable(f"{method} answered {UNSUPPORTED}")
    raise Failed(f"{method} failed with {code}: {env['error'].get('message', '')}")


# Real M0 control-plane surface (verified against the handlers, not assumed).
# Results list the fields the harness reads; the engine may return more.
ENGINE_METHODS = {
    "engine.info": {
        "params": "{}",
        "result": "engine_version, protocol, ir_version, abi_version, numeric_profile, runtime_mode, "
                  "transport, pid, capabilities[], active_worker, process",
        "source": "src/agent/su_agent.c h_engine_info",
    },
    "capsule.submit": {
        "params": "{capsule_path}  (creation-only; each accepted submit takes one of 16 candidate slots)",
        "result": "candidate_id, status, system_id, source_hash, capsule_hash, semantic_hash, artifact_format",
        "source": "src/agent/su_agent.c h_capsule_submit",
    },
    "candidate.status": {
        "params": "{candidate_id}",
        "result": "candidate_id, status, system_id, source_hash, capsule_hash, semantic_hash, "
                  "evaluation{status, cases_total, cases_passed}, worker_pid, install{state, txid, "
                  "effective_tick, error_status}",
        "source": "src/agent/su_agent.c h_candidate_status",
    },
    "candidate.evaluate": {
        "params": "{candidate_id, test_path, timeout_ms?}  (synchronous; no job id)",
        "result": "candidate_id, evaluation_id, status (passed|failed|worker_failed), cases_total, "
                  "cases_passed, failures[]",
        "source": "src/agent/su_agent.c h_candidate_evaluate",
    },
    "candidate.install": {
        "params": "{candidate_id, world_id, expected_world_revision, allowed_effective_tick}",
        "result": "world_id, candidate_id, txid, state, system_id, system_revision, effective_tick, "
                  "previous_txid, previous_semantic_hash, semantic_hash, world_revision, tick, worker_pid",
        "source": "src/agent/su_agent.c h_candidate_install, src/runtime/reload.c",
    },
    "world.create": {
        "params": "{fixture_path, capsule_path} or {fixture_path, capsule_paths: [1..8]}",
        "result": "world_id, world_revision, worker_pid, tick, row_count, schema_id, system_id, source_hash, "
                  "capsule_hash, semantic_hash, semantic_manifest[]",
        "source": "src/agent/su_agent.c h_world_create, src/runtime/worker.c worker_create",
    },
    "world.inspect": {
        "params": "{world_id, fields?: [names] (<=64), limit?: 1..256 (default 16), offset?: 0..}",
        "result": "world_id, world_revision, worker_pid, tick, row_count, offset, returned_rows, entity_ids[], "
                  "columns{name: [u32]}, semantic_manifest[], registry{}",
        "source": "src/agent/su_agent.c h_world_inspect, src/runtime/worker.c worker_inspect",
    },
    "world.step": {
        "params": "{world_id, expected_world_revision (required), ticks (0..100000)}",
        "result": "world_id, world_revision, ticks_completed, tick, worker_pid",
        "source": "src/agent/su_agent.c h_world_step, src/runtime/worker.c worker_step",
    },
}
# Not implemented by the M0 engine: routed to SU_E_UNSUPPORTED_METHOD, not advertised.
ENGINE_METHODS["world.snapshot"] = {
    "params": "{world_id, expected_world_revision}",
    "result": "snapshot_id, world_revision, tick, bytes, sha256, capture_ns (format field is inside the document)",
    "source": "src/runtime/worker.c, src/world/snapshot.c",
}
ENGINE_METHODS["world.restore"] = {
    "params": "{world_id, snapshot_id, expected_world_revision}",
    "result": "world_id, world_revision, tick, restored_from, worker_pid, semantic_manifest[]",
    "source": "src/runtime/worker.c h_world_restore, src/world/snapshot.c",
}

INSPECT_PAGE = 256            # engine maximum for world.inspect limit
INSPECT_FIELDS = ("position_x", "velocity_x")
# Harness-chosen bound, NOT an engine requirement: growth of VmRSS after warm-up, per process.
HARNESS_RSS_GROWTH_BOUND_KIB = 8192
EDIT_V1_TEST = "tests/fixtures/edit/movement.test.json"
EDIT_V2_TEST = "tests/fixtures/edit/movement_v2.test.json"
EDIT_V2_WRONG_TEST = "tests/fixtures/edit/movement_v2_wrong.test.json"


def call(tctx, sess, method, params, timeout=60.0) -> dict:
    return ok_result(tctx, method, sess.request(method, params, timeout))


def manifest_identity(manifest) -> list:
    """Content identity of each system: install-history fields (txid, system_revision,
    installed_tick) are excluded because every install legitimately changes them."""
    if not isinstance(manifest, list):
        return None
    return [{k: m.get(k) for k in ("system_id", "semantic_hash", "source_hash", "capsule_hash")}
            for m in manifest if isinstance(m, dict)]


def supervisor_pid(tctx, sess) -> int:
    """Supervisor pid as engine.info reports it (result.process.pid), checked against the OS pid."""
    proc = call(tctx, sess, "engine.info", {}).get("process")
    pid = proc.get("pid") if isinstance(proc, dict) else None
    if not isinstance(pid, int) or isinstance(pid, bool):
        raise Failed("engine.info lacks process.pid")
    tctx.check("engine.info pid is the serve process", sess.proc.pid, pid)
    return pid


def merge_pages(pages: list, fields, label="world.inspect") -> dict:
    """Merge world.inspect pages read with offset paging. Any inconsistency raises Failed."""
    if not pages:
        raise Failed(f"{label}: no pages read")
    first = pages[0]
    row_count = first.get("row_count")
    if not isinstance(row_count, int) or isinstance(row_count, bool) or row_count < 0:
        raise Failed(f"{label}: row_count is not a non-negative integer")
    columns = {f: [] for f in fields}
    entity_ids: list = []
    offset = 0
    for page in pages:
        for key in ("world_revision", "tick", "row_count"):
            if page.get(key) != first.get(key):
                raise Failed(f"{label}: page at offset {offset} changed {key}: "
                             f"{_short(first.get(key))} -> {_short(page.get(key))}")
        if page.get("offset") != offset:
            raise Failed(f"{label}: page reports offset {page.get('offset')}, expected {offset}")
        n = page.get("returned_rows")
        if not isinstance(n, int) or isinstance(n, bool) or n < 0:
            raise Failed(f"{label}: returned_rows is not a non-negative integer")
        ids = page.get("entity_ids")
        if not isinstance(ids, list) or len(ids) != n:
            raise Failed(f"{label}: entity_ids length does not equal returned_rows at offset {offset}")
        cols = page.get("columns")
        if not isinstance(cols, dict):
            raise Failed(f"{label}: columns missing at offset {offset}")
        for f in fields:
            vals = cols.get(f)
            if not isinstance(vals, list) or len(vals) != n:
                raise Failed(f"{label}: column {f} has wrong length at offset {offset}")
            columns[f].extend(vals)
        entity_ids.extend(ids)
        offset += n
    if offset != row_count:
        raise Failed(f"{label}: paged {offset} rows, row_count is {row_count}")
    merged = {k: first.get(k) for k in ("world_id", "world_revision", "tick", "row_count", "schema_id",
                                        "semantic_manifest", "worker_pid")}
    merged["columns"] = columns
    merged["entity_ids"] = entity_ids
    merged["returned_rows"] = offset
    merged["pages"] = len(pages)
    return merged


def canonical_state_bytes(state: dict) -> bytes:
    """Canonical serialisation of the observable state, for byte-for-byte comparison."""
    doc = {"tick": state.get("tick"), "row_count": state.get("row_count"),
           "entity_ids": state.get("entity_ids"), "columns": state.get("columns"),
           "semantic_manifest": state.get("semantic_manifest")}
    return json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def rss_verdict(name, baseline_kib, end_kib, bound_kib) -> list:
    if baseline_kib is None or end_kib is None:
        return [f"{name}: RSS not observable"]
    growth = end_kib - baseline_kib
    if growth > bound_kib:
        return [f"{name}: VmRSS grew {growth} KiB after warm-up (harness bound {bound_kib} KiB)"]
    return []


def latency_stats(seconds: list) -> dict:
    """Nearest-rank statistics in milliseconds. Reports only; no target is compared here."""
    if not seconds:
        return {"n": 0, "units": "ms"}
    s = sorted(seconds)
    n = len(s)

    def pick(p):
        return s[max(0, math.ceil(p * n) - 1)]

    return {"n": n, "units": "ms", "min": round(s[0] * 1000, 3), "median": round(pick(0.5) * 1000, 3),
            "p95": round(pick(0.95) * 1000, 3), "max": round(s[-1] * 1000, 3)}


def machine_info() -> dict:
    info = {"platform": platform.platform(), "machine": platform.machine(), "cpu_count": os.cpu_count(),
            "python": sys.version.split()[0], "clock": "time.monotonic (host)"}
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as fh:
            info["cpu_model"] = next((ln.split(":", 1)[1].strip() for ln in fh if ln.startswith("model name")), None)
    except OSError:
        info["cpu_model"] = None
    return info


def create_world(tctx, sess, world_rel, cap_rels):
    if len(cap_rels) == 1:
        params = {"fixture_path": world_rel, "capsule_path": cap_rels[0]}
    else:
        params = {"fixture_path": world_rel, "capsule_paths": list(cap_rels)}
    res = call(tctx, sess, "world.create", params)
    wid, rev = res.get("world_id"), res.get("world_revision")
    if not isinstance(wid, str) or not isinstance(rev, str):
        raise Failed("world.create result lacks world_id/world_revision strings")
    sess.revs[wid] = rev
    return wid, rev


def inspect(tctx, sess, wid) -> dict:
    """Read every row through world.inspect offset paging (limit 256) and merge the pages."""
    pages, offset = [], 0
    while True:
        res = call(tctx, sess, "world.inspect",
                   {"world_id": wid, "fields": list(INSPECT_FIELDS), "limit": INSPECT_PAGE, "offset": offset})
        pages.append(res)
        n = res.get("returned_rows")
        if not isinstance(n, int) or isinstance(n, bool):
            raise Failed("world.inspect result lacks returned_rows")
        offset += n
        if n == 0 or offset >= res.get("row_count", 0):
            break
        if len(pages) > 1 + 10_000_000 // INSPECT_PAGE:
            raise Failed("world.inspect paging did not terminate")
    return merge_pages(pages, INSPECT_FIELDS)


def step(tctx, sess, wid, ticks: int, expect=None) -> dict:
    expect = expect if expect is not None else sess.revs.get(wid)
    if not isinstance(expect, str):
        raise Failed("harness has no tracked world_revision for this world")
    res = call(tctx, sess, "world.step",
               {"world_id": wid, "expected_world_revision": expect, "ticks": ticks})
    if not isinstance(res.get("world_revision"), str):
        raise Failed("world.step result lacks world_revision")
    sess.revs[wid] = res["world_revision"]
    return res


def declared_source(tctx, cap_rel):
    """The engine binds a capsule to the source named by its own `source` field, resolved beside it."""
    cap = tctx.load(cap_rel)
    src = cap.get("source") if isinstance(cap, dict) else None
    if not isinstance(src, str):
        raise Failed(f"{cap_rel} has no string `source` field")
    return os.path.normpath(os.path.join(os.path.dirname(cap_rel), src)).replace(os.sep, "/")


def check_expressible(tctx, cap_rel, source_rel):
    """capsule.submit takes only capsule_path; a source other than the capsule's own cannot be sent."""
    declared = declared_source(tctx, cap_rel)
    if declared != source_rel:
        raise Unavailable(f"capsule.submit takes only capsule_path; {cap_rel} binds source {declared}, "
                          f"so {source_rel} cannot be submitted against it")


def submit(tctx, sess, cap_rel, source_rel) -> str:
    check_expressible(tctx, cap_rel, source_rel)
    res = call(tctx, sess, "capsule.submit", {"capsule_path": cap_rel})
    cid = res.get("candidate_id")
    if not isinstance(cid, str):
        raise Failed("capsule.submit result lacks candidate_id")
    return cid


def evaluate(tctx, sess, cid, test_rel) -> dict:
    return call(tctx, sess, "candidate.evaluate", {"candidate_id": cid, "test_path": test_rel})


def prepare(tctx, sess, cap_rel, source_rel, test_rel) -> str:
    """Submit a candidate and evaluate it. The evaluation must pass or the test fails."""
    cid = submit(tctx, sess, cap_rel, source_rel)
    ev = evaluate(tctx, sess, cid, test_rel)
    tctx.check(f"evaluation of {cid} passed", "passed", ev.get("status"))
    tctx.check(f"evaluation of {cid} ran every case", ev.get("cases_total"), ev.get("cases_passed"))
    return cid


def install(tctx, sess, wid, cid, rev, tick) -> dict:
    """candidate.install that must succeed; the world revision it returns is tracked for later steps."""
    res = call(tctx, sess, "candidate.install",
               {"world_id": wid, "candidate_id": cid, "expected_world_revision": rev,
                "allowed_effective_tick": tick})
    if isinstance(res.get("world_revision"), str):
        sess.revs[wid] = res["world_revision"]
    tctx.check("install state is installed", "installed", res.get("state"))
    return res


def install_expect(tctx, sess, wid, cid, rev, tick, code) -> None:
    """candidate.install that must be refused with `code`; the raw envelope is checked."""
    env = sess.request("candidate.install", {"world_id": wid, "candidate_id": cid,
                                             "expected_world_revision": rev,
                                             "allowed_effective_tick": tick})
    if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
        raise Unavailable("candidate.install answered SU_E_UNSUPPORTED_METHOD")
    tctx.check_mismatches(f"install refused with {code}", classify_error(env, code))


def drive(tctx, sess, world_rel, cap_rels, checkpoints, label):
    """Create a world, step to each checkpoint tick, and compare the state there.

    checkpoints: list of (tick, expected_columns) with non-decreasing ticks.
    """
    wid, _rev = create_world(tctx, sess, world_rel, cap_rels)
    current = 0
    for tick, expected in checkpoints:
        if tick > current:
            step(tctx, sess, wid, tick - current)
            current = tick
        state = inspect(tctx, sess, wid)
        tctx.check_mismatches(f"{label} tick {tick}", verify_state(tick, expected, state))
    return wid


def fixture_consistency(tctx, label, derived, fixture_columns):
    tctx.check_mismatches(f"fixture expectation {label} agrees with reference evaluator",
                          compare_columns(derived, fixture_columns))


# ------------------------------------------------------------------ test bodies

def stable_version_fields(fields: dict) -> dict:
    """Version fields that identify a build. `pid` is the pid of the --version process, so it is excluded."""
    return {k: v for k, v in fields.items() if k != "pid"}


def _tool_line(out: str) -> str:
    return next((ln.strip() for ln in out.splitlines() if ln.strip()), "")


def t00(t: TestCtx):
    """Clean bootstrap (TEST_SPEC T00). Never incremental: a fresh build tree outside the repository is
    removed if present, configured (Debug, the dev profile's flags) and built. The clean binary's version
    must match the headers, and the binary under test must report the same version as the clean build.
    The binary under test is the one every other test runs; with --no-build T00 is not run."""
    if t.run.no_build:
        raise NotRun("build skipped by --no-build; the clean bootstrap was not run in this invocation")
    t.need_file("CMakeLists.txt")
    gen = "Ninja" if shutil.which("ninja") is not None else None
    root = tempfile.mkdtemp(prefix="m0-clean-build-")  # outside the repository
    bdir = os.path.join(root, "build")
    if os.path.lexists(bdir):
        shutil.rmtree(bdir)  # rm -rf the scratch build tree
    tools = {}
    _rc, out_c = t.run_cmd(["cmake", "--version"], timeout=60)
    tools["cmake"] = _tool_line(out_c)
    if gen:
        _rc, out_n = t.run_cmd(["ninja", "--version"], timeout=60)
        tools["ninja"] = _tool_line(out_n)
    cc = shutil.which("cc")
    if cc:
        _rc, out_cc = t.run_cmd([cc, "--version"], timeout=60)
        tools["compiler"] = _tool_line(out_cc)
    t.details["tool_versions"] = tools
    argv = ["cmake", "-S", REPO, "-B", bdir, "-DCMAKE_BUILD_TYPE=Debug"] + (["-G", gen] if gen else [])
    rc_cfg, _ = t.run_cmd(argv, timeout=900)
    t.check("clean configure returncode", 0, rc_cfg)
    rc_bld, _ = t.run_cmd(["cmake", "--build", bdir, "--parallel"], timeout=1800)
    t.check("clean build returncode", 0, rc_bld)
    clean_bin = os.path.join(bdir, "sutekh")
    if not os.path.isfile(clean_bin):
        raise Failed(f"clean build produced no binary at {clean_bin}")
    clean_sha = sha256_file(clean_bin)
    t.inputs[os.path.relpath(clean_bin, root)] = clean_sha
    rc_v, out_v = t.run_cmd([clean_bin, "--version"], timeout=30)
    t.check("clean --version returncode", 0, rc_v)
    fields = parse_version_output(out_v)
    t.check("abi_version equals include/sutekh/abi.h", str(read_header_constant("include/sutekh/abi.h", "SU_ABI_VERSION")),
            fields.get("abi_version"))
    t.check("ir_version equals include/sutekh/ir.h", read_header_constant("include/sutekh/ir.h", "SU_IR_VERSION_STRING"),
            fields.get("ir_version"))
    t.check("artifact_format equals include/sutekh/ir.h",
            str(read_header_constant("include/sutekh/ir.h", "SU_ARTIFACT_FORMAT_VERSION")),
            fields.get("artifact_format"))
    t.check("sutekh version line present", True, fields.get("sutekh") is not None)
    binary = t.binary
    if not os.path.isfile(binary):
        raise Failed(f"binary under test not found: {binary}")
    rc_u, out_u = t.run_cmd([binary, "--version"], timeout=30)
    t.check("binary under test --version returncode", 0, rc_u)
    under = parse_version_output(out_u)
    # `pid` is the pid of the --version process itself, so it differs on every run and is excluded.
    t.check("binary under test reports the same version fields as the clean build (pid excluded)",
            stable_version_fields(fields), stable_version_fields(under))
    t.details["clean_build"] = {"root": root, "build_dir": bdir, "generator": gen or "default",
                                "configure_rc": rc_cfg, "build_rc": rc_bld,
                                "binary": {"path": clean_bin, "sha256": clean_sha},
                                "version_output": out_v.strip()}
    t.details["binary"] = {"path": t.rel(binary) if binary.startswith(REPO) else binary,
                           "sha256": sha256_file(binary)}
    t.inputs[t.rel(binary) if binary.startswith(REPO) else binary] = t.details["binary"]["sha256"]
    t.details["version_output"] = out_u.strip()


def t01(t: TestCtx):
    sess = t.session("T01 serve")
    world_rel = "examples/world.json"
    cap_rel = "examples/movement.capsule.json"
    world_doc = t.load(world_rel)
    cases_doc = t.load("examples/movement.test.json")
    cap, lir_doc, _ = t.load_system(cap_rel)
    initial = world_doc["columns"]
    checkpoints = []
    for case in cases_doc["cases"]:
        derived = ref.run_ticks(lir_doc, cap, initial, case["ticks"])
        fixture_consistency(t, case["name"], derived, case["expected_columns"])
        checkpoints.append((case["ticks"], derived))
    checkpoints.sort(key=lambda x: x[0])
    drive(t, sess, world_rel, [cap_rel], checkpoints, "T01 movement")


def _t02_or_t03_case(t, sess, world_rel, cap_rel, cases, lir_doc, cap):
    initial = t.load(world_rel)["columns"]
    for case in cases:
        derived = ref.run_ticks(lir_doc, cap, initial, case["ticks"])
        fixture_consistency(t, case["name"], derived, case["expected_columns"])
        drive(t, sess, world_rel, [cap_rel], [(case["ticks"], derived)], case["name"])


def t02(t: TestCtx):
    cap_rel = "tests/fixtures/t02/movement.capsule.json"
    cap, lir_doc, _ = t.load_system(cap_rel)
    counts = [0, 1, 7, 8, 9, 1024]
    seen = {}
    for n in counts:
        # A worker holds at most 4 worlds (worker.c), so each world size gets its own serve process.
        sess = t.session(f"T02 serve world_{n}")
        world_rel = f"tests/fixtures/t02/world_{n}.json"
        test_doc = t.load(f"tests/fixtures/t02/movement_{n}.test.json")
        if len(t.load(world_rel)["columns"]["position_x"]) != n:
            raise Failed(f"fixture world_{n} row count is not {n}")
        _t02_or_t03_case(t, sess, world_rel, cap_rel, test_doc["cases"], lir_doc, cap)
        seen[n] = [c["ticks"] for c in test_doc["cases"]]
        sess.close()
    t.details["row_counts_checked_all_rows"] = seen


def t03(t: TestCtx):
    base = "tests/fixtures/t03"
    names = sorted(d for d in os.listdir(t.abspath(base)) if os.path.isdir(t.abspath(os.path.join(base, d))))
    if not names:
        raise NotRun("no t03 cases present")
    checked = []
    for name in names:
        d = f"{base}/{name}"
        cap_rel, world_rel = f"{d}/capsule.json", f"{d}/world.json"
        cap, lir_doc, _ = t.load_system(cap_rel)
        test_doc = t.load(f"{d}/test.json")
        sess = t.session(f"T03 serve {name}")
        _t02_or_t03_case(t, sess, world_rel, cap_rel, test_doc["cases"], lir_doc, cap)
        sess.close()
        checked.append(name)
    t.details["cases_checked"] = checked


def _submit_expect_rejection(t, sess, cap_rel, source_rel, expected_code):
    """Submit a candidate that must be rejected with expected_code; return (rejected, envelope)."""
    check_expressible(t, cap_rel, source_rel)
    env = sess.request("capsule.submit", {"capsule_path": cap_rel})
    if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
        raise Unavailable("capsule.submit answered SU_E_UNSUPPORTED_METHOD")
    if env["ok"]:
        return False, env
    t.check_mismatches(f"rejection {expected_code} for {cap_rel}", classify_error(env, expected_code))
    return True, env


NEG_MANIFESTS = ["tests/fixtures/negative_cases.json", "examples/negative_cases.json",
                 "tests/fixtures/m0_neg/manifest.json"]
INVALID_ARGUMENT = "SU_E_INVALID_ARGUMENT"
CANDIDATE_PROBE_MAX = 20  # AGENT_MAX_CANDIDATES is 16; ids are candidate-<n> from a counter
# Codes the T04 matrix must exercise at least once (TEST_SPEC T04 and the M0-12 audit list).
T04_REQUIRED_CODES = ("SU_E_RESOURCE_LIMIT", "SU_E_PARSE", "SU_E_TYPE", "SU_E_DUPLICATE_KEY",
                      "SU_E_UNDECLARED_READ", "SU_E_UNDECLARED_WRITE", "SU_E_SCHEMA_MISMATCH",
                      "SU_E_MISSING_STORE")


def _negative_cases(t) -> list:
    """Every negative case from all manifests as (capsule_rel, source_rel, expected_code, manifest_rel, stage).
    Paths resolve against the manifest's directory. Identical (capsule, source) pairs are deduplicated;
    the same pair with two different expected codes is a manifest error. stage is None when the manifest
    does not name one."""
    seen, out = {}, []
    for rel in NEG_MANIFESTS:
        doc = t.load(rel)
        base_dir = os.path.dirname(rel)
        for case in doc["cases"]:
            cap_rel = os.path.normpath(os.path.join(base_dir, case.get("capsule", doc["capsule"]))).replace(os.sep, "/")
            src_rel = os.path.normpath(os.path.join(base_dir, case["path"])).replace(os.sep, "/")
            key = (cap_rel, src_rel)
            if key in seen:
                if seen[key] != case["expected_code"]:
                    raise Failed(f"manifest conflict: {src_rel} expects {seen[key]} and {case['expected_code']}")
                continue
            seen[key] = case["expected_code"]
            stage = case.get("stage")
            if stage is not None and not stage_code_consistent(stage, case["expected_code"]):
                raise Failed(f"manifest {rel}: {case['id']} stage {stage} cannot report {case['expected_code']}")
            out.append((cap_rel, src_rel, case["expected_code"], rel, stage))
    return out


def _candidate_probe(sess) -> list:
    """Existing candidate numbers among candidate-1..candidate-CANDIDATE_PROBE_MAX, via candidate.status."""
    present = []
    for k in range(1, CANDIDATE_PROBE_MAX + 1):
        env = sess.request("candidate.status", {"candidate_id": f"candidate-{k}"})
        if env.get("ok") is True:
            present.append(k)
        elif env.get("error", {}).get("code") != INVALID_ARGUMENT:
            raise Failed(f"candidate.status candidate-{k} returned unexpected {env.get('error')}")
    return present


def t04(t: TestCtx):
    sess = t.session("T04 serve")
    world_rel = "tests/fixtures/t02/world_1.json"
    base_cap = "tests/fixtures/t02/movement.capsule.json"
    wid, _rev = create_world(t, sess, world_rel, [base_cap])
    before = inspect(t, sess, wid)
    before_snapshot = {"world_revision": before["world_revision"], "tick": before["tick"],
                       "columns": before["columns"]}
    # Control: the valid movement candidate must be accepted, so "reject everything" cannot pass.
    env = sess.request("capsule.submit", {"capsule_path": base_cap})
    if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
        raise Unavailable("capsule.submit answered SU_E_UNSUPPORTED_METHOD")
    t.check("control: valid movement candidate accepted", True, env["ok"])
    if env["ok"] is not True:
        raise Failed(f"control submit rejected: {env.get('error')}")
    known = _candidate_probe(sess)
    if known != [int(env["result"]["candidate_id"].split("-")[1])]:
        raise Failed(f"control submit: expected exactly the new candidate, probe found {known}")
    results, failures = [], []
    cases = _negative_cases(t)
    # Coverage guard: the matrix must contain every required code and every verifier stage 1-7 that
    # the manifests name. A manifest that loses a case is a failed check, not a silent smaller matrix.
    codes = {code for _c, _s, code, _m, _st in cases}
    t.check_mismatches("T04 matrix covers every required code",
                       [f"missing {c}" for c in T04_REQUIRED_CODES if c not in codes])
    t.check_mismatches("T04 matrix names verifier stages 1 through 7",
                       [f"missing stage {s}" for s in range(1, 8)
                        if s not in {st for _c, _s, _k, _m, st in cases if st is not None}])
    for cap_rel, src_rel, code, manifest_rel, stage in cases:
        t.need_file(cap_rel)
        t.need_file(src_rel)
        # source_path is project-root relative and replaces the capsule's own `source` binding.
        env = sess.request("capsule.submit", {"capsule_path": cap_rel, "source_path": src_rel})
        if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
            raise Unavailable("capsule.submit answered SU_E_UNSUPPORTED_METHOD")
        mism = classify_error(env, code)
        after_probe = _candidate_probe(sess)
        if after_probe != known:
            mism.append(f"candidate set changed from {known} to {after_probe}")
        after = inspect(t, sess, wid)
        mism += verify_rejections_leave_state(before_snapshot, after)
        actual = "accepted" if env["ok"] else env["error"].get("code")
        t._record({"name": f"negative {src_rel} -> {code}", "mismatches": mism[:5], "ok": not mism})
        results.append({"capsule": cap_rel, "source": src_rel, "manifest": manifest_rel, "stage": stage,
                        "expected": code, "actual": actual, "ok": not mism})
        if mism:
            failures.append(f"FAILED {src_rel} (capsule {cap_rel}): expected {code}, actual {actual}; "
                            + "; ".join(mism[:2]))
    # Protocol-level variant: one byte over the 1 MiB message limit, shaped as a capsule.submit line.
    # Rejected before any method runs, so the world and candidate set must be unchanged.
    raw = make_oversize_line(MAX_MESSAGE + 1, method="capsule.submit")
    t.check("oversize capsule.submit line is one byte over the limit", MAX_MESSAGE + 1, len(raw))
    sess.send(raw + b"\n")
    reply = sess.read_message(timeout=15)
    if reply is None:
        raise Failed("oversize protocol line: engine closed without a parseable reply")
    env = parse_envelope(reply, None)
    mism = classify_error(env, "SU_E_RESOURCE_LIMIT")
    after_probe = _candidate_probe(sess)
    if after_probe != known:
        mism.append(f"candidate set changed from {known} to {after_probe}")
    mism += verify_rejections_leave_state(before_snapshot, inspect(t, sess, wid))
    t.check_mismatches("oversize protocol line rejected as SU_E_RESOURCE_LIMIT with no state change", mism)
    results.append({"capsule": None, "source": "protocol line (capsule.submit, 1 MiB + 1 byte)", "manifest": None,
                    "stage": 1, "expected": "SU_E_RESOURCE_LIMIT",
                    "actual": "accepted" if env["ok"] else env["error"].get("code"), "ok": not mism})
    t.check("session answers after the oversize line", True, sess.request("engine.info", {})["ok"])
    t.details["negative_cases"] = results
    t.details["negative_case_count"] = len(results)
    if failures:
        raise Failed(f"{len(failures)} of {len(results)} negative cases not rejected as expected: "
                     + " | ".join(failures[:6]))


def _find_candidate(t, manifest_rel, cid):
    manifest = t.load(manifest_rel)
    for item in manifest:
        if item["id"] == cid:
            cap_rel = os.path.normpath(os.path.join(os.path.dirname(manifest_rel), item["capsule"])).replace(os.sep, "/")
            cap, _lir, src_rel = t.load_system(cap_rel)
            return item, cap_rel, src_rel
    raise Failed(f"candidate {cid} absent from {manifest_rel}")


EDIT_WORLD = "tests/fixtures/edit/world.json"
EDIT_V1 = "tests/fixtures/edit/movement.capsule.json"
EDIT_V2 = "tests/fixtures/edit/movement_v2.capsule.json"
EDIT_SCHED = "tests/fixtures/edit/swap_schedule.json"
EDIT_CANDS = "tests/fixtures/edit/candidates/manifest.json"
V1_SRC = "tests/fixtures/edit/movement.lir.json"
V2_SRC = "tests/fixtures/edit/movement_v2.lir.json"


def _swap_expected(t):
    sched = t.load(EDIT_SCHED)
    expected = {int(k): v["columns"] for k, v in sched["expected_by_tick"].items()}
    return sched, expected


def t05(t: TestCtx):
    """Capsule-only swap: submit -> evaluate (pass) -> install at tick 2; behaviour changes at tick 2."""
    sess = t.session("T05 serve")
    sched, expected = _swap_expected(t)
    swap_tick = sched["swap"]["effective_tick"]
    v1_lir, v1_cap = t.load(V1_SRC), t.load(EDIT_V1)
    v2_lir, v2_cap = t.load(V2_SRC), t.load(EDIT_V2)
    pid0 = supervisor_pid(t, sess)
    wid, _rev0 = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    t.check_mismatches("T05 tick 0", verify_state(0, expected[0], inspect(t, sess, wid)))
    step(t, sess, wid, swap_tick)
    pre = inspect(t, sess, wid)
    t.check_mismatches(f"T05 v1 at effective tick {swap_tick}", verify_state(swap_tick, expected[swap_tick], pre))
    worker_pid = pre.get("worker_pid")
    t.check("worker pid reported before install", True, isinstance(worker_pid, int))
    # OS facts, not the engine's word: the reported worker is a live child of the supervisor, and its
    # (pid, start time, parent) identity must stay the same through install and every later tick.
    sup_id0 = identity_key(os_identity(pid0))
    w_id0 = identity_key(os_identity(worker_pid))
    t.check("T05 reported worker is a live child of the supervisor (OS ppid)", True,
            w_id0 is not None and w_id0[2] == pid0)
    cid = prepare(t, sess, EDIT_V2, V2_SRC, EDIT_V2_TEST)
    install(t, sess, wid, cid, pre["world_revision"], swap_tick)
    mid = inspect(t, sess, wid)
    t.check_mismatches("T05 install does not step or change the world",
                       verify_state(swap_tick, expected[swap_tick], mid))
    t.check("T05 install leaves every column unchanged", pre["columns"], mid["columns"])
    t.check("T05 semantic manifest changes at install", True, mid.get("semantic_manifest") != pre.get("semantic_manifest"))
    t.check("T05 worker pid unchanged by install", worker_pid, mid.get("worker_pid"))
    t.check("T05 worker OS identity unchanged by install", w_id0, identity_key(os_identity(worker_pid)))
    # The swap must be observable: v1 would give different values at the first post-swap tick.
    v1_next = ref.run_tick(v1_lir, v1_cap, expected[swap_tick])
    t.check("T05 v1 and v2 differ at the first post-swap tick", True, v1_next != expected[swap_tick + 1])
    for tick in range(swap_tick + 1, sched["total_ticks"] + 1):
        step(t, sess, wid, 1)
        st = inspect(t, sess, wid)
        t.check_mismatches(f"T05 tick {tick} (v2 from effective tick {swap_tick})",
                           verify_state(tick, expected[tick], st))
    # Re-derive the post-swap tick from the reference evaluator as a cross-check on the fixture.
    t.check_mismatches("T05 fixture tick 3 equals v2 applied to tick 2",
                       compare_columns(ref.run_tick(v2_lir, v2_cap, expected[swap_tick]), expected[swap_tick + 1]))
    post = inspect(t, sess, wid)
    t.check("T05 supervisor pid preserved", pid0, supervisor_pid(t, sess))
    t.check("T05 worker pid preserved", worker_pid, post.get("worker_pid"))
    t.check("T05 supervisor OS identity preserved", sup_id0, identity_key(os_identity(pid0)))
    t.check("T05 worker OS identity preserved", w_id0, identity_key(os_identity(worker_pid)))


def t06(t: TestCtx):
    """Invalid and failing candidates are refused; committed state and active semantic hash are unchanged."""
    sess = t.session("T06 serve")
    sched, expected = _swap_expected(t)
    swap_tick = sched["swap"]["effective_tick"]
    v1_lir, v1_cap = t.load(V1_SRC), t.load(EDIT_V1)
    wid, _rev = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, swap_tick)
    pre = inspect(t, sess, wid)
    # (a) capsule rejected at submission: it never becomes a candidate, so it cannot be installed.
    item, cap_rel, src_rel = _find_candidate(t, EDIT_CANDS, "wrong_permission")
    rejected, _env = _submit_expect_rejection(t, sess, cap_rel, src_rel, item["expected_code"])
    if not rejected:
        raise Failed("wrong_permission candidate was accepted by capsule.submit")
    # (b) valid capsule whose evaluation fails: install must refuse it.
    cid = submit(t, sess, EDIT_V2, V2_SRC)
    ev = evaluate(t, sess, cid, EDIT_V2_WRONG_TEST)
    t.check("evaluation with a wrong expectation reports failed", "failed", ev.get("status"))
    install_expect(t, sess, wid, cid, pre["world_revision"], swap_tick, "SU_E_CANDIDATE_FAILED")
    st = call(t, sess, "candidate.status", {"candidate_id": cid})
    t.check("failed candidate is not installed", True, st.get("install", {}).get("state") != "installed")
    post = inspect(t, sess, wid)
    t.check_mismatches("failed swap leaves committed state unchanged",
                       verify_rejections_leave_state(pre, post))
    t.check("active semantic manifest unchanged", pre.get("semantic_manifest"), post.get("semantic_manifest"))
    v1_next = ref.run_tick(v1_lir, v1_cap, expected[swap_tick])
    step(t, sess, wid, 1)
    t.check_mismatches("active code is still v1 after failed swap",
                       verify_state(swap_tick + 1, v1_next, inspect(t, sess, wid)))


def t07(t: TestCtx):
    """Stale candidate: a valid candidate installed against a revision captured before a mutation."""
    sess = t.session("T07 serve")
    item, cap_rel, src_rel = _find_candidate(t, EDIT_CANDS, "stale_revision")
    wid, rev0 = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, 1)
    pre = inspect(t, sess, wid)
    if pre["world_revision"] == rev0:
        raise NotRun("engine did not change world_revision on a tick; a stale expectation cannot be constructed")
    if item["expected_world_revision"] != "@captured_before_mutation":
        raise Failed("manifest stale case does not name the captured-revision placeholder")
    cid = prepare(t, sess, cap_rel, src_rel, EDIT_V2_TEST)
    install_expect(t, sess, wid, cid, rev0, pre["tick"], item["expected_code"])
    post = inspect(t, sess, wid)
    t.check_mismatches("stale rejection leaves state unchanged", verify_rejections_leave_state(pre, post))
    t.check("stale rejection leaves semantic manifest unchanged",
            pre.get("semantic_manifest"), post.get("semantic_manifest"))
    # Control: the same candidate with the current revision installs at the current tick.
    install(t, sess, wid, cid, pre["world_revision"], pre["tick"])
    t.check("control install at current revision leaves tick unchanged", pre["tick"],
            inspect(t, sess, wid)["tick"])


def t08(t: TestCtx):
    sess = t.session("T08 serve")
    sched, expected = _swap_expected(t)
    wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, 2)
    at2 = inspect(t, sess, wid)
    t.check_mismatches("T08 state at tick 2", verify_state(2, expected[2], at2))
    snap = call(t, sess, "world.snapshot", {"world_id": wid, "expected_world_revision": at2["world_revision"]})
    sid = snap.get("snapshot_id")
    if not isinstance(sid, str):
        raise Failed("world.snapshot result lacks snapshot_id")
    # The snapshot reply is a summary (snapshot_id, world_revision, tick, bytes, sha256). Its format field
    # lives in the snapshot document, which no control-plane method returns, so it cannot be compared here.
    # That gap is recorded in details, not hidden by an optional check.
    t.check("snapshot reply names the revision and tick it was taken at",
            [at2["world_revision"], 2], [snap.get("world_revision"), snap.get("tick")])
    t.check("snapshot reply is self-consistent (snapshot_id equals its sha256 field)", sid, snap.get("sha256"))
    t.check("snapshot document has a positive byte length", True,
            type(snap.get("bytes")) is int and snap["bytes"] > 0)
    t.details["snapshot_format_unverified"] = ("snapshot_format is written inside the snapshot document "
                                               "(src/world/snapshot.c) and no RPC returns the document, so the "
                                               "format is not compared by the control-plane harness")
    step(t, sess, wid, 2)
    later = inspect(t, sess, wid)
    v1 = t.load(EDIT_V1)
    v1_lir = t.load("tests/fixtures/edit/movement.lir.json")
    t.check_mismatches("T08 state at tick 4 before restore",
                       verify_state(4, ref.run_ticks(v1_lir, v1, expected[2], 2), later))
    res = call(t, sess, "world.restore", {"world_id": wid, "snapshot_id": sid,
                                          "expected_world_revision": later["world_revision"]})
    t.check("restore assigns a new revision", True, res.get("world_revision") != later["world_revision"])
    t.check("restored_from names the snapshot", sid, res.get("restored_from"))
    restored = inspect(t, sess, wid)
    t.check_mismatches("T08 restored tick and columns", verify_state(2, expected[2], restored))
    t.check("T08 semantic manifest identical to snapshot-time manifest", at2.get("semantic_manifest"),
            restored.get("semantic_manifest"))
    t.check("restored revision matches restore result", res.get("world_revision"), restored["world_revision"])


def t09(t: TestCtx):
    """Multi-system world with a failing second system.

    Admission part (real binary): a second system that fails verification must refuse world.create.
    Runtime part (real binary): system B exceeds its own limits.max_rows during world.step; the step
    must fail with the manifest's code and the committed state must be unchanged (no publish).
    """
    manifest = t.load("tests/fixtures/t09/manifest.json")
    systems = [f"tests/fixtures/t09/{s}" for s in manifest["systems_in_order"]]
    for s in systems:
        t.need_file(s)
    world_rel = "tests/fixtures/t09/world.json"
    t.need_file(world_rel)
    sess = t.session("T09 serve")
    # Admission: system B is replaced by a capsule that fails verification.
    bad_item, bad_cap, _bad_src = _find_candidate(t, EDIT_CANDS, "wrong_permission")
    env = sess.request("world.create", {"fixture_path": world_rel, "capsule_paths": [systems[0], bad_cap]})
    if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
        raise Unavailable("world.create answered SU_E_UNSUPPORTED_METHOD")
    t.check_mismatches("multi-system admission refuses a failing second system",
                       classify_error(env, bad_item["expected_code"]))
    t.check("session answers after refused multi-system create", True, sess.request("engine.info", {})["ok"])
    # Runtime: system A succeeds into staging, system B exceeds its own limits.max_rows. The step must
    # fail, and the published (committed) root must be exactly what it was before the step.
    wid, rev = create_world(t, sess, world_rel, systems)
    before = inspect(t, sess, wid)
    t.check("T09 starts at tick 0", 0, before["tick"])
    env = sess.request("world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": 1})
    if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
        raise Unavailable("world.step answered SU_E_UNSUPPORTED_METHOD")
    t.check("failing second system makes world.step fail", True, env["ok"] is False)
    t.check_mismatches("step failure code", classify_error(env, manifest["expected_failure_code"]))
    after = inspect(t, sess, wid)
    t.check("tick not published", manifest["expected_tick_after_failed_step"], after["tick"])
    t.check("committed position_x unchanged by A's staged write",
            manifest["expected_columns_after_failed_step"]["position_x"], after["columns"]["position_x"])
    t.check("committed velocity_x unchanged",
            manifest["expected_columns_after_failed_step"]["velocity_x"], after["columns"]["velocity_x"])
    t.check("columns identical to pre-step inspection", before["columns"], after["columns"])
    t.check("world_revision unchanged", before.get("world_revision"), after.get("world_revision"))
    t.check("session still answers", True, sess.request("engine.info", {})["ok"])


def _children_fast(pid) -> set:
    """Child pids of a process, from /proc/<pid>/task/<tid>/children (one small read per thread)."""
    kids: set = set()
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return kids
    for tid in tids:
        try:
            with open(f"/proc/{pid}/task/{tid}/children", encoding="utf-8") as fh:
                kids.update(int(x) for x in fh.read().split())
        except (OSError, ValueError):
            continue
    return kids


T10_ATTEMPTS = 12


def _comm(pid):
    try:
        with open(f"/proc/{pid}/comm", encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return None


def t10(t: TestCtx):
    """Kill the disposable evaluation worker while candidate.evaluate is in flight.

    candidate.evaluate is synchronous, so the kill is issued from a harness thread that watches the
    supervisor's children while the request is outstanding. The evaluation is only a few milliseconds
    long, so an attempt can lose the race (the reply is already written). Such attempts are recorded
    and retried in a fresh serve process, up to T10_ATTEMPTS. A success requires a kill that landed
    while the evaluation was in flight (reply is an error).
    """
    if not os.path.isdir("/proc/self"):
        raise NotRun("no /proc: child processes cannot be observed or signalled")
    attempts = []
    found = None
    for attempt in range(T10_ATTEMPTS):
        sess = t.session(f"T10 serve attempt {attempt}")
        sup = supervisor_pid(t, sess)
        wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
        step(t, sess, wid, 1)
        pre = inspect(t, sess, wid)
        active = pre.get("worker_pid")
        if not isinstance(active, int):
            raise Failed("world.inspect lacks worker_pid")
        cid = submit(t, sess, EDIT_V2, V2_SRC)
        killed: list = []
        # pid -> OS identity read just before the signal (a victim may be reaped later); the string keys hold
        # the supervisor and active worker identities taken before the evaluation request.
        meta: dict = {"sup": identity_key(os_identity(sup)), "active": identity_key(os_identity(active))}
        stop = threading.Event()

        def killer(sup=sup, active=active, killed=killed, meta=meta, stop=stop):
            while not stop.is_set():
                for pid in _children_fast(sup):
                    if pid != active and pid not in killed:
                        ident = os_identity(pid)
                        try:
                            os.kill(pid, signal.SIGKILL)
                            killed.append(pid)
                            meta[pid] = ident
                        except OSError:
                            pass
                time.sleep(0.00005)

        th = threading.Thread(target=killer, daemon=True)
        th.start()
        try:
            env = sess.request("candidate.evaluate", {"candidate_id": cid, "test_path": EDIT_V2_TEST})
        finally:
            stop.set()
            th.join(2)
        attempts.append({"attempt": attempt, "killed": list(killed), "reply_ok": env["ok"],
                         "reply_code": None if env["ok"] else env["error"].get("code")})
        if killed and not env["ok"]:
            found = (sess, sup, wid, pre, active, cid, env, list(killed), dict(meta))
            break
        sess.close()
    t.details["attempts"] = attempts
    if found is None:
        raise NotRun(f"in {len(attempts)} attempts no kill landed while candidate.evaluate was in flight "
                     "(each attempt either saw no child or killed the worker after its reply); no isolation "
                     "result is claimed")
    sess, sup, wid, pre, active, cid, env, killed, meta = found
    t.details["kill_observation"] = {"supervisor_pid": sup, "active_worker_pid": active, "killed": killed}
    # OS identities: victims as read just before SIGKILL; supervisor and active worker before and after.
    killed_ids = [identity_key(meta.get(p)) for p in killed]
    sup_id, active_id = meta.get("sup"), meta.get("active")
    t.check("killed processes were children of the supervisor (OS ppid)", True,
            all(k is not None and k[2] == sup for k in killed_ids))
    t.check("supervisor and active worker are live before the kill (OS facts)", True,
            sup_id is not None and active_id is not None and active_id[2] == sup)
    t.check_mismatches("evaluation after worker kill is SU_E_WORKER_FAILED",
                       classify_error(env, "SU_E_WORKER_FAILED"))
    st = call(t, sess, "candidate.status", {"candidate_id": cid})
    t.check("killed pid is the evaluation worker reported by candidate.status", True,
            st.get("worker_pid") in killed)
    t.check("evaluation state after kill is worker_failed", "worker_failed", st.get("evaluation", {}).get("status"))
    t.check("killed processes are dead (OS facts)", True, all(is_dead(os_identity(p)) for p in killed))
    t.check("supervisor answers after kill", sup, supervisor_pid(t, sess))
    t.check("supervisor OS identity unchanged by kill", sup_id, identity_key(os_identity(sup)))
    post = inspect(t, sess, wid)
    t.check_mismatches("active world state unchanged by evaluation kill", verify_rejections_leave_state(pre, post))
    t.check("active worker pid unchanged by evaluation kill", active, post.get("worker_pid"))
    t.check("active worker OS identity unchanged by evaluation kill", active_id,
            identity_key(os_identity(active)))
    t.check("active semantic manifest unchanged", pre.get("semantic_manifest"), post.get("semantic_manifest"))
    v1_lir, v1_cap = t.load(V1_SRC), t.load(EDIT_V1)
    step(t, sess, wid, 1)
    t.check_mismatches("world still steps with v1 after the kill",
                       verify_state(2, ref.run_tick(v1_lir, v1_cap, pre["columns"]), inspect(t, sess, wid)))


def _rss_kib(pid):
    path = f"/proc/{pid}/status"
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return next((int(line.split()[1]) for line in fh if line.startswith("VmRSS:")), None)


T11_CYCLE = ("install", "revert", "fail_stale_revision", "fail_past_tick", "fail_layout_generation")
T11_SUCCESS = ("install", "revert")
T11_EXPECT = {"fail_stale_revision": "SU_E_STALE_REVISION",
              "fail_past_tick": "SU_E_STALE_REVISION",
              "fail_layout_generation": "SU_E_SCHEMA_MISMATCH"}
LAYOUT_CAP = "tests/fixtures/m0_neg/layout_gen2.capsule.json"
LAYOUT_SRC = "tests/fixtures/m0_neg/layout_gen2.lir.json"
LAYOUT_TEST = "tests/fixtures/m0_neg/layout_gen2.test.json"


def _raw_install(sess, wid, cid, rev, tick):
    env = sess.request("candidate.install", {"world_id": wid, "candidate_id": cid,
                                             "expected_world_revision": rev, "allowed_effective_tick": tick})
    if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
        raise Unavailable("candidate.install answered SU_E_UNSUPPORTED_METHOD")
    return env


def t11(t: TestCtx):
    """Repeated reload (TEST_SPEC T11): 1,000 iterations by default, in a fixed five-kind cycle.

      install               successful install of the other version (v1 <-> v2), then one tick
      revert                successful install back to the previous version (install v2, re-install v1)
      fail_stale_revision   install with the world revision from before the previous iteration -> SU_E_STALE_REVISION
      fail_past_tick        install with allowed_effective_tick one behind the world -> SU_E_STALE_REVISION
                            (src/runtime/reload.c requires allowed_effective_tick == current tick)
      fail_layout_generation a candidate with layout_generation 2 that evaluates correctly -> SU_E_SCHEMA_MISMATCH

    Every iteration also submits an invalid capsule (refused at submission). After each kind the state is
    verified: a refused install leaves tick, revision, columns and manifest unchanged; a successful install
    leaves tick and columns unchanged and makes the manifest the active version's. After each tick the
    columns and manifest match the reference evaluator run with the active version.

    RSS (harness-chosen bound, not an engine requirement): median VmRSS of the warm-up window
    [iters/5, iters/5 + iters/10) is compared with the median of the final tenth. Under AddressSanitizer
    the allocator quarantine (256 MB by default) grows RSS for reasons unrelated to the engine, so the
    engine process is started with quarantine_size_mb=1 when the binary is ASan-instrumented.
    """
    sanitizers = binary_sanitizers(t.binary)
    sess_env = with_asan_quarantine(t.env) if "asan" in sanitizers else None
    sess = t.session("T11 serve", env=sess_env)
    iters = t.run.reload_iterations
    sup = supervisor_pid(t, sess)
    sup_id0 = identity_key(os_identity(sup))
    wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    st = inspect(t, sess, wid)
    worker0 = st.get("worker_pid")
    w_id0 = identity_key(os_identity(worker0))
    if _rss_kib(sup) is None or _rss_kib(worker0) is None:
        raise NotRun("process RSS not observable (no /proc/<pid>/status)")
    v1_cid = prepare(t, sess, EDIT_V1, V1_SRC, EDIT_V1_TEST)
    v2_cid = prepare(t, sess, EDIT_V2, V2_SRC, EDIT_V2_TEST)
    layout_cid = prepare(t, sess, LAYOUT_CAP, LAYOUT_SRC, LAYOUT_TEST)
    bad_item, bad_cap, bad_src = _find_candidate(t, EDIT_CANDS, "wrong_permission")
    v1_lir, v1_cap = t.load(V1_SRC), t.load(EDIT_V1)
    v2_lir, v2_cap = t.load(V2_SRC), t.load(EDIT_V2)
    progs = {"v1": (v1_cid, v1_lir, v1_cap), "v2": (v2_cid, v2_lir, v2_cap)}
    active = "v1"
    cols = t.load(EDIT_WORLD)["columns"]
    manifest_for = {"v1": manifest_identity(st.get("semantic_manifest"))}
    counts = collections.Counter()
    rss_sup, rss_worker = [], []
    prev_rev = None
    for i in range(iters):
        kind = T11_CYCLE[i % len(T11_CYCLE)]
        pre = inspect(t, sess, wid)
        rss_sup.append((i, _rss_kib(sup)))
        rss_worker.append((i, _rss_kib(worker0)))
        other = "v2" if active == "v1" else "v1"
        cid_o, _prog_o, _capd_o = progs[other]
        rejected, _env = _submit_expect_rejection(t, sess, bad_cap, bad_src, bad_item["expected_code"])
        if not rejected:
            raise Failed(f"iteration {i}: invalid candidate was accepted")
        counts["invalid_refused"] += 1
        if kind in T11_SUCCESS:
            install(t, sess, wid, cid_o, pre["world_revision"], pre["tick"])
            active = other
            counts["successful_install"] += 1
            if kind == "revert":
                counts["reverted_revision"] += 1
            post = inspect(t, sess, wid)
            t.check_mismatches(f"iteration {i} {kind}: install leaves tick and columns unchanged",
                               compare_columns(pre["columns"], post["columns"])
                               + ([] if post["tick"] == pre["tick"] else [f"tick {pre['tick']} -> {post['tick']}"]))
            now = manifest_identity(post.get("semantic_manifest"))
            manifest_for.setdefault(active, now)
            t.check_mismatches(f"iteration {i} {kind}: manifest is the {active} identity",
                               compare_manifest(manifest_for[active], now))
        else:
            if kind == "fail_stale_revision":
                t.check(f"iteration {i}: stale expectation differs from the current revision", True,
                        prev_rev != pre["world_revision"])
                env = _raw_install(sess, wid, cid_o, prev_rev, pre["tick"])
            elif kind == "fail_past_tick":
                t.check(f"iteration {i}: a past tick exists", True, pre["tick"] >= 1)
                env = _raw_install(sess, wid, cid_o, pre["world_revision"], pre["tick"] - 1)
            else:  # fail_layout_generation
                env = _raw_install(sess, wid, layout_cid, pre["world_revision"], pre["tick"])
            t.check_mismatches(f"iteration {i} {kind}: install refused with {T11_EXPECT[kind]}",
                               classify_error(env, T11_EXPECT[kind]))
            counts["failed_install"] += 1
            post = inspect(t, sess, wid)
            t.check_mismatches(f"iteration {i} {kind}: refused install leaves revision, tick and columns unchanged",
                               verify_rejections_leave_state(pre, post))
            t.check_mismatches(f"iteration {i} {kind}: refused install leaves the {active} manifest unchanged",
                               compare_manifest(manifest_for[active], manifest_identity(post.get("semantic_manifest"))))
        counts[kind] += 1
        # One tick under the active version: the state must match the reference for that version.
        step(t, sess, wid, 1)
        _cid, prog, capd = progs[active]
        cols = ref.run_tick(prog, capd, cols)
        after = inspect(t, sess, wid)
        t.check_mismatches(f"iteration {i} {kind}: one tick under {active} matches the reference",
                           verify_state(pre["tick"] + 1, cols, after))
        t.check_mismatches(f"iteration {i} {kind}: manifest after the tick is {active}",
                           compare_manifest(manifest_for[active], manifest_identity(after.get("semantic_manifest"))))
        prev_rev = pre["world_revision"]
    end_worker = inspect(t, sess, wid).get("worker_pid")
    t.check("worker pid preserved across all reloads", worker0, end_worker)
    t.check("worker OS identity preserved across all reloads", w_id0, identity_key(os_identity(worker0)))
    t.check("supervisor OS identity preserved across all reloads", sup_id0, identity_key(os_identity(sup)))
    missing = [k for k in T11_CYCLE if counts[k] == 0]
    t.check_mismatches("every kind ran at least once", [f"{k} never ran" for k in missing]
                       + ([] if iters >= len(T11_CYCLE) else [f"{iters} iterations is fewer than one cycle"]))
    t.check("every iteration ran exactly one kind", iters, sum(counts[k] for k in T11_CYCLE))
    warm = iters // 5
    win = max(1, iters // 10)
    base = {"supervisor": rss_window_median(rss_sup, warm, warm + win),
            "worker": rss_window_median(rss_worker, warm, warm + win)}
    end = {"supervisor": rss_window_median(rss_sup, iters - win, iters),
           "worker": rss_window_median(rss_worker, iters - win, iters)}
    t.details.update({
        "iterations": iters,
        "cycle": list(T11_CYCLE),
        "counts": dict(counts),
        "rss_kib": {"warm_up_window": [warm, warm + win], "final_window": [iters - win, iters],
                    "baseline_median": base, "end_median": end, "start": {"supervisor": rss_sup[0][1], "worker": rss_worker[0][1]}},
        "rss_bound_kib_harness_chosen": HARNESS_RSS_GROWTH_BOUND_KIB,
        "sanitizers": sanitizers,
        "rss_caveat": ("ASan allocator quarantine: engine started with quarantine_size_mb=1 under AddressSanitizer"
                       if sanitizers else "none (no sanitizer runtime in the binary)"),
    })
    if None in base.values() or None in end.values():
        raise NotRun("too few iterations for a warm-up window and a final window")
    t.check_mismatches("median RSS growth from warm-up window to final window within harness bound",
                       rss_verdict("supervisor", base["supervisor"], end["supervisor"], HARNESS_RSS_GROWTH_BOUND_KIB)
                       + rss_verdict("worker", base["worker"], end["worker"], HARNESS_RSS_GROWTH_BOUND_KIB))


PROBE_SCRIPT = os.path.join(REPO, "tests", "e2e", "no_build_probe.py")


def _t12_fallback(t: TestCtx, sched, expected):
    """Secondary evidence: PATH shims for named tools plus /proc child polling during one capsule-only swap.
    It cannot see an absolute-path exec, so it never stands alone (the strace probe is primary)."""
    shim_root = tempfile.mkdtemp(prefix="m0-shim-", dir=os.path.join(REPO, "build")) \
        if os.path.isdir(os.path.join(REPO, "build")) else tempfile.mkdtemp(prefix="m0-shim-")
    log = os.path.join(shim_root, "invocations.log")
    open(log, "w", encoding="utf-8").close()
    env = dict(t.env)
    shims = {}
    for name in SHIM_TOOLS:
        real = shutil.which(name, path=t.env.get("PATH"))
        if not real:
            continue
        path = os.path.join(shim_root, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("#!/bin/sh\n"
                     f"printf '%s %s %s\\n' \"$(date +%s.%N)\" {shlex.quote(name)} \"$*\" >> {shlex.quote(log)}\n"
                     f"exec {shlex.quote(real)} \"$@\"\n")
        os.chmod(path, 0o755)
        shims[name] = real
    env["PATH"] = shim_root + os.pathsep + t.env.get("PATH", "")
    t.details["shimmed_tools"] = sorted(shims)
    if "cc" not in shims:
        # Secondary evidence only: without a compiler on PATH it is skipped, and the primary probe decides.
        shutil.rmtree(shim_root, ignore_errors=True)
        t.details["secondary"] = "skipped: no cc on PATH to instrument"
        return
    subprocess.run([os.path.join(shim_root, "cc"), "--version"], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)
    with open(log, encoding="utf-8") as fh:
        t.check("instrumentation control records a compiler invocation", True, bool(fh.read().strip()))
    open(log, "w", encoding="utf-8").close()
    sess = t.session("T12 serve (instrumented PATH)", env=env)
    stop = threading.Event()
    hits = []
    sample_count = [0]

    def sampler():
        while not stop.is_set():
            now = time.time()
            sample_count[0] += 1
            desc, frontier = set(), [sess.proc.pid]
            while frontier:
                for kid in _children_fast(frontier.pop()):
                    if kid not in desc:
                        desc.add(kid)
                        frontier.append(kid)
            for pid in desc:
                name = _comm(pid)
                if name in BUILD_COMMS:
                    hits.append((now, name, pid))
            time.sleep(0.0005)

    wid, rev0 = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, 2)
    thread = threading.Thread(target=sampler, daemon=True)
    thread.start()
    window_start = time.time()
    try:
        pre = inspect(t, sess, wid)
        cid = prepare(t, sess, EDIT_V2, V2_SRC, EDIT_V2_TEST)
        install(t, sess, wid, cid, pre["world_revision"], sched["swap"]["effective_tick"])
        step(t, sess, wid, 2)
        final = inspect(t, sess, wid)
    finally:
        window_end = time.time()
        stop.set()
        thread.join(2)
    t.check_mismatches("capsule-only swap produces the v2 state at tick 4",
                       verify_state(4, expected[4], final))
    with open(log, encoding="utf-8") as fh:
        lines = [ln.split(" ", 2) for ln in fh.read().splitlines() if ln.strip()]
    in_window = [ln for ln in lines if window_start <= float(ln[0]) <= window_end]
    sampled = [h for h in hits if window_start <= h[0] <= window_end]
    t.details["shim_invocations_total"] = len(lines)
    t.details["shim_invocations_in_swap_window"] = in_window
    t.details["sampled_build_processes_in_swap_window"] = [{"comm": c, "pid": p} for _ts, c, p in sampled]
    t.details["sampling"] = {"method": "PATH shims for named tools plus /proc/<pid>/task/*/children tree polling (about every 0.5 ms)",
                             "samples": sample_count[0],
                             "limitation": "polling can miss processes shorter than the interval; shims see only "
                                           "invocations by bare name through PATH, not absolute-path execs"}
    t.check("secondary: no compiler, assembler, linker or build tool seen by shims or polling in the swap window",
            0, len(in_window) + len(sampled))
    shutil.rmtree(shim_root, ignore_errors=True)


def _t12_probe(t: TestCtx) -> dict:
    """Primary evidence: tests/e2e/no_build_probe.py (strace -f over the whole demo run, both positive controls
    required). Its evidence file goes to a scratch directory outside the repository. Returns the evidence."""
    if shutil.which("strace") is None:
        raise NotRun("inconclusive: strace is not installed, so the primary no-build evidence was not collected")
    scratch = tempfile.mkdtemp(prefix="m0-t12-probe-")
    out = os.path.join(scratch, "no_build_probe.json")
    try:
        rc, _text = t.run_cmd([sys.executable, "-I", PROBE_SCRIPT, "--out", out, "--binary", t.binary], timeout=1200)
        if not os.path.isfile(out):
            raise NotRun(f"inconclusive: the strace probe wrote no evidence (exit {rc})")
        with open(out, encoding="utf-8") as fh:
            evidence = strict_loads(fh.read())
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    t.details["strace_probe"] = {
        "exit_status": rc, "verdict": evidence.get("verdict"), "reason": evidence.get("reason"),
        "method": evidence.get("method"), "positive_controls": [
            {"name": c.get("name"), "detected": c.get("detected")} for c in evidence.get("positive_controls", [])],
        "run_summary": evidence.get("run_summary"), "edit_window_epoch_s": evidence.get("edit_window_epoch_s"),
        "demo_status": (evidence.get("demo") or {}).get("status"),
        "binary_sha256": (evidence.get("demo") or {}).get("binary_sha256"),
    }
    return evidence


def t12(t: TestCtx):
    """No native gameplay build during a capsule-only edit and install (TEST_SPEC T12).

    Primary: the strace probe (tests/e2e/no_build_probe.py). Verdict pass requires both positive controls
    to be detected and zero build-tool execs in the whole traced run. A probe fail is a failed test. An
    inconclusive probe (strace missing, a positive control missed, demo not passed) is reported as
    not_run with the reason; it is never read as a pass.
    Secondary: PATH shims and /proc polling in the swap window, which must also see nothing.
    """
    sched, expected = _swap_expected(t)
    _t12_fallback(t, sched, expected)
    evidence = _t12_probe(t)
    verdict = evidence.get("verdict")
    controls = evidence.get("positive_controls", [])
    if verdict == "fail":
        t.check("strace probe verdict is pass", "pass", verdict)
    if verdict not in ("pass", "fail"):
        raise NotRun(f"inconclusive: strace probe verdict {verdict!r}: {evidence.get('reason')}")
    t.check("strace probe: both positive controls detected", True,
            len(controls) == 2 and all(c.get("detected") is True for c in controls))
    t.check("strace probe: demo passed under tracing", "passed", (evidence.get("demo") or {}).get("status"))
    summary = evidence.get("run_summary") or {}
    t.check("strace probe: zero build-tool execs in the whole traced run", 0,
            len(summary.get("build_tool_execs_total") or []))
    t.check("strace probe: zero build-tool execs in the edit window", 0,
            len(summary.get("build_tool_execs_in_window") or []))
    t.check("strace probe verdict is pass", "pass", verdict)


def _schema_signature(cap: dict):
    """Fixed-schema identity of a capsule: schema block, field table and numeric profile."""
    return (cap.get("schema"), [(f.get("id"), f.get("name"), f.get("type"), f.get("read"), f.get("write"))
                                for f in cap.get("fields", [])], cap.get("numeric_profile"))


def _rows_of(columns: dict) -> int:
    return len(next(iter(columns.values()))) if columns else 0


def t13(t: TestCtx):
    """Latency measurement compared with the proposed targets in docs/PERFORMANCE.md section 2.

    Distributions (all milliseconds, nearest-rank p95, host clock):
      prepare_submit_and_evaluate  submit + evaluate, fresh serve process per sample
      evaluate_fresh_session       evaluate alone, fresh serve process per sample
      evaluate_reusable_worker     evaluate alone, one serve process and one candidate, repeated
      install_call_round_trip      candidate.install RPC (includes queue, boundary and publish)
      first_step_after_install     the first tick that runs the new code
      edit_to_first_effect         submit through first step, per sample
    Each target gets a verdict met / not_met / not_comparable with its reason and every gate it depends on.
    All verdicts are provisional: the host is a shared VM (C10). The test passes when the measurements are
    collected and a verdict is recorded for every target; the verdicts themselves are report fields.
    """
    sched, expected = _swap_expected(t)
    swap_tick = sched["swap"]["effective_tick"]
    reps = t.run.latency_samples
    prepare_s, eval_fresh_s, install_s, step_s, edit_s = [], [], [], [], []
    for i in range(reps):
        sess = t.session(f"T13 serve sample {i}")
        wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
        step(t, sess, wid, swap_tick)
        pre = inspect(t, sess, wid)
        t0 = time.monotonic()
        cid = submit(t, sess, EDIT_V2, V2_SRC)
        tsub = time.monotonic()
        ev = evaluate(t, sess, cid, EDIT_V2_TEST)
        t1 = time.monotonic()
        t.check(f"T13 sample {i} evaluation passed", "passed", ev.get("status"))
        install(t, sess, wid, cid, pre["world_revision"], swap_tick)
        t2 = time.monotonic()
        step(t, sess, wid, 1)
        t3 = time.monotonic()
        prepare_s.append(t1 - t0)
        eval_fresh_s.append(t1 - tsub)
        install_s.append(t2 - t1)
        step_s.append(t3 - t2)
        edit_s.append(t3 - t0)
        sess.close()
    # Reusable worker: one serve process, one candidate, the same test file evaluated repeatedly.
    reuse_s = []
    sess = t.session("T13 serve reusable worker")
    wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, swap_tick)
    cid = submit(t, sess, EDIT_V2, V2_SRC)
    for i in range(reps):
        t0 = time.monotonic()
        ev = evaluate(t, sess, cid, EDIT_V2_TEST)
        reuse_s.append(time.monotonic() - t0)
        t.check(f"T13 reusable evaluation {i} passed", "passed", ev.get("status"))
    sess.close()

    metrics = {
        "prepare_submit_and_evaluate": latency_stats(prepare_s),
        "evaluate_fresh_session": latency_stats(eval_fresh_s),
        "evaluate_reusable_worker": latency_stats(reuse_s),
        "install_call_round_trip": latency_stats(install_s),
        "first_step_after_install": latency_stats(step_s),
        "edit_to_first_effect": latency_stats(edit_s),
    }
    # Gate facts from the fixtures themselves.
    v1_cap, v2_cap = t.load(EDIT_V1), t.load(EDIT_V2)
    src_bytes = os.path.getsize(t.abspath(V2_SRC))
    rows = _rows_of(t.load(EDIT_WORLD)["columns"])
    cases = len(t.load(EDIT_V2_TEST)["cases"])
    same_schema = _schema_signature(v1_cap) == _schema_signature(v2_cap)
    same_layout = v1_cap["schema"]["layout_generation"] == v2_cap["schema"]["layout_generation"]
    warm = (True, "one world created and stepped before the timed window; each sample is a fresh serve process, "
                  "so process spawn is outside the timed window")
    gates = {
        "ir_prepare": {
            "source <= 64 KiB": (src_bytes <= 65536, f"source is {src_bytes} bytes"),
            "existing fixed schema": (same_schema, "capsule schema, fields and profile equal to the installed one"
                                      if same_schema else "capsule schema differs from the installed one"),
            "warm process": warm,
            "1,024-row preview": (rows == 1024, f"fixture has {rows} rows; the target names a 1,024-row preview"),
        },
        "preview_response": {
            "reusable worker": (True, f"one serve process evaluated the same candidate {reps} times"),
            "small fixture": (rows <= 16 and cases <= 8, f"{rows} rows, {cases} cases"),
            "excludes long qualification suites": (True, "only the edit candidate test file is evaluated"),
        },
        "install_critical_section": {
            "code-only swap": (same_schema and same_layout, "same schema and layout_generation as the installed capsule"),
            "bindings already prepared": (True, "the candidate was submitted and evaluated before every install"),
            "no migration": (same_layout, "layout_generation unchanged" if same_layout else "layout changed"),
        },
        "warm_edit_to_first_effect": {"warm process": warm},
        "native_incremental_check": {},
    }
    verdicts = [compare_target(tg, metrics.get(tg["metric"]) if tg["metric"] else None, gates[tg["id"]], ())
                for tg in TIMING_TARGETS]
    t.check("T13 a verdict is recorded for every proposed target",
            [tg["id"] for tg in TIMING_TARGETS], [v["id"] for v in verdicts])
    t.check("T13 every verdict is met, not_met or not_comparable", True,
            all(v["verdict"] in TIMING_VERDICTS and v["reason"] for v in verdicts))
    t.check("T13 measured samples equal the requested count", [reps] * 5,
            [len(prepare_s), len(eval_fresh_s), len(reuse_s), len(install_s), len(edit_s)])
    t.details["latency_ms"] = {
        "prepare_submit_and_evaluate": metrics["prepare_submit_and_evaluate"],
        "evaluate_fresh_session": metrics["evaluate_fresh_session"],
        "evaluate_reusable_worker": metrics["evaluate_reusable_worker"],
        "install_call_round_trip": metrics["install_call_round_trip"],
        "first_step_after_install": metrics["first_step_after_install"],
        "edit_to_first_effect": metrics["edit_to_first_effect"],
    }
    t.details["timing_targets"] = verdicts
    t.details["timing_target_source"] = "docs/PERFORMANCE.md section 2 (proposed, unmeasured there)"
    t.details["timing_target_note"] = ("provisional: shared VM (C10). p95 from "
                                       f"{reps} samples is nearest-rank; with fewer than 20 samples it is close to the maximum.")
    t.details["decomposition_limits"] = ("candidate.install is synchronous: queue, worker and publish happen inside one "
                                         "RPC, so they cannot be separated from the host clock. The install figure "
                                         "includes them; the step figure is the first tick that runs the new code.")
    t.details["machine"] = machine_info()
    t.details["status_meaning"] = ("passed = measurements collected and a verdict recorded for every proposed target. "
                                   "The verdicts (timing_targets) are provisional and are not the test status.")


def _canonical_run(t, label, sched, expected):
    sess = t.session(f"T14 {label}")
    wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, sched["swap"]["effective_tick"])
    pre = inspect(t, sess, wid)
    cid = prepare(t, sess, EDIT_V2, V2_SRC, EDIT_V2_TEST)
    install(t, sess, wid, cid, pre["world_revision"], sched["swap"]["effective_tick"])
    step(t, sess, wid, 2)
    post = inspect(t, sess, wid)
    sess.close()
    return canonical_state_bytes(post), post


def t14(t: TestCtx):
    sched, expected = _swap_expected(t)
    bytes_a, post_a = _canonical_run(t, "run A", sched, expected)
    bytes_b, _post_b = _canonical_run(t, "run B", sched, expected)
    t.check("T14 canonical state byte-identical across fresh sessions", True, bytes_a == bytes_b)
    t.details["canonical_state_sha256"] = {"run_A": sha256_bytes(bytes_a), "run_B": sha256_bytes(bytes_b)}
    t.check_mismatches("T14 final state equals reference", verify_state(4, expected[4], post_a))


def t15(t: TestCtx):
    corpus = t.load("tests/fixtures/protocol/cases.json")
    max_bytes = corpus["maximum_message_bytes"]
    results = []
    for case in corpus["cases"]:
        sess = t.session(f"T15 {case['id']}")
        if "file" in case:
            with open(t.need_file(f"tests/fixtures/protocol/{case['file']}"), "rb") as fh:
                raw = fh.read()
        else:
            raw = make_oversize_line(max_bytes + 1)
            t.check("oversize line is one byte over the limit", max_bytes + 1, len(raw))
        sess.send(raw + (b"" if raw.endswith(b"\n") else b"\n"))
        reply = sess.read_message(timeout=15)
        if reply is None:
            raise Failed(f"{case['id']}: engine closed without a parseable reply")
        env = parse_envelope(reply, None)
        t.check_mismatches(f"{case['id']} rejection code", classify_error(env, case["expected_code"]))
        probe = sess.request("engine.info", {}, timeout=15)
        t.check(f"{case['id']} session survives", True, probe["ok"])
        results.append({"case": case["id"], "expected": case["expected_code"], "sha256": sha256_bytes(raw)})
        sess.close()
    t.details["cases"] = results


_EVAL = ("world.create", "world.step", "world.inspect", "capsule.submit", "candidate.evaluate", "candidate.install")

TESTS = [
    ("T00", "Clean bootstrap", (), t00),
    ("T01", "Reference movement", ("world.create", "world.step", "world.inspect"), t01),
    ("T02", "Empty and awkward counts", ("world.create", "world.step", "world.inspect"), t02),
    ("T03", "Numeric edges", ("world.create", "world.step", "world.inspect"), t03),
    ("T04", "Invalid input matrix", ("world.create", "world.inspect", "capsule.submit"), t04),
    ("T05", "Successful code-only swap", _EVAL, t05),
    ("T06", "Failed swap", _EVAL, t06),
    ("T07", "Stale candidate", _EVAL, t07),
    ("T08", "Snapshot round trip", ("world.create", "world.step", "world.inspect", "world.snapshot",
                                    "world.restore"), t08),
    ("T09", "Working-root failure", ("world.create", "world.step", "world.inspect"), t09),
    ("T10", "Candidate isolation", _EVAL + ("candidate.status",), t10),
    ("T11", "Repeated reload", _EVAL, t11),
    ("T12", "No native gameplay build", _EVAL, t12),
    ("T13", "Latency decomposition", _EVAL, t13),
    ("T14", "Deterministic fixture repeat", _EVAL, t14),
    ("T15", "Structured protocol", ("engine.info",), t15),
]



def run_test(run: RunCtx, test_id, name, requires, fn) -> dict:
    t = TestCtx(run, test_id)
    status, reason, details = PASSED, "", {}
    try:
        if requires:
            probe = run.probe
            if probe["state"] == "absent":
                raise Unavailable(probe["reason"])
            if probe["state"] == "malformed":
                raise Failed(probe["reason"])
            caps = probe.get("capabilities")
            if caps is None:
                raise Unavailable("engine.info lists no capabilities")
            missing = sorted(set(requires) - caps)
            if missing:
                raise Unavailable("engine does not advertise: " + ", ".join(missing))
        fn(t)
    except Outcome as exc:
        status, reason = exc.status, str(exc.args[0]) if exc.args else exc.status
    except SystemExit as exc:
        status, reason = NOT_RUN, f"harness configuration error: {exc}"
    except Exception as exc:  # harness bug: never report as passed or failed
        status = NOT_RUN
        reason = f"harness error: {type(exc).__name__}: {exc}"
        details["traceback"] = traceback.format_exc()[-2000:]
    finally:
        try:
            t.close()
        except Exception as exc:  # pragma: no cover - close must not hide the verdict
            details["close_error"] = str(exc)
    # Guard rail: a body that returns without recording any check has verified nothing, so it cannot pass.
    if status == PASSED and t.checks_total == 0:
        status, reason = NOT_RUN, "test body returned without recording any check; nothing was verified"
    details.update(t.details)
    return {"id": test_id, "name": name, "status": status, "reason": reason, "checks": t.checks,
            "checks_total": t.checks_total, "checks_failed": t.checks_failed,
            "checks_recorded": len(t.checks), "checks_dropped": t.checks_dropped,
            "inputs": t.inputs, "commands": t.commands, "details": details}


def run_probe(run: RunCtx) -> dict:
    """Send engine.info to `serve` once and classify the real reply."""
    t = TestCtx(run, "probe")
    sess = Session(t, "probe serve")
    probe_id = "m0-probe-1"
    body = json.dumps({"protocol": PROTOCOL, "id": probe_id, "method": "engine.info", "params": {}},
                      separators=(",", ":")).encode("utf-8") + b"\n"
    raw, cls = None, None
    try:
        sess.send(body, tolerate_closed=True)
        raw = sess.read_message(timeout=15)
    except Failed as exc:
        cls = {"state": "absent", "reason": f"serve did not answer engine.info: {exc.args[0]}"}
    finally:
        sess.close()
    if cls is None:
        cls = classify_engine_info(raw, probe_id)
        if raw is None:
            cls["reason"] += f"; stderr tail {sess.stderr_tail()[-300:]!r}"
    cls["reason"] += f" [serve exit status {sess.cmd['returncode']}]" if cls.get("reason") else ""
    cls["request"] = body.decode("utf-8").strip()
    cls["reply"] = raw.decode("utf-8", "replace")[:600] if raw is not None else None
    cls["serve_returncode"] = sess.cmd["returncode"]
    cls["commands"] = t.commands
    return cls


def summarize(results: list) -> dict:
    counts = {s: 0 for s in STATUSES}
    for r in results:
        counts[r["status"]] += 1
    return counts


def write_outputs(report: dict, report_dir: str):
    os.makedirs(report_dir, exist_ok=True)
    json_path = os.path.join(report_dir, "m0_report.json")
    text_path = os.path.join(report_dir, "m0_summary.txt")
    with open(json_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(report, fh, indent=2, sort_keys=True)
        fh.write("\n")
    lines = [f"M0 harness {report['harness']['sha256'][:12]} against {report['binary']['path']} "
             f"(sha256 {report['binary']['sha256']})",
             f"probe: {report['probe']['state']}" + (f" - {report['probe']['reason']}" if report['probe']['reason'] else ""),
             ""]
    for r in report["tests"]:
        lines.append(f"{r['id']:4} {r['status']:12} {r['name']}  (checks {r.get('checks_total', 0)}, "
                     f"failed {r.get('checks_failed', 0)}, dropped from detail {r.get('checks_dropped', 0)})")
        if r["reason"]:
            lines.append(f"       {r['reason']}")
    if report.get("timing_targets"):
        lines += ["", "timing targets (docs/PERFORMANCE.md section 2; provisional, shared VM, C10):"]
        for v in report["timing_targets"]:
            p95 = "n/a" if v["p95_ms"] is None else f"{v['p95_ms']} ms"
            lines.append(f"  {v['id']:26} {v['verdict']:15} p95 {p95} (target <= {v['target_p95_ms_max']} ms)")
    lines += ["", "totals: " + ", ".join(f"{k}={v}" for k, v in report["summary"].items())]
    with open(text_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    return json_path, text_path, "\n".join(lines)


def git_head():
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True,
                             shell=False, timeout=10)
        return out.stdout.strip() if out.returncode == 0 else None
    except OSError:
        return None


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default=os.path.join(REPO, "build", "sutekh"))
    parser.add_argument("--report-dir", default=os.path.join(REPO, "build", "m0_harness"))
    parser.add_argument("--no-build", action="store_true", help="skip the T00 build command")
    parser.add_argument("--only", default="", help="comma-separated test ids, e.g. T00,T15")
    parser.add_argument("--reload-iterations", type=int, default=DEFAULT_RELOAD_ITERATIONS,
                        help="T11 iterations of the five-kind reload cycle (default 1000, the TEST_SPEC count; "
                             "a smaller value is a shorter run, not a closure result)")
    parser.add_argument("--latency-samples", type=int, default=30, help="T13 fresh-session samples")
    parser.add_argument("--self-test", action="store_true", help="unit-test the harness logic and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    binary = os.path.abspath(args.binary)
    if args.reload_iterations < 2 or args.latency_samples < 1:
        print("--reload-iterations must be >= 2 and --latency-samples >= 1", file=sys.stderr)
        return 2
    run = RunCtx(binary, args.no_build, args.reload_iterations, args.latency_samples)
    wanted = {x.strip() for x in args.only.split(",") if x.strip()}
    bad = wanted - {tid for tid, *_ in TESTS}
    if bad:
        print(f"unknown test ids: {sorted(bad)}", file=sys.stderr)
        return 2

    # Probe before T00 so the report explains what the binary supports; T00 may rebuild it.
    run.probe = run_probe(run) if os.path.isfile(binary) else {
        "state": "absent", "reason": f"binary not found: {binary}", "capabilities": None,
        "commands": []}
    results = []
    for test_id, name, requires, fn in TESTS:
        if wanted and test_id not in wanted:
            continue
        if test_id == "T00":
            # The bootstrap test may build; re-probe afterwards so later tests see the new binary.
            results.append(run_test(run, test_id, name, requires, fn))
            if os.path.isfile(binary):
                run.probe = run_probe(run)
            continue
        results.append(run_test(run, test_id, name, requires, fn))

    binary_hash = sha256_file(binary) if os.path.isfile(binary) else None
    report = {
        "report_format": HARNESS_FORMAT,
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "repo_head": git_head(),
        "harness": {"path": "tests/harness/run_m0.py", "sha256": sha256_file(os.path.join(HERE, "run_m0.py")),
                    "reference_eval_sha256": sha256_file(os.path.join(HERE, "reference_eval.py")),
                    "python": sys.version.split()[0], "argv": sys.argv},
        "binary": {"path": os.path.relpath(binary, REPO) if binary.startswith(REPO) else binary,
                   "sha256": binary_hash},
        "clean_build": next((r["details"].get("clean_build") for r in results if r["id"] == "T00"
                             and r["details"].get("clean_build")), None),
        "probe": {k: (sorted(v) if isinstance(v, set) else v) for k, v in run.probe.items()
                  if k != "commands"},
        "tests": results,
        "summary": summarize(results),
        "timing_targets": next((r["details"].get("timing_targets") for r in results if r["id"] == "T13"
                                and r["details"].get("timing_targets")), None),
        "timing_target_status": "provisional (shared VM, C10); verdicts are not the test status",
        "note": "passed means every check ran and matched independently derived expectations, and a test "
                "that recorded no check cannot pass; no engine-provided flag was trusted.",
    }
    json_path, text_path, text = write_outputs(report, args.report_dir)
    print(text)
    print(f"report: {json_path}")
    print(f"summary: {text_path}")
    return 1 if report["summary"][FAILED] else 0


# ------------------------------------------------------------------ self-test

def _failing_counts(run) -> tuple:
    """(checks_total, checks_failed) after one mismatching check."""
    tc = TestCtx(run, "fail")
    try:
        tc.check("bad", 1, 2)
    except Failed:
        pass
    return tc.checks_total, tc.checks_failed


def _cap_counts(run, cap: int, n: int) -> tuple:
    """(checks_total, checks_recorded, checks_dropped) for n passing checks with the record cap set to cap."""
    global MAX_RECORDED_CHECKS
    saved = MAX_RECORDED_CHECKS
    MAX_RECORDED_CHECKS = cap
    try:
        tc = TestCtx(run, "cap")
        for k in range(n):
            tc.check(f"c{k}", 1, 1)
        return tc.checks_total, len(tc.checks), tc.checks_dropped
    finally:
        MAX_RECORDED_CHECKS = saved


def self_test() -> int:
    checks = []

    def ok(name, cond):
        checks.append((name, bool(cond)))

    good = {"a": [1, 2, 4294967295]}
    ok("identical columns: no mismatch", compare_columns(good, {"a": [1, 2, 4294967295]}) == [])
    ok("one wrong value: mismatch", compare_columns(good, {"a": [1, 3, 4294967295]}) != [])
    ok("short column: mismatch", compare_columns(good, {"a": [1, 2]}) != [])
    ok("extra column name: mismatch", compare_columns(good, {"a": [1, 2, 4294967295], "b": []}) != [])
    ok("bool is not u32", compare_columns({"a": [1]}, {"a": [True]}) != [])
    ok("float is not u32", compare_columns({"a": [1]}, {"a": [1.0]}) != [])
    ok("out-of-range value: mismatch", compare_columns({"a": [1]}, {"a": [4294967296]}) != [])
    ok("missing columns object: mismatch", compare_columns(good, None) != [])
    ok("verify_state good", verify_state(2, {"a": [5]}, {"tick": 2, "columns": {"a": [5]}}) == [])
    ok("verify_state wrong tick", verify_state(2, {"a": [5]}, {"tick": 3, "columns": {"a": [5]}}) != [])
    ok("verify_state bool tick", verify_state(1, {"a": [5]}, {"tick": True, "columns": {"a": [5]}}) != [])
    env_good = {"ok": False, "error": {"code": "SU_E_TYPE"}}
    ok("rejection with right code passes", classify_error(env_good, "SU_E_TYPE") == [])
    ok("rejection with wrong code fails", classify_error(env_good, "SU_E_PARSE") != [])
    ok("acceptance where rejection expected fails", classify_error({"ok": True, "result": {}}, "SU_E_TYPE") != [])
    rej = {"tick": 0, "world_revision": "r1", "columns": {"a": [1]}}
    ok("no-collateral: identical passes", verify_rejections_leave_state(rej, dict(rej)) == [])
    ok("no-collateral: revision change fails",
       verify_rejections_leave_state(rej, {**rej, "world_revision": "r2"}) != [])
    ok("no-collateral: column change fails",
       verify_rejections_leave_state(rej, {**rej, "columns": {"a": [2]}}) != [])
    ok("manifest equal passes", compare_manifest([{"system_id": "x"}], [{"system_id": "x"}]) == [])
    ok("manifest missing fails", compare_manifest([], None) != [])
    raw_ok = b'{"protocol":"0.1","id":"q-1","ok":true,"result":{"pid":5}}'
    ok("parse valid envelope", parse_envelope(raw_ok, "q-1")["result"]["pid"] == 5)
    for label, raw in (
        ("duplicate key reply", b'{"protocol":"0.1","protocol":"0.1","id":"q-1","ok":true,"result":{}}'),
        ("bad utf-8 reply", b'{"protocol":"0.1","id":"q-1","ok":true,"result":{"x":"\xff"}}'),
        ("wrong protocol reply", b'{"protocol":"9.9","id":"q-1","ok":true,"result":{}}'),
        ("wrong id reply", b'{"protocol":"0.1","id":"other","ok":true,"result":{}}'),
        ("non-object reply", b'[1,2]'),
        ("NaN reply", b'{"protocol":"0.1","id":"q-1","ok":true,"result":{"x":NaN}}'),
    ):
        try:
            parse_envelope(raw, "q-1")
            ok(f"parse rejects {label}", False)
        except Failed:
            ok(f"parse rejects {label}", True)
    skeleton = b'{"ok":false,"error":{"code":"SU_E_UNSUPPORTED_METHOD","status":12,"message":"role \'serve\' is not implemented in the M0 skeleton"}}'
    ok("probe: skeleton reply is absent", classify_engine_info(skeleton, "m0-probe-1")["state"] == "absent")
    ok("probe: no reply is absent", classify_engine_info(None, "m0-probe-1")["state"] == "absent")
    ok("probe: garbage is malformed", classify_engine_info(b"hello", "m0-probe-1")["state"] == "malformed")
    present = classify_engine_info(
        b'{"protocol":"0.1","id":"m0-probe-1","ok":true,"result":{"pid":7,"capabilities":{"world.step":true,"x":false}}}',
        "m0-probe-1")
    ok("probe: envelope present with capabilities", present["state"] == "present" and present["capabilities"] == {"world.step"})

    ok("reference evaluator matches hand-derived known answers", ref.self_check_known_answers() == [])
    wrong = ref.known_answer_result("add_u32", 0xFFFFFFFF, 1) != 1
    ok("known-answer table detects a wrong expected value", wrong)
    ok("reference evaluator: wrap at 2^32", ref.known_answer_result("add_u32", 0xFFFFFFFF, 1) == 0)

    ok("oversize line is one byte over 1 MiB", len(make_oversize_line(MAX_MESSAGE + 1)) == MAX_MESSAGE + 1)

    def run_fixture_check(rel):
        path = os.path.join(REPO, rel)
        return os.path.isfile(path)

    if run_fixture_check("tests/fixtures/edit/swap_schedule.json"):
        with open(os.path.join(REPO, "tests/fixtures/edit/swap_schedule.json"), encoding="utf-8") as fh:
            sched = strict_loads(fh.read())
        cap = strict_loads(open(os.path.join(REPO, "tests/fixtures/edit/movement.capsule.json"), encoding="utf-8").read())
        lir = strict_loads(open(os.path.join(REPO, "tests/fixtures/edit/movement.lir.json"), encoding="utf-8").read())
        lir2 = strict_loads(open(os.path.join(REPO, "tests/fixtures/edit/movement_v2.lir.json"), encoding="utf-8").read())
        cap2 = strict_loads(open(os.path.join(REPO, "tests/fixtures/edit/movement_v2.capsule.json"), encoding="utf-8").read())
        cols = sched["expected_by_tick"]["0"]["columns"]
        derived = {}
        for tick in range(1, 5):
            prog, c = (lir, cap) if tick <= 2 else (lir2, cap2)
            cols = ref.run_tick(prog, c, cols)
            derived[str(tick)] = cols
            ok(f"edit fixture tick {tick} equals reference", compare_columns(sched["expected_by_tick"][str(tick)]["columns"], cols) == [])
        bad = dict(sched["expected_by_tick"]["3"]["columns"])
        bad["position_x"] = list(bad["position_x"])
        bad["position_x"][0] += 1
        ok("edit fixture: a one-off engine value is a mismatch", compare_columns(sched["expected_by_tick"]["3"]["columns"], bad) != [])

    # Runner-level statuses on synthetic test bodies (no subprocess).
    class FakeRun:
        binary = "unused"
        env = {}
        codes = {}
        no_build = True
        reload_iterations = 2
        latency_samples = 1
        probe = {"state": "absent", "reason": "synthetic absent", "capabilities": None}

    fake = FakeRun()
    fake.probe = {"state": "present", "reason": "", "capabilities": {"a.b"}}

    def body_pass(t):
        t.check("synthetic equal", 1, 1)

    def body_fail(t):
        t.check_mismatches("synthetic bad", compare_columns({"a": [1]}, {"a": [2]}))

    def body_notrun(t):
        raise NotRun("synthetic")

    def body_crash(t):
        raise KeyError("bug")

    ok("runner: passing body is passed", run_test(fake, "S1", "s", (), body_pass)["status"] == PASSED)
    ok("runner: mismatching body is failed", run_test(fake, "S2", "s", (), body_fail)["status"] == FAILED)
    ok("runner: not_run body is not_run", run_test(fake, "S3", "s", (), body_notrun)["status"] == NOT_RUN)
    ok("runner: harness exception is not_run, never passed",
       run_test(fake, "S4", "s", (), body_crash)["status"] == NOT_RUN)
    ok("runner: advertised method is passed", run_test(fake, "S5", "s", ("a.b",), body_pass)["status"] == PASSED)
    ok("runner: missing method is unavailable",
       run_test(fake, "S6", "s", ("a.b", "c.d"), body_pass)["status"] == UNAVAILABLE)
    fake.probe = {"state": "absent", "reason": "synthetic absent", "capabilities": None}
    ok("runner: absent probe is unavailable",
       run_test(fake, "S7", "s", ("a.b",), body_pass)["status"] == UNAVAILABLE)

    # Protocol corpus shape (fixtures present in the repo).
    pdir = os.path.join(REPO, "tests", "fixtures", "protocol")
    if os.path.isfile(os.path.join(pdir, "cases.json")):
        with open(os.path.join(pdir, "cases.json"), encoding="utf-8") as fh:
            corpus = strict_loads(fh.read())
        for case in corpus["cases"]:
            if "file" not in case:
                ok("protocol corpus: generated oversize case declared", case.get("expected_code") == "SU_E_RESOURCE_LIMIT")
                continue
            with open(os.path.join(pdir, case["file"]), "rb") as fh:
                data = fh.read()
            ok(f"protocol file {case['file']} is one line", data.count(b"\n") == 1 and data.endswith(b"\n"))
            if case["id"] == "invalid_utf8":
                try:
                    data.decode("utf-8")
                    ok("protocol invalid_utf8 really is invalid UTF-8", False)
                except UnicodeDecodeError:
                    ok("protocol invalid_utf8 really is invalid UTF-8", True)
            if case["id"] == "duplicate_key":
                try:
                    strict_loads(data.decode("utf-8"))
                    ok("protocol duplicate_key really has a duplicate key", False)
                except ValueError:
                    ok("protocol duplicate_key really has a duplicate key", True)

    # Cross-check the harness's evaluator against tools/make_fixtures.py on the T03 cases.
    gen_path = os.path.join(REPO, "tools", "make_fixtures.py")
    if os.path.isfile(gen_path):
        import importlib.util
        spec = importlib.util.spec_from_file_location("_gen_cross", gen_path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for case in mod.T03_CASES:
            prog = mod.lir(case["instructions"])
            cap = mod.capsule("program.lir.json", ["test.json"])
            cols = {"position_x": [p for p, _ in case["rows"]], "velocity_x": [v for _, v in case["rows"]]}
            mine = ref.run_tick(prog, cap, cols)["position_x"]
            ok(f"cross-check T03 {case['name']} equals known answer", mine == case["expected"])

    # Paging: merge_pages must reassemble pages and reject every inconsistency.
    def page(offset, rows, rc=5, rev="w.r1", tick=0):
        return {"world_revision": rev, "tick": tick, "row_count": rc, "offset": offset,
                "returned_rows": len(rows), "entity_ids": list(range(offset + 1, offset + 1 + len(rows))),
                "columns": {"position_x": [r for r in rows], "velocity_x": [0] * len(rows)},
                "semantic_manifest": [], "worker_pid": 9}

    def raises(fn):
        try:
            fn()
            return False
        except Failed:
            return True

    pages = [page(0, [1, 2]), page(2, [3, 4]), page(4, [5])]
    merged = merge_pages(pages, INSPECT_FIELDS)
    ok("paging: three pages reassemble in order", merged["columns"]["position_x"] == [1, 2, 3, 4, 5])
    ok("paging: entity ids reassemble", merged["entity_ids"] == [1, 2, 3, 4, 5])
    ok("paging: zero-row world merges to zero rows",
       merge_pages([page(0, [], rc=0)], INSPECT_FIELDS)["returned_rows"] == 0)
    ok("paging: a missing page is rejected", raises(lambda: merge_pages([pages[0], pages[2]], INSPECT_FIELDS)))
    ok("paging: a page at the wrong offset is rejected",
       raises(lambda: merge_pages([page(0, [1, 2]), page(3, [3, 4]), page(4, [5])], INSPECT_FIELDS)))
    ok("paging: a revision change between pages is rejected",
       raises(lambda: merge_pages([pages[0], page(2, [3, 4], rev="w.r2"), pages[2]], INSPECT_FIELDS)))
    ok("paging: a tick change between pages is rejected",
       raises(lambda: merge_pages([pages[0], page(2, [3, 4], tick=1), pages[2]], INSPECT_FIELDS)))
    ok("paging: a short column is rejected",
       raises(lambda: merge_pages([{**pages[0], "columns": {"position_x": [1], "velocity_x": [0, 0]}}],
                                  INSPECT_FIELDS)))
    ok("paging: returned_rows not matching row_count is rejected",
       raises(lambda: merge_pages([page(0, [1, 2], rc=3)], INSPECT_FIELDS)))
    ok("paging: a bool row count is rejected", raises(lambda: merge_pages([page(0, [], rc=True)], INSPECT_FIELDS)))

    # Canonical state: identical states give identical bytes; any difference changes them.
    s1 = {"tick": 2, "row_count": 1, "entity_ids": [1], "columns": {"a": [5]}, "semantic_manifest": [{"x": 1}]}
    ok("canonical: equal states give equal bytes", canonical_state_bytes(dict(s1)) == canonical_state_bytes(dict(s1)))
    ok("canonical: a changed column changes bytes",
       canonical_state_bytes(s1) != canonical_state_bytes({**s1, "columns": {"a": [6]}}))
    ok("canonical: key order does not matter",
       canonical_state_bytes({"columns": {"a": [5]}, "tick": 2, "row_count": 1, "entity_ids": [1],
                              "semantic_manifest": [{"x": 1}]}) == canonical_state_bytes(s1))

    # RSS verdict: harness-chosen bound, both directions.
    ok("rss: growth within bound passes", rss_verdict("p", 1000, 1500, 600) == [])
    ok("rss: growth over bound fails", rss_verdict("p", 1000, 1700, 600) != [])
    ok("rss: shrinkage passes", rss_verdict("p", 1000, 900, 600) == [])
    ok("rss: unobservable is a failure", rss_verdict("p", None, 900, 600) != [])

    # Latency statistics: nearest rank, milliseconds.
    st = latency_stats([0.001, 0.002, 0.003, 0.004, 0.100])
    ok("latency: count", st["n"] == 5)
    ok("latency: min/median/max", (st["min"], st["median"], st["max"]) == (1.0, 3.0, 100.0))
    ok("latency: p95 nearest rank", st["p95"] == 100.0)
    ok("latency: empty input reports zero samples", latency_stats([]) == {"n": 0, "units": "ms"})

    # ---- M0-12: timing-target comparison (docs/PERFORMANCE.md section 2)
    tg = {x["id"]: x for x in TIMING_TARGETS}

    def gates_for(target, flag=True, reason="ok", only=None):
        names = target["gates"]
        out = {n: (flag, reason) for n in names}
        if only is not None:
            out[only] = (False, "fresh serve process per sample")
        return out

    st_ok = {"n": 30, "p95": 9.0}
    ok("timing: p95 within target with every gate met is met",
       compare_target(tg["preview_response"], st_ok, gates_for(tg["preview_response"]))["verdict"] == "met")
    ok("timing: p95 over target with every gate met is not_met (a comparator that never fails would not)",
       compare_target(tg["preview_response"], {"n": 30, "p95": 150.0},
                      gates_for(tg["preview_response"]))["verdict"] == "not_met")
    v_gate = compare_target(tg["preview_response"], {"n": 30, "p95": 150.0},
                            gates_for(tg["preview_response"], only="reusable worker"))
    ok("timing: an unmet gate makes an over-target number not_comparable, with the gate named",
       v_gate["verdict"] == "not_comparable" and "fresh serve process" in v_gate["reason"]
       and v_gate["gates"]["reusable worker"]["met"] is False)
    ok("timing: an unmet gate also blocks a within-target number from being called met",
       compare_target(tg["preview_response"], st_ok, gates_for(tg["preview_response"], only="reusable worker"))
       ["verdict"] == "not_comparable")
    ok("timing: the boundary p95 == target is met",
       compare_target(tg["preview_response"], {"n": 30, "p95": 100.0},
                      gates_for(tg["preview_response"]))["verdict"] == "met")
    ok("timing: install proxy within 1 ms is met (the RPC bounds the section from above)",
       compare_target(tg["install_critical_section"], {"n": 30, "p95": 0.8},
                      gates_for(tg["install_critical_section"]))["verdict"] == "met")
    v_proxy = compare_target(tg["install_critical_section"], {"n": 30, "p95": 1.3},
                             gates_for(tg["install_critical_section"]))
    ok("timing: install proxy over 1 ms is not_comparable, never not_met, and says why",
       v_proxy["verdict"] == "not_comparable" and "upper bound" in v_proxy["reason"])
    ok("timing: native target is not_comparable without a measurement",
       compare_target(tg["native_incremental_check"], None, {})["verdict"] == "not_comparable")
    ok("timing: every verdict carries provisional=True and a reason",
       all(v["provisional"] is True and v["reason"] for v in (
           compare_target(tg["preview_response"], st_ok, gates_for(tg["preview_response"])),
           v_gate, v_proxy)))

    def raises_value(fn):
        try:
            fn()
            return False
        except ValueError:
            return True

    ok("timing: a missing measurement raises instead of being skipped",
       raises_value(lambda: compare_target(tg["preview_response"], None, gates_for(tg["preview_response"]))))
    ok("timing: an empty sample set raises",
       raises_value(lambda: compare_target(tg["preview_response"], {"n": 0, "p95": 1.0},
                                           gates_for(tg["preview_response"]))))
    ok("timing: a NaN p95 raises",
       raises_value(lambda: compare_target(tg["preview_response"], {"n": 3, "p95": float("nan")},
                                           gates_for(tg["preview_response"]))))
    ok("timing: a boolean p95 raises",
       raises_value(lambda: compare_target(tg["preview_response"], {"n": 3, "p95": True},
                                           gates_for(tg["preview_response"]))))
    ok("timing: a gate set that differs from the declared set raises",
       raises_value(lambda: compare_target(tg["preview_response"], st_ok, {"reusable worker": (True, "x")})))

    # ---- stage table and the M0-12 negative matrix
    ok("stage: RESOURCE_LIMIT is a stage-1 code only",
       stage_code_consistent(1, "SU_E_RESOURCE_LIMIT") and not stage_code_consistent(2, "SU_E_RESOURCE_LIMIT"))
    ok("stage: MISSING_STORE is a stage-7 code only",
       stage_code_consistent(7, "SU_E_MISSING_STORE") and not stage_code_consistent(5, "SU_E_MISSING_STORE"))
    ok("stage: SCHEMA_MISMATCH reported at stage 4 is inconsistent",
       not stage_code_consistent(4, "SU_E_SCHEMA_MISMATCH"))
    m0_manifest = os.path.join(REPO, "tests", "fixtures", "m0_neg", "manifest.json")
    if os.path.isfile(m0_manifest):
        with open(m0_manifest, encoding="utf-8") as fh:
            m0doc = strict_loads(fh.read())
        ok("m0_neg manifest: every case's stage can report its expected code",
           all(stage_code_consistent(c["stage"], c["expected_code"]) for c in m0doc["cases"]))
        ok("m0_neg manifest: every verifier stage 1 through 7 is represented",
           {c["stage"] for c in m0doc["cases"]} >= set(range(1, 8)))
        ok("m0_neg manifest: every case file pair exists",
           all(os.path.isfile(os.path.join(REPO, "tests", "fixtures", "m0_neg", c["path"]))
               and os.path.isfile(os.path.join(REPO, "tests", "fixtures", "m0_neg", c["capsule"]))
               for c in m0doc["cases"]))
        ok("m0_neg manifest: oversize IR is one byte over 256 KiB",
           os.path.getsize(os.path.join(REPO, "tests", "fixtures", "m0_neg", "oversize_ir.lir.json")) == 262145)
        codes_all = set()
        for rel in NEG_MANIFESTS:
            if os.path.isfile(os.path.join(REPO, rel)):
                with open(os.path.join(REPO, rel), encoding="utf-8") as fh:
                    codes_all |= {c["expected_code"] for c in strict_loads(fh.read())["cases"]}
        codes_all.add("SU_E_RESOURCE_LIMIT")  # the generated protocol case in T04 is not in a manifest
        ok("T04 required codes are all named by the negative manifests (or the protocol variant)",
           set(T04_REQUIRED_CODES) <= codes_all)

    # ---- runner guard rails
    class _FakeRun2(FakeRun):
        pass

    fake2 = _FakeRun2()
    fake2.probe = {"state": "present", "reason": "", "capabilities": {"a.b"}}

    def body_no_checks(t):
        return None

    def body_one_check(t):
        t.check("one", 1, 1)

    ok("runner: a body that records no check is not_run, never passed",
       run_test(fake2, "Z1", "z", (), body_no_checks)["status"] == NOT_RUN)
    ok("runner: a body that records a check can pass",
       run_test(fake2, "Z2", "z", (), body_one_check)["status"] == PASSED)
    z2 = run_test(fake2, "Z3", "z", (), body_one_check)
    ok("runner: the result reports checks_total and checks_failed",
       z2["checks_total"] == 1 and z2["checks_failed"] == 0 and z2["checks_recorded"] == 1)

    ok("check counts: a failing check is counted as failed and is recorded",
       _failing_counts(fake2) == (1, 1))
    ok("check cap: total and dropped are counted when the record cap is reached",
       _cap_counts(fake2, 3, 5) == (5, 3, 2))
    ok("check cap: default cap is large enough for T11 at 1,000 iterations (no truncation)",
       MAX_RECORDED_CHECKS >= 6 * DEFAULT_RELOAD_ITERATIONS + 100)
    ok("reload default is the TEST_SPEC count of 1000", DEFAULT_RELOAD_ITERATIONS == 1000)

    # ---- OS facts for worker and process identity
    # Fields after ')': index 0 = state (field 3), 1 = ppid (field 4), 19 = starttime (field 22).
    raw_stat = "123 (weird) name) " + " ".join(["S", "45"] + ["0"] * 17 + ["555"])
    parsed = parse_proc_stat(raw_stat, 123)
    ok("proc stat: a comm with spaces and ')' parses (state, ppid, starttime)",
       (parsed["comm"], parsed["state"], parsed["ppid"], parsed["starttime"]) == ("weird) name", "S", 45, 555))
    me = os_identity(os.getpid())
    ok("proc identity: this process is found by its OS facts",
       me is not None and me["pid"] == os.getpid() and isinstance(me["starttime"], int))
    ok("proc identity: an impossible pid is None", os_identity(-5) is None and os_identity(None) is None)
    ok("proc identity: a zombie or missing process is dead; a sleeping one is not",
       is_dead({"state": "Z"}) and is_dead(None) and not is_dead({"state": "S"}))
    ok("proc identity: a recycled pid with a new start time is a different identity",
       identity_key({"pid": 9, "starttime": 1, "ppid": 2}) != identity_key({"pid": 9, "starttime": 2, "ppid": 2}))
    with tempfile.TemporaryDirectory(prefix="m0-selftest-") as sdir:
        asan_bin = os.path.join(sdir, "fake_asan")
        with open(asan_bin, "wb") as fh:
            fh.write(b"\x7fELF...__asan_init...")
        plain_bin = os.path.join(sdir, "plain")
        with open(plain_bin, "wb") as fh:
            fh.write(b"\x7fELF plain")
        ok("sanitizer: an ASan-instrumented binary is detected", binary_sanitizers(asan_bin) == ["asan"])
        ok("sanitizer: a plain binary reports none", binary_sanitizers(plain_bin) == [])
    ok("quarantine: the cap is appended and existing ASAN_OPTIONS are kept",
       with_asan_quarantine({"ASAN_OPTIONS": "detect_leaks=1"})["ASAN_OPTIONS"] == "detect_leaks=1:quarantine_size_mb=1")
    ok("quarantine: an empty ASAN_OPTIONS gets only the cap",
       with_asan_quarantine({})["ASAN_OPTIONS"] == "quarantine_size_mb=1")
    src_env = {"ASAN_OPTIONS": "x=1"}
    with_asan_quarantine(src_env)
    ok("quarantine: the caller's environment is not modified", src_env == {"ASAN_OPTIONS": "x=1"})

    # ---- RSS window: median over a window, so one spike and start-up growth do not decide the verdict
    ok("rss window: the median ignores one spike", rss_window_median(
        [(0, 100), (1, 101), (2, 9999), (3, 102), (4, 103)], 0, 5) == 102)
    ok("rss window: an even count averages the middle pair", rss_window_median([(0, 100), (1, 200)], 0, 2) == 150.0)
    ok("rss window: the bounds are half-open", rss_window_median([(0, 1), (1, 7)], 1, 2) == 7)
    ok("rss window: an empty window is None", rss_window_median([(0, 1)], 5, 9) is None)
    ok("rss window: start-up growth before the warm-up window does not count",
       rss_verdict("w", rss_window_median([(0, 5000), (5, 1000), (6, 1001)], 5, 7),
                   rss_window_median([(9, 1002)], 9, 10), 8192) == [])

    # ---- protocol variant: oversize capsule.submit line
    big = make_oversize_line(200, method="capsule.submit")
    ok("oversize variant: the line has the requested length", len(big) == 200)
    ok("oversize variant: the padded line is valid JSON for capsule.submit",
       strict_loads(big.decode("ascii"))["method"] == "capsule.submit")

    # ---- T00: pid is excluded from the clean-build identity comparison
    ok("version identity: pid is excluded",
       stable_version_fields({"sutekh": "0.1.0", "pid": "1"}) == {"sutekh": "0.1.0"})
    ok("version identity: a different abi_version is a difference",
       stable_version_fields({"abi_version": "1"}) != stable_version_fields({"abi_version": "2"}))

    # ---- T11 cycle: five kinds, two successful, three refused with the contract codes
    ok("T11 cycle: five kinds, install and revert are the successful ones",
       len(T11_CYCLE) == 5 and T11_SUCCESS == ("install", "revert"))
    ok("T11 refused kinds map to the install-time contract codes",
       T11_EXPECT == {"fail_stale_revision": "SU_E_STALE_REVISION", "fail_past_tick": "SU_E_STALE_REVISION",
                      "fail_layout_generation": "SU_E_SCHEMA_MISMATCH"})

    # ---- T12 probe: absolute-path compiler is a build tool; strace parsing sees an absolute execve
    probe_spec = importlib.util.spec_from_file_location(
        "_probe_selftest", os.path.join(REPO, "tests", "e2e", "no_build_probe.py"))
    probe_mod = importlib.util.module_from_spec(probe_spec)
    probe_spec.loader.exec_module(probe_mod)
    ok("probe: an absolute /usr/bin/cc is a build tool", probe_mod.is_build_tool("/usr/bin/cc"))
    ok("probe: a versioned gcc-13 is a build tool", probe_mod.is_build_tool("/usr/bin/gcc-13"))
    ok("probe: the sutekh binary is not a build tool", not probe_mod.is_build_tool("/x/build/sutekh"))
    recs = probe_mod.parse_strace('1234 1700000000.000100 execve("/usr/bin/cc", ["cc", "--version"], 0x7ff) = 0\n')
    ok("probe: an absolute-path successful execve is parsed", len(recs) == 1 and recs[0]["path"] == "/usr/bin/cc"
       and recs[0]["rc"] == 0)
    summ = probe_mod.summarise(recs, window=(1700000000.0, 1700000001.0))
    ok("probe: an absolute-path cc inside the edit window is counted there",
       len(summ["build_tool_execs_in_window"]) == 1)
    ok("probe: the absolute-path control line is recognised by name",
       probe_mod.is_build_tool(os.path.join(os.sep, "usr", "bin", "cc")))

    failed = [name for name, passed in checks if not passed]
    for name in failed:
        print(f"self-test FAILED: {name}")
    print(f"self-test: {len(checks)} checks, {len(failed)} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
