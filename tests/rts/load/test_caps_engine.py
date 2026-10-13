# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""T-RTS-CAPS-1 (config check, N-3) and T-RTS-CAPS-2 through a real `sutekh serve` (contract M3 9.4, 10.3).

CAPS-1: world.create of an RTS world with max_struct_per_tick or max_events_per_tick below its capacity is refused
at creation (M3-M28). CAPS-2: an RTS world of capacity 16 with max_struct = max_events = 16 is filled by one
world.spawn_batch (16 control records, not counted), then all 16 units die in one tick, which must commit with 16
SU_EVENT_DESPAWNED records; repeated with event_log_max = 1 and with two tick workers (chunked from one row). Also
spawn 16, kill 8, spawn 8, kill 16 across two death ticks. The tick of every death and the alive counts are taken
from the Python reference model (bench/rts/ref/model.py) running the same calls.

Usage: python3 -I tests/rts/load/test_caps_engine.py <path to sutekh>
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "bench", "rts", "replay"))

import rts_session as rs  # noqa: E402
from rts_session import model  # noqa: E402

SPAWNED = 0xFFFFFFFE
DESPAWNED = 0xFFFFFFFD
CAP = 16

checks = 0
failures = 0


def check(cond: bool, what: str) -> None:
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def duel_pair(k: int, fighting: bool) -> list:
    """Two units of opposite teams on one spot, isolated from other pairs; each kills the other (hp 1, damage 5)
    when fighting, and never fires when not (range_sq 0)."""
    rows = []
    for team in (0, 1):
        rows.append({"values": {"kind": 1, "team": team, "hp": 1, "pos_x": 2000 * k + 100, "pos_y": 100,
                                "range_sq": 100 if fighting else 0, "reload": 1, "damage": 5, "flight": 1}})
    return rows


class Twin:
    """The engine world and the reference model, driven with the same calls."""

    def __init__(self, s: rs.Session, **cfg):
        self.s = s
        self.wid, self.rev = s.create_rts(CAP, max_struct_per_tick=CAP, max_events_per_tick=CAP, **cfg)
        self.m = model.World(CAP, cfg.get("event_log_max", model.EVENT_LOG_MAX_DEFAULT))

    def spawn(self, rows: list) -> list:
        r = self.s.call("world.spawn_batch", {"world_id": self.wid, "expected_world_revision": self.rev, "rows": rows})
        self.rev = r["world_revision"]
        for row in rows:
            self.m.spawn(dict(row["values"]))
        return r["entities"]

    def set_fields(self, handles: list, values: dict) -> None:
        ents = [{"entity": h, "values": dict(values)} for h in handles]
        r = self.s.call("world.set_fields", {"world_id": self.wid, "expected_world_revision": self.rev,
                                             "entities": ents})
        self.rev = r["world_revision"]
        for h in handles:
            for name, v in values.items():
                self.m.cols[name][h & 0xFFFFFFFF] = v

    def step(self) -> tuple[int, int]:
        """One tick in both. Returns (engine alive, model alive) after it."""
        r = self.s.call("world.step", {"world_id": self.wid, "expected_world_revision": self.rev, "ticks": 1})
        self.rev = r["world_revision"]
        model.tick(self.m)
        alive = self.s.call("world.inspect", {"world_id": self.wid, "limit": 1})["entities"]["alive"]
        return alive, sum(self.m.alive)

    def despawned_at(self, tick: int) -> list:
        ev, _ = self.s.all_events(self.wid)
        return [e for e in ev if e["kind"] == DESPAWNED and e["tick"] == tick]


def caps1_config_check(binary: str) -> None:
    s = rs.Session(binary)
    try:
        for cfg in ({"max_struct_per_tick": CAP - 1, "max_events_per_tick": CAP},
                    {"max_struct_per_tick": CAP, "max_events_per_tick": CAP - 1}):
            try:
                s.create_rts(CAP, **cfg)
                check(False, f"RTS world with {cfg} below capacity {CAP} must be refused")
            except rs.RpcError as e:
                check(e.name == "SU_E_RESOURCE_LIMIT", f"{cfg} refused with SU_E_RESOURCE_LIMIT ({e.name})")
        wid, _ = s.create_rts(CAP)
        check(wid.startswith("world-"), "caps equal to capacity are accepted")
    finally:
        s.close()


def caps2_kill_all(binary: str, env: dict, event_log_max: int | None, tag: str) -> None:
    s = rs.Session(binary, env)
    try:
        cfg = {} if event_log_max is None else {"event_log_max": event_log_max}
        t = Twin(s, **cfg)
        rows = [r for k in range(8) for r in duel_pair(k, True)]
        t.spawn(rows)
        deaths_tick = None
        for tick in range(1, 4):
            alive, m_alive = t.step()
            check(alive == m_alive, f"{tag}: tick {tick} alive {alive} equals the model {m_alive}")
            if alive == 0 and deaths_tick is None:
                deaths_tick = tick
        check(deaths_tick is not None, f"{tag}: all 16 units die")
        if event_log_max is None:
            dead = t.despawned_at(deaths_tick or 0)
            check(len(dead) == 16, f"{tag}: 16 SU_EVENT_DESPAWNED records in the death tick ({len(dead)})")
        else:
            ev = s.events(t.wid, 0, 256)
            check(len(ev["events"]) == min(event_log_max, 32), f"{tag}: log keeps {event_log_max} record(s)")
            check(ev["dropped"] == 32 - event_log_max, f"{tag}: dropped counts the rest ({ev['dropped']})")
            check(ev["dropped"] == t.m.dropped, f"{tag}: dropped equals the model ({t.m.dropped})")
    finally:
        s.close()


def caps2_two_waves(binary: str) -> None:
    """Spawn 16 (8 fighting, 8 idle), 8 die; spawn 8 fighting; set the idle 8 fighting; 16 die in one tick."""
    s = rs.Session(binary)
    try:
        t = Twin(s)
        first = t.spawn([r for k in range(8) for r in duel_pair(k, k < 4)])
        alive = None
        for _ in range(3):
            alive, m_alive = t.step()
            check(alive == m_alive, f"wave 1: alive {alive} equals the model {m_alive}")
        check(alive == 8, f"wave 1: 8 of 16 die ({alive})")
        t.spawn([r for k in range(8, 12) for r in duel_pair(k, True)])
        t.set_fields(first[8:], {"range_sq": 100})
        tick0 = s.call("world.inspect", {"world_id": t.wid, "limit": 1})["tick"]
        died = []
        for i in range(3):
            alive, m_alive = t.step()
            check(alive == m_alive, f"wave 2: alive {alive} equals the model {m_alive}")
            died.append(len(t.despawned_at(tick0 + i + 1)))
        check(alive == 0 and 16 in died, f"wave 2: all 16 die in one tick ({died})")
    finally:
        s.close()


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    binary = os.path.abspath(sys.argv[1])
    caps1_config_check(binary)
    caps2_kill_all(binary, {}, None, "serial")
    caps2_kill_all(binary, {}, 1, "event_log_max 1")
    caps2_kill_all(binary, {"SUTEKH_TICK_WORKERS": "2", "SUTEKH_TICK_CHUNK_MIN_ROWS": "1"}, None, "two workers")
    caps2_two_waves(binary)
    print(f"test_caps_engine: {checks} checks, {failures} failures")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
