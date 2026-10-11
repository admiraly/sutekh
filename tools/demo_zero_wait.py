#!/usr/bin/env python3
"""Reproducible zero-wait live-edit demonstration against the REAL build/sutekh (stdlib only).

Run from anywhere:  python3 tools/demo_zero_wait.py [--out EVIDENCE.json]

Sequence (all expectations are computed by tests/harness/reference_eval.py, never taken from
the engine):
  1. start `build/sutekh serve --root <repo>`; check the supervisor pid against the OS pid.
  2. world.create examples/world.json + examples/movement.capsule.json (v1: position += velocity).
  3. step N ticks with v1; record worker_pid, worker process start time and committed state.
  4. capsule edit in a scratch copy under build/demo-scratch/ (v2: position += 2 * velocity).
     capsule.submit -> candidate.evaluate (must pass every case) while the world keeps running.
  5. step to the planned effective tick E (still v1), then candidate.install with
     allowed_effective_tick = E. The first tick computed by v2 is E+1.
  6. step M ticks: v2 values from E+1, same supervisor pid, same worker pid and start time,
     the other columns and entity ids unchanged by the install.
  7. rejected invalid edit (capsule that writes a read-only field): capsule.submit must return
     SU_E_UNDECLARED_WRITE; world revision, tick, columns and semantic manifest unchanged; v2
     keeps running afterwards.
  8. world.snapshot, two ticks, world.restore, one tick: the restored state, tick and manifest
     equal the snapshot exactly, and the next tick equals the reference result.

Every request and reply is printed with its host monotonic timestamp (CLOCK_MONOTONIC via
time.monotonic_ns, relative to demo start). The JSON evidence file (default
~/.cache/sutekh/demo_zero_wait/<UTC>.json, never inside the repository) records the binary
SHA-256, the source git HEAD and dirty state, exact commands, per-phase durations and every
RPC. Exit status: 0 when every observed value matched, 1 on any mismatch or protocol error,
2 on usage or environment errors (missing binary, missing git is tolerated and recorded).
"""
from __future__ import annotations

import argparse
import collections
import copy
import datetime
import hashlib
import json
import os
import platform
import queue
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "tests", "harness"))
import reference_eval as ref  # noqa: E402  (independent pure-Python evaluator)

EVIDENCE_FORMAT = "m0-zero-wait-demo/0.1"
PROTOCOL = "0.1"
FIELDS = ("position_x", "velocity_x")
INSPECT_PAGE = 256
UNDECLARED_WRITE = "SU_E_UNDECLARED_WRITE"
EXAMPLE_WORLD = "examples/world.json"
EXAMPLE_CAPSULE = "examples/movement.capsule.json"
EXAMPLE_DIR = "examples"
V2_REFERENCE = "examples/movement_v2.lir.json"
BAD_LIR = "examples/invalid/undeclared_write.lir.json"

T0 = time.monotonic_ns()


def mono_ns() -> int:
    return time.monotonic_ns()


def ms_since_start(ns: int) -> float:
    return (ns - T0) / 1e6


class DemoFailure(Exception):
    """A checked value differed from its independent expectation, or the protocol broke."""


def say(msg: str) -> None:
    print(msg, flush=True)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def sha256_obj(obj) -> str:
    return hashlib.sha256(json.dumps(obj, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=False).encode("utf-8")).hexdigest()


def load_json(rel: str):
    with open(os.path.join(REPO, rel), encoding="utf-8") as fh:
        return json.load(fh)


def write_json(path: str, obj) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2, sort_keys=True, ensure_ascii=False)
        fh.write("\n")


def git(*args: str):
    try:
        out = subprocess.run(["git", "-C", REPO, *args], capture_output=True, text=True,
                             shell=False, check=False)
        return out.returncode, out.stdout
    except OSError as exc:
        return None, str(exc)


def proc_starttime(pid: int):
    """Field 22 of /proc/<pid>/stat: distinguishes the same PID from a recycled one."""
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8") as fh:
            data = fh.read()
    except OSError:
        return None
    return int(data.rsplit(")", 1)[1].split()[19])


class Serve:
    """One `<binary> serve --root <repo>` process, speaking the JSON-lines protocol."""

    def __init__(self, binary: str, evidence: dict):
        self.argv = [binary, "serve", "--root", REPO]
        self.proc = subprocess.Popen(self.argv, cwd=REPO, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False)
        self.replies: queue.Queue = queue.Queue()
        self.stderr = collections.deque(maxlen=40)
        self.seq = 0
        self.evidence = evidence
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._read_err, daemon=True).start()

    def _read(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.replies.put(line)
        self.replies.put(None)

    def _read_err(self):
        for line in iter(self.proc.stderr.readline, b""):
            self.stderr.append(line.decode("utf-8", "replace"))

    def call(self, phase: str, method: str, params: dict, timeout: float = 60.0) -> dict:
        self.seq += 1
        rid = f"demo-{self.seq}"
        body = json.dumps({"protocol": PROTOCOL, "id": rid, "method": method, "params": params},
                          separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\n"
        sent = mono_ns()
        say(f"[+{ms_since_start(sent):11.3f} ms] -> {rid} {method} {json.dumps(params, sort_keys=True)[:160]}")
        self.proc.stdin.write(body)
        self.proc.stdin.flush()
        try:
            raw = self.replies.get(timeout=timeout)
        except queue.Empty:
            raise DemoFailure(f"{method}: no reply within {timeout}s") from None
        got = mono_ns()
        if raw is None:
            raise DemoFailure(f"{method}: serve closed stdout; stderr: {''.join(self.stderr)[-400:]!r}")
        try:
            env = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            raise DemoFailure(f"{method}: reply is not JSON: {exc}") from None
        if not isinstance(env, dict) or env.get("protocol") != PROTOCOL or env.get("id") != rid \
                or not isinstance(env.get("ok"), bool):
            raise DemoFailure(f"{method}: malformed envelope {raw[:200]!r}")
        code = None if env["ok"] else (env.get("error") or {}).get("code")
        status = "ok" if env["ok"] else f"error {code}"
        say(f"[+{ms_since_start(got):11.3f} ms] <- {rid} {status} ({(got - sent) / 1e6:.3f} ms)")
        self.evidence["rpc"].append({"id": rid, "phase": phase, "method": method,
                                     "sent_ms": round(ms_since_start(sent), 3),
                                     "received_ms": round(ms_since_start(got), 3),
                                     "duration_ms": round((got - sent) / 1e6, 3),
                                     "ok": env["ok"], "error_code": code})
        return env

    def ok(self, phase: str, method: str, params: dict, timeout: float = 60.0) -> dict:
        env = self.call(phase, method, params, timeout)
        if not env["ok"]:
            raise DemoFailure(f"{method} failed: {env.get('error')}")
        return env["result"]

    def close(self) -> int:
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            return self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return self.proc.wait()


class Demo:
    def __init__(self, args):
        self.args = args
        self.checks: list = []
        self.table: list = []
        self.phases: list = []
        self.evidence = {"evidence_format": EVIDENCE_FORMAT, "status": "running", "failure": None,
                         "rpc": [], "checks": self.checks, "step_table": self.table,
                         "phases": self.phases}
        self.serve = None
        self.wid = None
        self.rev = None
        self.worker_pid = None
        self.worker_start = None
        self.v1 = None
        self.state = None

    # ---------------------------------------------------------------- bookkeeping
    def check(self, phase: str, name: str, expected, actual) -> None:
        ok = expected == actual
        self.checks.append({"phase": phase, "name": name, "expected": expected,
                            "actual": actual, "ok": ok})
        mark = "ok  " if ok else "FAIL"
        say(f"    check {mark} {name}: expected {repr(expected)[:140]} actual {repr(actual)[:140]}")
        if not ok:
            raise DemoFailure(f"{phase}: {name}: expected {expected!r} actual {actual!r}")

    def phase_start(self, name: str) -> dict:
        say(f"== phase {name}")
        return {"name": name, "start_ms": round(ms_since_start(mono_ns()), 3)}

    def phase_end(self, rec: dict) -> None:
        end = mono_ns()
        rec["end_ms"] = round(ms_since_start(end), 3)
        rec["duration_ms"] = round(rec["end_ms"] - rec["start_ms"], 3)
        self.phases.append(rec)
        say(f"   phase {rec['name']} took {rec['duration_ms']:.3f} ms")

    def row(self, phase: str, method: str, state: dict, matches: bool) -> None:
        self.table.append({
            "n": len(self.table) + 1, "phase": phase, "method": method, "tick": state["tick"],
            "world_revision": state["world_revision"], "worker_pid": state["worker_pid"],
            "position_x": state["columns"]["position_x"], "velocity_x": state["columns"]["velocity_x"],
            "manifest_hash16": (state.get("semantic_manifest") or [{}])[0].get("semantic_hash", "")[:16],
            "matches_reference": matches})

    def inspect(self, phase: str) -> dict:
        pages, offset, row_count = [], 0, None
        while True:
            res = self.serve.ok(phase, "world.inspect", {"world_id": self.wid, "fields": list(FIELDS),
                                                         "limit": INSPECT_PAGE, "offset": offset})
            pages.append(res)
            row_count = res["row_count"]
            offset += res["returned_rows"]
            if res["returned_rows"] == 0 or offset >= row_count:
                break
        columns = {f: [] for f in FIELDS}
        entity_ids = []
        for page in pages:
            for f in FIELDS:
                columns[f].extend(page["columns"][f])
            entity_ids.extend(page["entity_ids"])
        head = pages[0]
        if offset != row_count or len(entity_ids) != row_count:
            raise DemoFailure(f"{phase}: paged {offset} rows, row_count {row_count}")
        return {"world_revision": head["world_revision"], "tick": head["tick"],
                "row_count": row_count, "worker_pid": head["worker_pid"],
                "semantic_manifest": head["semantic_manifest"], "entity_ids": entity_ids,
                "columns": columns}

    @staticmethod
    def manifest_identity(manifest):
        return [{k: m.get(k) for k in ("system_id", "semantic_hash", "source_hash", "capsule_hash")}
                for m in manifest]

    def check_worker(self, phase: str, state: dict) -> None:
        self.check(phase, "worker_pid unchanged", self.worker_pid, state["worker_pid"])
        self.check(phase, "worker process start time unchanged (same process, not a recycled pid)",
                   self.worker_start, proc_starttime(self.worker_pid))

    def expect_state(self, phase: str, expected_tick: int, expected_cols: dict,
                     method: str, label: str) -> dict:
        st = self.inspect(phase)
        self.check(phase, f"{label}: tick", expected_tick, st["tick"])
        self.check(phase, f"{label}: position_x matches reference evaluator",
                   list(expected_cols["position_x"]), st["columns"]["position_x"])
        self.check(phase, f"{label}: velocity_x matches reference evaluator",
                   list(expected_cols["velocity_x"]), st["columns"]["velocity_x"])
        self.check(phase, f"{label}: entity ids", self.world_entities, st["entity_ids"])
        self.check(phase, f"{label}: worker pid", self.worker_pid, st["worker_pid"])
        self.row(phase, method, st, True)
        self.state = st
        return st

    # ---------------------------------------------------------------- phases
    def run(self) -> int:
        a = self.args
        binary = os.path.realpath(a.binary)
        if not os.access(binary, os.X_OK):
            say(f"error: no executable at {binary}; run: python3 tools/dev.py build")
            return 2
        self.binary = binary
        self.evidence["binary"] = {"path": binary, "bytes": os.path.getsize(binary),
                                   "sha256": sha256_file(binary)}
        rc_head, head = git("rev-parse", "HEAD")
        rc_st, status = git("status", "--porcelain")
        self.evidence["source"] = {
            "repo_root": REPO,
            "git_head": head.strip() if rc_head == 0 else None,
            "git_dirty": bool(status.strip()) if rc_st == 0 else None,
            "dirty_paths": status.splitlines()[:200] if rc_st == 0 else None,
        }
        self.evidence["machine"] = {"platform": platform.platform(), "python": sys.version.split()[0],
                                    "cpu_count": os.cpu_count(), "clock": "time.monotonic_ns (host)"}
        self.evidence["commands"] = {
            "demo": [sys.executable, os.path.relpath(os.path.abspath(__file__), REPO), *a.argv_echo],
            "serve": [os.path.relpath(binary, REPO), "serve", "--root", "<repo>"],
            "build": ["python3", "tools/dev.py", "build"]}
        self.evidence["parameters"] = {"ticks_before": a.ticks_before, "ticks_after": a.ticks_after,
                                       "effective_tick": a.ticks_before + 1}
        status_code = 1
        try:
            self.body()
            self.evidence["status"] = "passed"
            status_code = 0
        except DemoFailure as exc:
            self.evidence["status"] = "failed"
            self.evidence["failure"] = str(exc)
            say(f"FAILED: {exc}")
        finally:
            if self.serve is not None:
                rc = self.serve.close()
                self.evidence["serve_exit_code"] = rc
                if self.evidence["status"] == "passed":
                    self.check("shutdown", "serve exits 0 after stdin closes", 0, rc)
            self.evidence["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        self.print_table()
        write_json(a.out, self.evidence)
        say(f"evidence: {a.out}")
        say(f"status: {self.evidence['status']}")
        return status_code

    def body(self) -> None:
        a = self.args
        E = a.ticks_before + 1
        M = a.ticks_after
        # 1. serve and supervisor identity
        ph = self.phase_start("start_serve")
        self.serve = Serve(self.binary, self.evidence)
        self.evidence["serve_argv"] = self.serve.argv
        info = self.serve.ok("start_serve", "engine.info", {})
        self.check("start_serve", "supervisor pid is the serve process", self.serve.proc.pid,
                   info["process"]["pid"])
        caps = info.get("capabilities", [])
        for need in ("candidate.install", "world.snapshot", "world.restore"):
            self.check("start_serve", f"capability {need} advertised", True, need in caps)
        self.evidence["supervisor_pid"] = self.serve.proc.pid
        self.phase_end(ph)

        # 2. world.create
        ph = self.phase_start("create_world")
        world = load_json(EXAMPLE_WORLD)
        self.world_entities = world["entity_ids"]
        self.world_cols = {f: list(world["columns"][f]) for f in FIELDS}
        v1_lir, v1_cap = load_json("examples/movement.lir.json"), load_json(EXAMPLE_CAPSULE)
        self.v1 = (v1_lir, v1_cap)
        created = self.serve.ok("create_world", "world.create",
                                {"fixture_path": EXAMPLE_WORLD, "capsule_path": EXAMPLE_CAPSULE})
        self.wid, self.rev = created["world_id"], created["world_revision"]
        self.worker_pid = created["worker_pid"]
        self.worker_start = proc_starttime(self.worker_pid)
        self.check("create_world", "worker pid is not the supervisor pid", False,
                   self.worker_pid == self.serve.proc.pid)
        self.check("create_world", "worker start time readable", True, self.worker_start is not None)
        st0 = self.expect_state("create_world", 0, self.world_cols, "world.create", "tick 0")
        self.check_worker("create_world", st0)
        self.phase_end(ph)

        # 3. baseline steps with v1
        ph = self.phase_start("baseline_v1")
        cols = self.world_cols
        for t in range(1, a.ticks_before + 1):
            cols = ref.run_tick(v1_lir, v1_cap, cols)
            res = self.serve.ok("baseline_v1", "world.step",
                                {"world_id": self.wid, "expected_world_revision": self.rev, "ticks": 1})
            self.rev = res["world_revision"]
            self.expect_state("baseline_v1", t, cols, "world.step", f"v1 tick {t}")
        self.check_worker("baseline_v1", self.state)
        pre_edit = {"tick": self.state["tick"], "state_sha256": sha256_obj(self.canon(self.state)),
                    "manifest": self.manifest_identity(self.state["semantic_manifest"])}
        self.evidence["pre_edit_state"] = pre_edit
        self.phase_end(ph)

        # 4. scratch edit, submit, evaluate (world keeps its state while this happens)
        ph = self.phase_start("edit_submit_evaluate")
        edit_start_epoch = time.time_ns()
        edit_start_mono = mono_ns()
        scratch_rel = os.path.join("build", "demo-scratch",
                                   datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%f")
                                   + f"-{os.getpid()}")
        scratch = os.path.join(REPO, scratch_rel)
        self.scratch = scratch
        try:
            self.materialise_v2(scratch, scratch_rel, v1_lir, v1_cap)
            self.materialise_bad(os.path.join(scratch, "bad"), scratch_rel)
            cap_rel = f"{scratch_rel}/movement.capsule.json"
            test_rel = f"{scratch_rel}/movement.test.json"
            sub = self.serve.ok("edit_submit_evaluate", "capsule.submit", {"capsule_path": cap_rel})
            cid = sub["candidate_id"]
            ev = self.serve.ok("edit_submit_evaluate", "candidate.evaluate",
                               {"candidate_id": cid, "test_path": test_rel})
            self.check("edit_submit_evaluate", "candidate evaluation status", "passed", ev["status"])
            self.check("edit_submit_evaluate", "every evaluation case passed",
                       ev["cases_total"], ev["cases_passed"])
            self.check("edit_submit_evaluate", "evaluation case count", a.eval_cases, ev["cases_total"])
            self.phase_end(ph)

            # 5. advance to the effective tick under v1, then install at E
            ph = self.phase_start("advance_to_effective_tick")
            cols = ref.run_tick(v1_lir, v1_cap, cols)
            res = self.serve.ok("advance_to_effective_tick", "world.step",
                                {"world_id": self.wid, "expected_world_revision": self.rev, "ticks": 1})
            self.rev = res["world_revision"]
            self.expect_state("advance_to_effective_tick", E, cols, "world.step",
                              f"v1 at effective tick {E}")
            self.phase_end(ph)

            ph = self.phase_start("install")
            before = self.state
            ins = self.serve.ok("install", "candidate.install",
                                {"world_id": self.wid, "candidate_id": cid,
                                 "expected_world_revision": self.rev, "allowed_effective_tick": E})
            self.check("install", "install state", "installed", ins.get("state"))
            self.check("install", "install effective tick", E, ins.get("effective_tick"))
            self.rev = ins["world_revision"]
            after = self.expect_state("install", E, cols, "candidate.install",
                                      f"after install at tick {E} (no step)")
            self.check("install", "every column unchanged by install",
                       {f: before["columns"][f] for f in FIELDS}, {f: after["columns"][f] for f in FIELDS})
            self.check("install", "entity ids unchanged by install", before["entity_ids"], after["entity_ids"])
            self.check("install", "semantic manifest changed by install", True,
                       self.manifest_identity(before["semantic_manifest"]) !=
                       self.manifest_identity(after["semantic_manifest"]))
            self.check_worker("install", after)
            self.evidence["edit_window"] = {"start_epoch_ns": edit_start_epoch,
                                            "start_mono_ms": round(ms_since_start(edit_start_mono), 3),
                                            "end_epoch_ns": time.time_ns(),
                                            "end_mono_ms": round(ms_since_start(mono_ns()), 3),
                                            "candidate_id": cid}
            self.phase_end(ph)

            # 6. v2 after the effective tick
            ph = self.phase_start("v2_after_install")
            v2_lir, v2_cap = self.v2_program(v1_lir, v1_cap)
            v2_cols = cols
            behaviour_differs = None
            for k in range(1, M + 1):
                v1_next = ref.run_tick(v1_lir, v1_cap, v2_cols)
                v2_cols = ref.run_tick(v2_lir, v2_cap, v2_cols)
                if behaviour_differs is None:
                    behaviour_differs = v1_next != v2_cols
                res = self.serve.ok("v2_after_install", "world.step",
                                    {"world_id": self.wid, "expected_world_revision": self.rev, "ticks": 1})
                self.rev = res["world_revision"]
                self.expect_state("v2_after_install", E + k, v2_cols, "world.step", f"v2 tick {E + k}")
            self.check("v2_after_install", "v1 and v2 differ at the first post-install tick", True,
                       behaviour_differs)
            self.check_worker("v2_after_install", self.state)
            cols = v2_cols
            self.phase_end(ph)

            # 7. rejected invalid edit
            ph = self.phase_start("invalid_edit_rejected")
            pre = self.inspect("invalid_edit_rejected")
            bad_cap = f"{scratch_rel}/bad/movement.capsule.json"
            env = self.serve.call("invalid_edit_rejected", "capsule.submit", {"capsule_path": bad_cap})
            self.check("invalid_edit_rejected", "invalid capsule refused with SU_E_UNDECLARED_WRITE",
                       {"ok": False, "code": UNDECLARED_WRITE},
                       {"ok": env["ok"], "code": None if env["ok"] else env["error"].get("code")})
            post = self.inspect("invalid_edit_rejected")
            self.check("invalid_edit_rejected", "world revision unchanged", pre["world_revision"],
                       post["world_revision"])
            self.check("invalid_edit_rejected", "tick unchanged", pre["tick"], post["tick"])
            self.check("invalid_edit_rejected", "columns unchanged", pre["columns"], post["columns"])
            self.check("invalid_edit_rejected", "active semantic manifest unchanged",
                       self.manifest_identity(pre["semantic_manifest"]),
                       self.manifest_identity(post["semantic_manifest"]))
            self.check_worker("invalid_edit_rejected", post)
            self.state = post
            self.phase_end(ph)

            ph = self.phase_start("v2_still_active")
            cols = ref.run_tick(v2_lir, v2_cap, cols)
            res = self.serve.ok("v2_still_active", "world.step",
                                {"world_id": self.wid, "expected_world_revision": self.rev, "ticks": 1})
            self.rev = res["world_revision"]
            self.expect_state("v2_still_active", E + M + 1, cols, "world.step", "v2 after rejected edit")
            self.phase_end(ph)

            # 8. snapshot / restore
            ph = self.phase_start("snapshot_restore")
            snap_state = self.inspect("snapshot_restore")
            snap = self.serve.ok("snapshot_restore", "world.snapshot",
                                 {"world_id": self.wid, "expected_world_revision": self.rev})
            self.check("snapshot_restore", "snapshot tick", snap_state["tick"], snap["tick"])
            snap_hash = sha256_obj(self.canon(snap_state))
            snap_manifest = self.manifest_identity(snap_state["semantic_manifest"])
            cols_at_snap = copy.deepcopy(snap_state["columns"])
            tick_at_snap = snap_state["tick"]
            two = copy.deepcopy(cols_at_snap)
            for _ in range(2):
                two = ref.run_tick(v2_lir, v2_cap, two)
            for _ in range(2):
                res = self.serve.ok("snapshot_restore", "world.step",
                                    {"world_id": self.wid, "expected_world_revision": self.rev, "ticks": 1})
                self.rev = res["world_revision"]
            moved = self.inspect("snapshot_restore")
            self.check("snapshot_restore", "state moved on before restore (tick)", tick_at_snap + 2, moved["tick"])
            self.check("snapshot_restore", "state moved on before restore (columns match reference)",
                       {f: list(two[f]) for f in FIELDS}, {f: moved["columns"][f] for f in FIELDS})
            rest = self.serve.ok("snapshot_restore", "world.restore",
                                 {"world_id": self.wid, "snapshot_id": snap["snapshot_id"],
                                  "expected_world_revision": moved["world_revision"]})
            self.rev = rest["world_revision"]
            back = self.inspect("snapshot_restore")
            self.check("snapshot_restore", "restored tick equals snapshot tick", tick_at_snap, back["tick"])
            self.check("snapshot_restore", "restored canonical state equals snapshot state byte-for-byte",
                       snap_hash, sha256_obj(self.canon(back)))
            self.check("snapshot_restore", "restored semantic manifest equals snapshot manifest",
                       snap_manifest, self.manifest_identity(back["semantic_manifest"]))
            self.row("snapshot_restore", "world.restore", back, True)
            res = self.serve.ok("snapshot_restore", "world.step",
                                {"world_id": self.wid, "expected_world_revision": self.rev, "ticks": 1})
            self.rev = res["world_revision"]
            nxt = ref.run_tick(v2_lir, v2_cap, cols_at_snap)
            self.expect_state("snapshot_restore", tick_at_snap + 1, nxt, "world.step", "step after restore")
            self.check_worker("snapshot_restore", self.state)
            self.evidence["snapshot"] = {"snapshot_id": snap["snapshot_id"], "tick": tick_at_snap,
                                         "state_sha256": snap_hash}
            self.phase_end(ph)
        finally:
            if not a.keep_scratch:
                shutil.rmtree(scratch, ignore_errors=True)
            self.evidence["scratch_dir_kept"] = bool(a.keep_scratch)

        self.evidence["worker"] = {"pid": self.worker_pid, "start_time_ticks": self.worker_start,
                                   "same_pid_all_run": all(r["worker_pid"] == self.worker_pid
                                                           for r in self.table)}
        self.check("summary", "worker pid identical in every step row", True,
                   self.evidence["worker"]["same_pid_all_run"])

    # ---------------------------------------------------------------- scratch edits
    def v2_program(self, v1_lir, v1_cap):
        prog = copy.deepcopy(v1_lir)
        ins = prog["instructions"]
        idx = [i for i, x in enumerate(ins) if x["op"] == "add_u32"]
        if len(idx) != 1 or ins[idx[0]]["a"] != "p" or ins[idx[0]]["b"] != "v":
            raise DemoFailure("v1 LIR has an unexpected shape; expected a single add_u32 p + v")
        i = idx[0]
        ins[i:i + 1] = [{"op": "const_u32", "dst": "two", "value": 2},
                        {"op": "mul_u32", "dst": "twice_v", "a": "v", "b": "two"},
                        {"op": "add_u32", "dst": "next", "a": "p", "b": "twice_v"}]
        return prog, v1_cap

    def materialise_v2(self, scratch: str, scratch_rel: str, v1_lir, v1_cap) -> None:
        os.makedirs(scratch, exist_ok=True)
        shutil.copy(os.path.join(REPO, EXAMPLE_WORLD), os.path.join(scratch, "world.json"))
        shutil.copy(os.path.join(REPO, EXAMPLE_CAPSULE), os.path.join(scratch, "movement.capsule.json"))
        v2_lir, cap = self.v2_program(v1_lir, v1_cap)
        cross = load_json(V2_REFERENCE)
        self.check("edit_submit_evaluate", "scratch edit reproduces examples/movement_v2.lir.json", cross, v2_lir)
        write_json(os.path.join(scratch, "movement.lir.json"), v2_lir)
        cols = {f: list(self.world_cols[f]) for f in FIELDS}
        cases = []
        for t in range(1, self.args.eval_cases + 1):
            cases.append({"name": f"v2_ticks_{t}", "ticks": t,
                          "expected_columns": ref.run_ticks(v2_lir, cap, cols, t)})
        write_json(os.path.join(scratch, "movement.test.json"),
                   {"test_format": "0.1", "system_id": "demo.movement",
                    "capsule": "movement.capsule.json", "world": "world.json", "cases": cases})
        self.evidence["scratch"] = {"dir": scratch_rel, "edit": "add_u32(p, v) -> mul_u32(v, 2) then add_u32(p, twice_v)",
                                    "files": {n: sha256_file(os.path.join(scratch, n)) for n in
                                              ("world.json", "movement.capsule.json", "movement.lir.json",
                                               "movement.test.json")}}

    def materialise_bad(self, bad_dir: str, scratch_rel: str) -> None:
        os.makedirs(bad_dir, exist_ok=True)
        shutil.copy(os.path.join(REPO, BAD_LIR), os.path.join(bad_dir, "movement.lir.json"))
        shutil.copy(os.path.join(REPO, EXAMPLE_CAPSULE), os.path.join(bad_dir, "movement.capsule.json"))
        shutil.copy(os.path.join(self.scratch, "movement.test.json"), os.path.join(bad_dir, "movement.test.json"))
        shutil.copy(os.path.join(self.scratch, "world.json"), os.path.join(bad_dir, "world.json"))
        self.evidence.setdefault("invalid_source", {})["sha256"] = sha256_file(os.path.join(bad_dir, "movement.lir.json"))

    @staticmethod
    def canon(state):
        return {"tick": state["tick"], "entity_ids": state["entity_ids"], "columns": state["columns"],
                "semantic_manifest": Demo.manifest_identity(state["semantic_manifest"])}

    def print_table(self) -> None:
        say("")
        say("step table (position_x / velocity_x are the committed columns after each row)")
        say(f"{'n':>3} {'phase':<26} {'method':<18} {'tick':>4} {'pid':>7} {'position_x':<28} {'velocity_x':<22} ref")
        for r in self.table:
            say(f"{r['n']:>3} {r['phase']:<26} {r['method']:<18} {r['tick']:>4} {r['worker_pid'] or 0:>7} "
                f"{str(r['position_x']):<28} {str(r['velocity_x']):<22} {'match' if r['matches_reference'] else 'DIFF'}")


def default_out() -> str:
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return os.path.join(os.path.expanduser("~"), ".cache", "sutekh", "demo_zero_wait", f"{stamp}-{os.getpid()}.json")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default=os.path.join(REPO, "build", "sutekh"))
    parser.add_argument("--out", default=None, help="evidence JSON path (default: outside the repository)")
    parser.add_argument("--ticks-before", type=int, default=3, help="v1 ticks before the edit")
    parser.add_argument("--ticks-after", type=int, default=3, help="v2 ticks after the install")
    parser.add_argument("--eval-cases", type=int, default=3, help="ticks checked by the candidate test file")
    parser.add_argument("--keep-scratch", action="store_true", help="keep build/demo-scratch/<id>")
    args = parser.parse_args(argv)
    if args.ticks_before < 1 or args.ticks_after < 1 or args.eval_cases < 1:
        parser.error("tick counts must be at least 1")
    args.out = args.out or default_out()
    args.argv_echo = list(argv if argv is not None else sys.argv[1:])
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    say(f"sutekh zero-wait demo (repo {REPO})")
    demo = Demo(args)
    return demo.run()


if __name__ == "__main__":
    sys.exit(main())
