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
import copy
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
    # M1-02 additive top-level members: a recovery object (on the request that found a dead worker)
    # and a worker_event (on the next reply); M1-02c-2 adds specialization (world.inspect). They are kept
    # when present and absent otherwise.
    extra = {k: obj[k] for k in ("recovery", "worker_event", "specialization") if k in obj}
    if ok:
        if not isinstance(obj.get("result"), dict):
            raise Failed("ok reply has no result object")
        return {"ok": True, "result": obj["result"], "id": obj.get("id"), **extra}
    err = obj.get("error")
    if not isinstance(err, dict) or not isinstance(err.get("code"), str):
        raise Failed("error reply has no error.code")
    return {"ok": False, "error": err, "id": obj.get("id"), **extra}


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
# gates: conditions the measurement must satisfy. Each gate is observed by the harness (or taken from
# the engine's own report) and given as (True | False | None, reason); None means the fact was not
# observed, which makes the verdict not_comparable. caveats: reporting requirements recorded in the
# verdict that do not change it.
TIMING_TARGETS = (
    {"id": "ir_prepare", "row": "IR prepare latency: p95 <= 20 ms (<=64 KiB source, fixed schema, warm process, 1,024-row preview)",
     "metric": "ir_prepare_warm_1024", "p95_ms_max": 20.0,
     "gates": ("source <= 64 KiB", "existing fixed schema", "warm process", "1,024-row preview"),
     "caveats": ("submit (capsule.submit: parse, verify, prepare) and the isolated preview are timed together",)},
    {"id": "preview_response", "row": "Isolated preview response: p95 <= 100 ms (reusable worker, small fixture; excludes long qualification suites)",
     "metric": "preview_reusable_worker", "p95_ms_max": 100.0,
     "gates": ("reusable worker", "small fixture", "excludes long qualification suites"),
     "caveats": ("the timed call is candidate.evaluate on an already submitted candidate",)},
    {"id": "install_critical_section", "row": "Code installation critical section: p95 <= 1 ms (code-only swap, bindings prepared, no migration)",
     "metric": "install_critical_section_engine", "p95_ms_max": 1.0,
     "gates": ("code-only swap", "bindings already prepared", "no migration", "engine clock readable"),
     "caveats": ("measured inside the engine (src/runtime/reload.c): from the start of the registry publish to the "
                 "new schedule entry being in place; the install RPC is reported separately",)},
    {"id": "warm_edit_to_first_effect", "row": "Warm edit to first preview effect: p95 <= 100 ms",
     "metric": "edit_to_first_effect_warm", "p95_ms_max": 100.0,
     "gates": ("warm process",),
     "caveats": ("submit, evaluate, install RPC and first step are also reported separately in latency_ms",)},
    {"id": "native_incremental_check", "row": "Targeted native incremental check: aim p95 <= 2 s (reference machine)",
     "metric": "native_incremental_rebuild", "p95_ms_max": 2000.0,
     "gates": ("small changed target", "cached dependencies", "reference machine"),
     "caveats": ("aim: harness touches src/vm/su_vm.c in a scratch copy and times cmake --build",)},
)
TIMING_VERDICTS = ("met", "not_met", "not_comparable")
REFERENCE_CPU = "14700K"  # docs/PERFORMANCE.md section 3: the user-reported primary development machine


def _p95_valid(stats) -> bool:
    p = stats.get("p95") if isinstance(stats, dict) else None
    return (isinstance(p, (int, float)) and not isinstance(p, bool) and math.isfinite(p)
            and bool(stats.get("n")))


def compare_target(target: dict, stats, gates: dict, caveats=()) -> dict:
    """Verdict for one proposed target. gates maps each gate name to (observed: True | False | None, reason).

    not_comparable  any gate is False or None (None = fact not observed); the measured p95 is still recorded
    met             every gate True and p95 <= target
    not_met         every gate True and p95 > target
    A gate set that differs from the declared set, a malformed gate value, or (with every gate True) a
    missing, empty or non-finite p95 raises ValueError: a target without a measurement is never skipped.
    """
    if set(gates) != set(target["gates"]):
        raise ValueError(f"{target['id']}: gate set {sorted(gates)} differs from declared {sorted(target['gates'])}")
    for name, val in gates.items():
        if not (isinstance(val, tuple) and len(val) == 2 and (val[0] is None or isinstance(val[0], bool))
                and isinstance(val[1], str)):
            raise ValueError(f"{target['id']}: gate {name!r} must be (True | False | None, reason)")
    base = {"id": target["id"], "row": target["row"], "metric": target["metric"], "provisional": True,
            "target_p95_ms_max": target["p95_ms_max"],
            "gates": {k: {"met": v[0], "reason": v[1]} for k, v in gates.items()},
            "caveats": list(target["caveats"]) + list(caveats),
            "note": "provisional: shared VM, C10 (proposed budgets are not measurements)"}
    unmet = [name for name, (ok, _r) in gates.items() if ok is not True]
    if unmet:
        reasons = "; ".join(f"{name}: {gates[name][1]}" + (" (not observed)" if gates[name][0] is None else "")
                            for name in unmet)
        p95 = float(stats["p95"]) if _p95_valid(stats) else None
        n = stats.get("n", 0) if isinstance(stats, dict) else 0
        return {**base, "p95_ms": p95, "n": n, "verdict": "not_comparable",
                "reason": f"conditions not met or not observed ({reasons}); measured p95 {p95} ms"}
    if not _p95_valid(stats):
        raise ValueError(f"{target['id']}: no finite measured p95 for {target['metric']}")
    p95 = float(stats["p95"])
    if p95 <= target["p95_ms_max"]:
        verdict, reason = "met", f"measured p95 {p95} ms <= {target['p95_ms_max']} ms with every condition observed"
    else:
        verdict, reason = "not_met", f"measured p95 {p95} ms > {target['p95_ms_max']} ms with every condition observed"
    return {**base, "p95_ms": p95, "n": stats["n"], "verdict": verdict, "reason": reason}


def identities_stable(keys: list) -> bool:
    """True when there is at least one key, none is None, and all equal the first: one OS process throughout."""
    return bool(keys) and keys[0] is not None and all(k == keys[0] for k in keys)


def pids_reused(pids: list) -> bool:
    """True when at least two evaluations reported a pid and every report is the same integer pid."""
    return (len(pids) >= 2 and all(isinstance(p, int) and not isinstance(p, bool) for p in pids)
            and all(p == pids[0] for p in pids))


def worker_record_mismatches(reported, active_ident, sup_pid) -> list:
    """Checks a worker_pid the engine reports for a candidate against the OS. active_ident is the OS identity
    of the active worker, observed by the harness from the world (not from the candidate record). The
    reported pid must be a live process whose parent is the supervisor and whose identity is that worker."""
    if not isinstance(reported, int) or isinstance(reported, bool):
        return ["candidate record worker_pid is not an integer"]
    if active_ident is None:
        return ["active worker identity is not observable"]
    ident = os_identity(reported)
    if is_dead(ident):
        return [f"candidate record worker_pid {reported} is not a live process"]
    out = []
    if ident["ppid"] != sup_pid:
        out.append(f"candidate record worker_pid {reported} has ppid {ident['ppid']}, not the supervisor {sup_pid}")
    if identity_key(ident) != identity_key(active_ident):
        out.append(f"candidate record worker_pid {reported} is not the active worker (OS identity differs)")
    return out


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
                  "transport, pid, capabilities[], active_worker, process, worker_generation, recoveries, "
                  "recovery{limits, spool_bytes, committed_bytes, worlds[]}",
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
ENGINE_METHODS["world.close"] = {
    "params": "{world_id, expected_world_revision}  (stale revision: SU_E_STALE_REVISION; unknown or closed "
              "world_id: SU_E_INVALID_ARGUMENT)",
    "result": "world_id, closed (true), released{snapshots, systems}",
    "source": "src/agent/su_agent.c h_world_close, src/runtime/worker.c h_world_close",
}

# M2-00c3b entity control plane (src/agent/su_agent.c h_world_spawn/h_world_despawn/h_events_read; worker.c).
# Handles are JSON integers (u64, exact in the engine reader); entity = (generation << 32) | slot.
ENGINE_METHODS["world.spawn"] = {
    "params": "{world_id, expected_world_revision, values: {field: u32}, external_id?: u32}",
    "result": "world_id, world_revision, entity, slot, generation",
    "source": "src/agent/su_agent.c h_world_spawn, src/runtime/worker.c h_world_spawn",
}
ENGINE_METHODS["world.despawn"] = {
    "params": "{world_id, expected_world_revision, entity}  (stale handle: SU_E_STALE_HANDLE)",
    "result": "world_id, world_revision, entity",
    "source": "src/agent/su_agent.c h_world_despawn, src/runtime/worker.c h_world_despawn",
}
ENGINE_METHODS["events.read"] = {
    "params": "{world_id, cursor, max: 1..256}  (cursor above next_seq: SU_E_INVALID_ARGUMENT)",
    "result": "world_id, events[{event_seq, tick, kind, system_index, slot, seq, a, b}], next_cursor, skipped, "
              "dropped",
    "source": "src/agent/su_agent.c h_events_read, src/runtime/worker.c h_events_read",
}

INSPECT_PAGE = 256            # engine maximum for world.inspect limit
INSPECT_FIELDS = ("position_x", "velocity_x")
# Harness-chosen bound, NOT an engine requirement: growth of VmRSS after warm-up, per process.
HARNESS_RSS_GROWTH_BOUND_KIB = 8192
EDIT_V1_TEST = "tests/fixtures/edit/movement.test.json"
EDIT_V2_TEST = "tests/fixtures/edit/movement_v2.test.json"
EDIT_V2_WRONG_TEST = "tests/fixtures/edit/movement_v2_wrong.test.json"
EDIT_V1_WRONG_TEST = "tests/fixtures/edit/movement_v1_wrong.test.json"


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


def close_mismatches(res: dict, world_id: str, systems: int, snapshots: int) -> list:
    """Mismatch list for a world.close result: the world, closed=true and the released counts."""
    out = []
    if res.get("world_id") != world_id:
        out.append(f"world_id: expected {world_id} actual {res.get('world_id')!r}")
    if res.get("closed") is not True:
        out.append(f"closed: expected true actual {res.get('closed')!r}")
    rel = res.get("released") if isinstance(res.get("released"), dict) else {}
    for key, want in (("systems", systems), ("snapshots", snapshots)):
        got = rel.get(key)
        if type(got) is not int or got != want:
            out.append(f"released.{key}: expected {want} actual {got!r}")
    return out


def recovery_mismatches(env, old_pid) -> list:
    """Checks the request that found its worker dead (M1-02): SU_E_WORKER_FAILED with a recovery object
    that reports a successful recovery, the old pid, and a different replacement pid. Pure; used by TX2
    and the self-test."""
    out = []
    if not isinstance(env, dict) or env.get("ok") is not False:
        return ["the request on the dead worker did not fail"]
    err = env.get("error") if isinstance(env.get("error"), dict) else {}
    if err.get("code") != "SU_E_WORKER_FAILED":
        out.append(f"error code: expected SU_E_WORKER_FAILED actual {err.get('code')!r}")
    rec = env.get("recovery")
    if not isinstance(rec, dict):
        return out + ["recovery object missing from the failed request"]
    if rec.get("recovered") is not True:
        out.append(f"recovered: expected true actual {rec.get('recovered')!r}")
    old = rec.get("replaced_pid_old")
    if type(old) is not int or old != old_pid:
        out.append(f"replaced_pid_old: expected {old_pid} actual {old!r}")
    new = rec.get("replaced_pid_new")
    if type(new) is not int or new <= 0 or new == old_pid:
        out.append(f"replaced_pid_new: expected a new positive pid actual {new!r}")
    for key in ("worlds_recovered", "ops_replayed", "duration_ms", "worker_generation"):
        v = rec.get(key)
        if type(v) is not int or v < 0:
            out.append(f"{key}: expected a non-negative integer actual {v!r}")
    if rec.get("unrecoverable") != [] or rec.get("quarantined") != []:
        out.append("unrecoverable and quarantined must be empty after a successful recovery")
    tick = rec.get("from_checkpoint_tick")
    if tick is not None and (type(tick) is not int or tick < 0):
        out.append(f"from_checkpoint_tick: expected a non-negative integer or null actual {tick!r}")
    return out


def close_world(tctx, sess, wid, systems=1, snapshots=0) -> dict:
    """world.close at the revision the harness tracks; the world is then forgotten by the session."""
    rev = sess.revs.get(wid)
    if not isinstance(rev, str):
        raise Failed(f"harness has no tracked world_revision for {wid}")
    res = call(tctx, sess, "world.close", {"world_id": wid, "expected_world_revision": rev})
    tctx.check_mismatches(f"world.close {wid}", close_mismatches(res, wid, systems, snapshots))
    sess.revs.pop(wid, None)
    return res


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
    removed if present, configured (same build type as the binary under test: Debug or Release) and built. The clean binary's version
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
    # The clean reference is built with the same CMAKE_BUILD_TYPE the binary under test reports
    # (Debug or Release); any other value is a harness error, not a reason to guess.
    if not os.path.isfile(t.binary):
        raise Failed(f"binary under test not found: {t.binary}")
    _rc0, out_probe = t.run_cmd([t.binary, "--version"], timeout=30)
    probe_type = parse_version_output(out_probe).get("build_type")
    if probe_type not in ("Debug", "Release"):
        raise Failed(f"binary under test reports unsupported build_type {probe_type!r}; T00 supports Debug and Release")
    t.details["clean_build_type"] = probe_type
    argv = ["cmake", "-S", REPO, "-B", bdir, f"-DCMAKE_BUILD_TYPE={probe_type}"] + (["-G", gen] if gen else [])
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
        wid = drive(t, sess, world_rel, [cap_rel], [(case["ticks"], derived)], case["name"])
        # Each world is closed after its checks, so one serve session serves every case.
        close_world(t, sess, wid)


def t02(t: TestCtx):
    cap_rel = "tests/fixtures/t02/movement.capsule.json"
    cap, lir_doc, _ = t.load_system(cap_rel)
    counts = [0, 1, 7, 8, 9, 1024]
    seen = {}
    sess = t.session("T02 serve")
    for n in counts:
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
    sess = t.session("T03 serve")
    for name in names:
        d = f"{base}/{name}"
        cap_rel, world_rel = f"{d}/capsule.json", f"{d}/world.json"
        cap, lir_doc, _ = t.load_system(cap_rel)
        test_doc = t.load(f"{d}/test.json")
        _t02_or_t03_case(t, sess, world_rel, cap_rel, test_doc["cases"], lir_doc, cap)
        checked.append(name)
    sess.close()
    t.details["cases_checked"] = checked


def tx1(t: TestCtx):
    """World close beyond the 4-world cap (extra check; not a TEST_SPEC id). One serve session creates
    more worlds than a worker holds, closes them, reuses the slots and never reuses an id."""
    world_rel = "examples/world.json"
    cap_rel = "examples/movement.capsule.json"
    cap, lir_doc, _ = t.load_system(cap_rel)
    initial = t.load(world_rel)["columns"]
    sess = t.session("TX1 serve")
    seen_ids = []
    expected_by_id = {}

    def make(ticks):
        wid, _rev = create_world(t, sess, world_rel, [cap_rel])
        seen_ids.append(wid)
        if ticks:
            step(t, sess, wid, ticks)
        expected = ref.run_ticks(lir_doc, cap, initial, ticks)
        expected_by_id[wid] = (ticks, expected)
        t.check_mismatches(f"TX1 {wid} after {ticks} ticks", verify_state(ticks, expected, inspect(t, sess, wid)))
        return wid

    def refused(method, params, code, label):
        t.check_mismatches(label, classify_error(sess.request(method, params), code))

    held = [make(k) for k in range(4)]
    refused("world.create", {"fixture_path": world_rel, "capsule_path": cap_rel}, "SU_E_RESOURCE_LIMIT",
            "TX1 fifth live world refused with SU_E_RESOURCE_LIMIT")

    first = held[0]
    refused("world.close", {"world_id": first, "expected_world_revision": f"{first}.r999"}, "SU_E_STALE_REVISION",
            "TX1 stale close refused with SU_E_STALE_REVISION")
    ticks0, exp0 = expected_by_id[first]
    t.check_mismatches("TX1 stale close left the world untouched",
                       verify_state(ticks0, exp0, inspect(t, sess, first)))

    close_world(t, sess, first)
    refused("world.inspect", {"world_id": first, "limit": 1}, "SU_E_INVALID_ARGUMENT",
            "TX1 inspect of a closed world refused with SU_E_INVALID_ARGUMENT")
    refused("world.step", {"world_id": first, "expected_world_revision": f"{first}.r1", "ticks": 1},
            "SU_E_INVALID_ARGUMENT", "TX1 step of a closed world refused with SU_E_INVALID_ARGUMENT")
    refused("world.close", {"world_id": first, "expected_world_revision": f"{first}.r1"}, "SU_E_INVALID_ARGUMENT",
            "TX1 second close refused with SU_E_INVALID_ARGUMENT")

    # The slot is free again: three live worlds plus one new world is four, and the id is new.
    close_world(t, sess, make(2))
    for k in range(8):
        wid = make(k % 4)
        close_world(t, sess, wid)
    for wid in held[1:]:
        close_world(t, sess, wid)
    sess.close()

    t.check("TX1 created more worlds than a worker holds", True, len(seen_ids) > 4)
    t.check("TX1 world ids are never reused within one session", len(seen_ids), len(set(seen_ids)))
    t.details["world_ids_issued"] = len(seen_ids)


def tx2(t: TestCtx):
    """Worker crash recovery through the real binary (extra check; not a TEST_SPEC id). The active worker of a
    `serve` session is killed from outside, after accepted steps and after an accepted install. The request that
    finds the dead worker reports SU_E_WORKER_FAILED with a recovery object; the replacement is a live child of the
    supervisor (OS facts); and every tick afterwards matches the independent reference evaluator."""
    sess = t.session("TX2 serve")
    sched, expected = _swap_expected(t)
    swap_tick = sched["swap"]["effective_tick"]
    total = sched["total_ticks"]
    sup_pid = supervisor_pid(t, sess)
    wid, _rev0 = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    t.check_mismatches("TX2 tick 0", verify_state(0, expected[0], inspect(t, sess, wid)))

    def kill_active_worker(label):
        pid = call(t, sess, "engine.info", {}).get("active_worker", {}).get("pid")
        if type(pid) is not int or pid <= 0:
            raise Failed("engine.info does not report the active worker pid")
        t.check(f"TX2 {label}: worker pid is a live child of the supervisor (OS ppid)", True,
                (identity_key(os_identity(pid)) or (0, 0, 0))[2] == sup_pid)
        os.kill(pid, signal.SIGKILL)
        env = sess.request("world.step", {"world_id": wid, "expected_world_revision": sess.revs[wid], "ticks": 1})
        t.check_mismatches(f"TX2 {label}: request that found the dead worker", recovery_mismatches(env, pid))
        new = env.get("recovery", {}).get("replaced_pid_new")
        t.check(f"TX2 {label}: the killed worker is gone (OS)", None, identity_key(os_identity(pid)))
        t.check(f"TX2 {label}: the replacement is a live child of the supervisor (OS ppid)", True,
                (identity_key(os_identity(new)) or (0, 0, 0))[2] == sup_pid)
        return pid, new

    step(t, sess, wid, 1)
    t.check_mismatches("TX2 tick 1", verify_state(1, expected[1], inspect(t, sess, wid)))
    old1, new1 = kill_active_worker("after steps")
    t.check("TX2 replacement differs from the killed worker", True, new1 != old1)
    t.check_mismatches("TX2 tick 1 after recovery", verify_state(1, expected[1], inspect(t, sess, wid)))
    step(t, sess, wid, 1)
    t.check_mismatches(f"TX2 tick 2 (retry after recovery)", verify_state(2, expected[2], inspect(t, sess, wid)))

    cid = prepare(t, sess, EDIT_V2, V2_SRC, EDIT_V2_TEST)
    install(t, sess, wid, cid, sess.revs[wid], swap_tick)
    t.check_mismatches(f"TX2 install at tick {swap_tick} changes no column",
                       verify_state(swap_tick, expected[swap_tick], inspect(t, sess, wid)))
    old2, _new2 = kill_active_worker("after install")
    for tick in range(swap_tick + 1, total + 1):
        step(t, sess, wid, 1)
        t.check_mismatches(f"TX2 tick {tick} (v2 from effective tick {swap_tick}, after recovery)",
                           verify_state(tick, expected[tick], inspect(t, sess, wid)))
    info = call(t, sess, "engine.info", {})
    t.check("TX2 engine.info counts two recoveries", 2, info.get("recoveries"))
    t.check("TX2 worker generation is one spawn per worker process", True,
            isinstance(info.get("worker_generation"), int) and info["worker_generation"] >= 3)
    t.check("TX2 the last killed pid is not the active worker", True,
            (info.get("active_worker") or {}).get("pid") not in (old1, old2))
    close_world(t, sess, wid)


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


MAX_ROWS_CAP = "tests/fixtures/t04/movement_max_rows_4.capsule.json"
MAX_ROWS_WORLD = "tests/fixtures/t02/world_8.json"
MOVEMENT_CAP = "tests/fixtures/t02/movement.capsule.json"


def _max_rows_case(t) -> dict:
    """Runtime part of T04: a capsule whose limits.max_rows is below the world's row count must make world.step
    fail with SU_E_RESOURCE_LIMIT, and the committed root must not change (su_vm.c row limit). A control with
    the unmodified capsule on the same world must step, so the failure is the limit and not the world."""
    rows = _rows_of(t.load(MAX_ROWS_WORLD)["columns"])
    limit = t.load(MAX_ROWS_CAP)["limits"]["max_rows"]
    t.check("T04 max_rows fixture: limit is positive and below the world row count", True, 0 < limit < rows)
    ctl = t.session("T04 max_rows control serve")
    wid_c, rev_c = create_world(t, ctl, MAX_ROWS_WORLD, [MOVEMENT_CAP])
    env_c = ctl.request("world.step", {"world_id": wid_c, "expected_world_revision": rev_c, "ticks": 1})
    t.check("T04 max_rows control: the unmodified capsule steps on the same world", True, env_c["ok"])
    sess = t.session("T04 max_rows serve")
    wid, rev = create_world(t, sess, MAX_ROWS_WORLD, [MAX_ROWS_CAP])
    before = inspect(t, sess, wid)
    env = sess.request("world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": 1})
    if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
        raise Unavailable("world.step answered SU_E_UNSUPPORTED_METHOD")
    mism = classify_error(env, "SU_E_RESOURCE_LIMIT")
    mism += verify_rejections_leave_state(before, inspect(t, sess, wid))
    t.check_mismatches("T04 max_rows: world.step over capsule limits.max_rows is SU_E_RESOURCE_LIMIT "
                       "with the committed root unchanged", mism)
    t.check("T04 session answers after the max_rows failure", True, sess.request("engine.info", {})["ok"])
    return {"world": MAX_ROWS_WORLD, "rows": rows, "max_rows": limit, "control_stepped": env_c["ok"],
            "step_ok": env["ok"], "code": None if env["ok"] else env["error"].get("code")}


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
    t.details["max_rows_case"] = _max_rows_case(t)
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
    rec = call(t, sess, "candidate.status", {"candidate_id": cid})
    t.check_mismatches("T05 candidate record names the live active worker (OS ppid and identity)",
                       worker_record_mismatches(rec.get("worker_pid"), os_identity(worker_pid), pid0))
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


T11_CYCLE = ("install", "revert", "fail_stale_revision", "fail_past_tick", "fail_layout_generation",
             "fail_evidence")
T11_SUCCESS = ("install", "revert")
T11_EXPECT = {"fail_stale_revision": "SU_E_STALE_REVISION",
              "fail_past_tick": "SU_E_STALE_REVISION",
              "fail_layout_generation": "SU_E_SCHEMA_MISMATCH",
              "fail_evidence": "SU_E_CANDIDATE_FAILED"}
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
    """Repeated reload (TEST_SPEC T11): 1,000 iterations by default, in a fixed six-kind cycle.

      install               successful install of the other version (v1 <-> v2), then one tick
      revert                successful install back to the previous version (install v2, re-install v1)
      fail_stale_revision   install with the world revision from before the previous iteration -> SU_E_STALE_REVISION
      fail_past_tick        install with allowed_effective_tick one behind the world -> SU_E_STALE_REVISION
                            (src/runtime/reload.c requires allowed_effective_tick == current tick)
      fail_layout_generation a candidate with layout_generation 2 that evaluates correctly -> SU_E_SCHEMA_MISMATCH
      fail_evidence         a candidate of the other version whose evaluation FAILED (one expectation wrong) ->
                            SU_E_CANDIDATE_FAILED; the engine refuses it on its own evidence record

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
    # Candidates whose evaluation fails: valid capsules, one expected value wrong on purpose.
    fail_cid = {}
    for name, cap_rel, src_rel, wrong_rel in (("v1", EDIT_V1, V1_SRC, EDIT_V1_WRONG_TEST),
                                              ("v2", EDIT_V2, V2_SRC, EDIT_V2_WRONG_TEST)):
        fid = submit(t, sess, cap_rel, src_rel)
        fev = evaluate(t, sess, fid, wrong_rel)
        t.check(f"T11 {name} wrong-expectation candidate evaluates as failed", "failed", fev.get("status"))
        fail_cid[name] = fid
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
            elif kind == "fail_layout_generation":
                env = _raw_install(sess, wid, layout_cid, pre["world_revision"], pre["tick"])
            else:  # fail_evidence: the other version's failed-evaluation candidate
                env = _raw_install(sess, wid, fail_cid[other], pre["world_revision"], pre["tick"])
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
    # The candidate record of an installed candidate names the live active worker (OS facts, not the record).
    rec = call(t, sess, "candidate.status", {"candidate_id": v2_cid})
    t.check_mismatches("T11 candidate record names the live active worker after the reloads",
                       worker_record_mismatches(rec.get("worker_pid"), os_identity(worker0), sup))
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


W_UP = 3                     # untimed warm-up cycles before each warm series
NATIVE_MIN_SAMPLES = 5       # TEST_SPEC/M0-14: the incremental check needs n >= 5
IR_WORLD = "tests/fixtures/t02/world_1024.json"
IR_CAP = "tests/fixtures/t02/movement.capsule.json"
IR_SRC = "tests/fixtures/t02/movement.lir.json"
IR_TEST = "tests/fixtures/t02/movement_1024.test.json"
NATIVE_EDIT_FILE = "src/vm/su_vm.c"


def _schema_fixed(cap: dict, world: dict) -> bool:
    """The capsule's schema identity equals the world fixture's (id and version)."""
    sch = cap.get("schema") if isinstance(cap, dict) else None
    return isinstance(sch, dict) and sch.get("id") == world.get("schema_id") and sch.get("version") == world.get("schema_version")


def _ir_warm_series(t, reps):
    """capsule.submit + candidate.evaluate of the movement capsule against the 1,024-row preview, in one serve
    process after W_UP untimed cycles. Also returns the OS facts that back the warm-process and row gates."""
    sess = t.session("T13 serve ir_prepare warm (1,024-row preview)")
    try:
        wid, _ = create_world(t, sess, IR_WORLD, [IR_CAP])
        sup = sess.proc.pid
        sup0 = identity_key(os_identity(sup))
        worker0 = inspect(t, sess, wid).get("worker_pid")
        w0 = identity_key(os_identity(worker0))
        sup_keys, worker_keys, samples, cases_seen = [], [], [], []
        for i in range(W_UP + reps):
            t0 = time.monotonic()
            cid = submit(t, sess, IR_CAP, IR_SRC)
            ev = evaluate(t, sess, cid, IR_TEST)
            t1 = time.monotonic()
            t.check(f"T13 ir_prepare warm cycle {i} evaluation passed", "passed", ev.get("status"))
            sup_keys.append(identity_key(os_identity(sup)))
            worker_keys.append(identity_key(os_identity(worker0)))
            cases_seen.append(ev.get("cases_total"))
            if i >= W_UP:
                samples.append(t1 - t0)
        worker1 = inspect(t, sess, wid).get("worker_pid")
        return {"samples": samples, "cases_total": cases_seen,
                "supervisor_stable": identities_stable(sup_keys) and sup0 is not None,
                "worker_stable": identities_stable(worker_keys) and worker1 == worker0 and w0 is not None,
                "warm_up_cycles": W_UP,
                "supervisor_pid": sup, "worker_pid": worker0}
    finally:
        sess.close()


def _edit_warm_series(t, reps, swap_tick):
    """One serve process: W_UP untimed cycles, then reps timed cycles of submit -> evaluate -> install ->
    first step. Then the same candidate is evaluated reps times to observe whether the evaluation worker is reused."""
    sess = t.session("T13 serve warm edit")
    try:
        wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
        step(t, sess, wid, swap_tick)
        sup = sess.proc.pid
        sup_keys, worker_keys, reported_workers = [identity_key(os_identity(sup))], [], []
        worker0 = inspect(t, sess, wid).get("worker_pid")
        w0 = identity_key(os_identity(worker0))
        ser = {"edit": [], "submit": [], "evaluate": [], "install": [], "step": []}
        install_facts = {"prepared": [], "critical_ns": [], "prepare_ns": [], "states": []}
        for i in range(W_UP + reps):
            pre = inspect(t, sess, wid)
            t0 = time.monotonic()
            cid = submit(t, sess, EDIT_V2, V2_SRC)
            t1 = time.monotonic()
            ev = evaluate(t, sess, cid, EDIT_V2_TEST)
            t2 = time.monotonic()
            t.check(f"T13 warm edit cycle {i} evaluation passed", "passed", ev.get("status"))
            res = install(t, sess, wid, cid, pre["world_revision"], pre["tick"])
            t3 = time.monotonic()
            step(t, sess, wid, 1)
            t4 = time.monotonic()
            sup_keys.append(identity_key(os_identity(sup)))
            worker_keys.append(identity_key(os_identity(worker0)))
            reported_workers.append(res.get("worker_pid"))
            if i >= W_UP:
                ser["edit"].append(t4 - t0)
                ser["submit"].append(t1 - t0)
                ser["evaluate"].append(t2 - t1)
                ser["install"].append(t3 - t2)
                ser["step"].append(t4 - t3)
                install_facts["prepared"].append(res.get("bindings_prepared") is True)
                install_facts["critical_ns"].append(res.get("critical_section_ns"))
                install_facts["prepare_ns"].append(res.get("prepare_ns"))
                install_facts["states"].append(res.get("state"))
        # Evaluation worker reuse: the same candidate evaluated repeatedly; the record names each worker.
        cid = submit(t, sess, EDIT_V2, V2_SRC)
        reuse_s, reuse_pids, reuse_cases = [], [], []
        for i in range(reps):
            t0 = time.monotonic()
            ev = evaluate(t, sess, cid, EDIT_V2_TEST)
            reuse_s.append(time.monotonic() - t0)
            t.check(f"T13 reusable evaluation {i} passed", "passed", ev.get("status"))
            rec = call(t, sess, "candidate.status", {"candidate_id": cid})
            reuse_pids.append(rec.get("worker_pid"))
            reuse_cases.append(ev.get("cases_total"))
        worker1 = inspect(t, sess, wid).get("worker_pid")
        return {"series": ser, "install": install_facts, "reuse_s": reuse_s, "reuse_pids": reuse_pids,
                "reuse_cases_total": reuse_cases,
                "supervisor_stable": identities_stable(sup_keys) and sup_keys[0] is not None,
                "worker_stable": identities_stable(worker_keys) and worker1 == worker0 and w0 is not None,
                "reported_worker_pids_equal": all(p == worker0 for p in reported_workers),
                "warm_up_cycles": W_UP, "supervisor_pid": sup, "worker_pid": worker0}
    finally:
        sess.close()


def _build_type(t) -> str:
    _rc, out = t.run_cmd([t.binary, "--version"], timeout=30)
    bt = parse_version_output(out).get("build_type")
    if bt not in ("Debug", "Release"):
        raise Failed(f"binary under test reports unsupported build_type {bt!r} for the native check")
    return bt


def _native_incremental(t, n):
    """Incremental native rebuild: configure and build a scratch copy of the repository once (untimed), then
    n times touch src/vm/su_vm.c and time `cmake --build`. The repository itself is not modified."""
    build_type = _build_type(t)
    root = tempfile.mkdtemp(prefix="m0-native-inc-")  # outside the repository
    res = {"build_type": build_type, "samples_s": [], "compile_units": [], "vm_compiled": [],
           "reconfigured": [], "build_rc": [], "generator": "Ninja" if shutil.which("ninja") else "default"}
    try:
        src = os.path.join(root, "repo")
        shutil.copytree(REPO, src, ignore=shutil.ignore_patterns(".git", ".claude", "build*", "planning", "third_party", "bench", "docs", "__pycache__"))
        bdir = os.path.join(root, "build")
        gen = ["-G", "Ninja"] if shutil.which("ninja") else []
        res["configure_rc"], _ = t.run_cmd(["cmake", "-S", src, "-B", bdir, f"-DCMAKE_BUILD_TYPE={build_type}"] + gen,
                                           timeout=900)
        res["full_build_rc"], _ = t.run_cmd(["cmake", "--build", bdir, "--parallel"], timeout=1800)
        vm = os.path.join(src, *NATIVE_EDIT_FILE.split("/"))
        for _i in range(n):
            os.utime(vm, None)  # the touch: content unchanged, modification time advanced
            t0 = time.monotonic()
            rc, text = t.run_cmd(["cmake", "--build", bdir, "--parallel"], timeout=900)
            dt = time.monotonic() - t0
            lines = text.splitlines()
            compiles = [ln for ln in lines if "Building C object" in ln]
            res["samples_s"].append(dt)
            res["build_rc"].append(rc)
            res["compile_units"].append(len(compiles))
            res["vm_compiled"].append(any("su_vm.c" in ln for ln in compiles))
            res["reconfigured"].append(any("Re-running CMake" in ln or "Configuring done" in ln for ln in lines))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    res["scratch_root_removed"] = not os.path.exists(root)
    return res


def t13(t: TestCtx):
    """Latency measurement compared with the proposed targets in docs/PERFORMANCE.md section 2.

    Warm series (the figures the targets are compared with) run in one serve process after untimed warm-up
    cycles; the process identity is read from the OS on every cycle:
      ir_prepare_warm_1024            capsule.submit + candidate.evaluate, 1,024-row preview
      preview_reusable_worker         candidate.evaluate of one candidate repeated (evaluation worker pid recorded)
      install_critical_section_engine critical_section_ns reported by candidate.install (engine clock)
      edit_to_first_effect_warm       submit -> evaluate -> install -> first step
      native_incremental_rebuild      touch src/vm/su_vm.c in a scratch copy; cmake --build (n >= 5)
    Cold series (fresh serve process per sample) are reported, not compared.

    Gate facts are observed, never assumed: each one is True, False or None (not observed) and is checked
    against the OS, the engine's own report, or the fixture files. Each target gets a verdict met / not_met /
    not_comparable with its reason and every gate. All verdicts are provisional: the host is a shared VM (C10).
    The test passes when measurements are collected and a verdict is recorded for every target; the verdicts
    are report fields, not the test status.
    """
    sched, _expected = _swap_expected(t)
    swap_tick = sched["swap"]["effective_tick"]
    reps = t.run.latency_samples

    # Cold: fresh serve process per sample. Reported only.
    cold_prep, cold_eval, cold_edit = [], [], []
    for i in range(reps):
        sess = t.session(f"T13 cold sample {i}")
        wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
        step(t, sess, wid, swap_tick)
        pre = inspect(t, sess, wid)
        t0 = time.monotonic()
        cid = submit(t, sess, EDIT_V2, V2_SRC)
        t1 = time.monotonic()
        ev = evaluate(t, sess, cid, EDIT_V2_TEST)
        t2 = time.monotonic()
        t.check(f"T13 cold sample {i} evaluation passed", "passed", ev.get("status"))
        install(t, sess, wid, cid, pre["world_revision"], swap_tick)
        step(t, sess, wid, 1)
        t3 = time.monotonic()
        cold_prep.append(t2 - t0)
        cold_eval.append(t2 - t1)
        cold_edit.append(t3 - t0)
        sess.close()

    ir = _ir_warm_series(t, reps)
    ed = _edit_warm_series(t, reps, swap_tick)
    native = _native_incremental(t, max(NATIVE_MIN_SAMPLES, reps))

    # Gate facts, from the fixtures and the observations above.
    v1_cap, v2_cap = t.load(EDIT_V1), t.load(EDIT_V2)
    edit_world = t.load(EDIT_WORLD)
    ir_world, ir_cap = t.load(IR_WORLD), t.load(IR_CAP)
    ir_test = t.load(IR_TEST)
    ir_src_bytes = os.path.getsize(t.abspath(IR_SRC))
    ir_rows = _rows_of(ir_world["columns"])
    ir_resolved = os.path.normpath(os.path.join(os.path.dirname(IR_TEST), ir_test["world"])).replace(os.sep, "/")
    edit_rows = _rows_of(edit_world["columns"])
    edit_cases = len(t.load(EDIT_V2_TEST)["cases"])
    same_layout = v1_cap["schema"]["layout_generation"] == v2_cap["schema"]["layout_generation"]
    same_schema = _schema_signature(v1_cap) == _schema_signature(v2_cap)
    cs_ns = ed["install"]["critical_ns"]
    clock_ok = all(isinstance(v, int) and not isinstance(v, bool) for v in cs_ns) and len(cs_ns) > 0
    cs_stats = latency_stats([v / 1e9 for v in cs_ns if isinstance(v, int) and not isinstance(v, bool)])
    prep_ns = ed["install"]["prepare_ns"]
    prep_stats = latency_stats([v / 1e9 for v in prep_ns if isinstance(v, int) and not isinstance(v, bool)])
    machine = machine_info()
    cpu = machine.get("cpu_model") or ""
    native_s = native["samples_s"]
    native_ok = (native["configure_rc"] == 0 and native["full_build_rc"] == 0 and len(native_s) >= NATIVE_MIN_SAMPLES
                 and all(rc == 0 for rc in native["build_rc"]))
    native_stats = latency_stats(native_s)

    gates = {
        "ir_prepare": {
            "source <= 64 KiB": (ir_src_bytes <= 65536, f"source is {ir_src_bytes} bytes"),
            "existing fixed schema": (_schema_fixed(ir_cap, ir_world),
                                      "capsule schema id and version equal the world fixture's"
                                      if _schema_fixed(ir_cap, ir_world) else "capsule schema differs from the world fixture"),
            "warm process": (ir["supervisor_stable"] and ir["worker_stable"] and ir["warm_up_cycles"] >= 1,
                             f"one serve process (pid {ir['supervisor_pid']}) and active worker (pid "
                             f"{ir['worker_pid']}) kept their OS identity across {W_UP} warm-up and {len(ir['samples'])} "
                             "timed cycles" if ir["supervisor_stable"] and ir["worker_stable"]
                             else "the serve process or worker identity changed during the series"),
            "1,024-row preview": (ir_rows == 1024 and ir_resolved == IR_WORLD,
                                  f"evaluation resolves {ir_resolved} with {ir_rows} rows"),
        },
        "preview_response": {
            "reusable worker": (pids_reused(ed["reuse_pids"]),
                                "the evaluation worker pid was the same across repeated evaluations"
                                if pids_reused(ed["reuse_pids"]) else
                                "each evaluation ran in a new worker process (candidate.evaluate spawns a disposable "
                                "worker per request, su_agent.c h_candidate_evaluate); the pids reported were "
                                f"{len(set(ed['reuse_pids']))} distinct values in {len(ed['reuse_pids'])} evaluations"),
            "small fixture": (edit_rows <= 16 and edit_cases <= 8, f"{edit_rows} rows, {edit_cases} cases"),
            "excludes long qualification suites": (
                all(c == edit_cases for c in ed["reuse_cases_total"]) and len(ed["reuse_cases_total"]) > 0,
                f"the engine reported cases_total {edit_cases} for every evaluation: only the edit test file ran"),
        },
        "install_critical_section": {
            "code-only swap": (same_schema and same_layout and all(s == "installed" for s in ed["install"]["states"]),
                               "same schema and layout_generation as the installed capsule; every install installed"
                               if same_schema and same_layout else "capsule schema or layout differs"),
            "bindings already prepared": (all(ed["install"]["prepared"]) and len(ed["install"]["prepared"]) > 0,
                                          "candidate.install reported bindings_prepared=true for every sample"),
            "no migration": (same_layout, "layout_generation unchanged" if same_layout else "layout changed"),
            "engine clock readable": (clock_ok, "critical_section_ns is an integer for every install" if clock_ok
                                      else "critical_section_ns was null (monotonic clock unreadable)"),
        },
        "warm_edit_to_first_effect": {
            "warm process": (ed["supervisor_stable"] and ed["worker_stable"] and ed["reported_worker_pids_equal"]
                             and ed["warm_up_cycles"] >= 1,
                             f"one serve process (pid {ed['supervisor_pid']}) and its active worker (pid "
                             f"{ed['worker_pid']}) kept their OS identity; {W_UP} untimed warm-up cycles"
                             if ed["supervisor_stable"] and ed["worker_stable"] else
                             "the serve process or active worker identity changed during the series"),
        },
        "native_incremental_check": {
            "small changed target": (native_ok and all(native["vm_compiled"]) and all(c == 1 for c in native["compile_units"]),
                                     "each incremental build compiled exactly one unit, src/vm/su_vm.c"),
            "cached dependencies": (native_ok and not any(native["reconfigured"]),
                                    "no reconfigure in any incremental build; all other units came from the cache"),
            "reference machine": (REFERENCE_CPU in cpu, f"CPU model is {cpu!r}; the reference is the i7-14700K "
                                                         "(docs/PERFORMANCE.md section 3)"),
        },
    }
    metrics = {
        "ir_prepare_warm_1024": latency_stats(ir["samples"]),
        "preview_reusable_worker": latency_stats(ed["reuse_s"]),
        "install_critical_section_engine": cs_stats if clock_ok else {"n": 0, "units": "ms"},
        "edit_to_first_effect_warm": latency_stats(ed["series"]["edit"]),
        "native_incremental_rebuild": native_stats if native_ok else {"n": 0, "units": "ms"},
    }
    verdicts = [compare_target(tg, metrics[tg["metric"]], gates[tg["id"]]) for tg in TIMING_TARGETS]
    t.check("T13 a verdict is recorded for every proposed target",
            [tg["id"] for tg in TIMING_TARGETS], [v["id"] for v in verdicts])
    t.check("T13 every verdict is met, not_met or not_comparable", True,
            all(v["verdict"] in TIMING_VERDICTS and v["reason"] for v in verdicts))
    t.check("T13 measured samples equal the requested count", [reps, reps, reps, reps, max(NATIVE_MIN_SAMPLES, reps)],
            [len(ir["samples"]), len(ed["reuse_s"]), len(ed["series"]["edit"]), len(cold_edit), len(native_s)])
    t.details["latency_ms"] = {
        "cold_prepare_submit_and_evaluate": latency_stats(cold_prep),
        "cold_evaluate_fresh_session": latency_stats(cold_eval),
        "cold_edit_to_first_effect": latency_stats(cold_edit),
        "ir_prepare_warm_1024": metrics["ir_prepare_warm_1024"],
        "preview_reusable_worker": metrics["preview_reusable_worker"],
        "warm_submit": latency_stats(ed["series"]["submit"]),
        "warm_evaluate": latency_stats(ed["series"]["evaluate"]),
        "warm_install_rpc": latency_stats(ed["series"]["install"]),
        "warm_first_step_after_install": latency_stats(ed["series"]["step"]),
        "edit_to_first_effect_warm": metrics["edit_to_first_effect_warm"],
        "native_incremental_rebuild": native_stats,
    }
    t.details["engine_timings_ms"] = {"critical_section_ns_to_ms": cs_stats, "prepare_ns_to_ms": prep_stats,
                                      "critical_section_ns_raw": cs_ns, "prepare_ns_raw": prep_ns}
    t.details["native_incremental"] = native
    t.details["gate_facts"] = {"warm_identities": {"ir_prepare": {k: ir[k] for k in ("supervisor_stable", "worker_stable", "warm_up_cycles")},
                                                   "warm_edit": {k: ed[k] for k in ("supervisor_stable", "worker_stable", "warm_up_cycles")}},
                               "reuse_worker_pids": ed["reuse_pids"], "reuse_cases_total": ed["reuse_cases_total"]}
    t.details["timing_targets"] = verdicts
    t.details["timing_target_source"] = "docs/PERFORMANCE.md section 2 (proposed, unmeasured there)"
    t.details["timing_target_note"] = (f"provisional: shared VM (C10). Warm series use {reps} timed cycles after "
                                       f"{W_UP} untimed warm-up cycles; p95 is nearest-rank, so with fewer than 20 "
                                       "samples it is close to the maximum.")
    t.details["decomposition_limits"] = ("candidate.install is one RPC. The engine reports prepare_ns and "
                                         "critical_section_ns from the monotonic clock; queue and worker "
                                         "transport are inside the RPC figure only.")
    t.details["machine"] = machine
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

SPEC_WORLD = "examples/world.json"
SPEC_CAP = "examples/movement.capsule.json"
SPEC_SYSTEM = "demo.movement"


def _spec_envelope(tctx, sess, wid, rev, backend, system=SPEC_SYSTEM, extra=None):
    params = {"world_id": wid, "expected_world_revision": rev, "system_id": system, "backend": backend}
    params.update(extra or {})
    return sess.request("world.specialize", params)


def tx3(t: TestCtx):
    """Native specialization through the real binary (extra check; not a TEST_SPEC id). Skipped with the
    reason when engine.info does not advertise world.specialize (default build). For scalar and avx2 (when the
    host reports it): the movement system is specialized in one world at tick 10 while a VM-only control world
    keeps ticking; both worlds are compared column by column at every tick to tick 50, and tick 50 is also
    compared with the independent reference evaluator. The world revision does not change. Rejections: stale
    revision, stale semantic hash, unknown system, unknown backend, and avx2 refusal on a host without it."""
    cap, lir_doc, _ = t.load_system(SPEC_CAP)
    initial = t.load(SPEC_WORLD)["columns"]
    sess = t.session("TX3 serve")
    info = call(t, sess, "engine.info", {})
    host = info.get("native", {}).get("specialization", {}).get("host_caps", {})
    backends = [("scalar", "native_scalar")]
    if host.get("avx2") is True:
        backends.append(("avx2", "native_avx2"))
    else:
        t.details["avx2"] = "host does not report AVX2; avx2 refusal checked instead"
    for backend, tier in backends:
        ctrl, ctrl_rev = create_world(t, sess, SPEC_WORLD, [SPEC_CAP])
        nat, nat_rev = create_world(t, sess, SPEC_WORLD, [SPEC_CAP])
        for _ in range(10):
            step(t, sess, ctrl, 1)
            step(t, sess, nat, 1)
        res = call(t, sess, "world.specialize", {"world_id": nat, "expected_world_revision": sess.revs[nat],
                                                 "system_id": SPEC_SYSTEM, "backend": backend})
        t.check(f"TX3 {backend} state", "installed", res.get("state"))
        t.check(f"TX3 {backend} tier", tier, res.get("execution_tier"))
        t.check(f"TX3 {backend} mismatches", 0, res.get("source", {}).get("admission", {}).get("mismatches"))
        t.check(f"TX3 {backend} artifact_hash is 64 hex", True,
                isinstance(res.get("artifact_hash"), str) and len(res["artifact_hash"]) == 64)
        t.check(f"TX3 {backend} revision unchanged by specialize", sess.revs[nat], res.get("world_revision"))
        # M1-02c-1 (C07): admission ran in a disposable worker, over the corpus (42) and the system's own
        # test cases (3 in examples/movement.test.json).
        t.check(f"TX3 {backend} admission in a disposable worker", "disposable_worker",
                res.get("source", {}).get("kind"))
        t.check(f"TX3 {backend} admission covers the 42 corpus and 3 own cases", 45,
                res.get("source", {}).get("admission", {}).get("cases"))
        for tick in range(11, 51):
            step(t, sess, ctrl, 1)
            step(t, sess, nat, 1)
            c_state = inspect(t, sess, ctrl)
            n_state = inspect(t, sess, nat)
            t.check(f"TX3 {backend} tick {tick} native == VM control", c_state.get("columns"), n_state.get("columns"))
        derived = ref.run_ticks(lir_doc, cap, initial, 50)
        t.check_mismatches(f"TX3 {backend} tick 50 vs reference",
                           verify_state(50, derived, inspect(t, sess, nat)))
        # Rejections leave the world on its tier and revision unchanged.
        env = _spec_envelope(t, sess, nat, "bogus-rev", "scalar")
        t.check_mismatches(f"TX3 {backend} stale revision", classify_error(env, "SU_E_STALE_REVISION"))
        env = _spec_envelope(t, sess, nat, sess.revs[nat], "scalar", system="no.such.system")
        t.check_mismatches(f"TX3 {backend} unknown system", classify_error(env, "SU_E_INVALID_ARGUMENT"))
        env = _spec_envelope(t, sess, nat, sess.revs[nat], "scalar", extra={"expected_semantic_hash": "0" * 64})
        t.check_mismatches(f"TX3 {backend} stale semantic hash", classify_error(env, "SU_E_STALE_REVISION"))
        env = _spec_envelope(t, sess, nat, sess.revs[nat], "turbo")
        t.check_mismatches(f"TX3 {backend} unknown backend", classify_error(env, "SU_E_INVALID_ARGUMENT"))
        close_world(t, sess, ctrl)
        close_world(t, sess, nat)
    # Code-only install of a new semantic revision after specialization: the kernel is dropped at the
    # effective tick (tier back to vm), the old artifact is released, and outputs follow the new semantics.
    sched, expected = _swap_expected(t)
    swap_tick = sched["swap"]["effective_tick"]
    nat, nat_rev = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, nat, swap_tick)
    pre = inspect(t, sess, nat)
    sys_id = pre["semantic_manifest"][0]["system_id"]
    res = call(t, sess, "world.specialize", {"world_id": nat, "expected_world_revision": sess.revs[nat],
                                             "system_id": sys_id, "backend": "scalar"})
    t.check("TX3 swap: specialized before the install", "native_scalar", res.get("execution_tier"))
    t.check("TX3 swap: manifest reports native tier before install", "native_scalar",
            inspect(t, sess, nat)["semantic_manifest"][0].get("execution_tier"))
    cid = prepare(t, sess, EDIT_V2, V2_SRC, EDIT_V2_TEST)
    install(t, sess, nat, cid, pre["world_revision"], swap_tick)
    post = inspect(t, sess, nat)
    t.check_mismatches("TX3 swap: install does not step", verify_state(swap_tick, expected[swap_tick], post))
    t.check("TX3 swap: kernel dropped at the install (tier vm)", "vm",
            post["semantic_manifest"][0].get("execution_tier"))
    for tick in range(swap_tick + 1, sched["total_ticks"] + 1):
        step(t, sess, nat, 1)
        t.check_mismatches(f"TX3 swap: tick {tick} follows the new semantics",
                           verify_state(tick, expected[tick], inspect(t, sess, nat)))
    close_world(t, sess, nat)
    # M1-02c-2: the active worker is killed from outside while a scalar kernel is installed. Nothing is
    # attributed to the kernel (no fault), so it is not quarantined: recovery returns the world to the VM
    # tier with the kernel pending, a world.step is not preceded by respecialization, and the next non-tick
    # answer after that shows the respecialized tier. Ticks equal the VM control world throughout.
    ctrl, _ = create_world(t, sess, SPEC_WORLD, [SPEC_CAP])
    nat, _ = create_world(t, sess, SPEC_WORLD, [SPEC_CAP])
    for _ in range(2):
        step(t, sess, ctrl, 1)
        step(t, sess, nat, 1)
    res = call(t, sess, "world.specialize", {"world_id": nat, "expected_world_revision": sess.revs[nat],
                                             "system_id": SPEC_SYSTEM, "backend": "scalar"})
    t.check("TX3 kill: scalar installed before the kill", "native_scalar", res.get("execution_tier"))
    old_pid = call(t, sess, "engine.info", {})["active_worker"]["pid"]
    os.kill(old_pid, signal.SIGKILL)
    env = sess.request("world.inspect", {"world_id": nat})
    t.check_mismatches("TX3 kill: first request after the death reports the recovery",
                       recovery_mismatches(env, old_pid))
    # A tick right after the recovery answer runs on the VM; the queue is not serviced for a world.step.
    step(t, sess, nat, 1)
    step(t, sess, ctrl, 1)
    # The first non-tick answer after the recovery is sent before the request is serviced.
    env = sess.request("world.inspect", {"world_id": nat})
    spec = env.get("specialization") if isinstance(env.get("specialization"), dict) else {}
    tier = env.get("result", {}).get("semantic_manifest", [{}])[0].get("execution_tier")
    t.check("TX3 kill: tier vm after recovery, before the respecialization", "vm", tier)
    t.check("TX3 kill: respecialization pending after recovery", True, spec.get("pending_respecialization"))
    t.check("TX3 kill: the kernel was not quarantined (nothing attributed to it)", [], spec.get("quarantined"))
    env = sess.request("world.inspect", {"world_id": nat})
    tier = env.get("result", {}).get("semantic_manifest", [{}])[0].get("execution_tier")
    t.check("TX3 kill: respecialized tier after the next non-tick answer", "native_scalar", tier)
    t.check("TX3 kill: respecialization outcome", "respecialized",
            env.get("specialization", {}).get("respecialization"))
    for tick in range(4, 21):
        step(t, sess, ctrl, 1)
        step(t, sess, nat, 1)
        t.check(f"TX3 kill: tick {tick} native == VM control", inspect(t, sess, ctrl).get("columns"),
                inspect(t, sess, nat).get("columns"))
    close_world(t, sess, ctrl)
    close_world(t, sess, nat)
    if "avx2" not in [b for b, _ in backends]:
        ctrl, _ = create_world(t, sess, SPEC_WORLD, [SPEC_CAP])
        env = _spec_envelope(t, sess, ctrl, sess.revs[ctrl], "avx2")
        t.check_mismatches(f"TX3 avx2 refused without host support", classify_error(env, "SU_E_CAPABILITY_DENIED"))
        close_world(t, sess, ctrl)


SPAWN_PROBE = os.path.join(REPO, "tests", "e2e", "native_spawn_probe.py")


def native_spawn_mismatches(rc, evidence) -> list:
    """Pure verdict of one native_spawn_probe run (TX4). rc 0 with a pass verdict is the only pass; rc 3 is
    'not a native build'; every other outcome is a mismatch (a violation, or an inconclusive probe that must
    not be read as a pass)."""
    if rc == 0:
        if not isinstance(evidence, dict) or evidence.get("verdict") != "pass":
            return ["probe exited 0 without a pass verdict"]
        if not evidence.get("own_successful_execs") or not isinstance(evidence.get("steps"), list):
            return ["probe pass lacks the traced session evidence"]
        return []
    if rc == 1:
        return ["a build tool was executed by the native session (violation)"]
    return [f"probe verdict is not a pass (exit {rc}: {(evidence or {}).get('verdict')})"]


def tx4(t: TestCtx):
    """Native specialization spawns no build tool (extra check; not a TEST_SPEC id). The session of TX3 plus an
    IR edit (capsule.submit, candidate.evaluate, candidate.install) runs under strace -f with execve and
    execveat traced. Positive controls must detect a deliberate cc (PATH and absolute path). Unavailable on a
    binary that does not advertise world.specialize."""
    out = os.path.join(REPO, "build", "tx4_native_spawn_probe.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    rc, text = t.run_cmd([sys.executable, SPAWN_PROBE, "--binary", t.binary, "--out", out], timeout=900)
    try:
        with open(out, encoding="utf-8") as fh:
            evidence = json.load(fh)
    except (OSError, ValueError):
        evidence = None
    if rc == 3:
        raise Unavailable(f"binary is not a native build: {(evidence or {}).get('reason', text[-200:])}")
    mism = native_spawn_mismatches(rc, evidence)
    t.check_mismatches("TX4 native session executes no build tool and the probe passes", mism)
    t.details["tx4_steps"] = (evidence or {}).get("steps")
    t.details["tx4_exec_summary"] = (evidence or {}).get("summary", {}).get("distinct_executed_basenames")


# ---- M1 closure B: chunked tick workers (TX5) and large-world containment (TX6) ----
TICK_COUNTS = (1, 2, 4)            # SUTEKH_TICK_WORKERS values TX5 runs the real binary with
TICK_ROWS = 100001                 # TX5 world size (chunked path: rows >= 4096 threshold)
TICK_TICKS = 20                    # TX5 ticks, each compared with the reference evaluator
BIG_ROWS = 1000000                 # TX6 world size, DEFAULT limits
BIG_TICKS = 3
RLIMIT_AS_BOUND = 4 * 1024 * 1024 * 1024   # constitution C07 bound stated in the task (4 GiB)
SPOOL_NEEDED_FOR_1M = 128 * 1024 * 1024    # regression guard: the earlier 32 MiB default blocked 1M rows
SECRET_NAME = "SECRET_X"
TX_DIR = "build"                   # generated fixtures live under the project root (path policy)
U32_MASK = (1 << 32) - 1
MOVEMENT_CAP_DIR = "tests/fixtures/t02"
CONFLICT_DIR = "tests/fixtures/tick_workers/conflict"
CYCLE_DIR = "tests/fixtures/tick_workers/cycle"
ORDER_DIR = "tests/fixtures/tick_workers/order"


def gen_columns(rows: int, seed: int):
    """Deterministic u32 columns with the edge values the reference exercises (no RNG library)."""
    pos = [0] * rows
    vel = [0] * rows
    x = seed & ((1 << 64) - 1)
    for i in range(rows):
        x = (x * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        p = (x >> 32) & U32_MASK
        x = (x * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        v = (x >> 32) & U32_MASK
        if i % 11 == 3:
            p = U32_MASK
        if i % 5 == 0:
            v = p
        if i % 13 == 4:
            v = 0
        pos[i] = p
        vel[i] = v
    return pos, vel


def write_world_fixture(path: str, rows: int, pos, vel) -> None:
    doc = {"fixture_format": "0.1", "schema_id": "demo.actor", "schema_version": 1, "tick": 0,
           "entity_ids": list(range(1, rows + 1)), "columns": {"position_x": pos, "velocity_x": vel}}
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, separators=(",", ":"))


def pid_constant_mismatches(pids: list) -> list:
    """Every observation of the active worker must name the same pid (no silent restart)."""
    distinct = {p for p in pids}
    if len(distinct) != 1 or None in distinct or any(type(p) is not int for p in pids):
        return [f"worker_pid over the ticks: expected one integer pid, observed {sorted(map(repr, distinct))}"]
    return []


def rlimit_as_mismatches(limits_text: str, bound: int = RLIMIT_AS_BOUND) -> list:
    """Parses one `Max address space` line of /proc/<pid>/limits: the soft limit must be a number <= bound."""
    for line in limits_text.splitlines():
        if line.startswith("Max address space"):
            parts = line.split()
            if len(parts) < 5:
                return [f"RLIMIT_AS line is malformed: {line!r}"]
            soft = parts[3]
            if soft == "unlimited":
                return ["RLIMIT_AS soft limit is unlimited (C07 requires a limit)"]
            if not soft.isdigit():
                return [f"RLIMIT_AS soft limit is not a number: {soft!r}"]
            if int(soft) > bound or int(soft) == 0:
                return [f"RLIMIT_AS soft limit {soft} is outside (0, {bound}]"]
            return []
    return ["no Max address space line in /proc limits"]


def limit_soft(limits_text: str, key: str):
    """Soft limit of one /proc/<pid>/limits row named by key (for example "Max file size"): an int,
    the string "unlimited", or None when the row is missing or not a number."""
    for line in limits_text.splitlines():
        if line.startswith(key):
            parts = line[len(key):].split()
            if not parts:
                return None
            if parts[0] == "unlimited":
                return "unlimited"
            return int(parts[0]) if parts[0].isdigit() else None
    return None


def exact_limit_mismatches(limits_text: str, key: str, expected) -> list:
    """The soft limit of one /proc limits row must equal expected exactly (an int or "unlimited")."""
    got = limit_soft(limits_text, key)
    if got != expected:
        return [f"{key} soft limit is {got!r}, expected {expected!r}"]
    return []


def seccomp_mismatches(status: bytes, mode: int = 2) -> list:
    """The process's own /proc/<pid>/status must report Seccomp mode `mode` (2 = filter)."""
    for line in status.decode("utf-8", "replace").splitlines():
        if line.startswith("Seccomp:"):
            val = line.split(":", 1)[1].strip()
            return [] if val == str(mode) else [f"Seccomp mode is {val!r}, expected {mode}"]
    return ["no Seccomp line in /proc status"]


def seccomp_filters_mismatches(status: bytes, want: int) -> list:
    """Seccomp_filters (kernel 5.11 and later) counts the filters on the process. An engine worker has two:
    the network filter the supervisor installs before exec, and the worker self-sandbox it installs itself."""
    for line in status.decode("utf-8", "replace").splitlines():
        if line.startswith("Seccomp_filters:"):
            val = line.split(":", 1)[1].strip()
            return [] if val == str(want) else [f"Seccomp_filters is {val!r}, expected {want}"]
    return ["no Seccomp_filters line in /proc status (kernel older than 5.11)"]


def _has_two_filters(status) -> bool:
    """True when a /proc status sample shows Seccomp_filters 2 (None or other values: False)."""
    if not status:
        return False
    return seccomp_filters_mismatches(status, 2) == []


def containment_mismatches(cont, expected_as: int, expected_fsize: int, expected_nofile: int,
                           expect_fs: bool = False) -> list:
    """The containment object of a disposable worker answer (candidate.evaluate) must report the
    limits the engine asked for, and seccomp as the enforced network denial. With expect_fs, it must
    also report Landlock as the enforced file-write denial."""
    if not isinstance(cont, dict):
        return ["containment object is missing from the answer"]
    out = []
    if cont.get("reported") is not True:
        out.append(f"containment not reported by the child: {cont.get('reason')!r}")
    for key, want in (("rlimit_as", expected_as), ("rlimit_fsize", expected_fsize),
                      ("rlimit_nofile", expected_nofile)):
        if cont.get(key) != want:
            out.append(f"containment {key} is {cont.get(key)!r}, expected {want!r}")
    if cont.get("no_network") != "seccomp":
        out.append(f"containment no_network is {cont.get('no_network')!r}, expected 'seccomp'")
    if expect_fs and cont.get("fs_write_denied") != "landlock":
        out.append(f"containment fs_write_denied is {cont.get('fs_write_denied')!r}, expected 'landlock'")
    return out


def environ_secret_mismatches(environ: bytes, name: str = SECRET_NAME) -> list:
    """A child's environment (NUL-separated /proc/<pid>/environ) must not carry the named variable."""
    prefix = name.encode("ascii") + b"="
    if any(entry.startswith(prefix) for entry in environ.split(b"\0")):
        return [f"{name} is present in the child environment (scrubbed environment failed)"]
    return []


def spool_mismatches(spool_bytes_max, needed: int = SPOOL_NEEDED_FOR_1M) -> list:
    if type(spool_bytes_max) is not int or spool_bytes_max < needed:
        return [f"recovery spool default {spool_bytes_max!r} is below the {needed} bytes a 1,000,000-row world needs"]
    return []


def sample_mismatches(expected: dict, pages: list, offsets_ok=True) -> list:
    """Compares inspect pages (offset, result) with reference columns at the same rows."""
    out = []
    for offset, page in pages:
        n = page.get("returned_rows")
        if type(n) is not int or n <= 0:
            out.append(f"page at offset {offset}: returned_rows is {n!r}")
            continue
        for f in INSPECT_FIELDS:
            vals = (page.get("columns") or {}).get(f)
            if not isinstance(vals, list) or len(vals) != n:
                out.append(f"page at offset {offset}: column {f} has the wrong length")
                continue
            exp = expected[f][offset:offset + n]
            if vals != exp:
                first = next(i for i, (a, b) in enumerate(zip(vals, exp)) if a != b)
                out.append(f"{f}[{offset + first}]: expected {exp[first]} actual {vals[first]}")
            if len(out) >= 20:
                return out
    return out


def _sample_offsets(rows: int, seed: int, count: int) -> list:
    rng = __import__("random").Random(seed)
    pages = max(1, (rows + INSPECT_PAGE - 1) // INSPECT_PAGE)
    picks = {0, pages - 1}
    while len(picks) < count:
        picks.add(rng.randrange(pages))
    return sorted(p * INSPECT_PAGE for p in picks)


def _inspect_sampled(tctx, sess, wid, rows: int, seed: int, count: int) -> list:
    out = []
    for off in _sample_offsets(rows, seed, count):
        res = call(tctx, sess, "world.inspect",
                   {"world_id": wid, "fields": list(INSPECT_FIELDS), "limit": INSPECT_PAGE, "offset": off})
        out.append((off, res))
    return out


def _proc_read(path: str):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError:
        return None


def _write_scratch_dir(tctx, prefix: str) -> str:
    root = os.path.join(REPO, TX_DIR)
    os.makedirs(root, exist_ok=True)
    return tempfile.mkdtemp(prefix=prefix, dir=root)


def tx5(t: TestCtx):
    cap_rel = f"{MOVEMENT_CAP_DIR}/movement.capsule.json"
    cap, lir_doc, _ = t.load_system(cap_rel)
    pos0, vel0 = gen_columns(TICK_ROWS, 5)
    expected = []
    cols = {"position_x": pos0, "velocity_x": vel0}
    for _k in range(TICK_TICKS):
        cols = ref.run_tick(lir_doc, cap, cols)
        expected.append({"position_x": cols["position_x"], "velocity_x": cols["velocity_x"]})
    scratch = _write_scratch_dir(t, "m0-tickw-")
    try:
        world_path = os.path.join(scratch, "world.json")
        write_world_fixture(world_path, TICK_ROWS, pos0, vel0)
        world_rel = t.rel(world_path)
        pids_by_n = {}
        for n in TICK_COUNTS:
            env = dict(t.env)
            env["SUTEKH_TICK_WORKERS"] = str(n)
            sess = t.session(f"TX5 serve workers={n}", env=env)
            tw = call(t, sess, "engine.info", {}).get("tick_workers")
            if not isinstance(tw, dict):
                raise Unavailable("engine.info does not advertise tick_workers")
            t.check(f"TX5 workers={n}: engine.info advertises the configured workers", n, tw.get("workers"))
            t.check(f"TX5 workers={n}: engine.info advertises the chunk threshold", 4096, tw.get("chunk_min_rows"))
            wid, _rev = create_world(t, sess, world_rel, [cap_rel])
            pids = []
            for k, exp in enumerate(expected, start=1):
                step(t, sess, wid, 1)
                state = inspect(t, sess, wid)
                pids.append(state.get("worker_pid"))
                t.check_mismatches(f"TX5 workers={n} tick {k} columns", verify_state(k, exp, state))
            t.check_mismatches(f"TX5 workers={n} worker_pid constant over {TICK_TICKS} ticks",
                               pid_constant_mismatches(pids))
            pids_by_n[n] = pids[0]
            close_world(t, sess, wid)
            sess.close()
        # Effect analysis at world.create (one serve session): conflicting and cyclic schedules are refused.
        sess = t.session("TX5 refusals")
        for label, dirn, order, want in (
                ("conflict", CONFLICT_DIR, ("advance", "damp"), "SU_E_SCHEMA_MISMATCH"),
                ("cycle", CYCLE_DIR, ("advance", "damp"), "SU_E_INVALID_ARGUMENT"),
                ("capsule-order", ORDER_DIR, ("damp", "advance"), "SU_E_INVALID_ARGUMENT")):
            env = sess.request("world.create", {
                "fixture_path": f"{dirn}/pipe_world.json",
                "capsule_paths": [f"{dirn}/{name}.capsule.json" for name in order]})
            code = env.get("error", {}).get("code") if not env.get("ok") else "created"
            t.check(f"TX5 {label}: world.create refuses the schedule with {want}", want, code)
        sess.close()
        t.details["worker_pids"] = {str(k): v for k, v in pids_by_n.items()}
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def tx6(t: TestCtx):
    base_env = {k: v for k, v in t.env.items() if not k.startswith("SUTEKH_")}  # DEFAULT limits, no overrides
    base_env[SECRET_NAME] = "1"
    sess = t.session("TX6 serve", env=base_env)
    info = call(t, sess, "engine.info", {})
    spool = (((info.get("recovery") or {}).get("limits") or {}).get("spool_bytes_max"))
    # (d) the recovery spool default admits a 1,000,000-row world (regression guard)
    t.check_mismatches("TX6 (d) recovery spool default admits a 1,000,000-row world", spool_mismatches(spool))
    scratch = _write_scratch_dir(t, "m0-big-")
    try:
        # (a) 1,000,000-row world: create, step 3, snapshot, step 2, restore, sampled rows vs reference
        pos, vel = gen_columns(BIG_ROWS, 7)
        big_path = os.path.join(scratch, "world.json")
        write_world_fixture(big_path, BIG_ROWS, pos, vel)
        big_rel = t.rel(big_path)
        cap, lir_doc, _ = t.load_system(f"{MOVEMENT_CAP_DIR}/movement.capsule.json")
        res = call(t, sess, "world.create", {"fixture_path": big_rel,
                                             "capsule_path": f"{MOVEMENT_CAP_DIR}/movement.capsule.json"})
        wid = res.get("world_id")
        sess.revs[wid] = res.get("world_revision")
        worker_pid = res.get("worker_pid")
        t.check("TX6 (a) 1,000,000-row world created with default limits", BIG_ROWS, res.get("row_count"))
        step(t, sess, wid, BIG_TICKS)
        ref3 = ref.run_ticks(lir_doc, cap, {"position_x": pos, "velocity_x": vel}, BIG_TICKS)
        expected3 = {"position_x": ref3["position_x"], "velocity_x": ref3["velocity_x"]}
        pages = _inspect_sampled(t, sess, wid, BIG_ROWS, 3, 12)
        t.check("TX6 (a) sampled reads report tick 3", BIG_TICKS, pages[0][1].get("tick"))
        t.check_mismatches("TX6 (a) sampled rows at tick 3 equal the reference",
                           sample_mismatches(expected3, pages))
        snap = call(t, sess, "world.snapshot", {"world_id": wid, "expected_world_revision": sess.revs[wid]})
        sid = snap.get("snapshot_id")
        if not isinstance(sid, str):
            raise Failed("world.snapshot result lacks snapshot_id")
        step(t, sess, wid, 2)
        rest = call(t, sess, "world.restore", {"world_id": wid, "snapshot_id": sid,
                                               "expected_world_revision": sess.revs[wid]})
        sess.revs[wid] = rest.get("world_revision")
        t.check("TX6 (a) restore returns to tick 3", BIG_TICKS, rest.get("tick"))
        t.check_mismatches("TX6 (a) sampled rows after restore equal the tick-3 reference",
                           sample_mismatches(expected3, _inspect_sampled(t, sess, wid, BIG_ROWS, 9, 8)))
        # (b) the active worker runs under an address-space limit
        limits = _proc_read(f"/proc/{worker_pid}/limits")
        if limits is None:
            raise NotRun("active worker /proc limits are not readable on this platform")
        limits_txt = limits.decode("utf-8", "replace")
        t.check_mismatches("TX6 (b) active worker RLIMIT_AS is limited to <= 4 GiB",
                           rlimit_as_mismatches(limits_txt))
        # Exact values: the default is 4 GiB (C07). The active worker has no CPU bound (long runs),
        # RLIMIT_FSIZE 256 MiB (spool and snapshots), RLIMIT_NOFILE 1024.
        t.check_mismatches("TX6 (b) active worker RLIMIT_AS is exactly 4 GiB",
                           exact_limit_mismatches(limits_txt, "Max address space", RLIMIT_AS_BOUND))
        t.check_mismatches("TX6 (b) active worker RLIMIT_FSIZE is exactly 256 MiB",
                           exact_limit_mismatches(limits_txt, "Max file size", 256 * 1024 * 1024))
        t.check_mismatches("TX6 (b) active worker RLIMIT_NOFILE is exactly 1024",
                           exact_limit_mismatches(limits_txt, "Max open files", 1024))
        t.check_mismatches("TX6 (b) active worker has no CPU limit (long-running simulation)",
                           exact_limit_mismatches(limits_txt, "Max cpu time", "unlimited"))
        if platform.machine() == "x86_64":
            active_status = _proc_read(f"/proc/{worker_pid}/status")
            if active_status is None:
                raise NotRun("active worker /proc status is not readable on this platform")
            t.check_mismatches("TX6 (b) active worker runs under a seccomp filter (no sockets)",
                               seccomp_mismatches(active_status))
            # Two filters: the network filter (exec, socket and io_uring denial) and the worker self-sandbox
            # (execve, fork, clone of processes). The self-sandbox is ON in a default build (C07).
            t.check_mismatches("TX6 (b) active worker self-sandbox is on (Seccomp_filters 2)",
                               seccomp_filters_mismatches(active_status, 2))
        # (c) SECRET_X is absent from the active worker's environment
        active_env = _proc_read(f"/proc/{worker_pid}/environ")
        if active_env is None:
            raise NotRun("active worker /proc environ is not readable on this platform")
        t.check_mismatches("TX6 (c) active worker environment is scrubbed",
                           environ_secret_mismatches(active_env))
        close_world(t, sess, wid, systems=1, snapshots=1)
        # (c) disposable worker: poll the children of the supervisor while candidate.evaluate runs
        cap8_dir = _write_scratch_dir(t, "m0-cand-")
        seen = {}
        try:
            rel_dir = t.rel(cap8_dir)
            for name in ("movement.capsule.json", "movement.lir.json"):
                src_name = f"{MOVEMENT_CAP_DIR}/{name}"
                with open(t.abspath(src_name), "rb") as fh_in, open(os.path.join(cap8_dir, name), "wb") as fh_out:
                    fh_out.write(fh_in.read())
            w8 = t.load(f"{MOVEMENT_CAP_DIR}/world_8.json")
            cap8, lir8, _ = t.load_system(f"{rel_dir}/movement.capsule.json")
            cols8 = {"position_x": w8["columns"]["position_x"], "velocity_x": w8["columns"]["velocity_x"]}
            end8 = ref.run_ticks(lir8, cap8, cols8, 100000)
            with open(os.path.join(cap8_dir, "world.json"), "w", encoding="utf-8") as fh:
                json.dump(w8, fh)
            test_doc = {"test_format": "m0-test/0.1", "system_id": "movement", "capsule": "movement.capsule.json",
                        "world": "world.json",
                        "cases": [{"name": "tx6_long", "ticks": 100000,
                                   "expected_columns": {"position_x": end8["position_x"],
                                                        "velocity_x": end8["velocity_x"]}}]}
            with open(os.path.join(cap8_dir, "movement.test.json"), "w", encoding="utf-8") as fh:
                json.dump(test_doc, fh)
            cid = submit(t, sess, f"{rel_dir}/movement.capsule.json", f"{rel_dir}/movement.lir.json")
            stop = threading.Event()

            def poll(sup=sess.proc.pid, active=worker_pid, out=seen, stop=stop):
                while not stop.is_set():
                    for kid in _children_fast(sup):
                        if kid == active:
                            continue
                        if kid in out:
                            # The first sample can precede the worker's own self-sandbox (exec happens first):
                            # re-read the status until it shows both filters, or the child is gone.
                            if not _has_two_filters(out[kid].get("status")):
                                st_b = _proc_read(f"/proc/{kid}/status")
                                if st_b is not None:
                                    out[kid]["status"] = st_b
                            continue
                        # A forked child carries the supervisor's environment until execve replaces
                        # it, so only a child whose argv is the worker role (`sutekh worker`) counts.
                        cmd_b = _proc_read(f"/proc/{kid}/cmdline")
                        if not cmd_b or cmd_b.rstrip(b"\0").split(b"\0")[-1:] != [b"worker"]:
                            continue
                        env_b = _proc_read(f"/proc/{kid}/environ")
                        if env_b is not None:
                            out[kid] = {"environ": env_b, "limits": _proc_read(f"/proc/{kid}/limits"),
                                        "status": _proc_read(f"/proc/{kid}/status")}
                    time.sleep(0.001)

            th = threading.Thread(target=poll, daemon=True)
            th.start()
            try:
                ev = sess.request("candidate.evaluate", {"candidate_id": cid,
                                                         "test_path": f"{rel_dir}/movement.test.json"},
                                  timeout=120.0)
            finally:
                stop.set()
                th.join(5)
            t.check("TX6 (c) evaluation answered", True, bool(ev.get("ok")))
            # The engine's own record of the disposable child's containment must match what the child
            # reads back in /proc (checked per observed child above).
            if ev.get("ok") and platform.machine() == "x86_64":
                cont_doc = (ev.get("result") or {}).get("containment")
                # Landlock is checked when the host enforces it; a host without it is recorded, not passed.
                fs_on = isinstance(cont_doc, dict) and cont_doc.get("fs_write_denied") == "landlock"
                t.details["disposable_fs_write_denial"] = (
                    "landlock" if fs_on else f"not enforced: {(cont_doc or {}).get('fs_reason')!r}")
                t.check_mismatches("TX6 (c) candidate.evaluate reports the disposable containment",
                                   containment_mismatches(cont_doc, RLIMIT_AS_BOUND, 256 * 1024 * 1024, 64,
                                                          expect_fs=fs_on))
        finally:
            shutil.rmtree(cap8_dir, ignore_errors=True)
        if not seen:
            t.details["disposable_worker_environ"] = "not caught: no disposable child was observed during evaluate"
        for kid, rec in seen.items():
            t.check_mismatches(f"TX6 (c) disposable worker {kid} environment is scrubbed",
                               environ_secret_mismatches(rec["environ"]))
            if rec["limits"] is not None:
                dl = rec["limits"].decode("utf-8", "replace")
                t.check_mismatches(f"TX6 (b) disposable worker {kid} RLIMIT_AS is limited",
                                   rlimit_as_mismatches(dl))
                t.check_mismatches(f"TX6 (b) disposable worker {kid} RLIMIT_AS is exactly 4 GiB",
                                   exact_limit_mismatches(dl, "Max address space", RLIMIT_AS_BOUND))
                t.check_mismatches(f"TX6 (b) disposable worker {kid} RLIMIT_FSIZE is exactly 256 MiB",
                                   exact_limit_mismatches(dl, "Max file size", 256 * 1024 * 1024))
                t.check_mismatches(f"TX6 (b) disposable worker {kid} RLIMIT_NOFILE is exactly 64",
                                   exact_limit_mismatches(dl, "Max open files", 64))
                if rec.get("status") is not None and platform.machine() == "x86_64":
                    t.check_mismatches(f"TX6 (b) disposable worker {kid} runs under a seccomp filter",
                                       seccomp_mismatches(rec["status"]))
                    t.check_mismatches(f"TX6 (b) disposable worker {kid} self-sandbox is on (Seccomp_filters 2)",
                                       seccomp_filters_mismatches(rec["status"], 2))
        t.details["disposable_workers_observed"] = len(seen)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


# ---- M2-00c3b entities: TX7 (spawn/despawn/stale handles/events vs model) and TX8 (IR 0.2 tick-worker
# equality at N=1,2,4 vs model). Expected values come only from tests/harness/reference_eval.py
# (EntityWorld), derived from the contract; the engine is never used to produce them.
ENT_DIR = "tests/fixtures/entities"
ENT_CAP_MOVE = f"{ENT_DIR}/despawn_move.capsule.json"
ENT_CAP_GUARD = f"{ENT_DIR}/guard.capsule.json"
ENT_CAP_EMIT = f"{ENT_DIR}/emit_then_despawn.capsule.json"
ENT_ACTORS = f"{ENT_DIR}/actors_3.json"
ENT_EVENT_KEYS = ("event_seq", "tick", "kind", "system_index", "slot", "seq", "a", "b")
ENT_TX8_ROWS = 3000              # spans three storage chunks (1024 slots each)
ENT_TX8_CAPACITY = 4096
ENT_TX8_TICKS = 4
ENT_TX8_WORKERS = (1, 2, 4)


def ent_systems(t: "TestCtx", rels) -> list:
    """(program, capsule) pairs in schedule order, from the capsule files and their sources."""
    out = []
    for rel in rels:
        cap, lir, _src = t.load_system(rel)
        out.append((lir, cap))
    return out


def write_entity_fixture(path: str, ids, pos, vel) -> None:
    doc = {"fixture_format": "0.1", "schema_id": "demo.actor", "schema_version": 1, "tick": 0,
           "entity_ids": list(ids), "columns": {"position_x": list(pos), "velocity_x": list(vel)}}
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, separators=(",", ":"))


def ent_event_mismatches(engine_events, model_events) -> list:
    """Engine events.read records against the model's retained records, field by field."""
    out = []
    if not isinstance(engine_events, list) or len(engine_events) != len(model_events):
        n = len(engine_events) if isinstance(engine_events, list) else repr(engine_events)
        return [f"event count: expected {len(model_events)} actual {n}"]
    for i, (got, want) in enumerate(zip(engine_events, model_events)):
        for key in ENT_EVENT_KEYS:
            if got.get(key) != want.get(key):
                out.append(f"event[{i}].{key}: expected {want.get(key)} actual {got.get(key)}")
                if len(out) >= 20:
                    return out
    return out


def ent_seq_continuity_mismatches(events) -> list:
    """event_seq must count up by one with no gap or repeat (a page or a whole log)."""
    seqs = [e.get("event_seq") for e in events]
    for i in range(1, len(seqs)):
        if seqs[i] != seqs[i - 1] + 1:
            return [f"event_seq gap or repeat at index {i}: {seqs[i - 1]} -> {seqs[i]}"]
    return []


def ent_columns_mismatches(model_cols: dict, rows: int, engine_cols) -> list:
    """Initial rows [0, rows) of every field against the model's columns."""
    if not isinstance(engine_cols, dict):
        return ["columns missing from world.inspect"]
    out = []
    for name, values in model_cols.items():
        got = engine_cols.get(name)
        if got != values[:rows]:
            out.append(f"column {name}: expected {values[:rows][:8]}... actual {(got or [])[:8]}...")
    return out


def ent_entities_mismatches(model_table, entities) -> list:
    """world.inspect entities object against the model's table counts."""
    want = {"capacity": model_table.capacity, "alive": model_table.alive_count(),
            "retired": model_table.retired_count(), "high_water": model_table.high_water}
    if entities != want:
        return [f"entities: expected {want} actual {entities}"]
    return []


def ent_inspect_all(t, sess, wid, fields, rows) -> dict:
    """Every initial row through world.inspect paging; keeps the counts from the last page."""
    cols = {f: [] for f in fields}
    off = 0
    last = None
    while off < rows:
        res = call(t, sess, "world.inspect", {"world_id": wid, "fields": list(fields),
                                              "limit": INSPECT_PAGE, "offset": off})
        n = res.get("returned_rows")
        if type(n) is not int or n <= 0:
            raise Failed(f"world.inspect returned no rows at offset {off}")
        for f in fields:
            cols[f].extend(res["columns"][f])
        off += n
        last = res
    return {"columns": cols, "entities": (last or {}).get("entities"), "tick": (last or {}).get("tick"),
            "world_revision": (last or {}).get("world_revision")}


def ent_events_all(t, sess, wid) -> list:
    """events.read from cursor 0 in pages of 256 until a page is empty."""
    out = []
    cursor = 0
    while True:
        res = call(t, sess, "events.read", {"world_id": wid, "cursor": cursor, "max": 256})
        if not res.get("events"):
            break
        out.extend(res["events"])
        cursor = res["next_cursor"]
    return out


def ent_model_events(model, start: int = 0) -> list:
    """Every retained model record from cursor start, paged like the engine (no records are skipped here)."""
    return model.ring.read(start, max(1, len(model.ring.records)))["events"]


def ent_check_state(t, sess, wid, model, label: str, rows_fields=("position_x", "velocity_x")) -> None:
    """Compares columns of the initial rows, entity counts, and the whole event log with the model."""
    ins = ent_inspect_all(t, sess, wid, rows_fields, model.initial_rows)
    t.check_mismatches(f"{label}: columns of the initial rows match the model",
                       ent_columns_mismatches(model.cols, model.initial_rows, ins["columns"]))
    t.check_mismatches(f"{label}: entity table counts match the model",
                       ent_entities_mismatches(model.table, ins["entities"]))
    evs = ent_events_all(t, sess, wid)
    t.check_mismatches(f"{label}: events.read equals the model log", ent_event_mismatches(evs, ent_model_events(model)))
    t.check_mismatches(f"{label}: events are contiguous", ent_seq_continuity_mismatches(evs))


def _ent_probe(t, sess, method, params, label, code):
    """A request that must be refused with exactly code; the envelope is classified."""
    env = sess.request(method, params)
    t.check_mismatches(label, classify_error(env, code))


def tx7(t: TestCtx):
    """Entities through the real binary (extra check; not a TEST_SPEC id): spawn and despawn between ticks,
    lowest-slot reuse and generation bumps, stale handles (generation 0, out of range, wrong generation, zero),
    IR 0.2 despawn_self and emit_event end to end, the bounded event log (wrap, skipped), a cap overflow that
    leaves the world unchanged, a crash of the active worker with spawn and despawn history, snapshot and
    restore across churn, and world.close. Every answer is compared with reference_eval.EntityWorld."""
    systems_rels = [ENT_CAP_MOVE, ENT_CAP_GUARD]
    systems = ent_systems(t, systems_rels)
    fx = t.load(ENT_ACTORS)
    fx_rel = ENT_ACTORS
    cap0 = t.load(ENT_CAP_MOVE)
    sess = t.session("TX7 serve")
    res = call(t, sess, "world.create", {"fixture_path": fx_rel, "capsule_paths": systems_rels, "max_entities": 8})
    wid, rev = res["world_id"], res["world_revision"]
    model = ref.EntityWorld(cap0["fields"], fx["columns"], fx["entity_ids"], 8, 4096, 4096, 65536)
    model.initial_rows = len(fx["entity_ids"])
    ent_check_state(t, sess, wid, model, "TX7 initial")

    # Spawn between ticks: explicit values with an external id, then defaults (zero) without one.
    res = call(t, sess, "world.spawn", {"world_id": wid, "expected_world_revision": rev,
                                       "values": {"position_x": 4, "velocity_x": 0}, "external_id": 7})
    h = model.spawn({"position_x": 4, "velocity_x": 0}, 7)
    t.check("TX7 spawn: handle equals the model", h, res["entity"])
    t.check("TX7 spawn: slot 3, generation 1", (3, 1), (res["slot"], res["generation"]))
    rev = res["world_revision"]
    res = call(t, sess, "world.spawn", {"world_id": wid, "expected_world_revision": rev, "values": {}})
    h2 = model.spawn({})
    t.check("TX7 spawn with no values: handle equals the model", h2, res["entity"])
    rev = res["world_revision"]

    # Refusals leave the state unchanged. Handles: generation 0, out of range, wrong generation, zero.
    before = ent_inspect_all(t, sess, wid, ("position_x",), 3)["entities"]
    for label, handle in (("generation 0 with slot 1", 1), ("slot out of range", (1 << 32) | 99),
                          ("wrong generation for slot 0", (2 << 32) | 0), ("zero handle", 0)):
        _ent_probe(t, sess, "world.despawn", {"world_id": wid, "expected_world_revision": rev, "entity": handle},
                   f"TX7 stale {label} refused with SU_E_STALE_HANDLE", "SU_E_STALE_HANDLE")
        try:
            model.despawn(handle)
            raise Failed(f"model accepted stale handle {handle}")
        except ref.EntityError as exc:
            t.check(f"TX7 model agrees: {label} is stale", ref.STATUS_STALE, exc.status)
    t.check_mismatches("TX7 refused despawns left the entity table unchanged",
                       ent_entities_mismatches(model.table, ent_inspect_all(t, sess, wid, ("position_x",), 3)["entities"]))
    t.check("TX7 refused despawns left the entity counts unchanged", before, ent_inspect_all(
        t, sess, wid, ("position_x",), 3)["entities"])
    ins = call(t, sess, "world.inspect", {"world_id": wid, "entity": 4294967297})
    t.check("TX7 inspect of a live fixture row returns its handle", 4294967297, ins["entity"]["handle"])
    t.check("TX7 inspect of a live fixture row returns its values",
            {"position_x": model.cols["position_x"][1], "velocity_x": model.cols["velocity_x"][1]},
            ins["entity"]["values"])

    # Step: row 0 (position 0, velocity 1) despawns at tick 1 and its emit follows the despawn record.
    res = call(t, sess, "world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": 3})
    model.tick_systems(systems, 3)
    rev = res["world_revision"]
    ent_check_state(t, sess, wid, model, "TX7 after 3 ticks")

    # Lowest-slot reuse and the generation bump: slot 0 died at tick 1, so the next spawn takes slot 0, gen 2.
    res = call(t, sess, "world.spawn", {"world_id": wid, "expected_world_revision": rev,
                                       "values": {"position_x": 1}})
    h3 = model.spawn({"position_x": 1})
    t.check("TX7 reuse: lowest free slot with the bumped generation", h3, res["entity"])
    t.check("TX7 reuse: slot 0, generation 2", (0, 2), (res["slot"], res["generation"]))
    rev = res["world_revision"]
    _ent_probe(t, sess, "world.despawn", {"world_id": wid, "expected_world_revision": rev, "entity": 4294967296},
               "TX7 old generation of the reused slot refused", "SU_E_STALE_HANDLE")
    try:
        model.despawn(4294967296)
        raise Failed("model accepted the old generation")
    except ref.EntityError as exc:
        t.check("TX7 model agrees: old generation is stale", ref.STATUS_STALE, exc.status)

    # Events: cursor ahead of the log is refused; max above 256 is refused.
    _ent_probe(t, sess, "events.read", {"world_id": wid, "cursor": 10 ** 6, "max": 4},
               "TX7 events.read cursor ahead refused", "SU_E_INVALID_ARGUMENT")
    env = sess.request("events.read", {"world_id": wid, "cursor": 0, "max": 257})
    t.check("TX7 events.read max 257 refused", False, env.get("ok") is True)
    ent_check_state(t, sess, wid, model, "TX7 after reuse")

    # Crash of the active worker with spawn and despawn history: the request that finds the dead worker is
    # not executed; after recovery the world is exactly the acknowledged state and the retry succeeds.
    pid = call(t, sess, "world.inspect", {"world_id": wid, "limit": 1})["worker_pid"]
    os.kill(pid, signal.SIGKILL)
    env = sess.request("world.despawn", {"world_id": wid, "expected_world_revision": rev, "entity": h3})
    t.check_mismatches("TX7 request that found the dead worker reports SU_E_WORKER_FAILED",
                       classify_error(env, "SU_E_WORKER_FAILED"))
    ins = call(t, sess, "world.inspect", {"world_id": wid, "limit": 1})
    rev = ins["world_revision"]
    t.check("TX7 recovery: replacement worker is a different pid", True, ins["worker_pid"] != pid)
    ent_check_state(t, sess, wid, model, "TX7 after crash recovery")
    res = call(t, sess, "world.despawn", {"world_id": wid, "expected_world_revision": rev, "entity": h3})
    model.despawn(h3)
    rev = res["world_revision"]
    ent_check_state(t, sess, wid, model, "TX7 after the retried despawn")

    # Snapshot across churn, more churn, then restore: the event log and the entity table return exactly.
    snap = call(t, sess, "world.snapshot", {"world_id": wid, "expected_world_revision": rev})
    saved = copy.deepcopy(model)
    rev = snap["world_revision"]
    res = call(t, sess, "world.spawn", {"world_id": wid, "expected_world_revision": rev,
                                       "values": {"position_x": 2, "velocity_x": 2}})
    model.spawn({"position_x": 2, "velocity_x": 2})
    rev = res["world_revision"]
    call(t, sess, "world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": 1})
    model.tick_systems(systems, 1)
    res = call(t, sess, "world.restore", {"world_id": wid, "snapshot_id": snap["snapshot_id"],
                                         "expected_world_revision": call(t, sess, "world.inspect", {
                                             "world_id": wid, "limit": 1})["world_revision"]})
    model = saved
    t.check("TX7 restore reports the snapshot tick", model.tick, res["tick"])
    ent_check_state(t, sess, wid, model, "TX7 after restore")
    call(t, sess, "world.close", {"world_id": wid, "expected_world_revision": res["world_revision"]})

    # Bounded log: event_log_max 2 keeps the newest two of the four records three ticks produce (two dropped).
    sess2 = t.session("TX7 serve (small log)")
    res = call(t, sess2, "world.create", {"fixture_path": fx_rel, "capsule_paths": systems_rels, "max_entities": 4,
                                          "event_log_max": 2})
    wid2, rev2 = res["world_id"], res["world_revision"]
    small = ref.EntityWorld(cap0["fields"], fx["columns"], fx["entity_ids"], 4, 4096, 4096, 2)
    small.initial_rows = len(fx["entity_ids"])
    res = call(t, sess2, "world.step", {"world_id": wid2, "expected_world_revision": rev2, "ticks": 3})
    small.tick_systems(systems, 3)
    page = call(t, sess2, "events.read", {"world_id": wid2, "cursor": 0, "max": 256})
    want = small.ring.read(0, 256)
    t.check("TX7 wrap precondition: the run drops events from the bounded log", True, want["dropped"] > 0)
    t.check("TX7 wrap: skipped equals the model", want["skipped"], page["skipped"])
    t.check("TX7 wrap: dropped equals the model", want["dropped"], page["dropped"])
    t.check("TX7 wrap: next_cursor equals the model", want["next_cursor"], page["next_cursor"])
    t.check_mismatches("TX7 wrap: retained events equal the model",
                       ent_event_mismatches(page["events"], want["events"]))
    call(t, sess2, "world.close", {"world_id": wid2, "expected_world_revision": res["world_revision"]})

    # Cap overflow: two rows despawn in one tick and the per-tick struct cap is 1. The tick is refused,
    # and nothing changes: columns, entity table, event log and revision.
    scratch = _write_scratch_dir(t, "m2-ent-cap-")
    cap_path = os.path.join(scratch, "cap.json")
    write_entity_fixture(cap_path, [1, 2, 3], [0, 1, 9], [1, 2, 3])
    cap_rel = t.rel(cap_path)
    sess3 = t.session("TX7 serve (cap)")
    res = call(t, sess3, "world.create", {"fixture_path": cap_rel, "capsule_paths": systems_rels,
                                          "max_entities": 4, "max_struct_per_tick": 1})
    wid3, rev3 = res["world_id"], res["world_revision"]
    capmodel = ref.EntityWorld(cap0["fields"], {"position_x": [0, 1, 9], "velocity_x": [1, 2, 3]},
                               [1, 2, 3], 4, 1, 4096, 65536)
    capmodel.initial_rows = 3
    env = sess3.request("world.step", {"world_id": wid3, "expected_world_revision": rev3, "ticks": 1})
    t.check_mismatches("TX7 cap overflow refused with SU_E_RESOURCE_LIMIT",
                       classify_error(env, "SU_E_RESOURCE_LIMIT"))
    try:
        capmodel.tick_systems(systems, 1)
        raise Failed("model accepted a tick over the struct cap")
    except ref.EntityError as exc:
        t.check("TX7 model agrees: struct cap overflow", ref.STATUS_RESOURCE, exc.status)
    ent_check_state(t, sess3, wid3, capmodel, "TX7 after the refused tick")
    t.check("TX7 refused tick left the revision unchanged", rev3,
            call(t, sess3, "world.inspect", {"world_id": wid3, "limit": 1})["world_revision"])
    call(t, sess3, "world.close", {"world_id": wid3, "expected_world_revision": rev3})


def model_rev(rev: str) -> int:
    """The N of a world revision string '<world>.r<N>'."""
    return int(rev.rsplit(".r", 1)[1])


def tx8(t: TestCtx):
    """IR 0.2 programs at N=1,2,4 tick workers (extra check; not a TEST_SPEC id). A 3000-row world (three
    storage chunks) runs the movement system (despawn_self, emit_event), the guard system ordered after it (its
    despawn of an already-despawned row is refused with an event), for several ticks at each worker count. The
    canonical state (world.snapshot sha256: columns, entity table and event log) is equal for every N and equal
    to the model; a second world runs the emit-then-despawn program the same way."""
    rows = ENT_TX8_ROWS
    pos, vel = gen_columns(rows, 8)
    scratch = _write_scratch_dir(t, "m2-ent-tx8-")
    world_path = os.path.join(scratch, "world.json")
    write_entity_fixture(world_path, list(range(1, rows + 1)), pos, vel)
    world_rel = t.rel(world_path)
    systems = ent_systems(t, [ENT_CAP_MOVE, ENT_CAP_GUARD])
    emit_systems = ent_systems(t, [ENT_CAP_EMIT])
    cap0 = t.load(ENT_CAP_MOVE)
    shas = {}
    for label, rels, sysm, cap_fields in (
            ("movement+guard", [ENT_CAP_MOVE, ENT_CAP_GUARD], systems, cap0["fields"]),
            ("emit_then_despawn", [ENT_CAP_EMIT], emit_systems, cap0["fields"])):
        model = ref.EntityWorld(cap_fields, {"position_x": pos, "velocity_x": vel}, list(range(1, rows + 1)),
                                ENT_TX8_CAPACITY, 65536, 65536, 100000)
        model.initial_rows = rows
        model.tick_systems(sysm, ENT_TX8_TICKS)
        for n in ENT_TX8_WORKERS:
            env = dict(t.env)
            env["SUTEKH_TICK_WORKERS"] = str(n)
            env["SUTEKH_TICK_CHUNK_MIN_ROWS"] = "1"
            sess = t.session(f"TX8 {label} workers={n}", env=env)
            res = call(t, sess, "world.create", {"fixture_path": world_rel, "capsule_paths": rels,
                                                "max_entities": ENT_TX8_CAPACITY, "max_struct_per_tick": 65536,
                                                "max_events_per_tick": 65536, "event_log_max": 100000})
            wid, rev = res["world_id"], res["world_revision"]
            tw = call(t, sess, "engine.info", {}).get("tick_workers")
            t.check(f"TX8 {label} workers={n}: engine.info advertises the configured workers", n,
                    (tw or {}).get("workers"))
            res = call(t, sess, "world.step", {"world_id": wid, "expected_world_revision": rev,
                                              "ticks": ENT_TX8_TICKS})
            t.check(f"TX8 {label} workers={n}: all ticks completed", ENT_TX8_TICKS, res.get("ticks_completed"))
            ent_check_state(t, sess, wid, _with_rows(model, rows), f"TX8 {label} workers={n}")
            snap = call(t, sess, "world.snapshot", {"world_id": wid, "expected_world_revision": res["world_revision"]})
            shas.setdefault(label, {})[n] = snap["sha256"]
            call(t, sess, "world.close", {"world_id": wid, "expected_world_revision": res["world_revision"]})
        t.check(f"TX8 {label}: canonical state is equal for N=1,2,4", 1,
                len(set(shas[label].values())))


def _with_rows(model, rows):
    model.initial_rows = rows
    return model


TESTS = [
    ("T00", "Clean bootstrap", (), t00),
    ("T01", "Reference movement", ("world.create", "world.step", "world.inspect"), t01),
    ("T02", "Empty and awkward counts", ("world.create", "world.step", "world.inspect", "world.close"), t02),
    ("T03", "Numeric edges", ("world.create", "world.step", "world.inspect", "world.close"), t03),
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
    ("TX1", "World close beyond the 4-world cap (extra)", ("world.create", "world.step", "world.inspect",
                                                          "world.close"), tx1),
    ("TX2", "Worker crash recovery (extra)", _EVAL + ("engine.info", "world.close"), tx2),
    ("TX3", "Native specialization (extra)", _EVAL + ("world.close", "world.specialize"), tx3),
    ("TX4", "Native specialization spawns no build tool (extra)", _EVAL + ("world.close", "world.specialize"), tx4),
    ("TX5", "Chunked tick workers (extra)", ("engine.info", "world.create", "world.step", "world.inspect",
                                            "world.close", "capsule.submit"), tx5),
    ("TX6", "Large world within default limits and containment (extra)", ("engine.info", "world.create",
                                                                          "world.step", "world.inspect",
                                                                          "world.snapshot", "world.restore",
                                                                          "world.close", "capsule.submit",
                                                                          "candidate.evaluate"), tx6),
    ("TX7", "Entities: spawn/despawn/stale handles/events (real binary vs Python model, extra)",
     ("engine.info", "world.create", "world.step", "world.inspect", "world.spawn", "world.despawn", "events.read",
      "world.snapshot", "world.restore", "world.close"), tx7),
    ("TX8", "IR 0.2 programs and tick-worker equality (N=1,2,4, extra)",
     ("engine.info", "world.create", "world.step", "world.inspect", "events.read", "world.snapshot",
      "world.close"), tx8),
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
                        help="T11 iterations of the six-kind reload cycle (default 1000, the TEST_SPEC count; "
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


def self_test_entities(ok) -> None:
    """Self-test cases for the M2-00c3b entity helpers and the reference model (TX7 and TX8)."""
    for msg in ref.self_check_entity_model():
        ok(f"entity model: {msg}", False)
    ok("entity model: every hand-derived entity answer holds", not ref.self_check_entity_model())

    evs = [{"event_seq": 0, "tick": 1, "kind": 4294967293, "system_index": 0, "slot": 0, "seq": 0, "a": 0, "b": 1},
           {"event_seq": 1, "tick": 1, "kind": 7, "system_index": 0, "slot": 0, "seq": 1, "a": 0, "b": 1}]
    same = [dict(e) for e in evs]
    ok("entity events: identical logs give no mismatch", ent_event_mismatches(same, evs) == [])
    changed = [dict(e) for e in evs]
    changed[1]["kind"] = 8
    ok("entity events: a changed kind is a mismatch", ent_event_mismatches(changed, evs) != [])
    ok("entity events: a missing record is a mismatch", ent_event_mismatches(same[:1], evs) != [])
    ok("entity events: an extra record is a mismatch", ent_event_mismatches(same + same, evs) != [])
    ok("entity events: a non-list is a mismatch", ent_event_mismatches(None, evs) != [])
    ok("entity seq: 0,1,2 is contiguous", ent_seq_continuity_mismatches([{"event_seq": s} for s in (0, 1, 2)]) == [])
    ok("entity seq: a gap is a mismatch", ent_seq_continuity_mismatches([{"event_seq": s} for s in (0, 2)]) != [])
    ok("entity seq: a repeat is a mismatch", ent_seq_continuity_mismatches([{"event_seq": s} for s in (4, 4)]) != [])
    cols = {"position_x": [0, 7, 12, 0], "velocity_x": [1, 2, 3, 0]}
    model = ref.EntityWorld([{"name": "position_x", "read": True, "write": True},
                             {"name": "velocity_x", "read": True, "write": False}],
                            {"position_x": [0, 5, 9], "velocity_x": [1, 2, 3]}, [1, 2, 3], 4, 4096, 4096, 8)
    model.cols = cols
    ok("entity columns: equal prefix gives no mismatch",
       ent_columns_mismatches(cols, 3, {"position_x": [0, 7, 12], "velocity_x": [1, 2, 3]}) == [])
    ok("entity columns: a wrong value is a mismatch",
       ent_columns_mismatches(cols, 3, {"position_x": [0, 7, 13], "velocity_x": [1, 2, 3]}) != [])
    ok("entity columns: a missing field is a mismatch",
       ent_columns_mismatches(cols, 3, {"position_x": [0, 7, 12]}) != [])
    ok("entity columns: a missing columns object is a mismatch", ent_columns_mismatches(cols, 3, None) != [])
    want_entities = {"capacity": 4, "alive": 3, "retired": 0, "high_water": 3}
    ok("entity counts: equal counts give no mismatch", ent_entities_mismatches(model.table, want_entities) == [])
    ok("entity counts: a wrong alive count is a mismatch",
       ent_entities_mismatches(model.table, {**want_entities, "alive": 2}) != [])
    ok("entity fixture writer: round trip through json",
       _entity_fixture_round_trip())
    ok("entity TX7 is registered and needs the entity methods",
       any(tid == "TX7" and "world.spawn" in req and "events.read" in req and "world.despawn" in req
           for tid, _n, req, _f in TESTS))
    ok("entity TX8 is registered and needs events.read", any(tid == "TX8" and "events.read" in req
                                                          for tid, _n, req, _f in TESTS))
    ok("entity methods are documented in the method table",
       all(m in ENGINE_METHODS for m in ("world.spawn", "world.despawn", "events.read")))
    ok("entity stale refusal is classified with SU_E_STALE_HANDLE",
       classify_error({"ok": False, "error": {"code": "SU_E_STALE_HANDLE"}}, "SU_E_STALE_HANDLE") == [])
    ok("entity stale refusal: a different code is a mismatch",
       classify_error({"ok": False, "error": {"code": "SU_E_TYPE"}}, "SU_E_STALE_HANDLE") != [])


def _entity_fixture_round_trip() -> bool:
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    try:
        write_entity_fixture(path, [1, 2], [3, 4], [5, 6])
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        return (doc["entity_ids"] == [1, 2] and doc["columns"]["position_x"] == [3, 4]
                and doc["columns"]["velocity_x"] == [5, 6] and doc["schema_id"] == "demo.actor")
    finally:
        os.unlink(path)


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

    # TX6 containment helpers (M1 closure C).
    lim_txt = ("Limit                     Soft Limit           Hard Limit           Units     \n"
               "Max address space         4294967296           4294967296           bytes     \n"
               "Max file size             268435456            268435456            bytes     \n"
               "Max open files            64                   64                   files     \n"
               "Max cpu time              unlimited            unlimited            seconds   \n")
    ok("limit_soft reads an integer soft limit", limit_soft(lim_txt, "Max address space") == 4294967296)
    ok("limit_soft reads unlimited", limit_soft(lim_txt, "Max cpu time") == "unlimited")
    ok("limit_soft missing row is None", limit_soft(lim_txt, "Max core file size") is None)
    ok("exact limit: equal passes", exact_limit_mismatches(lim_txt, "Max open files", 64) == [])
    ok("exact limit: one more fails", exact_limit_mismatches(lim_txt, "Max open files", 1024) != [])
    ok("exact limit: 1 GiB is not 4 GiB", exact_limit_mismatches(
        lim_txt.replace("4294967296", "1073741824"), "Max address space", RLIMIT_AS_BOUND) != [])
    ok("exact limit: unlimited expected", exact_limit_mismatches(lim_txt, "Max cpu time", "unlimited") == [])
    ok("seccomp mode 2 passes", seccomp_mismatches(b"Name:\tx\nSeccomp:\t2\n") == [])
    ok("seccomp mode 0 fails", seccomp_mismatches(b"Name:\tx\nSeccomp:\t0\n") != [])
    ok("two seccomp filters pass", seccomp_filters_mismatches(b"Seccomp:\t2\nSeccomp_filters:\t2\n", 2) == [])
    ok("one seccomp filter fails (self-sandbox missing)", seccomp_filters_mismatches(b"Seccomp_filters:\t1\n", 2) != [])
    ok("missing Seccomp_filters line fails", seccomp_filters_mismatches(b"Seccomp:\t2\n", 2) != [])
    ok("early one-filter sample is re-read, not accepted", not _has_two_filters(b"Seccomp_filters:\t1\n"))
    ok("two-filter sample accepted", _has_two_filters(b"Seccomp_filters:\t2\n"))
    ok("seccomp line absent fails", seccomp_mismatches(b"Name:\tx\n") != [])
    cont_ok = {"reported": True, "rlimit_as": RLIMIT_AS_BOUND, "rlimit_fsize": 268435456,
               "rlimit_nofile": 64, "no_network": "seccomp", "reason": "seccomp"}
    ok("containment: matching object passes",
       containment_mismatches(cont_ok, RLIMIT_AS_BOUND, 268435456, 64) == [])
    ok("containment: wrong nofile fails",
       containment_mismatches(cont_ok, RLIMIT_AS_BOUND, 268435456, 1024) != [])
    ok("containment: network none fails",
       containment_mismatches({**cont_ok, "no_network": "none"}, RLIMIT_AS_BOUND, 268435456, 64) != [])
    ok("landlock expected and reported passes",
       containment_mismatches({**cont_ok, "fs_write_denied": "landlock"}, RLIMIT_AS_BOUND, 268435456, 64,
                              expect_fs=True) == [])
    ok("landlock expected and not reported fails",
       containment_mismatches({**cont_ok, "fs_write_denied": "none"}, RLIMIT_AS_BOUND, 268435456, 64,
                              expect_fs=True) != [])
    ok("containment: unreported fails",
       containment_mismatches({**cont_ok, "reported": False}, RLIMIT_AS_BOUND, 268435456, 64) != [])
    ok("containment: missing object fails", containment_mismatches(None, RLIMIT_AS_BOUND, 268435456, 64) != [])

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

    # ---- M0-12 / M0-14: timing-target comparison (docs/PERFORMANCE.md section 2)
    tg = {x["id"]: x for x in TIMING_TARGETS}

    def gates_for(target, flag=True, reason="ok", only=None):
        names = target["gates"]
        out = {n: (flag, reason) for n in names}
        if only is not None:
            out[only] = (False, "fact observed as false")
        return out

    def raises_value(fn):
        try:
            fn()
            return False
        except ValueError:
            return True

    st_ok = {"n": 30, "p95": 9.0}
    pv, ir, ins, nat = tg["preview_response"], tg["ir_prepare"], tg["install_critical_section"], tg["native_incremental_check"]
    ok("timing: p95 within target with every gate observed true is met",
       compare_target(pv, st_ok, gates_for(pv))["verdict"] == "met")
    ok("timing: a slow synthetic sample (p95 over target) with every gate observed true is not_met",
       compare_target(ir, {"n": 30, "p95": 35.0}, gates_for(ir))["verdict"] == "not_met")
    ok("timing: the boundary p95 == target is met",
       compare_target(pv, {"n": 30, "p95": 100.0}, gates_for(pv))["verdict"] == "met")
    v_false = compare_target(pv, {"n": 30, "p95": 150.0}, gates_for(pv, only="reusable worker"))
    ok("timing: a gate observed false makes an over-target number not_comparable, with the gate named",
       v_false["verdict"] == "not_comparable" and "fact observed as false" in v_false["reason"]
       and v_false["gates"]["reusable worker"]["met"] is False)
    ok("timing: a gate observed false also blocks a within-target number from being called met",
       compare_target(pv, st_ok, gates_for(pv, only="reusable worker"))["verdict"] == "not_comparable")
    v_none = compare_target(ir, {"n": 30, "p95": 3.0}, {**gates_for(ir), "warm process": (None, "not observed")})
    ok("timing: a missing gate fact (None) is not_comparable even when the number is within target",
       v_none["verdict"] == "not_comparable" and v_none["gates"]["warm process"]["met"] is None
       and "not observed" in v_none["reason"])
    ok("timing: a malformed gate value raises",
       raises_value(lambda: compare_target(ir, st_ok, {**gates_for(ir), "warm process": ("yes", "x")})))
    ok("timing: install over 1 ms with every condition observed is not_met (engine-measured section)",
       compare_target(ins, {"n": 30, "p95": 1.3}, gates_for(ins))["verdict"] == "not_met")
    ok("timing: install at 0.8 ms with every condition observed is met",
       compare_target(ins, {"n": 30, "p95": 0.8}, gates_for(ins))["verdict"] == "met")
    v_nat = compare_target(nat, {"n": 5, "p95": 2500.0}, gates_for(nat, only="reference machine"))
    ok("timing: native p95 over the aim with the reference machine not observed is not_comparable, p95 kept",
       v_nat["verdict"] == "not_comparable" and v_nat["p95_ms"] == 2500.0)
    ok("timing: native p95 over the aim with every condition observed is not_met",
       compare_target(nat, {"n": 5, "p95": 2500.0}, gates_for(nat))["verdict"] == "not_met")
    ok("timing: native p95 within the aim with every condition observed is met",
       compare_target(nat, {"n": 5, "p95": 1500.0}, gates_for(nat))["verdict"] == "met")
    ok("timing: an unmeasured target with an unmet condition is not_comparable (no raise)",
       compare_target(nat, None, gates_for(nat, flag=False))["verdict"] == "not_comparable")
    ok("timing: a missing measurement with every condition observed raises instead of being skipped",
       raises_value(lambda: compare_target(pv, None, gates_for(pv))))
    ok("timing: an empty sample set with every condition observed raises",
       raises_value(lambda: compare_target(pv, {"n": 0, "p95": 1.0}, gates_for(pv))))
    ok("timing: a NaN p95 with every condition observed raises",
       raises_value(lambda: compare_target(pv, {"n": 3, "p95": float("nan")}, gates_for(pv))))
    ok("timing: a boolean p95 with every condition observed raises",
       raises_value(lambda: compare_target(pv, {"n": 3, "p95": True}, gates_for(pv))))
    ok("timing: a gate set that differs from the declared set raises",
       raises_value(lambda: compare_target(pv, st_ok, {"reusable worker": (True, "x")})))
    ok("timing: every verdict carries provisional=True and a reason",
       all(v["provisional"] is True and v["reason"] for v in (
           compare_target(pv, st_ok, gates_for(pv)), v_false, v_none, v_nat)))
    ok("timing: identities_stable accepts one process throughout",
       identities_stable([(1, 2, 3), (1, 2, 3)]))
    ok("timing: identities_stable rejects a changed identity and a missing one",
       not identities_stable([(1, 2, 3), (4, 2, 3)]) and not identities_stable([None]) and not identities_stable([]))
    ok("timing: pids_reused needs two equal integer pids",
       pids_reused([7, 7, 7]) and not pids_reused([7, 8]) and not pids_reused([7]) and not pids_reused([7, None]))
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    try:
        cid_ident = os_identity(child.pid)
        ok("worker record: a live child of this process is accepted",
           worker_record_mismatches(child.pid, cid_ident, os.getpid()) == [])
        ok("worker record: a pid+1 lie is refused (not the active worker, or not live)",
           worker_record_mismatches(child.pid + 1, cid_ident, os.getpid()) != [])
        ok("worker record: a wrong supervisor pid is refused (ppid check)",
           worker_record_mismatches(child.pid, cid_ident, os.getpid() + 1) != [])
        ok("worker record: a non-integer pid is refused",
           worker_record_mismatches("12", cid_ident, os.getpid()) != [])
    finally:
        child.kill()
        child.wait()

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

    # ---- T11 cycle: six kinds, two successful, four refused with the contract codes
    ok("T11 cycle: six kinds, install and revert are the successful ones",
       len(T11_CYCLE) == 6 and T11_SUCCESS == ("install", "revert"))
    ok("T11 refused kinds map to the install-time contract codes",
       T11_EXPECT == {"fail_stale_revision": "SU_E_STALE_REVISION", "fail_past_tick": "SU_E_STALE_REVISION",
                      "fail_layout_generation": "SU_E_SCHEMA_MISMATCH", "fail_evidence": "SU_E_CANDIDATE_FAILED"})

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

    # ---- world.close: result check, method table, and the tests that depend on it
    good_close = {"world_id": "world-1", "closed": True, "released": {"snapshots": 1, "systems": 1}}
    ok("close: a correct reply has no mismatch", close_mismatches(good_close, "world-1", 1, 1) == [])
    ok("close: closed=false is a mismatch", close_mismatches({**good_close, "closed": False}, "world-1", 1, 1) != [])
    ok("close: a different world id is a mismatch", close_mismatches(good_close, "world-2", 1, 1) != [])
    ok("close: a wrong systems count is a mismatch",
       close_mismatches({**good_close, "released": {"snapshots": 1, "systems": 2}}, "world-1", 1, 1) != [])
    ok("close: a wrong snapshots count is a mismatch",
       close_mismatches({**good_close, "released": {"snapshots": 0, "systems": 1}}, "world-1", 1, 1) != [])
    ok("close: a bool count is not an integer",
       close_mismatches({**good_close, "released": {"snapshots": True, "systems": 1}}, "world-1", 1, 1) != [])
    ok("close: a missing released object is a mismatch",
       close_mismatches({"world_id": "world-1", "closed": True}, "world-1", 1, 1) != [])
    good_rec = {"protocol": "0.1", "id": "x", "ok": False,
                "error": {"code": "SU_E_WORKER_FAILED", "status": 15, "message": "m", "retry": True},
                "recovery": {"recovered": True, "replaced_pid_old": 100, "replaced_pid_new": 101,
                             "worlds_recovered": 1, "ops_replayed": 0, "duration_ms": 2,
                             "from_checkpoint_tick": 3, "worker_generation": 2, "unrecoverable": [],
                             "quarantined": []}}
    ok("tx4: exit 0 with a pass verdict and session evidence passes",
       native_spawn_mismatches(0, {"verdict": "pass", "own_successful_execs": 7, "steps": ["x"]}) == [])
    ok("tx4: exit 0 without a pass verdict fails",
       native_spawn_mismatches(0, {"verdict": "inconclusive", "own_successful_execs": 7, "steps": ["x"]}) != [])
    ok("tx4: exit 0 with a pass but no traced execs fails",
       native_spawn_mismatches(0, {"verdict": "pass", "own_successful_execs": 0, "steps": ["x"]}) != [])
    ok("tx4: exit 1 (build tool executed) fails", native_spawn_mismatches(1, {"verdict": "violation"}) != [])
    ok("tx4: exit 2 (inconclusive) is never a pass", native_spawn_mismatches(2, {"verdict": "inconclusive"}) != [])
    ok("tx4: missing evidence file on exit 0 fails", native_spawn_mismatches(0, None) != [])
    ok("recovery: a successful recovery object passes", recovery_mismatches(good_rec, 100) == [])
    ok("recovery: the replacement pid equal to the old one is a mismatch",
       recovery_mismatches({**good_rec, "recovery": {**good_rec["recovery"], "replaced_pid_new": 100}}, 100) != [])
    ok("recovery: recovered=false is a mismatch",
       recovery_mismatches({**good_rec, "recovery": {**good_rec["recovery"], "recovered": False}}, 100) != [])
    ok("recovery: a non-empty unrecoverable list is a mismatch",
       recovery_mismatches({**good_rec, "recovery": {**good_rec["recovery"], "unrecoverable": [{}]}}, 100) != [])
    ok("recovery: a missing recovery object is a mismatch",
       recovery_mismatches({k: v for k, v in good_rec.items() if k != "recovery"}, 100) != [])
    ok("recovery: a request that succeeded is a mismatch", recovery_mismatches({"ok": True}, 100) != [])
    ok("recovery: a wrong error code is a mismatch",
       recovery_mismatches({**good_rec, "error": {"code": "SU_E_INTERNAL", "status": 19}}, 100) != [])
    ok("recovery: a null from_checkpoint_tick is accepted",
       recovery_mismatches({**good_rec, "recovery": {**good_rec["recovery"], "from_checkpoint_tick": None}}, 100) == [])
    ok("recovery: TX2 is registered and requires engine.info",
       any(tid == "TX2" and "engine.info" in req for tid, _n, req, _f in TESTS))
    ok("recovery: engine.info documents worker_generation and recoveries",
       "worker_generation" in ENGINE_METHODS["engine.info"]["result"]
       and "recoveries" in ENGINE_METHODS["engine.info"]["result"])
    ok("close: world.close is in the method table",
       "world.close" in ENGINE_METHODS and "expected_world_revision" in ENGINE_METHODS["world.close"]["params"])
    ok("close: TX1 requires world.close",
       any(tid == "TX1" and "world.close" in req for tid, _n, req, _f in TESTS))
    ok("close: T02 and T03 require world.close (they close each world)",
       all("world.close" in req for tid, _n, req, _f in TESTS if tid in ("T02", "T03")))

    # ---- M1 closure B: TX5 / TX6 comparison logic (pure; no binary)
    ok("tx5: a single worker pid over every tick is a pass",
       pid_constant_mismatches([42, 42, 42]) == [])
    ok("tx5: a pid that changes between ticks is a mismatch",
       pid_constant_mismatches([42, 43]) != [])
    ok("tx5: a missing or boolean pid is a mismatch",
       pid_constant_mismatches([None, None]) != [] and pid_constant_mismatches([True]) != [])
    ok("tx5: the test is registered with engine.info and the world methods",
       any(tid == "TX5" and "engine.info" in req for tid, _n, req, _f in TESTS))
    ok("tx5: the worker counts are 1, 2 and 4, over at least 20 ticks of 100,001 rows",
       TICK_COUNTS == (1, 2, 4) and TICK_TICKS >= 20 and TICK_ROWS == 100001)
    gen_a = gen_columns(12, 1)
    ok("tx5: generated columns are deterministic and u32",
       gen_a == gen_columns(12, 1) and all(0 <= v <= U32_MASK for v in gen_a[0] + gen_a[1]))
    ok("tx5: generated columns include the u32 maximum and equal-column rows",
       gen_a[0][3] == U32_MASK and gen_a[1][0] == gen_a[0][0])
    ok("tx6: RLIMIT_AS of 4 GiB is accepted",
       rlimit_as_mismatches("Limit                     Soft Limit           Hard Limit           Units\n"
                            "Max address space         4294967296           4294967296           bytes\n") == [])
    ok("tx6: an unlimited RLIMIT_AS is a mismatch",
       rlimit_as_mismatches("Max address space         unlimited            unlimited            bytes\n") != [])
    ok("tx6: an RLIMIT_AS above 4 GiB is a mismatch",
       rlimit_as_mismatches("Max address space         8589934592           unlimited            bytes\n") != [])
    ok("tx6: a limits text without the address-space line is a mismatch",
       rlimit_as_mismatches("Max cpu time              unlimited            unlimited            seconds\n") != [])
    ok("tx6: a clean environment has no SECRET_X",
       environ_secret_mismatches(b"PATH=/usr/bin\0SUTEKH_TICK_WORKERS=2\0") == [])
    ok("tx6: SECRET_X in an environment is a mismatch",
       environ_secret_mismatches(b"PATH=/usr/bin\0SECRET_X=1\0") != [])
    ok("tx6: a variable that only ends in SECRET_X is not SECRET_X",
       environ_secret_mismatches(b"MY_SECRET_X=1\0") == [])
    ok("tx6: a 512 MiB spool admits a 1,000,000-row world",
       spool_mismatches(512 * 1024 * 1024) == [])
    ok("tx6: the earlier 32 MiB spool default is a mismatch (regression guard)",
       spool_mismatches(32 * 1024 * 1024) != [])
    ok("tx6: a missing spool limit is a mismatch", spool_mismatches(None) != [])
    exp_s = {"position_x": [1, 2, 3, 4], "velocity_x": [9, 8, 7, 6]}
    ok("tx6: sampled pages equal to the reference give no mismatch",
       sample_mismatches(exp_s, [(2, {"returned_rows": 2, "columns": {"position_x": [3, 4],
                                                                       "velocity_x": [7, 6]}})]) == [])
    ok("tx6: a sampled value that differs from the reference is a mismatch",
       sample_mismatches(exp_s, [(2, {"returned_rows": 2, "columns": {"position_x": [3, 5],
                                                                       "velocity_x": [7, 6]}})]) != [])
    ok("tx6: a sampled page with the wrong length is a mismatch",
       sample_mismatches(exp_s, [(0, {"returned_rows": 2, "columns": {"position_x": [1],
                                                                       "velocity_x": [9, 8]}})]) != [])
    offs = _sample_offsets(1000000, 3, 12)
    ok("tx6: sample offsets are page-aligned, sorted, in range, and deterministic",
       offs == _sample_offsets(1000000, 3, 12) and offs == sorted(offs)
       and all(o % INSPECT_PAGE == 0 and 0 <= o < 1000000 for o in offs) and 0 in offs)
    ok("tx6: the test is registered and needs candidate.evaluate",
       any(tid == "TX6" and "candidate.evaluate" in req for tid, _n, req, _f in TESTS))
    self_test_entities(ok)
    failed = [name for name, passed in checks if not passed]
    for name in failed:
        print(f"self-test FAILED: {name}")
    print(f"self-test: {len(checks)} checks, {len(failed)} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
