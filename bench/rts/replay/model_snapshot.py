# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Independent model encoding of snapshot 0.2 for Q8 comparisons.

Metadata is pinned from the engine at creation; rows and event history come only
from the semantic Python model. This does not replace the standalone model digest.
No migration/schema-history encoding is supported by this RTS comparison lane.
"""
import hashlib
import json
from collections import deque

import rts_replay as rr
import model

NONE = 0xffffffff


class SnapshotWorld(model.World):
    def __init__(self, capacity, event_log_max):
        super().__init__(capacity, event_log_max)
        self.external_ids = [0] * capacity
        self.records = deque(maxlen=event_log_max)
        self.boundary = False
        self.spawned = []

    def spawn(self, spec):
        handle = super().spawn(spec)
        self.spawned.append(handle & NONE)
        return handle

    def _log(self, kind, slot, gen):
        self.records.append(dict(event_seq=self.event_next_seq, tick=self.tick_no + int(self.boundary),
                                 kind=NONE-1 if kind == "SU_EVENT_SPAWNED" else NONE-2,
                                 system_index=5 if self.boundary else NONE,
                                 slot=slot, seq=0, a=slot, b=gen))
        super()._log(kind, slot, gen)

    def _despawn_slot(self, slot):
        super()._despawn_slot(slot)
        self.external_ids[slot] = 0


class SnapshotModel(rr.ModelBackend):
    """Model backend with explicit engine identity metadata, never engine state."""
    def __init__(self, capacity, event_log_max, manifest):
        self.world = SnapshotWorld(capacity, event_log_max)
        self.manifest = [{k: m[k] for k in ("system_id", "semantic_hash")} for m in manifest]

    def apply(self, cmd):
        self.world.spawned = []
        super().apply(cmd)
        if cmd["cmd"] == rr.CMD_SPAWN_BATCH:
            for row, slot in zip(cmd["args"]["rows"], self.world.spawned):
                self.world.external_ids[slot] = row.get("external_id", 0)

    def step(self):
        self.world.boundary = True
        try:
            super().step()
        finally:
            self.world.boundary = False

    def snapshot_bytes(self, revision):
        w = self.world
        obj = dict(snapshot_format="0.2", world_revision=revision, tick=w.tick_no,
                   schema_id="rts", schema_version=1, layout_generation=1,
                   numeric_profile="u32_mod_v1",
                   fields=[dict(id=i+1, name=n, type="u32") for i,n in enumerate(model.FIELD_NAMES)],
                   semantic_manifest=self.manifest,
                   entity_ids=w.external_ids[:w.hw],
                   columns={n: w.cols[n][:w.hw] for n in model.FIELD_NAMES},
                   capacity=w.cap, high_water=w.hw, free_policy="lowest_slot",
                   retired_count=w.retired,
                   slots=[[w.gen[s], w.alive[s], w.external_ids[s]] for s in range(w.hw)],
                   event_next_seq=w.event_next_seq, event_dropped=w.dropped,
                   event_log_max=w.event_log_max,
                   events=[[e[k] for k in ("event_seq", "tick", "kind", "system_index", "slot", "seq", "a", "b")]
                           for e in w.records])
        return json.dumps(obj, separators=(",", ":"), ensure_ascii=True).encode("ascii")

    def engine_digest(self, t, revision):
        w = self.world
        h = hashlib.sha256()
        for e in w.records:
            h.update((json.dumps(e, sort_keys=True, separators=(",", ":"))+"\n").encode("ascii"))
        return dict(t=t, state="sha256:"+hashlib.sha256(self.snapshot_bytes(revision)).hexdigest(),
                    events="sha256:"+h.hexdigest(),
                    counts=dict(alive=sum(w.alive), retired=w.retired, events=len(w.records),
                                dropped=w.dropped, truncated_rows=w.last_truncated_rows))
