# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Checks the BFME evidence registry format (M4-00). It checks structure and labels only, not game behavior."""
import json
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REG = os.path.normpath(os.path.join(HERE, "..", "..", "profiles", "bfme", "evidence.json"))
STATUSES = {"confirmed", "inferred", "unknown"}
ASSET_HINT = re.compile(r"\.(big|big\b|wav|dds|tga|w3d|ini|str)$", re.IGNORECASE)


def load():
    with open(REG, "r", encoding="utf-8") as f:
        return json.load(f)


class RegistryFormat(unittest.TestCase):
    def test_variant_is_labelled_and_provisional(self):
        v = load()["variant"]
        for key in ("game", "expansion", "patch", "status"):
            self.assertIn(key, v)
        self.assertEqual(v["status"], "provisional")

    def test_every_behavior_has_version_label_status_and_source(self):
        for b in load()["behaviors"]:
            self.assertIn(b.get("status"), STATUSES, b)
            self.assertIn("variant", b, b)
            self.assertIn("patch", b, b)
            if b["status"] == "confirmed":
                self.assertTrue(b.get("source"), "confirmed entries must name a source: %r" % b)

    def test_no_original_asset_names_in_registry(self):
        text = open(REG, encoding="utf-8").read()
        for token in re.findall(r'"([^"]+)"', text):
            self.assertFalse(ASSET_HINT.search(token), "asset-like name in registry: %r" % token)

    def test_sections_are_kept_separate(self):
        reg = load()
        for key in ("behaviors", "unknowns", "fidelity", "regression"):
            self.assertIsInstance(reg[key], list)


if __name__ == "__main__":
    unittest.main()
