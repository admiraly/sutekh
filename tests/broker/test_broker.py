#!/usr/bin/env python3
"""Tests for tools/broker/broker.py (M1-00). Stdlib unittest only.

Run: python3 -m unittest -v tests/broker/test_broker.py
Set SUTEKH_BROKER_DIR to test a copy of tools/broker (used for mutation checks).
Temporary state lives in a fresh directory created by tempfile (TMPDIR is honoured).
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
BROKER_DIR = os.environ.get("SUTEKH_BROKER_DIR") or os.path.join(REPO_ROOT, "tools", "broker")
BROKER_PY = os.path.join(BROKER_DIR, "broker.py")
sys.path.insert(0, BROKER_DIR)

import broker as B  # noqa: E402

PY = sys.executable
SYS_KEYS = {"semantic_hash": "aa" * 32, "layout_generation": 1, "backend": "asmjit-x86-64-scalar"}


def py_argv(code, *args):
    return [PY, "-I", "-c", code, *args]


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        with open(f"/proc/{pid}/stat") as fh:
            return fh.read().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


def cli(state_dir, *args, timeout=120):
    res = subprocess.run([PY, BROKER_PY, "--state-dir", state_dir, *args], shell=False,
                         capture_output=True, text=True, timeout=timeout, check=False)
    return res.returncode, json.loads(res.stdout)


class BrokerTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="broker-test-")
        self.state = os.path.join(self.tmp, "state")
        self.marker = os.path.join(self.tmp, "marker.txt")
        self.broker = B.Broker(self.state)
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
                p.wait()
        self.broker.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def reopen(self):
        self.broker.close()
        self.broker = B.Broker(self.state)
        return self.broker

    def run_once(self, **kw):
        return self.broker.run_until_idle(poll_s=0.01, **kw)

    def run_once_subprocess(self, *extra):
        return subprocess.Popen([PY, BROKER_PY, "--state-dir", self.state, "run-once", *extra],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, shell=False)

    def wait_for(self, predicate, timeout=30.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate():
                return True
            time.sleep(0.05)
        return False

    def submit(self, argv, system_id="sys-a", inputs=None, **kw):
        return self.broker.submit(system_id, SYS_KEYS if inputs is None else inputs, argv, **kw)

    def append_marker_argv(self, text):
        code = "import sys; open(sys.argv[1], 'a').write(sys.argv[2] + '\\n')"
        return py_argv(code, self.marker, text)


class TestDedupe(BrokerTestCase):
    def test_same_key_returns_existing_job_and_runs_once(self):
        first = self.submit(self.append_marker_argv("run"))
        second = self.submit(self.append_marker_argv("run"))
        self.assertFalse(first["deduplicated"])
        self.assertTrue(second["deduplicated"])
        self.assertEqual(first["job_id"], second["job_id"])
        self.assertEqual(first["job_key"], second["job_key"])
        self.run_once()
        third = self.submit(self.append_marker_argv("run"))
        self.assertEqual(third["job_id"], first["job_id"])
        self.assertEqual(third["state"], "succeeded")
        with open(self.marker) as fh:
            self.assertEqual(fh.read().splitlines(), ["run"])

    def test_failed_job_needs_explicit_resubmit(self):
        argv = py_argv("import sys; sys.exit(3)")
        first = self.submit(argv)
        self.run_once()
        self.assertEqual(self.broker.get(first["job_id"])["state"], "failed")
        again = self.submit(argv)
        self.assertEqual(again["job_id"], first["job_id"])
        self.assertTrue(again["deduplicated"])
        redo = self.submit(argv, resubmit=True)
        self.assertNotEqual(redo["job_id"], first["job_id"])
        self.assertEqual(redo["job_key"], first["job_key"])
        self.assertEqual(self.broker.get(first["job_id"])["exit_code"], 3)

    def test_different_inputs_give_different_keys(self):
        a = self.submit(self.append_marker_argv("x"), inputs={**SYS_KEYS, "semantic_hash": "bb" * 32})
        b = self.submit(self.append_marker_argv("x"))
        self.assertNotEqual(a["job_key"], b["job_key"])


class TestSupersession(BrokerTestCase):
    def test_newer_job_cancels_queued_older_job(self):
        old = self.submit(self.append_marker_argv("old"))
        other = self.submit(self.append_marker_argv("other"), system_id="sys-b")
        new = self.submit(self.append_marker_argv("new"))
        self.assertEqual(new["superseded"], [old["job_id"]])
        old_job = self.broker.get(old["job_id"])
        self.assertEqual(old_job["state"], "cancelled")
        self.assertEqual(old_job["superseded_by"], new["job_id"])
        self.run_once()
        self.assertEqual(self.broker.get(other["job_id"])["state"], "succeeded")
        self.assertEqual(self.broker.get(new["job_id"])["state"], "succeeded")
        with open(self.marker) as fh:
            self.assertEqual(sorted(fh.read().splitlines()), ["new", "other"])

    def test_running_older_job_is_killed_and_its_result_discarded(self):
        slow = self.submit(py_argv("import time; time.sleep(60)"))
        proc = self.run_once_subprocess()
        self.procs.append(proc)
        self.assertTrue(self.wait_for(lambda: self.broker.get(slow["job_id"])["child_pid"] is not None))
        newer = self.submit(self.append_marker_argv("fresh"))
        self.assertEqual(newer["superseded"], [slow["job_id"]])
        out, err = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 0, err)
        slow_job = self.reopen().get(slow["job_id"])
        self.assertEqual(slow_job["state"], "cancelled")
        self.assertIsNone(slow_job["exit_code"])
        self.assertEqual(self.broker.get(newer["job_id"])["state"], "succeeded")


class TestStaleResults(BrokerTestCase):
    def test_result_is_valid_only_while_bound_identity_matches(self):
        res = self.submit(self.append_marker_argv("bind"))
        self.assertEqual(self.broker.validate_result(res["job_id"], SYS_KEYS)["status"], "not_succeeded")
        self.run_once()
        ok = self.broker.validate_result(res["job_id"], dict(SYS_KEYS))
        self.assertEqual(ok["status"], "valid")
        changed = dict(SYS_KEYS, semantic_hash="bb" * 32)
        stale = self.broker.validate_result(res["job_id"], changed)
        self.assertEqual(stale["status"], "stale")
        self.assertEqual(stale["mismatched"], ["semantic_hash"])
        self.assertEqual(self.broker.get(res["job_id"])["state"], "stale")
        # Once stale, it stays stale even when the installed identity is restored.
        self.assertEqual(self.broker.validate_result(res["job_id"], SYS_KEYS)["status"], "stale")

    def test_backend_and_layout_changes_are_stale(self):
        for n, (field, value) in enumerate((("layout_generation", 2), ("backend", "asmjit-x86-64-avx2"))):
            res = self.submit(self.append_marker_argv(field), inputs=dict(SYS_KEYS, n=n))
            self.run_once()
            out = self.broker.validate_result(res["job_id"], dict(SYS_KEYS, n=n, **{field: value}))
            self.assertEqual(out["status"], "stale", field)
            self.assertEqual(out["mismatched"], [field])
            self.assertEqual(self.broker.get(res["job_id"])["state"], "stale")


class TestLimits(BrokerTestCase):
    def test_timeout_kills_the_job(self):
        res = self.submit(py_argv("import time; time.sleep(30)"), timeout_s=1)
        t0 = time.monotonic()
        self.run_once()
        self.assertLess(time.monotonic() - t0, 20)
        job = self.broker.get(res["job_id"])
        self.assertEqual(job["state"], "failed")
        self.assertTrue(job["reason"].startswith("timeout"), job["reason"])
        self.assertFalse(alive(job["child_pid"] or -1))

    def test_memory_limit_fails_large_allocation_and_allows_small(self):
        big = self.submit(py_argv("x = bytearray(1 << 30)"), system_id="mem-big", memory_mb=256)
        small = self.submit(py_argv("x = bytearray(1 << 20); print('small ok')"), system_id="mem-small",
                            memory_mb=256)
        self.run_once()
        self.assertEqual(self.broker.get(big["job_id"])["state"], "failed")
        with open(self.broker.get(big["job_id"])["log_path"]) as fh:
            self.assertIn("MemoryError", fh.read())
        self.assertEqual(self.broker.get(small["job_id"])["state"], "succeeded")

    def test_concurrency_cap_is_respected_and_used(self):
        # Distinct system ids: jobs sharing one system_id would supersede each other.
        ids = [self.submit(py_argv("import time; time.sleep(0.5)"), system_id=f"cap-{i}")["job_id"]
               for i in range(4)]
        summary = self.run_once(max_concurrent=2)
        self.assertEqual(summary["peak_concurrency"], 2)
        jobs = [self.broker.get(i) for i in ids]
        for j in jobs:
            self.assertEqual(j["state"], "succeeded")
        events = []
        for j in jobs:
            events.append((j["started_mono_ns"], 1))
            events.append((j["finished_mono_ns"], -1))
        live = peak = 0
        for _, delta in sorted(events, key=lambda e: (e[0], e[1])):
            live += delta
            peak = max(peak, live)
        self.assertLessEqual(peak, 2)
        self.assertEqual(peak, 2)


class TestSafety(BrokerTestCase):
    def test_argv_is_never_interpreted_by_a_shell(self):
        tokens = ["a;b", "$(touch %s)" % self.marker, "`touch %s`" % self.marker,
                  "x && touch %s" % self.marker, "*", "|cat"]
        code = "import sys; print(repr(sys.argv[1:]))"
        res = self.submit(py_argv(code, *tokens))
        self.run_once()
        job = self.broker.get(res["job_id"])
        self.assertEqual(job["state"], "succeeded")
        with open(job["log_path"]) as fh:
            self.assertEqual(fh.read().strip(), repr(tokens))
        self.assertFalse(os.path.exists(self.marker))

    def test_env_allowlist_cwd_and_argv_are_validated(self):
        argv = py_argv("pass")
        with self.assertRaises(B.BrokerError):
            self.submit(argv, env={"LD_PRELOAD": "/tmp/x.so"})
        with self.assertRaises(B.BrokerError):
            self.submit(argv, cwd="/")
        with self.assertRaises(B.BrokerError):
            self.submit([])
        with self.assertRaises(B.BrokerError):
            self.submit(argv, inputs={"bad": float("nan")})
        with self.assertRaises(B.BrokerError):
            self.submit(argv, system_id="")

    def test_child_environment_contains_only_allowlisted_keys(self):
        code = "import os, json; print(json.dumps(sorted(os.environ)))"
        res = self.submit(py_argv(code), env={"PATH": "/usr/bin:/bin", "LANG": "C"})
        self.run_once()
        with open(self.broker.get(res["job_id"])["log_path"]) as fh:
            keys = json.loads(fh.read())
        # Python may add LC_CTYPE itself (PEP 538 locale coercion); nothing else from the parent.
        self.assertLessEqual(set(keys), {"PATH", "LANG", "LC_CTYPE"})
        self.assertNotIn("HOME", keys)


class TestDurability(BrokerTestCase):
    def test_queue_survives_broker_restart(self):
        res = self.submit(self.append_marker_argv("durable"))
        self.reopen()
        again = self.submit(self.append_marker_argv("durable"))
        self.assertEqual(again["job_id"], res["job_id"])
        self.assertEqual(self.broker.get(res["job_id"])["state"], "queued")
        proc = self.run_once_subprocess()
        self.procs.append(proc)
        _, err = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 0, err)
        self.assertEqual(self.reopen().get(res["job_id"])["state"], "succeeded")

    def test_kill_9_runner_mid_job_then_recovery(self):
        res = self.submit(py_argv("import time; time.sleep(60)"))
        proc = self.run_once_subprocess()
        self.procs.append(proc)
        self.assertTrue(self.wait_for(lambda: self.broker.get(res["job_id"])["child_pid"] is not None))
        child = self.broker.get(res["job_id"])["child_pid"]
        proc.send_signal(signal.SIGKILL)
        proc.communicate(timeout=30)
        self.assertTrue(alive(child), "job child should survive the runner's death until recovery")
        self.assertEqual(self.reopen().get(res["job_id"])["state"], "running")
        code, out = cli(self.state, "run-once")
        self.assertEqual(code, 0)
        self.assertEqual(out["recovered"], [res["job_id"]])
        job = self.broker.get(res["job_id"])
        self.assertEqual(job["state"], "failed")
        self.assertIn("recovered on runner start", job["reason"])
        self.assertFalse(self.wait_for(lambda: alive(child), timeout=5))

    def test_cancel_queued_job(self):
        res = self.submit(self.append_marker_argv("never"))
        self.assertTrue(self.broker.cancel(res["job_id"])["changed"])
        self.run_once()
        self.assertEqual(self.broker.get(res["job_id"])["state"], "cancelled")
        self.assertFalse(os.path.exists(self.marker))

    def test_cli_json_and_exit_codes(self):
        code, out = cli(self.state, "submit", "--system-id", "cli", "--inputs", "{}", "--",
                        PY, "-I", "-c", "pass")
        self.assertEqual((code, out["ok"]), (0, True))
        code, out = cli(self.state, "status", "999")
        self.assertEqual(code, 3)
        code, out = cli(self.state, "submit", "--system-id", "cli", "--inputs", "[]", "--", PY)
        self.assertEqual(code, 1)
        self.assertFalse(out["ok"])
        code, out = cli(self.state, "list")
        self.assertEqual(code, 0)
        self.assertIn("queue_length", out["metrics"])


if __name__ == "__main__":
    unittest.main()
