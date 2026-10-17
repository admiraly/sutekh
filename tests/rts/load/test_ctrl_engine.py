# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""T-RTS-CTRL-1 on the full 39-field RTS world through a real `sutekh serve` (contract M3 10.3, 2.2, D1, D2).

Every case is sent to the engine and applied to the Python reference model (bench/rts/ref/model.py spawn rules,
bench/rts/replay/rts_replay.py set_fields rules); the engine's status must equal the model's, and a refused call
must leave the engine's revision and alive count unchanged. Cases: every 2.2 domain at boundary + 1 (and 0 where 0
is outside) in world.spawn, world.spawn_batch and world.set_fields; the D1 spawn defaults; the D2 resulting-row and
formation-generation rules; writable set; unknown field; duplicate and stale entities; stale revision; and the
1,024-entity maximum set_fields call (14 writable fields at maximum values, 20-digit handles).

Usage: python3 -I tests/rts/load/test_ctrl_engine.py <path to sutekh>
"""
from __future__ import annotations

import copy
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "bench", "rts", "replay"))

import rts_session as rs  # noqa: E402
from rts_session import model  # noqa: E402

sys.path.insert(0, os.path.join(rs.REPO, "bench", "rts", "replay"))
import rts_replay  # noqa: E402

NONE = rs.NONE
CAP = 1100

checks = 0
failures = 0


def check(cond: bool, what: str) -> None:
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def unit(**over) -> dict:
    v = {"kind": 1, "hp": 10, "reload": 5, "damage": 3, "flight": 2}
    v.update(over)
    return v


class Twin:
    def __init__(self, s: rs.Session):
        self.s = s
        self.wid, self.rev = s.create_rts(CAP)
        self.m = model.World(CAP, model.EVENT_LOG_MAX_DEFAULT)

    def state(self) -> tuple:
        ins = self.s.call("world.inspect", {"world_id": self.wid, "limit": 1})
        return self.rev, ins["world_revision"], ins["entities"]["alive"]

    def _send(self, method: str, params: dict) -> str:
        p = {"world_id": self.wid, "expected_world_revision": self.rev}
        p.update(params)
        resp = self.s.raw(self.s.line(method, p))
        if "error" in resp:
            return resp["error"]["code"]
        self.rev = resp["result"]["world_revision"]
        self.last = resp["result"]
        return "SU_OK"

    def _model(self, fn) -> str:
        trial = copy.deepcopy(self.m)
        try:
            fn(trial)
        except model.ModelError as e:
            return e.code
        self.m = trial
        return "SU_OK"

    def case(self, what: str, method: str, params: dict, want: str | None = None) -> str:
        if method == "world.spawn":
            expect = self._model(lambda w: w.spawn(dict(params["values"])))
        elif method == "world.spawn_batch":
            def batch(w):
                specs = [model.validate_spawn(dict(r["values"]), w.cap) for r in params["rows"]]
                for spec in specs:  # Q2 against the state before the batch, as rts_replay does
                    w.check_formation_live(spec["formation_slot"])
                if len(specs) > w.cap - sum(w.alive):
                    raise model.ModelError("SU_E_RESOURCE_LIMIT", "world")
                for r in params["rows"]:
                    w.spawn(dict(r["values"]))
            expect = self._model(batch)
        else:
            expect = self._model(lambda w: rts_replay._set_fields(w, copy.deepcopy(params)))
        before = self.state()
        got = self._send(method, params)
        check(got == expect, f"{what}: engine {got}, model {expect}")
        if want is not None:
            check(got == want, f"{what}: want {want}, got {got}")
        if got != "SU_OK":
            check(self.state() == before, f"{what}: refusal changes nothing")
        return got


def spawn_cases(t: Twin) -> None:
    domain_bad = [("flight", 0), ("flight", 65), ("reload", 0), ("reload", 1025), ("damage", 0), ("damage", 65536),
                  ("order_mode", 3), ("pos_x", 32768), ("pos_y", 32768), ("order_x", 32768), ("order_y", 32768),
                  ("speed", 1025), ("range_sq", 262145), ("team", 2), ("hp", 0), ("hp", 65536), ("kind", 0),
                  ("kind", 3)]
    for name, v in domain_bad:
        for method in ("world.spawn", "world.spawn_batch"):
            params = {"values": unit(**{name: v})} if method == "world.spawn" else {"rows": [{"values": unit()},
                                                                                             {"values": unit(**{name: v})}]}
            t.case(f"{method} {name}={v}", method, params, "SU_E_TYPE")
    # Anchors: kind 2 needs hp 0 and range_sq 0.
    t.case("kind 2 with hp", "world.spawn", {"values": unit(kind=2, hp=1)}, "SU_E_TYPE")
    t.case("kind 2 with range_sq", "world.spawn", {"values": unit(kind=2, hp=0, range_sq=1)}, "SU_E_TYPE")
    t.case("kind 2 anchor", "world.spawn", {"values": unit(kind=2, hp=0)}, "SU_OK")
    live = t.last["slot"]  # a live row that formation_slot may name (Q2), so the cases below test their own rule
    # A stale expected_world_revision refuses every mutating method with SU_E_STALE_REVISION (M3-M19).
    stale = t.wid + ".r0"
    for method, params in (("world.spawn", {"values": unit()}), ("world.spawn_batch", {"rows": [{"values": unit()}]}),
                           ("world.despawn", {"entity": t.last["entity"]})):
        before = t.state()
        resp = t.s.raw(t.s.line(method, dict(params, world_id=t.wid, expected_world_revision=stale)))
        check(resp.get("error", {}).get("code") == "SU_E_STALE_REVISION", f"{method} stale revision refused: {resp}")
        check(t.state() == before, f"{method} stale revision changes nothing")
    # D1 spawn defaults.
    for name in ("reload", "damage", "flight", "kind"):
        v = unit()
        del v[name]
        t.case(f"spawn omitting {name}", "world.spawn", {"values": v}, "SU_E_TYPE")
    t.case("live formation_slot without formation_ox", "world.spawn",
           {"values": unit(formation_slot=live, formation_gen=1, formation_oy=5)}, "SU_E_TYPE")
    t.case("live formation_slot without formation_oy", "world.spawn",
           {"values": unit(formation_slot=live, formation_gen=1, formation_ox=5)}, "SU_E_TYPE")
    t.case("live formation_slot without formation_gen", "world.spawn",
           {"values": unit(formation_slot=live, formation_ox=5, formation_oy=5)}, "SU_E_TYPE")
    t.case("target_slot 0 named", "world.spawn", {"values": unit(target_slot=0)}, "SU_E_TYPE")
    t.case("system-owned cooldown nonzero", "world.spawn", {"values": unit(cooldown=1)}, "SU_E_TYPE")
    t.case("unknown field", "world.spawn", {"values": unit(no_such_field=1)}, "SU_E_SCHEMA_MISMATCH")
    t.case("spawn omitting formation_slot", "world.spawn", {"values": unit()}, "SU_OK")
    # A spawned row is read back with world.inspect {entity} (AGENT_RPC 15).
    got = t.s.call("world.inspect", {"world_id": t.wid, "limit": 1, "entity": t.last["entity"]})["entity"]["values"]
    slot = t.last["slot"]
    want = t.m.row(slot)
    check(got == want, f"spawned row reads back as the model's row (D1 defaults): {got} vs {want}")
    for name in ("target_slot", "dmg_target", "proj_target", "formation_slot"):
        check(got.get(name) == NONE, f"omitted {name} reads back NONE")
    # Q2: a formation_slot must name a live row (CAP - 1 is never spawned here).
    t.case("formation_slot naming a free slot", "world.spawn",
           {"values": unit(formation_slot=CAP - 1, formation_gen=1, formation_ox=5, formation_oy=6)}, "SU_E_TYPE")
    t.case("spawn_batch row naming a row of the same batch", "world.spawn_batch",
           {"rows": [{"values": unit()}, {"values": unit(formation_slot=t.m.hw, formation_gen=1, formation_ox=5,
                                                         formation_oy=6)}]}, "SU_E_TYPE")
    t.case("formed unit", "world.spawn",
           {"values": unit(formation_slot=slot, formation_gen=1, formation_ox=5, formation_oy=6)}, "SU_OK")


def set_fields_cases(t: Twin) -> None:
    u = t._send("world.spawn", {"values": unit()})
    check(u == "SU_OK", "spawn a unit for set_fields")
    h_unit = t.last["entity"]
    t.m.spawn(unit())
    a = t._send("world.spawn", {"values": unit(kind=2, hp=0)})
    check(a == "SU_OK", "spawn an anchor for set_fields")
    h_anchor = t.last["entity"]
    t.m.spawn(unit(kind=2, hp=0))

    def sf(what, ents, want):
        t.case(what, "world.set_fields", {"entities": ents}, want)

    for name, v in [("flight", 0), ("flight", 65), ("reload", 0), ("damage", 0), ("order_mode", 3),
                    ("pos_x", 32768), ("speed", 1025), ("range_sq", 262145)]:
        sf(f"set_fields {name}={v}", [{"entity": h_unit, "values": {name: v}}], "SU_E_TYPE")
    for name in ("kind", "team", "hp", "cooldown", "goal_x", "incoming", "target_slot"):
        sf(f"set_fields writes {name}", [{"entity": h_unit, "values": {name: 1}}], "SU_E_TYPE")
    sf("unknown field", [{"entity": h_unit, "values": {"no_such_field": 1}}], "SU_E_SCHEMA_MISMATCH")
    sf("duplicate entity", [{"entity": h_unit, "values": {"pos_x": 1}}, {"entity": h_unit, "values": {"pos_x": 2}}],
       "SU_E_INVALID_ARGUMENT")
    sf("stale handle among valid ones", [{"entity": h_unit, "values": {"pos_x": 1}},
                                         {"entity": h_unit + (5 << 32), "values": {"pos_x": 2}}], "SU_E_STALE_HANDLE")
    d = t._send("world.spawn", {"values": unit()})
    h_dead = t.last["entity"]
    t.m.spawn(unit())
    check(d == "SU_OK" and t._send("world.despawn", {"entity": h_dead}) == "SU_OK", "spawn and despawn a unit")
    t.m.despawn(h_dead)
    sf("despawned entity among valid ones", [{"entity": h_unit, "values": {"pos_x": 1}},
                                             {"entity": h_dead, "values": {"pos_x": 2}}], "SU_E_STALE_HANDLE")
    sf("D2 formation_gen without slot", [{"entity": h_unit, "values": {"formation_gen": 5}}], "SU_E_TYPE")
    sf("D2 range_sq on an anchor", [{"entity": h_anchor, "values": {"range_sq": 100}}], "SU_E_TYPE")
    sf("D2 slot without generation", [{"entity": h_unit, "values": {"formation_slot": 3}}], "SU_E_TYPE")
    sf("Q2 set_fields naming a dead row", [{"entity": h_unit, "values": {"formation_slot": h_dead & 0xFFFFFFFF,
                                                                          "formation_gen": 1}}], "SU_E_TYPE")
    sf("D2 slot with generation", [{"entity": h_unit, "values": {"formation_slot": 3, "formation_gen": 1}}], "SU_OK")
    resp = t.s.raw(t.s.line("world.set_fields", {"world_id": t.wid, "expected_world_revision": t.wid + ".r1",
                                                  "entities": [{"entity": h_unit, "values": {"pos_x": 1}}]}))
    check(resp.get("error", {}).get("code") == "SU_E_STALE_REVISION", f"stale expected revision (M3-M19): {resp}")


def max_set_fields(t: Twin) -> None:
    """1,024 entities x 14 writable fields at maximum values: one call, one revision bump."""
    alive = sum(t.m.alive)
    rows = [{"values": unit()} for _ in range(1024)]
    t.case("fill 1,024 units", "world.spawn_batch", {"rows": rows}, "SU_OK")
    handles = t.last["entities"]
    check(len(handles) == 1024 and sum(t.m.alive) == alive + 1024, "1,024 units spawned")
    top = {"pos_x": 32767, "pos_y": 32767, "speed": 1024, "range_sq": 262144, "reload": 1024, "damage": 65535,
           "flight": 64, "order_mode": 2, "order_x": 32767, "order_y": 32767, "formation_slot": handles[-1] & 0xFFFFFFFF,
           "formation_gen": NONE, "formation_ox": 32767, "formation_oy": 32767}
    check(len(top) == 14 and set(top) == set(rts_replay.WRITABLE_FIELDS), "the 14 writable fields of 2.2")
    ents = [{"entity": h, "values": dict(top)} for h in handles]
    line = t.s.line("world.set_fields", {"world_id": t.wid, "expected_world_revision": t.rev, "entities": ents})
    check(len(line) <= rs.REQUEST_BYTES_MAX, f"maximum set_fields request fits ({len(line)} bytes)")
    rev_before = t.rev
    t.case("maximum set_fields", "world.set_fields", {"entities": ents}, "SU_OK")
    num = lambda r: int(r.rsplit(".r", 1)[1])  # noqa: E731
    check(num(t.rev) == num(rev_before) + 1, "one revision bump for 1,024 entities")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    s = rs.Session(os.path.abspath(sys.argv[1]))
    try:
        t = Twin(s)
        spawn_cases(t)
        set_fields_cases(t)
        max_set_fields(t)
    finally:
        s.close()
    print(f"test_ctrl_engine: {checks} checks, {failures} failures")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
