# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Mutation runner for the M3-02 reference (contract section 15, plus evaluator and oracle mutants).

Each mutant is a textual substitution applied to an in-memory copy of one reference module. Every
substitution must match exactly once, or the mutant is reported as invalid. The modules are loaded
as fresh module objects (the repository files are not changed), the shared checks in checks.py run
against them, and the mutant is 'detected' when at least one check fails.

Expected outcomes are recorded per mutant:
  detected   at least one check must fail (the contract's named test, or an equivalent check)
  survives   no check may fail; used only for mutants the contract names as equivalent, so that
             the equivalence stays explicit (M3-M09b)
A mismatch between the expected and the observed outcome is a failure of the reference suite.

CLI:  python3 -I tests/rts/ref/mutants.py [--json PATH]
"""
from __future__ import annotations

import contextlib
import json
import os
import sys
import types

_HERE = os.path.dirname(os.path.abspath(__file__))
REF_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "..", "bench", "rts", "ref"))
for _p in (REF_DIR, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import checks  # noqa: E402  (path set above)

DETECTED = "detected"
SURVIVES = "survives"


def _m(mid, contract, module, description, subs, expect=DETECTED, note=""):
    return {"id": mid, "contract": contract, "module": module, "description": description,
            "subs": list(subs), "expect": expect, "note": note}


MUTANTS = [
    # ---- model.py: spatial ----
    _m("M3-M01", "M3-M01", "model", "tie-break on slot reversed (higher slot wins)",
       [("if best is None or cand < best:", "if best is None or (dist_sq, -e) < (best[0], -best[1]):")]),
    _m("M3-M02", "M3-M02", "model", "cell index off by one column",
       [("return (y >> CELL_SHIFT) * GRID + (x >> CELL_SHIFT)",
         "return (y >> CELL_SHIFT) * GRID + min(GRID - 1, (x >> CELL_SHIFT) + 1)")]),
    _m("M3-M03", "M3-M03", "model", "range check uses < instead of <=",
       [("if dist_sq <= range_sq:", "if dist_sq < range_sq:")]),
    _m("M3-M04", "M3-M04", "model", "query budget ignored (examined limit removed)",
       [("if examined == K_BUDGET:", "if False:")]),
    _m("M3-M05", "M3-M05", "model", "generation check removed from gather",
       [("world.alive[fslot] == 1 and world.gen[fslot] == fgen", "world.alive[fslot] == 1")]),
    _m("M3-M24", "M3-M24", "model",
       "gather without the alive check (contract calls this equivalent; see handoff question Q6)",
       [("world.alive[fslot] == 1 and world.gen[fslot] == fgen", "world.gen[fslot] == fgen")],
       note="contract says equivalent because despawn bumps the generation; a control command can name "
            "a dead slot's current generation (2.2 checks no liveness), so this is detectable"),
    _m("M3-M21", "M3-M21", "model", "grid indexes kind 2 (anchor) entries",
       [("if alive[s] and kind[s] == KIND_UNIT and hp[s] > 0:",
         "if alive[s] and (kind[s] == KIND_UNIT or kind[s] == KIND_ANCHOR) and hp[s] >= 0:")]),
    _m("M3-M22", "M3-M22", "model", "team filter removed from target query",
       [("if e == r or team[e] == my_team:", "if e == r:")]),
    _m("M3-M23", "M3-M23", "model", "domain-edge box clipping removed (wraps into the previous row)",
       [("xs = [c for c in (cx - d, cx + d) if 0 <= c < GRID]", "xs = [c for c in (cx - d, cx + d)]")]),
    # ---- model.py: scatter, combat, damage, goal, move ----
    _m("M3-M06", "M3-M06", "model", "scatter applies to a dead target",
       [('if t < cap and alive[t] == 1 and gen[t] == cols["dmg_target_gen"][r]',
         'if t < cap and gen[t] == cols["dmg_target_gen"][r]')]),
    _m("M3-M07", "M3-M07", "model", "scatter applies to a non-unit (kind 2) target",
       [('and cols["kind"][t] == KIND_UNIT:', ":")]),
    _m("M3-M08", "M3-M08", "model", "scatter uses wrapping add instead of saturating add",
       [('cols["incoming"][t] = sat_add(cols["incoming"][t], amount)',
         'cols["incoming"][t] = (cols["incoming"][t] + amount) & MASK')]),
    _m("M3-M09", "M3-M09", "model", "scatter applies only the first shooter per target per tick",
       [("    applied = 0\n    for r in range(cap):", "    applied = 0\n    seen = set()\n    for r in range(cap):"),
        ('            cols["incoming"][t] = sat_add(cols["incoming"][t], amount)\n            applied += 1',
         '            if t not in seen:\n                seen.add(t)\n                cols["incoming"][t] = sat_add(cols["incoming"][t], amount)\n                applied += 1')]),
    _m("M3-M09b", "M3-M09b", "model", "scatter iterates in descending slot order",
       [("    applied = 0\n    for r in range(cap):", "    applied = 0\n    for r in reversed(range(cap)):")],
       expect=SURVIVES, note="equivalent under saturating add (4.3): not detectable, recorded as equivalent"),
    _m("M3-M10", "M3-M10", "model", "arrival off by one (proj_left == 2 instead of 1)",
       [('arrive = row["proj_active"] == 1 and row["proj_left"] == 1',
         'arrive = row["proj_active"] == 1 and row["proj_left"] == 2')]),
    _m("M3-M11", "M3-M11", "model", "reload not reduced by one on fire (period off by one)",
       [('cooldown = row["reload"] - 1', 'cooldown = row["reload"]')]),
    _m("M3-M12", "M3-M12", "model", "incoming not zeroed by combat",
       [('"incoming": 0,', '"incoming": row["incoming"],')]),
    _m("M3-M25", "M3-M25", "model", "dmg_out set on non-arrival ticks",
       [('"dmg_out": row["proj_damage"] if arrive else 0,', '"dmg_out": row["proj_damage"],')]),
    _m("M3-M13", "M3-M13", "model", "damage kill emitted for hp 0 (anchor killed)",
       [('kill = hp2 == 0 and row["hp"] > 0', "kill = hp2 == 0")]),
    _m("M3-M14", "M3-M14", "model", "goal clamp removed for wrapped negatives",
       [("    if v < 0:\n        return 0", "    if v < 0:\n        return v")]),
    _m("M3-M15", "M3-M15", "model", "move overshoots goal (no min with speed)",
       [('return pos + min(goal - pos, row["speed"])', 'return pos + row["speed"]')]),
    _m("M3-M16", "M3-M16", "model", "move ignores the mode-2 engagement stop",
       [('moving = row["order_mode"] == ORDER_MOVE or row["target_valid"] != 1', "moving = True")]),
    _m("M3-M05b", "M3-M05 (generation)", "model", "despawn does not bump the generation (M2 rule)",
       [("self.gen[slot] = (self.gen[slot] + 1) & MASK", "self.gen[slot] = self.gen[slot]")]),
    # ---- ir_programs.py (typed from section 5; mutations of the IR form) ----
    _m("IR-M3-M11", "M3-M11", "ir_programs", "IR combat: cooldown set to reload (period off by one)",
       [('b.sub(b.load("reload"), one),', 'b.load("reload"),')]),
    _m("IR-M3-M12", "M3-M12", "ir_programs", "IR combat: incoming store removed",
       [("    b.store(\"incoming\", zero)  # incoming' = 0", "    pass")]),
    _m("IR-M3-M13", "M3-M13", "ir_programs", "IR damage: kill without the hp > 0 check",
       [('kill_flag = b.sel(b.eq(hp2, zero), b.sel(b.lt(zero, b.load("hp")), one, zero), zero)',
         'kill_flag = b.sel(b.eq(hp2, zero), one, zero)')]),
    _m("IR-M3-M14", "M3-M14", "ir_programs", "IR goal: low clamp removed",
       [("return b.sel(b.lt(big, raw), zero, b.sel(b.lt(top, raw), top, raw))",
         "return b.sel(b.lt(top, raw), top, raw)")]),
    _m("IR-M3-M15", "M3-M15", "ir_programs", "IR move: step not capped at speed",
       [("        m = b.sel(b.lt(ax, s), ax, s)", "        m = s")]),
    _m("IR-M3-M16", "M3-M16", "ir_programs", "IR move: mode-2 engagement stop ignored",
       [('mv_flag = b.sel(mode_move, one, b.sel(has_tgt, zero, one))', "mv_flag = one")]),
    # ---- ir_eval.py (evaluator) ----
    _m("EVAL-select", "evaluator", "ir_eval", "evaluator select returns the false branch when true",
       [('regs[ins["dst"]] = regs[ins["on_true"]] if regs[ins["cond"]] else regs[ins["on_false"]]',
         'regs[ins["dst"]] = regs[ins["on_false"]] if regs[ins["cond"]] else regs[ins["on_true"]]')]),
    _m("EVAL-lt", "evaluator", "ir_eval", "evaluator lt_u32 is <= instead of <",
       [('regs[ins["dst"]] = regs[ins["a"]] < regs[ins["b"]]', 'regs[ins["dst"]] = regs[ins["a"]] <= regs[ins["b"]]')]),
    # ---- oracle.py and digest.py ----
    _m("ORACLE-range", "oracle", "oracle", "oracle range test uses < (misses dist == range_sq)",
       [("        if d <= range_sq:", "        if d < range_sq:")]),
    _m("DIGEST-dropped", "9.2", "digest", "state digest omits the dropped count",
       [("world.event_next_seq, world.dropped, MODEL_LAYOUT_GENERATION)",
         "world.event_next_seq, 0, MODEL_LAYOUT_GENERATION)")]),
]


class MutantError(Exception):
    pass


def _path(name: str) -> str:
    return os.path.join(REF_DIR, name + ".py")


def _source(name: str, subs) -> str:
    with open(_path(name), encoding="utf-8") as fh:
        src = fh.read()
    for old, new in subs:
        count = src.count(old)
        if count != 1:
            raise MutantError(f"{name}: substitution matched {count} times: {old[:60]!r}")
        src = src.replace(old, new)
    return src


@contextlib.contextmanager
def loaded(mutant=None):
    """Load every reference module fresh, with the mutant applied to its module (if any)."""
    saved = {name: sys.modules.get(name) for name in checks.MODULE_NAMES}
    try:
        for name in checks.MODULE_NAMES:
            subs = mutant["subs"] if (mutant and mutant["module"] == name) else []
            src = _source(name, subs)
            mod = types.ModuleType(name)
            mod.__file__ = _path(name)
            sys.modules[name] = mod
            exec(compile(src, _path(name), "exec"), mod.__dict__)
        yield
    finally:
        for name, prev in saved.items():
            if prev is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = prev


def run_checks(mutant=None) -> dict:
    """Run every shared check against the (optionally mutated) modules. Returns name -> None or message."""
    results = {}
    with loaded(mutant):
        b = checks.bundle()
        for fn in checks.ALL_CHECKS:
            try:
                fn(b.model, b.ir_eval)
                results[fn.__name__] = None
            except Exception as err:  # a mutant may fail any check in any way
                results[fn.__name__] = f"{type(err).__name__}: {str(err)[:200]}"
    return results


def run_mutant(mutant: dict) -> dict:
    try:
        results = run_checks(mutant)
    except MutantError as err:
        return {"id": mutant["id"], "contract": mutant["contract"], "module": mutant["module"],
                "status": "invalid", "expected": mutant["expect"], "ok": False, "error": str(err)}
    failing = {k: v for k, v in results.items() if v is not None}
    observed = DETECTED if failing else SURVIVES
    return {"id": mutant["id"], "contract": mutant["contract"], "module": mutant["module"],
            "description": mutant["description"], "expected": mutant["expect"], "status": observed,
            "ok": observed == mutant["expect"], "failing_checks": sorted(failing),
            "note": mutant["note"]}


def run_baseline() -> dict:
    results = run_checks(None)
    return {"failing": {k: v for k, v in results.items() if v is not None}, "ok": all(v is None for v in results.values())}


def run_all() -> dict:
    baseline = run_baseline()
    rows = [run_mutant(m) for m in MUTANTS]
    detected = sum(1 for r in rows if r["status"] == DETECTED)
    survived = sum(1 for r in rows if r["status"] == SURVIVES)
    return {
        "baseline_ok": baseline["ok"],
        "baseline_failing": baseline["failing"],
        "total": len(rows),
        "detected": detected,
        "survived": survived,
        "invalid": sum(1 for r in rows if r["status"] == "invalid"),
        "mismatches": [r["id"] for r in rows if not r["ok"]],
        "rows": rows,
    }


def main(argv) -> int:
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--json", help="write the full result to this path")
    args = parser.parse_args(argv)
    result = run_all()
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
            fh.write("\n")
    for row in result["rows"]:
        print(f"{row['id']:<12} {row['status']:<9} expected={row['expected']:<9} "
              f"{'ok' if row['ok'] else 'MISMATCH'}  {','.join(row.get('failing_checks', [])) or row.get('error', '')}")
    print(f"baseline_ok={result['baseline_ok']} total={result['total']} detected={result['detected']} "
          f"survived={result['survived']} invalid={result['invalid']} mismatches={result['mismatches']}")
    good = result["baseline_ok"] and not result["mismatches"]
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
