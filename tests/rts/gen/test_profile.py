#!/usr/bin/env python3
"""Profile checks against contract r4 section 2 (table) and 2.2 (domains). Run: python3 -I tests/rts/gen/test_profile.py

The expected names, ids, writable set and spawn classes are transcribed here from the contract
text, not read from the profile file. The schema digest pin is a regression value.
"""
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "..", "bench", "rts", "gen")))
import rtsgen  # noqa: E402

# Section 2 table, ids 1..39 in order.
CONTRACT_NAMES = [
    "kind", "team", "hp", "pos_x", "pos_y", "speed", "range_sq", "reload", "cooldown", "damage",
    "flight", "order_mode", "order_x", "order_y", "formation_slot", "formation_gen", "formation_ox",
    "formation_oy", "goal_x", "goal_y", "anchor_x", "anchor_y", "anchor_valid", "target_slot",
    "target_gen", "target_dist_sq", "target_valid", "target_x", "target_y", "truncated",
    "proj_active", "proj_left", "proj_target", "proj_target_gen", "proj_damage", "dmg_out",
    "dmg_target", "dmg_target_gen", "incoming",
]
# 2.2 writable set of world.set_fields (exactly these 14).
SET_FIELDS_WRITABLE = {
    "pos_x", "pos_y", "speed", "range_sq", "reload", "damage", "flight", "order_mode", "order_x",
    "order_y", "formation_slot", "formation_gen", "formation_ox", "formation_oy",
}
# 2.2 spawn classes: "required" where the table or D1 requires a named value.
SPAWN_REQUIRED = {"kind", "team", "hp", "reload", "damage", "flight"}
SPAWN_IDLE_NONE = {"target_slot", "dmg_target", "proj_target"}
SYSTEM_OWNED = {
    "cooldown", "goal_x", "goal_y", "anchor_x", "anchor_y", "anchor_valid", "target_gen",
    "target_dist_sq", "target_valid", "target_x", "target_y", "truncated", "proj_active",
    "proj_left", "proj_target_gen", "proj_damage", "dmg_out", "dmg_target_gen", "incoming",
}
# Access (section 2 "written by" / "read by" columns), spot-checked for the rows the contract
# states most explicitly.
ACCESS_SPOT = {
    "kind": ({"control(spawn)"}, {"spatial", "scatter"}),
    "hp": ({"damage", "control(spawn)"}, {"damage", "spatial"}),
    "cooldown": ({"combat"}, {"combat"}),
    "goal_x": ({"goal"}, {"move"}),
    "anchor_valid": ({"spatial"}, {"goal"}),
    "target_slot": ({"spatial"}, {"combat"}),
    "proj_target": ({"combat"}, {"combat"}),
    "dmg_target": ({"combat"}, {"scatter"}),
    "incoming": ({"combat (store 0)", "scatter (saturating add)"}, {"damage"}),
}


class ProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = rtsgen.load_profile()
        cls.doc = cls.profile.doc

    def test_names_and_ids(self):
        self.assertEqual([f["name"] for f in self.profile.fields], CONTRACT_NAMES)
        self.assertEqual([f["id"] for f in self.profile.fields], list(range(1, 40)))
        self.assertEqual(rtsgen.FIELD_NAMES, tuple(CONTRACT_NAMES))

    def test_type_default_and_count(self):
        self.assertEqual(self.doc["field_count"], 39)
        for f in self.profile.fields:
            self.assertEqual(f["type"], "u32", f["name"])
            self.assertEqual(f["default"], 0, f["name"])

    def test_set_fields_writable_set(self):
        got = {f["name"] for f in self.profile.fields if f["set_fields"]}
        self.assertEqual(got, SET_FIELDS_WRITABLE)
        self.assertEqual(len(got), 14)

    def test_spawn_classes(self):
        for f in self.profile.fields:
            n = f["name"]
            if n in SPAWN_REQUIRED:
                self.assertEqual(f["spawn"], "required", n)
            elif n in SPAWN_IDLE_NONE:
                self.assertEqual(f["spawn"], "idle", n)
                self.assertEqual(f["domain"], {"idle": "NONE"}, n)
            elif n in SYSTEM_OWNED:
                self.assertEqual(f["spawn"], "idle", n)
                self.assertEqual(f["domain"], {"idle": 0}, n)
                self.assertFalse(f["set_fields"], n)
        self.assertEqual(self.profile.by_name["formation_slot"]["spawn"], "default_none")
        self.assertEqual(self.profile.by_name["formation_gen"]["spawn"], "required_if_formation")
        self.assertEqual(self.profile.by_name["formation_ox"]["spawn"], "required_if_formation")

    def test_domains_from_table_2_2(self):
        d = {f["name"]: f["domain"] for f in self.profile.fields}
        self.assertEqual(d["kind"], {"set": [1, 2]})
        self.assertEqual(d["team"], {"set": [0, 1]})
        self.assertEqual(d["pos_x"], {"range": [0, 32767]})
        self.assertEqual(d["speed"], {"range": [0, 1024]})
        self.assertEqual(d["reload"], {"range": [1, 1024]})
        self.assertEqual(d["damage"], {"range": [1, 65535]})
        self.assertEqual(d["flight"], {"range": [1, 64]})
        self.assertEqual(d["order_mode"], {"set": [0, 1, 2]})
        self.assertEqual(d["order_x"], {"range": [0, 32767]})
        self.assertEqual(d["formation_ox"], {"range": [0, 32767]})
        self.assertEqual(d["formation_gen"], {"rule": "formation_gen"})
        self.assertEqual(d["formation_slot"], {"rule": "formation_slot"})
        self.assertEqual(d["range_sq"], {"rule": "range_sq_by_kind"})
        self.assertEqual(self.profile.by_name["range_sq"]["range_units"], {"set": [0], "or_range": [1, 262144]})
        self.assertEqual(self.profile.by_name["hp"]["hp_kind1"], {"range": [1, 65535], "proposed": "OD-9"})
        self.assertEqual(self.profile.by_name["hp"]["hp_kind2"], {"set": [0]})

    def test_access_spot_checks(self):
        for name, (writers, readers) in ACCESS_SPOT.items():
            f = self.profile.by_name[name]
            self.assertEqual(set(f["written_by"]), writers, name)
            self.assertEqual(set(f["read_by"]), readers, name)

    def test_constants(self):
        c = self.doc["constants"]
        self.assertEqual(c["none"], 0xFFFFFFFF)
        self.assertEqual(c["bias"], 16384)
        self.assertEqual(c["coord_max"], 32767)
        self.assertEqual(c["max_squared_distance"], 2 * 32767 * 32767)
        self.assertLess(c["max_squared_distance"], 2 ** 31)
        self.assertEqual(self.doc["chunk_slots"], 1024)

    def test_rejects_bad_layout(self):
        bad = json.loads(json.dumps(self.doc))
        bad["fields"][0]["id"] = 99
        with self.assertRaises(ValueError):
            rtsgen.Profile(bad)
        bad = json.loads(json.dumps(self.doc))
        bad["fields"][3]["name"] = "pos_z"
        with self.assertRaises(ValueError):
            rtsgen.Profile(bad)
        bad = json.loads(json.dumps(self.doc))
        bad["fields"][5]["default"] = 1
        with self.assertRaises(ValueError):
            rtsgen.Profile(bad)

    def test_schema_digest_regression_pin(self):
        # Regression pin of the encoding in rtsgen.schema_digest (not an independent derivation).
        self.assertEqual(self.profile.schema_digest,
                         "4506e08520602d72e99cab594fcacd1be7ff086e0c92c779b6298a2391fa1ef0")

    def test_schema_digest_independent_of_file_order(self):
        shuffled = json.loads(json.dumps(self.doc))
        shuffled["fields"].reverse()
        self.assertEqual(rtsgen.Profile(shuffled).schema_digest, self.profile.schema_digest)


if __name__ == "__main__":
    unittest.main(verbosity=2)
