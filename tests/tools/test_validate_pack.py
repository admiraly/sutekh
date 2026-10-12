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


class ReportModeGates(unittest.TestCase):
    """--check-report must fail on a stale report, and must not depend on whether jsonschema is installed."""

    def _run_main(self, argv, report_text, schema_status=None):
        import tempfile
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "PACK_VALIDATION.json"
            report.write_bytes(report_text.encode("utf-8"))
            patches = [mock.patch.object(vp, "REPORT_PATH", report)]
            if schema_status is not None:
                skipped = {"status": schema_status, "reason": "test: jsonschema absent"}
                patches.append(mock.patch.object(vp, "schema_checks", lambda: skipped))
            for patch in patches:
                patch.start()
            try:
                rc = vp.main(argv)
            finally:
                for patch in patches:
                    patch.stop()
            return rc, report.read_bytes().decode("utf-8")

    def test_stale_report_fails_check(self):
        import contextlib, io
        stale = COMMITTED_REPORT.decode("utf-8").replace('"status": "passed"', '"status": "passed", "x": 1', 1)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc, _ = self._run_main(["--check-report"], stale)
        self.assertEqual(rc, 1)

    def test_matching_report_passes_check(self):
        import contextlib, io
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc, _ = self._run_main(["--check-report"], COMMITTED_REPORT.decode("utf-8"))
        self.assertEqual(rc, 0)

    def test_missing_jsonschema_does_not_make_the_committed_report_stale(self):
        # A host without jsonschema reports json_schema as skipped. The rest of the report must still
        # decide the check, so the result does not depend on the host's installed packages.
        import contextlib, io
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc, _ = self._run_main(["--check-report"], COMMITTED_REPORT.decode("utf-8"), schema_status="skipped")
        self.assertEqual(rc, 0)

    def test_write_refuses_to_commit_a_skipped_schema_result(self):
        # --write-report must not store a host-dependent "skipped" result as the committed report.
        import contextlib, io
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc, after = self._run_main(["--write-report"], COMMITTED_REPORT.decode("utf-8"), schema_status="skipped")
        self.assertEqual(rc, 1)
        self.assertEqual(after, COMMITTED_REPORT.decode("utf-8"))


if __name__ == "__main__":
    unittest.main()
