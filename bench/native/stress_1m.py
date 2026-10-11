#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""1,000,000-row stress of the CURRENT default release binary (M1 audit blocker, evidence only).

Reuses the stress procedure of bench/reload/bench_reload.py (stress_mode) by importing that file
(it is not copied). It drives the real `<binary> serve` over JSON lines and records, for one
1,000,000-row world: world.create time, step latency (20 single-tick steps, each verified against
an independent model), 6 timed edits, snapshot time and bytes, restore time, and the worker and
supervisor VmRSS / VmHWM. Failures are recorded as returned, with the engine's error code and text.

  python3 -I bench/native/stress_1m.py --binary build/sutekh --build-dir build

Build the binary first (default profile, SUTEKH_NATIVE off):
  python3 tools/dev.py build --profile release

Output: planning/evidence/m1/stress-1m-current.json. Nothing is copied from an earlier run.
"""

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RELOAD = os.path.join(REPO, "bench", "reload", "bench_reload.py")
OUT = os.path.join(REPO, "planning", "evidence", "m1", "stress-1m-current.json")


def load_reload():
    spec = importlib.util.spec_from_file_location("bench_reload", RELOAD)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args):
    r = subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True, shell=False, check=False)
    return r.stdout.strip()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=os.path.join(REPO, "build", "sutekh"))
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build"))
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--steps", type=int, default=20)
    ap.add_argument("--edits", type=int, default=6)
    ap.add_argument("--cap-s", type=float, default=900.0)
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args(argv)

    binary = os.path.abspath(args.binary)
    if not os.path.isfile(binary):
        raise SystemExit(f"binary not found: {binary}; build it with: python3 tools/dev.py build --profile release")
    reload_mod = load_reload()
    ns = argparse.Namespace(binary=binary, build_dir=os.path.abspath(args.build_dir), profile_label="release",
                            stress_steps=args.steps, stress_edits=args.edits, stress_cap_s=args.cap_s)
    head = git("rev-parse", "HEAD")
    dirty = git("status", "--porcelain", "--untracked-files=no") != ""
    env_rec = reload_mod.environment(binary, ns.build_dir, "release")
    doc = {
        "report_format": "0.1",
        "kind": "stress_1m_current_binary",
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "git": {"head": head, "dirty_tracked_files": dirty},
        "binary": {"path_relative_to_repo": os.path.relpath(binary, REPO), "sha256": sha256_file(binary),
                   "bytes": os.path.getsize(binary)},
        "configuration_in_source": {
            "worker_address_space": "RLIMIT_AS 4 GiB default (src/platform/sutekh_main.c); SUTEKH_WORKER_AS_BYTES overrides",
            "recovery_spool": "512 MiB default (src/platform/sutekh_main.c); SUTEKH_SPOOL_BYTES_MAX overrides",
            "fixture_cap": "SUTEKH_FIXTURE_MAX_BYTES (src/runtime/worker.c; default 64 MiB)",
            "environment_overrides_present": sorted(k for k in os.environ if k.startswith("SUTEKH_")),
        },
        "environment": env_rec,
        "procedure": "bench/reload/bench_reload.py stress_mode, imported and not modified",
        "rows": args.rows,
        "seed": args.rows,
        "nominal_column_bytes": args.rows * 2 * 4,
        "status": "running",
        "result": None,
        "error": None,
    }
    world = None
    try:
        world = reload_mod.make_world(args.rows, args.rows)
        doc["fixture"] = {k: world[k] for k in ("path", "rows", "seed", "sha256", "bytes")}
        res = reload_mod.stress_mode(ns, world, env_rec)
        doc["result"] = res
        doc["status"] = res.get("status")
    except Exception as exc:  # recorded verbatim; never hidden
        doc["status"] = "exception"
        doc["error"] = {"type": type(exc).__name__, "message": str(exc), "traceback": traceback.format_exc()[-3000:]}
    finally:
        if world is not None and os.path.exists(os.path.join(REPO, world["path"])):
            os.remove(os.path.join(REPO, world["path"]))
    doc["summary"] = summarize(doc)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, sort_keys=True, default=str)
        fh.write("\n")
    print(json.dumps(doc["summary"], indent=1, default=str))
    return 0 if doc["status"] == "passed" else 1


def summarize(doc):
    res = doc.get("result") or {}
    ph = res.get("phases") or {}
    steps = ph.get("step_ms") or []
    steps_sorted = sorted(steps)
    p95 = steps_sorted[max(1, -(-95 * len(steps_sorted) // 100)) - 1] if steps_sorted else None
    edits = ph.get("edits") or []
    snap = ph.get("snapshot") or {}
    rest = ph.get("restore") or {}
    return {
        "status": doc.get("status"),
        "reason": res.get("reason") or (doc.get("error") or {}).get("message"),
        "refused_code": res.get("refused_code"),
        "world_create_ms": ph.get("world_create_ms"),
        "step_ms": {"n": len(steps), "median": sorted(steps)[len(steps) // 2] if steps else None,
                    "p95_nearest_rank": p95, "max": max(steps) if steps else None},
        "timed_edit_samples": len(edits),
        "snapshot": {"host_ms": snap.get("host_ms"), "bytes": snap.get("bytes"),
                     "engine_capture_ns": snap.get("engine_capture_ns")},
        "restore_host_ms": rest.get("host_ms"),
        "worker_rss_after_create": res.get("worker_rss_after_create"),
        "worker_rss_after_steps": res.get("worker_rss_after_steps"),
        "worker_rss_end": res.get("worker_rss_end"),
    }


if __name__ == "__main__":
    sys.exit(main())
