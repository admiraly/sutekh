/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Numeric profile fixed_q32_v1 (IR 0.3): exact reference semantics of the signed integer, Q32.32
 * fixed-point, vec3 and quaternion operations. The normative text is docs/NUMERIC_FIXED_Q32.md; this
 * file is its reference implementation and is shared by the verifier (constant rounding), the VM and the
 * space3d host operators.
 *
 * Integer only: no floating point, no compiler-specific 128-bit type, no signed overflow, no
 * implementation-defined conversion of out-of-range values. Every result is a pure function of the
 * operand bits, so any conforming C17 compiler on any target yields the same bits.
 *
 * Representations:
 *   i32   int32_t, two's complement; arithmetic wraps modulo 2^32
 *   i64   int64_t, two's complement; arithmetic wraps modulo 2^64
 *   q32   int64_t raw; value = raw / 2^32 (Q32.32); arithmetic saturates to [INT64_MIN, INT64_MAX]
 *   v3    three q32 (x, y, z)
 *   quat  four q32 (w, x, y, z)
 * Rounding: every q32 result that is not exact is rounded to nearest, ties to even (RNE), from the exact
 * mathematical value of the stated expression. Sums of products are formed exactly and rounded once. */
#ifndef SU_NUM_H
#define SU_NUM_H

#include <stdint.h>

#define SU_Q32_FRAC_BITS 32u
#define SU_Q32_ONE INT64_C(0x100000000)
#define SU_Q32_HALF INT64_C(0x80000000)

typedef struct SuV3 {
    int64_t x, y, z;
} SuV3;

typedef struct SuQuat {
    int64_t w, x, y, z;
} SuQuat;

/* ---- bit-exact conversions ---- */
int32_t su_i32_from_bits(uint32_t u);
int64_t su_i64_from_bits(uint64_t u);

/* ---- i32 (wrapping) ---- */
int32_t su_i32_add(int32_t a, int32_t b);
int32_t su_i32_sub(int32_t a, int32_t b);
int32_t su_i32_mul(int32_t a, int32_t b);
int32_t su_i32_div(int32_t a, int32_t b);   /* truncates toward zero; b == 0 -> 0; MIN / -1 -> MIN */
int32_t su_i32_neg(int32_t a);              /* -MIN -> MIN */
int32_t su_i32_abs(int32_t a);              /* abs(MIN) -> MIN */
int32_t su_i32_shl(int32_t a, uint32_t n);  /* n >= 32 -> 0 */
int32_t su_i32_shr(int32_t a, uint32_t n);  /* arithmetic (floor); n >= 32 -> 0 or -1 */

/* ---- i64 (wrapping) ---- */
int64_t su_i64_add(int64_t a, int64_t b);
int64_t su_i64_sub(int64_t a, int64_t b);
int64_t su_i64_mul(int64_t a, int64_t b);
int64_t su_i64_div(int64_t a, int64_t b);
int64_t su_i64_neg(int64_t a);
int64_t su_i64_abs(int64_t a);
int64_t su_i64_shl(int64_t a, uint32_t n);  /* n >= 64 -> 0 */
int64_t su_i64_shr(int64_t a, uint32_t n);  /* n >= 64 -> 0 or -1 */

/* ---- q32 (saturating, RNE) ---- */
int64_t su_q32_add(int64_t a, int64_t b);
int64_t su_q32_sub(int64_t a, int64_t b);
int64_t su_q32_neg(int64_t a);              /* -MIN -> MAX */
int64_t su_q32_abs(int64_t a);              /* abs(MIN) -> MAX */
int64_t su_q32_mul(int64_t a, int64_t b);   /* RNE(a*b / 2^32) */
int64_t su_q32_div(int64_t a, int64_t b);   /* RNE(a*2^32 / b); b == 0 -> MAX, MIN or 0 by the sign of a */
int64_t su_q32_shl(int64_t a, uint32_t n);  /* saturating a * 2^n */
int64_t su_q32_shr(int64_t a, uint32_t n);  /* floor(a / 2^n) on the raw value */
int64_t su_q32_sqrt(int64_t a);             /* RNE(sqrt(a)); a <= 0 -> 0 */
int64_t su_q32_rsqrt(int64_t a);            /* su_q32_div(ONE, su_q32_sqrt(a)); a <= 0 -> MAX */

/* ---- conversions ---- */
int64_t su_q32_from_i32(int32_t a);         /* exact */
int64_t su_q32_from_i64(int64_t a);         /* saturating a * 2^32 */
int32_t su_q32_to_i32_floor(int64_t a);     /* floor; always representable */
int32_t su_q32_to_i32_round(int64_t a);     /* RNE to an integer, saturating to the i32 range */
int64_t su_q32_to_i64_floor(int64_t a);     /* floor; always representable */
/* Exact RNE(num * 2^32 / den). Returns 1 and sets *out, or 0 if den == 0 or the result does not fit
 * (no saturation: a constant that does not fit is an authoring error). */
int su_q32_from_ratio(int64_t num, uint64_t den, int64_t *out);

/* ---- vec3 ---- */
SuV3 su_v3_add(SuV3 a, SuV3 b);
SuV3 su_v3_sub(SuV3 a, SuV3 b);
SuV3 su_v3_neg(SuV3 a);
SuV3 su_v3_scale(SuV3 a, int64_t s);        /* per component su_q32_mul */
int64_t su_v3_dot(SuV3 a, SuV3 b);          /* RNE(exact sum of products / 2^32), saturating */
SuV3 su_v3_cross(SuV3 a, SuV3 b);           /* each component one RNE of the exact difference */
int64_t su_v3_len_sq(SuV3 a);               /* su_v3_dot(a, a) */
int64_t su_v3_len(SuV3 a);                  /* RNE(sqrt(exact sum of raw squares)), saturating */
SuV3 su_v3_normalize(SuV3 a);               /* len 0 -> zero vector; else su_q32_div(c, len) */
SuV3 su_v3_clamp_len(SuV3 a, int64_t max_len); /* see docs/NUMERIC_FIXED_Q32.md */

/* ---- quaternion (w, x, y, z) ---- */
SuQuat su_quat_mul(SuQuat a, SuQuat b);     /* Hamilton product, one RNE per component */
SuQuat su_quat_conj(SuQuat a);
SuQuat su_quat_normalize(SuQuat a);         /* len 0 -> identity */
SuV3 su_quat_rotate(SuQuat q, SuV3 v);      /* v + w*t + cross(u, t), t = 2*cross(u, v) */
SuQuat su_quat_integrate(SuQuat q, SuV3 w_local, int64_t dt); /* normalize(q + (q * (0,w)) * dt/2) */

/* ---- exact helpers exposed for tests and the space3d operators ---- */
/* floor(sqrt(n)) of the 128-bit value hi:lo, and the RNE variant. Results fit 64 bits. */
uint64_t su_num_isqrt_u128(uint64_t hi, uint64_t lo);
uint64_t su_num_sqrt_rne_u128(uint64_t hi, uint64_t lo, int *overflow); /* *overflow = 1 when the RNE result is 2^64 */
/* Unsigned (hi:lo) / d with hi < d required; returns the quotient, *rem the remainder. d != 0. */
uint64_t su_num_divlu(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem);
/* 64x64 -> 128 unsigned product. */
void su_num_mul_u64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo);

#endif
