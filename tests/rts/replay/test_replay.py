# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Replay recorder and replayer (M3-05, OD-17) against the semantic model. Gates T-RTS-REPLAY-1 and -2
are exercised here on the model backend only. T-RTS-REPLAY-3 (kill and recover) needs the engine."""
import json
import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_BENCH = os.path.normpath(os.path.join(_HERE, "..", "..", "..", "bench", "rts", "replay"))
if _BENCH not in sys.path:
    sys.path.insert(0, _BENCH)
_REF = os.path.normpath(os.path.join(_HERE, "..", "..", "..", "bench", "rts", "ref"))
if _REF not in sys.path:
    sys.path.insert(0, _REF)

import rts_replay as rr  # noqa: E402
import spawn_batch_limits as limits  # noqa: E402
import model  # noqa: E402
from model import ModelError  # noqa: E402

CAP = 64
TICKS = 24
DIGEST_EVERY = 4
SCENE = {"generator": "rts-units-splitmix64/0.1", "params": {"units": 20}, "seed": 7,
         "capacity": CAP}


def handle(slot: int, gen: int = 1) -> int:
    return (gen << 32) | slot


def record(path: str, commands=None) -> rr.Recorder:
    rec = rr.Recorder(rr.ModelBackend(CAP), SCENE, TICKS, DIGEST_EVERY)
    for t, name, args in (commands if commands is not None else default_commands()):
        rec.command(t, name, args)
    rec.run()
    rec.write(path)
    return rec


def default_commands():
    return [
        (3, rr.CMD_SET_FIELDS, {"entities": [{"entity": handle(2), "values": {"order_x": 900}}]}),
        (5, rr.CMD_SPAWN_BATCH, {"rows": [
            {"values": {"kind": 1, "team": 1, "hp": 9, "pos_x": 100, "pos_y": 100, "reload": 2,
                        "damage": 3, "flight": 4}, "external_id": 77},
            {"values": {"kind": 2, "team": 0, "hp": 0, "pos_x": 300, "pos_y": 300, "reload": 1,
                        "damage": 1, "flight": 1}},
        ]}),
        (8, rr.CMD_DESPAWN, {"entity": handle(1)}),
        (10, rr.CMD_SET_FIELDS, {"entities": [{"entity": handle(3), "values": {"flight": 5}}]}),
    ]


def edit_lines(path: str, fn) -> None:
    with open(path, "r", encoding="ascii") as f:
        lines = f.read().split("\n")
    lines = [ln for ln in fn(lines) if ln != ""]
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def edit_header(path: str, key: str, value) -> None:
    def fn(lines):
        h = json.loads(lines[0])
        h[key] = value
        lines[0] = rr.canonical_json(h)
        return lines
    edit_lines(path, fn)


class RecordAndReplay(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="rts_replay_")
        self.path = os.path.join(self.dir, "run.rtsreplay")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def replay_fresh(self, path=None):
        return rr.replay(path or self.path, rr.ModelBackend(CAP))

    def test_record_then_replay_identical_digests(self):  # T-RTS-REPLAY-1 (model backend)
        rec = record(self.path)
        result = self.replay_fresh()
        self.assertTrue(result["ok"], result["divergence"])
        self.assertEqual(result["digests_checked"], TICKS // DIGEST_EVERY + 1)
        header, cmds, digests = rr.read_replay(self.path)
        self.assertEqual(header["caps"]["control_events_counted"], False)
        self.assertEqual(len(cmds), len(default_commands()))
        self.assertEqual(digests, rec.digests)

    def test_replay_is_repeatable(self):
        record(self.path)
        self.assertTrue(self.replay_fresh()["ok"])
        self.assertTrue(self.replay_fresh()["ok"])

    def test_flipped_command_value_reports_first_divergent_tick(self):  # T-RTS-REPLAY-2
        record(self.path)

        def flip(lines):
            return [ln.replace('"order_x":900', '"order_x":901') for ln in lines]
        edit_lines(self.path, flip)
        result = self.replay_fresh()
        self.assertFalse(result["ok"])
        # The command is applied at boundary 3; the first digest record after it is at tick 4.
        self.assertEqual(result["divergence"].t, 4)
        self.assertEqual(result["divergence"].field, "state")

    def test_dropped_set_fields_reports_divergence(self):  # T-RTS-REPLAY-2
        record(self.path)
        edit_lines(self.path, lambda ls: [ln for ln in ls if '"order_x":900' not in ln])
        result = self.replay_fresh()
        self.assertFalse(result["ok"])
        self.assertEqual(result["divergence"].t, 4)

    def test_control_events_counted_true_fails_closed(self):  # T-RTS-REPLAY-2
        record(self.path)
        edit_header(self.path, "caps", dict(rr.read_replay(self.path)[0]["caps"],
                                            control_events_counted=True))
        with self.assertRaises(rr.ReplayError) as cm:
            self.replay_fresh()
        self.assertEqual(cm.exception.code, "SU_E_INVALID_ARGUMENT")

    def test_operator_manifest_mismatch_fails_closed(self):  # T-RTS-REPLAY-2
        record(self.path)
        edit_header(self.path, "operator_manifest_hash", "sha256:" + "1" * 64)
        with self.assertRaises(rr.ReplayError) as cm:
            self.replay_fresh()
        self.assertEqual(cm.exception.code, "SU_E_SCHEMA_MISMATCH")

    def test_scene_digest_mismatch_fails_closed(self):
        record(self.path)
        header = rr.read_replay(self.path)[0]
        edit_header(self.path, "scene", dict(header["scene"], scene_digest="sha256:" + "2" * 64))
        with self.assertRaises(rr.ReplayError):
            self.replay_fresh()

    def test_missing_digest_record_is_a_mismatch(self):
        record(self.path)
        edit_lines(self.path, lambda ls: [ln for ln in ls if ln][:-1])
        result = self.replay_fresh()
        self.assertFalse(result["ok"])

    def test_out_of_order_commands_are_refused(self):
        record(self.path)

        def swap(lines):
            # Put the t=10 command before the t=8 command.
            idx = [i for i, ln in enumerate(lines) if '"t":8}' in ln or '"t":10}' in ln]
            lines[idx[0]], lines[idx[1]] = lines[idx[1]], lines[idx[0]]
            return lines
        edit_lines(self.path, swap)
        with self.assertRaises(rr.ReplayError):
            rr.read_replay(self.path)

    SAME_BOUNDARY = [
        (3, rr.CMD_SET_FIELDS, {"entities": [{"entity": handle(2), "values": {"pos_x": 100}}]}),
        (3, rr.CMD_SET_FIELDS, {"entities": [{"entity": handle(2), "values": {"pos_x": 200}}]}),
        (6, rr.CMD_DESPAWN, {"entity": handle(1)}),
    ]

    def test_same_boundary_seq_order_is_enforced_by_the_reader(self):  # M3-M18 (ignores seq order)
        record(self.path, self.SAME_BOUNDARY)

        def swap(lines):
            idx = [i for i, ln in enumerate(lines) if '"t":3}' in ln]
            self.assertEqual(len(idx), 2)
            lines[idx[0]], lines[idx[1]] = lines[idx[1]], lines[idx[0]]
            return lines
        edit_lines(self.path, swap)
        with self.assertRaises(rr.ReplayError) as cm:
            rr.read_replay(self.path)
        self.assertIn("out of (t, seq) order", str(cm.exception))

    def test_same_boundary_commands_apply_in_seq_order(self):  # M3-M18 (applies out of seq order)
        # pos_x 100 then 200 on one entity at one boundary: the final value, and so every later digest, depends on
        # the order. A replayer that applies them in any other order diverges at the next digest record.
        rec = record(self.path, self.SAME_BOUNDARY)
        self.assertTrue(self.replay_fresh()["ok"])
        be = rr.ModelBackend(CAP)
        be.load_scene(SCENE)
        for _ in range(3):
            be.step()
        be.apply({"cmd": rr.CMD_SET_FIELDS, "args": self.SAME_BOUNDARY[1][2]})
        be.apply({"cmd": rr.CMD_SET_FIELDS, "args": self.SAME_BOUNDARY[0][2]})
        be.step()
        self.assertNotEqual(be.digest(4)["state"], rec.digests[1]["state"],
                            "the two orders must differ, or this test cannot detect an order bug")

    def test_digest_records_out_of_order_are_refused(self):  # M3-M18 (ignores digest order)
        record(self.path)

        def swap(lines):
            idx = [i for i, ln in enumerate(lines) if '"state"' in ln]
            lines[idx[0]], lines[idx[1]] = lines[idx[1]], lines[idx[0]]
            return lines
        edit_lines(self.path, swap)
        with self.assertRaises(rr.ReplayError) as cm:
            rr.read_replay(self.path)
        self.assertIn("digest records out of order", str(cm.exception))

    def test_noncanonical_line_is_refused(self):
        record(self.path)
        edit_lines(self.path, lambda ls: [ls[0] + " "] + ls[1:])
        with self.assertRaises(rr.ReplayError):
            rr.read_replay(self.path)

    def test_cli_exit_status(self):
        record(self.path)
        self.assertEqual(rr.main(["replay", self.path]), 0)
        edit_lines(self.path, lambda ls: [ln.replace('"order_x":900', '"order_x":901') for ln in ls])
        self.assertEqual(rr.main(["replay", self.path]), 1)


class BatchAndSetFieldsRules(unittest.TestCase):
    def test_spawn_batch_is_atomic(self):  # D3 / T-RTS-LOAD-1(g) on the model
        b = rr.ModelBackend(CAP)
        b.load_scene(SCENE)
        before = b.scene_digest()
        bad = {"rows": [
            {"values": {"kind": 1, "hp": 5, "reload": 1, "damage": 1, "flight": 1}},
            {"values": {"kind": 1, "hp": 0, "reload": 1, "damage": 1, "flight": 1}},  # hp >= 1
        ]}
        with self.assertRaises(ModelError):
            b.apply({"cmd": rr.CMD_SPAWN_BATCH, "args": bad})
        self.assertEqual(b.scene_digest(), before)

    def test_spawn_batch_refuses_when_free_slots_are_short(self):
        b = rr.ModelBackend(4)
        row = {"values": {"kind": 1, "hp": 1, "reload": 1, "damage": 1, "flight": 1}}
        with self.assertRaises(ModelError) as cm:
            b.apply({"cmd": rr.CMD_SPAWN_BATCH, "args": {"rows": [row] * 5}})
        self.assertEqual(cm.exception.code, "SU_E_RESOURCE_LIMIT")
        self.assertEqual(sum(b.world.alive), 0)

    def test_set_fields_refuses_system_owned_field(self):
        b = rr.ModelBackend(CAP)
        b.load_scene(SCENE)
        with self.assertRaises(ModelError) as cm:
            b.apply({"cmd": rr.CMD_SET_FIELDS,
                     "args": {"entities": [{"entity": handle(0), "values": {"hp": 3}}]}})
        self.assertEqual(cm.exception.code, "SU_E_TYPE")

    def test_set_fields_enforces_rts_u32_v1_domains(self):  # contract 2.2 / T-RTS-CTRL-1
        # Same bounds as spawn (src/runtime/rts/control_domains.c). Each case must be refused.
        refused = [("flight", 0), ("flight", 65), ("reload", 0), ("reload", 1025), ("damage", 0),
                   ("damage", 65536), ("order_mode", 3), ("pos_x", 32768), ("pos_y", 40000),
                   ("order_x", 32768), ("speed", 1025), ("range_sq", 262145), ("formation_ox", 32768)]
        for name, value in refused:
            b = rr.ModelBackend(CAP)
            b.load_scene(SCENE)
            before = b.scene_digest()
            with self.subTest(field=name, value=value):
                with self.assertRaises(ModelError) as cm:
                    b.apply({"cmd": rr.CMD_SET_FIELDS,
                             "args": {"entities": [{"entity": handle(0), "values": {name: value}}]}})
                self.assertEqual(cm.exception.code, "SU_E_TYPE")
                self.assertEqual(b.scene_digest(), before)

    def test_set_fields_accepts_domain_edges(self):
        accepted = [("flight", 1), ("flight", 64), ("reload", 1), ("reload", 1024), ("damage", 1),
                    ("damage", 65535), ("order_mode", 2), ("pos_x", 32767), ("speed", 1024),
                    ("range_sq", 0), ("range_sq", 262144)]
        for name, value in accepted:
            b = rr.ModelBackend(CAP)
            b.load_scene(SCENE)
            with self.subTest(field=name, value=value):
                b.apply({"cmd": rr.CMD_SET_FIELDS,
                         "args": {"entities": [{"entity": handle(0), "values": {name: value}}]}})

    def test_spawn_batch_external_id_must_be_u32(self):  # contract D6 / OD-3b / F4
        b = rr.ModelBackend(CAP)
        b.load_scene(SCENE)
        before = b.scene_digest()
        for bad in (-1, 2 ** 32, 2 ** 40, "7", 1.0, True):
            row = {"values": {"kind": 1, "hp": 1, "reload": 1, "damage": 1, "flight": 1}, "external_id": bad}
            with self.subTest(external_id=bad):
                with self.assertRaises(ModelError) as cm:
                    b.apply({"cmd": rr.CMD_SPAWN_BATCH, "args": {"rows": [row]}})
                self.assertEqual(cm.exception.code, "SU_E_TYPE")
                self.assertEqual(b.scene_digest(), before)
        ok = {"values": {"kind": 1, "hp": 1, "reload": 1, "damage": 1, "flight": 1}, "external_id": 2 ** 32 - 1}
        b.apply({"cmd": rr.CMD_SPAWN_BATCH, "args": {"rows": [ok]}})

    def test_spawn_batch_request_size_checked_before_rows(self):  # agent protocol 1 MiB cap, step 1
        b = rr.ModelBackend(CAP)
        b.load_scene(SCENE)
        before = b.scene_digest()
        big = "x" * (limits.REQUEST_BYTES_MAX + 1)
        row = {"values": {"kind": 1, "hp": 1, "reload": 1, "damage": 1, "flight": 1}, "pad": big}
        with self.assertRaises(ModelError) as cm:
            b.apply({"cmd": rr.CMD_SPAWN_BATCH, "args": {"rows": [row]}})
        self.assertEqual(cm.exception.code, "SU_E_RESOURCE_LIMIT")
        self.assertEqual(b.scene_digest(), before)

    def test_set_fields_refuses_stale_handle(self):
        b = rr.ModelBackend(CAP)
        b.load_scene(SCENE)
        with self.assertRaises(ModelError) as cm:
            b.apply({"cmd": rr.CMD_SET_FIELDS,
                     "args": {"entities": [{"entity": handle(0, gen=9), "values": {"flight": 2}}]}})
        self.assertEqual(cm.exception.code, "SU_E_STALE_HANDLE")


class BatchLimits(unittest.TestCase):
    def test_request_size_refused_before_rows(self):
        with self.assertRaises(limits.BatchRefused) as cm:
            limits.check_request_size(b"x" * (limits.REQUEST_BYTES_MAX + 1))
        self.assertEqual(cm.exception.code, "SU_E_RESOURCE_LIMIT")
        limits.check_request_size(b"x" * limits.REQUEST_BYTES_MAX)

    def test_row_count_range(self):
        limits.check_row_count(1)
        limits.check_row_count(limits.ROWS_MAX)
        for bad in (0, limits.ROWS_MAX + 1):
            with self.assertRaises(limits.BatchRefused):
                limits.check_row_count(bad)

    def test_free_slot_and_high_water_prechecks(self):
        limits.check_prechecks(rows=4, capacity=10, alive=6, high_water=10)
        with self.assertRaises(limits.BatchRefused):
            limits.check_prechecks(rows=5, capacity=10, alive=6, high_water=10)
        with self.assertRaises(limits.BatchRefused):
            limits.check_prechecks(rows=2, capacity=limits.HW_MAX + 1, alive=limits.HW_MAX - 1,
                                   high_water=limits.HW_MAX - 1)

    def test_worst_case_request_fits_the_cap(self):
        # D6 / C (F4): 1,024 rows, every one of the 39 model fields plus external_id at 10 digits,
        # wrapped as the model sees it (canonical JSON of the whole command), must pass the cap.
        names = model.FIELD_NAMES
        self.assertEqual(len(names), 39)
        row = {"values": {name: 4294967295 for name in names}, "external_id": 4294967295}
        request = rr.canonical_json({"cmd": rr.CMD_SPAWN_BATCH, "args": {"rows": [row] * limits.ROWS_MAX}})
        size = len(request.encode("ascii"))
        self.assertLessEqual(size, limits.REQUEST_BYTES_MAX)
        limits.check_request_size(request.encode("ascii"))

    def test_request_size_boundary_is_exact(self):
        limits.check_request_size(b"x" * limits.REQUEST_BYTES_MAX)
        with self.assertRaises(limits.BatchRefused):
            limits.check_request_size(b"x" * (limits.REQUEST_BYTES_MAX + 1))

    def test_high_water_exact_boundary_is_accepted(self):
        limits.check_prechecks(rows=2, capacity=limits.HW_MAX, alive=limits.HW_MAX - 2, high_water=limits.HW_MAX - 2)
        with self.assertRaises(limits.BatchRefused):
            limits.check_prechecks(rows=3, capacity=limits.HW_MAX + 1, alive=limits.HW_MAX - 2, high_water=limits.HW_MAX - 2)

    def test_holes_at_limit_are_reused_and_retired_slots_are_not(self):
        limits.check_prechecks(rows=1, capacity=limits.HW_MAX, alive=limits.HW_MAX-1, high_water=limits.HW_MAX)
        with self.assertRaises(limits.BatchRefused):
            limits.check_prechecks(rows=1, capacity=limits.HW_MAX, alive=limits.HW_MAX-1,
                                   high_water=limits.HW_MAX, retired=1)

    def test_full_capacity_batch_is_accepted(self):
        limits.check_prechecks(rows=4, capacity=10, alive=6, high_water=10)


if __name__ == "__main__":
    unittest.main()
