# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-06 RPC edge slices with explicit outcomes and full independent digest equality."""
import argparse
import json
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/"bench/rts/replay"))
sys.path.insert(0, str(ROOT/"tests/rts/replay"))
import engine_backend as eb
import model_snapshot as ms
from test_model_snapshot import compare


def unit(team=0, x=100, hp=100, damage=3, flight=1, range_sq=10000):
    return dict(kind=1, team=team, hp=hp, pos_x=x, pos_y=100, speed=0,
                reload=100, damage=damage, flight=flight, range_sq=range_sq)


class Twin:
    def __init__(self, binary, capacity, workers):
        self.spool = tempfile.TemporaryDirectory(prefix="sutekh-edges-")
        self.e = eb.EngineBackend(binary, capacity, event_log_max=7,
                                  env={"TMPDIR":self.spool.name, "SUTEKH_TICK_WORKERS":str(workers), "SUTEKH_TICK_CHUNK_MIN_ROWS":"1"})
        self.e.test_spool = self.spool.name
        self.m = ms.SnapshotModel(capacity, 7, self.e._inspect()["semantic_manifest"])
        self.t = 0

    def apply(self, name, args):
        cmd = dict(cmd=name, args=args)
        self.e.apply(cmd); self.m.apply(cmd)
        compare(self.e, self.m, self.t)

    def spawn(self, rows):
        self.apply("world.spawn_batch", dict(rows=[dict(values=r) for r in rows]))

    def step(self, count=1):
        for _ in range(count):
            self.e.step(); self.m.step(); self.t += 1
            compare(self.e, self.m, self.t)

    def close(self):
        self.e.close()
        self.spool.cleanup()


def run(binary, workers):
    results = []
    twin = Twin(binary, 4, workers)
    try:
        twin.spawn([unit(flight=5), unit(team=1, x=110, range_sq=0)])
        twin.step()
        assert twin.m.world.cols["proj_target_gen"][0] == 1
        twin.apply("world.despawn", dict(entity=(1<<32)|1))
        twin.spawn([unit(team=1, x=110, range_sq=0)])
        assert twin.m.world.gen[1] == 2
        twin.step(5)
        assert twin.m.world.cols["hp"][1] == 100  # stale projectile must not damage replacement
        assert twin.m.world.cols["proj_target_gen"][0] == 0
        results.append("stale-projectile-slot-reuse")
    finally: twin.close()
    twin = Twin(binary, 4, workers)
    try:
        anchor = dict(kind=2, pos_x=500, pos_y=100, reload=1, flight=1, damage=1)
        member = unit(x=100, range_sq=0); member["speed"] = 96
        twin.spawn([anchor, member])
        twin.apply("world.set_fields", dict(entities=[dict(entity=(1<<32)|1,
                   values=dict(formation_slot=0, formation_gen=1, formation_ox=16384, formation_oy=16384))]))
        twin.step()
        assert twin.m.world.cols["pos_x"][1] == 196
        twin.apply("world.despawn", dict(entity=1<<32))
        twin.step()
        assert twin.m.world.cols["anchor_valid"][1] == 0 and twin.m.world.cols["pos_x"][1] == 196
        anchor["pos_x"] = 900
        twin.spawn([anchor]); twin.step()
        assert twin.m.world.cols["anchor_valid"][1] == 0 and twin.m.world.cols["pos_x"][1] == 196
        results.append("despawned-and-reused-anchor")
    finally: twin.close()
    twin = Twin(binary, 4, workers)
    try:
        twin.spawn([unit(x=100), unit(x=100), unit(team=1,x=110,hp=3)])
        twin.step(2)
        assert twin.m.world.alive[2] == 0 and twin.m.world.gen[2] == 2
        deaths = [e for e in twin.m.world.records if e["kind"] == 0xfffffffd]
        assert len(deaths) == 1 and deaths[0]["slot"] == 2 and deaths[0]["tick"] == 2
        assert all(twin.m.world.cols[n][2] == 0 for n in ms.model.FIELD_NAMES)
        results.append("two-shooters-one-boundary-kill")
    finally: twin.close()
    twin = Twin(binary, 4, workers)
    try:
        twin.spawn([unit(), unit(team=1,x=90), unit(team=1,x=110)])
        twin.step()
        assert twin.m.world.cols["target_slot"][0] == 1 and twin.m.world.cols["target_dist_sq"][0] == 100
        results.append("equal-distance-lower-slot")
    finally: twin.close()
    twin = Twin(binary, 2048, workers)
    try:
        twin.spawn([unit() for _ in range(201)] + [unit(team=1,x=164)])
        twin.step()
        assert twin.m.world.cols["target_valid"][0] == 0 and twin.m.world.cols["truncated"][0] == 1
        results.append("ring-zero-friendly-budget-exhaustion")
    finally: twin.close()
    print(json.dumps(dict(workers=workers, status="passed", fixtures=results)))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--binary", required=True); args = ap.parse_args()
    for workers in (1,2): run(str(Path(args.binary).resolve()), workers)
    return 0

if __name__ == "__main__": sys.exit(main())
