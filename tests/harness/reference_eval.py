# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Independent reference evaluator for SU-LIR 0.1 known answers (standard library only).

This module is written separately from tools/make_fixtures.py and does not import
it. The M0 harness uses it to derive expected columns from fixture inputs, so an
engine result is never compared with values that the engine itself produced.

Semantics (LIVE_IR_SPEC / M0_CONTRACT section 1, profile u32_mod_v1):
  add/sub/mul are modulo 2^32 (sub does not saturate); lt is unsigned; eq is exact;
  select picks one of two already-computed u32 values; bool never coerces to u32.
  Loads read the columns as they were at the start of the tick. A row runs the
  whole body in order. Stores are committed only after every row has run.
"""
from __future__ import annotations

MASK = (1 << 32) - 1
U32_MAX = MASK


class ReferenceError(Exception):
    """The program or inputs are outside the known-valid subset this evaluator handles."""


def _is_u32(value) -> bool:
    # bool is a subclass of int in Python; JSON true/false must never count as u32.
    return type(value) is int and 0 <= value <= MASK


def _need_u32(name: str, value) -> int:
    if not _is_u32(value):
        raise ReferenceError(f"value {value!r} for {name} is not a u32")
    return value


def _fields_of(capsule: dict) -> dict:
    return {f["name"]: {"read": bool(f["read"]), "write": bool(f["write"])} for f in capsule["fields"]}


def _check_static(program: dict, fields: dict) -> list[str]:
    """Return the writable fields that the body stores (exactly once each), after permission checks."""
    store_count: dict[str, int] = {}
    for ins in program["instructions"]:
        op = ins["op"]
        if op == "load_u32":
            if not fields.get(ins["field"], {}).get("read"):
                raise ReferenceError(f"load of unreadable field {ins['field']}")
        elif op == "store_u32":
            if not fields.get(ins["field"], {}).get("write"):
                raise ReferenceError(f"store to unwritable field {ins['field']}")
            store_count[ins["field"]] = store_count.get(ins["field"], 0) + 1
    for name, meta in fields.items():
        if meta["write"] and store_count.get(name, 0) != 1:
            raise ReferenceError(f"writable field {name} stored {store_count.get(name, 0)} times")
    return sorted(store_count)


def run_tick(program: dict, capsule: dict, columns: dict) -> dict:
    """Run one tick over every row and return the new columns (inputs are not modified)."""
    fields = _fields_of(capsule)
    written = _check_static(program, fields)
    lengths = {len(v) for v in columns.values()}
    if len(lengths) > 1:
        raise ReferenceError("columns have different lengths")
    rows = lengths.pop() if lengths else 0
    for name, values in columns.items():
        for v in values:
            _need_u32(name, v)

    staged = {name: [0] * rows for name in written}
    for row in range(rows):
        regs: dict[str, tuple[str, int]] = {}
        for ins in program["instructions"]:
            op = ins["op"]
            if op == "const_u32":
                regs[ins["dst"]] = ("u32", _need_u32("const", ins["value"]))
            elif op == "load_u32":
                regs[ins["dst"]] = ("u32", columns[ins["field"]][row])
            elif op in ("add_u32", "sub_u32", "mul_u32", "eq_u32", "lt_u32"):
                a = _reg(regs, ins["a"], "u32")
                b = _reg(regs, ins["b"], "u32")
                if op == "add_u32":
                    out = ("u32", (a + b) & MASK)
                elif op == "sub_u32":
                    out = ("u32", (a - b) & MASK)
                elif op == "mul_u32":
                    out = ("u32", (a * b) & MASK)
                elif op == "eq_u32":
                    out = ("bool", int(a == b))
                else:
                    out = ("bool", int(a < b))
                regs[ins["dst"]] = out
            elif op == "select_u32":
                cond = _reg(regs, ins["cond"], "bool")
                picked = ins["on_true"] if cond else ins["on_false"]
                regs[ins["dst"]] = ("u32", _reg(regs, picked, "u32"))
            elif op == "store_u32":
                staged[ins["field"]][row] = _reg(regs, ins["src"], "u32")
            else:
                raise ReferenceError(f"unknown opcode {op}")

    out = {name: list(values) for name, values in columns.items()}
    out.update(staged)
    return out


def _reg(regs: dict, name: str, kind: str) -> int:
    if name not in regs:
        raise ReferenceError(f"undefined register {name}")
    tag, value = regs[name]
    if tag != kind:
        raise ReferenceError(f"register {name} is {tag}, expected {kind}")
    return value


def run_ticks(program: dict, capsule: dict, columns: dict, ticks: int) -> dict:
    for _ in range(ticks):
        columns = run_tick(program, capsule, columns)
    return columns


# ---------------------------------------------------------------------------------------------
# M2-00c3b entity model (contract M2_ENTITIES_CONTRACT.md sections 1, 2, 5 and 6a; IR 0.2
# section 3). Written from the contract text, not from the engine. It models the entity table
# (generations, lowest-slot reuse, retirement), the bounded event ring, the per-tick caps, and a
# straight-line IR 0.2 tick: despawn_self and emit_event records share one per-row sequence; the
# boundary applies requests in canonical (system_index, slot, seq) order; a stale request is
# refused with an SU_EVENT_REJECTED record at its position; any cap overflow aborts the tick with
# SU_E_RESOURCE_LIMIT and changes nothing.
# ---------------------------------------------------------------------------------------------
GEN_FIRST = 1
GEN_RETIRED = 0xFFFFFFFF
CHUNK_SLOTS = 1024
STATUS_STALE = 21          # SU_E_STALE_HANDLE
STATUS_RESOURCE = 11       # SU_E_RESOURCE_LIMIT
STATUS_INVALID = 4         # SU_E_INVALID_ARGUMENT
STRUCT_DESPAWN = 1
EV_REJECTED = 0xFFFFFFFF
EV_SPAWNED = 0xFFFFFFFE
EV_DESPAWNED = 0xFFFFFFFD
SYSTEM_HOST = 0xFFFFFFFF


class EntityError(Exception):
    """A control-plane or tick request the engine refuses; status is the SU_E_* code."""

    def __init__(self, status: int, message: str = ""):
        super().__init__(message or f"status {status}")
        self.status = status


def handle_of(slot: int, gen: int) -> int:
    return ((gen & MASK) << 32) | (slot & MASK)


class EntityTable:
    """Generations and liveness per slot. Slots >= high_water are dead, generation 1, zero."""

    def __init__(self, capacity: int, initial_ids: list[int]):
        n = len(initial_ids)
        if capacity < n:
            raise ReferenceError("capacity below the initial row count")
        self.capacity = capacity
        self.gen = [GEN_FIRST] * capacity
        self.alive = [False] * capacity
        self.ext = [0] * capacity
        for i, ext in enumerate(initial_ids):
            self.alive[i] = True
            self.ext[i] = ext
        self.high_water = n

    def retired_count(self) -> int:
        return sum(1 for g in self.gen if g == GEN_RETIRED)

    def alive_count(self) -> int:
        return sum(1 for a in self.alive if a)

    def lowest_free(self) -> int | None:
        for s in range(self.capacity):
            if not self.alive[s] and self.gen[s] != GEN_RETIRED:
                return s
        return None

    def spawn(self, ext: int = 0) -> int:
        slot = self.lowest_free()
        if slot is None:
            raise EntityError(STATUS_RESOURCE, "no free slot")
        self.alive[slot] = True
        self.ext[slot] = ext
        self.high_water = max(self.high_water, slot + 1)
        return handle_of(slot, self.gen[slot])

    def check(self, handle: int) -> int:
        """Slot of a live handle, or EntityError(SU_E_STALE_HANDLE)."""
        gen = (handle >> 32) & MASK
        slot = handle & MASK
        if gen == 0 or slot >= self.capacity or not self.alive[slot]:
            raise EntityError(STATUS_STALE, "stale handle")
        if self.gen[slot] == GEN_RETIRED or self.gen[slot] != gen:
            raise EntityError(STATUS_STALE, "stale handle")
        return slot

    def despawn(self, handle: int) -> int:
        """Returns the old generation. Stores gen+1, or RETIRED when gen+1 would be 0xFFFFFFFF."""
        slot = self.check(handle)
        old = self.gen[slot]
        self.alive[slot] = False
        self.gen[slot] = GEN_RETIRED if old + 1 == GEN_RETIRED else old + 1
        return old


class EventRing:
    """Bounded ring of retained records. Records are dicts with the fields of SuEvent."""

    def __init__(self, capacity: int):
        self.capacity = capacity
        self.records: list[dict] = []
        self.next_seq = 0
        self.dropped = 0

    def append(self, rec: dict) -> None:
        rec = dict(rec)
        rec["event_seq"] = self.next_seq
        self.next_seq += 1
        self.records.append(rec)
        if len(self.records) > self.capacity:
            self.records.pop(0)
            self.dropped += 1

    def read(self, cursor: int, maximum: int) -> dict:
        """events.read semantics (contract section 2): cursor below the oldest retained seq starts at the
        oldest record and reports skipped = oldest - cursor; a cursor above next_seq is refused."""
        if cursor > self.next_seq:
            raise EntityError(STATUS_INVALID, "cursor is ahead of the next sequence")
        oldest = self.dropped
        if cursor < oldest:
            skipped, start = oldest - cursor, oldest
        else:
            skipped, start = 0, cursor
        avail = self.next_seq - start
        n = min(avail, maximum)
        events = self.records[start - oldest:start - oldest + n]
        return {"events": events, "next_cursor": start + n, "skipped": skipped, "dropped": self.dropped}


class EntityWorld:
    """Columns, entity table and event ring of one world, plus the committed tick count."""

    def __init__(self, fields: list[dict], columns: dict, entity_ids: list[int], capacity: int,
                 max_struct_tick: int, max_events_tick: int, log_max: int):
        self.field_names = [f["name"] for f in fields]
        n = len(entity_ids)
        self.capacity = capacity
        self.initial_rows = n          # rows world.inspect pages over (the fixture's row count)
        self.table = EntityTable(capacity, entity_ids)
        self.cols = {name: [0] * capacity for name in self.field_names}
        for name in self.field_names:
            for i in range(n):
                self.cols[name][i] = columns[name][i]
        self.ring = EventRing(log_max)
        self.tick = 0
        self.max_struct_tick = max_struct_tick
        self.max_events_tick = max_events_tick

    def spawn(self, values: dict, ext: int | None = None) -> int:
        for name in values:
            if name not in self.cols:
                raise EntityError(STATUS_INVALID, "unknown field")
            if type(values[name]) is not int or not 0 <= values[name] <= MASK:
                raise EntityError(STATUS_INVALID, "value is not u32")
        handle = self.table.spawn(0 if ext is None else ext)
        slot = handle & MASK
        for name in self.field_names:
            self.cols[name][slot] = values.get(name, 0)
        self.ring.append({"tick": self.tick, "kind": EV_SPAWNED, "system_index": SYSTEM_HOST, "slot": slot,
                          "seq": 0, "a": slot, "b": (handle >> 32)})
        return handle

    def despawn(self, handle: int) -> None:
        old = self.table.despawn(handle)
        slot = handle & MASK
        for name in self.field_names:
            self.cols[name][slot] = 0
        self.ring.append({"tick": self.tick, "kind": EV_DESPAWNED, "system_index": SYSTEM_HOST, "slot": slot,
                          "seq": 0, "a": slot, "b": old})

    def tick_systems(self, systems: list[tuple[dict, dict]], ticks: int = 1) -> None:
        """Runs ticks of straight-line IR 0.2 systems, each (program, capsule). Atomic per tick: a cap
        overflow raises EntityError(SU_E_RESOURCE_LIMIT) and leaves every field unchanged."""
        for _ in range(ticks):
            self._tick_once(systems)

    def _tick_once(self, systems: list[tuple[dict, dict]]) -> None:
        tick_low = (self.tick + 1) & MASK
        alive = self.table.alive[:]
        gen_view = self.table.gen[:]
        cols = {k: list(v) for k, v in self.cols.items()}
        structs: list[dict] = []    # {system_index, slot, seq, kind, arg}
        events: list[dict] = []     # {system_index, slot, seq, kind, a, b}
        n_struct_tick = 0
        for sys_index, (program, capsule) in enumerate(systems):
            written = _check_static(program, _fields_of(capsule))
            staged = {name: list(cols[name]) for name in written}
            per_row_struct_cap = capsule["limits"].get("max_struct_per_row", 0)
            per_row_event_cap = capsule["limits"].get("max_events_per_row", 0)
            for row in range(self.capacity):
                if not alive[row]:
                    continue
                regs: dict[str, tuple[str, int]] = {}
                seq = 0            # one per-row sequence shared by despawn_self and emit_event
                n_row_struct = 0   # per-kind counts for the per-row caps (contract r2.1 section 6a)
                n_row_event = 0
                for ins in program["instructions"]:
                    op = ins["op"]
                    if op == "const_u32":
                        regs[ins["dst"]] = ("u32", _need_u32("const", ins["value"]))
                    elif op == "load_u32":
                        regs[ins["dst"]] = ("u32", cols[ins["field"]][row])
                    elif op == "slot_u32":
                        regs[ins["dst"]] = ("u32", row)
                    elif op == "gen_u32":
                        regs[ins["dst"]] = ("u32", gen_view[row])
                    elif op == "tick_u32":
                        regs[ins["dst"]] = ("u32", tick_low)
                    elif op in ("add_u32", "sub_u32", "mul_u32", "eq_u32", "lt_u32"):
                        a = _reg(regs, ins["a"], "u32")
                        b = _reg(regs, ins["b"], "u32")
                        if op == "add_u32":
                            out = ("u32", (a + b) & MASK)
                        elif op == "sub_u32":
                            out = ("u32", (a - b) & MASK)
                        elif op == "mul_u32":
                            out = ("u32", (a * b) & MASK)
                        elif op == "eq_u32":
                            out = ("bool", int(a == b))
                        else:
                            out = ("bool", int(a < b))
                        regs[ins["dst"]] = out
                    elif op == "select_u32":
                        cond = _reg(regs, ins["cond"], "bool")
                        picked = ins["on_true"] if cond else ins["on_false"]
                        regs[ins["dst"]] = ("u32", _reg(regs, picked, "u32"))
                    elif op == "store_u32":
                        staged[ins["field"]][row] = _reg(regs, ins["src"], "u32")
                    elif op == "despawn_self":
                        if _reg(regs, ins["cond"], "bool"):
                            if n_row_struct >= per_row_struct_cap:
                                raise EntityError(STATUS_RESOURCE, "per-row struct cap")
                            structs.append({"system_index": sys_index, "slot": row, "seq": seq,
                                            "kind": STRUCT_DESPAWN, "arg": handle_of(row, gen_view[row])})
                            seq += 1
                            n_row_struct += 1
                    elif op == "emit_event":
                        if _reg(regs, ins["cond"], "bool"):
                            if n_row_event >= per_row_event_cap:
                                raise EntityError(STATUS_RESOURCE, "per-row event cap")
                            pa = _reg(regs, ins["a"], "u32")
                            pb = _reg(regs, ins["b"], "u32")
                            events.append({"system_index": sys_index, "slot": row, "seq": seq,
                                           "kind": _need_u32("kind", ins["kind"]), "a": pa, "b": pb})
                            seq += 1
                            n_row_event += 1
                    else:
                        raise ReferenceError(f"unknown opcode {op}")
            # Commit this system's staging (alive rows only), then zero dead rows of the working root.
            for name, values in staged.items():
                cols[name] = values
            for row in range(self.capacity):
                if not alive[row]:
                    for name in self.field_names:
                        cols[name][row] = 0
        n_struct_tick = len(structs)
        # Caps (before any boundary action): buffer capacity per (system, chunk) and the per-tick caps.
        if n_struct_tick > self.max_struct_tick:
            raise EntityError(STATUS_RESOURCE, "per-tick struct cap")
        if len(events) + n_struct_tick > self.max_events_tick:
            raise EntityError(STATUS_RESOURCE, "per-tick event cap")
        # Canonical merge and apply, on a copy of the table so a refusal aborts with no change.
        table_copy = EntityTable.__new__(EntityTable)
        table_copy.__dict__.update({k: (list(v) if isinstance(v, list) else v) for k, v in self.table.__dict__.items()})
        recs: list[tuple[tuple[int, int, int, int], dict]] = []
        for ev in events:
            recs.append(((ev["system_index"], ev["slot"], ev["seq"], 0), ev))
        for st in structs:
            recs.append(((st["system_index"], st["slot"], st["seq"], 1), st))
        recs.sort(key=lambda item: item[0])
        out_records: list[dict] = []
        despawned_slots: list[int] = []
        for _key, item in recs:
            if "arg" in item:
                try:
                    old = table_copy.despawn(item["arg"])
                    despawned_slots.append(item["arg"] & MASK)
                    out_records.append({"tick": tick_low, "kind": EV_DESPAWNED, "system_index": item["system_index"],
                                        "slot": item["slot"], "seq": item["seq"], "a": item["arg"] & MASK,
                                        "b": old})
                except EntityError as exc:
                    out_records.append({"tick": tick_low, "kind": EV_REJECTED, "system_index": item["system_index"],
                                        "slot": item["slot"], "seq": item["seq"], "a": item["kind"],
                                        "b": exc.status})
            else:
                out_records.append({"tick": tick_low, "kind": item["kind"], "system_index": item["system_index"],
                                    "slot": item["slot"], "seq": item["seq"], "a": item["a"], "b": item["b"]})
        for slot in despawned_slots:
            for name in self.field_names:
                cols[name][slot] = 0
        for rec in out_records:
            self.ring.append(rec)
        self.table = table_copy
        self.cols = cols
        self.tick += 1


def self_check_entity_model() -> list[str]:
    """Hand-derived entity answers (contract sections 1, 2 and 6a). Returns failure messages (empty when all
    hold). The fixtures are read from tests/fixtures/entities next to this directory."""
    import json
    import os

    fails: list[str] = []

    def want(name, expected, actual):
        if expected != actual:
            fails.append(f"{name}: expected {expected!r} actual {actual!r}")

    def raises(name, status, fn):
        try:
            fn()
        except EntityError as exc:
            want(name, status, exc.status)
            return
        fails.append(f"{name}: no refusal, expected status {status}")

    # Lowest free slot, generation bump, stale handles.
    t = EntityTable(4, [1, 2])
    want("spawn lowest free slot (slot 2, gen 1)", handle_of(2, 1), t.spawn())
    old = t.despawn(handle_of(1, 1))
    want("despawn returns the old generation", 1, old)
    want("reuse lowest slot with the bumped generation", handle_of(1, 2), t.spawn())
    raises("old generation of a reused slot is stale", STATUS_STALE, lambda: t.check(handle_of(1, 1)))
    raises("generation 0 is stale", STATUS_STALE, lambda: t.check(2))
    raises("slot out of range is stale", STATUS_STALE, lambda: t.check(handle_of(9, 1)))
    raises("zero handle is stale", STATUS_STALE, lambda: t.check(0))

    # Retirement at generation 0xFFFFFFFE: the slot is never reissued.
    r = EntityTable(2, [])
    r.gen[0] = GEN_RETIRED - 1
    r.alive[0] = True
    want("despawn at 0xFFFFFFFE retires the slot", GEN_RETIRED - 1, r.despawn(handle_of(0, GEN_RETIRED - 1)))
    want("retired slot is stored as RETIRED", GEN_RETIRED, r.gen[0])
    want("spawn skips the retired slot", handle_of(1, 1), r.spawn())
    raises("no slot left when the only free slot is retired", STATUS_RESOURCE, r.spawn)

    # Bounded ring: capacity 2, three records appended.
    ring = EventRing(2)
    for k in range(3):
        ring.append({"tick": 1, "kind": k, "system_index": 0, "slot": 0, "seq": 0, "a": 0, "b": 0})
    got = ring.read(0, 256)
    want("ring drops the oldest record", 1, ring.dropped)
    want("events.read skipped counts the lost records", 1, got["skipped"])
    want("events.read starts at the oldest retained seq", [1, 2], [e["event_seq"] for e in got["events"]])
    want("events.read next_cursor", 3, got["next_cursor"])
    raises("cursor ahead of the next seq is refused", STATUS_INVALID, lambda: ring.read(4, 1))

    # One tick of the movement fixture (row 0 despawns; its emit follows the despawn record).
    base = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures", "entities")

    def load(name):
        with open(os.path.join(base, name), encoding="utf-8") as fh:
            return json.load(fh)

    fx = load("actors_3.json")
    move_cap, move_lir = load("despawn_move.capsule.json"), load("despawn_move.lir.json")
    guard_cap, guard_lir = load("guard.capsule.json"), load("guard.lir.json")
    w = EntityWorld(move_cap["fields"], fx["columns"], fx["entity_ids"], 3, 4096, 4096, 64)
    w.tick_systems([(move_lir, move_cap)], 1)
    want("movement tick: despawned row 0 is zero", [0, 7, 12], w.cols["position_x"][:3])
    want("movement tick: records in canonical order",
         [(0, 1, EV_DESPAWNED, 0, 0, 0, 0, 1), (1, 1, 7, 0, 0, 1, 0, 1)],
         [(e["event_seq"], e["tick"], e["kind"], e["system_index"], e["slot"], e["seq"], e["a"], e["b"])
          for e in w.ring.records])
    w2 = EntityWorld(move_cap["fields"], fx["columns"], fx["entity_ids"], 3, 4096, 4096, 64)
    w2.tick_systems([(move_lir, move_cap), (guard_lir, guard_cap)], 1)
    want("two systems: the guard's despawn of a row already despawned is refused (REJECTED, status 21)",
         (2, 1, EV_REJECTED, 1, 0, 0, STRUCT_DESPAWN, STATUS_STALE)[:8],
         tuple([w2.ring.records[2][k] for k in ("event_seq", "tick", "kind", "system_index", "slot", "seq", "a", "b")]))
    want("two systems: the guard's emit follows its refused despawn", 9,
         w2.ring.records[3]["kind"])

    # Cap overflow aborts the tick: nothing changes.
    # Rows 0 and 1 both satisfy position < velocity: two struct requests against a per-tick cap of 1.
    w3 = EntityWorld(move_cap["fields"], {"position_x": [0, 1, 9], "velocity_x": [1, 2, 3]}, [1, 2, 3], 3, 1,
                     4096, 64)
    before = ([list(v) for v in w3.cols.values()], w3.table.alive[:], w3.ring.next_seq)
    raises("two struct requests over a per-tick cap of 1 abort the tick", STATUS_RESOURCE,
           lambda: w3.tick_systems([(move_lir, move_cap)], 1))
    want("an aborted tick leaves columns, table and log unchanged",
         before, ([list(v) for v in w3.cols.values()], w3.table.alive[:], w3.ring.next_seq))
    return fails


# Hand-derived scalar answers (not computed by the evaluator). Each entry is
# (opcode, a, b, expected) for the two-operand u32 ops, or (opcode, a, b, bool) for compares.
KNOWN_ANSWERS = [
    ("add_u32", 0xFFFFFFFF, 1, 0),                  # 2^32 wraps to 0
    ("add_u32", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE),  # 2^33-2 mod 2^32
    ("add_u32", 0xFFFFFFFE, 3, 1),                  # 2^32+1 mod 2^32
    ("sub_u32", 0, 1, 0xFFFFFFFF),                  # underflow wraps
    ("sub_u32", 5, 7, 0xFFFFFFFE),
    ("sub_u32", 0xFFFFFFFF, 0xFFFFFFFF, 0),
    ("sub_u32", 7, 0, 7),                           # not saturating
    ("mul_u32", 65536, 65536, 0),                   # 2^32 wraps to 0
    ("mul_u32", 0xFFFFFFFF, 2, 0xFFFFFFFE),
    ("mul_u32", 3, 1431655766, 2),                  # 2^32+2
    ("mul_u32", 0xFFFFFFFF, 0xFFFFFFFF, 1),         # (2^32-1)^2 mod 2^32
    ("mul_u32", 0, 0xFFFFFFFF, 0),
    ("lt_u32", 0, 0, 0),
    ("lt_u32", 0, 0xFFFFFFFF, 1),                   # unsigned
    ("lt_u32", 0xFFFFFFFF, 0, 0),
    ("lt_u32", 0xFFFFFFFF, 0xFFFFFFFF, 0),
    ("lt_u32", 1, 2, 1),
    ("eq_u32", 0, 0, 1),
    ("eq_u32", 0, 0xFFFFFFFF, 0),
    ("eq_u32", 0xFFFFFFFF, 0, 0),
    ("eq_u32", 0xFFFFFFFF, 0xFFFFFFFF, 1),
    ("eq_u32", 1, 2, 0),
]

_BINARY_OPS = {"add_u32", "sub_u32", "mul_u32", "eq_u32", "lt_u32"}


def known_answer_result(op: str, a: int, b: int) -> int:
    """Evaluate one binary op through the same program path used for fixtures."""
    program = {"instructions": [
        {"op": "load_u32", "dst": "a", "field": "position_x"},
        {"op": "load_u32", "dst": "b", "field": "velocity_x"},
        {"op": op, "dst": "r", "a": "a", "b": "b"},
    ]}
    if op in ("eq_u32", "lt_u32"):
        program["instructions"] += [
            {"op": "const_u32", "dst": "one", "value": 1},
            {"op": "const_u32", "dst": "zero", "value": 0},
            {"op": "select_u32", "dst": "out", "cond": "r", "on_true": "one", "on_false": "zero"},
        ]
        result_reg = "out"
    else:
        result_reg = "r"
    program["instructions"].append({"op": "store_u32", "field": "position_x", "src": result_reg})
    capsule = {"fields": [
        {"name": "position_x", "read": True, "write": True},
        {"name": "velocity_x", "read": True, "write": False},
    ]}
    out = run_tick(program, capsule, {"position_x": [a], "velocity_x": [b]})
    return out["position_x"][0]


def self_check_known_answers() -> list[str]:
    """Return mismatch descriptions for KNOWN_ANSWERS (empty list when all agree)."""
    bad = []
    for op, a, b, want in KNOWN_ANSWERS:
        if op not in _BINARY_OPS:
            bad.append(f"unknown op in table {op}")
            continue
        got = known_answer_result(op, a, b)
        if got != want:
            bad.append(f"{op}({a:#x},{b:#x}) evaluator={got:#x} known={want:#x}")
    return bad
