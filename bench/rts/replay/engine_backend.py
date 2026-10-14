# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Engine backend of the rts-replay/0.1 recorder and replayer (contract M3 9.1, T-RTS-REPLAY-1..3).

The backend drives a real `sutekh serve` supervisor (rts_session.Session) with the full six-system RTS world.
Digests are the engine's own, so an engine-recorded file is replayed against the engine; they are not
byte-compatible with the standalone model backend's digests. The independent
model_snapshot.SnapshotModel adapter reproduces the engine encoding for Q8 checks:
  - state: "sha256:" + the SHA-256 of the canonical snapshot bytes (world.snapshot, snapshot format 0.2: rows up
    to high water, entity table, retired count, event counters and retained records);
  - events: SHA-256 over canonical JSON lines (sorted keys, no whitespace) of the retained event records of
    events.read, in event_seq order;
  - counts: alive, retired (world.inspect entities), events (retained records), dropped (events.read),
    truncated_rows (world.inspect entities, alive rows flagged by the last rts.spatial run).
A refused command raises rts_replay.ModelError with the engine's status name, which the replayer reports as a
divergence at that tick.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rts_replay as rr  # noqa: E402
import rts_session as rs  # noqa: E402

BATCH_ROWS = 1024  # OD-3b


def _canon(obj) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"))


def _sha(lines) -> str:
    h = hashlib.sha256()
    for line in lines:
        h.update(line.encode("ascii") + b"\n")
    return "sha256:" + h.hexdigest()


class EngineBackend(rr.Backend):
    def __init__(self, binary: str, capacity: int, event_log_max: int = 65536, env: dict | None = None):
        self.session = rs.Session(binary, env)
        self.world = SimpleNamespace(cap=capacity, event_log_max=event_log_max)
        self.wid, self.rev = self.session.create_rts(capacity, event_log_max=event_log_max)
        ins = self._inspect()
        man = ins["semantic_manifest"]
        host = [m for m in man if m["source_hash"] == rs.EMPTY_SHA256]
        self._operator_hash = _sha(_canon({k: m[k] for k in ("system_id", "semantic_hash", "capsule_hash")})
                                   for m in host)
        self.system_manifest_hash = _sha(_canon({k: m[k] for k in ("system_id", "semantic_hash", "capsule_hash",
                                                                   "source_hash")}) for m in man)
        self.schema_digest = _sha(_canon(f) for f in ins["schema"])

    def close(self) -> None:
        self.session.close()

    def _inspect(self) -> dict:
        return self.session.call("world.inspect", {"world_id": self.wid, "limit": 1})

    def _call(self, method: str, params: dict) -> dict:
        p = {"world_id": self.wid, "expected_world_revision": self.rev}
        p.update(params)
        try:
            r = self.session.call(method, p)
        except rs.RpcError as e:
            raise rr.ModelError(e.name, method, e.message) from None
        self.rev = r["world_revision"]
        return r

    @property
    def operator_manifest_hash(self) -> str:
        return self._operator_hash

    def load_scene(self, scene: dict) -> None:
        rows = [{"values": v} for v in rr.scene_rows(scene)]
        for i in range(0, len(rows), BATCH_ROWS):
            self._call("world.spawn_batch", {"rows": rows[i:i + BATCH_ROWS]})

    def scene_digest(self) -> str:
        return self._state()

    def apply(self, cmd: dict) -> None:
        self._call(cmd["cmd"], dict(cmd["args"]))

    def step(self) -> None:
        self._call("world.step", {"ticks": 1})

    def _state(self) -> str:
        return "sha256:" + self.session.snapshot_sha(self.wid, self.rev)

    def digest(self, t: int) -> dict:
        ev, dropped = self.session.all_events(self.wid)
        ents = self._inspect()["entities"]
        return {
            "t": t,
            "state": self._state(),
            "events": _sha(_canon(e) for e in ev),
            "counts": {"alive": ents["alive"], "retired": ents["retired"], "events": len(ev), "dropped": dropped,
                       "truncated_rows": ents.get("truncated_rows", 0)},
        }
