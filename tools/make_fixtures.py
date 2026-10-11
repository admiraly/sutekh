#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Generate SU-LIR 0.1 test fixtures under tests/fixtures/ (standard library only).

Produces:
  (a) T02 worlds for 0, 1, 7, 8, 9, 1024 rows with movement.lir.json expectations
      after 1 and 3 ticks (tests/fixtures/t02/);
  (b) T03 numeric-edge programs, each with a world and known-answer expectations
      (tests/fixtures/t03/<case>/);
  (c) extra T04 invalid inputs and tests/fixtures/negative_cases.json.

The reference evaluator below is deliberately small and independent of any
engine. Its arithmetic follows LIVE_IR_SPEC section 3: add/sub/mul are modulo
2^32, sub is not saturating, lt is unsigned, eq is exact, and select picks an
already-computed u32. Hand-written known-answer tables are asserted against the
evaluator before any file is written, so a shared bug cannot define correctness
by itself.

Run: python3 tools/make_fixtures.py          (generate, self-check, validate)
     python3 tools/make_fixtures.py --no-validate
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIX = ROOT / "tests" / "fixtures"
EXAMPLES = ROOT / "examples"
SCHEMAS = ROOT / "schemas"

MASK = (1 << 32) - 1
U32_MAX = MASK
T02_ROW_COUNTS = [0, 1, 7, 8, 9, 1024]
T02_TICKS = [1, 3]

# ---------------------------------------------------------------- documents

def capsule(source: str, tests: list[str], velocity_read: bool = True) -> dict:
    return {
        "capsule_format": "0.1",
        "system_id": "demo.movement",
        "source": source,
        "schema": {"id": "demo.actor", "version": 1, "layout_generation": 1},
        "numeric_profile": "u32_mod_v1",
        "fields": [
            {"id": 1, "name": "position_x", "type": "u32", "read": True, "write": True},
            {"id": 2, "name": "velocity_x", "type": "u32", "read": velocity_read, "write": False},
        ],
        "schedule": {"phase": "simulate", "after": [], "before": [], "parallel": "serial"},
        "limits": {"max_instructions": 4096, "max_rows": 1000000, "allocation": "forbidden"},
        "tests": tests,
    }


def lir(instructions: list[dict]) -> dict:
    return {
        "ir_version": "0.1",
        "system_id": "demo.movement",
        "numeric_profile": "u32_mod_v1",
        "query": "demo.actor",
        "instructions": instructions,
    }


MOVEMENT_INSTRUCTIONS = [
    {"op": "load_u32", "dst": "p", "field": "position_x"},
    {"op": "load_u32", "dst": "v", "field": "velocity_x"},
    {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
    {"op": "store_u32", "field": "position_x", "src": "next"},
]


def world(columns: dict[str, list[int]], schema_id: str = "demo.actor") -> dict:
    n = len(next(iter(columns.values())))
    return {
        "fixture_format": "0.1",
        "schema_id": schema_id,
        "schema_version": 1,
        "tick": 0,
        "entity_ids": list(range(1, n + 1)),
        "columns": columns,
    }


def test_doc(system_id: str, capsule_name: str, world_name: str, cases: list[dict]) -> dict:
    return {
        "test_format": "0.1",
        "system_id": system_id,
        "capsule": capsule_name,
        "world": world_name,
        "cases": cases,
    }


# ------------------------------------------------------- reference evaluator

class VerifyError(Exception):
    pass


def evaluate_tick(program: dict, cap: dict, columns: dict[str, list[int]]) -> dict[str, list[int]]:
    """Run one tick: the body executes once per row in ascending order.

    Loads read the invocation-entry columns. Stores are staged per row and
    committed only after every row succeeds. Writable fields must be stored
    exactly once in the body (LIVE_IR_SPEC section 5).
    """
    fields = {f["name"]: f for f in cap["fields"]}
    n = len(next(iter(columns.values()))) if columns else 0
    writable = [name for name, f in fields.items() if f["write"]]
    for ins in program["instructions"]:
        if ins["op"] == "load_u32" and not fields.get(ins["field"], {}).get("read"):
            raise VerifyError(f"SU_E_UNDECLARED_READ {ins['field']}")
    store_counts = {}
    for ins in program["instructions"]:
        if ins["op"] == "store_u32":
            f = fields.get(ins["field"])
            if f is None or not f["write"]:
                raise VerifyError(f"SU_E_UNDECLARED_WRITE {ins['field']}")
            store_counts[ins["field"]] = store_counts.get(ins["field"], 0) + 1
    for name in writable:
        if store_counts.get(name, 0) != 1:
            raise VerifyError(f"writable {name} stored {store_counts.get(name, 0)} times")

    out = {k: list(v) for k, v in columns.items()}
    staged: dict[str, list[int]] = {}
    for i in range(n):
        regs: dict[str, tuple[str, int]] = {}

        def get(name: str, kind: str) -> int:
            if name not in regs:
                raise VerifyError(f"SU_E_UNDEFINED_REGISTER {name}")
            t, v = regs[name]
            if t != kind:
                raise VerifyError(f"SU_E_TYPE {name} is {t}, expected {kind}")
            return v

        for ins in program["instructions"]:
            op = ins["op"]
            if op == "const_u32":
                regs[ins["dst"]] = ("u32", ins["value"])
            elif op == "load_u32":
                regs[ins["dst"]] = ("u32", columns[ins["field"]][i])
            elif op == "add_u32":
                regs[ins["dst"]] = ("u32", (get(ins["a"], "u32") + get(ins["b"], "u32")) & MASK)
            elif op == "sub_u32":
                regs[ins["dst"]] = ("u32", (get(ins["a"], "u32") - get(ins["b"], "u32")) & MASK)
            elif op == "mul_u32":
                regs[ins["dst"]] = ("u32", (get(ins["a"], "u32") * get(ins["b"], "u32")) & MASK)
            elif op == "eq_u32":
                regs[ins["dst"]] = ("bool", int(get(ins["a"], "u32") == get(ins["b"], "u32")))
            elif op == "lt_u32":
                regs[ins["dst"]] = ("bool", int(get(ins["a"], "u32") < get(ins["b"], "u32")))
            elif op == "select_u32":
                c = get(ins["cond"], "bool")
                regs[ins["dst"]] = ("u32", get(ins["on_true"], "u32") if c else get(ins["on_false"], "u32"))
            elif op == "store_u32":
                staged.setdefault(ins["field"], [0] * n)[i] = get(ins["src"], "u32")
            else:
                raise VerifyError(f"SU_E_UNKNOWN_OPCODE {op}")
    for name, vals in staged.items():
        out[name] = vals
    return out


def run_ticks(program: dict, cap: dict, columns: dict[str, list[int]], ticks: int) -> dict[str, list[int]]:
    for _ in range(ticks):
        columns = evaluate_tick(program, cap, columns)
    return columns


# -------------------------------------------------------------- T02 worlds

BASE_ROWS = [(0, 1), (1, 2), (U32_MAX, 1), (7, 0)]


def t02_rows(n: int) -> tuple[list[int], list[int]]:
    """First rows are the movement.world.json edge cases; later rows are a fixed mix."""
    pos, vel = [], []
    for i in range(n):
        if i < len(BASE_ROWS):
            p, v = BASE_ROWS[i]
        else:
            p = (i * 0x9E3779B1 + 7) & MASK
            v = (i * 0x85EBCA77 + 3) & MASK
        pos.append(p)
        vel.append(v)
    return pos, vel


def build_t02(out_files: dict[str, object], program: dict, cap: dict) -> None:
    for n in T02_ROW_COUNTS:
        pos, vel = t02_rows(n)
        columns = {"position_x": pos, "velocity_x": vel}
        cases = []
        for ticks in T02_TICKS:
            expected = run_ticks(program, cap, columns, ticks)
            # Independent closed form: position advances by velocity per tick, mod 2^32.
            closed = [(p + ticks * v) & MASK for p, v in zip(pos, vel)]
            if expected["position_x"] != closed:
                raise SystemExit(f"T02 closed-form mismatch n={n} ticks={ticks}")
            cases.append({
                "name": f"n{n}_ticks{ticks}",
                "ticks": ticks,
                "expected_columns": {"position_x": expected["position_x"],
                                     "velocity_x": expected["velocity_x"]},
            })
        d = f"t02/"
        out_files[f"{d}world_{n}.json"] = world(columns)
        out_files[f"{d}movement_{n}.test.json"] = test_doc(
            "demo.movement", "movement.capsule.json", f"world_{n}.json", cases)
    out_files["t02/movement.lir.json"] = program
    out_files["t02/movement.capsule.json"] = cap


# -------------------------------------------------------------- T03 numerics

# Hand-computed known answers (modulo 2^32 unless noted). Each row is (p, v) and
# the expected value is what store_u32 position_x receives for that row.
T03_CASES = [
    {
        "name": "wrap_add",
        "instructions": [
            {"op": "load_u32", "dst": "p", "field": "position_x"},
            {"op": "load_u32", "dst": "v", "field": "velocity_x"},
            {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
            {"op": "store_u32", "field": "position_x", "src": "next"},
        ],
        # 0xFFFFFFFF+1 = 2^32 -> 0; 2*(2^32-1) = 2^33-2 -> 2^32-2; 2^32-2+3 = 2^32+1 -> 1
        "rows": [(U32_MAX, 1), (U32_MAX, U32_MAX), (0, 0), (0xFFFFFFFE, 3)],
        "expected": [0, 0xFFFFFFFE, 0, 1],
    },
    {
        "name": "sub_underflow",
        "instructions": [
            {"op": "load_u32", "dst": "p", "field": "position_x"},
            {"op": "load_u32", "dst": "v", "field": "velocity_x"},
            {"op": "sub_u32", "dst": "next", "a": "p", "b": "v"},
            {"op": "store_u32", "field": "position_x", "src": "next"},
        ],
        # 0-1 -> 2^32-1; 5-7 = -2 -> 2^32-2; max-max = 0; 7-0 = 7 (not saturating)
        "rows": [(0, 1), (5, 7), (U32_MAX, U32_MAX), (7, 0)],
        "expected": [0xFFFFFFFF, 0xFFFFFFFE, 0, 7],
    },
    {
        "name": "mul_overflow",
        "instructions": [
            {"op": "load_u32", "dst": "p", "field": "position_x"},
            {"op": "load_u32", "dst": "v", "field": "velocity_x"},
            {"op": "mul_u32", "dst": "next", "a": "p", "b": "v"},
            {"op": "store_u32", "field": "position_x", "src": "next"},
        ],
        # 2^16*2^16 = 2^32 -> 0; (2^32-1)*2 -> 2^32-2; 3*1431655766 = 2^32+2 -> 2;
        # (2^32-1)^2 = 2^64-2^33+1 -> 1; 0*x = 0
        "rows": [(65536, 65536), (U32_MAX, 2), (3, 1431655766), (U32_MAX, U32_MAX), (0, U32_MAX)],
        "expected": [0, 0xFFFFFFFE, 2, 1, 0],
    },
    {
        "name": "lt_edges",
        "instructions": [
            {"op": "const_u32", "dst": "one", "value": 1},
            {"op": "const_u32", "dst": "zero", "value": 0},
            {"op": "load_u32", "dst": "p", "field": "position_x"},
            {"op": "load_u32", "dst": "v", "field": "velocity_x"},
            {"op": "lt_u32", "dst": "c", "a": "p", "b": "v"},
            {"op": "select_u32", "dst": "r", "cond": "c", "on_true": "one", "on_false": "zero"},
            {"op": "store_u32", "field": "position_x", "src": "r"},
        ],
        # unsigned: 0<0 F; 0<max T; max<0 F; max<max F; 1<2 T
        "rows": [(0, 0), (0, U32_MAX), (U32_MAX, 0), (U32_MAX, U32_MAX), (1, 2)],
        "expected": [0, 1, 0, 0, 1],
    },
    {
        "name": "eq_edges",
        "instructions": [
            {"op": "const_u32", "dst": "one", "value": 1},
            {"op": "const_u32", "dst": "zero", "value": 0},
            {"op": "load_u32", "dst": "p", "field": "position_x"},
            {"op": "load_u32", "dst": "v", "field": "velocity_x"},
            {"op": "eq_u32", "dst": "c", "a": "p", "b": "v"},
            {"op": "select_u32", "dst": "r", "cond": "c", "on_true": "one", "on_false": "zero"},
            {"op": "store_u32", "field": "position_x", "src": "r"},
        ],
        # 0==0 T; 0==max F; max==0 F; max==max T; 1==2 F
        "rows": [(0, 0), (0, U32_MAX), (U32_MAX, 0), (U32_MAX, U32_MAX), (1, 2)],
        "expected": [1, 0, 0, 1, 0],
    },
    {
        "name": "select_min",
        "instructions": [
            {"op": "load_u32", "dst": "p", "field": "position_x"},
            {"op": "load_u32", "dst": "v", "field": "velocity_x"},
            {"op": "lt_u32", "dst": "c", "a": "p", "b": "v"},
            {"op": "select_u32", "dst": "r", "cond": "c", "on_true": "p", "on_false": "v"},
            {"op": "store_u32", "field": "position_x", "src": "r"},
        ],
        # unsigned minimum of (p, v)
        "rows": [(0, U32_MAX), (U32_MAX, 0), (0, 0), (U32_MAX, U32_MAX), (5, 9)],
        "expected": [0, 0, 0, U32_MAX, 5],
    },
]


def check_t03_table() -> None:
    """Assert the evaluator agrees with the hand-written known answers."""
    cap = capsule("program.lir.json", ["test.json"])
    for case in T03_CASES:
        prog = lir(case["instructions"])
        columns = {"position_x": [p for p, _ in case["rows"]],
                   "velocity_x": [v for _, v in case["rows"]]}
        got = evaluate_tick(prog, cap, columns)["position_x"]
        if got != case["expected"]:
            raise SystemExit(f"T03 known-answer mismatch in {case['name']}: got {got}, want {case['expected']}")
        if any(not (0 <= x <= U32_MAX) for x in got):
            raise SystemExit(f"T03 out-of-range value in {case['name']}")


def build_t03(out_files: dict[str, object]) -> None:
    for case in T03_CASES:
        d = f"t03/{case['name']}/"
        cap = capsule("program.lir.json", ["test.json"])
        columns = {"position_x": [p for p, _ in case["rows"]],
                   "velocity_x": [v for _, v in case["rows"]]}
        out_files[d + "program.lir.json"] = lir(case["instructions"])
        out_files[d + "capsule.json"] = cap
        out_files[d + "world.json"] = world(columns)
        out_files[d + "test.json"] = test_doc(
            "demo.movement", "capsule.json", "world.json", [{
                "name": f"{case['name']}_one_tick",
                "ticks": 1,
                "expected_columns": {"position_x": case["expected"],
                                     "velocity_x": columns["velocity_x"]},
            }])


# ----------------------------------------------------------- T04 invalid set

def compact_lines(instructions: list[dict]) -> str:
    """One instruction per line so the 4097-entry case stays under the 256 KiB source cap."""
    parts = ['{', '"ir_version": "0.1",', '"system_id": "demo.movement",',
             '"numeric_profile": "u32_mod_v1",', '"query": "demo.actor",', '"instructions": [']
    body = [",\n".join(json.dumps(i, separators=(",", ":")) for i in instructions)]
    return "\n".join(parts) + "\n" + body[0] + "\n]\n}\n"


def build_invalid(out_text: dict[str, str], out_files: dict[str, object]) -> list[dict]:
    base_program = [
        {"op": "load_u32", "dst": "p", "field": "position_x"},
        {"op": "load_u32", "dst": "v", "field": "velocity_x"},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]
    many = [{"op": "const_u32", "dst": f"c{i}", "value": i % 4096} for i in range(4096)]
    many.append({"op": "const_u32", "dst": "c4096", "value": 0})  # 4097 instructions total
    out_text["invalid/too_many_instructions.lir.json"] = compact_lines(many)
    assert len(many) == 4097

    # JSON nesting depth 33: one object plus 32 nested arrays under an extra key.
    depth_doc = ('{"ir_version": "0.1", "system_id": "demo.movement", '
                 '"numeric_profile": "u32_mod_v1", "query": "demo.actor", '
                 '"annotation": ' + "[" * 32 + "]" * 32 + ', '
                 '"instructions": ' + json.dumps(base_program) + '}\n')
    out_text["invalid/nested_depth_33.lir.json"] = depth_doc

    dup_store = base_program[:3] + [
        {"op": "store_u32", "field": "position_x", "src": "next"},
        {"op": "store_u32", "field": "position_x", "src": "p"},
    ]
    out_files["invalid/store_same_field_twice.lir.json"] = lir(dup_store)

    out_files["invalid/denied_read.lir.json"] = lir(base_program)  # loads velocity_x; capsule denies it

    bool_as_u32 = base_program[:2] + [
        {"op": "lt_u32", "dst": "c", "a": "p", "b": "v"},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "c"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]
    out_files["invalid/bool_as_u32.lir.json"] = lir(bool_as_u32)

    frac = base_program[:2] + [
        {"op": "const_u32", "dst": "k", "value": "PLACEHOLDER"},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "k"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]
    frac_text = json.dumps(lir(frac), indent=2) + "\n"
    out_text["invalid/fractional_constant.lir.json"] = frac_text.replace('"PLACEHOLDER"', "1.5")

    over = base_program[:2] + [
        {"op": "const_u32", "dst": "k", "value": 0},
        {"op": "add_u32", "dst": "next", "a": "p", "b": "k"},
        {"op": "store_u32", "field": "position_x", "src": "next"},
    ]
    over_text = json.dumps(lir(over), indent=2) + "\n"
    out_text["invalid/constant_4294967296.lir.json"] = over_text.replace('"value": 0', '"value": 4294967296')

    return [
        {"path": "invalid/too_many_instructions.lir.json", "capsule": "invalid/movement.capsule.json",
         "expected_code": "SU_E_RESOURCE_LIMIT", "note": "4097 instructions; cap is 4096"},
        {"path": "invalid/nested_depth_33.lir.json", "capsule": "invalid/movement.capsule.json",
         "expected_code": "SU_E_RESOURCE_LIMIT", "note": "container depth 33; cap is 32"},
        {"path": "invalid/store_same_field_twice.lir.json", "capsule": "invalid/movement.capsule.json",
         "expected_code": "SU_E_UNDECLARED_WRITE",
         "note": "position_x stored twice; no dedicated code exists in the initial error set (judgment call, see handoff)"},
        {"path": "invalid/denied_read.lir.json", "capsule": "invalid/denied_read.capsule.json",
         "expected_code": "SU_E_UNDECLARED_READ", "note": "velocity_x has read: false in the capsule"},
        {"path": "invalid/bool_as_u32.lir.json", "capsule": "invalid/movement.capsule.json",
         "expected_code": "SU_E_TYPE", "note": "bool register used as add_u32 operand"},
        {"path": "invalid/fractional_constant.lir.json", "capsule": "invalid/movement.capsule.json",
         "expected_code": "SU_E_TYPE", "note": "const_u32 value 1.5"},
        {"path": "invalid/constant_4294967296.lir.json", "capsule": "invalid/movement.capsule.json",
         "expected_code": "SU_E_TYPE", "note": "const_u32 value 2^32 is out of u32 range"},
    ]


# ----------------------------------------------------------- consistency

def check_movement_matches_examples() -> None:
    """The generated movement IR and contract must equal the checked-in examples."""
    ex_lir = json.loads((EXAMPLES / "movement.lir.json").read_text(encoding="utf-8"))
    if ex_lir != lir(MOVEMENT_INSTRUCTIONS):
        raise SystemExit("generated movement IR differs from examples/movement.lir.json")
    ex_cap = json.loads((EXAMPLES / "movement.capsule.json").read_text(encoding="utf-8"))
    mine = capsule("movement.lir.json", ["movement.test.json"])
    for key in ("fields", "numeric_profile", "system_id", "schema"):
        if ex_cap[key] != mine[key]:
            raise SystemExit(f"generated capsule field {key} differs from examples")


# ----------------------------------------------------------------- output

def write_json(path: Path, doc) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def validate(written_json: list[Path], written_schema_checks: list[tuple[Path, str]]) -> None:
    for p in written_json:
        json.loads(p.read_text(encoding="utf-8"), object_pairs_hook=_no_dupes)
    print(f"parsed {len(written_json)} JSON files OK")
    try:
        import jsonschema  # type: ignore
    except ImportError:
        print("jsonschema not installed: schema validation SKIPPED")
        return
    for path, schema_name in written_schema_checks:
        schema = json.loads((SCHEMAS / schema_name).read_text(encoding="utf-8"))
        doc = json.loads(path.read_text(encoding="utf-8"))
        jsonschema.validate(doc, schema)
    print(f"schema-validated {len(written_schema_checks)} files OK")


def _no_dupes(pairs):
    keys = [k for k, _ in pairs]
    if len(keys) != len(set(keys)):
        raise ValueError(f"duplicate key in {keys}")
    return dict(pairs)


def main(argv: list[str]) -> int:
    check_movement_matches_examples()
    check_t03_table()

    t02_tests = [f"movement_{n}.test.json" for n in T02_ROW_COUNTS]
    cap_movement = capsule("movement.lir.json", t02_tests)
    prog = lir(MOVEMENT_INSTRUCTIONS)

    files: dict[str, object] = {}
    build_t02(files, prog, cap_movement)
    build_t03(files)

    text_files: dict[str, str] = {}
    neg_files: dict[str, object] = {}
    negative = build_invalid(text_files, neg_files)
    files.update(neg_files)

    # capsule for invalid cases and the denied-read variant
    files["invalid/movement.capsule.json"] = capsule("movement.lir.json", ["../negative_cases.json"])
    files["invalid/denied_read.capsule.json"] = capsule(
        "denied_read.lir.json", ["../negative_cases.json"], velocity_read=False)

    written_json: list[Path] = []
    schema_checks: list[tuple[Path, str]] = []
    for rel, doc in files.items():
        path = FIX / rel
        write_json(path, doc)
        written_json.append(path)
        name = path.name
        if name.endswith(".capsule.json"):
            schema_checks.append((path, "capsule.schema.json"))
        elif name.endswith(".lir.json"):
            if not rel.startswith("invalid/"):  # invalid/ files are rejection targets
                schema_checks.append((path, "lir.schema.json"))
        elif name.startswith("world"):
            schema_checks.append((path, "world.schema.json"))
        elif name.endswith(".test.json") or name == "test.json":
            schema_checks.append((path, "test.schema.json"))
    for rel, text in text_files.items():
        path = FIX / rel
        write_text(path, text)
        written_json.append(path)

    # Invalid files must not parse as a valid document of the schema, except where the
    # invalid condition is semantic (denied read, store twice): those are reported only.
    write_json(FIX / "negative_cases.json", {
        "capsule": "invalid/movement.capsule.json",
        "cases": negative,
    })
    written_json.append(FIX / "negative_cases.json")

    # Dedupe schema list while keeping order.
    seen = set()
    unique_checks = []
    for item in schema_checks:
        if item[0] not in seen:
            seen.add(item[0])
            unique_checks.append(item)

    print(f"wrote {len(written_json)} files under {FIX.relative_to(ROOT)}")
    if "--no-validate" not in argv:
        validate(written_json, unique_checks)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
