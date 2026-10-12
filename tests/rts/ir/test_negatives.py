# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-03 negative fixtures (contract 10.4): each committed defect must be refused with its code.

Fixtures: tests/rts/ir/fixtures/neg (written by make_negatives.py; expected codes in
fixtures/negative_cases.json). Each case runs through the verifier driver and through sutekh
capsule.submit. Cases marked not_run are reported as skipped with the reason; they are not passes.

Environment: SU_BUILD_DIR (release build directory).
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # works under python3 -I
import ir_harness as h  # noqa: E402

FIX = os.path.join(h.HERE, "fixtures")


class NegativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = h.build_dir_from_env()
        cls.binary = os.path.join(cls.build, "sutekh")
        cls.work = h.make_work_dir()
        cls.drv = h.Driver(cls.build, cls.work)
        with open(os.path.join(FIX, "negative_cases.json"), "r", encoding="utf-8") as fh:
            cls.cases = json.load(fh)["cases"]
        cls.root = os.path.join(cls.work, "root")
        os.makedirs(os.path.join(cls.root, "neg"))
        for case in cls.cases:
            if case["status"] != "run":
                continue
            for fname in (case["capsule"], case["source"]):
                shutil.copyfile(os.path.join(FIX, "neg", fname), os.path.join(cls.root, "neg", fname))

    @classmethod
    def tearDownClass(cls):
        h.cleanup(cls.work)

    def test_driver_refuses_each_case_with_its_code(self):
        for case in self.cases:
            if case["status"] != "run":
                continue
            with self.subTest(case=case["name"]):
                cap = os.path.join(FIX, "neg", case["capsule"])
                lir = os.path.join(FIX, "neg", case["source"])
                info = self.drv.verify(cap, lir)
                self.assertFalse(info["ok"], "engine accepted a defective record")
                self.assertEqual(info["name"], case["expected_code"], info.get("detail"))

    def test_binary_refuses_each_case_with_its_code(self):
        runs = [c for c in self.cases if c["status"] == "run"]
        req = os.path.join(self.work, "requests.jsonl")
        with open(req, "w", encoding="utf-8") as fh:
            for case in runs:
                fh.write(json.dumps({"protocol": "0.1", "id": case["name"], "method": "capsule.submit",
                                     "params": {"capsule_path": f"neg/{case['capsule']}"}}) + "\n")
        with open(req, "rb") as stdin:
            proc = subprocess.run([self.binary, "serve", "--root", self.root], stdin=stdin,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr.decode("utf-8", "replace")[-400:])
        replies = {}
        for line in proc.stdout.decode("utf-8").splitlines():
            obj = json.loads(line)
            replies[obj["id"]] = obj
        for case in runs:
            with self.subTest(case=case["name"]):
                rep = replies.get(case["name"])
                self.assertIsNotNone(rep, "no reply")
                self.assertFalse(rep["ok"], "binary accepted a defective record")
                self.assertEqual(rep["error"]["code"], case["expected_code"], rep["error"].get("message"))

    def test_not_run_cases_are_reported_not_passed(self):
        for case in self.cases:
            if case["status"] == "run":
                continue
            with self.subTest(case=case["name"]):
                self.skipTest(f"not run: {case['reason']}")


if __name__ == "__main__":
    unittest.main()
