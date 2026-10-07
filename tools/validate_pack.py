#!/usr/bin/env python3
"""Validate the specification package and tiny SU-LIR examples, not an engine.

Standard-library checks always run. Draft 2020-12 checks also run when the
optional jsonschema package is installed. No native compiler is invoked.
"""
from __future__ import annotations
import copy
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MASK = (1 << 32) - 1
NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

class SpecError(Exception):
    def __init__(self, code: str, message: str):
        self.code = code
        super().__init__(f"{code}: {message}")

def fail(code: str, message: str) -> None:
    raise SpecError(code, message)

def unique_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            fail("SU_E_DUPLICATE_KEY", key)
        result[key] = value
    return result

def load(path: Path) -> Any:
    def invalid_constant(value: str) -> None:
        fail("SU_E_PARSE", f"non-JSON constant {value}")
    try:
        return json.loads(path.read_text(encoding="utf-8"),
                          object_pairs_hook=unique_pairs,
                          parse_constant=invalid_constant)
    except (json.JSONDecodeError, UnicodeError) as exc:
        fail("SU_E_PARSE", f"{path.name}: {exc}")

def check(condition: bool, message: str) -> None:
    if not condition:
        fail("PACK_CHECK_FAILED", message)

def is_u32(value: Any) -> bool:
    return type(value) is int and 0 <= value <= MASK

def exact_keys(value: Any, expected: set[str], context: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        fail("SU_E_PARSE", f"unexpected or missing keys in {context}")

OPS = {
    "const_u32": {"op", "dst", "value"},
    "load_u32": {"op", "dst", "field"},
    "add_u32": {"op", "dst", "a", "b"},
    "sub_u32": {"op", "dst", "a", "b"},
    "mul_u32": {"op", "dst", "a", "b"},
    "eq_u32": {"op", "dst", "a", "b"},
    "lt_u32": {"op", "dst", "a", "b"},
    "select_u32": {"op", "dst", "cond", "on_true", "on_false"},
    "store_u32": {"op", "field", "src"},
}

def validate_program(program: dict[str, Any], capsule: dict[str, Any]) -> None:
    exact_keys(program, {"ir_version", "system_id", "numeric_profile", "query", "instructions"}, "module")
    if program["ir_version"] != "0.1" or program["numeric_profile"] != "u32_mod_v1":
        fail("SU_E_TYPE", "unsupported IR/numeric profile")
    if (program["system_id"] != capsule["system_id"] or
        program["query"] != capsule["schema"]["id"] or
        program["numeric_profile"] != capsule["numeric_profile"]):
        fail("SU_E_SCHEMA_MISMATCH", "capsule identity does not match")
    fields: dict[str, dict[str, Any]] = {}
    ids: set[int] = set()
    for field in capsule["fields"]:
        if field["name"] in fields or field["id"] in ids:
            fail("SU_E_SCHEMA_MISMATCH", "duplicate field name or ID")
        if field["type"] != "u32":
            fail("SU_E_TYPE", "M0 fields must be u32")
        fields[field["name"]] = field
        ids.add(field["id"])
    instructions = program["instructions"]
    if not isinstance(instructions, list) or not 1 <= len(instructions) <= capsule["limits"]["max_instructions"]:
        fail("SU_E_RESOURCE_LIMIT", "invalid instruction count")
    registers: dict[str, str] = {}
    stored: set[str] = set()

    def require(name: str, typ: str) -> None:
        if name not in registers:
            fail("SU_E_UNDEFINED_REGISTER", name)
        if registers[name] != typ:
            fail("SU_E_TYPE", f"{name}: wanted {typ}, got {registers[name]}")

    for index, ins in enumerate(instructions):
        if not isinstance(ins, dict) or ins.get("op") not in OPS:
            fail("SU_E_UNKNOWN_OPCODE", f"instruction {index}")
        op = ins["op"]
        exact_keys(ins, OPS[op], f"instruction {index}")
        dst = ins.get("dst")
        if dst is not None:
            if not isinstance(dst, str) or not NAME.fullmatch(dst):
                fail("SU_E_TYPE", "bad register name")
            if dst in registers:
                fail("SU_E_DUPLICATE_REGISTER", dst)
        typ = "u32"
        if op == "const_u32":
            if not is_u32(ins["value"]):
                fail("SU_E_TYPE", "constant is not a u32 integer")
        elif op == "load_u32":
            field = fields.get(ins["field"])
            if field is None:
                fail("SU_E_SCHEMA_MISMATCH", ins["field"])
            if not field["read"]:
                fail("SU_E_UNDECLARED_READ", ins["field"])
        elif op in ("add_u32", "sub_u32", "mul_u32", "eq_u32", "lt_u32"):
            require(ins["a"], "u32")
            require(ins["b"], "u32")
            if op in ("eq_u32", "lt_u32"):
                typ = "bool"
        elif op == "select_u32":
            require(ins["cond"], "bool")
            require(ins["on_true"], "u32")
            require(ins["on_false"], "u32")
        elif op == "store_u32":
            field = fields.get(ins["field"])
            if field is None:
                fail("SU_E_SCHEMA_MISMATCH", ins["field"])
            if not field["write"]:
                fail("SU_E_UNDECLARED_WRITE", ins["field"])
            require(ins["src"], "u32")
            if ins["field"] in stored:
                fail("SU_E_TYPE", "more than one store to a field")
            stored.add(ins["field"])
        if dst is not None:
            registers[dst] = typ
    expected_stores = {name for name, f in fields.items() if f["write"]}
    if stored != expected_stores:
        fail("SU_E_TYPE", "each writable field requires exactly one store")

def evaluate(program: dict[str, Any], capsule: dict[str, Any], world: dict[str, Any], ticks: int) -> dict[str, Any]:
    """Small semantic model for pack fixtures, not the production VM."""
    validate_program(program, capsule)
    if type(ticks) is not int or not 0 <= ticks <= 1000:
        fail("SU_E_RESOURCE_LIMIT", "test tick limit")
    if world["schema_id"] != capsule["schema"]["id"] or world["schema_version"] != capsule["schema"]["version"]:
        fail("SU_E_SCHEMA_MISMATCH", "world schema")
    count = len(world["entity_ids"])
    if count > capsule["limits"]["max_rows"]:
        fail("SU_E_RESOURCE_LIMIT", "row limit")
    if len(set(world["entity_ids"])) != count:
        fail("SU_E_SCHEMA_MISMATCH", "duplicate entity identity")
    required = {f["name"] for f in capsule["fields"]}
    if set(world["columns"]) != required:
        fail("SU_E_SCHEMA_MISMATCH", "world fields")
    for values in world["columns"].values():
        if len(values) != count or not all(is_u32(v) for v in values):
            fail("SU_E_TYPE", "invalid column values/count")
    result = copy.deepcopy(world)
    for _ in range(ticks):
        entry = result["columns"]
        staged = copy.deepcopy(entry)
        for row in range(count):
            registers: dict[str, Any] = {}
            for ins in program["instructions"]:
                op = ins["op"]
                if op == "const_u32": value = ins["value"]
                elif op == "load_u32": value = entry[ins["field"]][row]
                elif op == "add_u32": value = (registers[ins["a"]] + registers[ins["b"]]) & MASK
                elif op == "sub_u32": value = (registers[ins["a"]] - registers[ins["b"]]) & MASK
                elif op == "mul_u32": value = (registers[ins["a"]] * registers[ins["b"]]) & MASK
                elif op == "eq_u32": value = registers[ins["a"]] == registers[ins["b"]]
                elif op == "lt_u32": value = registers[ins["a"]] < registers[ins["b"]]
                elif op == "select_u32": value = registers[ins["on_true"]] if registers[ins["cond"]] else registers[ins["on_false"]]
                elif op == "store_u32":
                    staged[ins["field"]][row] = registers[ins["src"]]
                    continue
                else:
                    fail("SU_E_UNKNOWN_OPCODE", op)
                registers[ins["dst"]] = value
        result["columns"] = staged
        result["tick"] += 1
    return result

def check_task_graph() -> int:
    data = load(ROOT / "planning/tasks.json")
    tasks = data["tasks"]
    by_id = {t["id"]: t for t in tasks}
    check(len(by_id) == len(tasks), "duplicate task IDs")
    visiting: set[str] = set()
    visited: set[str] = set()
    def visit(task_id: str) -> None:
        check(task_id in by_id, f"missing task dependency {task_id}")
        check(task_id not in visiting, f"task dependency cycle at {task_id}")
        if task_id in visited: return
        visiting.add(task_id)
        task = by_id[task_id]
        check(task["milestone"] == task_id.split("-")[0], "task milestone mismatch")
        for dep in task["depends_on"]: visit(dep)
        visiting.remove(task_id)
        visited.add(task_id)
    for task_id in by_id: visit(task_id)
    return len(tasks)

def check_links() -> int:
    total = 0
    pattern = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
    for path in ROOT.rglob("*.md"):
        for match in pattern.finditer(path.read_text(encoding="utf-8")):
            target = match.group(1).strip().split("#", 1)[0]
            if not target or "://" in target or target.startswith(("mailto:", "sandbox:")):
                continue
            resolved = (path.parent / target).resolve()
            check(resolved.is_relative_to(ROOT), f"link leaves pack: {path.name} → {target}")
            check(resolved.exists(), f"broken link: {path.relative_to(ROOT)} → {target}")
            total += 1
    return total

def check_examples() -> tuple[int, int, int]:
    cap = load(ROOT / "examples/movement.capsule.json")
    program = load(ROOT / "examples" / cap["source"])
    world = load(ROOT / "examples/world.json")
    suite = load(ROOT / "examples/movement.test.json")
    positives = 0
    for case in suite["cases"]:
        actual = evaluate(program, cap, world, case["ticks"])
        check(actual["columns"] == case["expected_columns"], f"fixture {case['name']}")
        positives += 1
    v2 = load(ROOT / "examples/movement_v2.lir.json")
    check(evaluate(v2, cap, world, 1)["columns"]["position_x"] == [2, 5, 1, 7], "v2 fixture")
    positives += 1
    for count in [0, 1, 7, 8, 9, 1024]:
        test_world = copy.deepcopy(world)
        test_world["entity_ids"] = list(range(1, count + 1))
        test_world["columns"] = {"position_x": [MASK] * count, "velocity_x": [1] * count}
        check(evaluate(program, cap, test_world, 1)["columns"]["position_x"] == [0] * count, f"row count {count}")
        positives += 1
    negative_count = 0
    for case in load(ROOT / "examples/negative_cases.json")["cases"]:
        try:
            invalid = load(ROOT / "examples" / case["path"])
            validate_program(invalid, cap)
        except SpecError as exc:
            check(exc.code == case["expected_code"], f"negative case {case['path']}: got {exc.code}")
        else:
            fail("PACK_CHECK_FAILED", f"invalid program accepted: {case['path']}")
        negative_count += 1
    # Known-answer smoke cases exercise every opcode beyond the movement samples.
    instruction_cases = [
        ([{"op":"const_u32","dst":"a","value":0}, {"op":"const_u32","dst":"b","value":1}, {"op":"sub_u32","dst":"out","a":"a","b":"b"}], MASK),
        ([{"op":"const_u32","dst":"a","value":MASK}, {"op":"const_u32","dst":"b","value":2}, {"op":"mul_u32","dst":"out","a":"a","b":"b"}], MASK - 1),
        ([{"op":"const_u32","dst":"a","value":7}, {"op":"const_u32","dst":"b","value":7}, {"op":"eq_u32","dst":"c","a":"a","b":"b"}, {"op":"const_u32","dst":"other","value":9}, {"op":"select_u32","dst":"out","cond":"c","on_true":"a","on_false":"other"}], 7),
        ([{"op":"const_u32","dst":"a","value":9}, {"op":"const_u32","dst":"b","value":7}, {"op":"lt_u32","dst":"c","a":"a","b":"b"}, {"op":"select_u32","dst":"out","cond":"c","on_true":"a","on_false":"b"}], 7),
    ]
    for instructions, expected in instruction_cases:
        modified = copy.deepcopy(program)
        modified["instructions"] = instructions + [{"op":"store_u32","field":"position_x","src":"out"}]
        check(evaluate(modified, cap, world, 1)["columns"]["position_x"] == [expected] * 4, "opcode known answer")
    return positives, negative_count, len(instruction_cases)

def schema_checks() -> dict[str, Any]:
    try:
        import jsonschema
    except ImportError:
        return {"status":"skipped", "reason":"Optional jsonschema package is not installed; standard-library checks still ran."}
    schemas = {}
    for path in (ROOT / "schemas").glob("*.schema.json"):
        schema = load(path)
        jsonschema.Draft202012Validator.check_schema(schema)
        schemas[path.name] = schema
    mapping = {
        "examples/movement.lir.json": "lir.schema.json",
        "examples/movement_v2.lir.json": "lir.schema.json",
        "examples/movement.capsule.json": "capsule.schema.json",
        "examples/world.json": "world.schema.json",
        "examples/movement.test.json": "test.schema.json",
        "examples/report_unmeasured.json": "evidence.schema.json",
        "planning/tasks.json": "task.schema.json",
    }
    for path, schema_name in mapping.items():
        jsonschema.Draft202012Validator(schemas[schema_name]).validate(load(ROOT / path))
    return {"status":"passed", "schemas_checked":len(schemas), "instances_checked":len(mapping)}

def main() -> int:
    try:
        json_count = 0
        for path in ROOT.rglob("*.json"):
            if "invalid" in path.parts or path.name in ("PACK_VALIDATION.json", "PACK_INDEX.json"):
                continue
            load(path)
            json_count += 1
        tasks = check_task_graph()
        positive, negative, opcode_cases = check_examples()
        schemas = schema_checks()
        links = check_links()
        result = {
            "scope":"specification_pack_and_python_example_model_only",
            "status":"passed",
            "strict_json_files_checked":json_count,
            "task_dag_nodes_checked":tasks,
            "positive_fixture_checks":positive,
            "negative_ir_checks":negative,
            "additional_opcode_known_answer_checks":opcode_cases,
            "internal_markdown_links_checked":links,
            "json_schema":schemas,
            "native_engine_compilation":"not_run",
            "native_runtime_tests":"not_run",
            "engine_performance":"not_run",
            "original_bfme_fidelity":"not_run",
        }
        (ROOT / "PACK_VALIDATION.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
        print(json.dumps(result, indent=2))
        return 0
    except (SpecError, OSError, KeyError, TypeError, ValueError) as exc:
        print(json.dumps({"scope":"specification_pack_only","status":"failed","error":str(exc)}, indent=2), file=sys.stderr)
        return 1
    except Exception as exc:
        # Includes optional jsonschema validation errors: never convert to PASS.
        print(json.dumps({"scope":"specification_pack_only","status":"failed","error":f"{type(exc).__name__}: {exc}"}, indent=2), file=sys.stderr)
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
