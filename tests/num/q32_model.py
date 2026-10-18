# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Independent model of numeric profile fixed_q32_v1 (docs/NUMERIC_FIXED_Q32.md) with Python integers.

Written from the specification text, not from src/ir/su_num.c: every operation is computed on exact
rationals (Python int and fractions) and rounded with the specified rule. The C reference prints random
operation records (test_num --vectors); this script recomputes each result and reports every mismatch.

    q32_model.py --binary build/test_num --count 20000 --seeds 1 2 3
    q32_model.py --self-test
"""
import argparse
import math
import subprocess
import sys
from fractions import Fraction

I32_MIN, I32_MAX = -(1 << 31), (1 << 31) - 1
I64_MIN, I64_MAX = -(1 << 63), (1 << 63) - 1
ONE = 1 << 32


def wrap(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def sat(v):
    return I64_MAX if v > I64_MAX else I64_MIN if v < I64_MIN else v


def rne(fr):
    """Round a Fraction to the nearest integer, ties to even."""
    fl = fr.numerator // fr.denominator
    rem = fr - fl
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and fl % 2 == 1):
        return fl + 1
    return fl


def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def rne_sqrt_int(n):
    """RNE(sqrt(n)) for an integer n >= 0 (a tie cannot occur)."""
    r = math.isqrt(n)
    return r + 1 if n - r * r > r else r


# ---- integer families (wrapping) ----

def int_ops(bits):
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1

    def div(a, b):
        if b == 0:
            return 0
        if a == lo and b == -1:
            return lo
        return trunc_div(a, b)

    def shl(a, n):
        return 0 if n >= bits else wrap(a << n, bits)

    def shr(a, n):
        return (-1 if a < 0 else 0) if n >= bits else a >> n  # Python >> is floor

    return {
        "add": lambda a, b: wrap(a + b, bits),
        "sub": lambda a, b: wrap(a - b, bits),
        "mul": lambda a, b: wrap(a * b, bits),
        "div": div,
        "neg": lambda a: wrap(-a, bits),
        "abs": lambda a: wrap(abs(a), bits),
        "shl": shl,
        "shr": shr,
    }


I32 = int_ops(32)
I64 = int_ops(64)


# ---- q32 ----

def q_mul(a, b):
    return sat(rne(Fraction(a * b, ONE)))


def q_div(a, b):
    if b == 0:
        return I64_MAX if a > 0 else I64_MIN if a < 0 else 0
    return sat(rne(Fraction(a * ONE, b)))


def q_shl(a, n):
    return sat(a * (1 << n)) if n < 4096 else (0 if a == 0 else I64_MAX if a > 0 else I64_MIN)


def q_sqrt(a):
    return 0 if a <= 0 else rne_sqrt_int(a * ONE)


def q_rsqrt(a):
    return I64_MAX if a <= 0 else q_div(ONE, q_sqrt(a))


def q_from_ratio(num, den):
    if den == 0:
        return (0, 0)
    v = rne(Fraction(num * ONE, den))
    return (1, v) if I64_MIN <= v <= I64_MAX else (0, 0)


def q_to_i32_round(a):
    return min(rne(Fraction(a, ONE)), I32_MAX)


def v3_dot(a, b):
    return sat(rne(Fraction(sum(x * y for x, y in zip(a, b)), ONE)))


def v3_cross(a, b):
    return [sat(rne(Fraction(a[1] * b[2] - a[2] * b[1], ONE))),
            sat(rne(Fraction(a[2] * b[0] - a[0] * b[2], ONE))),
            sat(rne(Fraction(a[0] * b[1] - a[1] * b[0], ONE)))]


def norm_len(v):
    return min(rne_sqrt_int(sum(x * x for x in v)), I64_MAX)


def v3_normalize(a):
    n = norm_len(a)
    return [0, 0, 0] if n == 0 else [q_div(x, n) for x in a]


def v3_clamp_len(a, m):
    if m <= 0:
        return [0, 0, 0]
    n = norm_len(a)
    if n <= m:
        return list(a)
    return [sat(rne(Fraction(x * m, n))) for x in a]


def quat_mul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    exact = [aw * bw - ax * bx - ay * by - az * bz,
             aw * bx + ax * bw + ay * bz - az * by,
             aw * by - ax * bz + ay * bw + az * bx,
             aw * bz + ax * by - ay * bx + az * bw]
    return [sat(rne(Fraction(e, ONE))) for e in exact]


def quat_normalize(q):
    n = norm_len(q)
    return [ONE, 0, 0, 0] if n == 0 else [q_div(x, n) for x in q]


def q_add(a, b):
    return sat(a + b)


def quat_rotate(q, v):
    u = q[1:]
    t = v3_cross(u, v)
    t = [q_add(x, x) for x in t]
    c = v3_cross(u, t)
    return [q_add(q_add(v[i], q_mul(q[0], t[i])), c[i]) for i in range(3)]


def quat_integrate(q, w, dt):
    h = q_mul(dt, ONE // 2)
    p = quat_mul(q, [0] + list(w))
    q1 = [q_add(q[i], q_mul(p[i], h)) for i in range(4)]
    return quat_normalize(q1)


MODEL = {
    "i32_add": lambda a, b: I32["add"](a, b),
    "i32_sub": lambda a, b: I32["sub"](a, b),
    "i32_mul": lambda a, b: I32["mul"](a, b),
    "i32_div": lambda a, b: I32["div"](a, b),
    "i32_neg": lambda a: I32["neg"](a),
    "i32_abs": lambda a: I32["abs"](a),
    "i32_shl": lambda a, n: I32["shl"](a, n),
    "i32_shr": lambda a, n: I32["shr"](a, n),
    "i64_add": lambda a, b: I64["add"](a, b),
    "i64_sub": lambda a, b: I64["sub"](a, b),
    "i64_mul": lambda a, b: I64["mul"](a, b),
    "i64_div": lambda a, b: I64["div"](a, b),
    "i64_neg": lambda a: I64["neg"](a),
    "i64_abs": lambda a: I64["abs"](a),
    "i64_shl": lambda a, n: I64["shl"](a, n),
    "i64_shr": lambda a, n: I64["shr"](a, n),
    "q32_add": q_add,
    "q32_sub": lambda a, b: sat(a - b),
    "q32_mul": q_mul,
    "q32_div": q_div,
    "q32_neg": lambda a: sat(-a),
    "q32_abs": lambda a: sat(abs(a)),
    "q32_shl": q_shl,
    "q32_shr": lambda a, n: I64["shr"](a, n),
    "q32_sqrt": q_sqrt,
    "q32_rsqrt": q_rsqrt,
    "q32_from_i32": lambda a: a * ONE,
    "q32_from_i64": lambda a: sat(a * ONE),
    "q32_to_i32_floor": lambda a: a >> 32,
    "q32_to_i32_round": q_to_i32_round,
    "q32_to_i64_floor": lambda a: a >> 32,
}

# Number of input integers per record kind; the rest of the record is the C result.
ARITY = {k: (1 if k in ("i32_neg", "i32_abs", "i64_neg", "i64_abs", "q32_neg", "q32_abs", "q32_sqrt", "q32_rsqrt",
                        "q32_from_i32", "q32_from_i64", "q32_to_i32_floor", "q32_to_i32_round", "q32_to_i64_floor")
                  else 2) for k in MODEL}


def check_record(fields):
    """Returns None when the record matches the model, else a description."""
    op = fields[0]
    v = [int(x) for x in fields[1:]]
    if op in MODEL:
        n = ARITY[op]
        want = [MODEL[op](*v[:n])]
        got = v[n:]
    elif op == "q32_from_ratio":
        want = list(q_from_ratio(v[0], v[1]))
        got = v[2:]
    elif op == "v3_dot":
        want = [v3_dot(v[0:3], v[3:6])]
        got = v[6:]
    elif op == "v3_cross":
        want = v3_cross(v[0:3], v[3:6])
        got = v[6:]
    elif op == "v3_len":
        want = [norm_len(v[0:3])]
        got = v[3:]
    elif op == "v3_normalize":
        want = v3_normalize(v[0:3])
        got = v[3:]
    elif op == "v3_clamp_len":
        want = v3_clamp_len(v[0:3], v[3])
        got = v[4:]
    elif op == "quat_mul":
        want = quat_mul(v[0:4], v[4:8])
        got = v[8:]
    elif op == "quat_normalize":
        want = quat_normalize(v[0:4])
        got = v[4:]
    elif op == "quat_rotate":
        want = quat_rotate(v[0:4], v[4:7])
        got = v[7:]
    elif op == "quat_integrate":
        want = quat_integrate(v[0:4], v[4:7], v[7])
        got = v[8:]
    else:
        return "unknown record kind %s" % op
    if want != got:
        return "%s: model %s, C %s" % (" ".join(fields), want, got)
    return None


def self_test():
    """Hand-checked answers of the model itself (so a model bug cannot hide behind agreement)."""
    checks = [
        (q_mul(3, ONE // 2), 2), (q_mul(5, ONE // 2), 2), (q_mul(-3, ONE // 2), -2),
        (q_div(ONE, 3 * ONE), 1431655765), (q_div(2 * ONE, 3 * ONE), 2863311531),
        (q_sqrt(2 * ONE), 6074001000), (q_sqrt(1), 65536), (q_rsqrt(4 * ONE), ONE // 2),
        (q_from_ratio(1, 120), (1, 35791394)), (q_to_i32_round(5 * ONE // 2), 2),
        (q_to_i32_round(-5 * ONE // 2), -2), (I32["div"](I32_MIN, -1), I32_MIN), (I32["shr"](-5, 1), -3),
        (v3_dot([ONE, 2 * ONE, 3 * ONE], [4 * ONE, 5 * ONE, 6 * ONE]), 32 * ONE),
        (norm_len([3 * ONE, 0, 4 * ONE]), 5 * ONE),
        (quat_mul([0, ONE, 0, 0], [0, 0, ONE, 0]), [0, 0, 0, ONE]),
    ]
    bad = [(i, got, want) for i, (got, want) in enumerate(checks) if got != want]
    for i, got, want in bad:
        print("model self-test %d: got %s want %s" % (i, got, want))
    print("q32_model self-test: %d checks, %d failures" % (len(checks), len(bad)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("--seeds", type=int, nargs="*", default=[1])
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    rc = self_test()
    if args.self_test:
        return rc
    if not args.binary:
        ap.error("--binary is required")
    total = 0
    failures = []
    kinds = {}
    for seed in args.seeds:
        out = subprocess.run([args.binary, "--vectors", str(args.count), str(seed)], check=True,
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            fields = line.split()
            if not fields:
                continue
            total += 1
            kinds[fields[0]] = kinds.get(fields[0], 0) + 1
            err = check_record(fields)
            if err is not None:
                failures.append(err)
    for f in failures[:40]:
        print("MISMATCH", f)
    print("q32_model: %d records, %d kinds, %d mismatches (seeds %s, count %d)"
          % (total, len(kinds), len(failures), args.seeds, args.count))
    return 1 if failures or rc or total == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
