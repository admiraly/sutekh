# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Digests in the shape of the rts-replay/0.1 digest record (contract M3 sections 9.1 and 9.2).

The state digest here is the model's own canonical encoding (tag rts-model-state/0.1): the entity
table and the 39 columns for slots below the high-water mark, u32 little-endian, plus the retired
count and the event counters. It is NOT the engine's M2 recovery digest. The engine encoding (M2-00
v2, layout generation and schema descriptor hash) is not read here; byte equality with the engine
is an M3-06 check and is recorded as unrun.
"""
from __future__ import annotations

import hashlib
import json
import struct

from model import FIELD_NAMES, World

STATE_TAG = b"rts-model-state/0.1\x00"
MODEL_LAYOUT_GENERATION = 1  # model-defined; the engine value is not used here


def canonical_json(obj) -> str:
    """Sorted keys, no whitespace, decimal integers (9.2)."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"))


def state_bytes(world: World) -> bytes:
    hw = world.hw
    header = STATE_TAG + struct.pack("<IIIQQI", world.cap, hw, world.retired,
                                     world.event_next_seq, world.dropped, MODEL_LAYOUT_GENERATION)
    values = []
    for s in range(hw):
        values.append(world.alive[s])
        values.append(world.gen[s])
        for name in FIELD_NAMES:
            values.append(world.cols[name][s])
    body = struct.pack("<%dI" % len(values), *values) if values else b""
    return header + body


def state_digest(world: World) -> str:
    return "sha256:" + hashlib.sha256(state_bytes(world)).hexdigest()


def events_digest(world: World) -> str:
    h = hashlib.sha256()
    for seq, kind, slot, gen in world.events:
        line = canonical_json({"gen": gen, "kind": kind, "seq": seq, "slot": slot})
        h.update(line.encode("ascii") + b"\n")
    return "sha256:" + h.hexdigest()


def digest_record(world: World, t: int) -> dict:
    """One digest record in the 9.1 shape: t, state, events, counts."""
    return {
        "t": t,
        "state": state_digest(world),
        "events": events_digest(world),
        "counts": {
            "alive": sum(world.alive),
            "retired": world.retired,
            "events": len(world.events),
            "dropped": world.dropped,
            "truncated_rows": world.last_truncated_rows,
        },
    }
