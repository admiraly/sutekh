# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Detector tests for contract mutants M3-M20, M3-M30 and M3-M35 against the M3-02 reference model.

Each mutant is one exact text substitution in bench/rts/ref/model.py (applied through the loader in
tests/rts/ref/mutants.py, so the repository files are not changed). The detector checks below are run
against the mutated module, and the mutant is 'detected' when at least one check fails.

Not covered here, and recorded as skipped in the output (not as passing):
  M3-M18  replay is not implemented in bench/rts/ref (model.py header: replay, set_fields and spawn_batch
          are out of scope). No code to mutate.
  M3-M26  the incoming-zeroing rule of the native path (profiles/rts/native) needs the native build and
          the M3-04 differential. Not run by this Python harness.
  M3-M30 (set_fields half) and M3-M35 (set_fields half) need set_fields, which is not in the reference.

CLI:  python3 -I tests/rts/ref/m3_control_mutants.py [--json PATH]
"""
from __future__ import annotations

import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import checks  # noqa: E402
import mutants  # noqa: E402

DETECTED = "detected"
SURVIVES = "survives"
SKIPPED = "skipped"


def _m(mid, contract, description, subs, note=""):
    return {"id": mid, "contract": contract, "module": "model", "description": description,
            "subs": list(subs), "expect": DETECTED, "note": note}


# Each substitution must match exactly once in bench/rts/ref/model.py (checked by mutants._source).
MUTANTS = [
    _m("M3-M20", "M3-M20",
       "scatter applies damage from a dead sender (sender alive gate removed)",
       [("        if not alive[r]:\n            continue\n        t = cols[\"dmg_target\"][r]",
         "        t = cols[\"dmg_target\"][r]")],
       note="the literal state-clearing mutant (despawn keeps projectile fields) is masked by this gate in the "
            "model; this sub-mutant removes the gate so stale state from a dead shooter is observable"),
    _m("M3-M30a", "M3-M30 (flight domain)",
       "spawn accepts flight 0 (control domain check for flight widened to 0)",
       [('    if not _in(spec["flight"], 1, 64):', '    if not _in(spec["flight"], 0, 64):')]),
    _m("M3-M30b", "M3-M30 (system-owned fields)",
       "spawn accepts a system-owned field (idle-zero check removed)",
       [("    for name in SPAWN_IDLE_ZERO:\n        if name in spec and spec[name] != 0:",
         "    for name in ():\n        if name in spec and spec[name] != 0:")]),
    _m("M3-M35a", "M3-M35 (reload default)",
       "spawn accepts an omitted reload (reload removed from the required D1 list)",
       [('    for name in ("kind", "reload", "damage", "flight"):',
         '    for name in ("kind", "damage", "flight"):')]),
    _m("M3-M35b", "M3-M35 (formation gen rule)",
       "spawn accepts formation_gen 0 when formation_slot is set (nonzero rule dropped)",
       [("        if not _in(fgen, 1, MASK):", "        if not _in(fgen, 0, MASK):")]),
    _m("M3-M35c", "M3-M35 (formation gen NONE rule)",
       "spawn accepts a nonzero formation_gen when formation_slot is NONE",
       [('        if spec.get("formation_gen", 0) != 0:', '        if False:')]),
]

SKIPPED_ROWS = [
    {"id": "M3-M18", "contract": "M3-M18", "status": SKIPPED,
     "note": "replay is not implemented in bench/rts/ref; no code to mutate"},
    {"id": "M3-M26", "contract": "M3-M26", "status": SKIPPED,
     "note": "native dead-row incoming zeroing needs the native build and the M3-04 differential"},
]


class _Refused(Exception):
    pass


def _spec(m, **over):
    spec = {"kind": m.KIND_UNIT, "team": 0, "hp": 10, "reload": 1, "damage": 1, "flight": 1}
    spec.update(over)
    return spec


def _world_with_anchor(m):
    """A capacity-4 world whose slot 0 holds a live anchor, so a spec naming formation_slot 0 passes the Q2
    liveness rule and is judged by the rule under test."""
    w = m.World(4)
    w.spawn(_spec(m, kind=m.KIND_ANCHOR, hp=0))
    return w


def _must_refuse(m, spec: dict, label: str) -> None:
    """The control plane must refuse this spawn with a type error. Any other outcome fails the check."""
    try:
        _world_with_anchor(m).spawn(spec)
    except m.ModelError:
        return
    raise AssertionError(f"accepted but must be refused: {label}")


def _must_accept(m, spec: dict, label: str) -> None:
    try:
        _world_with_anchor(m).spawn(spec)
    except m.ModelError as err:
        raise AssertionError(f"refused but must be accepted: {label} ({err.code})")


def check_spawn_flight_domain(m, ir) -> None:
    # M3-M30: flight is 1..64 (2.2); 0 is an invalid projectile length
    _must_refuse(m, _spec(m, flight=0), "flight 0")
    _must_refuse(m, _spec(m, flight=65), "flight 65")
    _must_accept(m, _spec(m, flight=1), "flight 1")
    _must_accept(m, _spec(m, flight=64), "flight 64")


def check_spawn_system_owned_rejected(m, ir) -> None:
    # M3-M30: a system-owned field is idle-zero at spawn and must be refused when set
    for name in m.SPAWN_IDLE_ZERO:
        _must_refuse(m, _spec(m, **{name: 1}), f"system-owned {name} set to 1")


def check_spawn_required_defaults(m, ir) -> None:
    # M3-M35: reload, damage, flight and kind are required at spawn (D1); no silent default
    for name in ("kind", "reload", "damage", "flight"):
        spec = _spec(m)
        del spec[name]
        _must_refuse(m, spec, f"omitted {name}")


def check_spawn_formation_gen_rule(m, ir) -> None:
    # M3-M35 (D2): formation_gen is nonzero when formation_slot is set, and 0 when it is NONE
    base = _spec(m, formation_slot=0, formation_gen=0, formation_ox=0, formation_oy=0)
    _must_refuse(m, base, "formation_slot set with formation_gen 0")
    _must_accept(m, dict(base, formation_gen=1), "formation_slot set with formation_gen 1")
    _must_refuse(m, _spec(m, formation_gen=5), "formation_slot NONE with nonzero formation_gen")


def check_scatter_dead_sender_gated(m, ir) -> None:
    # M3-M20: after the shooter despawns, its stale projectile fields must not deliver damage
    w = m.World(3)
    target = w.spawn(_spec(m, hp=10))
    shooter = w.spawn(_spec(m, hp=10))
    w.despawn(shooter)
    # simulate state that a clearing bug would leave on the dead row
    w.cols["dmg_out"][1] = 40
    w.cols["dmg_target"][1] = 0
    w.cols["dmg_target_gen"][1] = w.gen[0]
    m.scatter_phase(w)
    assert w.cols["incoming"][0] == 0, "damage applied from a dead shooter"
    assert target == w.handle(0)


ALL_DETECTORS = (
    check_spawn_flight_domain,
    check_spawn_system_owned_rejected,
    check_spawn_required_defaults,
    check_spawn_formation_gen_rule,
    check_scatter_dead_sender_gated,
)


def _run_detectors(mutant=None) -> dict:
    results = {}
    with mutants.loaded(mutant):
        b = checks.bundle()
        for fn in ALL_DETECTORS:
            try:
                fn(b.model, b.ir_programs)
                results[fn.__name__] = None
            except Exception as err:  # any failure, including a KeyError from a mutated path, is a kill
                results[fn.__name__] = f"{type(err).__name__}: {str(err)[:200]}"
    return results


def run_all() -> dict:
    baseline = _run_detectors(None)
    base_failing = {k: v for k, v in baseline.items() if v is not None}
    rows = []
    for mutant in MUTANTS:
        try:
            results = _run_detectors(mutant)
        except mutants.MutantError as err:
            rows.append({"id": mutant["id"], "status": "invalid", "ok": False, "error": str(err)})
            continue
        failing = sorted(k for k, v in results.items() if v is not None)
        status = DETECTED if failing else SURVIVES
        rows.append({"id": mutant["id"], "contract": mutant["contract"], "description": mutant["description"],
                     "status": status, "ok": status == mutant["expect"], "failing_checks": failing,
                     "note": mutant["note"]})
    rows += SKIPPED_ROWS
    return {
        "baseline_ok": not base_failing,
        "baseline_failing": base_failing,
        "rows": rows,
        "detected": sum(1 for r in rows if r["status"] == DETECTED),
        "survived": sum(1 for r in rows if r["status"] == SURVIVES),
        "skipped": sum(1 for r in rows if r["status"] == SKIPPED),
        "mismatches": [r["id"] for r in rows if r["status"] in (SURVIVES, "invalid") and not r.get("ok", True)],
    }


def main(argv) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--json", help="write the full result to this path")
    args = parser.parse_args(argv)
    result = run_all()
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
            fh.write("\n")
    for row in result["rows"]:
        print(f"{row['id']:<10} {row['status']:<9} {'ok' if row.get('ok', True) else 'MISMATCH'}  "
              f"{','.join(row.get('failing_checks', [])) or row.get('note', row.get('error', ''))}")
    print(f"baseline_ok={result['baseline_ok']} detected={result['detected']} survived={result['survived']} "
          f"skipped={result['skipped']} mismatches={result['mismatches']}")
    return 0 if result["baseline_ok"] and not result["mismatches"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
