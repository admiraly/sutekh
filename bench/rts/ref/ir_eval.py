# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""IR 0.1 / 0.2 evaluator for the RTS reference (contract M3 section 8 part 2), standard library only.

Written from LIVE_IR_SPEC.md's instruction table and tests/harness/reference_eval.py's semantics
(u32 wrap for add/sub/mul, unsigned lt, exact eq, select on a bool, bool never coerces to u32, loads
see tick-start values, stores committed after the row). It does not import engine code. The
engine's VM is not used.

check_program() performs the static checks the contract asks for (N2 bool/u32 typing, declared
read and write sets, store exactly once, despawn count against max_struct_per_row). run_row()
interprets one row. ir_systems() adapts the programs to the row-function protocol of model.py.
"""
from __future__ import annotations

from model import DESPAWN_KEY, MASK

OPS_01 = {"const_u32", "load_u32", "add_u32", "sub_u32", "mul_u32", "eq_u32", "lt_u32",
          "select_u32", "store_u32"}
OPS_02 = OPS_01 | {"despawn_self"}
BINARY = {"add_u32", "sub_u32", "mul_u32", "eq_u32", "lt_u32"}


class IRError(Exception):
    """A refusal with the 10.4 error name in code."""

    def __init__(self, code: str, detail: str = ""):
        super().__init__(f"{code} {detail}".strip())
        self.code = code
        self.detail = detail


def _is_u32(v) -> bool:
    return type(v) is int and 0 <= v <= MASK


def check_program(prog: dict) -> dict:
    """Static checks. Returns {'stores': [...], 'despawn_count': n} or raises IRError."""
    version = prog.get("ir_version")
    if version not in ("0.1", "0.2"):
        raise IRError("SU_E_SCHEMA_MISMATCH", f"ir_version {version!r}")
    allowed = OPS_01 if version == "0.1" else OPS_02
    fields = prog["fields"]
    kinds = {}  # register -> "u32" | "bool"
    stored = {}
    despawns = 0
    for ins in prog["instructions"]:
        op = ins["op"]
        if op not in allowed:
            code = "SU_E_UNKNOWN_OPCODE"
            raise IRError(code, f"{op} in ir_version {version}")
        if op == "despawn_self":
            despawns += 1
            _need_kind(kinds, ins["cond"], "bool", op)
            continue
        if op == "store_u32":
            meta = fields.get(ins["field"])
            if meta is None or not meta["write"]:
                raise IRError("SU_E_UNDECLARED_WRITE", ins["field"])
            _need_kind(kinds, ins["src"], "u32", op)
            if ins["field"] in stored:
                raise IRError("SU_E_MISSING_STORE", f"{ins['field']} stored twice")
            stored[ins["field"]] = True
            continue
        dst = ins["dst"]
        if dst in kinds:
            raise IRError("SU_E_DUPLICATE_REGISTER", dst)
        if op == "const_u32":
            if not _is_u32(ins["value"]):
                raise IRError("SU_E_TYPE", "const_u32 value")
            kinds[dst] = "u32"
        elif op == "load_u32":
            meta = fields.get(ins["field"])
            if meta is None or not meta["read"]:
                raise IRError("SU_E_UNDECLARED_READ", ins["field"])
            kinds[dst] = "u32"
        elif op in BINARY:
            _need_kind(kinds, ins["a"], "u32", op)
            _need_kind(kinds, ins["b"], "u32", op)
            kinds[dst] = "bool" if op in ("eq_u32", "lt_u32") else "u32"
        elif op == "select_u32":
            _need_kind(kinds, ins["cond"], "bool", op)
            _need_kind(kinds, ins["on_true"], "u32", op)
            _need_kind(kinds, ins["on_false"], "u32", op)
            kinds[dst] = "u32"
    for name, meta in fields.items():
        if meta["write"] and name not in stored:
            raise IRError("SU_E_MISSING_STORE", name)
    limit = prog.get("limits", {}).get("max_struct_per_row", 0)
    if despawns > limit:
        raise IRError("SU_E_RESOURCE_LIMIT", f"despawn_self count {despawns} above {limit}")
    if version == "0.1" and despawns:
        raise IRError("SU_E_UNKNOWN_OPCODE", "despawn_self in 0.1")
    return {"stores": sorted(stored), "despawn_count": despawns}


def _need_kind(kinds: dict, reg: str, want: str, op: str) -> None:
    if reg not in kinds:
        raise IRError("SU_E_UNDEFINED_REGISTER", f"{reg} in {op}")
    if kinds[reg] != want:
        raise IRError("SU_E_TYPE", f"{op} needs {want} operand, {reg} is {kinds[reg]}")


def run_row(prog: dict, row: dict):
    """Interpret one row. row maps field name to u32 value (loads read only these). Returns
    (writes, despawn) where writes maps each stored field to its value and despawn is a bool."""
    regs = {}
    writes = {}
    despawn = False
    for ins in prog["instructions"]:
        op = ins["op"]
        if op == "const_u32":
            regs[ins["dst"]] = ins["value"]
        elif op == "load_u32":
            regs[ins["dst"]] = row[ins["field"]]
        elif op == "add_u32":
            regs[ins["dst"]] = (regs[ins["a"]] + regs[ins["b"]]) & MASK
        elif op == "sub_u32":
            regs[ins["dst"]] = (regs[ins["a"]] - regs[ins["b"]]) & MASK
        elif op == "mul_u32":
            regs[ins["dst"]] = (regs[ins["a"]] * regs[ins["b"]]) & MASK
        elif op == "eq_u32":
            regs[ins["dst"]] = regs[ins["a"]] == regs[ins["b"]]
        elif op == "lt_u32":
            regs[ins["dst"]] = regs[ins["a"]] < regs[ins["b"]]
        elif op == "select_u32":
            regs[ins["dst"]] = regs[ins["on_true"]] if regs[ins["cond"]] else regs[ins["on_false"]]
        elif op == "store_u32":
            writes[ins["field"]] = regs[ins["src"]]
        elif op == "despawn_self":
            if regs[ins["cond"]]:
                despawn = True
        else:
            raise IRError("SU_E_UNKNOWN_OPCODE", op)
    return writes, despawn


def ir_systems(programs: dict = None) -> dict:
    """Row-function adapters with the model's protocol: fn(row39) -> dict of writes, plus
    DESPAWN_KEY for rts.damage. The program is statically checked once, here."""
    if programs is None:
        from ir_programs import all_programs
        programs = all_programs()
    checked = {}
    for name, prog in programs.items():
        check_program(prog)
        checked[name] = prog

    def make(prog):
        reads = [f for f, meta in prog["fields"].items() if meta["read"]]
        has_despawn = any(ins["op"] == "despawn_self" for ins in prog["instructions"])

        def fn(row: dict) -> dict:
            sub = {f: row[f] for f in reads}
            writes, despawn = run_row(prog, sub)
            if has_despawn:
                writes = dict(writes)
                writes[DESPAWN_KEY] = 1 if despawn else 0
            return writes

        return fn

    return {name: make(prog) for name, prog in checked.items()}
