# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Q8: real six-system engine versus independent semantic model."""
import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, os.path.join(ROOT, "bench/rts/replay"))
sys.path.insert(0, os.path.join(ROOT, "bench/rts/gen"))
import engine_backend as eb
import model_snapshot as ms
import model
import rtsgen


def compare(engine, reference, t):
    expected = reference.engine_digest(t, engine.rev)
    actual = engine.digest(t)
    if actual != expected:
        # Test-owned spool bytes permit coordinates without adding a public bulk RPC.
        matches = list(Path(engine.test_spool).glob("sutekh-spool-*/*.snap"))
        sha = actual["state"].split(":", 1)[1]
        snapshot = next((p for p in matches if p.name == sha+".snap"), None)
        if snapshot:
            actual_state = json.loads(snapshot.read_bytes())
            for name in model.FIELD_NAMES:
                for slot, value in enumerate(actual_state["columns"][name]):
                    expected_value = reference.world.cols[name][slot]
                    if value != expected_value:
                        raise AssertionError(f"tick {t}, field {name}, slot {slot}: model {expected_value}, engine {value}")
        raise AssertionError(f"tick {t}: metadata/events divergence: {actual} != {expected}")
    return actual


def run(binary, spec, ticks):
    spool = tempfile.TemporaryDirectory(prefix="sutekh-q8-spool-")
    engine = eb.EngineBackend(binary, spec["rows"], event_log_max=7, env={"TMPDIR":spool.name})
    engine.test_spool = spool.name
    reference = ms.SnapshotModel(spec["rows"], 7, engine._inspect()["semantic_manifest"])
    try:
        compare(engine, reference, 0)
        arr, _, scene_sha = rtsgen.make_scene(rtsgen.load_profile(), spec)
        lock = json.loads(Path(ROOT, "bench/rts/scenes.lock.json").read_text())
        expected_scene = next(x for x in lock["scenes"] if x["name"] == spec["name"])
        assert "sha256:"+scene_sha == expected_scene["scene_digest"]
        formations = []
        # Q2 validates references against pre-batch state. Load unchanged slot order,
        # then bind formations once every anchor exists; no simulation between calls.
        for first, count, text in rtsgen.emit_spawn_batches(arr):
            rows = json.loads(text)
            for i, row in enumerate(rows):
                v = row["values"]
                if v["formation_slot"] != ms.NONE:
                    formations.append(dict(entity=(1 << 32) | (first+i),
                                           values={"formation_slot":v["formation_slot"], "formation_gen":v["formation_gen"]}))
                    v["formation_slot"], v["formation_gen"] = ms.NONE, 0
            cmd = dict(cmd="world.spawn_batch", args=dict(rows=rows))
            engine.apply(cmd); reference.apply(cmd)
        for i in range(0, len(formations), 1024):
            cmd = dict(cmd="world.set_fields", args=dict(entities=formations[i:i+1024]))
            engine.apply(cmd); reference.apply(cmd)
        for slot in range(spec["rows"]):
            for f, name in enumerate(model.FIELD_NAMES):
                assert reference.world.cols[name][slot] == arr[slot*39+f], (name, slot)
        compare(engine, reference, 0)
        # Digest sensitivity: a real column mutation and event mutation must diverge.
        original = reference.world.cols["pos_x"][0]
        reference.world.cols["pos_x"][0] ^= 1
        assert engine.digest(0) != reference.engine_digest(0, engine.rev)
        reference.world.cols["pos_x"][0] = original
        reference.world.records[-1]["b"] ^= 1
        assert engine.digest(0) != reference.engine_digest(0, engine.rev)
        reference.world.records[-1]["b"] ^= 1
        final = None
        for t in range(1, ticks+1):
            engine.step(); reference.step(); final = compare(engine, reference, t)
        print(json.dumps(dict(scene=spec["name"], scene_digest="sha256:"+scene_sha,
                              ticks=ticks, result="passed", final=final)), flush=True)
    finally:
        engine.close()
        spool.cleanup()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--max-rows", type=int, default=65536)
    ap.add_argument("--ticks", type=int, default=2)
    args = ap.parse_args()
    # Known handle-retirement boundary, independent of scene hash expectations.
    w = model.World(2)
    row = dict(kind=1, hp=1, reload=1, damage=1, flight=1)
    h = w.spawn(row); w.despawn(h)
    assert w.retired == 0 and w.gen[0] == 2
    w.gen[0] = 0xfffffffe
    h = w.spawn(row); w.despawn(h)
    assert w.retired == 1 and w.gen[0] == 0xffffffff
    assert w.spawn(row) & 0xffffffff == 1
    for spec in rtsgen.SCENE_SPECS:
        if spec["rows"] <= args.max_rows:
            run(os.path.abspath(args.binary), spec, args.ticks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
