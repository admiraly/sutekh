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
import json
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


def make_oversize_line(total_bytes: int) -> bytes:
    head = b'{"protocol":"0.1","id":"oversize-1","method":"engine.info","params":{"pad":"'
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

MAX_RECORDED_CHECKS = 300


class RunCtx:
    def __init__(self, binary, no_build, t11_cycles, t11_bound_kib):
        self.binary = binary
        self.no_build = no_build
        self.t11_cycles = t11_cycles
        self.t11_bound_kib = t11_bound_kib
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
        self.checks_dropped = 0
        self.inputs: dict = {}
        self.details: dict = {}
        self.sessions: list = []

    def _record(self, entry):
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
        "params": "{capsule_path}  (any other key -> SU_E_TYPE; creation-only, no expected revision)",
        "result": "candidate_id, status, system_id, source_hash, capsule_hash, semantic_hash, artifact_format",
        "source": "src/agent/su_agent.c h_capsule_submit",
    },
    "candidate.status": {
        "params": "{candidate_id}",
        "result": "candidate_id, status, system_id, source_hash, capsule_hash, semantic_hash, "
                  "evaluation{status, cases_total, cases_passed}  (no worker_pid)",
        "source": "src/agent/su_agent.c h_candidate_status",
    },
    "candidate.evaluate": {
        "params": "{candidate_id, test_path, timeout_ms?}  (synchronous; no job id)",
        "result": "candidate evaluation: status, cases_total, cases_passed, failures[]",
        "source": "src/agent/su_agent.c h_candidate_evaluate",
    },
    "world.create": {
        "params": "{fixture_path, capsule_path}  (one capsule; capsule_paths -> SU_E_TYPE)",
        "result": "world_id, world_revision, tick, row_count, schema_id, system_id, source_hash, "
                  "capsule_hash, semantic_hash",
        "source": "src/agent/su_agent.c h_world_create, src/runtime/worker.c worker_create",
    },
    "world.inspect": {
        "params": "{world_id, fields?: [names] (<=64), limit?: 1..256, default 16}",
        "result": "world_id, world_revision, tick, row_count, returned_rows, entity_ids[], columns{name: [u32]}",
        "source": "src/agent/su_agent.c h_world_inspect, src/runtime/worker.c worker_inspect",
    },
    "world.step": {
        "params": "{world_id, expected_world_revision (required), ticks (0..100000)}",
        "result": "world_id, world_revision, ticks_completed, tick",
        "source": "src/agent/su_agent.c h_world_step, src/runtime/worker.c worker_step",
    },
}
# Not implemented by the M0 engine: routed to SU_E_UNSUPPORTED_METHOD, not advertised.
ENGINE_MISSING = ("candidate.install", "world.snapshot", "world.restore")

INSPECT_LIMIT = 256  # engine maximum; there is no paging, so larger worlds cannot be compared in full


def call(tctx, sess, method, params, timeout=60.0) -> dict:
    return ok_result(tctx, method, sess.request(method, params, timeout))


def create_world(tctx, sess, world_rel, cap_rels):
    if len(cap_rels) != 1:
        raise Unavailable(f"multi-system worlds are not implemented: world.create takes one capsule_path, "
                          f"harness asked for {len(cap_rels)}")
    res = call(tctx, sess, "world.create", {"fixture_path": world_rel, "capsule_path": cap_rels[0]})
    wid, rev = res.get("world_id"), res.get("world_revision")
    if not isinstance(wid, str) or not isinstance(rev, str):
        raise Failed("world.create result lacks world_id/world_revision strings")
    sess.revs[wid] = rev
    return wid, rev


def inspect(tctx, sess, wid) -> dict:
    res = call(tctx, sess, "world.inspect",
               {"world_id": wid, "fields": ["position_x", "velocity_x"], "limit": INSPECT_LIMIT})
    if not isinstance(res.get("world_revision"), str):
        raise Failed("world.inspect result lacks world_revision")
    if res.get("returned_rows") != res.get("row_count"):
        raise Unavailable(f"world.inspect returned {res.get('returned_rows')} of {res.get('row_count')} rows; "
                          f"the engine caps limit at {INSPECT_LIMIT} and has no paging, so the full state "
                          f"cannot be compared")
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


def install(tctx, sess, wid, cid, rev, tick) -> dict:
    # candidate.install is not implemented by the M0 engine; tests that call this are gated on the
    # capability and never reach it. Kept so the intended shape is written down in one place.
    return call(tctx, sess, "candidate.install",
                {"world_id": wid, "candidate_id": cid, "expected_world_revision": rev,
                 "allowed_effective_tick": tick})


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

def t00(t: TestCtx):
    if t.run.no_build:
        raise NotRun("build skipped by --no-build; the bootstrap command was not run in this invocation")
    for rel in ("tools/dev.py", "CMakeLists.txt"):
        t.need_file(rel)
    rc, out = t.run_cmd([sys.executable, "tools/dev.py", "build"])
    t.check("documented build command returncode", 0, rc)
    t.details["tool_versions"] = parse_build_tool_lines(out)
    binary = t.binary
    if not os.path.isfile(binary):
        raise Failed(f"binary not found after build: {binary}")
    t.inputs[t.rel(binary)] = sha256_file(binary)
    t.details["binary"] = {"path": t.rel(binary), "sha256": t.inputs[t.rel(binary)]}
    rc2, out2 = t.run_cmd([binary, "--version"], timeout=30)
    t.check("--version returncode", 0, rc2)
    fields = parse_version_output(out2)
    t.details["version_output"] = out2.strip()
    t.check("abi_version equals include/sutekh/abi.h", str(read_header_constant("include/sutekh/abi.h", "SU_ABI_VERSION")),
            fields.get("abi_version"))
    t.check("ir_version equals include/sutekh/ir.h", read_header_constant("include/sutekh/ir.h", "SU_IR_VERSION_STRING"),
            fields.get("ir_version"))
    t.check("artifact_format equals include/sutekh/ir.h",
            str(read_header_constant("include/sutekh/ir.h", "SU_ARTIFACT_FORMAT_VERSION")),
            fields.get("artifact_format"))
    t.check("sutekh version line present", True, fields.get("sutekh") is not None)


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
    seen = []
    for n in counts:
        # A worker holds at most 4 worlds (worker.c), so each world gets its own serve process.
        sess = t.session(f"T02 serve world_{n}")
        world_rel = f"tests/fixtures/t02/world_{n}.json"
        test_doc = t.load(f"tests/fixtures/t02/movement_{n}.test.json")
        if len(t.load(world_rel)["columns"]["position_x"]) != n:
            raise Failed(f"fixture world_{n} row count is not {n}")
        _t02_or_t03_case(t, sess, world_rel, cap_rel, test_doc["cases"], lir_doc, cap)
        sess.close()
        seen.append(n)
    t.details["row_counts_checked"] = seen


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


BOUND_MANIFEST = "tests/fixtures/invalid/bound_manifest.json"  # written by make_negative_bindings.py


def _negative_manifest(t, rel):
    doc = t.load(rel)
    bound = t.load(BOUND_MANIFEST)
    base_dir = os.path.dirname(rel)
    cases = []
    for case in doc["cases"]:
        cap_rel = os.path.normpath(os.path.join(base_dir, case.get("capsule", doc["capsule"]))).replace(os.sep, "/")
        src_rel = os.path.normpath(os.path.join(base_dir, case["path"])).replace(os.sep, "/")
        if declared_source(t, cap_rel) != src_rel and src_rel in bound:
            cap_rel = bound[src_rel]  # a copy of the capsule whose source field names src_rel
        cases.append((cap_rel, src_rel, case["expected_code"], case.get("path")))
    return cases


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
    cases = _negative_manifest(t, "tests/fixtures/negative_cases.json")
    cases += _negative_manifest(t, "examples/negative_cases.json")
    results, failures, not_expressible = [], [], []
    for cap_rel, src_rel, code, _label in cases:
        t.need_file(cap_rel)
        t.need_file(src_rel)
        declared = declared_source(t, cap_rel)
        if declared != src_rel:
            not_expressible.append(f"{src_rel} (capsule {cap_rel} binds {declared})")
            continue
        env = sess.request("capsule.submit", {"capsule_path": cap_rel})
        if env["ok"] is False and env["error"].get("code") == UNSUPPORTED:
            raise Unavailable("capsule.submit answered SU_E_UNSUPPORTED_METHOD")
        mism = classify_error(env, code)
        after = inspect(t, sess, wid)
        mism += verify_rejections_leave_state(before_snapshot, after)
        t.checks.append({"name": f"negative {src_rel} -> {code}", "mismatches": mism[:5], "ok": not mism})
        results.append({"source": src_rel, "expected": code, "ok": not mism,
                        "actual": None if env["ok"] else env["error"].get("code")})
        if mism:
            failures.append(f"{src_rel}: {'; '.join(mism[:2])}")
    t.details["negative_cases"] = results
    t.details["not_expressible"] = not_expressible
    if failures:
        raise Failed(f"{len(failures)} of {len(cases)} negative cases not rejected as expected: "
                     + " | ".join(failures[:6]))
    if not_expressible:
        raise Unavailable(f"{len(not_expressible)} of {len(cases)} negative cases cannot be expressed: "
                          "capsule.submit takes only capsule_path and the engine binds each capsule to its "
                          "own source field; the other cases were checked and all rejected as expected")


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
    sess = t.session("T05 serve")
    sched, expected = _swap_expected(t)
    swap_tick = sched["swap"]["effective_tick"]
    pid0 = call(t, sess, "engine.info", {}).get("pid")
    wid, rev0 = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    t.check_mismatches("T05 tick 0", verify_state(0, expected[0], inspect(t, sess, wid)))
    step(t, sess, wid, 2)
    pre = inspect(t, sess, wid)
    t.check_mismatches("T05 v1 at swap tick", verify_state(swap_tick, expected[swap_tick], pre))
    t.check("worker pid reported before swap", True, isinstance(pre.get("worker_pid"), int))
    manifest_v1 = pre.get("semantic_manifest")
    cand = submit(t, sess, EDIT_V2, V2_SRC)
    install(t, sess, wid, cand, pre["world_revision"], swap_tick)
    mid = inspect(t, sess, wid)
    t.check_mismatches("T05 install does not step the world", verify_state(swap_tick, expected[swap_tick], mid))
    t.check("semantic manifest changes at install", True, mid.get("semantic_manifest") != manifest_v1)
    step(t, sess, wid, 1)
    t.check_mismatches("T05 first v2 tick", verify_state(3, expected[3], inspect(t, sess, wid)))
    step(t, sess, wid, 1)
    post = inspect(t, sess, wid)
    t.check_mismatches("T05 second v2 tick", verify_state(4, expected[4], post))
    t.check("supervisor pid preserved", pid0, call(t, sess, "engine.info", {}).get("pid"))
    t.check("worker pid preserved", pre.get("worker_pid"), post.get("worker_pid"))


def t06(t: TestCtx):
    sess = t.session("T06 serve")
    sched, expected = _swap_expected(t)
    wid, _rev = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, 2)
    pre = inspect(t, sess, wid)
    item, cap_rel, src_rel = _find_candidate(t, EDIT_CANDS, "wrong_permission")
    rejected, env = _submit_expect_rejection(t, sess, cap_rel, src_rel,
                                             item["expected_code"])
    if not rejected:
        cid = env["result"].get("candidate_id") if env["ok"] else None
        if not isinstance(cid, str):
            raise Failed("accepted submission has no candidate_id")
        env2 = sess.request("candidate.install", {"world_id": wid, "candidate_id": cid,
                                                  "expected_world_revision": pre["world_revision"],
                                                  "allowed_effective_tick": sched["swap"]["effective_tick"]})
        if env2["ok"] is False and env2["error"].get("code") == UNSUPPORTED:
            raise Unavailable("candidate.install answered SU_E_UNSUPPORTED_METHOD")
        t.check_mismatches(f"install rejection {item['expected_code']}", classify_error(env2, item["expected_code"]))
    post = inspect(t, sess, wid)
    t.check_mismatches("failed swap leaves committed state unchanged",
                       verify_rejections_leave_state(pre, post))
    t.check("active semantic manifest unchanged", pre.get("semantic_manifest"), post.get("semantic_manifest"))
    v1_next = ref.run_tick(t.load("tests/fixtures/edit/movement.lir.json"),
                           t.load(EDIT_V1), expected[2])
    step(t, sess, wid, 1)
    t.check_mismatches("active code is still v1 after failed swap",
                       verify_state(3, v1_next, inspect(t, sess, wid)))


def t07(t: TestCtx):
    sess = t.session("T07 serve")
    _sched, _expected = _swap_expected(t)
    item, cap_rel, src_rel = _find_candidate(t, EDIT_CANDS, "stale_revision")
    wid, rev0 = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, 1)
    pre = inspect(t, sess, wid)
    if pre["world_revision"] == rev0:
        raise NotRun("engine did not change world_revision on a tick; a stale expectation cannot be constructed")
    stale = rev0 if item["expected_world_revision"] == "@captured_before_mutation" else item["expected_world_revision"]
    rejected, env = _submit_expect_rejection(t, sess, cap_rel, src_rel, item["expected_code"])
    if not rejected:
        cid = env["result"]["candidate_id"]
        env2 = sess.request("candidate.install", {"world_id": wid, "candidate_id": cid,
                                                  "expected_world_revision": stale,
                                                  "allowed_effective_tick": pre["tick"]})
        if env2["ok"] is False and env2["error"].get("code") == UNSUPPORTED:
            raise Unavailable("candidate.install answered SU_E_UNSUPPORTED_METHOD")
        t.check_mismatches("stale install rejection", classify_error(env2, item["expected_code"]))
    post = inspect(t, sess, wid)
    t.check_mismatches("stale rejection leaves state unchanged", verify_rejections_leave_state(pre, post))
    # Control: the same candidate with the current revision installs.
    cid = submit(t, sess, cap_rel, src_rel)
    install(t, sess, wid, cid, pre["world_revision"], pre["tick"])


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
    if "snapshot_format" in snap:
        t.check("snapshot_format", "0.1", snap["snapshot_format"])
    step(t, sess, wid, 2)
    later = inspect(t, sess, wid)
    v1 = t.load(EDIT_V1)
    v1_lir = t.load("tests/fixtures/edit/movement.lir.json")
    t.check_mismatches("T08 state at tick 4 before restore",
                       verify_state(4, ref.run_ticks(v1_lir, v1, expected[2], 2), later))
    res = call(t, sess, "world.restore", {"world_id": wid, "snapshot_id": sid,
                                          "expected_world_revision": later["world_revision"]})
    t.check("restore assigns a new revision", True, res.get("world_revision") != later["world_revision"])
    if "restored_from" in res:
        t.check("restored_from names the snapshot", sid, res["restored_from"])
    restored = inspect(t, sess, wid)
    t.check_mismatches("T08 restored tick and columns", verify_state(2, expected[2], restored))
    t.check("T08 semantic manifest identical to snapshot-time manifest", at2.get("semantic_manifest"),
            restored.get("semantic_manifest"))
    t.check("restored revision matches restore result", res.get("world_revision"), restored["world_revision"])


def t09(t: TestCtx):
    manifest = t.load("tests/fixtures/t09/manifest.json")
    for s in manifest["systems_in_order"]:
        t.need_file("tests/fixtures/t09/" + s)
    raise Unavailable("multi-system worlds are not implemented: world.create takes one capsule_path "
                      "(capsule_paths is rejected with SU_E_TYPE), so the second-system working-root "
                      "failure cannot be constructed")


def t10(t: TestCtx):
    sess = t.session("T10 serve")
    wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, 1)
    cid = submit(t, sess, EDIT_V2, V2_SRC)
    status = call(t, sess, "candidate.status", {"candidate_id": cid})
    if not isinstance(status.get("worker_pid"), int):
        raise Unavailable("candidate.status exposes no worker_pid and candidate.evaluate is synchronous "
                          "(no job id), so the candidate worker cannot be SIGKILLed mid-evaluation "
                          "from the control plane")


def _rss_kib(pid):
    path = f"/proc/{pid}/status"
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return next((int(line.split()[1]) for line in fh if line.startswith("VmRSS:")), None)


def t11(t: TestCtx):
    """Each cycle: install v2 (fresh candidate), tick, reject a bad candidate, install v1, tick.

    Every tick is compared with the reference evaluator and every manifest with the one
    captured for that version, so a stale reference would surface as a mismatch.
    """
    sess = t.session("T11 serve")
    cycles = t.run.t11_cycles
    pid = call(t, sess, "engine.info", {}).get("pid")
    if _rss_kib(pid) is None:
        raise NotRun("process RSS not observable (no /proc)")
    v1_cap, v1_src, v2_cap, v2_src = EDIT_V1, V1_SRC, EDIT_V2, V2_SRC
    v1_doc, v1_lir = t.load_system(v1_cap)[0], t.load(V1_SRC)
    v2_doc, v2_lir = t.load_system(v2_cap)[0], t.load(V2_SRC)
    item, bad_cap, bad_src = _find_candidate(t, EDIT_CANDS, "wrong_permission")
    wid, _ = create_world(t, sess, EDIT_WORLD, [v1_cap])
    cols = t.load(EDIT_WORLD)["columns"]
    st = inspect(t, sess, wid)
    m_v1 = st.get("semantic_manifest")
    m_v2 = None
    samples = []
    rejected_total = 0
    for i in range(cycles):
        st = inspect(t, sess, wid)
        cid = submit(t, sess, v2_cap, v2_src)
        install(t, sess, wid, cid, st["world_revision"], st["tick"])
        step(t, sess, wid, 1)
        cols = ref.run_tick(v2_lir, v2_doc, cols)
        st = inspect(t, sess, wid)
        t.check_mismatches(f"cycle {i} v2 columns", compare_columns(cols, st["columns"]))
        if m_v2 is None:
            m_v2 = st.get("semantic_manifest")
            t.check("v2 manifest differs from v1", True, m_v2 != m_v1)
        t.check_mismatches(f"cycle {i} v2 manifest", compare_manifest(m_v2, st.get("semantic_manifest")))

        before_bad = inspect(t, sess, wid)
        rejected, _env = _submit_expect_rejection(t, sess, bad_cap, bad_src, item["expected_code"])
        if not rejected:
            raise Failed(f"cycle {i}: invalid candidate was accepted")
        rejected_total += 1
        after_bad = inspect(t, sess, wid)
        t.check_mismatches(f"cycle {i} rejected candidate left state unchanged",
                           verify_rejections_leave_state(before_bad, after_bad))

        st = inspect(t, sess, wid)
        cid = submit(t, sess, v1_cap, v1_src)
        install(t, sess, wid, cid, st["world_revision"], st["tick"])
        step(t, sess, wid, 1)
        cols = ref.run_tick(v1_lir, v1_doc, cols)
        st = inspect(t, sess, wid)
        t.check_mismatches(f"cycle {i} v1 columns", compare_columns(cols, st["columns"]))
        t.check_mismatches(f"cycle {i} v1 manifest", compare_manifest(m_v1, st.get("semantic_manifest")))
        if i % 100 == 0 or i == cycles - 1:
            samples.append({"cycle": i, "rss_kib": _rss_kib(pid)})
    t.details["cycles"] = cycles
    t.details["rejected_candidates"] = rejected_total
    t.details["rss_samples_kib"] = samples
    if t.run.t11_bound_kib is None:
        raise NotRun("no retained-memory bound is declared in TEST_SPEC; RSS samples recorded and "
                     "every reload check passed; rerun with --t11-max-growth-kib to judge growth")
    growth = samples[-1]["rss_kib"] - samples[0]["rss_kib"]
    t.details["rss_growth_kib"] = growth
    t.check_mismatches("retained memory growth within declared bound",
                       [] if growth <= t.run.t11_bound_kib else [f"growth {growth} KiB > {t.run.t11_bound_kib}"])


def t12(t: TestCtx):
    sched, expected = _swap_expected(t)
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
    # Control: prove the shim records an invocation, so an empty log means something.
    if "cc" in shims:
        subprocess.run([os.path.join(shim_root, "cc"), "--version"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
        with open(log, encoding="utf-8") as fh:
            t.check("instrumentation control records a compiler invocation", True, bool(fh.read().strip()))
        open(log, "w", encoding="utf-8").close()
    else:
        raise NotRun("no C compiler on PATH to instrument")
    sess = t.session("T12 serve (instrumented PATH)", env=env)
    samples = []
    stop = threading.Event()
    hits = []

    def sampler():
        while not stop.is_set():
            now = time.time()
            ppid, comm = {}, {}
            for entry in os.listdir("/proc"):
                if not entry.isdigit():
                    continue
                try:
                    with open(f"/proc/{entry}/stat", encoding="utf-8") as fh:
                        data = fh.read()
                except OSError:
                    continue
                name = data[data.index("(") + 1:data.rindex(")")]
                fields = data[data.rindex(")") + 2:].split()
                ppid[int(entry)] = int(fields[1])
                comm[int(entry)] = name
            desc = {sess.proc.pid}
            changed = True
            while changed:
                changed = False
                for pid, parent in ppid.items():
                    if parent in desc and pid not in desc:
                        desc.add(pid)
                        changed = True
            for pid in desc:
                if comm.get(pid) in BUILD_COMMS:
                    hits.append((now, comm[pid], pid))
            time.sleep(0.005)

    wid, rev0 = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    step(t, sess, wid, 2)
    thread = threading.Thread(target=sampler, daemon=True)
    thread.start()
    window_start = time.time()
    try:
        pre = inspect(t, sess, wid)
        cid = submit(t, sess, EDIT_V2, V2_SRC)
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
    t.details["shim_invocations_total"] = len(lines)
    t.details["shim_invocations_in_swap_window"] = in_window
    t.details["sampled_build_processes_in_swap_window"] = [
        {"comm": c, "pid": p} for ts, c, p in hits if window_start <= ts <= window_end]
    t.check("no compiler, assembler, linker or build tool invoked during capsule-only swap",
            0, len(in_window) + len([h for h in hits if window_start <= h[0] <= window_end]))
    shutil.rmtree(shim_root, ignore_errors=True)


def t13(t: TestCtx):
    sched, expected = _swap_expected(t)
    sess = t.session("T13 serve")
    reps = 20
    prepare, install_d, boundary = [], [], []
    wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
    for i in range(reps):
        rev = inspect(t, sess, wid)["world_revision"]
        t0 = time.monotonic()
        cid = submit(t, sess, EDIT_V2, V2_SRC)
        t1 = time.monotonic()
        tick = inspect(t, sess, wid)["tick"]
        install(t, sess, wid, cid, rev, tick)
        t2 = time.monotonic()
        step(t, sess, wid, 1)
        t3 = time.monotonic()
        prepare.append(t1 - t0)
        install_d.append(t2 - t1)
        boundary.append(t3 - t2)
        # swap back to v1 so the next repetition starts from a v1 install
        rev = inspect(t, sess, wid)["world_revision"]
        cid1 = submit(t, sess, EDIT_V1, V1_SRC)
        install(t, sess, wid, cid1, rev, inspect(t, sess, wid)["tick"])
        step(t, sess, wid, 1)

    def pct(values, p):
        s = sorted(values)
        return round(s[min(len(s) - 1, int(round(p * (len(s) - 1))))] * 1000, 3)

    t.details["measurements_ms"] = {
        "prepare_submit": {"p50": pct(prepare, 0.5), "p95": pct(prepare, 0.95), "n": reps},
        "queue_install": {"p50": pct(install_d, 0.5), "p95": pct(install_d, 0.95), "n": reps},
        "boundary_and_step": {"p50": pct(boundary, 0.5), "p95": pct(boundary, 0.95), "n": reps},
    }
    t.details["host_monotonic_only"] = True
    raise NotRun("TEST_SPEC section 8: timing tests compare against declared targets; none is declared "
                 "yet, so measurements are recorded but no pass or fail is claimed")


def t14(t: TestCtx):
    sched, expected = _swap_expected(t)

    def run_once(label):
        sess = t.session(f"T14 {label}")
        wid, _ = create_world(t, sess, EDIT_WORLD, [EDIT_V1])
        step(t, sess, wid, 2)
        pre = inspect(t, sess, wid)
        cid = submit(t, sess, EDIT_V2, V2_SRC)
        install(t, sess, wid, cid, pre["world_revision"], sched["swap"]["effective_tick"])
        step(t, sess, wid, 2)
        post = inspect(t, sess, wid)
        sess.close()
        return {"tick": post["tick"], "columns": post["columns"],
                "semantic_manifest": post.get("semantic_manifest")}

    first = run_once("run A")
    second = run_once("run B")
    t.check_mismatches("T14 run B state equals run A", compare_columns(first["columns"], second["columns"]))
    t.check("T14 manifest identical across runs", first["semantic_manifest"], second["semantic_manifest"])
    t.check_mismatches("T14 final state equals reference", verify_state(4, expected[4], first))


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


TESTS = [
    ("T00", "Clean bootstrap", (), t00),
    ("T01", "Reference movement", ("world.create", "world.step", "world.inspect"), t01),
    ("T02", "Empty and awkward counts", ("world.create", "world.step", "world.inspect"), t02),
    ("T03", "Numeric edges", ("world.create", "world.step", "world.inspect"), t03),
    ("T04", "Invalid input matrix", ("world.create", "world.inspect", "capsule.submit"), t04),
    ("T05", "Successful code-only swap", ("world.create", "world.step", "world.inspect", "capsule.submit",
                                          "candidate.install"), t05),
    ("T06", "Failed swap", ("world.create", "world.step", "world.inspect", "capsule.submit",
                            "candidate.install"), t06),
    ("T07", "Stale candidate", ("world.create", "world.step", "world.inspect", "capsule.submit",
                                "candidate.install"), t07),
    ("T08", "Snapshot round trip", ("world.create", "world.step", "world.inspect", "world.snapshot",
                                    "world.restore"), t08),
    ("T09", "Working-root failure", ("world.create", "world.step", "world.inspect"), t09),
    ("T10", "Candidate isolation", ("world.create", "world.step", "world.inspect", "capsule.submit",
                                    "candidate.evaluate", "candidate.status"), t10),
    ("T11", "Repeated reload", ("world.create", "world.step", "world.inspect", "capsule.submit",
                                "candidate.install"), t11),
    ("T12", "No native gameplay build", ("world.create", "world.step", "world.inspect", "capsule.submit",
                                         "candidate.install"), t12),
    ("T13", "Latency decomposition", ("world.create", "world.step", "world.inspect", "capsule.submit",
                                      "candidate.install"), t13),
    ("T14", "Deterministic fixture repeat", ("world.create", "world.step", "world.inspect", "capsule.submit",
                                             "candidate.install"), t14),
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
    details.update(t.details)
    return {"id": test_id, "name": name, "status": status, "reason": reason, "checks": t.checks,
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
        lines.append(f"{r['id']:4} {r['status']:12} {r['name']}")
        if r["reason"]:
            lines.append(f"       {r['reason']}")
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
    parser.add_argument("--t11-cycles", type=int, default=1000)
    parser.add_argument("--t11-max-growth-kib", type=int, default=None)
    parser.add_argument("--self-test", action="store_true", help="unit-test the harness logic and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    binary = os.path.abspath(args.binary)
    run = RunCtx(binary, args.no_build, args.t11_cycles, args.t11_max_growth_kib)
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
        "probe": {k: (sorted(v) if isinstance(v, set) else v) for k, v in run.probe.items()
                  if k != "commands"},
        "tests": results,
        "summary": summarize(results),
        "note": "passed means every check ran and matched independently derived expectations; "
                "no engine-provided flag was trusted.",
    }
    json_path, text_path, text = write_outputs(report, args.report_dir)
    print(text)
    print(f"report: {json_path}")
    print(f"summary: {text_path}")
    return 1 if report["summary"][FAILED] else 0


# ------------------------------------------------------------------ self-test

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
        t11_cycles = 1
        t11_bound_kib = None
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

    failed = [name for name, passed in checks if not passed]
    for name in failed:
        print(f"self-test FAILED: {name}")
    print(f"self-test: {len(checks)} checks, {len(failed)} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
