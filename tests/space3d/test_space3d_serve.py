# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""IR 0.3 + space3d.query through the control plane: a real `sutekh serve` supervisor over JSON-lines RPC.

The racer microcosm driver (bench/space3d/racer_microcosm.c) writes its initial world as a fixture and its
committed state after T/2 and T ticks. This test builds the same world through `world.create` (racer.race,
racer.move, and space3d.query as a host operator, in the driver's schedule order), and checks:
  1. after `world.step` T/2 and T, every lane of every row read back with `world.inspect` equals the driver's
     in-process state (the control plane, the worker process and the in-process schedule agree bit for bit);
  2. `world.snapshot` at T/2, step to T, `world.restore`, step to T again: identical lanes;
  3. `events.read` returns as many records as the driver's event log;
  4. builds with the native tier (engine.info lists world.specialize): in a second session `world.specialize` of
     racer.move with backend auto installs an IR 0.3 kernel (execution_tier native_scalar) at tick 0; stepping to
     T reproduces the T lanes and the VM session's snapshot sha256 at T (straight run, before the restore). Otherwise this part prints "not run".
Exit 0 only when every check ran and passed (the native part is reported, not required, on VM-only builds).

    test_space3d_serve.py --binary <sutekh> --driver <racer_microcosm> --work <dir> [--n 8] [--ticks 720]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
FAILURES = []
CHECKS = [0]


def check(cond, what):
    CHECKS[0] += 1
    if not cond:
        FAILURES.append(what)
        print("FAIL", what)


class Serve:
    def __init__(self, binary, root):
        # The binary keeps one checkpoint and checkpoints every 32 ticks; the T/2 snapshot must stay restorable.
        env = dict(os.environ, SUTEKH_CHECKPOINT_RETAIN="8", SUTEKH_CHECKPOINT_INTERVAL_TICKS="1048576")
        self.proc = subprocess.Popen([binary, "serve", "--root", root], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.seq = 0

    def call(self, method, params):
        self.seq += 1
        line = json.dumps({"protocol": "0.1", "id": "r%d" % self.seq, "method": method, "params": params},
                          separators=(",", ":")).encode("utf-8")
        self.proc.stdin.write(line + b"\n")
        self.proc.stdin.flush()
        out = self.proc.stdout.readline()
        if not out:
            raise RuntimeError("supervisor closed its output (stderr: %s)" % self.proc.stderr.read()[-800:])
        resp = json.loads(out)
        if "error" in resp:
            e = resp["error"]
            raise RuntimeError("%s: %s %s %s" % (method, e.get("code"), e.get("message"), e.get("detail", "")))
        return resp["result"]

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                self.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def run_driver(driver, n, ticks, extra):
    out = subprocess.run([driver, "--root", ROOT, "--n", str(n), "--ticks", str(ticks), "--warmup", "0",
                          "--digest-every", "120"] + extra, capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("driver failed: %s" % out.stderr[-800:])
    return json.loads(out.stdout)


def read_all(s, wid, rows, names):
    """Every lane of every row through world.inspect pages (at most 256 rows per call)."""
    cols = {name: [] for name in names}
    off = 0
    while off < rows:
        r = s.call("world.inspect", {"world_id": wid, "offset": off, "limit": 256})
        got = r["columns"]
        for name in names:
            cols[name].extend(got[name])
        off += r["returned_rows"]
        if r["returned_rows"] == 0:
            break
    return cols, r["tick"]


def compare(cols, want, tag):
    bad = [name for name in want["columns"] if cols.get(name) != want["columns"][name]]
    check(not bad, "%s: %d lanes differ from the in-process run (first: %s)" % (tag, len(bad), bad[:3]))
    return not bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--driver", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--n", type=int, default=8)
    ap.add_argument("--ticks", type=int, default=720)
    args = ap.parse_args()
    half = args.ticks // 2
    shutil.rmtree(args.work, ignore_errors=True)
    proj = os.path.join(args.work, "project")
    os.makedirs(proj)
    fixture = os.path.join(proj, "racer.json")
    final_half = os.path.join(args.work, "final_half.json")
    final_full = os.path.join(args.work, "final_full.json")
    rec_half = run_driver(args.driver, args.n, half, ["--dump-final", final_half])
    rec = run_driver(args.driver, args.n, args.ticks, ["--dump-fixture", fixture, "--dump-final", final_full])
    want_half = json.load(open(final_half, encoding="utf-8"))
    want_full = json.load(open(final_full, encoding="utf-8"))
    names = list(want_full["columns"])
    rows = len(json.load(open(fixture, encoding="utf-8"))["entity_ids"])
    for name in ("race", "move"):
        for ext in ("capsule.json", "lir.json"):
            shutil.copy(os.path.join(ROOT, "capsules", "space3d_racer", "%s.%s" % (name, ext)), proj)
    shutil.copy(os.path.join(ROOT, "profiles", "space3d", "space3d_query.capsule.json"), proj)

    create = {"fixture_path": "racer.json",
              "capsule_paths": ["race.capsule.json", "move.capsule.json",
                                {"capsule_path": "space3d_query.capsule.json", "host_operator": "space3d.query"}],
              "max_struct_per_tick": 1, "max_events_per_tick": 2 * args.n + 2, "event_log_max": 1048576}
    native = "not run (engine.info does not list world.specialize: build without SUTEKH_NATIVE)"
    has_native = False

    # Session 1, VM tier: step, snapshot at T/2, step to T, restore, step to T again.
    s = Serve(args.binary, proj)
    try:
        info = s.call("engine.info", {})
        has_native = "world.specialize" in json.dumps(info)
        created = s.call("world.create", create)
        wid, rev = created["world_id"], created["world_revision"]
        r = s.call("world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": half})
        rev = r["world_revision"]
        cols, tick = read_all(s, wid, rows, names)
        check(tick == half, "tick after the first step is %s, want %d" % (tick, half))
        compare(cols, want_half, "T/2 = %d" % half)
        snap = s.call("world.snapshot", {"world_id": wid, "expected_world_revision": rev})
        r = s.call("world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": args.ticks - half})
        rev = r["world_revision"]
        cols, tick = read_all(s, wid, rows, names)
        check(tick == args.ticks, "tick after the second step is %s, want %d" % (tick, args.ticks))
        compare(cols, want_full, "T = %d" % args.ticks)
        # Snapshot of the straight run (a restore restarts the event log, so later snapshots differ by design).
        sha_vm = s.call("world.snapshot", {"world_id": wid, "expected_world_revision": rev})["sha256"]
        ev = s.call("events.read", {"world_id": wid, "cursor": 0, "max": 256})
        check(len(ev["events"]) == rec["events"],
              "events.read returned %d records, the driver logged %d" % (len(ev["events"]), rec["events"]))
        r = s.call("world.restore", {"world_id": wid, "snapshot_id": snap["snapshot_id"],
                                     "expected_world_revision": rev})
        rev = r["world_revision"]
        cols, tick = read_all(s, wid, rows, names)
        check(tick == half, "tick after restore is %s, want %d" % (tick, half))
        compare(cols, want_half, "restored T/2")
        r = s.call("world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": args.ticks - half})
        rev = r["world_revision"]
        cols, _ = read_all(s, wid, rows, names)
        compare(cols, want_full, "restored then stepped to T")
        s.call("world.close", {"world_id": wid, "expected_world_revision": rev})
    finally:
        s.close()

    # Session 2, native builds: racer.move specialized at tick 0, stepped to T.
    if has_native:
        s = Serve(args.binary, proj)
        try:
            created = s.call("world.create", create)
            wid, rev = created["world_id"], created["world_revision"]
            sp = s.call("world.specialize", {"world_id": wid, "expected_world_revision": rev,
                                             "system_id": "racer.move", "backend": "auto"})
            check(sp.get("execution_tier") == "native_scalar",
                  "racer.move execution tier after world.specialize: %s" % sp.get("execution_tier"))
            r = s.call("world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": args.ticks})
            rev = r["world_revision"]
            cols, _ = read_all(s, wid, rows, names)
            ok = compare(cols, want_full, "native racer.move, stepped to T")
            sha_nat = s.call("world.snapshot", {"world_id": wid, "expected_world_revision": rev})["sha256"]
            check(sha_nat == sha_vm, "snapshot sha256 native %s vs VM %s" % (sha_nat, sha_vm))
            native = "ran (backend %s, tier %s, admission %s, fallback %r): %s" % (
                sp.get("backend"), sp.get("execution_tier"), sp.get("source", {}).get("admission"),
                sp.get("fallback"), "ok" if ok and sha_nat == sha_vm else "FAILED")
            s.call("world.close", {"world_id": wid, "expected_world_revision": rev})
        finally:
            s.close()
    print("space3d serve: N=%d, %d rows, %d lanes, T=%d (snapshot at %d), %d events; native: %s"
          % (args.n, rows, len(names), args.ticks, half, rec["events"], native))
    print("space3d serve: %d checks, %d failures" % (CHECKS[0], len(FAILURES)))
    return 1 if FAILURES or CHECKS[0] == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
