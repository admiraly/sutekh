#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M0-13 latency bench: live edit (hot reload) and native build latency (stdlib only).

Live edit mode drives the REAL `<binary> serve` over JSON lines (docs/AGENT_RPC.md)
and times each control call with the host monotonic clock. Native mode times a
clean and an incremental native build with cmake directly (same arguments that
tools/dev.py uses) in a scratch build directory outside the repository.

  python3 -I bench/reload/bench_reload.py live   --binary build/sutekh --profile-label dev
  python3 -I bench/reload/bench_reload.py native --scratch <dir outside the repo>

Outputs (planning/evidence/):
  m0-latency-<label>.json       summary, shaped like schemas/evidence.schema.json
  m0-latency-<label>-raw.json   environment, configuration, every raw sample, statistics
  m0-native-build.json          native build timings (separate from live edit latency)

Every value is computed from a run. Each live edit is verified against an independent
Python model of the movement system (v1: p += v; v2: p += 2v, both mod 2^32) outside
the timed window. A verification mismatch marks the run as failed. Nothing is copied
from a previous run, and no target is assumed to be met.

Timing definitions
  * Each control call is timed from the write of its request line to the read of its
    reply line (host monotonic clock, one process, includes JSON encode/decode).
  * warm edit = capsule.submit + candidate.evaluate + candidate.install + world.step,
    measured end to end with the host clock. The tick-boundary wait cannot be separated
    from the install RPC: install is synchronous in M0, so it is reported as unavailable.
  * percentiles use the nearest-rank definition on the sorted samples.
"""

import argparse
import collections
import hashlib
import json
import math
import os
import platform
import random
import re
import select
import shutil
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
EVIDENCE = os.path.join(REPO, "planning", "evidence")
FIXTURE_REL = "build/bench-reload"  # under /build/ (gitignored); engine paths must lie under the repo root
PROTOCOL = "0.1"
MASK = 0xFFFFFFFF
PAGE = 256
NOW = time.monotonic_ns

VERSIONS = {
    "v1": {"capsule": "examples/movement.capsule.json", "test": "examples/movement.test.json",
           "source": "examples/movement.lir.json", "mult": 1},
    "v2": {"capsule": "tests/fixtures/edit/movement_v2.capsule.json",
           "test": "tests/fixtures/edit/movement_v2.test.json",
           "source": "tests/fixtures/edit/movement_v2.lir.json", "mult": 2},
}
WRONG_TEST = "tests/fixtures/edit/movement_v2_wrong.test.json"  # one value off by +1 on purpose
CAPSULE_SLOTS = 16  # src/agent/su_agent.c AGENT_MAX_CANDIDATES; the oldest record is evicted when full
SEED_WORLD_1024 = 1024
SEED_WORLD_STRESS = 1000000


class BenchError(Exception):
    pass


def now_ms_from(t0_ns):
    return (NOW() - t0_ns) / 1e6


def reject_constant(name):
    raise ValueError("non-finite JSON constant " + name)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def git(*args):
    r = subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True, shell=False, check=False)
    return r.returncode, r.stdout.strip()


def first_line(argv):
    """First line of a tool's version output, or None when the tool cannot be run."""
    try:
        r = subprocess.run(argv, capture_output=True, text=True, shell=False, check=False, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    out = (r.stdout or r.stderr).strip().splitlines()
    return out[0] if out else None


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return None


def rss_kib(pid):
    """VmRSS and VmHWM of a process in KiB, or None with a reason (Linux /proc only)."""
    text = read_text(f"/proc/{pid}/status")
    if text is None:
        return {"VmRSS_KiB": None, "VmHWM_KiB": None, "reason": "no /proc/<pid>/status (not Linux or process gone)"}
    out = {"VmRSS_KiB": None, "VmHWM_KiB": None, "reason": None}
    for line in text.splitlines():
        if line.startswith("VmRSS:"):
            out["VmRSS_KiB"] = int(line.split()[1])
        elif line.startswith("VmHWM:"):
            out["VmHWM_KiB"] = int(line.split()[1])
    return out


def stats(values):
    if not values:
        return {"n": 0, "min": None, "median": None, "mean": None, "p95": None, "p99": None, "max": None,
                "percentile_method": "nearest-rank"}
    s = sorted(values)
    n = len(s)

    def pct(p):
        return s[max(1, math.ceil(p / 100.0 * n)) - 1]

    return {"n": n, "min": s[0], "median": statistics.median(s), "mean": statistics.fmean(s),
            "p95": pct(95), "p99": pct(99), "max": s[-1], "percentile_method": "nearest-rank"}


# ---------------------------------------------------------------- serve process


class Serve:
    """One `<binary> serve` process. Requests are JSON lines; replies are read with select()."""

    def __init__(self, binary):
        self.argv = [binary, "serve"]
        self.proc = subprocess.Popen(self.argv, cwd=REPO, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, shell=False)
        self._buf = b""
        self._err = collections.deque(maxlen=60)
        self._seq = 0
        self._closed = False
        threading.Thread(target=self._drain_err, daemon=True).start()

    def _drain_err(self):
        for raw in iter(self.proc.stderr.readline, b""):
            self._err.append(raw.decode("utf-8", "replace"))

    def stderr_tail(self):
        return "".join(self._err)[-800:]

    def _readline(self, timeout):
        fd = self.proc.stdout.fileno()
        deadline = time.monotonic() + timeout
        while b"\n" not in self._buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BenchError(f"no reply within {timeout}s; stderr tail: {self.stderr_tail()!r}")
            ready, _, _ = select.select([fd], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(fd, 65536)
            if not chunk:
                raise BenchError(f"serve closed stdout; stderr tail: {self.stderr_tail()!r}")
            self._buf += chunk
            if len(self._buf) > 4 * 1024 * 1024 and b"\n" not in self._buf:
                raise BenchError("reply exceeds 4 MiB without a newline")
        line, _, rest = self._buf.partition(b"\n")
        self._buf = rest
        return line

    def call(self, method, params, timeout=600.0):
        """Return (envelope, host_ms). host_ms spans the request write to the reply read."""
        self._seq += 1
        rid = f"b{self._seq}"
        body = json.dumps({"protocol": PROTOCOL, "id": rid, "method": method, "params": params},
                          separators=(",", ":")).encode("utf-8") + b"\n"
        t0 = NOW()
        self.proc.stdin.write(body)
        self.proc.stdin.flush()
        line = self._readline(timeout)
        ms = now_ms_from(t0)
        try:
            env = json.loads(line.decode("utf-8"), parse_constant=reject_constant)
        except ValueError as exc:
            raise BenchError(f"{method}: reply is not strict JSON: {exc}") from None
        if env.get("id") != rid or env.get("protocol") != PROTOCOL:
            raise BenchError(f"{method}: reply id/protocol mismatch: {env.get('id')!r}")
        return env, ms

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            rc = self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = self.proc.wait()
        self.returncode = rc


def ok(env, method):
    if env.get("ok") is not True:
        err = env.get("error") or {}
        raise BenchError(f"{method} refused: {err.get('code')}: {err.get('message')}")
    return env["result"]


# ---------------------------------------------------------------- fixtures and model


def gen_columns(n, seed):
    rng = random.Random(seed)
    edges = (0, 1, 2, 7, 65536, 0x7FFFFFFF, 0x80000000, MASK - 1, MASK)

    def val():
        return rng.choice(edges) if rng.random() < 0.0625 else rng.getrandbits(32)

    p = [val() for _ in range(n)]
    v = [val() for _ in range(n)]
    return p, v


def write_world(path_abs, n, p, v):
    doc = {"fixture_format": "0.1", "schema_id": "demo.actor", "schema_version": 1, "tick": 0,
           "entity_ids": list(range(1, n + 1)), "columns": {"position_x": p, "velocity_x": v}}
    with open(path_abs, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, separators=(",", ":"))
    return sha256_file(path_abs), os.path.getsize(path_abs)


def make_world(n, seed):
    rel = f"{FIXTURE_REL}/world_{n}_seed{seed}.json"
    p, v = gen_columns(n, seed)
    os.makedirs(os.path.join(REPO, FIXTURE_REL), exist_ok=True)
    digest, size = write_world(os.path.join(REPO, rel), n, p, v)
    return {"path": rel, "rows": n, "seed": seed, "sha256": digest, "bytes": size, "p": p, "v": v}


def segments_for(world, offsets=None):
    """Model segments. None covers all rows; otherwise (offset, count) windows of the world."""
    if offsets is None:
        return [{"offset": 0, "p": list(world["p"]), "v": list(world["v"])}]
    return [{"offset": o, "p": world["p"][o:o + c], "v": world["v"][o:o + c]} for o, c in offsets]


def model_tick(segments, mult):
    for seg in segments:
        seg["p"] = [(a + mult * b) & MASK for a, b in zip(seg["p"], seg["v"])]


def read_rows(serve, wid, offset, count):
    """Read `count` rows at `offset` through world.inspect paging (limit 256)."""
    got_p, got_v, tick = [], [], None
    pos = offset
    end = offset + count
    while pos < end:
        limit = min(PAGE, end - pos)
        env, _ = serve.call("world.inspect", {"world_id": wid, "fields": ["position_x", "velocity_x"],
                                              "limit": limit, "offset": pos})
        res = ok(env, "world.inspect")
        n = res.get("returned_rows")
        if not isinstance(n, int) or n <= 0 or n > limit:
            raise BenchError(f"world.inspect returned_rows={n!r} for limit {limit}")
        got_p.extend(res["columns"]["position_x"])
        got_v.extend(res["columns"]["velocity_x"])
        tick = res.get("tick")
        pos += n
    return got_p, got_v, tick


def verify(serve, wid, segments, expect_tick, label):
    for seg in segments:
        got_p, got_v, tick = read_rows(serve, wid, seg["offset"], len(seg["p"]))
        if tick != expect_tick:
            raise BenchError(f"{label}: tick {tick} != expected {expect_tick}")
        if got_p != seg["p"] or got_v != seg["v"]:
            i = next((k for k, (a, b) in enumerate(zip(got_p, seg["p"])) if a != b), -1)
            raise BenchError(f"{label}: state mismatch at offset {seg['offset']} row {i}")


def create_world(serve, world_rel):
    env, ms = serve.call("world.create", {"fixture_path": world_rel, "capsule_path": VERSIONS["v1"]["capsule"]})
    res = ok(env, "world.create")
    return {"wid": res["world_id"], "rev": res["world_revision"], "tick": res["tick"],
            "worker_pid": res["worker_pid"], "row_count": res["row_count"]}, ms


# ---------------------------------------------------------------- one live edit


def timed_edit(serve, w, vname, segments, label):
    """capsule.submit -> candidate.evaluate -> candidate.install -> world.step(1), host-timed.

    Verification is done after the timed window and does not contribute to the samples.
    """
    spec = VERSIONS[vname]
    t0 = NOW()
    env, ms_submit = serve.call("capsule.submit", {"capsule_path": spec["capsule"]})
    sub = ok(env, "capsule.submit")
    cid = sub["candidate_id"]
    env, ms_eval = serve.call("candidate.evaluate", {"candidate_id": cid, "test_path": spec["test"]})
    ev = ok(env, "candidate.evaluate")
    if ev.get("status") != "passed":
        raise BenchError(f"{label}: candidate {cid} evaluation {ev.get('status')}: {ev.get('failures')}")
    env, ms_install = serve.call("candidate.install", {
        "world_id": w["wid"], "candidate_id": cid, "expected_world_revision": w["rev"],
        "allowed_effective_tick": w["tick"]})
    inst = ok(env, "candidate.install")
    if inst.get("state") != "installed":
        raise BenchError(f"{label}: install state {inst.get('state')}")
    env, ms_step = serve.call("world.step", {"world_id": w["wid"], "expected_world_revision": inst["world_revision"],
                                             "ticks": 1})
    st = ok(env, "world.step")
    total = now_ms_from(t0)
    if st.get("tick") != w["tick"] + 1:
        raise BenchError(f"{label}: step tick {st.get('tick')} != {w['tick'] + 1}")
    w["rev"] = st["world_revision"]
    w["tick"] = st["tick"]
    model_tick(segments, spec["mult"])
    verify(serve, w["wid"], segments, w["tick"], label)
    return {
        "version": vname, "candidate_id": cid, "txid": inst.get("txid"),
        "source_bytes": os.path.getsize(os.path.join(REPO, spec["source"])),
        "submit_ms": ms_submit, "evaluate_ms": ms_eval, "install_rpc_ms": ms_install,
        "tick_boundary_wait_ms": None, "step_first_effect_ms": ms_step, "total_edit_to_first_effect_ms": total,
        "effective_tick": inst.get("effective_tick"), "evaluation_cases": ev.get("cases_total"),
        "tick_after": w["tick"], "verified": True,
    }


def failed_candidate_trial(serve, w, segments, label):
    """A candidate whose evaluation fails must never be published. Returns a record."""
    env, _ = serve.call("capsule.submit", {"capsule_path": VERSIONS["v2"]["capsule"]})
    cid = ok(env, "capsule.submit")["candidate_id"]
    env, _ = serve.call("candidate.evaluate", {"candidate_id": cid, "test_path": WRONG_TEST})
    ev = ok(env, "candidate.evaluate")
    if ev.get("status") != "failed":
        raise BenchError(f"{label}: the wrong-test candidate evaluated as {ev.get('status')!r}, expected failed")
    rev_before = w["rev"]
    env, _ = serve.call("candidate.install", {"world_id": w["wid"], "candidate_id": cid,
                                              "expected_world_revision": rev_before,
                                              "allowed_effective_tick": w["tick"]})
    published = env.get("ok") is True
    code = None if published else (env.get("error") or {}).get("code")
    if published:
        raise BenchError(f"{label}: a failed candidate was installed (publication count not zero)")
    # The next step must accept the pre-rejection revision. A revision bumped by a publication
    # would be refused as SU_E_STALE_REVISION, so this step proves the revision is unchanged.
    env, _ = serve.call("world.step", {"world_id": w["wid"], "expected_world_revision": rev_before, "ticks": 1})
    st = ok(env, "world.step")
    w["rev"] = st["world_revision"]
    w["tick"] = st["tick"]
    # the active code is still the one installed before the failed candidate
    active = w["active"]
    model_tick(segments, VERSIONS[active]["mult"])
    verify(serve, w["wid"], segments, w["tick"], label)
    return {"evaluation_status": ev.get("status"), "install_refused_code": code, "published": published,
            "revision_unchanged_by_rejection": True,
            "revision_check": "the following world.step accepted the pre-rejection revision",
            "active_after": active, "verified_after_rejection": True}


# ---------------------------------------------------------------- live modes


def warm_mode(args, world, env_rec):
    """Warm epochs. Default: a fresh serve process per epoch (each epoch's world lives in its own
    worker). --share-serve reuses one serve process; that fails once the worker holds its maximum
    number of worlds (SU_E_RESOURCE_LIMIT), because no RPC closes a world (M0-13 evidence)."""
    samples, failed, epochs = [], [], []
    per_epoch_slots = 1 + args.per_epoch + 2  # warm-up + timed + failed trials
    serve, info, info_ms, serve_index = None, None, None, -1
    try:
        for ep in range(args.epochs):
            if serve is None or not args.share_serve:
                if serve is not None:
                    serve.close()
                serve = Serve(args.binary)
                serve_index += 1
                env, info_ms = serve.call("engine.info", {})
                info = ok(env, "engine.info")
            w, create_ms = create_world(serve, world["path"])
            w["active"] = "v1"
            segments = segments_for(world)
            verify(serve, w["wid"], segments, 0, f"epoch {ep} initial state")
            rss_before = rss_kib(w["worker_pid"])
            # warm-up: untimed, verified; j=0 installs v2 (world starts on v1), then alternate
            warm = timed_edit(serve, w, "v2", segments, f"epoch {ep} warm-up")
            w["active"] = "v2"
            for j in range(1, args.per_epoch + 1):
                vname = "v1" if j % 2 == 1 else "v2"
                rec = timed_edit(serve, w, vname, segments, f"epoch {ep} edit {j}")
                w["active"] = vname
                rec.update({"epoch": ep, "index_in_epoch": j})
                samples.append(rec)
            for k in range(args.failed_per_epoch):
                fr = failed_candidate_trial(serve, w, segments, f"epoch {ep} failed {k}")
                fr.update({"epoch": ep, "index_in_epoch": k})
                failed.append(fr)
            rss_after = rss_kib(w["worker_pid"])
            epochs.append({"epoch": ep, "engine_info_ms": info_ms, "world_create_ms": create_ms,
                           "worker_pid": w["worker_pid"], "warm_up_total_ms": warm["total_edit_to_first_effect_ms"],
                           "worker_rss_before": rss_before, "worker_rss_after": rss_after,
                           "supervisor_pid": info.get("process", {}).get("pid"), "submissions": per_epoch_slots,
                           "serve_process_index": serve_index})
            print(f"  warm epoch {ep + 1}/{args.epochs} done ({len(samples)} timed edits)", flush=True)
    finally:
        if serve is not None:
            serve.close()
    return samples, failed, epochs


def cold_mode(args, world):
    samples = []
    for i in range(args.cold_trials):
        serve = Serve(args.binary)
        try:
            env, first_ms = serve.call("engine.info", {})  # first request in a new process
            ok(env, "engine.info")
            w, create_ms = create_world(serve, world["path"])  # worker spawn happens here
            w["active"] = "v1"
            segments = segments_for(world)
            verify(serve, w["wid"], segments, 0, f"cold {i} initial state")
            rec = timed_edit(serve, w, "v2", segments, f"cold {i} first edit")  # first edit in the process
            rec.update({"trial": i, "process_first_request_ms": first_ms, "world_create_ms": create_ms,
                        "worker_pid": w["worker_pid"]})
            samples.append(rec)
        finally:
            serve.close()
    return samples


def summarize_edits(samples):
    keys = ["submit_ms", "evaluate_ms", "install_rpc_ms", "step_first_effect_ms", "total_edit_to_first_effect_ms"]
    out = {k: stats([s[k] for s in samples]) for k in keys}
    out["total_edit_to_first_effect_ms"]["per_version"] = {
        vn: stats([s["total_edit_to_first_effect_ms"] for s in samples if s["version"] == vn]) for vn in VERSIONS}
    return out


def stress_mode(args, world_big, env_rec):
    """1,000,000-row diagnostic: state memory, step time, snapshot capture and restore."""
    n = world_big["rows"]
    deadline = time.monotonic() + args.stress_cap_s
    res = {"rows": n, "seed": world_big["seed"], "fixture": {"path": world_big["path"],
                                                               "sha256": world_big["sha256"],
                                                               "bytes": world_big["bytes"]},
           "status": "running", "phases": {}, "reason": None}
    windows = [(0, PAGE), (n - PAGE, PAGE)]
    serve = Serve(args.binary)
    try:
        ok(serve.call("engine.info", {})[0], "engine.info")
        env, create_ms = serve.call("world.create", {"fixture_path": world_big["path"],
                                                     "capsule_path": VERSIONS["v1"]["capsule"]}, timeout=900)
        if env.get("ok") is not True:
            err = env.get("error") or {}
            res["status"] = "refused"
            res["refused_code"] = err.get("code")
            res["reason"] = (f"engine refused world.create of {n} rows: {err.get('code')}: {err.get('message')} "
                             f"(fixture {world_big['bytes']} bytes; see SUTEKH_FIXTURE_MAX_BYTES in src/runtime/worker.c)")
            return res
        rc = ok(env, "world.create")
        w = {"wid": rc["world_id"], "rev": rc["world_revision"], "tick": rc["tick"], "worker_pid": rc["worker_pid"],
             "active": "v1"}
        res["phases"]["world_create_ms"] = create_ms
        res["worker_rss_after_create"] = rss_kib(w["worker_pid"])
        res["supervisor_rss_after_create"] = rss_kib(serve.proc.pid)
        segments = segments_for(world_big, windows)
        verify(serve, w["wid"], segments, 0, "stress initial state")

        steps, edits = [], []
        for k in range(args.stress_steps):
            if time.monotonic() > deadline:
                raise StressCap("step loop")
            env, ms = serve.call("world.step", {"world_id": w["wid"], "expected_world_revision": w["rev"],
                                                "ticks": 1}, timeout=900)
            st = ok(env, "world.step")
            w["rev"], w["tick"] = st["world_revision"], st["tick"]
            model_tick(segments, VERSIONS[w["active"]]["mult"])
            verify(serve, w["wid"], segments, w["tick"], f"stress step {k}")
            steps.append(ms)
        res["phases"]["step_ms"] = steps
        res["worker_rss_after_steps"] = rss_kib(w["worker_pid"])

        for k in range(args.stress_edits):
            if time.monotonic() > deadline:
                raise StressCap("edit loop")
            vname = "v2" if w["active"] == "v1" else "v1"
            rec = timed_edit(serve, w, vname, segments, f"stress edit {k}")
            w["active"] = vname
            edits.append(rec)
        res["phases"]["edits"] = edits

        if time.monotonic() > deadline:
            raise StressCap("snapshot")
        saved_segments = json.loads(json.dumps(segments))
        env, snap_ms = serve.call("world.snapshot", {"world_id": w["wid"], "expected_world_revision": w["rev"]},
                                  timeout=900)
        snap = ok(env, "world.snapshot")
        res["phases"]["snapshot"] = {"host_ms": snap_ms, "bytes": snap.get("bytes"), "engine_capture_ns": snap.get(
            "capture_ns"), "sha256": snap.get("sha256"), "tick": snap.get("tick")}
        w["rev"] = snap["world_revision"]
        for k in range(3):
            env, ms = serve.call("world.step", {"world_id": w["wid"], "expected_world_revision": w["rev"],
                                                "ticks": 1}, timeout=900)
            st = ok(env, "world.step")
            w["rev"], w["tick"] = st["world_revision"], st["tick"]
            model_tick(segments, VERSIONS[w["active"]]["mult"])
            verify(serve, w["wid"], segments, w["tick"], f"stress post-snapshot step {k}")
        env, rs_ms = serve.call("world.restore", {"world_id": w["wid"], "snapshot_id": snap["snapshot_id"],
                                                  "expected_world_revision": w["rev"]}, timeout=900)
        rr = ok(env, "world.restore")
        w["rev"], w["tick"] = rr["world_revision"], rr["tick"]
        verify(serve, w["wid"], saved_segments, snap["tick"], "stress restore")
        res["phases"]["restore"] = {"host_ms": rs_ms, "restored_tick": rr.get("tick")}
        res["worker_rss_end"] = rss_kib(w["worker_pid"])
        res["status"] = "passed"
    except StressCap as exc:
        res["status"] = "cap_exceeded"
        res["reason"] = f"stress wall-clock cap {args.stress_cap_s}s reached during {exc}"
    except BenchError as exc:
        res["status"] = "failed"
        res["reason"] = str(exc)
    finally:
        serve.close()
    res["nominal_column_bytes"] = n * 2 * 4
    return res


class StressCap(Exception):
    pass


# ---------------------------------------------------------------- environment


def cpu_info():
    text = read_text("/proc/cpuinfo")
    if text is None:
        return {"model": None, "logical_processors": os.cpu_count(), "physical_cores": None,
                "hypervisor_flag": None, "reason": "no /proc/cpuinfo"}
    model, logical, cores, hyper, cur = None, 0, set(), False, {}
    for line in text.splitlines() + [""]:
        if not line.strip():
            if "core id" in cur and "physical id" in cur:
                cores.add((cur["physical id"], cur["core id"]))
            cur = {}
            continue
        key, _, val = line.partition(":")
        key, val = key.strip(), val.strip()
        if key == "processor":
            logical += 1
        if key == "model name" and model is None:
            model = val
        if key in ("physical id", "core id"):
            cur[key] = val
        if key == "flags" and "hypervisor" in val.split():
            hyper = True
    return {"model": model, "logical_processors": logical,
            "physical_cores": len(cores) if cores else None, "hypervisor_flag": hyper}


def container_info():
    evidence = []
    if os.path.exists("/.dockerenv"):
        evidence.append("/.dockerenv present")
    cg = read_text("/proc/1/cgroup") or ""
    for token in ("docker", "kubepods", "containerd", "lxc", "libpod"):
        if token in cg:
            evidence.append(f"/proc/1/cgroup mentions {token}")
    return {"container_detected": bool(evidence), "evidence": evidence}


def environment(binary, build_dir, profile_label):
    rc, head = git("rev-parse", "HEAD")
    _, tree_src = git("rev-parse", "HEAD:src")
    _, tree_inc = git("rev-parse", "HEAD:include")
    _, tree_cmake = git("rev-parse", "HEAD:CMakeLists.txt")
    _, dirty = git("status", "--porcelain")
    cache = read_text(os.path.join(build_dir, "CMakeCache.txt")) or ""
    m = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache, re.M)
    mem = {}
    for line in (read_text("/proc/meminfo") or "").splitlines():
        if line.startswith(("MemTotal:", "MemAvailable:")):
            k, v = line.split(":", 1)
            mem[k] = int(v.split()[0])
    gov = read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    return {
        "binary": {"path": os.path.relpath(binary, REPO) if binary.startswith(REPO) else binary,
                   "sha256": sha256_file(binary), "bytes": os.path.getsize(binary)},
        "source": {"git_head": head if rc == 0 else None, "tree_src": tree_src or None,
                   "tree_include": tree_inc or None, "tree_CMakeLists": tree_cmake or None,
                   "dirty": bool(dirty), "dirty_entries": dirty.splitlines()[:20]},
        "build": {"profile_label": profile_label, "cmake_build_type": m.group(1) if m else None,
                  "build_dir_for_cache": os.path.relpath(build_dir, REPO),
                  "cmake": first_line(["cmake", "--version"]), "ninja": first_line(["ninja", "--version"]),
                  "cc": first_line([os.environ.get("CC", "cc"), "--version"])},
        "cpu": cpu_info(),
        "memory_kB": mem or None,
        "kernel": {"release": platform.release(), "version": platform.version()},
        "os": platform.platform(),
        "python": sys.version.split()[0],
        "container": container_info(),
        "shared_vm_note": "4 vCPU shared VM (hypervisor flag set). Timing here is not performance qualification (C10).",
        "cpu_governor_cpu0": gov if gov is not None else {"value": None, "reason": "cpufreq governor not exposed"},
        "loadavg_before_run": list(os.getloadavg()),
        "pmu": {"status": "unavailable", "reason": "not collected: no perf_event_open counters are used by this "
                                                   "bench (PERFORMANCE.md section 5 makes them optional)"},
    }


# ---------------------------------------------------------------- live driver


def run_live(args):
    binary = os.path.abspath(args.binary)
    if not os.path.isfile(binary):
        raise SystemExit(f"binary not found: {binary}")
    label = args.profile_label
    build_dir = os.path.abspath(args.build_dir)
    args.stress_rows = [int(x) for x in str(args.stress_rows).split(",") if x.strip()]
    os.makedirs(EVIDENCE, exist_ok=True)
    env_rec = environment(binary, build_dir, label)
    t_run = time.monotonic()
    print("generating fixtures", flush=True)
    world_1024 = make_world(1024, SEED_WORLD_1024)
    failures = []
    warm_samples, failed_trials, epochs, cold_samples = [], [], [], []
    try:
        warm_samples, failed_trials, epochs = warm_mode(args, world_1024, env_rec)
    except BenchError as exc:
        failures.append(f"warm: {exc}")
    try:
        cold_samples = cold_mode(args, world_1024)
    except BenchError as exc:
        failures.append(f"cold: {exc}")
    stresses = []
    if not args.skip_stress:
        for n in args.stress_rows:
            print(f"stress {n} rows", flush=True)
            world_big = make_world(n, n)
            st = stress_mode(args, world_big, env_rec)
            stresses.append(st)
            if st["status"] == "failed":
                failures.append(f"stress {n}: {st['reason']}")
            # the fixture is generated under build/ (gitignored); remove the largest file after use
            if n >= 1000000:
                os.remove(os.path.join(REPO, world_big["path"]))
    status = "passed" if not failures and len(warm_samples) == args.epochs * args.per_epoch else "failed"
    wall = round(time.monotonic() - t_run, 1)
    summary_warm = summarize_edits(warm_samples) if warm_samples else None
    summary_cold = summarize_edits(cold_samples) if cold_samples else None
    fr_pub = sum(1 for f in failed_trials if f["published"])
    raw = {
        "report": "M0-13 raw live-edit samples", "profile_label": label,
        "environment": env_rec,
        "configuration": {
            "binary": binary, "epochs": args.epochs, "timed_edits_per_epoch": args.per_epoch,
            "warm_up_policy": "per warm epoch: one untimed, verified warm-up edit (install v2) after world.create; "
                              "the warm-up edit is excluded from samples",
            "cold_policy": "each cold trial starts a new serve process; first request = engine.info; "
                           "world.create (spawns worker) untimed; first edit (install v2) timed",
            "cold_trials": args.cold_trials, "failed_candidate_trials_per_epoch": args.failed_per_epoch,
            "serve_policy": ("one serve process for all warm epochs (--share-serve)" if args.share_serve
                             else "fresh serve process per warm epoch; each epoch's world is in its own worker"),
            "candidate_submissions_per_epoch": 1 + args.per_epoch + args.failed_per_epoch,
            "candidate_slots": CAPSULE_SLOTS,
            "world_1024": {k: world_1024[k] for k in ("path", "rows", "seed", "sha256", "bytes")},
            "versions": {k: dict(v, source_bytes=os.path.getsize(os.path.join(REPO, v["source"])),
                                 source_sha256=sha256_file(os.path.join(REPO, v["source"])))
                         for k, v in VERSIONS.items()},
            "wrong_test": WRONG_TEST,
            "clock": "time.monotonic_ns (host); select() reads; JSON encode/decode included",
            "percentile_method": "nearest-rank",
            "interleaving": "v1/v2 alternate on every edit (world starts on v1)",
        },
        "warm": {"samples": warm_samples, "summary": summary_warm, "epochs": epochs},
        "warm_failed_candidates": {"trials": failed_trials, "published_count": fr_pub},
        "cold": {"samples": cold_samples, "summary": summary_cold},
        "stress": stresses,
        "failures": failures,
        "wall_seconds": wall,
        "status": status,
    }
    raw_name = f"m0-latency-{label}-raw.json"
    with open(os.path.join(EVIDENCE, raw_name), "w", encoding="utf-8") as fh:
        json.dump(raw, fh, indent=1, sort_keys=True)
        fh.write("\n")
    write_summary(args, raw, env_rec, binary, raw_name)
    print(json.dumps({"status": status, "failures": failures, "warm_n": len(warm_samples),
                      "cold_n": len(cold_samples), "stress": [(s["rows"], s["status"]) for s in stresses]}),
          flush=True)
    return 0 if status == "passed" else 1


def fmt(s, key="p95"):
    return None if s is None else s[key]


def write_summary(args, raw, env_rec, binary, raw_name):
    label = args.profile_label
    m = {}

    def meas(key, value, unit, status, reason):
        m[key] = {"value": value, "unit": unit, "status": status, "reason": reason}

    raw_ref = f"raw samples and full statistics: planning/evidence/{raw_name}"
    warm = raw["warm"]["summary"]
    if warm:
        n = warm["total_edit_to_first_effect_ms"]["n"]
        for name, key in (("warm.capsule_submit_ms", "submit_ms"), ("warm.candidate_evaluate_ms", "evaluate_ms"),
                          ("warm.candidate_install_rpc_ms", "install_rpc_ms"),
                          ("warm.world_step_first_effect_ms", "step_first_effect_ms"),
                          ("warm.total_edit_to_first_effect_ms", "total_edit_to_first_effect_ms")):
            s = warm[key]
            meas(name + ".p95", s["p95"], "ms", "measured",
                 f"warm, 1024 rows, n={s['n']}, min={s['min']:.3f}, median={s['median']:.3f}, "
                 f"p99={s['p99']:.3f}, max={s['max']:.3f}; {raw_ref}")
        meas("warm.tick_boundary_wait_ms", None, "ms", "unavailable",
             "candidate.install is synchronous in M0: queue, tick-boundary wait and publish happen inside the "
             "install RPC, and the host cannot separate them. The install RPC time is reported instead.")
    else:
        meas("warm.total_edit_to_first_effect_ms.p95", None, "ms", "not_run", "no warm samples were collected")
    cold = raw["cold"]["summary"]
    if cold:
        s = cold["total_edit_to_first_effect_ms"]
        meas("cold.first_edit_total_ms.p95", s["p95"], "ms", "measured",
             f"new process, first edit, 1024 rows, n={s['n']}, min={s['min']:.3f}, median={s['median']:.3f}, "
             f"max={s['max']:.3f}; {raw_ref}")
        fr = [c["process_first_request_ms"] for c in raw["cold"]["samples"]]
        cw = [c["world_create_ms"] for c in raw["cold"]["samples"]]
        meas("cold.process_first_request_ms.p95", stats(fr)["p95"], "ms", "measured",
             f"engine.info as first request of a new serve process, n={len(fr)}; {raw_ref}")
        meas("cold.world_create_ms.p95", stats(cw)["p95"], "ms", "measured",
             f"world.create (spawns worker) in a new process, n={len(cw)}; {raw_ref}")
    fp = raw["warm_failed_candidates"]
    meas("failed_candidate_publications", fp["published_count"], "count", "measured",
         f"{len(fp['trials'])} failed-candidate trials; each refused by candidate.install and verified unchanged "
         f"after a following tick; {raw_ref}")
    checks = []
    for st in raw["stress"]:
        tag = f"stress_{st['rows']}"
        if st["status"] == "passed":
            stp = stats(st["phases"]["step_ms"])
            meas(f"{tag}.world_step_ms.p95", stp["p95"], "ms", "measured",
                 f"{st['rows']}-row stress diagnostic (not a game-scale claim), n={stp['n']}, "
                 f"median={stp['median']:.3f}, max={stp['max']:.3f}; {raw_ref}")
            meas(f"{tag}.world_create_ms", st["phases"]["world_create_ms"], "ms", "measured",
                 f"world.create host round trip, 1 sample; {raw_ref}")
            snap = st["phases"]["snapshot"]
            meas(f"{tag}.snapshot_capture_host_ms", snap["host_ms"], "ms", "measured",
                 f"world.snapshot host round trip, 1 sample; engine capture_ns={snap['engine_capture_ns']}; "
                 f"{raw_ref}")
            meas(f"{tag}.snapshot_bytes", snap["bytes"], "bytes", "measured",
                 "canonical snapshot size reported by the engine")
            meas(f"{tag}.restore_host_ms", st["phases"]["restore"]["host_ms"], "ms", "measured",
                 f"world.restore host round trip, 1 sample; {raw_ref}")
            rss = st["worker_rss_end"]
            meas(f"{tag}.worker_VmHWM_KiB", rss.get("VmHWM_KiB"), "KiB",
                 "measured" if rss.get("VmHWM_KiB") is not None else "unavailable",
                 rss.get("reason") or "worker VmHWM at end of the stress run")
            meas(f"{tag}.nominal_column_bytes", st["nominal_column_bytes"], "bytes", "measured",
                 "derived: 2 u32 columns x 4 bytes x rows; excludes entity_ids and runtime overhead")
        else:
            for key in ("world_step_ms.p95", "snapshot_capture_host_ms", "restore_host_ms"):
                meas(f"{tag}.{key}", None, "ms", "not_run",
                     f"stress {st['status']}: {st['reason']}")
        checks.append({"name": f"stress run ({st['rows']} rows)",
                       "status": {"passed": "passed", "refused": "not_run", "cap_exceeded": "not_run",
                                  "failed": "failed"}[st["status"]],
                       "reason": None if st["status"] == "passed" else st["reason"]})
    meas("pmu.cycles_per_entity", None, "cycles/entity", "unavailable",
         "no PMU counters collected (PERFORMANCE.md section 5 optional; not part of this bench)")
    checks[:0] = [
        {"name": "every warm and cold edit verified against the reference model after its timed window",
         "status": "passed" if not any(f.startswith(("warm", "cold")) for f in raw["failures"]) else "failed",
         "reason": None if not raw["failures"] else "; ".join(raw["failures"])},
        {"name": "every failed candidate refused and never published",
         "status": "passed" if fp["published_count"] == 0 and fp["trials"] else "failed",
         "reason": None},
    ]
    summary = {
        "report_format": "0.1", "kind": "runtime_evidence", "status": raw["status"],
        "source_tree_hash": f"git HEAD {env_rec['source']['git_head']}; tree src {env_rec['source']['tree_src']}; "
                            f"tree include {env_rec['source']['tree_include']}",
        "artifact_hash": f"sha256:{env_rec['binary']['sha256']}",
        "hardware": {"cpu": env_rec["cpu"], "memory_kB": env_rec["memory_kB"], "kernel": env_rec["kernel"],
                     "container": env_rec["container"], "profile_label": label,
                     "cmake_build_type": env_rec["build"]["cmake_build_type"]},
        "commands": [
            f"python3 -I bench/reload/bench_reload.py live --binary {os.path.relpath(binary, REPO)} "
            f"--profile-label {label} --epochs {args.epochs} --per-epoch {args.per_epoch} "
            f"--cold-trials {args.cold_trials} --stress-rows {','.join(map(str, args.stress_rows))}",
            f"{os.path.relpath(binary, REPO)} serve  (JSON lines over stdio, one process per epoch and per cold trial)",
        ],
        "measurements": m, "checks": checks,
        "notes": ("Summary of planning/evidence/%s; schema-conforming subset. Full distributions, raw samples, "
                  "per-epoch RSS and the environment are in %s. Shared 4-vCPU VM: this is observation, not "
                  "performance qualification (ENGINE_CONSTITUTION C10). Schema deviation: the schema forbids "
                  "extra top-level keys and a measurement's percentile table, so the full statistics live in the "
                  "raw file." % (f"m0-latency-{label}.json", raw_name)),
    }
    with open(os.path.join(EVIDENCE, f"m0-latency-{label}.json"), "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=1, sort_keys=True)
        fh.write("\n")


# ---------------------------------------------------------------- native build


def count_compiles(out):
    return {"compile_steps": len(re.findall(r"Building C object", out)),
            "link_steps": len(re.findall(r"Linking C (executable|static library|shared library)", out))}


def run_timed(argv):
    t0 = NOW()
    r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True, shell=False, check=False)
    ms = now_ms_from(t0)
    if r.returncode != 0:
        raise BenchError(f"{' '.join(argv)} exited {r.returncode}: {r.stderr[-800:]}")
    return ms, r.stdout + r.stderr


def touch(rel):
    p = os.path.join(REPO, rel)
    os.utime(p, None)
    return rel


def run_native(args):
    scratch = os.path.abspath(args.scratch)
    if scratch.startswith(REPO + os.sep) or scratch == REPO:
        raise SystemExit("scratch build directory must lie outside the repository")
    os.makedirs(scratch, exist_ok=True)
    os.makedirs(EVIDENCE, exist_ok=True)
    gen = ["-G", "Ninja"] if shutil.which("ninja") else []
    cfg = ["-DCMAKE_BUILD_TYPE=Debug"]  # dev.py --profile dev
    clean, incr_vm, incr_ir = [], [], []
    bdir = os.path.join(scratch, "native-build")
    touched_mtime_only = []
    try:
        for i in range(args.repeats):
            if os.path.isdir(bdir):
                shutil.rmtree(bdir)
            ms_conf, out_c = run_timed(["cmake", "-S", REPO, "-B", bdir] + gen + cfg)
            ms_build, out_b = run_timed(["cmake", "--build", bdir, "--parallel"])
            clean.append({"repeat": i, "configure_ms": ms_conf, "build_ms": ms_build,
                          "total_ms": ms_conf + ms_build, **count_compiles(out_b)})
            print(f"  clean {i}: configure {ms_conf:.0f} ms, build {ms_build:.0f} ms", flush=True)
        for i in range(args.repeats):
            rel = touch("src/vm/su_vm.c")
            touched_mtime_only.append(rel)
            ms, out = run_timed(["cmake", "--build", bdir, "--parallel"])
            incr_vm.append({"repeat": i, "build_ms": ms, **count_compiles(out)})
            print(f"  incremental su_vm.c {i}: {ms:.0f} ms", flush=True)
        for i in range(args.repeats):
            rel = touch("include/sutekh/ir.h")
            touched_mtime_only.append(rel)
            ms, out = run_timed(["cmake", "--build", bdir, "--parallel"])
            incr_ir.append({"repeat": i, "build_ms": ms, **count_compiles(out)})
            print(f"  incremental ir.h {i}: {ms:.0f} ms", flush=True)
    except BenchError as exc:
        status, reason = "failed", str(exc)
    else:
        status, reason = "passed", None
    def st(rows):
        vals = [r["build_ms"] if "build_ms" in r else r["total_ms"] for r in rows]
        return {"n": len(vals), "min": min(vals) if vals else None, "median": statistics.median(vals) if vals else None,
                "max": max(vals) if vals else None, "values": vals,
                "p95": None, "p95_reason": "n is below 20, so a p95 would not be meaningful; min/median/max and raw values are reported"}
    _, head = git("rev-parse", "HEAD")
    doc = {
        "report": "M0-13 native build latency (separate from live edit latency)",
        "status": status, "reason": reason,
        "source": {"git_head": head},
        "commands": [
            "cmake -S <repo> -B <scratch>/native-build -G Ninja -DCMAKE_BUILD_TYPE=Debug   (dev.py configure equivalent)",
            "cmake --build <scratch>/native-build --parallel   (dev.py build equivalent)",
            "touch src/vm/su_vm.c ; cmake --build ... --parallel",
            "touch include/sutekh/ir.h ; cmake --build ... --parallel",
        ],
        "scratch_build_dir": "<scratch outside repo>",
        "toolchain": {"cmake": first_line(["cmake", "--version"]), "ninja": first_line(["ninja", "--version"]),
                      "cc": first_line([os.environ.get("CC", "cc"), "--version"])},
        "clean_build": clean, "incremental_su_vm_c": incr_vm, "incremental_ir_h": incr_ir,
        "summary": {"clean_total_ms": st(clean), "incremental_su_vm_c_ms": st(incr_vm),
                    "incremental_ir_h_ms": st(incr_ir)},
        "touch_note": "touch changes mtime only; file contents are unchanged (git sees no content change)",
        "dependency_builds": "none: the project has no external dependencies to build",
        "touched_paths": sorted(set(touched_mtime_only)),
    }
    with open(os.path.join(EVIDENCE, "m0-native-build.json"), "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, sort_keys=True)
        fh.write("\n")
    print(json.dumps({"status": status, "reason": reason}), flush=True)
    return 0 if status == "passed" else 1


# ---------------------------------------------------------------- cli


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    live = sub.add_parser("live")
    live.add_argument("--binary", default=os.path.join(REPO, "build", "sutekh"))
    live.add_argument("--build-dir", default=os.path.join(REPO, "build"))
    live.add_argument("--profile-label", required=True, help="dev or release (names the output files)")
    live.add_argument("--epochs", type=int, default=60)
    live.add_argument("--per-epoch", type=int, default=12)
    live.add_argument("--failed-per-epoch", type=int, default=2)
    live.add_argument("--cold-trials", type=int, default=60)
    live.add_argument("--share-serve", action="store_true",
                      help="reuse one serve process for all warm epochs (fails: the worker world cap)")
    live.add_argument("--stress-rows", default="1000000,250000",
                      help="comma-separated stress world sizes (the engine caps fixtures at "
                           "SUTEKH_FIXTURE_MAX_BYTES, default 64 MiB)")
    live.add_argument("--stress-steps", type=int, default=20)
    live.add_argument("--stress-edits", type=int, default=6)
    live.add_argument("--stress-cap-s", type=float, default=900.0)
    live.add_argument("--skip-stress", action="store_true")
    live.set_defaults(func=run_live)
    nat = sub.add_parser("native")
    nat.add_argument("--scratch", required=True, help="scratch directory outside the repository")
    nat.add_argument("--repeats", type=int, default=3)
    nat.set_defaults(func=run_native)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
