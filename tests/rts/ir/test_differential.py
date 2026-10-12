# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-03 differential: engine VM on the committed LIR versus the Python evaluator and model.

Run: SU_BUILD_DIR=<release build> python3 -I -m unittest discover -s tests/rts/ir -p 'test_*.py'
Optional: SU_IR_DIFF_RECORD=<file> writes the per-scene summary as JSON.

Three comparisons per system:
  A. scene ticks: the model's tick runs on rtsgen scenes (64 to 4096 rows); each row-system phase
     input is captured per tick and run through the engine VM (all ticks of one system in one driver
     run). Engine == model. Coverage counters show which branches the scene exercised.
  B. domain-valid edge rows (2.2 domains, edge-biased): engine == model, and the reference evaluator
     (bench/rts/ref/ir_eval.py over the reference programs) == model.
  C. arbitrary u32 rows (IR semantics, every u32): engine == evaluator over the committed LIR, and the
     committed LIR evaluated == the reference programs evaluated (two independent authorings).
Damage despawn: the engine's despawn slots (compacted row indices) equal the model's deaths.
"""
from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # works under python3 -I
import ir_harness as h  # noqa: E402

# 220 ticks let the mixed bands (about 16000 units apart, attack-move at speed 96) reach combat.
SCENES = (
    {"name": "duel16", "generator": "duel.fixture.v1", "rows": 16, "seed": 0, "ticks": 6},
    {"name": "mixed-64", "generator": "mixed.battle.v1", "rows": 64, "seed": 0x1111000000000001, "ticks": 220},
    {"name": "mixed-256", "generator": "mixed.battle.v1", "rows": 256, "seed": 0x1111000000000002, "ticks": 220},
    {"name": "mixed-4096", "generator": "mixed.battle.v1", "rows": 4096, "seed": 0x1111000000000003, "ticks": 6},
    {"name": "congested-256", "generator": "congested.blob.v1", "rows": 256, "seed": 0x5EED000000000256, "ticks": 6},
    {"name": "congested-1024", "generator": "congested.blob.v1", "rows": 1024, "seed": 0x5EED000000001024, "ticks": 4},
    {"name": "sparse-256", "generator": "sparse.field.v1", "rows": 256, "seed": 0x0BAD000000000256, "ticks": 6},
)
EDGE_ROWS = 512
ANY_ROWS = 2048

RESULTS = {"scenes": [], "domain": {}, "any_u32": {}}


class DifferentialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = h.build_dir_from_env()
        cls.work = h.make_work_dir()
        cls.drv = h.Driver(cls.build, cls.work)
        cls.ref_fns, cls.rec_fns = h.evaluator_fns()
        cls.paths = {}
        for name in h.SYSTEMS:
            cap, lir = h.load_record(name)
            cls.paths[name] = cls.drv.write_pair(name, cap, lir, project=True)
            info = cls.drv.verify(*cls.paths[name])
            if not info["ok"]:
                raise AssertionError(f"engine refused {name}: {info}")

    @classmethod
    def tearDownClass(cls):
        out = os.environ.get("SU_IR_DIFF_RECORD")
        if out:
            with open(out, "w", encoding="utf-8") as fh:
                json.dump(RESULTS, fh, indent=2, sort_keys=True)
                fh.write("\n")
        h.cleanup(cls.work)

    def _engine_batch(self, name, calls):
        info, results = self.drv.run(*self.paths[name], calls)
        self.assertTrue(info["ok"], info)
        self.assertEqual(len(results), len(calls))
        return results

    # ---- A: scene ticks ------------------------------------------------------------------------
    def test_A_scene_ticks_engine_matches_model(self):
        for spec in SCENES:
            with self.subTest(scene=spec["name"]):
                world, digest = h.scene_world(spec)
                per_tick = h.capture_ticks(world, spec["ticks"])
                summary = {"scene": spec["name"], "rows": spec["rows"], "ticks": spec["ticks"],
                           "scene_digest": digest, "calls": 0, "row_outputs": 0, "mismatches": [],
                           "model_vs_ref_ir_mismatches": 0, "deaths_engine": 0, "deaths_model": 0,
                           "coverage": {}}
                cov = summary["coverage"]
                for name in h.SYSTEMS:
                    calls, meta = [], []
                    for t, tick in enumerate(per_tick):
                        recs = tick["rec"][name]
                        if not recs:
                            continue
                        rows_in = [r for r, _ in recs]
                        expected = [o for _, o in recs]
                        for r_in, exp in recs:
                            ref_out = self.ref_fns[name](r_in)
                            if any(ref_out.get(k) != v for k, v in exp.items()):
                                summary["model_vs_ref_ir_mismatches"] += 1
                            if name == "combat":
                                cov["fires"] = cov.get("fires", 0) + int(exp["proj_active"] == 1 and r_in["proj_active"] == 0)
                                cov["arrivals"] = cov.get("arrivals", 0) + int(exp["dmg_out"] != 0)
                            elif name == "move":
                                cov["moved_rows"] = cov.get("moved_rows", 0) + int(
                                    exp["pos_x"] != r_in["pos_x"] or exp["pos_y"] != r_in["pos_y"])
                            elif name == "goal":
                                cov["goal_nonhold"] = cov.get("goal_nonhold", 0) + int(
                                    exp["goal_x"] != r_in["pos_x"] or exp["goal_y"] != r_in["pos_y"])
                            elif name == "damage":
                                cov["hp_changed"] = cov.get("hp_changed", 0) + int(exp["hp"] != r_in["hp"])
                                cov["kills"] = cov.get("kills", 0) + int(bool(exp.get("_despawn")))
                        calls.append((len(rows_in), h.engine_call_inputs(name, rows_in)))
                        meta.append((t, expected))
                    if not calls:
                        continue
                    results = self._engine_batch(name, calls)
                    summary["calls"] += len(calls)
                    for (t, expected), res in zip(meta, results):
                        summary["row_outputs"] += len(expected)
                        self.assertEqual(res["status"], 0, f"{spec['name']} tick {t} {name}")
                        probs = h.compare_outputs(expected, res["out"], h.writable_names(name))
                        if name == "damage":
                            want = [i for i, o in enumerate(expected) if o.get("_despawn")]
                            summary["deaths_engine"] += len(res["despawn"])
                            summary["deaths_model"] += len(want)
                            if res["despawn"] != want:
                                probs.append(f"despawn engine {res['despawn'][:8]} model {want[:8]}")
                        for p in probs:
                            summary["mismatches"].append(f"tick {t} {name}: {p}")
                self.assertEqual(summary["mismatches"], [], summary["mismatches"][:5])
                self.assertEqual(summary["model_vs_ref_ir_mismatches"], 0)
                RESULTS["scenes"].append(summary)

    # ---- B: domain-valid edge rows ------------------------------------------------------------
    def test_B_domain_rows_engine_and_evaluator_match_model(self):
        rng = h.Rng(0xD0A1A10000000001)
        for name in h.SYSTEMS:
            with self.subTest(system=name):
                rows = h.rows_domain(name, rng, EDGE_ROWS)
                full = [h.full_row(r) for r in rows]
                expected = [h.model.MODEL_SYSTEMS[name](f) for f in full]
                ref_exp = [self.ref_fns[name](r) for r in rows]
                ref_mis = sum(1 for e, g in zip(expected, ref_exp)
                              if any(g.get(k) != v for k, v in e.items()))
                self.assertEqual(ref_mis, 0, f"{name}: reference evaluator disagrees with model")
                (res,) = self._engine_batch(name, [(len(rows), h.engine_call_inputs(name, rows))])
                self.assertEqual(res["status"], 0)
                probs = h.compare_outputs(expected, res["out"], h.writable_names(name))
                if name == "damage":
                    want = [i for i, o in enumerate(expected) if o.get("_despawn")]
                    if res["despawn"] != want:
                        probs.append("despawn differs from model")
                self.assertEqual(probs, [], probs[:5])
                RESULTS["domain"][name] = {"rows": EDGE_ROWS, "engine_mismatches": 0,
                                           "evaluator_vs_model_mismatches": ref_mis,
                                           "coverage": {"hp_kills": sum(1 for e in expected if e.get("_despawn"))}
                                           if name == "damage" else {}}

    # ---- C: arbitrary u32 rows ----------------------------------------------------------------
    def test_C_any_u32_engine_matches_evaluator_and_authorings_agree(self):
        rng = h.Rng(0xA4B1700000000002)
        for name in h.SYSTEMS:
            with self.subTest(system=name):
                rows = h.rows_any(name, rng, ANY_ROWS)
                rec_exp = [self.rec_fns[name](r) for r in rows]
                ref_exp = [self.ref_fns[name](r) for r in rows]
                self.assertEqual(rec_exp, ref_exp, f"{name}: committed LIR and reference programs differ")
                (res,) = self._engine_batch(name, [(len(rows), h.engine_call_inputs(name, rows))])
                self.assertEqual(res["status"], 0)
                probs = h.compare_outputs(rec_exp, res["out"], h.writable_names(name))
                if name == "damage":
                    want = [i for i, o in enumerate(rec_exp) if o.get("_despawn")]
                    if res["despawn"] != want:
                        probs.append("despawn differs from evaluator")
                self.assertEqual(probs, [], probs[:5])
                RESULTS["any_u32"][name] = {"rows": ANY_ROWS, "engine_mismatches": 0,
                                            "authorings_equal": True}


if __name__ == "__main__":
    unittest.main()
