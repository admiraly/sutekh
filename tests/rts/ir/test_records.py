# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-03 record tests: regenerate equality, contract 3.2 / 5.1-5.5 structure, JSON schema.

Run: python3 -I -m unittest discover -s tests/rts/ir -p 'test_records.py'
"""
from __future__ import annotations

import copy
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # works under python3 -I
import ir_harness as h  # noqa: E402

try:
    import jsonschema
except ImportError:  # the tests below fail loudly in that case; nothing is reported as passing
    jsonschema = None


class RecordTests(unittest.TestCase):
    def test_regenerate_equality(self):
        problems = h.gen_ir.check()
        self.assertEqual(problems, [], "run: python3 -I bench/rts/gen_ir.py")

    def test_generator_is_deterministic(self):
        self.assertEqual(h.gen_ir.build_records(), h.gen_ir.build_records())

    def test_contract_structure_per_system(self):
        for name in h.SYSTEMS:
            with self.subTest(system=name):
                cap, lir = h.load_record(name)
                self.assertEqual(h.structural_problems(name, cap, lir), [])

    def test_checker_rejects_defects(self):
        """Sensitivity of the structural checker: each defect below must be reported."""
        cap, lir = h.load_record("move")
        cases = {}
        c = copy.deepcopy(cap)
        c["schedule"]["after"] = ["rts.spatial", "rts.goal"]  # drops rts.combat (3.2)
        cases["move after list"] = (c, lir)
        c = copy.deepcopy(cap)
        c["fields"] = [f for f in c["fields"] if f["name"] != "target_valid"]  # undeclared read
        cases["move reads"] = (c, lir)
        c = copy.deepcopy(cap)
        del c["system_rev"]
        cases["system_rev missing"] = (c, lir)
        c = copy.deepcopy(cap)
        c["numeric_profile"] = "rts_u32_v1"
        cases["profile name"] = (c, lir)
        dcap, dlir = h.load_record("damage")
        c = copy.deepcopy(dcap)
        c["capabilities"] = []
        cases["damage capabilities"] = (c, dlir)
        c = copy.deepcopy(dcap)
        c["schedule"]["after"] = ["rts.spatial"]
        cases["damage after"] = (c, dlir)
        for label, (c, l) in cases.items():
            with self.subTest(defect=label):
                name = "damage" if label.startswith("damage") else "move"
                self.assertTrue(h.structural_problems(name, c, l), "defect not reported")

    def test_capsule_schema_ir_variant(self):
        self.assertIsNotNone(jsonschema, "python package jsonschema is required for this check")
        schema_path = os.path.join(h.REPO, "schemas", "capsule.schema.json")
        schema = h.load_json(schema_path)
        validator = jsonschema.Draft202012Validator(schema)
        for name in h.SYSTEMS:
            with self.subTest(system=name):
                cap, _ = h.load_record(name)
                errors = sorted(validator.iter_errors(cap), key=lambda e: list(e.path))
                self.assertEqual([e.message for e in errors], [])

    def test_record_files_exist_with_expected_names(self):
        for name in h.SYSTEMS:
            for suffix in (".capsule.json", ".lir.json"):
                self.assertTrue(os.path.isfile(os.path.join(h.IR_DIR, name + suffix)), name + suffix)


if __name__ == "__main__":
    unittest.main()
