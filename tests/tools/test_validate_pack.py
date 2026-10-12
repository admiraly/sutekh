# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Regression tests for tools/validate_pack.py (stdlib unittest, no engine build)."""
from __future__ import annotations

import copy
import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import validate_pack as vp  # noqa: E402

COMMITTED_REPORT = (ROOT / "PACK_VALIDATION.json").read_bytes()


def _fixtures():
    cap = vp.load(ROOT / "examples/movement.capsule.json")
    prog = vp.load(ROOT / "examples/movement.lir.json")
    world = vp.load(ROOT / "examples/world.json")
    return cap, prog, world


class ValidatorHoles(unittest.TestCase):
    def test_value_op_with_null_dst_is_rejected(self):
        cap, prog, _ = _fixtures()
        bad = copy.deepcopy(prog)
        bad["instructions"].insert(0, {"op": "const_u32", "dst": None, "value": 5})
        with self.assertRaises(vp.SpecError):
            vp.validate_program(bad, cap)

    def test_non_positive_entity_ids_are_rejected(self):
        cap, prog, world = _fixtures()
        for bad_id in (0, -1):
            bad_world = copy.deepcopy(world)
            bad_world["entity_ids"][0] = bad_id
            with self.subTest(entity_id=bad_id), self.assertRaises(vp.SpecError):
                vp.evaluate(prog, cap, bad_world, 1)


class ReportIsNotRewrittenByChecks(unittest.TestCase):
    def test_default_run_leaves_committed_report_untouched(self):
        report = ROOT / "PACK_VALIDATION.json"
        try:
            proc = subprocess.run([sys.executable, str(ROOT / "tools/validate_pack.py")],
                                  capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(report.read_bytes(), COMMITTED_REPORT)
        finally:
            report.write_bytes(COMMITTED_REPORT)

    def test_committed_report_matches_current_tree(self):
        proc = subprocess.run([sys.executable, str(ROOT / "tools/validate_pack.py"), "--check-report"],
                              capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
