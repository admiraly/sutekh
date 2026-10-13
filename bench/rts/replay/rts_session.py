# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Drives a real `sutekh serve` supervisor over stdio with the full M3 RTS world (stdlib only).

The world is built through the supervisor's own world.create from the committed capsules, in the capsule
order of contract 3.2: rts.spatial (host), rts.goal, rts.combat, rts.move, rts.scatter (host), rts.damage.
The fixture has the 39 fields of contract section 2 and no rows; capacity comes from max_entities.
"""
from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "bench", "rts", "ref"))

import model  # noqa: E402  (bench/rts/ref/model.py: the field table of contract section 2)

NONE = 0xFFFFFFFF
EMPTY_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"  # source hash of a host entry
REQUEST_BYTES_MAX = 1048576

IR_SYSTEMS = ("goal", "combat", "move", "damage")
# Capsule order of contract 3.2: a string is an IR capsule, a pair is (host capsule, operator).
SCHEDULE = (
    ("spatial.capsule.json", "rts.spatial"),
    "goal.capsule.json",
    "combat.capsule.json",
    "move.capsule.json",
    ("scatter.capsule.json", "rts.scatter"),
    "damage.capsule.json",
)


class RpcError(Exception):
    def __init__(self, status: int, name: str, message: str):
        super().__init__(f"{name} ({status}): {message}")
        self.status = status
        self.name = name
        self.message = message


class Session:
    """One supervisor process with a scratch project holding the RTS capsules and an empty fixture."""

    def __init__(self, binary: str, env: dict | None = None):
        self.dir = tempfile.mkdtemp(prefix="sutekh-rts-")
        caps = os.path.join(REPO, "capsules", "rts")
        for name in IR_SYSTEMS:
            shutil.copy(os.path.join(caps, f"{name}.capsule.json"), self.dir)
            shutil.copy(os.path.join(caps, f"{name}.lir.json"), self.dir)
        for name in ("spatial", "scatter"):
            shutil.copy(os.path.join(caps, "host", f"{name}.capsule.json"), self.dir)
        fixture = {"fixture_format": "0.1", "schema_id": "rts", "schema_version": 1, "tick": 0, "entity_ids": [],
                   "columns": {name: [] for name in model.FIELD_NAMES}}
        with open(os.path.join(self.dir, "rts_empty.json"), "w", encoding="utf-8") as fh:
            json.dump(fixture, fh, separators=(",", ":"))
        full_env = dict(os.environ)
        full_env.update(env or {})
        self.proc = subprocess.Popen([binary, "serve", "--root", self.dir], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=full_env)
        self.seq = 0

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)

    def raw(self, line: bytes) -> dict:
        """Sends one request line (without its newline) and returns the parsed response."""
        self.proc.stdin.write(line + b"\n")
        self.proc.stdin.flush()
        out = self.proc.stdout.readline()
        if not out:
            raise RuntimeError("supervisor closed its output")
        return json.loads(out)

    def line(self, method: str, params: dict) -> bytes:
        self.seq += 1
        return json.dumps({"protocol": "0.1", "id": f"r{self.seq}", "method": method, "params": params},
                          separators=(",", ":")).encode("utf-8")

    def call(self, method: str, params: dict) -> dict:
        resp = self.raw(self.line(method, params))
        if "error" in resp:
            e = resp["error"]
            raise RpcError(e.get("status", 0), e.get("code", ""), e.get("message", ""))
        return resp["result"]

    def create_rts(self, capacity: int, **cfg) -> tuple[str, str]:
        paths = [{"capsule_path": c[0], "host_operator": c[1]} if isinstance(c, tuple) else c for c in SCHEDULE]
        params = {"fixture_path": "rts_empty.json", "capsule_paths": paths, "max_entities": capacity,
                  "max_struct_per_tick": cfg.get("max_struct_per_tick", capacity),
                  "max_events_per_tick": cfg.get("max_events_per_tick", capacity)}
        if "event_log_max" in cfg:
            params["event_log_max"] = cfg["event_log_max"]
        r = self.call("world.create", params)
        return r["world_id"], r["world_revision"]

    def worker_pid(self) -> int:
        return self.call("engine.info", {})["active_worker"]["pid"]

    def kill_worker(self) -> int:
        pid = self.worker_pid()
        os.kill(pid, signal.SIGKILL)
        return pid

    def snapshot_sha(self, wid: str, rev: str) -> str:
        return self.call("world.snapshot", {"world_id": wid, "expected_world_revision": rev})["sha256"]

    def events(self, wid: str, cursor: int = 0, limit: int = 256) -> dict:
        return self.call("events.read", {"world_id": wid, "cursor": cursor, "max": limit})

    def all_events(self, wid: str) -> tuple[list, int]:
        """Every retained event record in order, and the dropped count (events.read pages of 256)."""
        out, cursor, dropped = [], 0, 0
        while True:
            r = self.events(wid, cursor)
            out.extend(r["events"])
            dropped = r["dropped"]
            if not r["events"] or r["next_cursor"] == cursor:
                return out, dropped
            cursor = r["next_cursor"]


def max_row(formation_slot: int | None) -> dict:
    """A domain-valid kind-1 row with the longest value of every field (contract 10.3 (a)). formation_slot must
    name a live row (Q2); None gives NONE with generation 0."""
    vals = {name: 0 for name in model.SPAWN_IDLE_ZERO}
    for name in model.SPAWN_IDLE_NONE:
        vals[name] = NONE
    vals.update({"kind": 1, "team": 1, "hp": 65535, "pos_x": 32767, "pos_y": 32767, "speed": 1024,
                 "range_sq": 262144, "reload": 1024, "damage": 65535, "flight": 64, "order_mode": 2,
                 "order_x": 32767, "order_y": 32767, "formation_ox": 32767, "formation_oy": 32767})
    if formation_slot is None:
        vals.update({"formation_slot": NONE, "formation_gen": 0})
    else:
        vals.update({"formation_slot": formation_slot, "formation_gen": NONE})
    assert set(vals) == set(model.FIELD_NAMES)
    return {"values": vals, "external_id": NONE}
