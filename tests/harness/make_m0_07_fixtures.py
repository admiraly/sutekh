# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Write the M0-07 candidate evaluation fixtures under tests/fixtures/edit/.

  movement_v2.test.json        candidate test file for movement_v2 (correct expectations)
  movement_v2_wrong.test.json  same candidate, one deliberately wrong expectation, so
                               candidate.evaluate fails and the candidate cannot be installed

Expected columns come from tests/harness/reference_eval.py only. Standard library only.
Run: python3 -I tests/harness/make_m0_07_fixtures.py
The generator refuses to write anything if the reference evaluator disagrees with the swap fixture.
"""
from __future__ import annotations

import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import reference_eval as ref  # noqa: E402

EDIT = os.path.join(REPO, "tests", "fixtures", "edit")


def load(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as fh:
        return json.load(fh)


def dump(path, doc):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=2)
        fh.write("\n")


def main() -> int:
    world = load("tests/fixtures/edit/world.json")
    v2_lir = load("tests/fixtures/edit/movement_v2.lir.json")
    v2_cap = load("tests/fixtures/edit/movement_v2.capsule.json")
    sched = load("tests/fixtures/edit/swap_schedule.json")

    # Independent check: v2 from the initial world must agree with the swap fixture at ticks 3 and 4
    # only after two v1 ticks, so compare the v2-only run against the swap run where it applies.
    cases = []
    for ticks in range(1, 5):
        cols = ref.run_ticks(v2_lir, v2_cap, world["columns"], ticks)
        cases.append({"name": f"v2_from_initial_ticks{ticks}", "ticks": ticks, "expected_columns": cols})

    # The swap fixture's tick-3 and tick-4 values are v1 for two ticks then v2; they must equal
    # running v1 twice and then v2 from the same start. Verify that before trusting either.
    v1_lir = load("tests/fixtures/edit/movement.lir.json")
    v1_cap = load("tests/fixtures/edit/movement.capsule.json")
    mid = ref.run_ticks(v1_lir, v1_cap, world["columns"], 2)
    for ticks, name in ((3, "3"), (4, "4")):
        expect = ref.run_ticks(v2_lir, v2_cap, mid, ticks - 2)
        if expect != sched["expected_by_tick"][name]["columns"]:
            raise SystemExit(f"swap fixture tick {name} disagrees with reference evaluator; not writing")

    good = {
        "test_format": "0.1",
        "system_id": "demo.movement",
        "capsule": "movement_v2.capsule.json",
        "world": "world.json",
        "cases": cases,
    }
    wrong = copy.deepcopy(good)
    wrong["cases"] = [copy.deepcopy(c) for c in cases]
    wrong["cases"][0]["name"] = "deliberately_wrong_expectation_ticks1"
    wrong["cases"][0]["expected_columns"]["position_x"][0] = (
        wrong["cases"][0]["expected_columns"]["position_x"][0] + 1) & 0xFFFFFFFF
    wrong["note"] = ("one expected value is changed by +1 on purpose; candidate.evaluate must report "
                     "failed and install must refuse the candidate (SU_E_CANDIDATE_FAILED)")

    v1_cases = []
    for ticks in range(1, 5):
        cols = ref.run_ticks(v1_lir, v1_cap, world["columns"], ticks)
        v1_cases.append({"name": f"v1_from_initial_ticks{ticks}", "ticks": ticks, "expected_columns": cols})
    v1_test = {
        "test_format": "0.1",
        "system_id": "demo.movement",
        "capsule": "movement.capsule.json",
        "world": "world.json",
        "cases": v1_cases,
    }

    dump(os.path.join(EDIT, "movement.test.json"), v1_test)
    dump(os.path.join(EDIT, "movement_v2.test.json"), good)
    dump(os.path.join(EDIT, "movement_v2_wrong.test.json"), wrong)
    print("wrote movement.test.json, movement_v2.test.json and movement_v2_wrong.test.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
