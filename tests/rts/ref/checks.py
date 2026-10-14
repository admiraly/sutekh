# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Shared M3-02 checks, written against a module bundle so the mutant runner can run the same
checks on mutated copies of the reference (tests/rts/ref/mutants.py).

Every expected value in this file is derived by hand from planning/contracts/M3_RTS_CONTRACT.md
(r4), with the derivation in a comment. A check raises AssertionError on failure. No check reads
engine code. ALL_CHECKS lists the checks the mutant runner uses; each is fast.
"""
from __future__ import annotations

import importlib
import random
import types

MODULE_NAMES = ("isqrt", "prng", "model", "digest", "ir_programs", "ir_eval", "oracle", "probe_scene")


def bundle() -> types.SimpleNamespace:
    """The modules currently importable under their plain names (the real ones unless a mutant is loaded)."""
    return types.SimpleNamespace(**{name: importlib.import_module(name) for name in MODULE_NAMES})


# ---- helpers ----------------------------------------------------------------------------------

def _row(m, **kw) -> dict:
    """A full 39-field row with the spawn idle values; kw overrides fields."""
    row = {name: 0 for name in m.FIELD_NAMES}
    row.update(target_slot=m.NONE, dmg_target=m.NONE, proj_target=m.NONE, formation_slot=m.NONE,
               reload=1, damage=1, flight=1, kind=1, hp=1, range_sq=0)
    row.update(kw)
    return row


def _expect(out: dict, expected: dict, label: str) -> None:
    for key, value in expected.items():
        assert key in out, f"{label}: missing output {key}"
        assert out[key] == value, f"{label}: {key} = {out[key]!r}, expected {value!r}"


def _unit(m, x, y, team=0, range_sq=0, hp=10, kind=1, reload=1, damage=1, flight=1, **extra) -> dict:
    spec = {"kind": kind, "team": team, "hp": hp, "pos_x": x, "pos_y": y, "range_sq": range_sq,
            "reload": reload, "damage": damage, "flight": flight}
    if kind == 2:
        spec["hp"] = 0
        spec["range_sq"] = 0
    spec.update(extra)
    return spec


def _world(m, specs, capacity=None, event_log_max=65536):
    w = m.World(capacity or max(1, len(specs)), event_log_max=event_log_max)
    handles = [w.spawn(spec) for spec in specs]
    return w, handles


def _phases(m, w, upto: str, systems=None) -> None:
    """Run tick phases in order up to and including `upto` (spatial, goal, combat, move, scatter, damage)."""
    systems = m.MODEL_SYSTEMS if systems is None else systems
    m.spatial_phase(w)
    if upto == "spatial":
        return
    m._run_row_system(w, "goal", systems["goal"])
    if upto == "goal":
        return
    m._run_row_system(w, "combat", systems["combat"])
    if upto == "combat":
        return
    m._run_row_system(w, "move", systems["move"])
    if upto == "move":
        return
    m.scatter_phase(w)
    if upto == "scatter":
        return
    raise ValueError(upto)


def _run(m, w, ticks: int, systems=None) -> None:
    for _ in range(ticks):
        m.tick(w, systems)


def _despawn_events(w) -> int:
    return sum(1 for _, kind, _, _ in w.events if kind == "SU_EVENT_DESPAWNED")


def _ir_fns(ir):
    return ir.ir_systems()


# ---- 10.1 known answers: rts.goal --------------------------------------------------------------

GOAL_CASES = (
    ("explicit order overrides formation",
     dict(anchor_valid=1, anchor_x=1000, anchor_y=1000, formation_ox=16384 + 32, formation_oy=16384 - 16,
          order_mode=2, order_x=500, order_y=600, pos_x=7, pos_y=8),
     {"goal_x": 500, "goal_y": 600}),
    ("formation member follows anchor with offset",
     dict(anchor_valid=1, anchor_x=1000, anchor_y=2000, formation_ox=16384 + 32, formation_oy=16384 - 16,
          order_mode=0, pos_x=7, pos_y=8),
     {"goal_x": 1032, "goal_y": 1984}),
    ("invalid anchor holds at the current position",
     dict(anchor_valid=0, anchor_x=5, anchor_y=5, formation_ox=16384 + 9, formation_oy=16384 + 9,
          order_mode=0, pos_x=700, pos_y=800),
     {"goal_x": 700, "goal_y": 800}),
    ("wrapped negative clamps to 0 (x) and in-range y stays",
     dict(anchor_valid=1, anchor_x=100, anchor_y=100, formation_ox=0, formation_oy=16384 + 5,
          order_mode=0),
     {"goal_x": 0, "goal_y": 105}),
    ("over-domain clamps to 32767",
     dict(anchor_valid=1, anchor_x=32767, anchor_y=32767, formation_ox=32767, formation_oy=16384,
          order_mode=0),
     {"goal_x": 32767, "goal_y": 32767}),
    ("anchor at zero is a legitimate position (flag distinguishes it)",
     dict(anchor_valid=1, anchor_x=0, anchor_y=0, formation_ox=16384 + 3, formation_oy=16384 + 3,
          order_mode=0, pos_x=9, pos_y=9),
     {"goal_x": 3, "goal_y": 3}),
    ("zero anchor with invalid flag holds",
     dict(anchor_valid=0, anchor_x=0, anchor_y=0, formation_ox=16384 + 3, formation_oy=16384 + 3,
          order_mode=0, pos_x=9, pos_y=9),
     {"goal_x": 9, "goal_y": 9}),
)


def check_goal_known_answers(m, ir) -> None:
    fns = {"model": m.MODEL_SYSTEMS["goal"], "ir": _ir_fns(ir)["goal"]}
    for label, kw, expected in GOAL_CASES:
        for impl, fn in fns.items():
            _expect(fn(_row(m, **kw)), expected, f"goal[{impl}] {label}")


# ---- 10.1 known answers: rts.move ---------------------------------------------------------------

MOVE_CASES = (
    ("arrival exactly at goal",
     dict(pos_x=500, pos_y=500, goal_x=500, goal_y=500, speed=96), {"pos_x": 500, "pos_y": 500}),
    ("step is min(distance, speed) per axis",
     dict(pos_x=1000, pos_y=1000, goal_x=1050, goal_y=900, speed=96), {"pos_x": 1050, "pos_y": 904}),
    ("step capped at speed on a long move",
     dict(pos_x=0, pos_y=0, goal_x=200, goal_y=0, speed=96), {"pos_x": 96, "pos_y": 0}),
    ("mode 1 ignores a valid target",
     dict(order_mode=1, target_valid=1, pos_x=0, pos_y=0, goal_x=200, goal_y=0, speed=96),
     {"pos_x": 96, "pos_y": 0}),
    ("mode 2 stops when a target is valid",
     dict(order_mode=2, target_valid=1, pos_x=0, pos_y=0, goal_x=200, goal_y=0, speed=96),
     {"pos_x": 0, "pos_y": 0}),
    ("mode 0 stops when a target is valid",
     dict(order_mode=0, target_valid=1, pos_x=0, pos_y=0, goal_x=200, goal_y=0, speed=96),
     {"pos_x": 0, "pos_y": 0}),
    ("mode 0 without target moves",
     dict(order_mode=0, target_valid=0, pos_x=0, pos_y=0, goal_x=200, goal_y=0, speed=96),
     {"pos_x": 96, "pos_y": 0}),
    ("speed 0 never moves",
     dict(order_mode=1, pos_x=10, pos_y=10, goal_x=30000, goal_y=30000, speed=0),
     {"pos_x": 10, "pos_y": 10}),
    ("no overshoot: a fast unit stops exactly at a near goal",
     dict(pos_x=100, pos_y=100, goal_x=103, goal_y=100, speed=1024), {"pos_x": 103, "pos_y": 100}),
)


def check_move_known_answers(m, ir) -> None:
    fns = {"model": m.MODEL_SYSTEMS["move"], "ir": _ir_fns(ir)["move"]}
    for label, kw, expected in MOVE_CASES:
        for impl, fn in fns.items():
            _expect(fn(_row(m, **kw)), expected, f"move[{impl}] {label}")


# ---- 10.1 known answers: rts.combat ------------------------------------------------------------

def _combat_driver(fn, row0: dict, ticks: int, flight: int) -> list:
    """Feed the outputs back as the next tick's inputs; return fire ticks (arrival - flight)."""
    row = dict(row0)
    arrivals = []
    for t in range(ticks):
        out = fn(row)
        if out["dmg_out"] != 0:
            arrivals.append(t)
        row.update(out)
    return [a - flight for a in arrivals]


def check_combat_known_answers(m, ir) -> None:
    fns = {"model": m.MODEL_SYSTEMS["combat"], "ir": _ir_fns(ir)["combat"]}
    armed = dict(target_valid=1, target_slot=7, target_gen=3)
    for impl, fn in fns.items():
        # fire at cooldown 0 with a target: cooldown = reload - 1, projectile armed, nothing arrives
        out = fn(_row(m, cooldown=0, reload=6, flight=2, proj_active=0, damage=3, **armed))
        _expect(out, {"cooldown": 5, "proj_active": 1, "proj_left": 2, "proj_target": 7,
                      "proj_target_gen": 3, "proj_damage": 3, "dmg_out": 0, "dmg_target": m.NONE,
                      "dmg_target_gen": 0, "incoming": 0}, f"combat[{impl}] fire")
        # no fire while a projectile is in flight (left 2 is not yet arrival)
        out = fn(_row(m, cooldown=0, reload=6, flight=2, proj_active=1, proj_left=2, proj_target=7,
                      proj_target_gen=3, proj_damage=3, **armed))
        _expect(out, {"cooldown": 0, "proj_active": 1, "proj_left": 1, "dmg_out": 0},
                f"combat[{impl}] no fire while in flight")
        # arrival: proj_left 1 at tick start; damage is emitted to the captured target generation
        out = fn(_row(m, cooldown=0, proj_active=1, proj_left=1, proj_target=7, proj_target_gen=3,
                      proj_damage=3, target_valid=0))
        _expect(out, {"proj_active": 0, "proj_left": 0, "proj_target": m.NONE, "proj_target_gen": 0,
                      "proj_damage": 0, "dmg_out": 3, "dmg_target": 7, "dmg_target_gen": 3,
                      "incoming": 0}, f"combat[{impl}] arrival")
        # cooldown never underflows
        out = fn(_row(m, cooldown=0, target_valid=0))
        _expect(out, {"cooldown": 0}, f"combat[{impl}] no underflow")
        # incoming is zeroed every tick regardless of its input
        out = fn(_row(m, incoming=77, target_valid=0))
        _expect(out, {"incoming": 0}, f"combat[{impl}] incoming zeroed")
        # flight edges: flight 1 arrives on the next tick, flight 5 arrives 5 ticks after fire
        f1 = _combat_driver(fn, _row(m, reload=10, flight=1, damage=3, **armed), 4, 1)
        assert f1 == [0], f"combat[{impl}] flight 1 fire ticks {f1}, expected [0]"
        f5 = _combat_driver(fn, _row(m, reload=10, flight=5, damage=3, **armed), 12, 5)
        assert f5 == [0], f"combat[{impl}] flight 5 fire ticks {f5}, expected [0]"
        # re-fire period = max(reload, flight + 1); three known answers from 5.2
        p1 = _combat_driver(fn, _row(m, reload=6, flight=2, damage=3, **armed), 16, 2)
        assert p1 == [0, 6, 12], f"combat[{impl}] reload 6 flight 2: {p1}"
        p2 = _combat_driver(fn, _row(m, reload=1, flight=1, damage=3, **armed), 8, 1)
        assert p2 == [0, 2, 4, 6], f"combat[{impl}] reload 1 flight 1: {p2}"
        p3 = _combat_driver(fn, _row(m, reload=2, flight=5, damage=3, **armed), 20, 5)
        assert p3 == [0, 6, 12], f"combat[{impl}] reload 2 flight 5: {p3}"


# ---- 10.1 known answers: rts.damage ------------------------------------------------------------

DAMAGE_CASES = (
    ("partial damage, no kill", dict(hp=12, incoming=3), {"hp": 9, "_despawn": 0}),
    ("exact kill emits one despawn", dict(hp=3, incoming=3), {"hp": 0, "_despawn": 1}),
    ("overkill saturates at a kill", dict(hp=5, incoming=0xFFFFFFFF), {"hp": 0, "_despawn": 1}),
    ("anchor (hp 0, incoming 0) is never killed", dict(hp=0, incoming=0, kind=2), {"hp": 0, "_despawn": 0}),
    ("below lethal", dict(hp=3, incoming=0), {"hp": 3, "_despawn": 0}),
)


def check_damage_known_answers(m, ir) -> None:
    fns = {"model": m.MODEL_SYSTEMS["damage"], "ir": _ir_fns(ir)["damage"]}
    for label, kw, expected in DAMAGE_CASES:
        for impl, fn in fns.items():
            out = fn(_row(m, **kw))
            want = dict(expected)
            if "_despawn" in want:
                assert out.get(m.DESPAWN_KEY, 0) == want.pop("_despawn"), f"damage[{impl}] {label}: despawn"
            _expect(out, want, f"damage[{impl}] {label}")


# ---- 10.1 known answers: rts.spatial -----------------------------------------------------------

def check_spatial_known_answers(m, ir) -> None:
    # tie on dist_sq: lower slot wins. Both enemies are 60 units away (dist_sq 3600).
    w, h = _world(m, [_unit(m, 100, 100, 0, range_sq=10000),
                      _unit(m, 100, 160, 1), _unit(m, 160, 100, 1)])
    m.spatial_phase(w)
    assert (w.cols["target_slot"][0], w.cols["target_dist_sq"][0], w.cols["target_valid"][0]) == (1, 3600, 1), "tie"
    # nearest by dist_sq beats a lower slot at a larger distance
    w, _ = _world(m, [_unit(m, 100, 100, 0, range_sq=10000), _unit(m, 100, 160, 1), _unit(m, 130, 100, 1)])
    m.spatial_phase(w)
    assert (w.cols["target_slot"][0], w.cols["target_dist_sq"][0]) == (2, 900), "nearest by distance"
    # range equality qualifies, range_sq - 1 does not (dist_sq 3600, range 60)
    w, _ = _world(m, [_unit(m, 100, 100, 0, range_sq=3600), _unit(m, 160, 100, 1)])
    m.spatial_phase(w)
    assert w.cols["target_valid"][0] == 1, "dist == range_sq must qualify"
    w, _ = _world(m, [_unit(m, 100, 100, 0, range_sq=3599), _unit(m, 160, 100, 1)])
    m.spatial_phase(w)
    assert w.cols["target_valid"][0] == 0, "dist == range_sq + 1 must not qualify"
    # cell boundaries at x = 63 / 64 and y = 63 / 64 and 255 / 256 (range_sq 4: rc = 1)
    for (qx, qy), (ex, ey) in (((63, 500), (64, 500)), ((64, 500), (63, 500)),
                               ((500, 255), (500, 256)), ((500, 256), (500, 255))):
        w, _ = _world(m, [_unit(m, qx, qy, 0, range_sq=4), _unit(m, ex, ey, 1)])
        m.spatial_phase(w)
        assert w.cols["target_valid"][0] == 1 and w.cols["target_slot"][0] == 1, f"boundary {(qx, qy)}"
    # domain edges: querier at (0, 0) and at (32767, 32767)
    w, _ = _world(m, [_unit(m, 0, 0, 0, range_sq=100), _unit(m, 5, 5, 1)])
    m.spatial_phase(w)
    assert w.cols["target_valid"][0] == 1, "domain low edge"
    w, _ = _world(m, [_unit(m, 32767, 32767, 0, range_sq=100), _unit(m, 32760, 32760, 1)])
    m.spatial_phase(w)
    assert w.cols["target_valid"][0] == 1, "domain high edge"
    # self excluded: a lone unit does not target itself
    w, _ = _world(m, [_unit(m, 10, 10, 0, range_sq=100)])
    m.spatial_phase(w)
    assert w.cols["target_valid"][0] == 0, "self must be excluded"
    # same team excluded even when nearer
    w, _ = _world(m, [_unit(m, 100, 100, 0, range_sq=6400), _unit(m, 101, 100, 0), _unit(m, 150, 100, 1)])
    m.spatial_phase(w)
    assert (w.cols["target_slot"][0], w.cols["target_dist_sq"][0]) == (2, 2500), "same team excluded"
    # anchors (kind 2) are never targets
    w, _ = _world(m, [_unit(m, 100, 100, 0, range_sq=100), _unit(m, 101, 100, 1, kind=2)])
    m.spatial_phase(w)
    assert w.cols["target_valid"][0] == 0, "anchor is not a target"
    # dead excluded
    w, h = _world(m, [_unit(m, 100, 100, 0, range_sq=100), _unit(m, 101, 100, 1)])
    w.despawn(h[1])
    m.spatial_phase(w)
    assert w.cols["target_valid"][0] == 0, "dead must be excluded"
    # box clipped at the grid edge: entries wrapping from cell x = -1 must not be counted (M3-M23)
    specs = [_unit(m, 0, 64, 0, range_sq=100)] + [_unit(m, 32750, 10, 1) for _ in range(m.K_BUDGET + 2)]
    w, _ = _world(m, specs)
    m.spatial_phase(w)
    assert w.cols["truncated"][0] == 0, "domain-edge clipping: far cell must not count toward the budget"
    # truncation at exactly the versioned K examined and at K + 1 (contract 10.1)
    base = [_unit(m, 10, 10, 0, range_sq=100)] + [_unit(m, 20, 20, 0) for _ in range(m.K_BUDGET - 2)]
    w, _ = _world(m, base + [_unit(m, 15, 10, 1)])  # box_total = K
    m.spatial_phase(w)
    assert (w.cols["truncated"][0], w.cols["target_valid"][0]) == (0, 1), "box_total K"
    w, _ = _world(m, base + [_unit(m, 15, 10, 1), _unit(m, 20, 20, 0)])  # box_total = K+1
    m.spatial_phase(w)
    assert (w.cols["truncated"][0], w.cols["target_valid"][0]) == (1, 1), "box_total K+1"
    # the exact ring fixture (10.1): ring-0 holds 200 friendly entries; a single enemy sits in ring 1
    specs = [_unit(m, 32, 32, 0, range_sq=65536)] + [_unit(m, 20, 20, 0) for _ in range(200)]
    specs.append(_unit(m, 70, 32, 1))
    w, _ = _world(m, specs)
    m.spatial_phase(w)
    assert w.cols["target_valid"][0] == 0, "ring fixture: target_valid must be 0"
    assert w.cols["truncated"][0] == 1, "ring fixture: truncated must be 1"


# ---- 10.1 known answers: gather ----------------------------------------------------------------

def check_gather_known_answers(m, ir) -> None:
    w, h = _world(m, [_unit(m, 1000, 1000, 0, kind=2),  # anchor, slot 0, gen 1
                      _unit(m, 1200, 1000, 0, formation_slot=0, formation_gen=2,
                            formation_ox=16384, formation_oy=16384)])
    m.spatial_phase(w)
    assert (w.cols["anchor_valid"][1], w.cols["anchor_x"][1]) == (0, 0), "stale generation"

    w, h = _world(m, [_unit(m, 1000, 1000, 0, kind=2), _unit(m, 1200, 1000, 0, formation_slot=0,
                                                              formation_gen=1, formation_ox=16384,
                                                              formation_oy=16384)])
    w.despawn(h[0])
    m.spatial_phase(w)
    assert w.cols["anchor_valid"][1] == 0, "despawned anchor with the assigned generation"

    # Dead anchor at its current (post-despawn) generation. Q2 (owner decision, M3-05): the control plane
    # refuses a formation_slot that names a dead row, so this state is not reachable through spawn or
    # set_fields any more; it is built white-box, and the alive check of 4.1(a) must still refuse it at
    # operator level. Mutant M3-M24 is caught here.
    w = m.World(3)
    h0 = w.spawn(_unit(m, 100, 100, 0))
    h1 = w.spawn(_unit(m, 1000, 1000, 0, kind=2))
    w.spawn(_unit(m, 2000, 2000, 0))
    w.despawn(h0)
    w.despawn(h1)
    try:
        w.spawn(_unit(m, 1200, 1000, 0, formation_slot=1, formation_gen=w.gen[1],
                      formation_ox=16384, formation_oy=16384))
        raise AssertionError("Q2: spawn naming a dead formation_slot must be refused")
    except m.ModelError as e:
        assert (e.code, e.path) == ("SU_E_TYPE", "formation_slot"), "Q2 refusal code"
    member = w.spawn(_unit(m, 1200, 1000, 0))
    assert member & 0xFFFFFFFF == 0, "member takes the lowest free slot"
    w.cols["formation_slot"][0] = 1  # white-box: dead slot 1 at the generation it now carries
    w.cols["formation_gen"][0] = w.gen[1]
    w.cols["formation_ox"][0] = w.cols["formation_oy"][0] = 16384
    m.spatial_phase(w)
    assert w.cols["anchor_valid"][0] == 0 and w.cols["anchor_x"][0] == 0, \
        "dead slot at its current generation must not gather"

    w, h = _world(m, [_unit(m, 1000, 1000, 0, kind=2), _unit(m, 1200, 1000, 0)])
    w.cols["formation_slot"][1] = 99  # out of range (white-box: the control plane refuses it)
    w.cols["formation_gen"][1] = 1
    m.spatial_phase(w)
    assert w.cols["anchor_valid"][1] == 0, "out of range slot"

    w, h = _world(m, [_unit(m, 1200, 1000, 0)])
    m.spatial_phase(w)
    assert w.cols["anchor_valid"][0] == 0, "NONE slot"

    # zero-valued legitimate anchor position is valid and distinguishable by the flag
    w, _ = _world(m, [_unit(m, 0, 0, 0, kind=2), _unit(m, 500, 500, 0, formation_slot=0, formation_gen=1,
                                                       formation_ox=16384, formation_oy=16384)])
    m.spatial_phase(w)
    assert (w.cols["anchor_valid"][1], w.cols["anchor_x"][1], w.cols["anchor_y"][1]) == (1, 0, 0), "zero anchor"


# ---- 10.1 known answers: scatter (operator level) ------------------------------------------------

def _shooter(w, slot, out, target, target_gen):
    w.cols["dmg_out"][slot] = out
    w.cols["dmg_target"][slot] = target
    w.cols["dmg_target_gen"][slot] = target_gen


def check_scatter_known_answers(m, ir) -> None:
    # saturation: two arrivals of 0xFFFFFFF0 give 0xFFFFFFFF (a wrapping add would give 0xFFFFFFE0)
    w, _ = _world(m, [_unit(m, 0, 0, 1, hp=65535), _unit(m, 5, 5, 0), _unit(m, 6, 6, 0)])
    _shooter(w, 1, 0xFFFFFFF0, 0, 1)
    _shooter(w, 2, 0xFFFFFFF0, 0, 1)
    m.scatter_phase(w)
    assert w.cols["incoming"][0] == 0xFFFFFFFF, "saturating add"
    # multiple shooters on one target are summed
    w, _ = _world(m, [_unit(m, 0, 0, 1, hp=65535), _unit(m, 5, 5, 0), _unit(m, 6, 6, 0)])
    _shooter(w, 1, 5, 0, 1)
    _shooter(w, 2, 7, 0, 1)
    m.scatter_phase(w)
    assert w.cols["incoming"][0] == 12, "multiple shooters summed"
    # a saturated lethal case kills exactly once
    w, _ = _world(m, [_unit(m, 0, 0, 1, hp=5), _unit(m, 5, 5, 0), _unit(m, 6, 6, 0)])
    _shooter(w, 1, 0xFFFFFFF0, 0, 1)
    _shooter(w, 2, 0xFFFFFFF0, 0, 1)
    m.scatter_phase(w)
    first = m.damage_row(w.row(0))
    assert first[m.DESPAWN_KEY] == 1 and first["hp"] == 0, "saturated lethal kills"
    assert m.damage_row(dict(w.row(0), hp=0))[m.DESPAWN_KEY] == 0, "no second kill"
    # dead target ignored even when the generation matches the slot's current generation. The boundary
    # zeroes dead rows, so kind is 0 there and the kind check alone would refuse it. The operator's own
    # alive gate (4.3) is tested with a white-box row that still says kind 1 after death.
    w, h = _world(m, [_unit(m, 0, 0, 1), _unit(m, 5, 5, 0)])
    w.despawn(h[0])
    w.cols["kind"][0] = 1
    _shooter(w, 1, 40, 0, w.gen[0])
    m.scatter_phase(w)
    assert w.cols["incoming"][0] == 0, "dead target (operator-level alive gate)"
    # stale generation ignored
    w, _ = _world(m, [_unit(m, 0, 0, 1), _unit(m, 5, 5, 0)])
    _shooter(w, 1, 40, 0, 0)
    m.scatter_phase(w)
    assert w.cols["incoming"][0] == 0, "stale generation"
    # non-unit target ignored
    w, _ = _world(m, [_unit(m, 0, 0, 1, kind=2), _unit(m, 5, 5, 0)])
    _shooter(w, 1, 40, 0, 1)
    m.scatter_phase(w)
    assert w.cols["incoming"][0] == 0, "non-unit target"


# ---- 10.2 edge fixtures through the full tick ---------------------------------------------------

def check_duel_fixtures(m, ir) -> None:
    # two units exactly range_sq apart: the shooter fires at tick 0, the hit lands at tick 1
    w, _ = _world(m, [_unit(m, 100, 100, 0, range_sq=3600, damage=3, reload=6, flight=1),
                      _unit(m, 160, 100, 1, hp=10)])
    m.tick(w)
    assert w.cols["hp"][1] == 10, "tick 0: projectile in flight"
    m.tick(w)
    assert w.cols["hp"][1] == 7, "tick 1: hit lands at distance == range_sq"

    # bow flight 5 against a target that dies in flight: the late shot finds no live target
    w, _ = _world(m, [_unit(m, 1000, 1000, 0, range_sq=262144, damage=5, reload=10, flight=5),
                      _unit(m, 1100, 1000, 1, hp=5),
                      _unit(m, 1100, 1100, 0, range_sq=65536, damage=10, reload=6, flight=1)])
    m.tick(w)
    m.tick(w)
    assert w.alive[1] == 0 and _despawn_events(w) == 1, "target dies to the rifle at tick 1"
    _run(m, w, 6)
    assert _despawn_events(w) == 1 and w.alive[0] == 1, "late bow shot causes no second death"

    # target hp 3 hit by two shooters in one tick: incoming 6 at scatter, killed once by damage
    def two_shooters():
        return _world(m, [_unit(m, 560, 500, 0, range_sq=65536, damage=3, reload=6, flight=1),
                          _unit(m, 500, 560, 0, range_sq=65536, damage=3, reload=6, flight=1),
                          _unit(m, 500, 500, 1, hp=3)])[0]

    w = two_shooters()
    m.tick(w)                # tick 0: both shooters fire
    _phases(m, w, "scatter")  # tick 1 up to scatter: both shots arrive
    assert w.cols["incoming"][2] == 6, "incoming sums to 6"
    w = two_shooters()
    m.tick(w)
    m.tick(w)                 # tick 1 completes: target killed once
    assert w.alive[2] == 0 and _despawn_events(w) == 1, "two shooters kill a 3 hp target once"

    # formation anchor despawned mid-run: the member holds afterwards
    w, h = _world(m, [_unit(m, 1000, 1000, 0, kind=2, speed=96),
                      _unit(m, 1200, 1000, 0, speed=96, formation_slot=0, formation_gen=1,
                            formation_ox=16384, formation_oy=16384)])
    m.tick(w)
    assert w.cols["pos_x"][1] == 1104, "member follows its anchor (96 per tick)"
    w.despawn(h[0])
    m.tick(w)
    pos_after = w.cols["pos_x"][1]
    assert w.cols["anchor_valid"][1] == 0 and pos_after == 1104, "member holds after anchor death"
    m.tick(w)
    assert w.cols["pos_x"][1] == 1104, "member still holds"

    # slot reuse: a stale projectile must not hit the new occupant of the slot
    w, h = _world(m, [_unit(m, 1000, 1000, 0, range_sq=262144, damage=5, reload=10, flight=5),
                      _unit(m, 1100, 1000, 1, hp=5),
                      _unit(m, 1100, 1100, 0, range_sq=65536, damage=10, reload=6, flight=1)])
    m.tick(w)
    m.tick(w)  # rifle arrival kills the target; its slot is free
    assert w.alive[1] == 0 and w.gen[1] == 2, "target slot freed with generation 2"
    new = w.spawn(_unit(m, 1100, 1000, 1, hp=5))
    assert new & 0xFFFFFFFF == 1 and (new >> 32) == 2, "new occupant takes slot 1 at generation 2"
    _run(m, w, 4)  # bow arrival at tick 5
    assert w.cols["hp"][1] == 5 and w.alive[1] == 1, "stale shot must not hit the new occupant"


# ---- IR evaluator static checks (10.4 subset: the evaluator's own checks) -----------------------

def check_ir_programs_validate(m, ir) -> None:
    for name, prog in ir_programs_all(ir).items():
        info = ir.check_program(prog)
        assert info["despawn_count"] == (1 if name == "damage" else 0), name


def ir_programs_all(ir):
    import ir_programs  # resolved through sys.modules, so a mutant bundle is honoured
    return ir_programs.all_programs()


def _mutated_prog(ir, name: str, edit) -> str:
    prog = ir_programs_all(ir)[name]
    edit(prog)
    try:
        ir.check_program(prog)
    except ir.IRError as err:
        return err.code
    return "accepted"


def check_ir_negatives(m, ir) -> None:
    def drop_store(prog):
        prog["instructions"] = [i for i in prog["instructions"] if not (i["op"] == "store_u32" and i["field"] == "incoming")]
    assert _mutated_prog(ir, "combat", drop_store) == "SU_E_MISSING_STORE", "missing store"

    def undeclared_write(prog):
        prog["fields"]["hp"]["write"] = False
    assert _mutated_prog(ir, "damage", undeclared_write) == "SU_E_UNDECLARED_WRITE", "undeclared write"

    def despawn_in_01(prog):
        prog["ir_version"] = "0.1"
    assert _mutated_prog(ir, "damage", despawn_in_01) == "SU_E_UNKNOWN_OPCODE", "0.2 op in a 0.1 module"

    def raw_u32_cond(prog):
        sel = next(i for i in prog["instructions"] if i["op"] == "select_u32")
        const = next(i for i in prog["instructions"] if i["op"] == "const_u32")
        sel["cond"] = const["dst"]
    assert _mutated_prog(ir, "goal", raw_u32_cond) == "SU_E_TYPE", "raw u32 as select condition (N2)"

    def undefined_reg(prog):
        sel = next(i for i in prog["instructions"] if i["op"] == "select_u32")
        sel["on_true"] = "no_such_register"
    assert _mutated_prog(ir, "move", undefined_reg) == "SU_E_UNDEFINED_REGISTER", "undefined register"

    def duplicate_reg(prog):
        prog["instructions"].insert(0, dict(prog["instructions"][0]))
    assert _mutated_prog(ir, "goal", duplicate_reg) == "SU_E_DUPLICATE_REGISTER", "duplicate register"

    def over_struct(prog):
        prog["instructions"].append({"op": "despawn_self", "cond": next(
            i["dst"] for i in prog["instructions"] if i["op"] == "eq_u32")})
    assert _mutated_prog(ir, "damage", over_struct) == "SU_E_RESOURCE_LIMIT", "despawn above max_struct_per_row"

    def undeclared_read(prog):
        prog["fields"]["incoming"]["read"] = False
    assert _mutated_prog(ir, "damage", undeclared_read) == "SU_E_UNDECLARED_READ", "undeclared read"


# ---- IR versus model: random rows and whole ticks (section 8 differential) ----------------------

def _rand_u32(rng: random.Random) -> int:
    return rng.getrandbits(32)


def _rand_domain(rng: random.Random, lo: int, hi: int) -> int:
    pick = rng.random()
    if pick < 0.15:
        return lo
    if pick < 0.30:
        return hi
    return rng.randint(lo, hi)


def _random_row(m, rng: random.Random, system: str) -> dict:
    row = _row(m)
    for name in m.FIELD_NAMES:
        row[name] = rng.getrandbits(8)  # placeholder, overwritten below
    if system == "goal":
        row.update(order_mode=rng.randint(0, 2), order_x=_rand_domain(rng, 0, 32767),
                   order_y=_rand_domain(rng, 0, 32767), anchor_valid=rng.randint(0, 1),
                   anchor_x=_rand_domain(rng, 0, 32767), anchor_y=_rand_domain(rng, 0, 32767),
                   formation_ox=_rand_domain(rng, 0, 32767), formation_oy=_rand_domain(rng, 0, 32767),
                   pos_x=_rand_domain(rng, 0, 32767), pos_y=_rand_domain(rng, 0, 32767))
    elif system == "combat":
        row.update(target_valid=rng.randint(0, 1), target_slot=_rand_u32(rng), target_gen=_rand_u32(rng),
                   cooldown=rng.randint(0, 1024), reload=rng.randint(1, 1024), damage=rng.randint(1, 65535),
                   flight=rng.randint(1, 64), proj_active=rng.randint(0, 1), proj_left=rng.randint(0, 64),
                   proj_target=_rand_u32(rng), proj_target_gen=_rand_u32(rng),
                   proj_damage=rng.randint(0, 65535))
    elif system == "move":
        row.update(goal_x=_rand_domain(rng, 0, 32767), goal_y=_rand_domain(rng, 0, 32767),
                   pos_x=_rand_domain(rng, 0, 32767), pos_y=_rand_domain(rng, 0, 32767),
                   speed=rng.randint(0, 1024), order_mode=rng.randint(0, 2), target_valid=rng.randint(0, 1))
    elif system == "damage":
        row.update(hp=rng.choice([0, 1, 2, 65535, rng.randint(0, 65535)]),
                   incoming=rng.choice([0, 0xFFFFFFFF, _rand_u32(rng), rng.randint(0, 70000)]))
    return row


def check_ir_row_differential(m, ir) -> None:
    rng = random.Random(20261012)
    fns = ir.ir_systems()
    for system in ("goal", "combat", "move", "damage"):
        for i in range(1500):
            row = _random_row(m, rng, system)
            model_out = m.MODEL_SYSTEMS[system](row)
            ir_out = fns[system](row)
            assert model_out == ir_out, f"{system} row {i}: model {model_out} ir {ir_out} row {row}"


def check_ir_tick_differential(m, ir) -> None:
    for seed, n in ((1, 48), (2, 40), (3, 64)):
        import probe_scene
        wm = probe_scene.build_probe(n, seed)
        wi = probe_scene.build_probe(n, seed)
        systems = ir.ir_systems()
        for t in range(40):
            m.tick(wm)
            m.tick(wi, systems)
            assert wm.cols == wi.cols and wm.alive == wi.alive and wm.gen == wi.gen, f"seed {seed} tick {t}"
            assert wm.events == wi.events, f"seed {seed} tick {t}: events"


def check_ir_tick_differential_formation_fixture(m, ir) -> None:
    """A fixed scene that exercises formation follow, firing and a death, compared tick by tick."""
    specs = [_unit(m, 1000, 1000, 0, kind=2, speed=96),
             _unit(m, 1200, 1000, 0, speed=96, range_sq=65536, formation_slot=0, formation_gen=1,
                   formation_ox=16384, formation_oy=16384, damage=3, reload=6, flight=2),
             _unit(m, 1100, 1000, 1, hp=4, range_sq=0, damage=1)]
    wm, _ = _world(m, specs)
    wi, _ = _world(m, specs)
    systems = ir.ir_systems()
    for t in range(12):
        m.tick(wm)
        m.tick(wi, systems)
        assert wm.cols == wi.cols and wm.alive == wi.alive, f"fixture tick {t}"


# ---- digest (9.1, 9.2) ----------------------------------------------------------------------------

def check_digest_shape_and_sensitivity(m, ir) -> None:
    import digest as d
    import probe_scene
    a = probe_scene.build_probe(40, 9)
    b = probe_scene.build_probe(40, 9)
    assert d.state_digest(a) == d.state_digest(b), "same build, same digest"
    assert d.events_digest(a) == d.events_digest(b), "same events, same digest"
    b.cols["pos_x"][3] ^= 1
    assert d.state_digest(a) != d.state_digest(b), "one field bit changes the state digest"
    rec = d.digest_record(a, 0)
    assert set(rec) == {"t", "state", "events", "counts"}, "record keys"
    assert set(rec["counts"]) == {"alive", "retired", "events", "dropped", "truncated_rows"}, "count keys"
    assert rec["state"].startswith("sha256:") and len(rec["state"]) == len("sha256:") + 64, "digest format"
    assert d.canonical_json({"b": 1, "a": [2, 3]}) == '{"a":[2,3],"b":1}', "canonical JSON"
    # dropped count is part of the state (9.2): two worlds differing only in dropped differ in state
    c = probe_scene.build_probe(40, 9, event_log_max=2)
    e = probe_scene.build_probe(40, 9, event_log_max=2)
    assert d.state_digest(c) == d.state_digest(e), "identical event histories"
    e.dropped += 1
    assert d.state_digest(c) != d.state_digest(e), "dropped is in the state digest"


def check_isqrt_and_prng(m, ir) -> None:
    import math
    import isqrt as isqrt_mod
    import prng as prng_mod
    for n in list(range(0, 2000)) + [2**31 - 1, 2**31, 262144, 262143, 65535 * 65535]:
        assert isqrt_mod.isqrt(n) == math.isqrt(n), f"isqrt({n})"
    # splitmix64 (seed 0): the first 64-bit output of the published reference
    assert prng_mod.SplitMix64(0).next_u64() == 0xE220A8397B1DCDAF, "splitmix64 known answer"
    rng = prng_mod.SplitMix64(12345)
    draws = [rng.uniform(10) for _ in range(2000)]
    assert min(draws) >= 0 and max(draws) <= 9, "uniform range"


# ---- oracle (4.2) --------------------------------------------------------------------------------

def check_oracle_fixtures(m, ir) -> None:
    import oracle as o
    # exact-range enemy is found by the oracle
    w, _ = _world(m, [_unit(m, 100, 100, 0, range_sq=3600), _unit(m, 160, 100, 1)])
    assert o.oracle_best(w, 0) == (3600, 1), "oracle includes dist == range_sq"
    # ring fixture: the budgeted query misses the enemy, the oracle sees it
    specs = [_unit(m, 32, 32, 0, range_sq=65536)] + [_unit(m, 20, 20, 0) for _ in range(200)]
    specs.append(_unit(m, 70, 32, 1))
    w, _ = _world(m, specs)
    met = o.targeting_metrics(w, [0], "m3-02-fixture")
    assert (met["missed"], met["valid_but_not_nearest"], met["target_valid"]) == (1, 0, 0), met
    assert met["missed_rate"] == 1.0, met
    # valid but not nearest: ring 0 has the first K entries, the nearer enemy sits in ring 1
    specs = [_unit(m, 60, 5, 0, range_sq=4096)]
    specs += [_unit(m, 30, 30, 0) for _ in range(m.K_BUDGET - 2)]
    specs.append(_unit(m, 0, 5, 1))   # ring 0, dist_sq 3600
    specs.append(_unit(m, 64, 5, 1))  # ring 1, dist_sq 16 (the true nearest)
    w, _ = _world(m, specs)
    met = o.targeting_metrics(w, [0], "m3-02-fixture")
    assert (met["target_valid"], met["valid_but_not_nearest"], met["missed"]) == (1, 1, 0), met
    assert met["ratio_p95"] == 15.0 and met["truncated_queriers"] == 1, met


ALL_CHECKS = (
    check_isqrt_and_prng,
    check_goal_known_answers,
    check_move_known_answers,
    check_combat_known_answers,
    check_damage_known_answers,
    check_spatial_known_answers,
    check_gather_known_answers,
    check_scatter_known_answers,
    check_duel_fixtures,
    check_ir_programs_validate,
    check_ir_negatives,
    check_ir_row_differential,
    check_ir_tick_differential,
    check_ir_tick_differential_formation_fixture,
    check_digest_shape_and_sensitivity,
    check_oracle_fixtures,
)
