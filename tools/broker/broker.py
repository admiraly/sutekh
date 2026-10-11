#!/usr/bin/env python3
"""Durable, deduplicating build/specialization job broker (M1-00, contract section 5).

Generic command jobs only: argv list, a small env allowlist, cwd inside the repo or the
state directory. The broker does not interpret gameplay edits and is never on the edit path.

State lives in <state-dir>/broker.sqlite3 (default build/broker/, git-ignored) with one log
file per job under <state-dir>/logs/. Stdlib only.

Job key = sha256 over canonical JSON of (system_id, inputs, argv, env, cwd). Submitting an
existing key returns the existing job; a failed, cancelled or stale job is re-run only with
resubmit=True. A newer job for the same system_id cancels older queued jobs, cancels older
running jobs (their children are killed by the runner) and marks them superseded.
validate_result() rejects a succeeded job whose bound identity (semantic_hash,
layout_generation, backend) no longer matches the current inputs; such a job becomes stale.

Runner: `run-once` takes an exclusive flock on <state-dir>/runner.lock. Jobs left 'running'
by a dead runner are recovered (their process groups are killed, state -> failed) when the
next runner acquires the lock. Children run in their own session with RLIMIT_AS set.

Timestamps: wall clock (time.time) for display and time.monotonic_ns for durations. Monotonic
values are only comparable within one boot.
"""

import argparse
import fcntl
import hashlib
import json
import math
import os
import resource
import signal
import sqlite3
import subprocess
import sys
import time
from contextlib import contextmanager

TOOLS_BROKER_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(TOOLS_BROKER_DIR))
DEFAULT_STATE_DIR = os.path.join(REPO_ROOT, "build", "broker")
STATES = ("queued", "running", "succeeded", "failed", "cancelled", "stale")
ACTIVE = ("queued", "running")
TERMINAL = ("succeeded", "failed", "cancelled", "stale")
BIND_FIELDS = ("semantic_hash", "layout_generation", "backend")
ENV_ALLOWLIST = frozenset({"PATH", "LANG", "LC_ALL", "TZ", "PYTHONHASHSEED", "SOURCE_DATE_EPOCH",
                           "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS"})
DEFAULT_ENV = {"PATH": "/usr/local/bin:/usr/bin:/bin"}
DEFAULT_TIMEOUT_S = 600.0
DEFAULT_MEMORY_MB = 2048
MAX_SYSTEM_ID = 128

SCHEMA = """
CREATE TABLE IF NOT EXISTS jobs (
    job_id INTEGER PRIMARY KEY AUTOINCREMENT,
    job_key TEXT NOT NULL,
    system_id TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('queued','running','succeeded','failed','cancelled','stale')),
    argv TEXT NOT NULL,
    env TEXT NOT NULL,
    cwd TEXT NOT NULL,
    inputs TEXT NOT NULL,
    bind TEXT NOT NULL,
    timeout_s REAL NOT NULL,
    memory_bytes INTEGER NOT NULL,
    submitted_wall REAL NOT NULL,
    submitted_mono_ns INTEGER NOT NULL,
    started_wall REAL,
    started_mono_ns INTEGER,
    finished_wall REAL,
    finished_mono_ns INTEGER,
    owner_pid INTEGER,
    child_pid INTEGER,
    exit_code INTEGER,
    reason TEXT,
    log_path TEXT,
    log_sha256 TEXT,
    superseded_by INTEGER
);
CREATE INDEX IF NOT EXISTS jobs_key ON jobs(job_key, job_id);
CREATE INDEX IF NOT EXISTS jobs_state ON jobs(state, job_id);
CREATE INDEX IF NOT EXISTS jobs_system ON jobs(system_id, state);
"""


class BrokerError(Exception):
    """A request the broker refuses (validation, lock held, unknown job)."""


class NotFound(BrokerError):
    pass


def canonical_json(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def job_key(system_id, inputs, argv, env, cwd):
    body = {"schema": 1, "system_id": system_id, "inputs": inputs, "argv": argv, "env": env, "cwd": cwd}
    return sha256_bytes(canonical_json(body).encode("utf-8"))


def _inside(path, root):
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:
        return False


def _kill_group(pid):
    """SIGKILL the process group led by pid, only if pid really leads its own group."""
    if not pid:
        return False
    try:
        if os.getpgid(pid) != pid:
            return False
        os.killpg(pid, signal.SIGKILL)
        return True
    except (ProcessLookupError, PermissionError):
        return False


def _p95(values):
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(0.95 * len(ordered)))
    return ordered[rank - 1]


class Broker:
    def __init__(self, state_dir=None, repo_root=REPO_ROOT):
        self.state_dir = os.path.realpath(state_dir or DEFAULT_STATE_DIR)
        self.logs_dir = os.path.join(self.state_dir, "logs")
        self.work_dir = os.path.join(self.state_dir, "work")
        self.roots = [os.path.realpath(repo_root), self.state_dir]
        os.makedirs(self.logs_dir, exist_ok=True)
        os.makedirs(self.work_dir, exist_ok=True)
        self.db_path = os.path.join(self.state_dir, "broker.sqlite3")
        self.conn = sqlite3.connect(self.db_path, timeout=30, isolation_level=None)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA synchronous=FULL")
        self.conn.executescript(SCHEMA)
        self._runner_fd = None

    def close(self):
        if self._runner_fd is not None:
            os.close(self._runner_fd)
            self._runner_fd = None
        self.conn.close()

    @contextmanager
    def _tx(self):
        self.conn.execute("BEGIN IMMEDIATE")
        try:
            yield self.conn
        except BaseException:
            self.conn.execute("ROLLBACK")
            raise
        else:
            self.conn.execute("COMMIT")

    # ----- validation -------------------------------------------------------------------

    def _validate_request(self, system_id, inputs, argv, env, cwd):
        if not isinstance(system_id, str) or not system_id or len(system_id) > MAX_SYSTEM_ID:
            raise BrokerError("system_id must be a non-empty string of at most 128 characters")
        if not isinstance(inputs, dict):
            raise BrokerError("inputs must be a JSON object")
        try:
            canonical_json(inputs)
        except (TypeError, ValueError) as exc:
            raise BrokerError(f"inputs are not canonical JSON: {exc}")
        if not isinstance(argv, list) or not argv or not all(isinstance(a, str) and "\0" not in a for a in argv):
            raise BrokerError("argv must be a non-empty list of strings without NUL")
        if not isinstance(env, dict) or not all(
                isinstance(k, str) and k in ENV_ALLOWLIST and isinstance(v, str) for k, v in env.items()):
            raise BrokerError(f"env may contain only the allowlisted keys {sorted(ENV_ALLOWLIST)}")
        real = os.path.realpath(cwd)
        if not any(_inside(real, root) for root in self.roots):
            raise BrokerError(f"cwd {real} must be inside the repository or the broker state directory")
        if not os.path.isdir(real):
            raise BrokerError(f"cwd {real} is not a directory")
        return real

    # ----- submission and queries ---------------------------------------------------------

    def submit(self, system_id, inputs, argv, env=None, cwd=None, timeout_s=DEFAULT_TIMEOUT_S,
               memory_mb=DEFAULT_MEMORY_MB, resubmit=False):
        env = dict(DEFAULT_ENV if env is None else env)
        cwd = os.path.realpath(cwd or self.work_dir)
        if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) or not timeout_s > 0:
            raise BrokerError("timeout_s must be a positive number")
        if not isinstance(memory_mb, int) or isinstance(memory_mb, bool) or memory_mb <= 0:
            raise BrokerError("memory_mb must be a positive integer")
        cwd = self._validate_request(system_id, inputs, argv, env, cwd)
        key = job_key(system_id, inputs, argv, env, cwd)
        bind = {k: inputs[k] for k in BIND_FIELDS if k in inputs}
        with self._tx() as c:
            row = c.execute("SELECT * FROM jobs WHERE job_key=? ORDER BY job_id DESC LIMIT 1", (key,)).fetchone()
            if row is not None and (row["state"] in ACTIVE + ("succeeded",) or not resubmit):
                return {"job_id": row["job_id"], "state": row["state"], "deduplicated": True, "job_key": key}
            now_w, now_m = time.time(), time.monotonic_ns()
            cur = c.execute(
                "INSERT INTO jobs (job_key, system_id, state, argv, env, cwd, inputs, bind, timeout_s, "
                "memory_bytes, submitted_wall, submitted_mono_ns) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (key, system_id, "queued", canonical_json(argv), canonical_json(env), cwd,
                 canonical_json(inputs), canonical_json(bind), float(timeout_s),
                 int(memory_mb) * 1024 * 1024, now_w, now_m))
            new_id = cur.lastrowid
            older = [r["job_id"] for r in c.execute(
                "SELECT job_id FROM jobs WHERE system_id=? AND job_id<? AND state IN ('queued','running')",
                (system_id, new_id)).fetchall()]
            for jid in older:
                c.execute("UPDATE jobs SET state='cancelled', reason=?, superseded_by=?, finished_wall=?, "
                          "finished_mono_ns=? WHERE job_id=? AND state IN ('queued','running')",
                          (f"superseded by job {new_id}", new_id, now_w, now_m, jid))
        return {"job_id": new_id, "state": "queued", "deduplicated": False, "job_key": key,
                "superseded": older}

    def get(self, job_id):
        row = self.conn.execute("SELECT * FROM jobs WHERE job_id=?", (job_id,)).fetchone()
        if row is None:
            raise NotFound(f"no job {job_id}")
        return self._row(row)

    def _row(self, row):
        d = dict(row)
        for k in ("argv", "env", "inputs", "bind"):
            d[k] = json.loads(d[k])
        return d

    def list(self, state=None, limit=200):
        if state is not None and state not in STATES:
            raise BrokerError(f"unknown state {state}")
        if state:
            rows = self.conn.execute("SELECT * FROM jobs WHERE state=? ORDER BY job_id DESC LIMIT ?",
                                     (state, int(limit))).fetchall()
        else:
            rows = self.conn.execute("SELECT * FROM jobs ORDER BY job_id DESC LIMIT ?", (int(limit),)).fetchall()
        return [self._row(r) for r in rows]

    def metrics(self):
        counts = {s: 0 for s in STATES}
        for r in self.conn.execute("SELECT state, COUNT(*) n FROM jobs GROUP BY state").fetchall():
            counts[r["state"]] = r["n"]
        waits = [(r["started_mono_ns"] - r["submitted_mono_ns"]) / 1e6 for r in self.conn.execute(
            "SELECT submitted_mono_ns, started_mono_ns FROM jobs WHERE started_mono_ns IS NOT NULL "
            "AND submitted_mono_ns IS NOT NULL").fetchall()
            if r["started_mono_ns"] >= r["submitted_mono_ns"]]
        done = counts["succeeded"] + counts["failed"]
        return {"counts": counts, "queue_length": counts["queued"],
                "p95_wait_ms": _p95(waits), "failure_rate": (counts["failed"] / done) if done else None}

    def cancel(self, job_id, reason="cancelled by request"):
        now_w, now_m = time.time(), time.monotonic_ns()
        with self._tx() as c:
            row = c.execute("SELECT state, child_pid FROM jobs WHERE job_id=?", (job_id,)).fetchone()
            if row is None:
                raise NotFound(f"no job {job_id}")
            if row["state"] not in ACTIVE:
                return {"job_id": job_id, "state": row["state"], "changed": False}
            c.execute("UPDATE jobs SET state='cancelled', reason=?, finished_wall=?, finished_mono_ns=? "
                      "WHERE job_id=? AND state IN ('queued','running')", (reason, now_w, now_m, job_id))
            child = row["child_pid"]
        if child:
            _kill_group(child)  # the runner also kills it on its next poll
        return {"job_id": job_id, "state": "cancelled", "changed": True}

    def validate_result(self, job_id, current_inputs):
        row = self.conn.execute("SELECT * FROM jobs WHERE job_id=?", (job_id,)).fetchone()
        if row is None:
            raise NotFound(f"no job {job_id}")
        bound = json.loads(row["bind"])
        current = {k: current_inputs[k] for k in BIND_FIELDS if k in current_inputs}
        mismatched = sorted(k for k in set(bound) | set(current) if bound.get(k, _MISSING) != current.get(k, _MISSING))
        if row["state"] == "succeeded" and mismatched:
            now_w, now_m = time.time(), time.monotonic_ns()
            with self._tx() as c:
                c.execute("UPDATE jobs SET state='stale', reason=?, finished_wall=COALESCE(finished_wall, ?), "
                          "finished_mono_ns=COALESCE(finished_mono_ns, ?) WHERE job_id=? AND state='succeeded'",
                          (f"stale: {','.join(mismatched)} changed", now_w, now_m, job_id))
            return {"job_id": job_id, "status": "stale", "mismatched": mismatched}
        if row["state"] == "stale":
            return {"job_id": job_id, "status": "stale", "mismatched": mismatched}
        if row["state"] != "succeeded":
            return {"job_id": job_id, "status": "not_succeeded", "state": row["state"]}
        return {"job_id": job_id, "status": "valid", "mismatched": []}

    # ----- runner -------------------------------------------------------------------------

    def _acquire_runner_lock(self):
        fd = os.open(os.path.join(self.state_dir, "runner.lock"), os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(fd)
            raise BrokerError("another runner holds the lock")
        self._runner_fd = fd

    def _recover_orphans(self):
        now_w, now_m = time.time(), time.monotonic_ns()
        with self._tx() as c:
            rows = c.execute("SELECT job_id, child_pid, owner_pid FROM jobs WHERE state='running'").fetchall()
            for r in rows:
                _kill_group(r["child_pid"])
                c.execute("UPDATE jobs SET state='failed', reason=?, finished_wall=?, finished_mono_ns=? "
                          "WHERE job_id=? AND state='running'",
                          (f"recovered on runner start: runner pid {r['owner_pid']} died while the job ran",
                           now_w, now_m, r["job_id"]))
        return [r["job_id"] for r in rows]

    def _claim_next(self, runner_pid):
        with self._tx() as c:
            row = c.execute("SELECT job_id FROM jobs WHERE state='queued' ORDER BY job_id LIMIT 1").fetchone()
            if row is None:
                return None
            c.execute("UPDATE jobs SET state='running', owner_pid=?, started_wall=?, started_mono_ns=? "
                      "WHERE job_id=? AND state='queued'",
                      (runner_pid, time.time(), time.monotonic_ns(), row["job_id"]))
        return self.get(row["job_id"])

    def _finish(self, job_id, state, reason, exit_code=None, log_path=None):
        """Record a terminal state only if the job is still running (cancellation wins)."""
        log_sha = sha256_file(log_path) if log_path and os.path.exists(log_path) else None
        now_w, now_m = time.time(), time.monotonic_ns()
        with self._tx() as c:
            cur = c.execute("UPDATE jobs SET state=?, reason=?, exit_code=?, finished_wall=?, finished_mono_ns=?, "
                            "log_path=?, log_sha256=?, child_pid=NULL WHERE job_id=? AND state='running'",
                            (state, reason, exit_code, now_w, now_m, log_path, log_sha, job_id))
            return cur.rowcount == 1

    def _spawn(self, job):
        log_path = os.path.join(self.logs_dir, f"job-{job['job_id']}.log")
        mem = job["memory_bytes"]

        def _limits():
            resource.setrlimit(resource.RLIMIT_AS, (mem, mem))

        logf = open(log_path, "wb")
        try:
            proc = subprocess.Popen(job["argv"], cwd=job["cwd"], env=job["env"], shell=False,
                                    stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT,
                                    close_fds=True, start_new_session=True, preexec_fn=_limits)
        except OSError as exc:
            logf.close()
            self._finish(job["job_id"], "failed", f"spawn failed: {exc}", None, log_path)
            return None
        with self._tx() as c:
            c.execute("UPDATE jobs SET child_pid=?, log_path=? WHERE job_id=?",
                      (proc.pid, log_path, job["job_id"]))
        return {"proc": proc, "log": logf, "log_path": log_path,
                "deadline": time.monotonic_ns() + int(job["timeout_s"] * 1e9)}

    def _poll(self, job_id, ch):
        """Return True when the child is finished and has been recorded."""
        proc = ch["proc"]
        state = self.conn.execute("SELECT state FROM jobs WHERE job_id=?", (job_id,)).fetchone()["state"]
        if state != "running":
            # Cancelled or superseded while running: stop it and keep the cancelled record.
            _kill_group(proc.pid)
            proc.wait()
            ch["log"].close()
            self._set_log_hash(job_id, ch["log_path"])
            return True
        rc = proc.poll()
        if rc is None:
            if time.monotonic_ns() > ch["deadline"]:
                _kill_group(proc.pid)
                proc.wait()
                ch["log"].close()
                timeout_s = self.get(job_id)["timeout_s"]
                self._finish(job_id, "failed", f"timeout after {timeout_s:g} s (killed)", None, ch["log_path"])
                return True
            return False
        _kill_group(proc.pid)  # clean up any descendants left in the job's process group
        ch["log"].close()
        if rc == 0:
            self._finish(job_id, "succeeded", "", 0, ch["log_path"])
        else:
            reason = f"exit {rc}" if rc > 0 else f"killed by signal {-rc}"
            self._finish(job_id, "failed", reason, rc, ch["log_path"])
        return True

    def _set_log_hash(self, job_id, log_path):
        if log_path and os.path.exists(log_path):
            with self._tx() as c:
                c.execute("UPDATE jobs SET log_path=?, log_sha256=? WHERE job_id=?",
                          (log_path, sha256_file(log_path), job_id))

    def run_until_idle(self, max_concurrent=1, poll_s=0.02, max_seconds=None):
        """Run queued jobs with at most max_concurrent children until the queue is empty."""
        if not isinstance(max_concurrent, int) or max_concurrent < 1:
            raise BrokerError("max_concurrent must be a positive integer")
        self._acquire_runner_lock()
        recovered = self._recover_orphans()
        active = {}
        started = time.monotonic()
        peak = 0
        ran = []
        try:
            while True:
                while len(active) < max_concurrent:
                    job = self._claim_next(os.getpid())
                    if job is None:
                        break
                    ran.append(job["job_id"])
                    ch = self._spawn(job)
                    if ch is not None:
                        active[job["job_id"]] = ch
                peak = max(peak, len(active))
                if not active:
                    break
                for jid in list(active):
                    if self._poll(jid, active[jid]):
                        del active[jid]
                if max_seconds is not None and time.monotonic() - started > max_seconds:
                    for jid, ch in list(active.items()):
                        _kill_group(ch["proc"].pid)
                        ch["proc"].wait()
                        ch["log"].close()
                        self._finish(jid, "failed", "runner stopped by max_seconds (killed)", None, ch["log_path"])
                        del active[jid]
                    break
                time.sleep(poll_s)
        finally:
            self._release_runner_lock()
        return {"ran": ran, "recovered": recovered, "peak_concurrency": peak,
                "max_concurrent": max_concurrent, "elapsed_s": round(time.monotonic() - started, 3)}

    def _release_runner_lock(self):
        if self._runner_fd is not None:
            os.close(self._runner_fd)
            self._runner_fd = None

    def gc(self, older_than_days=30.0):
        cutoff = time.time() - older_than_days * 86400
        removed = 0
        with self._tx() as c:
            rows = c.execute("SELECT job_id, log_path FROM jobs WHERE state IN ('succeeded','failed','cancelled','stale') "
                             "AND finished_wall < ?", (cutoff,)).fetchall()
            for r in rows:
                if r["log_path"] and os.path.commonpath([os.path.realpath(r["log_path"]), self.logs_dir]) == self.logs_dir:
                    if os.path.exists(r["log_path"]):
                        os.remove(r["log_path"])
                c.execute("DELETE FROM jobs WHERE job_id=?", (r["job_id"],))
                removed += 1
        return {"removed_jobs": removed, "cutoff_wall": cutoff}


_MISSING = object()


def _parse_env(items):
    env = {}
    for item in items or []:
        if "=" not in item:
            raise BrokerError(f"--env expects KEY=VALUE, got {item!r}")
        k, v = item.split("=", 1)
        env[k] = v
    return env


def _emit(obj, code=0):
    print(json.dumps(obj, indent=2, sort_keys=True))
    return code


def main(argv=None):
    ap = argparse.ArgumentParser(description="Durable, deduplicating local job broker (M1-00).")
    ap.add_argument("--state-dir", default=DEFAULT_STATE_DIR)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("submit", help="enqueue a command job (does not run it)")
    p.add_argument("--system-id", required=True)
    p.add_argument("--inputs", default="{}", help="JSON object of artifact-hash inputs")
    p.add_argument("--cwd", default=None)
    p.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    p.add_argument("--timeout-s", type=float, default=DEFAULT_TIMEOUT_S)
    p.add_argument("--memory-mb", type=int, default=DEFAULT_MEMORY_MB)
    p.add_argument("--resubmit", action="store_true")
    p.add_argument("argv", nargs=argparse.REMAINDER, help="command after --")

    p = sub.add_parser("status", help="show one job")
    p.add_argument("job_id", type=int)

    p = sub.add_parser("cancel", help="cancel a queued or running job")
    p.add_argument("job_id", type=int)

    p = sub.add_parser("list", help="list jobs and queue metrics")
    p.add_argument("--state", choices=STATES)
    p.add_argument("--limit", type=int, default=200)

    p = sub.add_parser("run-once", help="run queued jobs until the queue is empty")
    p.add_argument("--max-concurrent", type=int, default=1)
    p.add_argument("--max-seconds", type=float, default=None)

    p = sub.add_parser("gc", help="delete terminal jobs and logs older than N days")
    p.add_argument("--older-than-days", type=float, default=30.0)

    args = ap.parse_args(argv)
    try:
        broker = Broker(args.state_dir)
    except sqlite3.Error as exc:
        return _emit({"ok": False, "error": f"cannot open state: {exc}"}, 1)
    try:
        if args.cmd == "submit":
            cmd = list(args.argv)
            if cmd[:1] == ["--"]:
                cmd = cmd[1:]
            try:
                inputs = json.loads(args.inputs)
            except json.JSONDecodeError as exc:
                raise BrokerError(f"--inputs is not JSON: {exc}")
            res = broker.submit(args.system_id, inputs, cmd, env=_parse_env(args.env),
                                cwd=args.cwd, timeout_s=args.timeout_s, memory_mb=args.memory_mb,
                                resubmit=args.resubmit)
            return _emit({"ok": True, **res})
        if args.cmd == "status":
            return _emit({"ok": True, "job": broker.get(args.job_id)})
        if args.cmd == "cancel":
            return _emit({"ok": True, **broker.cancel(args.job_id)})
        if args.cmd == "list":
            return _emit({"ok": True, "metrics": broker.metrics(),
                          "jobs": broker.list(args.state, args.limit)})
        if args.cmd == "run-once":
            return _emit({"ok": True, **broker.run_until_idle(args.max_concurrent, max_seconds=args.max_seconds),
                          "metrics": broker.metrics()})
        if args.cmd == "gc":
            return _emit({"ok": True, **broker.gc(args.older_than_days)})
    except NotFound as exc:
        return _emit({"ok": False, "error": str(exc)}, 3)
    except BrokerError as exc:
        return _emit({"ok": False, "error": str(exc)}, 1)
    finally:
        broker.close()
    return _emit({"ok": False, "error": "unknown command"}, 2)


if __name__ == "__main__":
    sys.exit(main())
