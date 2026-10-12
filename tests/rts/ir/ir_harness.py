# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Shared helpers for the M3-03 IR tests (standard library only; run with python3 -I).

Engine side: tests/rts/ir/driver/rts_ir_driver.c is compiled against the release static libraries
of a build directory given by SU_BUILD_DIR (or --build). It calls the engine's su_lir_verify and
su_vm_invoke_v1. Reference side: bench/rts/ref (IR evaluator and model) and bench/rts/gen (scenes).

Engine projection: the engine verifier (src/ir/su_lir.c) refuses the top-level keys system_rev and
capabilities that contract 5.5 requires. The records keep them; the engine is given a copy without
them (projection) and the tests say so. Nothing else is removed or changed.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
IR_DIR = os.path.join(REPO, "capsules", "rts")
REF_DIR = os.path.join(REPO, "bench", "rts", "ref")
GEN_DIR = os.path.join(REPO, "bench", "rts", "gen")
BENCH_RTS_DIR = os.path.join(REPO, "bench", "rts")
DRIVER_C = os.path.join(HERE, "driver", "rts_ir_driver.c")
SYSTEMS = ("goal", "combat", "move", "damage")
REMOVED_BY_PROJECTION = ("system_rev", "capabilities")

for _p in (REF_DIR, GEN_DIR, BENCH_RTS_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gen_ir  # noqa: E402  (bench/rts/gen_ir.py, the authoring tool under test)
import ir_eval  # noqa: E402  (bench/rts/ref/ir_eval.py)
import ir_programs  # noqa: E402  (bench/rts/ref/ir_programs.py)
import model  # noqa: E402  (bench/rts/ref/model.py)
import rtsgen  # noqa: E402  (bench/rts/gen/rtsgen.py)

FIELD_IDS = gen_ir.load_profile_ids()          # name -> id (profile rts_u32_v1)
FIELD_NAMES_BY_ID = {v: k for k, v in FIELD_IDS.items()}


def load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


# Contract transcription (planning/contracts/M3_RTS_CONTRACT.md r4), kept here as a separate copy so
# the authoring tool cannot silently change what the tests expect.
CONTRACT_READS = {
    "goal": ["order_mode", "order_x", "order_y", "anchor_valid", "anchor_x", "anchor_y",
             "formation_ox", "formation_oy", "pos_x", "pos_y"],                              # 5.1
    "combat": ["target_valid", "target_slot", "target_gen", "cooldown", "reload", "damage", "flight",
               "proj_active", "proj_left", "proj_target", "proj_target_gen", "proj_damage"],  # 5.2
    "move": ["goal_x", "goal_y", "pos_x", "pos_y", "speed", "order_mode", "target_valid"],    # 5.3
    "damage": ["hp", "incoming"],                                                              # 5.4
}
CONTRACT_WRITES = {
    "goal": ["goal_x", "goal_y"],                                                              # 5.1
    "combat": ["cooldown", "proj_active", "proj_left", "proj_target", "proj_target_gen",
               "proj_damage", "dmg_out", "dmg_target", "dmg_target_gen", "incoming"],          # 5.2
    "move": ["pos_x", "pos_y"],                                                                # 5.3
    "damage": ["hp"],                                                                          # 5.4
}
CONTRACT_AFTER = {                                                                             # 3.2
    "goal": ["rts.spatial"],
    "combat": ["rts.spatial"],
    "move": ["rts.spatial", "rts.goal", "rts.combat"],
    "damage": ["rts.spatial", "rts.scatter"],
}
CONTRACT_IR_VERSION = {"goal": "0.1", "combat": "0.1", "move": "0.1", "damage": "0.2"}
CONTRACT_CAPABILITIES = {"goal": [], "combat": [], "move": [], "damage": ["despawn_self"]}


def structural_problems(name, capsule, lir):
    """Checks one record pair against contract 3.2, 5.1-5.5. Returns a list of problem strings.
    An empty list means the record matches the contract text; it says nothing about semantics."""
    problems = []
    profile_ids = FIELD_IDS
    sid = f"rts.{name}"
    if capsule.get("system_id") != sid or lir.get("system_id") != sid:
        problems.append("system_id")
    if capsule.get("system_rev") != 1:
        problems.append("system_rev must be 1")
    if sorted(capsule.get("capabilities", [])) != sorted(CONTRACT_CAPABILITIES[name]):
        problems.append("capabilities")
    if capsule.get("ir_version") != CONTRACT_IR_VERSION[name] or lir.get("ir_version") != CONTRACT_IR_VERSION[name]:
        problems.append("ir_version")
    sched = capsule.get("schedule", {})
    if sched.get("phase") != "rts":
        problems.append("schedule.phase")
    if sched.get("after") != CONTRACT_AFTER[name]:
        problems.append(f"schedule.after {sched.get('after')} != {CONTRACT_AFTER[name]}")
    if sched.get("before") != [] or sched.get("parallel") != "serial":
        problems.append("schedule.before/parallel")
    lim = capsule.get("limits", {})
    if lim.get("max_instructions") != 4096 or lim.get("max_rows") != 0 or lim.get("allocation") != "forbidden":
        problems.append("limits (4096, 0, forbidden)")
    if name == "damage":
        if lim.get("max_struct_per_row") != 1 or lim.get("max_events_per_row") != 0:
            problems.append("damage per-row caps")
    elif "max_struct_per_row" in lim or "max_events_per_row" in lim:
        problems.append("0.1 system declares per-row caps")
    if capsule.get("numeric_profile") != "u32_mod_v1" or lir.get("numeric_profile") != "u32_mod_v1":
        problems.append("numeric_profile")
    if capsule.get("schema") != {"id": "rts", "version": 1, "layout_generation": 1}:
        problems.append("schema")
    if not capsule.get("tests"):
        problems.append("tests empty")
    # declared sets against the contract text and against the instructions
    declared_read = sorted(f["name"] for f in capsule["fields"] if f["read"])
    declared_write = sorted(f["name"] for f in capsule["fields"] if f["write"])
    if declared_read != sorted(CONTRACT_READS[name]):
        problems.append(f"declared reads {declared_read} != contract {sorted(CONTRACT_READS[name])}")
    if declared_write != sorted(CONTRACT_WRITES[name]):
        problems.append(f"declared writes {declared_write} != contract {sorted(CONTRACT_WRITES[name])}")
    loads = {i["field"] for i in lir["instructions"] if i["op"] == "load_u32"}
    stores = {i["field"] for i in lir["instructions"] if i["op"] == "store_u32"}
    if sorted(loads) != declared_read:
        problems.append("derived reads differ from declared reads")
    if sorted(stores) != declared_write:
        problems.append("derived writes differ from declared writes")
    for f in capsule["fields"]:
        if profile_ids.get(f["name"]) != f["id"]:
            problems.append(f"field id {f['name']}")
    return problems


def load_record(name):
    """(capsule dict, lir dict) of the committed record in capsules/rts/."""
    cap = load_json(os.path.join(IR_DIR, f"{name}.capsule.json"))
    lir = load_json(os.path.join(IR_DIR, f"{name}.lir.json"))
    return cap, lir


def readable_fields(capsule):
    """Sorted list of (id, name) the capsule declares readable."""
    return sorted((f["id"], f["name"]) for f in capsule["fields"] if f["read"])


def writable_fields(capsule):
    return sorted((f["id"], f["name"]) for f in capsule["fields"] if f["write"])


def projection(capsule):
    """Engine-compatible copy: system_rev and capabilities removed, all else identical."""
    out = {k: v for k, v in capsule.items() if k not in REMOVED_BY_PROJECTION}
    return out


def ir_program_from_record(capsule, lir):
    """ir_eval program dict for a committed record (fields from the capsule, instructions from LIR)."""
    fields = {f["name"]: {"read": f["read"], "write": f["write"]} for f in capsule["fields"]}
    return {
        "ir_version": lir["ir_version"],
        "system_id": lir["system_id"],
        "fields": fields,
        "limits": {
            "max_struct_per_row": capsule["limits"].get("max_struct_per_row", 0),
            "max_events_per_row": capsule["limits"].get("max_events_per_row", 0),
        },
        "instructions": lir["instructions"],
    }


def compile_driver(build_dir, out_dir):
    """Compiles the driver with the release static libraries of build_dir. Returns the executable."""
    exe = os.path.join(out_dir, "rts_ir_driver")
    libs = ["libsu_vm.a", "libsu_world.a", "libsu_ents.a", "libsu_ir.a", "libsu_cmdq.a", "libsu_platform.a"]
    for lib in libs:
        path = os.path.join(build_dir, lib)
        if not os.path.isfile(path):
            raise RuntimeError(f"missing engine library {path}; build the release tree first")
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
           "-I", os.path.join(REPO, "src"), "-I", os.path.join(REPO, "include"), DRIVER_C, "-o", exe,
           "-Wl,--start-group"] + [os.path.join(build_dir, lib) for lib in libs] + ["-Wl,--end-group"]
    subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return exe


class Driver:
    """Engine driver for one work directory. Projected capsule and LIR copies are written there."""

    def __init__(self, build_dir, work_dir):
        self.work = work_dir
        os.makedirs(work_dir, exist_ok=True)
        self.exe = compile_driver(build_dir, work_dir)
        self.build_dir = build_dir

    def write_pair(self, tag, capsule_dict, lir_bytes_or_dict, project=True):
        """Writes <tag>.capsule.json and <tag>.lir.json into the work directory; returns their paths."""
        cap_path = os.path.join(self.work, f"{tag}.capsule.json")
        lir_path = os.path.join(self.work, f"{tag}.lir.json")
        cap = projection(capsule_dict) if project else capsule_dict
        with open(cap_path, "w", encoding="utf-8") as fh:
            json.dump(cap, fh, indent=2)
            fh.write("\n")
        if isinstance(lir_bytes_or_dict, (bytes, bytearray)):
            with open(lir_path, "wb") as fh:
                fh.write(lir_bytes_or_dict)
        elif isinstance(lir_bytes_or_dict, str):
            with open(lir_path, "w", encoding="utf-8") as fh:
                fh.write(lir_bytes_or_dict)
        else:
            with open(lir_path, "w", encoding="utf-8") as fh:
                json.dump(lir_bytes_or_dict, fh, indent=2)
                fh.write("\n")
        return cap_path, lir_path

    def verify(self, cap_path, lir_path):
        """Runs su_lir_verify. Returns {'ok': bool, 'code': int, 'name': str, ...} parsed from the output."""
        proc = subprocess.run([self.exe, "verify", cap_path, lir_path], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, universal_newlines=True)
        line = proc.stdout.strip().splitlines()
        if proc.returncode != 0 or not line:
            raise RuntimeError(f"driver verify failed rc={proc.returncode} stderr={proc.stderr!r}")
        text = line[-1]
        if text.startswith("VERIFY OK "):
            info = {"ok": True, "code": 0, "name": "SU_OK"}
            for tok in text[len("VERIFY OK "):].split():
                k, _, v = tok.partition("=")
                info[k] = int(v) if v.isdigit() else v
            return info
        if text.startswith("VERIFY ERR "):
            rest = text[len("VERIFY ERR "):]
            code_s, name, tail = rest.split(" ", 2)
            info = {"ok": False, "code": int(code_s), "name": name, "detail": tail}
            return info
        raise RuntimeError(f"unexpected driver output {text!r}")

    def run(self, cap_path, lir_path, calls):
        """calls: list of (rows, {field_id: [values]}). Returns (verify_info, [result dicts])."""
        lines = []
        for rows, inputs in calls:
            lines.append(f"CALL {rows}")
            for fid in sorted(inputs):
                vals = inputs[fid]
                lines.append("IN " + " ".join([str(fid)] + [str(int(v)) for v in vals]))
            lines.append("END")
        text = "\n".join(lines) + ("\n" if lines else "")
        proc = subprocess.run([self.exe, "run", cap_path, lir_path], input=text, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, universal_newlines=True)
        out = proc.stdout.splitlines()
        if proc.returncode != 0:
            raise RuntimeError(f"driver run rc={proc.returncode} stderr={proc.stderr!r} out={out[:3]!r}")
        if out and out[0].startswith("RUN ERR"):
            return {"ok": False, "detail": out[0]}, []
        if not out or not out[0].startswith("RUN READY"):
            raise RuntimeError(f"driver did not start: {out[:2]!r}")
        verify_info = {"ok": True, "insn": int(out[0].split("insn=")[1])}
        results = []
        cur = None
        for line in out[1:]:
            if line.startswith("RES "):
                _, st, failed = line.split()
                cur = {"status": int(st), "failed_insn": int(failed), "out": {}, "despawn": []}
            elif line.startswith("OUT "):
                toks = line.split()
                cur["out"][int(toks[1])] = [int(v) for v in toks[2:]]
            elif line.startswith("DESPAWN"):
                cur["despawn"] = [int(v) for v in line.split()[1:]]
            elif line == "END":
                results.append(cur)
                cur = None
            else:
                raise RuntimeError(f"unexpected driver line {line!r}")
        return verify_info, results


# ---- random and scene row sources -------------------------------------------------------------

class Rng:
    """splitmix64 (7.1), local to the test so the sets are reproducible without the model's PRNG."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFFFFFFFFFF

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        return z ^ (z >> 31)

    def below(self, n):
        return self.next() % n

    def pick(self, seq):
        return seq[self.below(len(seq))]


U32_EDGES = (0, 1, 2, 3, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF)


def any_u32(rng):
    """Arbitrary u32 (IR semantics must agree with the evaluator for every u32). Half the draws are
    small values and edges, so the flag and zero branches are reached (uniform u32 almost never is)."""
    k = rng.below(4)
    if k == 0:
        return rng.pick((0, 1, 2, 3))
    if k == 1:
        return rng.pick(U32_EDGES)
    return rng.next() & 0xFFFFFFFF


# Domain-valid value sources per system field (2.2 domains; the model is defined on these rows only).
# Small values, zero and domain edges are drawn often: the fire, arrival, clamp and kill branches are
# rare under uniform draws (cooldown 0 with a valid target and no projectile is about 1 row in 3000).
def _pos(rng):
    return rng.pick((0, 1, 16384, 32766, 32767)) if rng.below(2) else rng.below(32768)


def _small(rng, hi):
    """0, 1, a few small values, or uniform on 0..hi."""
    k = rng.below(4)
    if k == 0:
        return rng.pick((0, 1))
    if k == 1:
        return rng.below(min(hi, 4) + 1)
    return rng.below(hi + 1)


DOMAIN_VALID = {
    "goal": {
        "order_mode": lambda r: r.below(3),
        "order_x": _pos, "order_y": _pos,
        "anchor_valid": lambda r: r.below(2),
        "anchor_x": _pos, "anchor_y": _pos,
        "formation_ox": _pos, "formation_oy": _pos,  # stored offset + BIAS, 0..32767
        "pos_x": _pos, "pos_y": _pos,
    },
    "combat": {
        "target_valid": lambda r: r.below(2),
        "target_slot": any_u32, "target_gen": any_u32,
        "cooldown": lambda r: _small(r, 1024),
        "reload": lambda r: rng_pick_range(r, 1, 1024),
        "damage": lambda r: rng_pick_range(r, 1, 65535),
        "flight": lambda r: rng_pick_range(r, 1, 64),
        "proj_active": lambda r: r.below(2),
        "proj_left": lambda r: _small(r, 64),
        "proj_target": any_u32, "proj_target_gen": any_u32,
        "proj_damage": lambda r: _small(r, 65535),
    },
    "move": {
        "goal_x": _pos, "goal_y": _pos, "pos_x": _pos, "pos_y": _pos,
        "speed": lambda r: _small(r, 1024),
        "order_mode": lambda r: r.below(3),
        "target_valid": lambda r: r.below(2),
    },
    "damage": {
        "hp": lambda r: _small(r, 65535),
        "incoming": lambda r: _small(r, 65535),
    },
}


def rng_pick_range(rng, lo, hi):
    """Uniform-ish in [lo, hi] with the endpoints favoured."""
    if rng.below(3) == 0:
        return rng.pick((lo, hi))
    return lo + rng.below(hi - lo + 1)


def rows_any(system, rng, n):
    """n rows of arbitrary u32 values for the readable fields of system (IR-only comparison)."""
    cap, _ = load_record(system)
    names = [name for _, name in readable_fields(cap)]
    return [{name: any_u32(rng) for name in names} for _ in range(n)]


def rows_domain(system, rng, n):
    """n domain-valid rows for the readable fields of system (model comparison)."""
    cap, _ = load_record(system)
    names = [name for _, name in readable_fields(cap)]
    spec = DOMAIN_VALID[system]
    rows = []
    for _ in range(n):
        row = {name: spec[name](rng) for name in names}
        rows.append(row)
    return rows


def full_row(partial):
    """Fills the 39 model fields; the non-readable ones are zero (the model reads only its inputs)."""
    row = {name: 0 for name in model.FIELD_NAMES}
    row.update(partial)
    return row


def scene_world(spec):
    """Model world loaded from a committed-generator scene (rtsgen), all rows alive."""
    profile = rtsgen.load_profile()
    arr, _params, digest = rtsgen.make_scene(profile, spec)
    n = len(arr) // rtsgen.FIELD_COUNT
    world = model.World(n)
    for s in range(n):
        base = s * rtsgen.FIELD_COUNT
        for name in model.FIELD_NAMES:
            world.cols[name][s] = arr[base + rtsgen.IX[name]]
        world.alive[s] = 1
    world.hw = n
    return world, digest


def capture_ticks(world, ticks, keep_every=1):
    """Runs the model tick with recording wrappers on the four row systems. Returns a list over the kept
    ticks (every keep_every-th, from tick 0) of {system: [(row_in_39, out_model), ...]} in ascending slot
    order, plus the live slot list per tick. Rows are row-local, so the rows of several ticks may be
    concatenated into one engine call."""
    per_tick = []
    holder = {"rec": None}

    def make(name):
        fn = model.MODEL_SYSTEMS[name]

        def wrapped(row):
            out = fn(row)
            holder["rec"][name].append((dict(row), dict(out)))
            return out

        return wrapped

    systems = {name: make(name) for name in SYSTEMS}
    for t in range(ticks):
        holder["rec"] = {name: [] for name in SYSTEMS}
        live = world.live_slots()
        model.tick(world, systems)
        if t % keep_every == 0:
            per_tick.append({"live": live, "rec": holder["rec"]})
    return per_tick


def evaluator_fns():
    """{system: row fn} for the reference programs (bench/rts/ref) and for the committed records."""
    ref = ir_eval.ir_systems(ir_programs.all_programs())
    rec_progs = {}
    for name in SYSTEMS:
        cap, lir = load_record(name)
        rec_progs[name] = ir_program_from_record(cap, lir)
        ir_eval.check_program(rec_progs[name])
    rec = ir_eval.ir_systems(rec_progs)
    return ref, rec


def engine_call_inputs(system, rows39):
    """Calls input for a list of row dicts: {field_id: [values]} for the readable fields."""
    cap, _ = load_record(system)
    return {fid: [r[name] for r in rows39] for fid, name in readable_fields(cap)}


def compare_outputs(expected_rows, engine_out, writable):
    """expected_rows: list of dict (model or evaluator output, may hold _despawn). engine_out: {fid: [...]}.
    Returns a list of mismatch strings (empty when equal)."""
    problems = []
    for fid, name in writable:
        if fid not in engine_out:
            problems.append(f"engine omitted {name}")
            continue
        got = engine_out[fid]
        for i, exp in enumerate(expected_rows):
            if got[i] != exp[name]:
                problems.append(f"row {i} {name}: engine {got[i]} expected {exp[name]}")
                break
    return problems


def writable_names(system):
    cap, _ = load_record(system)
    return writable_fields(cap)


def cleanup(path):
    shutil.rmtree(path, ignore_errors=True)


def make_work_dir(prefix="rts_ir_"):
    return tempfile.mkdtemp(prefix=prefix)


def build_dir_from_env():
    path = os.environ.get("SU_BUILD_DIR")
    if not path:
        raise RuntimeError("set SU_BUILD_DIR to a release build directory (cmake -DCMAKE_BUILD_TYPE=Release)")
    return path
