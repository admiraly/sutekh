# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""IR 0.3 differential test: seeded random programs, the real verifier and VM (ir03_driver), and an independent
interpreter written here from the LIVE_IR_SPEC "IR 0.3" table and docs/NUMERIC_FIXED_Q32.md.

The interpreter shares only the number model of tests/num/q32_model.py (itself checked against hand-derived
answers and the C library). Everything IR-specific (op table, operand keys, lane layout "<name>.lo"/".hi",
loads/stores, register typing) is re-implemented here, so a bug in the verifier encoding, the VM dispatch or the
lane plumbing shows up as a mismatch.

    ir03_diff.py --driver build/ir03_driver --work <dir> --programs 300 --rows 48 --seed 1

Every program also runs on both VM tiers (su_vm.h): the driver reports whether the planned and reference tiers
committed identical lanes, and that must hold for every program. Every third program is "guarded": its stores
select between a computed value and the field's own loaded value under conditions on a u32 "gate" field
(identity on the false side, on the true side, nested, or a decoy that stores another field's value), which is
the shape the planned tier's identity-row guard proves and skips. "Clean" guarded programs must get a guard and
skip rows, so the proof is exercised, not only tolerated.
"""
import argparse
import hashlib
import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "num"))
import q32_model as Q  # noqa: E402

SCALARS = ("i32", "i64", "q32")
FAM = ("add", "sub", "mul", "div", "neg", "abs", "min", "max", "clamp", "eq", "lt", "le", "select", "shl", "shr")


def build_ops():
    """name -> (dst type or None, [(key, kind, type)]). Kinds: reg, fread, fwrite, ci32, ci64, cnum, cden, axis."""
    ops = {
        "const_u32": ("u32", [("value", "cu32", None)]),
        "add_u32": ("u32", [("a", "reg", "u32"), ("b", "reg", "u32")]),
        "mul_u32": ("u32", [("a", "reg", "u32"), ("b", "reg", "u32")]),
        "lt_u32": ("bool", [("a", "reg", "u32"), ("b", "reg", "u32")]),
        "select_u32": ("u32", [("cond", "reg", "bool"), ("on_true", "reg", "u32"), ("on_false", "reg", "u32")]),
        "const_i32": ("i32", [("value", "ci32", None)]),
        "const_i64": ("i64", [("value", "ci64", None)]),
        "const_q32": ("q32", [("num", "cnum", None), ("den", "cden", None)]),
        "and_bool": ("bool", [("a", "reg", "bool"), ("b", "reg", "bool")]),
        "or_bool": ("bool", [("a", "reg", "bool"), ("b", "reg", "bool")]),
        "not_bool": ("bool", [("a", "reg", "bool")]),
        "sqrt_q32": ("q32", [("a", "reg", "q32")]),
        "rsqrt_q32": ("q32", [("a", "reg", "q32")]),
        "i32_from_u32": ("i32", [("a", "reg", "u32")]),
        "u32_from_i32": ("u32", [("a", "reg", "i32")]),
        "i64_from_i32": ("i64", [("a", "reg", "i32")]),
        "i32_from_i64": ("i32", [("a", "reg", "i64")]),
        "q32_from_i32": ("q32", [("a", "reg", "i32")]),
        "q32_from_i64": ("q32", [("a", "reg", "i64")]),
        "q32_to_i32_floor": ("i32", [("a", "reg", "q32")]),
        "q32_to_i32_round": ("i32", [("a", "reg", "q32")]),
        "q32_from_raw": ("q32", [("a", "reg", "i64")]),
        "q32_raw": ("i64", [("a", "reg", "q32")]),
        "q32_to_i64_floor": ("i64", [("a", "reg", "q32")]),
        "make_v3": ("v3", [("x", "reg", "q32"), ("y", "reg", "q32"), ("z", "reg", "q32")]),
        "v3_get": ("q32", [("v", "reg", "v3"), ("axis", "axis", 3)]),
        "v3_add": ("v3", [("a", "reg", "v3"), ("b", "reg", "v3")]),
        "v3_sub": ("v3", [("a", "reg", "v3"), ("b", "reg", "v3")]),
        "v3_scale": ("v3", [("v", "reg", "v3"), ("s", "reg", "q32")]),
        "v3_neg": ("v3", [("a", "reg", "v3")]),
        "v3_dot": ("q32", [("a", "reg", "v3"), ("b", "reg", "v3")]),
        "v3_cross": ("v3", [("a", "reg", "v3"), ("b", "reg", "v3")]),
        "v3_len": ("q32", [("a", "reg", "v3")]),
        "v3_len_sq": ("q32", [("a", "reg", "v3")]),
        "v3_normalize": ("v3", [("a", "reg", "v3")]),
        "v3_clamp_len": ("v3", [("v", "reg", "v3"), ("max", "reg", "q32")]),
        "select_v3": ("v3", [("cond", "reg", "bool"), ("on_true", "reg", "v3"), ("on_false", "reg", "v3")]),
        "make_quat": ("quat", [("w", "reg", "q32"), ("v", "reg", "v3")]),
        "quat_get": ("q32", [("q", "reg", "quat"), ("axis", "axis", 4)]),
        "quat_mul": ("quat", [("a", "reg", "quat"), ("b", "reg", "quat")]),
        "quat_conj": ("quat", [("a", "reg", "quat")]),
        "quat_rotate": ("v3", [("q", "reg", "quat"), ("v", "reg", "v3")]),
        "quat_normalize": ("quat", [("a", "reg", "quat")]),
        "quat_integrate": ("quat", [("q", "reg", "quat"), ("w", "reg", "v3"), ("dt", "reg", "q32")]),
        "select_quat": ("quat", [("cond", "reg", "bool"), ("on_true", "reg", "quat"), ("on_false", "reg", "quat")]),
    }
    for t in SCALARS:
        for f in FAM:
            name = "%s_%s" % (f, t)
            if f in ("neg", "abs"):
                ops[name] = (t, [("a", "reg", t)])
            elif f == "clamp":
                ops[name] = (t, [("x", "reg", t), ("lo", "reg", t), ("hi", "reg", t)])
            elif f in ("eq", "lt", "le"):
                ops[name] = ("bool", [("a", "reg", t), ("b", "reg", t)])
            elif f == "select":
                ops[name] = (t, [("cond", "reg", "bool"), ("on_true", "reg", t), ("on_false", "reg", t)])
            elif f in ("shl", "shr"):
                ops[name] = (t, [("a", "reg", t), ("n", "reg", "u32")])
            else:
                ops[name] = (t, [("a", "reg", t), ("b", "reg", t)])
    return ops


OPS = build_ops()
COMPUTE = [n for n in OPS if not n.startswith("const_")]


# ---------- interpreter ----------

def wrap(v, bits):
    return Q.wrap(v, bits)


def family(name, t, args):
    f = name[: name.rindex("_")]
    if f in ("min", "max", "clamp", "eq", "lt", "le", "select"):
        if f == "min":
            return min(args)
        if f == "max":
            return max(args)
        if f == "clamp":
            return min(max(args[0], args[1]), args[2])
        if f == "eq":
            return args[0] == args[1]
        if f == "lt":
            return args[0] < args[1]
        if f == "le":
            return args[0] <= args[1]
        return args[1] if args[0] else args[2]
    if t in ("i32", "i64"):
        fam = Q.I32 if t == "i32" else Q.I64
        return fam[f](*args)
    table = {"add": Q.q_add, "sub": lambda a, b: Q.sat(a - b), "mul": Q.q_mul, "div": Q.q_div,
             "neg": lambda a: Q.sat(-a), "abs": lambda a: Q.sat(abs(a)), "shl": Q.q_shl,
             "shr": lambda a, n: Q.I64["shr"](a, n)}
    return table[f](*args)


def evaluate(name, ins, regs):
    dst_t, operands = OPS[name]
    args = []
    for key, kind, _ in operands:
        if kind == "reg":
            args.append(regs[ins[key]][1])
        else:
            args.append(ins[key])
    if name == "const_u32":
        return args[0]
    if name in ("const_i32", "const_i64"):
        return args[0]
    if name == "const_q32":
        ok, v = Q.q_from_ratio(args[0], args[1])
        assert ok
        return v
    if name == "add_u32":
        return (args[0] + args[1]) & 0xFFFFFFFF
    if name == "mul_u32":
        return (args[0] * args[1]) & 0xFFFFFFFF
    if name == "lt_u32":
        return args[0] < args[1]
    if name == "select_u32":
        return args[1] if args[0] else args[2]
    if name == "and_bool":
        return args[0] and args[1]
    if name == "or_bool":
        return args[0] or args[1]
    if name == "not_bool":
        return not args[0]
    if name == "sqrt_q32":
        return Q.q_sqrt(args[0])
    if name == "rsqrt_q32":
        return Q.q_rsqrt(args[0])
    if name == "i32_from_u32":
        return wrap(args[0], 32)
    if name == "u32_from_i32":
        return args[0] & 0xFFFFFFFF
    if name == "i64_from_i32":
        return args[0]
    if name == "i32_from_i64":
        return wrap(args[0], 32)
    if name == "q32_from_i32":
        return args[0] * Q.ONE
    if name == "q32_from_i64":
        return Q.sat(args[0] * Q.ONE)
    if name == "q32_to_i32_floor":
        return args[0] >> 32
    if name == "q32_to_i32_round":
        return Q.q_to_i32_round(args[0])
    if name in ("q32_from_raw", "q32_raw"):
        return args[0]
    if name == "q32_to_i64_floor":
        return args[0] >> 32
    if name == "make_v3":
        return tuple(args)
    if name in ("v3_get", "quat_get"):
        return args[0][args[1]]
    if name == "v3_add":
        return tuple(Q.q_add(a, b) for a, b in zip(args[0], args[1]))
    if name == "v3_sub":
        return tuple(Q.sat(a - b) for a, b in zip(args[0], args[1]))
    if name == "v3_scale":
        return tuple(Q.q_mul(a, args[1]) for a in args[0])
    if name == "v3_neg":
        return tuple(Q.sat(-a) for a in args[0])
    if name == "v3_dot":
        return Q.v3_dot(args[0], args[1])
    if name == "v3_cross":
        return tuple(Q.v3_cross(args[0], args[1]))
    if name == "v3_len":
        return Q.norm_len(args[0])
    if name == "v3_len_sq":
        return Q.v3_dot(args[0], args[0])
    if name == "v3_normalize":
        return tuple(Q.v3_normalize(args[0]))
    if name == "v3_clamp_len":
        return tuple(Q.v3_clamp_len(args[0], args[1]))
    if name in ("select_v3", "select_quat"):
        return args[1] if args[0] else args[2]
    if name == "make_quat":
        return (args[0],) + tuple(args[1])
    if name == "quat_mul":
        return tuple(Q.quat_mul(args[0], args[1]))
    if name == "quat_conj":
        return (args[0][0],) + tuple(Q.sat(-a) for a in args[0][1:])
    if name == "quat_rotate":
        return tuple(Q.quat_rotate(args[0], args[1]))
    if name == "quat_normalize":
        return tuple(Q.quat_normalize(args[0]))
    if name == "quat_integrate":
        return tuple(Q.quat_integrate(args[0], args[1], args[2]))
    t = name[name.rindex("_") + 1:]
    return family(name, t, args)


def lanes_of(fields):
    """Lane names in the verified layout: 64-bit fields are '<n>.lo' and '<n>.hi'."""
    out = []
    for f in fields:
        if f["type"] in ("i64", "q32"):
            out += [(f["name"] + ".lo", f, 0), (f["name"] + ".hi", f, 1)]
        else:
            out.append((f["name"], f, 0))
    return out


def run_program(fields, insns, lane_in):
    """Returns {lane name: u32} of the stored lanes for one row."""
    vals = {}
    for f in fields:
        if f["type"] in ("i64", "q32"):
            raw = lane_in[f["name"] + ".lo"] | (lane_in[f["name"] + ".hi"] << 32)
            vals[f["name"]] = wrap(raw, 64)
        elif f["type"] == "i32":
            vals[f["name"]] = wrap(lane_in[f["name"]], 32)
        else:
            vals[f["name"]] = lane_in[f["name"]]
    regs = {}
    out = {}

    def put(fname, v):
        f = next(x for x in fields if x["name"] == fname)
        if f["type"] in ("i64", "q32"):
            u = v & 0xFFFFFFFFFFFFFFFF
            out[fname + ".lo"] = u & 0xFFFFFFFF
            out[fname + ".hi"] = u >> 32
        else:
            out[fname] = v & 0xFFFFFFFF

    for ins in insns:
        op = ins["op"]
        if op.startswith("load_"):
            t = op[5:]
            if t == "v3":
                regs[ins["dst"]] = ("v3", (vals[ins["x"]], vals[ins["y"]], vals[ins["z"]]))
            else:
                regs[ins["dst"]] = (t, vals[ins["field"]])
        elif op.startswith("store_"):
            if op == "store_v3":
                v = regs[ins["src"]][1]
                put(ins["x"], v[0])
                put(ins["y"], v[1])
                put(ins["z"], v[2])
            else:
                put(ins["field"], regs[ins["src"]][1])
        else:
            regs[ins["dst"]] = (OPS[op][0], evaluate(op, ins, regs))
    return out


# ---------- generator ----------

def gen_program(rng, idx, guard_policy=None):
    """guard_policy: None (plain), "clean" (every store identity when the gate condition is false) or "mixed"."""
    nfields = {"q32": rng.randint(3, 6), "i32": rng.randint(1, 3), "i64": rng.randint(1, 2), "u32": rng.randint(1, 3)}
    ids = rng.sample(range(1, 400, 2), sum(nfields.values()))
    fields = []
    k = 0
    for t, n in nfields.items():
        for i in range(n):
            fields.append({"id": ids[k], "name": "%s_%d" % (t, i), "type": t, "read": True,
                           "write": rng.random() < (0.7 if guard_policy else 0.5)})
            k += 1
    if guard_policy:
        fields.append({"id": 401, "name": "gate", "type": "u32", "read": True, "write": False})
    rng.shuffle(fields)
    insns = []
    pool = {}
    counter = [0]

    def new_reg(t):
        counter[0] += 1
        name = "r%d" % counter[0]
        pool.setdefault(t, []).append(name)
        return name

    def get(t):
        if pool.get(t) and rng.random() < 0.85:
            return rng.choice(pool[t])
        return make(t)

    def make(t):
        if t == "u32":
            ins = {"op": "const_u32", "dst": None, "value": rng.choice([0, 1, 5, 31, 32, 33, 63, 64, 200,
                                                                         rng.getrandbits(32)])}
        elif t == "i32":
            ins = {"op": "const_i32", "dst": None, "value": rng.choice([0, 1, -1, 3, -7, 2 ** 31 - 1, -2 ** 31,
                                                                         rng.randint(-1000, 1000)])}
        elif t == "i64":
            ins = {"op": "const_i64", "dst": None, "value": rng.choice([0, 1, -1, 2 ** 63 - 1, -2 ** 63,
                                                                         rng.randint(-2 ** 63, 2 ** 63 - 1)])}
        elif t == "q32":
            den = rng.choice([1, 2, 3, 7, 10, 120, 2 ** 32, rng.randint(1, 2 ** 40)])
            ins = {"op": "const_q32", "dst": None, "num": rng.randint(-1000, 1000), "den": den}
        elif t == "bool":
            tt = rng.choice(SCALARS)
            a, b = get(tt), get(tt)
            ins = {"op": rng.choice(["lt_", "le_", "eq_"]) + tt, "dst": None, "a": a, "b": b}
        elif t == "v3":
            ins = {"op": "make_v3", "dst": None, "x": get("q32"), "y": get("q32"), "z": get("q32")}
        else:
            ins = {"op": "make_quat", "dst": None, "w": get("q32"), "v": get("v3")}
        ins["dst"] = new_reg(t)
        insns.append(ins)
        return ins["dst"]

    # Loads (sometimes a load_v3 over three readable q32 fields).
    qf = [f for f in fields if f["type"] == "q32"]
    if len(qf) >= 3 and rng.random() < 0.5:
        a, b, c = rng.sample(qf, 3)
        d = new_reg("v3")
        insns.append({"op": "load_v3", "dst": d, "x": a["name"], "y": b["name"], "z": c["name"]})
    loaded = {}
    for f in fields:
        if guard_policy or rng.random() < 0.9:
            d = new_reg(f["type"])
            insns.append({"op": "load_" + f["type"], "dst": d, "field": f["name"]})
            loaded[f["name"]] = d
    for _ in range(rng.randint(15, 60)):
        name = rng.choice(COMPUTE)
        dst_t, operands = OPS[name]
        ins = {"op": name}
        for key, kind, t in operands:
            if kind == "reg":
                ins[key] = get(t)
            elif kind == "axis":
                ins[key] = rng.randrange(t)
        ins["dst"] = new_reg(dst_t)
        insns.append(ins)
    if guard_policy:
        return guarded_stores(rng, idx, fields, insns, loaded, get, new_reg, guard_policy)
    # Stores: one per writable field (a store_v3 for three writable q32 fields when possible).
    wf = [f for f in fields if f["write"]]
    wq = [f for f in wf if f["type"] == "q32"]
    stored = set()
    if len(wq) >= 3 and rng.random() < 0.5:
        a, b, c = rng.sample(wq, 3)
        insns.append({"op": "store_v3", "x": a["name"], "y": b["name"], "z": c["name"], "src": get("v3")})
        stored |= {a["name"], b["name"], c["name"]}
    for f in wf:
        if f["name"] not in stored:
            insns.append({"op": "store_" + f["type"], "field": f["name"], "src": get(f["type"])})
    capsule = {
        "capsule_format": "0.1", "ir_version": "0.3", "system_id": "diff.p%d" % idx, "source": "p.lir.json",
        "schema": {"id": "diff.table", "version": 1, "layout_generation": 1}, "numeric_profile": "fixed_q32_v1",
        "fields": fields,
        "schedule": {"phase": "simulate", "after": [], "before": [], "parallel": "serial"},
        "limits": {"max_instructions": 4096, "max_rows": 1000000, "allocation": "forbidden"},
        "tests": ["p.test.json"],
    }
    lir = {"ir_version": "0.3", "system_id": "diff.p%d" % idx, "numeric_profile": "fixed_q32_v1",
           "query": "diff.table", "instructions": insns}
    return capsule, lir


def guarded_stores(rng, idx, fields, insns, loaded, get, new_reg, policy):
    """Stores of a guarded program; see the module docstring."""
    c35 = new_reg("u32")
    insns.append({"op": "const_u32", "dst": c35, "value": rng.choice([10, 35, 60])})
    c1 = new_reg("bool")
    insns.append({"op": "lt_u32", "dst": c1, "a": loaded["gate"], "b": c35})
    other = get("bool")  # before new_reg: the operand must not be the register being defined
    c2 = new_reg("bool")
    insns.append({"op": "and_bool", "dst": c2, "a": c1, "b": other})

    def sel(t, cond, on_true, on_false):
        d = new_reg(t)
        insns.append({"op": "select_" + t, "dst": d, "cond": cond, "on_true": on_true, "on_false": on_false})
        return d

    def value_for(f):
        t = f["type"]
        own = loaded[f["name"]]
        if policy == "clean":
            return sel(t, c1, get(t), own)
        mode = rng.choice(["false_side", "true_side", "nested", "decoy", "plain"])
        if mode == "false_side":
            return sel(t, c1, get(t), own)
        if mode == "true_side":
            return sel(t, c1, own, get(t))
        if mode == "nested":
            return sel(t, c2, get(t), sel(t, c1, get(t), own))
        if mode == "decoy":
            others = [x for x in fields if x["type"] == t and x["name"] != f["name"]]
            return sel(t, c1, get(t), loaded[others[0]["name"]] if others else get(t))
        return get(t)

    wf = [f for f in fields if f["write"]]
    wq = [f for f in wf if f["type"] == "q32"]
    stored = set()
    if len(wq) >= 3 and rng.random() < 0.6:
        a, b, c = rng.sample(wq, 3)
        if rng.random() < 0.5:
            own = new_reg("v3")
            insns.append({"op": "load_v3", "dst": own, "x": a["name"], "y": b["name"], "z": c["name"]})
        else:
            own = new_reg("v3")
            insns.append({"op": "make_v3", "dst": own, "x": loaded[a["name"]], "y": loaded[b["name"]],
                          "z": loaded[c["name"]]})
        src = sel("v3", c1, get("v3"), own)
        insns.append({"op": "store_v3", "x": a["name"], "y": b["name"], "z": c["name"], "src": src})
        stored |= {a["name"], b["name"], c["name"]}
    for f in wf:
        if f["name"] not in stored:
            insns.append({"op": "store_" + f["type"], "field": f["name"], "src": value_for(f)})
    capsule = {
        "capsule_format": "0.1", "ir_version": "0.3", "system_id": "diff.p%d" % idx, "source": "p.lir.json",
        "schema": {"id": "diff.table", "version": 1, "layout_generation": 1}, "numeric_profile": "fixed_q32_v1",
        "fields": fields,
        "schedule": {"phase": "simulate", "after": [], "before": [], "parallel": "serial"},
        "limits": {"max_instructions": 4096, "max_rows": 1000000, "allocation": "forbidden"},
        "tests": ["p.test.json"],
    }
    lir = {"ir_version": "0.3", "system_id": "diff.p%d" % idx, "numeric_profile": "fixed_q32_v1",
           "query": "diff.table", "instructions": insns}
    return capsule, lir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--programs", type=int, default=300)
    ap.add_argument("--rows", type=int, default=48)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    rng = random.Random(args.seed)
    corpus = hashlib.sha256()
    mismatches = []
    ops_seen = set()
    checked = 0
    tier_stats = {"plans": 0, "guards": 0, "clean": 0, "clean_guards": 0, "skipped_rows": 0, "body_rows": 0,
                  "hoisted": 0}
    for p in range(args.programs):
        policy = None if p % 3 != 1 else ("clean" if p % 6 == 1 else "mixed")
        capsule, lir = gen_program(rng, p, policy)
        cap_text = json.dumps(capsule, sort_keys=True)
        lir_text = json.dumps(lir, sort_keys=True)
        corpus.update(cap_text.encode() + b"\0" + lir_text.encode() + b"\0")
        for ins in lir["instructions"]:
            ops_seen.add(ins["op"])
        cp = os.path.join(args.work, "p.capsule.json")
        lp = os.path.join(args.work, "p.lir.json")
        with open(cp, "w", newline="\n") as f:
            f.write(cap_text)
        with open(lp, "w", newline="\n") as f:
            f.write(lir_text)
        out = subprocess.run([args.driver, cp, lp, str(args.rows), str(args.seed * 100003 + p)],
                             capture_output=True, text=True, check=True).stdout.splitlines()
        status = out[0].split()
        if status[1] != "0":
            mismatches.append("program %d: driver status %s" % (p, " ".join(status[1:])))
            continue
        lane_in = {}
        lane_out = {}
        tiers = out[1].split()
        if tiers[0] != "TIERS" or tiers[1] != "1":
            mismatches.append("program %d: planned and reference VM tiers differ (%s)" % (p, out[1]))
            continue
        has_plan, has_guard, body_rows, skipped, hoisted = (int(x) for x in tiers[2:7])
        tier_stats["plans"] += has_plan
        tier_stats["guards"] += has_guard
        tier_stats["body_rows"] += body_rows
        tier_stats["skipped_rows"] += skipped
        tier_stats["hoisted"] += hoisted
        if policy == "clean":
            tier_stats["clean"] += 1
            tier_stats["clean_guards"] += 1 if (has_guard and skipped > 0) else 0
        for line in out[2:]:
            kind, row, lane, val = line.split()
            (lane_in if kind == "IN" else lane_out).setdefault(int(row), {})[lane] = int(val)
        for r in range(args.rows):
            want = run_program(capsule["fields"], lir["instructions"], lane_in[r])
            got = lane_out.get(r, {})
            checked += len(want)
            if want != got:
                diff = {k: (want.get(k), got.get(k)) for k in set(want) | set(got) if want.get(k) != got.get(k)}
                mismatches.append("program %d row %d: %s" % (p, r, diff))
                break
    missing = sorted(set(OPS) - ops_seen - {"const_u32", "const_i32", "const_i64", "const_q32"})
    if tier_stats["clean"] > 0 and tier_stats["clean_guards"] != tier_stats["clean"]:
        mismatches.append("guard not exercised: %d of %d clean guarded programs got a guard that skipped rows"
                          % (tier_stats["clean_guards"], tier_stats["clean"]))
    for m in mismatches[:20]:
        print("MISMATCH", m)
    print("ir03_diff tiers: %d programs with a plan, %d with a guard (%d of %d clean guarded programs), "
          "%d rows executed, %d rows skipped by guards, %d hoisted instructions"
          % (tier_stats["plans"], tier_stats["guards"], tier_stats["clean_guards"], tier_stats["clean"],
             tier_stats["body_rows"], tier_stats["skipped_rows"], tier_stats["hoisted"]))
    print("ir03_diff: %d programs x %d rows, %d lane values compared, %d ops covered (%d not drawn: %s), "
          "corpus sha256 %s, %d mismatches"
          % (args.programs, args.rows, checked, len(ops_seen), len(missing), missing, corpus.hexdigest(),
             len(mismatches)))
    return 1 if mismatches or checked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
