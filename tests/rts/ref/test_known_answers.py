# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Contract 10.1 and 10.2 known answers for the semantic model and the IR evaluator (M3-02)."""
import unittest

import _paths  # noqa: F401  (sys.path setup)
import checks


class KnownAnswers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.b = checks.bundle()

    def _run(self, fn):
        fn(self.b.model, self.b.ir_eval)

    def test_isqrt_and_splitmix64(self):
        self._run(checks.check_isqrt_and_prng)

    def test_goal_rts_goal(self):
        self._run(checks.check_goal_known_answers)

    def test_move_rts_move(self):
        self._run(checks.check_move_known_answers)

    def test_combat_rts_combat_three_periods(self):
        self._run(checks.check_combat_known_answers)

    def test_damage_rts_damage(self):
        self._run(checks.check_damage_known_answers)

    def test_spatial_including_exact_ring_fixture(self):
        self._run(checks.check_spatial_known_answers)

    def test_gather_including_stale_and_zero_anchor(self):
        self._run(checks.check_gather_known_answers)

    def test_scatter_operator_level(self):
        self._run(checks.check_scatter_known_answers)

    def test_duel_edge_fixtures_full_tick(self):
        self._run(checks.check_duel_fixtures)


if __name__ == "__main__":
    unittest.main()
