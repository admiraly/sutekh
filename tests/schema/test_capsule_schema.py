#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Capsule schema checks for the M3-03a edit (OD-16, planning/contracts/M3_RTS_CONTRACT.md 5.5).

Requires the optional jsonschema package (Draft 2020-12). Without it the script fails;
it never reports a pass it did not check.

Checks:
  1. The schema itself is a valid Draft 2020-12 schema.
  2. Verdict stability: for every capsule-like JSON file in the repository (valid, invalid
     and negative engine fixtures alike), the verdict of the new schema equals the verdict of
     the baseline schema given with --baseline-schema (the schema before M3-03a). Files that
     are not strict JSON (duplicate keys, truncated text) are excluded and counted.
  3. Positive instances validate: the IR examples, the new fixtures, and every
     capsules/rts/host/*.capsule.json record when that directory exists.
  4. Negative instances built in memory from a valid IR and a valid host base are refused,
     including unknown top-level keys, a host variant carrying IR keys, and a host variant
     with an IR-only numeric profile.

Usage: python3 -I tests/schema/test_capsule_schema.py [--baseline-schema PATH]
Exit status 0 only when every check ran and passed.
"""
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
EXCLUDED_PARTS = {".git", ".claude", "third_party", "build"}


def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate key {key}")
        out[key] = value
    return out


def load_strict(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_no_duplicates)


def make_validator(schema_path: Path):
    import jsonschema  # optional dependency; ImportError fails the run

    schema = load_strict(schema_path)
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(schema)


def verdict(validator, instance: Any) -> bool:
    return not any(True for _ in validator.iter_errors(instance))


NEW_VARIANT_DIRS = ("tests/schema", "capsules/rts/host")


def capsule_files() -> list[Path]:
    """Every capsule-like JSON file that existed before M3-03a (the host variant is new and is
    checked by positives() and the negatives, not by verdict stability)."""
    found = []
    for path in ROOT.rglob("*capsule*.json"):
        parts = path.relative_to(ROOT).parts
        if set(parts) & EXCLUDED_PARTS or any(p.startswith("build-") for p in parts):
            continue
        rel = "/".join(parts)
        if any(rel.startswith(d + "/") for d in NEW_VARIANT_DIRS):
            continue
        found.append(path)
    return sorted(found)


def check_stability(new_v, base_v) -> tuple[int, int, list[str]]:
    compared = 0
    excluded = 0
    mismatches: list[str] = []
    for path in capsule_files():
        try:
            instance = load_strict(path)
        except (ValueError, UnicodeDecodeError):
            excluded += 1
            continue
        compared += 1
        if verdict(new_v, instance) != verdict(base_v, instance):
            mismatches.append(str(path.relative_to(ROOT)))
    return compared, excluded, mismatches


def positives(v) -> list[str]:
    failures = []
    names = [
        "examples/movement.capsule.json",
        "tests/fixtures/edit/movement.capsule.json",
        "tests/fixtures/ir02/ok_valid.capsule.json",
        "tests/scheduler/fixtures/pipe/advance.capsule.json",
    ]
    names += sorted(str(p.relative_to(ROOT)) for p in (ROOT / "tests/schema/fixtures").glob("*.capsule.json"))
    names += sorted(str(p.relative_to(ROOT)) for p in (ROOT / "capsules/rts/host").glob("*.capsule.json"))
    for name in names:
        path = ROOT / name
        if not path.exists():
            failures.append(f"positive instance missing: {name}")
            continue
        if not verdict(v, load_strict(path)):
            failures.append(f"positive instance refused: {name}")
    return failures


def ir_base() -> dict[str, Any]:
    return load_strict(ROOT / "examples/movement.capsule.json")


def host_base() -> dict[str, Any]:
    return load_strict(ROOT / "tests/schema/fixtures/host_ok.capsule.json")


def negatives() -> list[tuple[str, Any, bool]]:
    """(name, instance, expected_valid). Mutations of the valid bases."""
    cases: list[tuple[str, Any, bool]] = []

    ir_ok = ir_base()
    ir_ok["system_rev"] = 2
    ir_ok["capabilities"] = ["despawn_self"]
    cases.append(("ir_with_system_rev_and_capabilities", ir_ok, True))
    cases.append(("ir_without_system_rev_or_capabilities", ir_base(), True))

    host_ok = host_base()
    cases.append(("host_base", copy.deepcopy(host_ok), True))
    host_no_optional = copy.deepcopy(host_ok)
    del host_no_optional["system_rev"]
    del host_no_optional["capabilities"]
    cases.append(("host_without_system_rev_or_capabilities", host_no_optional, True))
    host_caps = copy.deepcopy(host_ok)
    host_caps["capabilities"] = ["despawn_self"]
    cases.append(("host_with_capabilities", host_caps, True))

    def ir(name, mutate):
        case = ir_base()
        mutate(case)
        cases.append((name, case, False))

    def host(name, mutate):
        case = copy.deepcopy(host_ok)
        mutate(case)
        cases.append((name, case, False))

    ir("ir_unknown_top_level_key", lambda c: c.__setitem__("owner", "x"))
    ir("ir_missing_source", lambda c: c.pop("source"))
    ir("ir_missing_fields", lambda c: c.pop("fields"))
    ir("ir_with_host_object", lambda c: c.__setitem__("host", copy.deepcopy(host_ok["host"])))
    ir("ir_system_rev_zero", lambda c: c.__setitem__("system_rev", 0))
    ir("ir_capabilities_duplicate", lambda c: c.__setitem__("capabilities", ["despawn_self", "despawn_self"]))
    ir("ir_capabilities_not_array", lambda c: c.__setitem__("capabilities", "despawn_self"))
    ir("ir_capabilities_non_string", lambda c: c.__setitem__("capabilities", [1]))
    ir("ir_system_rev_string", lambda c: c.__setitem__("system_rev", "2"))

    host("host_with_source", lambda c: c.__setitem__("source", "x.lir.json"))
    host("host_with_fields", lambda c: c.__setitem__("fields", ir_base()["fields"]))
    host("host_with_ir_version", lambda c: c.__setitem__("ir_version", "0.1"))
    host("host_missing_host_object", lambda c: c.pop("host"))
    host("host_unknown_top_level_key", lambda c: c.__setitem__("owner", "x"))
    host("host_missing_schedule", lambda c: c.pop("schedule"))
    host("host_missing_limits", lambda c: c.pop("limits"))
    host("host_missing_allocation", lambda c: c["limits"].pop("allocation"))
    host("host_numeric_profile_rts_u32_v1", lambda c: c.__setitem__("numeric_profile", "rts_u32_v1"))
    host("host_unknown_key_in_host", lambda c: c["host"].__setitem__("extra", 1))
    host("host_operator_unknown", lambda c: c["host"].__setitem__("operator", "other_ops"))
    host("host_operator_version_zero", lambda c: c["host"].__setitem__("operator_version", 0))
    host("host_constants_hash_short", lambda c: c["host"].__setitem__("constants_hash", "abc"))
    host("host_constants_hash_uppercase",
         lambda c: c["host"].__setitem__("constants_hash", "A" * 64))
    host("host_fields_empty", lambda c: c["host"].__setitem__("fields", []))
    host("host_field_access_none", lambda c: c["host"]["fields"][0].__setitem__("access", "none"))
    host("host_field_type_u64", lambda c: c["host"]["fields"][0].__setitem__("type", "u64"))
    host("host_field_extra_key", lambda c: c["host"]["fields"][0].__setitem__("write", True))
    host("host_field_id_zero", lambda c: c["host"]["fields"][0].__setitem__("id", 0))
    host("host_capabilities_not_array", lambda c: c.__setitem__("capabilities", "despawn_self"))
    host("host_system_rev_zero", lambda c: c.__setitem__("system_rev", 0))
    return cases


def check_negatives(v) -> tuple[int, list[str]]:
    failures = []
    cases = negatives()
    for name, instance, expected in cases:
        got = verdict(v, instance)
        if got != expected:
            failures.append(f"{name}: expected valid={expected}, got valid={got}")
    return len(cases), failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema", default=str(ROOT / "schemas/capsule.schema.json"))
    parser.add_argument("--baseline-schema", default=None,
                        help="schema before the edit; verdicts of every capsule file must not change")
    args = parser.parse_args()
    try:
        new_v = make_validator(Path(args.schema))
    except ImportError:
        print("FAILED: optional package jsonschema is not installed; nothing was checked", file=sys.stderr)
        return 1
    failures: list[str] = []
    result: dict[str, Any] = {"scope": "capsule_schema_m3_03a", "status": "failed"}

    if args.baseline_schema is not None:
        base_v = make_validator(Path(args.baseline_schema))
        compared, excluded, mismatches = check_stability(new_v, base_v)
        failures += [f"verdict changed for {m}" for m in mismatches]
        result["baseline_verdict_stability"] = {"files_compared": compared, "files_not_strict_json": excluded,
                                                "mismatches": len(mismatches)}
    failures += positives(new_v)
    count, neg_failures = check_negatives(new_v)
    failures += neg_failures
    result["negative_cases"] = count
    result["failures"] = failures
    result["status"] = "passed" if not failures else "failed"
    print(json.dumps(result, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
