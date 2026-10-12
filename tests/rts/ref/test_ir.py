# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""IR programs (section 5) against the semantic model (section 8 differential) and the evaluator's checks."""
import unittest

import _paths  # noqa: F401  (sys.path setup)
import checks


class IrVersusModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.b = checks.bundle()

    def _run(self, fn):
        fn(self.b.model, self.b.ir_eval)

    def test_programs_pass_static_checks(self):
        self._run(checks.check_ir_programs_validate)

    def test_static_negatives_are_refused_with_10_4_names(self):
        self._run(checks.check_ir_negatives)

    def test_random_rows_match_model_per_system(self):
        self._run(checks.check_ir_row_differential)

    def test_whole_tick_matches_model_on_probe_scenes(self):
        self._run(checks.check_ir_tick_differential)

    def test_formation_fixture_matches_model_tick_by_tick(self):
        self._run(checks.check_ir_tick_differential_formation_fixture)


if __name__ == "__main__":
    unittest.main()
