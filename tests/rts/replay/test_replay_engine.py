# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""T-RTS-REPLAY-1..3 against the engine (contract M3 9.1, 10.3) through a real `sutekh serve`.

A run of the full RTS world is recorded with the engine backend (bench/rts/replay/engine_backend.py) and replayed:
  REPLAY-1: replay gives identical digest records, with one tick worker and with two (chunked from one row);
  REPLAY-2: a changed command value and a dropped world.set_fields report the first divergent tick; a changed
            operator_manifest_hash and caps.control_events_counted = true fail closed;
  REPLAY-3: the worker is killed mid-run (SIGKILL) and the world is recovered in a replacement worker from a
            checkpoint plus the log, replaying at most checkpoint_every = 32 ticks; the run continues and every
            digest record equals the uninterrupted recording.
Digests are taken every tick, so the reported tick is the exact tick.

Usage: python3 -I tests/rts/replay/test_replay_engine.py <path to sutekh>
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "bench", "rts", "replay"))

import engine_backend as eb  # noqa: E402
import rts_replay as rr  # noqa: E402
import rts_session as rs  # noqa: E402

CAP = 160
TICKS = 40
CHECKPOINT_EVERY = 32
SCENE = {"generator": "rts-units-splitmix64/0.1", "params": {"units": 64}, "seed": 20261013, "capacity": CAP}
ENV = {"SUTEKH_CHECKPOINT_INTERVAL_TICKS": str(CHECKPOINT_EVERY)}

checks = 0
failures = 0


def check(cond: bool, what: str) -> None:
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def h(slot: int, gen: int = 1) -> int:
    return (gen << 32) | slot


def commands() -> list:
    """No unit can die before t = 6 (every scene row has range_sq 0), so the slots and generations named here are
    known when the run is recorded: the scene takes slots 0..63 and the batch at t = 2 takes 64..71."""
    armed = {"range_sq": 40000, "order_mode": 2, "order_x": 16000, "order_y": 16000}
    return [
        (0, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(s), "values": {"order_mode": 1, "order_x": 16000,
                                                                          "order_y": 16000}} for s in range(8)]}),
        (2, rr.CMD_SPAWN_BATCH, {"rows": [{"values": {"kind": 2, "hp": 0, "pos_x": 15000 + 400 * i, "pos_y": 15000,
                                                      "speed": 4, "reload": 1, "damage": 1, "flight": 1}}
                                          for i in range(4)] +
                                         [{"values": {"kind": 1, "team": i & 1, "hp": 50, "pos_x": 16000,
                                                      "pos_y": 16000 + 50 * i, "reload": 2, "damage": 7,
                                                      "flight": 3}, "external_id": 900 + i} for i in range(4)]}),
        (3, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(s), "values": {"formation_slot": 64, "formation_gen": 1,
                                                                          "formation_ox": 16384 + 64 * (s - 8),
                                                                          "formation_oy": 16384}}
                                             for s in range(8, 16)]}),
        (4, rr.CMD_DESPAWN, {"entity": h(10)}),
        (6, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(s), "values": dict(armed)} for s in range(16, 48)]}),
        (30, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(65), "values": {"order_mode": 1, "order_x": 100,
                                                                            "order_y": 100}}]}),
    ]


def record(binary: str, path: str, digest_every: int = 1) -> None:
    be = eb.EngineBackend(binary, CAP, env=ENV)
    try:
        rec = rr.Recorder(be, dict(SCENE), TICKS, digest_every, checkpoint_every=CHECKPOINT_EVERY)
        for t, name, args in commands():
            rec.command(t, name, args)
        rec.run()
        rec.write(path)
    finally:
        be.close()


def replay(binary: str, path: str, env: dict | None = None, backend=None) -> dict:
    be = backend or eb.EngineBackend(binary, CAP, env=dict(ENV, **(env or {})))
    try:
        return rr.replay(path, be)
    finally:
        be.close()


def edit(path: str, out: str, fn) -> None:
    with open(path, encoding="ascii") as f:
        lines = [ln for ln in f.read().split("\n") if ln]
    lines = fn(lines)
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


class KillingBackend(eb.EngineBackend):
    """Kills the worker before the step of tick `kill_at` and retries that step once after the reported recovery."""

    def __init__(self, binary: str, kill_at: int):
        super().__init__(binary, CAP, env=ENV)
        self.kill_at = kill_at
        self.ticks_done = 0
        self.recovery = None

    def step(self) -> None:
        if self.ticks_done == self.kill_at and self.recovery is None:
            old = self.session.kill_worker()
            resp = self.session.raw(self.session.line("world.step", {"world_id": self.wid,
                                                                     "expected_world_revision": self.rev,
                                                                     "ticks": 1}))
            check(resp.get("error", {}).get("code") == "SU_E_WORKER_FAILED", f"step after the kill fails: {resp}")
            self.recovery = resp.get("recovery", {})
            check(self.recovery.get("recovered") is True and self.recovery.get("replaced_pid_old") == old,
                  f"world recovered in a replacement worker: {self.recovery}")
        super().step()
        self.ticks_done += 1


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    binary = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="rts-replay-engine-")
    try:
        path = os.path.join(tmp, "run.rtsreplay")
        record(binary, path)
        header, cmds, digests = rr.read_replay(path)
        check(len(digests) == TICKS + 1 and len(cmds) == len(commands()), "recording has every digest and command")
        check(header["checkpoint_every"] == CHECKPOINT_EVERY and header["caps"]["control_events_counted"] is False,
              "header records checkpoint_every 32 and control_events_counted false")
        check(digests[-1]["counts"]["alive"] < digests[0]["counts"]["alive"] + 8, "combat kills units in the run")
        check(header["system_manifest_hash"] != "sha256:" + "0" * 64, "engine header carries a system manifest hash")

        # REPLAY-1
        r = replay(binary, path)
        check(r["ok"] and r["digests_checked"] == TICKS + 1, f"replay identical: {r['divergence']}")
        r = replay(binary, path, {"SUTEKH_TICK_WORKERS": "2", "SUTEKH_TICK_CHUNK_MIN_ROWS": "1"})
        check(r["ok"] and r["digests_checked"] == TICKS + 1, f"replay with two tick workers identical: {r['divergence']}")

        # REPLAY-2: a changed command value (the t = 6 range_sq) diverges at t = 6 or later, never before.
        flipped = os.path.join(tmp, "flipped.rtsreplay")
        edit(path, flipped, lambda ls: [ln.replace('"range_sq":40000', '"range_sq":40001') for ln in ls])
        r = replay(binary, flipped)
        check(not r["ok"] and r["divergence"] is not None and r["divergence"].t >= 6,
              f"changed command value reports a divergence at or after t 6: {r['divergence']}")
        flipped_t = r["divergence"].t if r["divergence"] else None
        dropped = os.path.join(tmp, "dropped.rtsreplay")
        edit(path, dropped, lambda ls: [ln for ln in ls
                                        if not (json.loads(ln).get("t") == 3 and json.loads(ln).get("cmd") == rr.CMD_SET_FIELDS)])
        r = replay(binary, dropped)
        check(not r["ok"] and r["divergence"] is not None and r["divergence"].t == 3,
              f"dropped world.set_fields at t 3 reports tick 3: {r['divergence']}")
        for key, value, what in (("operator_manifest_hash", "sha256:" + "1" * 64, "changed operator_manifest_hash"),
                                 ("caps", dict(header["caps"], control_events_counted=True),
                                  "control_events_counted true")):
            bad = os.path.join(tmp, "bad.rtsreplay")

            def fn(ls, key=key, value=value):
                hd = json.loads(ls[0])
                hd[key] = value
                return [rr.canonical_json(hd)] + ls[1:]
            edit(path, bad, fn)
            try:
                replay(binary, bad)
                check(False, f"{what} must fail closed")
            except rr.ReplayError as e:
                check(e.code in ("SU_E_SCHEMA_MISMATCH", "SU_E_INVALID_ARGUMENT"), f"{what} fails closed ({e.code})")

        # REPLAY-3: digests only at t 0 and t 40 (a digest is a world.snapshot, which is also a checkpoint), so
        # the base is the interval checkpoint at 32 and ticks 33..36 come from the replay log. Kill before the step
        # of tick 37; recovery replays at most 32 ticks; the final digest equals an uninterrupted recording.
        sparse = os.path.join(tmp, "sparse.rtsreplay")
        record(binary, sparse, digest_every=TICKS)
        kb = KillingBackend(binary, 36)
        r = replay(binary, sparse, backend=kb)
        check(r["ok"] and r["digests_checked"] == 2, f"run with a worker kill equals the recording: {r['divergence']}")
        rec = kb.recovery or {}
        span = 36 - rec.get("from_checkpoint_tick", -10**9)
        check(0 < span <= CHECKPOINT_EVERY, f"recovery replays 1..{CHECKPOINT_EVERY} ticks from a checkpoint ({rec})")
        check(rec.get("ops_replayed", 0) >= 1, f"recovery replays logged operations ({rec})")
        print(f"replay-engine: flipped value diverged at t {flipped_t}; recovery {rec}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"test_replay_engine: {checks} checks, {failures} failures")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
