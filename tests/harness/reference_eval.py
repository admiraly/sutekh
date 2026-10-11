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
