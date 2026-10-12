# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-03 engine verification of the four records, through two engine paths:

  1. the verifier driver (su_lir_verify, linked from the release build), and
  2. the sutekh binary's capsule.submit (sutekh serve, JSON lines), which runs the same verifier.

Both paths must agree on the verdict and on the semantic, capsule and source hashes.
The records as committed carry system_rev and capabilities (contract 5.5); the engine refuses those
keys (src/ir/su_lir.c check_capsule). The record as committed is therefore refused by the engine; the
projection (keys removed, nothing else changed) is what the engine verifies. Both facts are asserted.

Environment: SU_BUILD_DIR (release build directory with the static libraries and the sutekh binary).
Optional: SU_IR_WRITE_RECORDS=1 rewrites tests/rts/ir/records/ir_counts.json from this run.
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

RECORD_DIR = os.path.join(h.HERE, "records")


class EngineVerifyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = h.build_dir_from_env()
        cls.binary = os.path.join(cls.build, "sutekh")
        if not os.path.isfile(cls.binary):
            raise AssertionError(f"missing sutekh binary in {cls.build}")
        cls.work = h.make_work_dir()
        cls.drv = h.Driver(cls.build, cls.work)
        cls.root = os.path.join(cls.work, "root")
        os.makedirs(os.path.join(cls.root, "rts"))
        cls.projected = {}
        cls.as_is = {}
        as_is_dir = os.path.join(cls.root, "as_is")
        os.makedirs(as_is_dir)
        for name in h.SYSTEMS:
            cap, lir = h.load_record(name)
            cls.projected[name] = cls.drv.write_pair(name, cap, lir, project=True)
            shutil.copyfile(cls.projected[name][0], os.path.join(cls.root, "rts", f"{name}.capsule.json"))
            shutil.copyfile(cls.projected[name][1], os.path.join(cls.root, "rts", f"{name}.lir.json"))
            # the committed record as-is, with its source copied beside it under the root
            cap_path = os.path.join(as_is_dir, f"{name}.capsule.json")
            with open(cap_path, "w", encoding="utf-8") as fh:
                json.dump(cap, fh, indent=2)
                fh.write("\n")
            lir_path = os.path.join(as_is_dir, f"{name}.lir.json")
            shutil.copyfile(os.path.join(h.IR_DIR, f"{name}.lir.json"), lir_path)
            cls.as_is[name] = (cap_path, lir_path)

    @classmethod
    def tearDownClass(cls):
        h.cleanup(cls.work)

    def _binary(self, requests):
        """Runs sutekh serve on the scratch root with the given (id, capsule_path) requests."""
        req_path = os.path.join(self.work, "requests.jsonl")
        with open(req_path, "w", encoding="utf-8") as fh:
            for rid, rel in requests:
                fh.write(json.dumps({"protocol": "0.1", "id": rid, "method": "capsule.submit",
                                     "params": {"capsule_path": rel}}) + "\n")
        with open(req_path, "rb") as stdin:
            proc = subprocess.run([self.binary, "serve", "--root", self.root], stdin=stdin,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr.decode("utf-8", "replace")[-400:])
        replies = {}
        for line in proc.stdout.decode("utf-8").splitlines():
            obj = json.loads(line)
            replies[obj["id"]] = obj
        return replies

    def test_driver_verifies_projected_records_and_reports_counts(self):
        counts = {}
        for name in h.SYSTEMS:
            with self.subTest(system=name):
                info = self.drv.verify(*self.projected[name])
                self.assertTrue(info["ok"], info)
                self.assertEqual(info["format"], 2 if name == "damage" else 1)
                counts[name] = {
                    "instructions": info["insn"],
                    "registers": info["regs"],
                    "fields": info["fields"],
                    "ir_version": "0.2" if name == "damage" else "0.1",
                    "max_struct_per_row": info["max_struct"],
                    "max_events_per_row": info["max_events"],
                    "uses_entity_ops": info["uses_entity_ops"],
                    "semantic_hash": info["semantic"],
                    "capsule_hash": info["capsule"],
                    "source_hash": info["source"],
                }
        if os.environ.get("SU_IR_WRITE_RECORDS") == "1":
            os.makedirs(RECORD_DIR, exist_ok=True)
            with open(os.path.join(RECORD_DIR, "ir_counts.json"), "w", encoding="utf-8") as fh:
                json.dump({"format": "rts-ir-counts/0.1", "build_dir_note": "release build of this worktree",
                           "measure": "static: instructions of the verified LIR (su_lir insn_count)",
                           "systems": counts}, fh, indent=2, sort_keys=True)
                fh.write("\n")

    def test_as_is_records_are_accepted_by_engine(self):
        """After the capabilities / system_rev support landed, the committed records verify unchanged and
        their semantic hash equals that of the projection without the two keys."""
        for name in h.SYSTEMS:
            with self.subTest(system=name):
                info = self.drv.verify(*self.as_is[name])
                self.assertTrue(info["ok"], info)
                proj = self.drv.verify(*self.projected[name])
                self.assertEqual(info["semantic"], proj["semantic"])

    def test_binary_capsule_submit_agrees_with_driver(self):
        requests = [(name, f"rts/{name}.capsule.json") for name in h.SYSTEMS]
        replies = self._binary(requests)
        for name in h.SYSTEMS:
            with self.subTest(system=name):
                rep = replies[name]
                self.assertTrue(rep["ok"], rep)
                self.assertEqual(rep["result"]["status"], "effects_verified")
                info = self.drv.verify(*self.projected[name])
                self.assertEqual(rep["result"]["semantic_hash"], info["semantic"])
                self.assertEqual(rep["result"]["capsule_hash"], info["capsule"])
                self.assertEqual(rep["result"]["source_hash"], info["source"])
                self.assertEqual(rep["result"]["artifact_format"], 2 if name == "damage" else 1)

    def test_binary_accepts_as_is_records(self):
        """The committed records with system_rev and capabilities are accepted by the binary."""
        for name in h.SYSTEMS:
            with self.subTest(system=name):
                rel = os.path.relpath(self.as_is[name][0], self.root)  # as_is/<name>.capsule.json
                replies = self._binary([(name, rel)])
                rep = replies[name]
                self.assertTrue(rep["ok"], rep)
                self.assertEqual(rep["result"]["status"], "effects_verified")


if __name__ == "__main__":
    unittest.main()
