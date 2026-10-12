# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-03 mutation checks on the IR records (applied to copies in a scratch directory; the repository
files are never changed).

A mutant is killed when one of its detectors fires:
  structural   the contract structure checker (ir_harness.structural_problems: 3.2, 5.1-5.5)
  verify:CODE  the engine verifier refuses the projected record with CODE
  differential the engine VM disagrees with the reference evaluator on domain-valid or arbitrary-u32 rows

Named mutants (N-series) must each be killed by the detector the test names. The systematic campaign
(S-series) applies every single-site operator change to the LIR; its survivors are listed in the record and
checked against the classification in SURVIVOR_CLASS, which says why each one is equivalent.

Environment: SU_BUILD_DIR. Optional: SU_IR_WRITE_RECORDS=1 writes tests/rts/ir/records/mutants.json.
"""
from __future__ import annotations

import copy
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # works under python3 -I
import ir_harness as h  # noqa: E402

RECORD_DIR = os.path.join(h.HERE, "records")
U32 = 0xFFFFFFFF

# Survivor classification: mutant id -> reason it is equivalent (checked by reading the program, not
# by the test). Any other survivor fails test_systematic_campaign_and_counts.
SURVIVOR_CLASS = {
    "S-move-const@0-1": "equivalent: the zero constant feeds only select(has_tgt, zero, one) into mv_flag, "
                        "which is consumed as eq(mv_flag, 1); every value other than 1 gives the same bool",
    "S-damage-const@1+1": "equivalent: the one constant is both the kill flag value (select(lt(0, hp), one, zero)) "
                          "and the comparison constant in eq(kill_flag, one); changing it shifts both consistently",
}


def def_index(instructions, reg):
    for i, ins in enumerate(instructions):
        if ins.get("dst") == reg:
            return i
    raise ValueError(reg)


def first(instructions, op, pred=lambda ins: True):
    for i, ins in enumerate(instructions):
        if ins["op"] == op and pred(ins):
            return i
    raise ValueError(op)


def store_src(instructions, field):
    return instructions[first(instructions, "store_u32", lambda i: i["field"] == field)]["src"]


def named_mutants(name, cap, lir):
    """(id, description, expected detector prefix, capsule, lir) for the named N-series of one system."""
    out = []

    def lir_copy():
        return copy.deepcopy(lir)

    ins0 = lir["instructions"]
    if name == "goal":
        # N01: clamp removed for goal_x (store the raw value)
        m = lir_copy()
        outer = m["instructions"][def_index(m["instructions"], store_src(m["instructions"], "goal_x"))]
        inner = m["instructions"][def_index(m["instructions"], outer["on_false"])]
        raw = inner["on_false"]
        m["instructions"][first(m["instructions"], "store_u32", lambda i: i["field"] == "goal_x")]["src"] = raw
        out.append(("N01", "goal_x stored without clamp_domain", "differential", cap, m))
        # N02: BIAS constant zero (formation offsets read without the bias)
        m = lir_copy()
        for ins in m["instructions"]:
            if ins["op"] == "const_u32" and ins["value"] == 16384:
                ins["value"] = 0
        out.append(("N02", "BIAS constant 16384 replaced by 0", "differential", cap, m))
        # N21: clamp bound 32767 replaced by 32768
        m = lir_copy()
        for ins in m["instructions"]:
            if ins["op"] == "const_u32" and ins["value"] == 32767:
                ins["value"] = 32768
        out.append(("N21", "clamp bound 32767 replaced by 32768", "differential", cap, m))
    if name == "combat":
        # N03: incoming stored as the dmg_out value instead of 0
        m = lir_copy()
        dmg_out_src = store_src(m["instructions"], "dmg_out")
        m["instructions"][first(m["instructions"], "store_u32", lambda i: i["field"] == "incoming")]["src"] = dmg_out_src
        out.append(("N03", "incoming not zeroed (stored dmg_out value)", "differential", cap, m))
        # N04: dmg_out not gated by arrival (carries proj_damage every tick)
        m = lir_copy()
        pd = m["instructions"][first(m["instructions"], "load_u32", lambda i: i["field"] == "proj_damage")]["dst"]
        m["instructions"][first(m["instructions"], "store_u32", lambda i: i["field"] == "dmg_out")]["src"] = pd
        out.append(("N04", "dmg_out written every tick", "differential", cap, m))
        # N10: store of incoming dropped
        m = lir_copy()
        m["instructions"] = [i for i in m["instructions"] if not (i["op"] == "store_u32" and i["field"] == "incoming")]
        out.append(("N10", "incoming never stored", "verify:SU_E_MISSING_STORE", cap, m))
        # N22: fire condition on pa0 dropped (fires while a projectile is in flight)
        m = lir_copy()
        pa0_reg = None
        for ins in m["instructions"]:
            if ins["op"] == "eq_u32" and m["instructions"][def_index(m["instructions"], ins["a"])]["op"] == "load_u32" \
                    and m["instructions"][def_index(m["instructions"], ins["a"])]["field"] == "proj_active" \
                    and m["instructions"][def_index(m["instructions"], ins["b"])]["op"] == "const_u32" \
                    and m["instructions"][def_index(m["instructions"], ins["b"])]["value"] == 0:
                pa0_reg = ins["dst"]
        sel = m["instructions"][first(m["instructions"], "select_u32", lambda i: i["cond"] == pa0_reg)]
        sel["on_true"], sel["on_false"] = sel["on_false"], sel["on_true"]
        out.append(("N22", "fire allowed while a projectile is in flight", "differential", cap, m))
    if name == "move":
        # N05: min replaced by max in the step length
        m = lir_copy()
        speed = m["instructions"][first(m["instructions"], "load_u32", lambda i: i["field"] == "speed")]["dst"]
        sel = m["instructions"][first(m["instructions"], "select_u32", lambda i: i["on_false"] == speed)]
        sel["on_true"], sel["on_false"] = sel["on_false"], sel["on_true"]
        out.append(("N05", "step length is max(distance, speed)", "differential", cap, m))
        # N06: mode-2 engagement stop removed (a valid target does not stop the unit)
        m = lir_copy()
        tv = m["instructions"][first(m["instructions"], "load_u32", lambda i: i["field"] == "target_valid")]["dst"]
        eq_tv = m["instructions"][first(m["instructions"], "eq_u32", lambda i: i["a"] == tv)]["dst"]
        sel = m["instructions"][first(m["instructions"], "select_u32", lambda i: i["cond"] == eq_tv)]
        sel["on_true"], sel["on_false"] = sel["on_false"], sel["on_true"]
        out.append(("N06", "valid target does not stop a mode-0/2 unit", "differential", cap, m))
        # N11: capsule does not declare target_valid as read (the IR still loads it)
        c = copy.deepcopy(cap)
        c["fields"] = [f for f in c["fields"] if f["name"] != "target_valid"]
        out.append(("N11", "capsule drops target_valid from fields", "verify:SU_E_UNDECLARED_READ", c, lir_copy()))
        # N12: capsule after list drops rts.combat
        c = copy.deepcopy(cap)
        c["schedule"]["after"] = ["rts.spatial", "rts.goal"]
        out.append(("N12", "move after list drops rts.combat", "structural", c, lir_copy()))
        # N19: capsule declares pos_x read only while the IR stores it
        c = copy.deepcopy(cap)
        for f in c["fields"]:
            if f["name"] == "pos_x":
                f["write"] = False
        out.append(("N19", "pos_x store without declared write", "verify:SU_E_UNDECLARED_WRITE", c, lir_copy()))
    if name == "damage":
        # N07: kill fires for hp == 0 (anchors die)
        m = lir_copy()
        hp = m["instructions"][first(m["instructions"], "load_u32", lambda i: i["field"] == "hp")]["dst"]
        zero = next(i["dst"] for i in m["instructions"] if i["op"] == "const_u32" and i["value"] == 0)
        lt_reg = m["instructions"][first(m["instructions"], "lt_u32", lambda i: i["a"] == zero and i["b"] == hp)]["dst"]
        sel = m["instructions"][first(m["instructions"], "select_u32", lambda i: i["cond"] == lt_reg)]
        sel["on_true"], sel["on_false"] = sel["on_false"], sel["on_true"]
        out.append(("N07", "anchors (hp 0) are killed", "differential", cap, m))
        # N08: despawn_self dropped
        m = lir_copy()
        m["instructions"] = [i for i in m["instructions"] if i["op"] != "despawn_self"]
        out.append(("N08", "kill never emits despawn_self", "differential", cap, m))
        # N09: hp reduced by addition instead of subtraction
        m = lir_copy()
        hp = m["instructions"][first(m["instructions"], "load_u32", lambda i: i["field"] == "hp")]["dst"]
        sub = m["instructions"][first(m["instructions"], "sub_u32", lambda i: i["a"] == hp)]
        sub["op"] = "add_u32"
        out.append(("N09", "hp' = hp + min(incoming, hp)", "differential", cap, m))
        # N16: capsule per-row struct cap 0 with one despawn_self
        c = copy.deepcopy(cap)
        c["limits"]["max_struct_per_row"] = 0
        out.append(("N16", "despawn_self above max_struct_per_row", "verify:SU_E_RESOURCE_LIMIT", c, lir_copy()))
        # N15: capabilities list empty
        c = copy.deepcopy(cap)
        c["capabilities"] = []
        out.append(("N15", "despawn_self not declared as a capability", "structural", c, lir_copy()))
        # N18: after list drops rts.scatter
        c = copy.deepcopy(cap)
        c["schedule"]["after"] = ["rts.spatial"]
        out.append(("N18", "damage after list drops rts.scatter", "structural", c, lir_copy()))
    # structural mutants that apply to every system
    c = copy.deepcopy(cap)
    if name == "goal":
        c["fields"].append({"id": h.FIELD_IDS["hp"], "name": "hp", "type": "u32", "read": True, "write": False})
        c["fields"].sort(key=lambda f: f["id"])
        out.append(("N14", "goal declares an extra read (hp) it never loads", "structural", c, lir_copy()))
    if name == "combat":
        c["schedule"]["phase"] = "simulate"
        out.append(("N17", "combat in another phase", "structural", c, lir_copy()))
    if name == "goal":
        c2 = copy.deepcopy(cap)
        del c2["system_rev"]
        out.append(("N13", "system_rev missing from the record", "structural", c2, lir_copy()))
    return out


def systematic_mutants(name, lir):
    """(id, description, lir) for every single-site operator change of one LIR program (S-series)."""
    ins = lir["instructions"]
    out = []
    for i, x in enumerate(ins):
        op = x["op"]
        if op == "select_u32":
            m = copy.deepcopy(lir)
            m["instructions"][i]["on_true"], m["instructions"][i]["on_false"] = x["on_false"], x["on_true"]
            out.append((f"S-{name}-swap_select@{i}", "swap select branches", m))
        elif op in ("add_u32", "sub_u32", "eq_u32", "lt_u32"):
            m = copy.deepcopy(lir)
            other = {"add_u32": "sub_u32", "sub_u32": "add_u32", "eq_u32": "lt_u32", "lt_u32": "eq_u32"}[op]
            m["instructions"][i]["op"] = other
            out.append((f"S-{name}-op@{i}", f"{op} -> {other}", m))
        elif op == "const_u32":
            for delta, label in ((1, "+1"), (-1, "-1")):
                m = copy.deepcopy(lir)
                m["instructions"][i]["value"] = (x["value"] + delta) & U32
                out.append((f"S-{name}-const@{i}{label}", f"const {x['value']} {label}", m))
        elif op == "store_u32":
            m = copy.deepcopy(lir)
            m["instructions"] = [y for j, y in enumerate(m["instructions"]) if j != i]
            out.append((f"S-{name}-drop_store@{i}", f"drop store to {x['field']}", m))
        elif op == "despawn_self":
            m = copy.deepcopy(lir)
            m["instructions"] = [y for j, y in enumerate(m["instructions"]) if j != i]
            out.append((f"S-{name}-drop_despawn@{i}", "drop despawn_self", m))
    return out


class MutantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = h.build_dir_from_env()
        cls.work = h.make_work_dir()
        cls.drv = h.Driver(cls.build, cls.work)
        cls.ref_fns = h.evaluator_fns()[0]
        cls.inputs = {name: {} for name in h.SYSTEMS}
        for name in h.SYSTEMS:
            dom = h.rows_domain(name, h.Rng(0x6D0A1A1000000001), 512)
            anyr = h.rows_any(name, h.Rng(0x6A4B1700000000003), 2048)
            cls.inputs[name]["domain"] = (dom, [cls.ref_fns[name](r) for r in dom])
            cls.inputs[name]["any"] = (anyr, [cls.ref_fns[name](r) for r in anyr])
        # scene inputs: model phase inputs from two rtsgen scenes (mixed: long run to combat; congested)
        scene_rows = {name: [] for name in h.SYSTEMS}
        for spec, keep in (({"name": "mixed-256", "generator": "mixed.battle.v1", "rows": 256,
                             "seed": 0x1111000000000002}, 10),
                           ({"name": "congested-256", "generator": "congested.blob.v1", "rows": 256,
                             "seed": 0x5EED000000000256}, 1)):
            world, _digest = h.scene_world(dict(spec, ticks=0))
            ticks = 220 if spec["name"].startswith("mixed") else 6
            for tick in h.capture_ticks(world, ticks, keep_every=keep):
                for name in h.SYSTEMS:
                    scene_rows[name].extend(r for r, _ in tick["rec"][name])
        for name in h.SYSTEMS:
            rows = scene_rows[name]
            cls.inputs[name]["scene"] = (rows, [cls.ref_fns[name](r) for r in rows])

    @classmethod
    def tearDownClass(cls):
        h.cleanup(cls.work)

    def detect(self, name, cap, lir):
        """Returns the list of detectors that fire for this mutant (empty when it survives)."""
        found = []
        if h.structural_problems(name, cap, lir):
            found.append("structural")
        paths = self.drv.write_pair(f"mut_{name}", cap, lir, project=True)
        info = self.drv.verify(*paths)
        if not info["ok"]:
            return found + [f"verify:{info['name']}"]
        for label in ("domain", "any", "scene"):
            rows, exp = self.inputs[name][label]
            calls = [(len(rows), h.engine_call_inputs(name, rows))]
            vinfo, results = self.drv.run(*paths, calls)
            if not vinfo["ok"]:
                found.append(f"verify:{vinfo.get('detail', 'run')}")
                break
            res = results[0]
            if res["status"] != 0:
                found.append(f"differential:{label}:status{res['status']}")
                continue
            probs = h.compare_outputs(exp, res["out"], h.writable_names(name))
            if name == "damage":
                want = [i for i, o in enumerate(exp) if o.get("_despawn")]
                if res["despawn"] != want:
                    probs.append("despawn")
            if probs:
                found.append(f"differential:{label}")
        return found

    def test_named_mutants_are_killed_by_their_detector(self):
        records = []
        for name in h.SYSTEMS:
            cap, lir = h.load_record(name)
            for mid, desc, expect, c, l in named_mutants(name, cap, lir):
                with self.subTest(mutant=mid, system=name):
                    detectors = self.detect(name, c, l)
                    ok = any(d.startswith(expect) for d in detectors)
                    records.append({"id": mid, "system": name, "description": desc, "expected": expect,
                                    "detectors": detectors, "killed": bool(detectors), "as_expected": ok})
                    self.assertTrue(ok, f"{mid} ({desc}) not caught by {expect}: {detectors}")
        RESULTS["named"] = records

    def test_systematic_campaign_and_counts(self):
        records = []
        for name in h.SYSTEMS:
            cap, lir = h.load_record(name)
            for mid, desc, l in systematic_mutants(name, lir):
                detectors = self.detect(name, cap, l)
                records.append({"id": mid, "system": name, "description": desc,
                                "detectors": detectors, "killed": bool(detectors)})
        survivors = [r["id"] for r in records if not r["killed"]]
        RESULTS["systematic"] = {"total": len(records), "killed": len(records) - len(survivors),
                                 "survivors": [{"id": s, "class": SURVIVOR_CLASS.get(s, "UNCLASSIFIED")}
                                               for s in survivors],
                                 "records": records}
        unclassified = [s for s in survivors if s not in SURVIVOR_CLASS]
        self.assertEqual(unclassified, [], "survivors without a recorded equivalence class")
        self.assertGreaterEqual(len(records), 10)

    def test_named_set_is_large_enough(self):
        named = RESULTS.get("named", [])
        self.assertGreaterEqual(len(named), 10)
        self.assertTrue(all(r["as_expected"] for r in named))


RESULTS = {}


def _write_record():
    if os.environ.get("SU_IR_WRITE_RECORDS") == "1" and RESULTS:
        os.makedirs(RECORD_DIR, exist_ok=True)
        named = RESULTS.get("named", [])
        sysm = RESULTS.get("systematic", {})
        doc = {"format": "rts-ir-mutants/0.1",
               "named": {"total": len(named), "killed": sum(1 for r in named if r["killed"]),
                         "as_expected": sum(1 for r in named if r["as_expected"]), "records": named},
               "systematic": {k: sysm[k] for k in ("total", "killed", "survivors")} if sysm else {},
               "systematic_records": sysm.get("records", [])}
        with open(os.path.join(RECORD_DIR, "mutants.json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=2, sort_keys=True)
            fh.write("\n")


if __name__ == "__main__":
    try:
        unittest.main(exit=False)
    finally:
        _write_record()
