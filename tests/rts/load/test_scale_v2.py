# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Real supervisor limits-v2 scale gate; explicit requests, capture, reuse and optional recovery.

This is a long validation tool, not default CI. No model-oracle or performance claim.
"""
import argparse
import gc
import hashlib
import json
import sys
import tempfile
import time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/"bench/rts/replay"))
import rts_session as rs
import model


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--recover", action="store_true")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    started = time.monotonic()
    record = dict(format="rts-scale-v2/0.1", rows=args.rows, status="running", calls=0,
                  performance_qualification=False, recovery_requested=args.recover)
    output = Path(args.out)
    def save():
        record["elapsed_seconds"] = time.monotonic()-started
        output.write_text(json.dumps(record,indent=2)+"\n", newline="\n")
    with tempfile.TemporaryDirectory(prefix="sutekh-scale-spool-") as spool:
        s = rs.Session(str(Path(args.binary).resolve()), env={"TMPDIR":spool})
        try:
            wid, rev = s.create_rts(args.rows)
            record["limits"] = s.call("engine.info", {})["recovery"]["limits"]
            row = dict(values=dict(kind=1,hp=1,reload=1,damage=1,flight=1))
            loaded = 0
            while loaded < args.rows:
                n = min(1024,args.rows-loaded)
                response = s.call("world.spawn_batch",dict(world_id=wid,expected_world_revision=rev,rows=[row]*n))
                assert response["spawned"] == n
                assert response["entities"] == [(1<<32)|(loaded+i) for i in range(n)]
                rev = response["world_revision"]; loaded += n; record["calls"] += 1
                if record["calls"] % 64 == 0:
                    record["accepted_rows"] = loaded; save(); print("loaded",loaded,flush=True)
            assert record["calls"] == (args.rows+1023)//1024
            ins = s.call("world.inspect",dict(world_id=wid,limit=1))
            assert ins["entities"]["alive"] == args.rows and ins["entities"]["high_water"] == args.rows
            snap = s.call("world.snapshot",dict(world_id=wid,expected_world_revision=rev))
            paths = list(Path(spool).glob("sutekh-spool-*/"+snap["sha256"]+".snap"))
            assert len(paths)==1
            data = paths[0].read_bytes()
            assert hashlib.sha256(data).hexdigest() == snap["sha256"]
            state = json.loads(data); del data
            expected = model.validate_spawn(row["values"],args.rows)
            assert state["high_water"]==args.rows and state["capacity"]==args.rows
            for name in model.FIELD_NAMES:
                assert len(state["columns"][name]) == args.rows
                assert all(v==expected[name] for v in state["columns"][name]),name
            assert all(slot==[1,1,0] for slot in state["slots"])
            assert state["entity_ids"] == [0]*args.rows
            kept = min(65536,args.rows)
            assert len(state["events"])==kept and state["event_next_seq"]==args.rows
            assert state["event_dropped"]==args.rows-kept
            for seq,e in enumerate(state["events"],args.rows-kept):
                assert e == [seq,0,0xfffffffe,0xffffffff,seq,0,seq,1]
            record["capture"] = snap; record["accepted_rows"]=loaded; save()
            del state; gc.collect()
            # At full capacity an extra row refuses without changing revision, digest or event counters.
            try:
                s.call("world.spawn_batch",dict(world_id=wid,expected_world_revision=rev,rows=[row]))
                raise AssertionError("full capacity admitted an extra row")
            except rs.RpcError as error:
                assert error.name=="SU_E_RESOURCE_LIMIT"
            assert s.snapshot_sha(wid,rev)==snap["sha256"]
            # Reuse at the high-water ceiling must be admitted under the exact growth precheck.
            response=s.call("world.despawn",dict(world_id=wid,expected_world_revision=rev,entity=(1<<32)|3))
            rev=response["world_revision"]
            response=s.call("world.spawn_batch",dict(world_id=wid,expected_world_revision=rev,rows=[row]))
            rev=response["world_revision"]; assert response["entities"]==[(2<<32)|3]
            before=s.snapshot_sha(wid,rev)
            if args.recover:
                old=s.kill_worker()
                try:
                    s.call("world.inspect",dict(world_id=wid,limit=1))
                except rs.RpcError as error:
                    assert error.name=="SU_E_WORKER_FAILED",error
                ins=s.call("world.inspect",dict(world_id=wid,limit=1))
                assert ins["worker_pid"]!=old and ins["world_revision"]==rev
                assert ins["entities"]["alive"]==args.rows
                assert s.snapshot_sha(wid,rev)==before
                record["recovery"]="passed_replacement_worker_checkpoint_restore"
            response=s.call("world.step",dict(world_id=wid,expected_world_revision=rev,ticks=1))
            assert response["ticks_completed"]==1 and response["tick"]==1
            record["status"]="passed";save();print(json.dumps(record),flush=True)
        except BaseException as error:
            record["status"]="failed";record["error"]=repr(error);save();raise
        finally:s.close()
    return 0

if __name__=="__main__":sys.exit(main())
