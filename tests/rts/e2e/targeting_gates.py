# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-06 committed congested-scene targeting gates, backed by full engine/model equality."""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
for rel in ("bench/rts/gen", "bench/rts/ref", "tests/rts/replay"):
    sys.path.insert(0, str(ROOT / rel))
import rtsgen
import model
import oracle
import test_model_snapshot

SEED = 20261012


def measure(spec):
    arr, _, digest = rtsgen.make_scene(rtsgen.load_profile(), spec)
    w = model.World(spec["rows"])
    w.hw = w.cap
    w.alive = [1] * w.cap
    for i, name in enumerate(model.FIELD_NAMES):
        w.cols[name] = list(arr[i::39])
    queriers = oracle.sample_queriers(w, 1024, SEED)
    # OD-6 amendment: sample up to 1024 distinct armed units; use every eligible unit if fewer.
    metrics = oracle.targeting_metrics(w, queriers, "committed:"+spec["name"])
    metrics["requested_queriers"] = 1024
    metrics["sample_count_met"] = len(queriers) == min(1024, len(oracle.armed_units(w)))
    metrics["gates_applicable"] = metrics["sample_count_met"]
    metrics["gates"] = oracle.gate_verdicts(metrics)
    metrics["thin_margin"] = metrics["thin_margin_point_estimate"]
    metrics["owner_decision"] = "OD-6" if metrics["thin_margin"] or not metrics["gates"]["pass"] else None
    metrics["sampling_limitation"] = None if metrics["sample_count_met"] else "sampling invariant failed"
    metrics["querier_seed"] = SEED
    metrics["querier_sha256"] = hashlib.sha256(json.dumps(queriers, separators=(",", ":")).encode()).hexdigest()
    metrics["scene_digest"] = "sha256:"+digest
    return metrics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    binary = Path(args.binary).resolve()
    start = time.monotonic()
    records = []
    for spec in rtsgen.SCENE_SPECS:
        if spec["generator"] != "congested.blob.v1":
            continue
        # This checks the real engine through all phases at the same committed input,
        # including the tick-one spatial outputs used by the independent metrics.
        test_model_snapshot.run(str(binary), spec, 1)
        records.append(measure(spec))
    assert len(records) == 2
    result = dict(format="rts-targeting-gates/0.1", source_revision=subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        artifact_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        source_hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
                       [Path(__file__), ROOT/"bench/rts/ref/model.py", ROOT/"bench/rts/ref/oracle.py", ROOT/"bench/rts/gen/rtsgen.py", ROOT/"bench/rts/scenes.lock.json"]},
        environment=dict(os=platform.platform(), machine=platform.machine(), python=platform.python_version(),
                         cpu=os.cpu_count(), toolchain=subprocess.check_output(["cc", "--version"],text=True).splitlines()[0]),
        elapsed_seconds=time.monotonic()-start, performance_qualification=False, records=records)
    Path(args.out).write_text(json.dumps(result, indent=2, allow_nan=False)+"\n")
    print(json.dumps(result, allow_nan=False))
    return 0 if all(m["gates"]["pass"] and m["sample_count_met"] for m in records) else 1


if __name__ == "__main__":
    sys.exit(main())
