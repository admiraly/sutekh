#!/usr/bin/env python3
"""Scene generator tests (contract 7.2, 7.3, 7.5, 10.2, 10.3 T-RTS-SCENE-1 and T-RTS-LOAD-1 bytes).

Run: python3 -I tests/rts/gen/test_scenes.py
Digests are not hard-coded here: they come from the committed lock (bench/rts/scenes.lock.json),
which check_lock.py regenerates. The tests check structure, determinism and the byte bounds.
"""
import array
import copy
import json
import math
import os
import struct
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
GEN = os.path.join(REPO, "bench", "rts", "gen")
sys.path.insert(0, GEN)
import rtsgen  # noqa: E402
import check_lock  # noqa: E402

NONE = 0xFFFFFFFF
BIAS = 16384
PROFILE = rtsgen.load_profile()
SPEC = {s["name"]: s for s in rtsgen.SCENE_SPECS}
CACHE = {}


def scene(name):
    if name not in CACHE:
        CACHE[name] = rtsgen.make_scene(PROFILE, SPEC[name])
    return CACHE[name]


def row(arr, slot):
    return dict(zip(rtsgen.FIELD_NAMES, arr[slot * 39:(slot + 1) * 39]))


class EncodingTests(unittest.TestCase):
    def test_row_bytes_are_le_u32_in_id_order(self):
        arr, _p, _d = scene("duel16")
        blob = rtsgen.row_bytes(PROFILE, arr)
        self.assertEqual(len(blob), 16 * 156)
        for slot in (0, 9, 15):
            want = struct.pack("<39I", *arr[slot * 39:(slot + 1) * 39])
            self.assertEqual(blob[slot * 156:(slot + 1) * 156], want)

    def test_header_is_canonical_json_with_fixed_keys(self):
        arr, params, _d = scene("duel16")
        header = rtsgen.canon_header("duel.fixture.v1", params, 0, PROFILE.schema_digest, 16, 16)
        self.assertTrue(header.endswith(b"\n"))
        obj = json.loads(header[:-1])
        self.assertEqual(sorted(obj), sorted(["format", "generator", "params", "seed", "schema_digest",
                                              "capacity", "rows"]))
        self.assertEqual(obj["format"], "rts-scene-canon/0.1")
        self.assertEqual(header[:-1], rtsgen.canon_json(obj))  # sorted keys, no whitespace

    def test_scene_digest_is_sha256_of_header_and_rows(self):
        import hashlib
        arr, params, digest = scene("duel16")
        header = rtsgen.canon_header("duel.fixture.v1", params, 0, PROFILE.schema_digest, 16, 16)
        h = hashlib.sha256(header + rtsgen.row_bytes(PROFILE, arr)).hexdigest()
        self.assertEqual(digest, h)


class DeterminismTests(unittest.TestCase):
    def test_generation_twice_gives_identical_bytes_and_digest(self):
        for name in ("duel16", "mixed-1024", "congested-1024", "sparse-16384"):
            a1, p1, d1 = rtsgen.make_scene(PROFILE, SPEC[name])
            a2, p2, d2 = rtsgen.make_scene(PROFILE, SPEC[name])
            self.assertEqual(d1, d2, name)
            self.assertEqual(a1.tobytes(), a2.tobytes(), name)
            self.assertEqual(p1, p2, name)

    def test_separate_processes_agree(self):
        out = []
        for _ in range(2):
            r = subprocess.run([sys.executable, "-I", os.path.join(GEN, "rtsgen.py"), "digest"],
                               capture_output=True, text=True, check=True)
            out.append(r.stdout)
        self.assertEqual(out[0], out[1])
        self.assertEqual(len(out[0].strip().splitlines()), len(rtsgen.SCENE_SPECS) + 1)

    def test_one_field_change_changes_digest(self):
        arr, params, digest = scene("mixed-1024")
        changed = array.array("I", arr)
        changed[IX_POS_X] = changed[IX_POS_X] + 1
        d2 = rtsgen.scene_digest(PROFILE, "mixed.battle.v1", params, SPEC["mixed-1024"]["seed"], changed)
        self.assertNotEqual(digest, d2)

    def test_seed_change_changes_digest(self):
        arr, params, digest = scene("sparse-16384")
        d2 = rtsgen.scene_digest(PROFILE, "sparse.field.v1", params, SPEC["sparse-16384"]["seed"] ^ 1, arr)
        self.assertNotEqual(digest, d2)


IX_POS_X = rtsgen.IX["pos_x"]


class LockTests(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(REPO, "bench", "rts", "scenes.lock.json"), "r", encoding="utf-8") as fh:
            self.lock = json.load(fh)

    def test_committed_lock_matches_generator(self):
        self.assertEqual(check_lock.check_lock(self.lock, PROFILE), [])

    def test_lock_text_is_regenerable(self):
        with open(os.path.join(REPO, "bench", "rts", "scenes.lock.json"), "r", encoding="utf-8") as fh:
            committed = fh.read()
        self.assertEqual(rtsgen.lock_text(rtsgen.build_lock(PROFILE)), committed)

    def test_lock_detects_each_kind_of_drift(self):
        cases = {
            "digest": lambda L: L["scenes"][1].__setitem__("scene_digest", "sha256:" + "0" * 64),
            "seed": lambda L: L["scenes"][1].__setitem__("seed", L["scenes"][1]["seed"] ^ 1),
            "capacity": lambda L: L["scenes"][1].__setitem__("capacity", 1025),
            "params": lambda L: L["scenes"][1]["params"].__setitem__("group_size", 16),
            "schema": lambda L: L.__setitem__("schema_digest", "sha256:" + "1" * 64),
            "generator": lambda L: L["scenes"][1].__setitem__("generator", "sparse.field.v1"),
            "missing": lambda L: L["scenes"].pop(),
        }
        for label, mutate in cases.items():
            bad = copy.deepcopy(self.lock)
            mutate(bad)
            self.assertNotEqual(check_lock.check_lock(bad, PROFILE), [], label)


class DuelFixtureTests(unittest.TestCase):
    def test_exact_range_pair_and_x_extremes(self):
        arr, _p, _d = scene("duel16")
        a, b = row(arr, 0), row(arr, 1)
        dx, dy = abs(a["pos_x"] - b["pos_x"]), abs(a["pos_y"] - b["pos_y"])
        self.assertEqual(dx * dx + dy * dy, a["range_sq"])
        self.assertEqual(a["range_sq"], b["range_sq"])
        self.assertEqual(row(arr, 2)["pos_x"], 0)
        self.assertEqual(row(arr, 3)["pos_x"], 32767)

    def test_equal_distance_candidates(self):
        arr, _p, _d = scene("duel16")
        s = row(arr, 4)
        d5 = (row(arr, 5)["pos_x"] - s["pos_x"]) ** 2 + (row(arr, 5)["pos_y"] - s["pos_y"]) ** 2
        d6 = (row(arr, 6)["pos_x"] - s["pos_x"]) ** 2 + (row(arr, 6)["pos_y"] - s["pos_y"]) ** 2
        self.assertEqual(d5, d6)
        self.assertLessEqual(d5, s["range_sq"])

    def test_hp3_target_and_two_shooters(self):
        arr, _p, _d = scene("duel16")
        t = row(arr, 9)
        self.assertEqual((t["hp"], t["kind"], t["team"]), (3, 1, 1))
        for shooter in (7, 8):
            self.assertEqual(row(arr, shooter)["team"], 0)
            dx = row(arr, shooter)["pos_x"] - t["pos_x"]
            dy = row(arr, shooter)["pos_y"] - t["pos_y"]
            self.assertLessEqual(dx * dx + dy * dy, row(arr, shooter)["range_sq"])

    def test_formation_anchor_and_member(self):
        arr, _p, _d = scene("duel16")
        anchor, member = row(arr, 10), row(arr, 11)
        self.assertEqual((anchor["kind"], anchor["hp"], anchor["range_sq"]), (2, 0, 0))
        self.assertEqual((member["formation_slot"], member["formation_gen"]), (10, 1))
        self.assertEqual((member["formation_ox"], member["formation_oy"]), (BIAS, BIAS))


class RemainderAndFormationTests(unittest.TestCase):
    def check_grouped(self, name, n):
        arr, params, _d = scene(name)
        groups, rem = divmod(n, 17)
        self.assertEqual(rem, 4 if n == 1024 else 13)
        for g in range(groups):
            base = g * 17
            anchor = row(arr, base)
            self.assertEqual(anchor["kind"], 2, (name, g))
            self.assertEqual(anchor["formation_slot"], NONE)
            for i in range(16):
                m = row(arr, base + 1 + i)
                self.assertEqual(m["kind"], 1)
                self.assertEqual(m["formation_slot"], base, (name, g, i))
                self.assertEqual(m["formation_gen"], 1)
                self.assertEqual(m["team"], anchor["team"])
                self.assertEqual(m["formation_ox"], BIAS + (i % 4) * 32 - 48)
                self.assertEqual(m["formation_oy"], BIAS + (i // 4) * 32 - 48)
                self.assertEqual(m["order_mode"], 0)
        for j in range(rem):
            r = row(arr, groups * 17 + j)
            self.assertEqual(r["kind"], 1, name)
            self.assertEqual(r["formation_slot"], NONE, name)
            self.assertEqual((r["formation_gen"], r["order_mode"]), (0, 2), name)
        self.assertEqual(len(arr) // 39, n)

    def test_remainder_rule_1024(self):
        self.check_grouped("mixed-1024", 1024)     # 60 groups + 4 remainder rows
        self.check_grouped("congested-1024", 1024)

    def test_remainder_rule_16384(self):
        self.check_grouped("mixed-16384", 16384)   # 963 groups + 13 remainder rows
        self.check_grouped("congested-16384", 16384)

    def test_anchor_idle_values(self):
        for name in ("duel16", "mixed-1024", "congested-1024", "congested-16384"):
            arr, _p, _d = scene(name)
            for slot in range(len(arr) // 39):
                r = row(arr, slot)
                if r["kind"] != 2:
                    continue
                self.assertEqual((r["reload"], r["damage"], r["flight"]), (1, 1, 1), (name, slot))
                self.assertEqual((r["range_sq"], r["hp"]), (0, 0), (name, slot))
                for f in ("cooldown", "goal_x", "goal_y", "anchor_x", "anchor_y", "anchor_valid",
                          "target_gen", "target_dist_sq", "target_valid", "target_x", "target_y",
                          "truncated", "proj_active", "proj_left", "proj_target_gen", "proj_damage",
                          "dmg_out", "dmg_target_gen", "incoming"):
                    self.assertEqual(r[f], 0, (name, slot, f))
                for f in ("target_slot", "proj_target", "dmg_target"):
                    self.assertEqual(r[f], NONE, (name, slot, f))

    def test_validator_accepts_all_committed_scenes(self):
        for spec in rtsgen.SCENE_SPECS:
            arr, _p, _d = scene(spec["name"])
            self.assertTrue(rtsgen.validate_scene(PROFILE, arr, capacity=spec["rows"]), spec["name"])


class FamilyShapeTests(unittest.TestCase):
    def test_mixed_bands_and_both_teams(self):
        arr, _p, _d = scene("mixed-16384")
        seen = {0: 0, 1: 0}
        for slot in range(16384):
            r = row(arr, slot)
            lo, hi = rtsgen.MIXED_BANDS[r["team"]]
            self.assertTrue(lo <= r["pos_x"] <= hi, slot)
            self.assertLessEqual(r["pos_y"], 32767)
            seen[r["team"]] += 1
        self.assertTrue(seen[0] > 0 and seen[1] > 0)

    def test_congested_box_and_interleaving(self):
        arr, _p, _d = scene("congested-1024")
        counts = {0: 0, 1: 0}
        for slot in range(1024):
            r = row(arr, slot)
            self.assertTrue(15360 <= r["pos_x"] <= 17407 and 15360 <= r["pos_y"] <= 17407, slot)
            counts[r["team"]] += 1
        self.assertEqual(counts, {0: 512, 1: 512})  # 30 + 30 groups of 17, 2 + 2 remainder rows

    def test_sparse_team_fraction_and_no_formations(self):
        arr, _p, _d = scene("sparse-65536")
        team1 = 0
        for slot in range(65536):
            r = row(arr, slot)
            self.assertEqual(r["formation_slot"], NONE)
            team1 += r["team"]
        self.assertTrue(7000 <= team1 <= 9200, team1)  # expected about 8192 (1/8)

    def test_sparse_positions_cover_domain(self):
        arr, _p, _d = scene("sparse-16384")
        xs = [row(arr, s)["pos_x"] for s in range(16384)]
        self.assertTrue(min(xs) < 1024 and max(xs) > 31743)


class EmitterTests(unittest.TestCase):
    def test_worst_case_row_is_925_bytes(self):
        names = rtsgen.FIELD_NAMES
        obj = {"values": {n: 4294967295 for n in names}, "external_id": 4294967295}
        text = json.dumps(obj, separators=(",", ":"))
        self.assertEqual(len(text), rtsgen.MAX_ROW_JSON_BYTES)
        self.assertEqual(rtsgen.MAX_ROW_JSON_BYTES, 925)

    def test_1024_worst_case_rows_is_948225_bytes(self):
        row_text = json.dumps({"values": {n: 4294967295 for n in rtsgen.FIELD_NAMES},
                               "external_id": 4294967295}, separators=(",", ":"))
        body = "[" + ",".join([row_text] * 1024) + "]"
        self.assertEqual(len(body), 948225)
        self.assertEqual(rtsgen.MAX_BATCH_JSON_BYTES, 948225)
        self.assertLess(len(body) + 200, rtsgen.REQUEST_BYTES_MAX)

    def test_emitted_calls_respect_bounds_and_round_trip(self):
        for name in ("mixed-1024", "sparse-16384"):
            arr, _p, _d = scene(name)
            n = len(arr) // 39
            total = 0
            calls = 0
            for first, count, text in rtsgen.emit_spawn_batches(arr):
                calls += 1
                self.assertLessEqual(count, 1024)
                self.assertLessEqual(len(text), rtsgen.MAX_BATCH_JSON_BYTES)
                rows = json.loads(text)
                self.assertEqual(len(rows), count)
                for k, obj in enumerate(rows):
                    slot = first + k
                    self.assertEqual(obj["external_id"], slot)
                    self.assertEqual(list(obj["values"]), list(rtsgen.FIELD_NAMES))
                    self.assertEqual([obj["values"][f] for f in rtsgen.FIELD_NAMES],
                                     list(arr[slot * 39:(slot + 1) * 39]))
                total += count
            self.assertEqual(total, n)
            self.assertEqual(calls, math.ceil(n / 1024))

    def test_one_million_rows_is_977_calls(self):
        self.assertEqual(math.ceil(1000000 / 1024), 977)  # 10.3(e)

class ValidatorNegativeTests(unittest.TestCase):
    """Each mutation of a valid scene must be refused by validate_scene (2.2, 7.3)."""

    def base(self):
        arr, _p, _d = scene("duel16")
        return array.array("I", arr)

    def expect_refused(self, label, edit):
        arr = self.base()
        edit(arr)
        with self.assertRaises(ValueError, msg=label):
            rtsgen.validate_scene(PROFILE, arr, capacity=16)

    def test_refusals(self):
        def set_(slot, name, val):
            return lambda a: a.__setitem__(slot * 39 + rtsgen.IX[name], val)
        self.expect_refused("flight 0", set_(0, "flight", 0))
        self.expect_refused("flight 65", set_(0, "flight", 65))
        self.expect_refused("reload 0", set_(0, "reload", 0))
        self.expect_refused("damage 0", set_(0, "damage", 0))
        self.expect_refused("order_mode 3", set_(0, "order_mode", 3))
        self.expect_refused("unit hp 0", set_(0, "hp", 0))
        self.expect_refused("anchor hp 1", set_(10, "hp", 1))
        self.expect_refused("anchor reload 2", set_(10, "reload", 2))
        self.expect_refused("anchor range 100", set_(10, "range_sq", 100))
        self.expect_refused("range_sq 262145", set_(0, "range_sq", 262145))
        self.expect_refused("pos_x 32768", set_(0, "pos_x", 32768))
        self.expect_refused("speed 1025", set_(0, "speed", 1025))
        self.expect_refused("team 2", set_(0, "team", 2))
        self.expect_refused("kind 0", set_(0, "kind", 0))
        self.expect_refused("cooldown nonzero at spawn", set_(0, "cooldown", 1))
        self.expect_refused("target_slot 0 at spawn", set_(0, "target_slot", 0))
        self.expect_refused("gen 0 with slot", set_(11, "formation_gen", 0))
        self.expect_refused("gen nonzero with NONE slot", set_(0, "formation_gen", 5))
        self.expect_refused("slot not an anchor", set_(11, "formation_slot", 9))
        self.expect_refused("slot beyond capacity", set_(11, "formation_slot", 16))
        self.expect_refused("member team differs from anchor", set_(11, "team", 1))

    def test_accepts_unmodified_base(self):
        self.assertTrue(rtsgen.validate_scene(PROFILE, self.base(), capacity=16))


class LockCheckCliTests(unittest.TestCase):
    def test_check_lock_cli_exit_zero(self):
        r = subprocess.run([sys.executable, "-I", os.path.join(GEN, "check_lock.py")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("lock check passed", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
