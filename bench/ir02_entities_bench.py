#!/usr/bin/env python3
"""IR 0.2 entity tick benchmark (M2-00 audit, performance gap).

Creates a 1,000,000-row entity world (fixture: tests/fixtures/entities/despawn_move, one IR 0.2 system:
despawn_self and emit_event per row) through the real binary (`<binary> serve`, JSON lines on stdio), with the
per-tick caps raised to 1,048,576 (the default 4,096 cannot run a tick where ~370,000 rows despawn; see
docs/AGENT_RPC.md "Default caps and 1,000,000-row worlds"). For each worker count N and each run: create the
world, run STEPS single-tick world.step calls, time each call on the client, and report the median ticks/s.

Usage (from anywhere; the repository root is found from this file):
    python3 bench/ir02_entities_bench.py --binary <build>/sutekh --workers 1,4 --runs 5 --steps 5 \
        --out <build>/bench_ir02.json

The 1,000,000-row fixture (about 20 MB) is written under <repo>/build/bench_ir02 and removed afterwards unless
--keep is given. Timings depend on the host; the output records the host, the binary hash and every sample.
Standard library only.
"""
import argparse
import collections
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
import threading
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CAP_REL = "tests/fixtures/entities/despawn_move.capsule.json"
ROWS = 1000000
TICK_CAP = 1048576
# Fixture rows: the same deterministic generator as tests/harness/run_m0.py gen_columns (seed 8).
MASK = (1 << 64) - 1
U32 = 0xFFFFFFFF


def gen_columns(rows, seed):
    pos = [0] * rows
    vel = [0] * rows
    x = seed & MASK
    for i in range(rows):
        x = (x * 6364136223846793005 + 1442695040888963407) & MASK
        p = (x >> 32) & U32
        x = (x * 6364136223846793005 + 1442695040888963407) & MASK
        v = (x >> 32) & U32
        if i % 11 == 3:
            p = U32
        if i % 5 == 0:
            v = p
        if i % 13 == 4:
            v = 0
        pos[i] = p
        vel[i] = v
    return pos, vel


class Client:
    def __init__(self, binary, env):
        self.proc = subprocess.Popen([binary, "serve"], cwd=REPO, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.seq = 0
        # Drain stderr continuously; an unread pipe would block the server once its buffer fills.
        self._err = collections.deque(maxlen=60)
        threading.Thread(target=self._drain_err, daemon=True).start()

    def _drain_err(self):
        for raw in iter(self.proc.stderr.readline, b""):
            self._err.append(raw.decode("utf-8", "replace"))

    def stderr_tail(self):
        return "".join(self._err)[-800:]

    def request(self, method, params):
        self.seq += 1
        line = json.dumps({"protocol": "0.1", "id": "b%d" % self.seq, "method": method, "params": params},
                          separators=(",", ":")) + "\n"
        self.proc.stdin.write(line.encode("utf-8"))
        self.proc.stdin.flush()
        raw = self.proc.stdout.readline()
        if not raw:
            raise RuntimeError("serve closed stdout: " + self.stderr_tail())
        return json.loads(raw.decode("utf-8"))

    def ok(self, method, params):
        env = self.request(method, params)
        if env.get("ok") is not True:
            raise RuntimeError("%s failed: %s" % (method, json.dumps(env.get("error"))))
        return env["result"]

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.wait(timeout=60)


def run_once(binary, fixture_rel, workers, steps):
    env = dict(os.environ)
    env["SUTEKH_TICK_WORKERS"] = str(workers)
    client = Client(binary, env)
    try:
        t0 = time.perf_counter()
        res = client.ok("world.create", {"fixture_path": fixture_rel, "capsule_paths": [CAP_REL],
                                         "max_struct_per_tick": TICK_CAP, "max_events_per_tick": TICK_CAP})
        create_s = time.perf_counter() - t0
        wid, rev = res["world_id"], res["world_revision"]
        samples = []
        for _ in range(steps):
            t0 = time.perf_counter()
            res = client.ok("world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": 1})
            samples.append(time.perf_counter() - t0)
            rev = res["world_revision"]
        client.ok("world.close", {"world_id": wid, "expected_world_revision": rev})
        return {"create_s": create_s, "step_s": samples, "ticks_per_s": [1.0 / s for s in samples]}
    finally:
        client.close()


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--workers", default="1,4", help="comma-separated tick worker counts")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--steps", type=int, default=5, help="single-tick steps timed per run")
    ap.add_argument("--rows", type=int, default=ROWS)
    ap.add_argument("--out", default="")
    ap.add_argument("--keep", action="store_true", help="keep the generated fixture")
    args = ap.parse_args(argv)

    workers = [int(x) for x in args.workers.split(",") if x]
    scratch = os.path.join(REPO, "build", "bench_ir02")
    os.makedirs(scratch, exist_ok=True)
    fixture = os.path.join(scratch, "rows%d.json" % args.rows)
    pos, vel = gen_columns(args.rows, 8)
    doc = {"fixture_format": "0.1", "schema_id": "demo.actor", "schema_version": 1, "tick": 0,
           "entity_ids": list(range(1, args.rows + 1)), "columns": {"position_x": pos, "velocity_x": vel}}
    with open(fixture, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, separators=(",", ":"))
    del pos, vel, doc
    fixture_rel = os.path.relpath(fixture, REPO)
    with open(args.binary, "rb") as fh:
        binary_sha = hashlib.sha256(fh.read()).hexdigest()

    report = {"benchmark": "ir02_entities_movement_despawn", "rows": args.rows, "steps_per_run": args.steps,
              "runs": args.runs, "tick_caps": TICK_CAP, "binary": args.binary, "binary_sha256": binary_sha,
              "host": {"machine": platform.machine(), "system": platform.system(),
                       "release": platform.release(), "cpus": os.cpu_count()},
              "results": {}}
    try:
        for n in workers:
            runs = []
            for r in range(args.runs):
                one = run_once(args.binary, fixture_rel, n, args.steps)
                med = statistics.median(one["ticks_per_s"])
                runs.append({"run": r + 1, "create_s": round(one["create_s"], 3),
                             "step_s": [round(s, 4) for s in one["step_s"]],
                             "median_ticks_per_s": round(med, 3)})
                print("N=%d run %d: median %.3f ticks/s (create %.2f s)" % (n, r + 1, med, one["create_s"]),
                      file=sys.stderr)
            meds = [x["median_ticks_per_s"] for x in runs]
            report["results"][str(n)] = {"runs": runs, "median": round(statistics.median(meds), 3),
                                         "min": min(meds), "max": max(meds)}
            print("N=%d: median of runs %.3f ticks/s [min %.3f, max %.3f]" % (
                n, statistics.median(meds), min(meds), max(meds)), file=sys.stderr)
    finally:
        if not args.keep:
            os.remove(fixture)
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
