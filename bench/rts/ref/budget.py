# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Oracle budget observations for the M3-02 Python reference (contract M3 section 8, N13). Stdlib only.

Each planned point (rows, ticks) moves through a small state machine:

    planned -> running -> measured                   all ticks completed inside the budget
                       -> not_run (reason: budget)   projected run time exceeds --max-seconds
                       -> failed (error recorded)    an exception; never reported as a success

Only observations are recorded: per-tick wall time, percentiles, and the time of one targeting-metrics
pass (1,024 seeded queriers at tick 0). Nothing here is a target. The budget numbers are fixed by the
owner before M3-06 (contract N13), and this file records the measurement and the decision state.

Usage:  python3 -I bench/rts/ref/budget.py [--max-seconds 300] [--out bench/rts/ref/records/oracle_budget.json]
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import platform
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

PLANNED, RUNNING, MEASURED, NOT_RUN, FAILED = "planned", "running", "measured", "not_run", "failed"
MIN_TICKS_BEFORE_PROJECTION = 3
QUERIERS = 1024
QUERIER_SEED = 20261012
DEFAULT_POINTS = ((1024, 120), (4096, 120), (16384, 120))
SOURCE_FILES = ("model.py", "digest.py", "ir_programs.py", "ir_eval.py", "oracle.py", "probe_scene.py",
                "prng.py", "isqrt.py", "budget.py")


class Point:
    def __init__(self, rows: int, ticks: int, seed: int = 1):
        self.rows = rows
        self.ticks_requested = ticks
        self.seed = seed
        self.state = PLANNED
        self.ticks_done = 0
        self.tick_ns = []
        self.reason = None
        self.error = None
        self.build_ns = None
        self.metrics_ns = None
        self.metrics = None

    def as_dict(self) -> dict:
        return {
            "rows": self.rows,
            "ticks_requested": self.ticks_requested,
            "ticks_done": self.ticks_done,
            "seed": self.seed,
            "state": self.state,
            "reason": self.reason,
            "error": self.error,
            "build_ns": self.build_ns,
            "tick_ns": self.tick_ns,
            "tick_ns_p50": _pct(self.tick_ns, 0.50),
            "tick_ns_p95": _pct(self.tick_ns, 0.95),
            "tick_ns_max": max(self.tick_ns) if self.tick_ns else None,
            "tick_ns_total": sum(self.tick_ns) if self.tick_ns else 0,
            "metrics_ns": self.metrics_ns,
            "targeting_metrics_tick0": self.metrics,
        }


def _pct(values, q):
    """Nearest-rank percentile of a list of nanosecond samples (None when empty)."""
    if not values:
        return None
    ordered = sorted(values)
    return ordered[math.ceil(q * len(ordered)) - 1]


def run_point(point: Point, max_seconds: float, clock=time.perf_counter_ns, build=None, tick=None,
              metrics=None) -> Point:
    """Drive one point through the state machine. clock, build, tick and metrics are injectable for tests."""
    import model
    import oracle
    import probe_scene

    build = build or (lambda p: probe_scene.build_probe(p.rows, p.seed))
    tick = tick or (lambda world: model.tick(world))
    metrics = metrics or (lambda world: oracle.targeting_metrics(
        world, oracle.sample_queriers(world, QUERIERS, QUERIER_SEED), "m3-02-probe"))
    point.state = RUNNING
    try:
        t0 = clock()
        world = build(point)
        point.build_ns = clock() - t0

        t0 = clock()
        point.metrics = metrics(world)  # tick-0 state; spatial phase is idempotent on tick-start state
        point.metrics_ns = clock() - t0

        world = build(point)  # fresh tick-0 world for the timed run
        budget_ns = max_seconds * 1e9
        elapsed = 0
        for _ in range(point.ticks_requested):
            t0 = clock()
            tick(world)
            dt = clock() - t0
            point.tick_ns.append(dt)
            point.ticks_done += 1
            elapsed += dt
            if point.ticks_done < point.ticks_requested and point.ticks_done >= MIN_TICKS_BEFORE_PROJECTION:
                projected = elapsed / point.ticks_done * point.ticks_requested
                if projected > budget_ns:
                    point.state = NOT_RUN
                    point.reason = "budget"
                    return point
        point.state = MEASURED
    except Exception as err:  # recorded, never converted into a pass
        point.state = FAILED
        point.error = f"{type(err).__name__}: {err}"
    return point


def hardware_block() -> dict:
    cpu = None
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as fh:
            for line in fh:
                if line.lower().startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    mem_kib = None
    try:
        with open("/proc/meminfo", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("MemTotal:"):
                    mem_kib = int(line.split()[1])
                    break
    except OSError:
        pass
    return {
        "cpu_model": cpu,
        "vcpu_count": os.cpu_count(),
        "mem_total_kib": mem_kib,
        "os": platform.platform(),
        "python": sys.version.split()[0],
        "implementation": platform.python_implementation(),
        "measurement_mode": "dev_checked",
        "note": "CPython, no optimisation claim; shared VM timings are observations (C10)",
    }


def source_hashes() -> dict:
    out = {}
    for name in SOURCE_FILES:
        with open(os.path.join(_HERE, name), "rb") as fh:
            out[name] = "sha256:" + hashlib.sha256(fh.read()).hexdigest()
    return out


def main(argv) -> int:
    import argparse
    parser = argparse.ArgumentParser(description="M3-02 oracle budget observations")
    parser.add_argument("--max-seconds", type=float, default=300.0,
                        help="projected tick-loop budget per point (state not_run if exceeded)")
    parser.add_argument("--out", default=os.path.join(_HERE, "records", "oracle_budget.json"))
    args = parser.parse_args(argv)
    points = [Point(rows, ticks, seed=1) for rows, ticks in DEFAULT_POINTS]
    for p in points:
        run_point(p, args.max_seconds)
        print(f"rows={p.rows} ticks={p.ticks_requested} state={p.state} done={p.ticks_done} "
              f"reason={p.reason} error={p.error}", file=sys.stderr)
    record = {
        "kind": "observation",
        "contract": "planning/contracts/M3_RTS_CONTRACT.md r4 section 8 (N13) and section 10.6",
        "scene_source": "m3-02-probe (not the committed congested.blob.v1)",
        "decision": "pending owner (N13 fixes the budget before M3-06)",
        "max_seconds_per_point": args.max_seconds,
        "hardware": hardware_block(),
        "source_hashes": source_hashes(),
        "points": [p.as_dict() for p in points],
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=2, sort_keys=True)
        fh.write("\n")
    return 0 if all(p.state in (MEASURED, NOT_RUN) for p in points) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
