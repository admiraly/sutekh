<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Numeric profile `fixed_q32_v1`

Status: specified and implemented in the reference library `src/ir/su_num.c` (2026-10-18). Used by IR 0.3 (`LIVE_IR_SPEC.md`, section "IR 0.3") and the space3d host operators (`docs/SPACE3D.md`). Authorization: `planning/amendments/2026-10-18-numeric-3d.md`.

## 1. Decision record

**Need.** A 6DOF racing workload: 120 Hz fixed simulation; server-authoritative multiplayer with prediction, reconciliation, rollback and replays. Ship state is position (vec3), unit orientation quaternion, world velocity, local angular velocity, timers and [0,1] scalars. Speeds reach about 1,000 m/s (8.3 m per tick); courses span tens of km. Requirements, in order: bit-exact results across x64 compilers and backends (replay and rollback); ±100 km at sub-millimetre precision; acceptable interpreter speed.

| Option | Range | Resolution | Determinism risk | Cost per op | Verdict |
|---|---|---|---|---|---|
| Q16.16 in i32 | ±32,768 m | 1.5e-5 m | none (integer) | lowest | rejected: range is 3x too small |
| Q48.16 in i64 | ±1.4e14 m | 1.5e-5 m | none | 64-bit mul plus a 128-bit shift | rejected: unit vectors and quaternions get 16 fractional bits; a 0.01 rad/s turn integrated at 120 Hz (8.3e-5 rad per tick) is quantized by 18% |
| **Q32.32 in i64** | ±2.1e9 m | 2.3e-10 m | none: integers with explicit rounding | 4 partial products for mul; 128/64 division for div | **chosen** |
| strict IEEE f64, no contraction | ±1e308 | 1.5e-11 m at 100 km | high: contraction (clang's C default fuses within an expression when FMA is enabled), x87 vs SSE2, fast-math, MXCSR DAZ/FTZ state changed by third-party DLLs, NaN payloads and signed zero in hashes, libm sin/cos differences between CRTs; every future native lowering must preserve all of it | fastest on hardware | deferred: a valid later profile (`fp64_strict_v1`), not the authoritative default |

Q32.32 makes determinism a property of integer arithmetic: every result is a pure function of the operand bits, specified below and implemented without floating point, compiler-specific 128-bit types, signed overflow or implementation-defined conversions. Any conforming C17 compiler (MSVC, clang, gcc) and any future native lowering that matches the reference produces identical bits. The cost is that wide products need explicit 128/192-bit limbs. The integer range (±2^31 m) is far beyond the course size; the useful property is the 32 fractional bits, which keep quaternion, normal and angular-increment error near 2^-32.

**Range consequences.** A product of two Q32.32 values must fit Q32.32: squared lengths above (46,340 m)^2 saturate. Length, normalize, clamp-magnitude, dot and cross are therefore defined on exact wide sums and rounded once, so `v3_len` is exact (to RNE) for any representable vector; `v3_len_sq` and `v3_dot` saturate for long vectors. Authors who need squared distances over long baselines should compare lengths or work in relative coordinates. No trigonometric functions are part of this profile: quaternion integration uses the first-order update plus normalization, which needs none.

**Measured behaviour (reference library, this machine).** 1,000,000 integration steps at 120 Hz with random angular velocity up to 8 rad/s per axis keep `| |q|^2 - 1 |` at or below 3 units of 2^-32 (`test_num`, `integrate_drift`). The C reference agrees with an independent Python big-integer model (`tests/num/q32_model.py`) on 2,460,000 random records over 41 operation kinds.

## 2. Types

| Type | Storage | Meaning |
|---|---|---|
| `u32` | one u32 lane | unchanged from `u32_mod_v1` |
| `bool` | register only | unchanged |
| `i32` | one u32 lane, two's complement | wrapping arithmetic |
| `i64` | two u32 lanes (low word first) | wrapping arithmetic |
| `q32` | two u32 lanes (low word first) | signed Q32.32: value = raw / 2^32, raw an i64; saturating arithmetic |
| `v3` | register only | three `q32` (x, y, z) |
| `quat` | register only | four `q32` (w, x, y, z) |

Constants: ONE = 2^32 (raw), HALF = 2^31, MAX = 2^63 - 1, MIN = -2^63.

## 3. Rounding and saturation

**RNE.** Where a result is "RNE(e)", e is the exact rational value of the stated expression over the raw integers, and the result is the nearest integer, ties to the even integer. RNE is symmetric: RNE(-e) = -RNE(e).

**SAT.** Where a `q32` result is "SAT(v)", v is clamped to [MIN, MAX]. A q32 operation that is RNE then SAT rounds first, then clamps.

**Wrap.** Integer (`i32`, `i64`) arithmetic is modulo 2^32 or 2^64 in two's complement.

## 4. Operations

Integer `iN` (N = 32 or 64; MINN and MAXN are the type bounds):

| Op | Result |
|---|---|
| add, sub, mul | wrap(a op b) |
| div | b = 0: 0. a = MINN and b = -1: MINN. Otherwise the quotient truncated toward zero |
| neg, abs | wrap(-a), wrap(\|a\|); so neg(MINN) = abs(MINN) = MINN |
| min, max | the smaller / larger operand |
| clamp(x, lo, hi) | min(max(x, lo), hi); if lo > hi the result is hi |
| eq, lt, le | bool |
| select(c, t, f) | t when c, else f |
| shl(a, n), n: u32 | n >= N: 0. Else wrap(a * 2^n) |
| shr(a, n), n: u32 | n >= N: -1 if a < 0 else 0. Else floor(a / 2^n) (arithmetic shift) |

Fixed `q32` (a, b raw i64):

| Op | Result |
|---|---|
| add, sub | SAT(a ± b) |
| neg, abs | SAT(-a), SAT(\|a\|) |
| mul | SAT(RNE(a * b / 2^32)) |
| div | b = 0: MAX if a > 0, MIN if a < 0, 0 if a = 0. Otherwise SAT(RNE(a * 2^32 / b)) |
| min, max, clamp, eq, lt, le, select | as for integers, on the raw values |
| shl(a, n) | a = 0: 0. n >= 64: MAX if a > 0, MIN if a < 0. Otherwise SAT(a * 2^n) |
| shr(a, n) | as integer shr on the raw value (floor) |
| sqrt(a) | a <= 0: 0. Otherwise RNE(sqrt(a * 2^32)), the correctly rounded square root (a tie cannot occur) |
| rsqrt(a) | a <= 0: MAX. Otherwise div(ONE, sqrt(a)) (two roundings, in that order) |

Conversions:

| Op | Result |
|---|---|
| i32_from_u32, u32_from_i32 | same 32 bits |
| i64_from_i32 | sign extension |
| i32_from_i64 | low 32 bits (wrap) |
| q32_from_i32 | a * 2^32, exact |
| q32_from_i64 | SAT(a * 2^32) |
| q32_to_i32_floor | floor(a / 2^32) (always in range) |
| q32_to_i32_round | RNE(a / 2^32), clamped to MAX32 (the lower bound cannot be exceeded) |
| q32_to_i64_floor | floor(a / 2^32) |
| q32_from_raw, q32_raw | same 64 bits (i64 <-> q32) |
| const q32 (num, den) | RNE(num * 2^32 / den) for an i64 num and a u64 den >= 1. A value outside [MIN, MAX] is an authoring error, rejected by the verifier (no saturation of constants) |

Vectors (`v3`) and quaternions (`quat`, components w, x, y, z). "Exact sum" means the sum of the exact 128-bit products, formed without intermediate rounding:

| Op | Result |
|---|---|
| v3_add, v3_sub, v3_neg | per component q32 add, sub, neg |
| v3_scale(v, s) | per component q32 mul(v_i, s) |
| v3_dot(a, b) | SAT(RNE(exact sum a_i b_i / 2^32)) |
| v3_cross(a, b) | x = SAT(RNE((a_y b_z - a_z b_y) / 2^32)), and cyclically |
| v3_len_sq(a) | v3_dot(a, a) |
| v3_len(a) | min(RNE(sqrt(exact sum a_i^2)), MAX); exact for every representable vector |
| v3_normalize(a) | L = v3_len(a). L = 0: (0, 0, 0). Otherwise per component q32 div(a_i, L) |
| v3_clamp_len(v, m) | m <= 0: (0, 0, 0). L = v3_len(v). L <= m: v unchanged. Otherwise per component SAT(RNE(v_i * m / L)) |
| quat_mul(a, b) | Hamilton product; each component one SAT(RNE(exact sum / 2^32)): w = aw bw - ax bx - ay by - az bz; x = aw bx + ax bw + ay bz - az by; y = aw by - ax bz + ay bw + az bx; z = aw bz + ax by - ay bx + az bw |
| quat_conj(q) | (w, neg x, neg y, neg z) |
| quat_normalize(q) | L = min(RNE(sqrt(exact sum q_i^2)), MAX). L = 0: identity (ONE, 0, 0, 0). Otherwise per component div(q_i, L) |
| quat_rotate(q, v) | u = (qx, qy, qz); t = v3_cross(u, v); t = v3_add(t, t); c = v3_cross(u, t); result_i = add(add(v_i, mul(qw, t_i)), c_i). Equals q v q* for a unit q |
| quat_integrate(q, w, dt) | h = mul(dt, HALF); p = quat_mul(q, (0, w_x, w_y, w_z)); q1_i = add(q_i, mul(p_i, h)); result = quat_normalize(q1). w is the body-frame (local) angular velocity in rad/s |

The left-to-right order of every composite above is normative: an implementation may compute differently only if it produces the same bits for every input.

## 5. Reference implementation and evidence

`src/ir/su_num.c` implements every operation with 64-bit limbs (`U128`, `U192`), sign-and-magnitude accumulation of products, Hacker's Delight 128/64 division and an exact integer square root (normalization, a 64-bit integer Newton estimate, one 128/64 Newton step and an exact fix-up by multiplication; the bit-serial restoring method is kept as `su_num_isqrt_u128_ref` and compared on 952,969 inputs per test run). The method is free: any implementation that returns the specified result is conforming. Tests: `tests/num/test_num.c` (known answers hand-derived from this text, algebraic checks q*d + r = n and r^2 <= n < (r+1)^2 on 200,000 random cases each, symmetry and identity properties, the 1,000,000-step drift band) and `tests/num/q32_model.py` (independent Python model; the C binary prints random records, the model recomputes them with exact rationals).

## Fast paths (2026-10-18)

`src/ir/su_num_fast.h` holds inline functions (`su_q32_mulf`, `su_v3_normalizef`, ...) that return exactly the bits
of the reference functions in `src/ir/su_num.c` for every input; the planned VM tier and the space3d operators use
them. They change only how the result is reached: compiler 128-bit multiplies, signed 128-bit accumulation of up
to four products when every operand lies in [-2^62, 2^62) (otherwise the reference function is called), the
hardware 128/64 divide when the quotient fits 64 bits, and an integer square root that starts from a
double-precision estimate and is then corrected with exact integer comparisons until x^2 <= n < (x+1)^2 (roots of
2^52 and above, where the estimate may be several units off, use the reference method). No result depends on the
floating-point environment. `test_num_fast` compares every function with the reference on edge combinations and
seeded random inputs at every magnitude (15.9 million checks per run); `test_num_fast_portable` repeats it with
`SU_NUM_FAST_PORTABLE`, which disables the compiler intrinsics.

## Cross-build qualification (2026-10-26)

The statement in section 1 that results are identical across x64 compilers was tested end to end
(docs/DETERMINISM_MATRIX.md, issue #141). Five workloads ran through the host API: RTS IR, the 6DOF racer with
space3d.query, a q32 probe workload on saturation-edge operands, and entity churn. They produced byte-identical
manifests of per-keyframe `su_host_digest`, per-tick digest traces and replay bytes on every qualified build and tier
listed there. The builds include GCC and Clang at `-O0` and `-O3`, `-march=native` (FMA), the portable limb path,
and the Windows clang and MSVC legs of CI. The audit found one undefined-behaviour site outside this library, in the
space3d box queries on `INT64_MIN` half extents. It is fixed, with a regression fixture; see that document,
section 5, D1.
