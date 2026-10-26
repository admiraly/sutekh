# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Full six-system RTS world (rts.spatial and rts.scatter native operators included) against a committed recording.

The host API does not carry the RTS operators (src/host/su_host_operators.c), so the determinism matrix
(det_matrix.c) runs the RTS IR systems with host stand-ins. This test covers the operators: a real `sutekh serve`
(bench/rts/replay/engine_backend.py) replays tests/determinism/rts_engine_golden.rtsreplay, an rts-replay/0.1
command stream recorded once by a Linux GCC build, and compares the engine's canonical state digest (SHA-256 of the
world.snapshot bytes), event digest and counts at every tick. Every build that passes produced the recorded state
bytes at all 241 boundaries. These digests are the engine snapshot (format 0.2), not su_host_digest.

    python3 -I tests/determinism/rts_engine_golden.py check <sutekh> [golden]
    python3 -I tests/determinism/rts_engine_golden.py record <sutekh> <out>   (maintainers: regenerates the golden)
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "bench", "rts", "replay"))

import engine_backend as eb  # noqa: E402
import rts_replay as rr  # noqa: E402

GOLDEN = os.path.join(HERE, "rts_engine_golden.rtsreplay")
CAP = 256
TICKS = 240
SCENE = {"generator": "rts-units-splitmix64/0.1", "params": {"units": 192}, "seed": 20261026, "capacity": CAP}
ENV = {"SUTEKH_CHECKPOINT_INTERVAL_TICKS": "64"}


def h(slot: int, gen: int = 1) -> int:
    return (gen << 32) | slot


def commands() -> list:
    """Orders, a reinforcement batch with anchors, a formation, a despawn and armed units: combat through rts.spatial
    target selection and rts.scatter damage, with deaths. Every handle is named before any unit can have died
    (t <= 4), except slot 193, an anchor (hp 0: never killed)."""
    armed = {"range_sq": 90000, "order_mode": 2, "order_x": 16384, "order_y": 16384}
    out = [
        (0, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(s), "values": dict(armed)} for s in range(0, 96)]}),
        (0, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(s), "values": {"order_mode": 1, "order_x": 12000 + 40 * s,
                                                                          "order_y": 20000}}
                                             for s in range(96, 128)]}),
        (1, rr.CMD_SPAWN_BATCH, {"rows": [{"values": {"kind": 2, "hp": 0, "pos_x": 15000 + 400 * i, "pos_y": 15000,
                                                      "speed": 6, "reload": 1, "damage": 1, "flight": 1}}
                                          for i in range(4)] +
                                         [{"values": {"kind": 1, "team": i & 1, "hp": 80, "pos_x": 16000 + 30 * i,
                                                      "pos_y": 16000 + 50 * i, "reload": 3, "damage": 9,
                                                      "flight": 2, "range_sq": 60000}, "external_id": 900 + i}
                                          for i in range(24)]}),
        (2, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(s), "values": {"formation_slot": 192, "formation_gen": 1,
                                                                          "formation_ox": 16384 + 64 * (s - 128),
                                                                          "formation_oy": 16384}}
                                             for s in range(128, 144)]}),
        (3, rr.CMD_DESPAWN, {"entity": h(150)}),
        (4, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(s), "values": dict(armed)} for s in range(128, 190)
                                             if s != 150]}),
        (120, rr.CMD_SET_FIELDS, {"entities": [{"entity": h(193), "values": {"order_mode": 1, "order_x": 100,
                                                                              "order_y": 100}}]}),
    ]
    return out


def record(binary: str, path: str) -> int:
    be = eb.EngineBackend(binary, CAP, env=ENV)
    try:
        rec = rr.Recorder(be, dict(SCENE), TICKS, 1, checkpoint_every=64)
        for t, name, args in commands():
            rec.command(t, name, args)
        rec.run()
        rec.write(path)
    finally:
        be.close()
    _, cmds, digests = rr.read_replay(path)
    last = digests[-1]
    print("recorded %s: %d commands, %d digest records, end counts %s" % (path, len(cmds), len(digests),
                                                                         last.get("counts")))
    return 0


def check(binary: str, path: str) -> int:
    be = eb.EngineBackend(binary, CAP, env=ENV)
    try:
        res = rr.replay(path, be)
    except rr.ReplayError as exc:
        print("FAIL: replay refused: %s" % exc, file=sys.stderr)
        return 1
    finally:
        be.close()
    if not res["ok"]:
        print("FAIL: %s" % res["divergence"], file=sys.stderr)
        return 1
    print("rts_engine_golden: %d digest records match %s" % (res["digests_checked"], os.path.basename(path)))
    return 0


def main(argv: list) -> int:
    if len(argv) >= 2 and argv[0] == "check":
        return check(os.path.abspath(argv[1]), argv[2] if len(argv) > 2 else GOLDEN)
    if len(argv) == 3 and argv[0] == "record":
        return record(os.path.abspath(argv[1]), argv[2])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
