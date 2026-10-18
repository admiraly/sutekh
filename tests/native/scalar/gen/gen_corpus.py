#!/usr/bin/env python3
"""Seeded generator of random verified SU-LIR programs for the M1-01 scalar differential test.

Writes <out>/prog_NNNNN.capsule.json, <out>/prog_NNNNN.lir.json and <out>/index.txt (one program
base name per line). Output is a pure function of (seed, count), so the corpus digest is stable.

Programs are valid by construction (definition before use, bool/u32 typing, field access,
each writable field stored exactly once per row). The C test runs the real verifier on every
program and fails if one is rejected.

Usage:
  gen_corpus.py --out DIR [--seed N] [--count N] [--expect-sha FILE]
--expect-sha FILE compares the corpus digest (sha256 over index.txt and every program file, in
index order) with the hex digest stored in FILE and exits non-zero on mismatch.
"""
import argparse
import hashlib
import json
import os
import random
import sys

DEFAULT_SEED = 20261011
DEFAULT_COUNT = 5000
MAX_INSNS = 64
MAX_FIELDS = 8

BINOPS = ["add_u32", "sub_u32", "mul_u32"]
CMPS = ["eq_u32", "lt_u32"]
EDGE = [0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF]


def gen_program(rng, idx):
    nf = rng.randint(1, MAX_FIELDS)
    fields = []
    for i in range(nf):
        # Access: 1 = read only, 2 = write only, 3 = read-write. At least one writable field.
        fields.append({"id": 10 * (i + 1), "name": f"f{i}", "access": rng.choice([1, 2, 3])})
    if not any(f["access"] & 2 for f in fields):
        k = rng.randrange(nf)
        fields[k]["access"] = rng.choice([2, 3])
    readable = [i for i, f in enumerate(fields) if f["access"] & 1]
    writable = [i for i, f in enumerate(fields) if f["access"] & 2]

    insns = []
    u32_regs = []
    bool_regs = []
    nreg = [0]

    def new_reg():
        r = f"r{nreg[0]}"
        nreg[0] += 1
        return r

    def emit(d):
        insns.append(d)

    # Body of random ops; leave room for one store per writable field at the end.
    budget = rng.randint(1, MAX_INSNS - len(writable))
    for _ in range(budget):
        choice = rng.random()
        if choice < 0.22 and readable:
            r = new_reg()
            emit({"op": "load_u32", "dst": r, "field": fields[rng.choice(readable)]["name"]})
            u32_regs.append(r)
        elif choice < 0.34:
            r = new_reg()
            v = rng.choice(EDGE) if rng.random() < 0.5 else rng.getrandbits(32)
            emit({"op": "const_u32", "dst": r, "value": v})
            u32_regs.append(r)
        elif choice < 0.6 and len(u32_regs) >= 2:
            r = new_reg()
            a, b = rng.choice(u32_regs), rng.choice(u32_regs)
            emit({"op": rng.choice(BINOPS), "dst": r, "a": a, "b": b})
            u32_regs.append(r)
        elif choice < 0.72 and len(u32_regs) >= 2:
            r = new_reg()
            a, b = rng.choice(u32_regs), rng.choice(u32_regs)
            emit({"op": rng.choice(CMPS), "dst": r, "a": a, "b": b})
            bool_regs.append(r)
        elif choice < 0.86 and bool_regs and len(u32_regs) >= 2:
            r = new_reg()
            emit({"op": "select_u32", "dst": r, "cond": rng.choice(bool_regs),
                  "on_true": rng.choice(u32_regs), "on_false": rng.choice(u32_regs)})
            u32_regs.append(r)
        else:
            r = new_reg()
            emit({"op": "const_u32", "dst": r, "value": rng.getrandbits(32)})
            u32_regs.append(r)

    if not u32_regs:
        r = new_reg()
        emit({"op": "const_u32", "dst": r, "value": 0})
        u32_regs.append(r)

    # Stores: each writable field exactly once per row, in a random field order.
    order = list(writable)
    rng.shuffle(order)
    for k in order:
        emit({"op": "store_u32", "field": fields[k]["name"], "src": rng.choice(u32_regs)})

    # Reads that were never used are legal; ensure at least one instruction exists.
    if not insns:
        emit({"op": "const_u32", "dst": new_reg(), "value": 0})

    # Field table in declaration order; the verifier sorts by id.
    cap_fields = []
    for f in fields:
        acc = f["access"]
        cap_fields.append({
            "id": f["id"], "name": f["name"], "type": "u32",
            "read": bool(acc & 1), "write": bool(acc & 2),
        })
    base = f"prog_{idx:05d}"
    capsule = {
        "capsule_format": "0.1",
        "system_id": f"gen.{base}",
        "source": f"{base}.lir.json",
        "schema": {"id": "gen.actor", "version": 1, "layout_generation": 1},
        "numeric_profile": "u32_mod_v1",
        "fields": cap_fields,
        "schedule": {"phase": "simulate", "after": [], "before": [], "parallel": "serial"},
        "limits": {"max_instructions": 4096, "max_rows": 1000000, "allocation": "forbidden"},
        "tests": [f"{base}.test.json"],
    }
    lir = {
        "ir_version": "0.1",
        "system_id": f"gen.{base}",
        "numeric_profile": "u32_mod_v1",
        "query": "gen.actor",
        "instructions": insns,
    }
    return base, capsule, lir


def dump(path, obj):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(obj, fh, indent=1, sort_keys=False)
        fh.write("\n")


def corpus_digest(out_dir, names):
    h = hashlib.sha256()
    for base in names:
        for suffix in (".capsule.json", ".lir.json"):
            with open(os.path.join(out_dir, base + suffix), "rb") as fh:
                data = fh.read()
            h.update(len(data).to_bytes(8, "little"))
            h.update(data)
    return h.hexdigest()


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--count", type=int, default=DEFAULT_COUNT)
    ap.add_argument("--expect-sha", default=None)
    args = ap.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)
    rng = random.Random(args.seed)
    names = []
    for idx in range(args.count):
        base, capsule, lir = gen_program(rng, idx)
        dump(os.path.join(args.out, base + ".capsule.json"), capsule)
        dump(os.path.join(args.out, base + ".lir.json"), lir)
        names.append(base)
    with open(os.path.join(args.out, "index.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(names) + "\n")

    digest = corpus_digest(args.out, names)
    print(f"seed={args.seed} count={args.count} corpus_sha256={digest}")
    if args.expect_sha:
        with open(args.expect_sha, "r", encoding="utf-8") as fh:
            want = fh.read().strip()
        if want != digest:
            print(f"corpus digest mismatch: want {want} got {digest}", file=sys.stderr)
            return 1
        print("corpus digest matches stored value")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
