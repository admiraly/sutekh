# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Digest shape (9.1, 9.2), the targeting oracle (4.2) at 1,024 queriers on the probe scene, and the
oracle budget state machine (N13). The 1,024-querier numbers are observations on an M3-02 probe scene,
not the committed congested.blob.v1 gate; the test checks invariants and that every count is consistent."""
import math
import unittest

import _paths  # noqa: F401  (sys.path setup)
import checks


class DigestAndOracle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.b = checks.bundle()

    def test_digest_shape_and_sensitivity(self):
        checks.check_digest_shape_and_sensitivity(self.b.model, self.b.ir_eval)

    def test_oracle_fixtures(self):
        checks.check_oracle_fixtures(self.b.model, self.b.ir_eval)

    def test_probe_1024_queriers_invariants(self):
        import oracle
        import probe_scene
        world = probe_scene.build_probe(1024, 1)
        queriers = oracle.sample_queriers(world, 1024, checks_seed())
        met = oracle.targeting_metrics(world, queriers, "m3-02-probe")
        self.assertEqual(met["gates_applicable"], False)
        self.assertLessEqual(met["queriers"], 1024)
        # every oracle-positive querier is either valid (found) or missed
        self.assertEqual(met["target_valid"] + met["missed"], met["oracle_positive"])
        self.assertLessEqual(met["valid_but_not_nearest"], met["target_valid"])
        self.assertEqual(met["ratio_count"], met["valid_but_not_nearest"])
        if met["ratio_p95"] is not None and not math.isinf(met["ratio_p95"]):
            self.assertGreaterEqual(met["ratio_p95"], 1.0)
        # the same query is deterministic
        again = oracle.targeting_metrics(probe_again(), oracle.sample_queriers(probe_again(), 1024, checks_seed()),
                                         "m3-02-probe")
        self.assertEqual(again, met)


def checks_seed() -> int:
    return 20261012


def probe_again():
    import probe_scene
    return probe_scene.build_probe(1024, 1)


class BudgetStateMachine(unittest.TestCase):
    """The budget script's states, driven with a fake clock and tiny stand-ins."""

    @classmethod
    def setUpClass(cls):
        import budget
        cls.budget = budget

    def test_measured_when_projection_fits(self):
        clock_state = {"now": 0}

        def clock():
            return clock_state["now"]

        def tick(_world):
            clock_state["now"] += 10  # 10 ns per tick

        p = self.budget.Point(4, 5)
        self.budget.run_point(p, max_seconds=1.0, clock=clock, build=lambda _p: object(), tick=tick,
                              metrics=lambda _w: {"stub": True})
        self.assertEqual(p.state, self.budget.MEASURED)
        self.assertEqual(p.ticks_done, 5)
        self.assertEqual(len(p.tick_ns), 5)
        self.assertEqual(p.reason, None)

    def test_not_run_budget_when_projection_exceeds(self):
        clock_state = {"now": 0}

        def clock():
            return clock_state["now"]

        def tick(_world):
            clock_state["now"] += 2_000_000_000  # 2 s per tick

        p = self.budget.Point(4, 120)
        self.budget.run_point(p, max_seconds=10.0, clock=clock, build=lambda _p: object(), tick=tick,
                              metrics=lambda _w: {"stub": True})
        self.assertEqual(p.state, self.budget.NOT_RUN)
        self.assertEqual(p.reason, "budget")
        self.assertEqual(p.ticks_done, self.budget.MIN_TICKS_BEFORE_PROJECTION)

    def test_failure_is_recorded_not_a_pass(self):
        def tick(_world):
            raise RuntimeError("boom")

        p = self.budget.Point(4, 3)
        self.budget.run_point(p, max_seconds=10.0, clock=lambda: 0, build=lambda _p: object(), tick=tick,
                              metrics=lambda _w: {})
        self.assertEqual(p.state, self.budget.FAILED)
        self.assertIn("boom", p.error)

    def test_percentile_nearest_rank(self):
        self.assertEqual(self.budget._pct([5, 1, 3, 2, 4], 0.5), 3)
        self.assertEqual(self.budget._pct([5, 1, 3, 2, 4], 0.95), 5)
        self.assertIsNone(self.budget._pct([], 0.5))


class MutantsSuite(unittest.TestCase):
    def test_every_mutant_meets_its_expected_outcome(self):
        import mutants
        result = mutants.run_all()
        self.assertTrue(result["baseline_ok"], result["baseline_failing"])
        self.assertEqual(result["mismatches"], [], [r for r in result["rows"] if not r["ok"]])
        self.assertEqual(result["invalid"], 0)
        self.assertGreaterEqual(result["detected"], 10)


if __name__ == "__main__":
    unittest.main()
