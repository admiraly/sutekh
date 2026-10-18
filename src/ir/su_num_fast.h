/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Inline fast paths of numeric profile fixed_q32_v1 (su_num.h) for the planned VM tier.
 *
 * Every function here returns exactly the bits of the su_num.h function of the same name without the "f"
 * (su_q32_mulf == su_q32_mul, ...) for every input; tests/num/test_num_fast.c checks that on edge and random
 * inputs. su_num.c stays the readable reference; this header only changes how the same results are reached:
 *   - 64x64->128 products use the compiler's 128-bit multiply (unsigned __int128 or _umul128) when present;
 *   - sums of up to four products are accumulated in 128-bit two's complement when every operand is in
 *     [-2^62, 2^62) (the sum then fits); otherwise the reference function is called;
 *   - a 128/64 division whose quotient fits 64 bits uses the hardware divide when available;
 *   - floor(sqrt(n)) starts from a double-precision estimate and is then corrected with exact integer
 *     comparisons until x^2 <= n < (x+1)^2. The estimate only decides how many correction steps run: the
 *     result is the exact integer root whatever the floating-point unit returns, and when the estimate is not
 *     within two steps of the root (roots of 2^52 and above) the reference integer method is used.
 * Integer results are therefore independent of the floating-point environment. Defining SU_NUM_FAST_PORTABLE
 * disables the compiler intrinsics (the portable limb code of su_num.c is used instead); the tests build both. */
#ifndef SU_NUM_FAST_H
#define SU_NUM_FAST_H

#include <math.h>
#include <stdint.h>

#include "ir/su_num.h"

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
#include <intrin.h>
#endif

/* ---- 128-bit helpers ---- */

#if defined(__SIZEOF_INT128__) && !defined(SU_NUM_FAST_PORTABLE)
__extension__ typedef unsigned __int128 su_numf_u128;
#endif

static inline void su_numf_mul_u64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
#if defined(__SIZEOF_INT128__) && !defined(SU_NUM_FAST_PORTABLE)
    su_numf_u128 p = (su_numf_u128)a * b;
    *hi = (uint64_t)(p >> 64);
    *lo = (uint64_t)p;
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(SU_NUM_FAST_PORTABLE)
    *lo = _umul128(a, b, hi);
#else
    su_num_mul_u64(a, b, hi, lo);
#endif
}

/* (hi:lo) / d with hi < d (quotient fits 64 bits); *rem the remainder. */
static inline uint64_t su_numf_div_u128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem) {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) && !defined(SU_NUM_FAST_PORTABLE)
    uint64_t q, r;
    __asm__("divq %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(d) : "cc");
    *rem = r;
    return q;
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64) && _MSC_VER >= 1920 && !defined(SU_NUM_FAST_PORTABLE)
    return _udiv128(hi, lo, d, rem);
#else
    return su_num_divlu(hi, lo, d, rem);
#endif
}

static inline uint64_t su_numf_mag(int64_t a) {
    return a < 0 ? (uint64_t)0 - (uint64_t)a : (uint64_t)a;
}

static inline int64_t su_numf_sat_mag(int neg, uint64_t m) {
    if (!neg) {
        return m > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)m;
    }
    if (m >= (uint64_t)1 << 63) {
        return INT64_MIN;
    }
    return -(int64_t)m;
}

/* 1 when -2^62 <= a < 2^62. */
static inline int su_numf_small(int64_t a) {
    return (uint64_t)a + ((uint64_t)1 << 62) < ((uint64_t)1 << 63);
}

/* RNE(m / 2^32) of a 128-bit magnitude with sign, saturating (the reference acc_round32). */
static inline int64_t su_numf_round32(int neg, uint64_t hi, uint64_t lo) {
    uint64_t q = (hi << 32) | (lo >> 32);
    uint64_t qh = hi >> 32;
    uint64_t frac = lo & 0xFFFFFFFFu;
    if (frac > 0x80000000u || (frac == 0x80000000u && (q & 1u) != 0u)) {
        q++;
        if (q == 0u) {
            qh++;
        }
    }
    if (qh != 0u) {
        return neg ? INT64_MIN : INT64_MAX;
    }
    return su_numf_sat_mag(neg, q);
}

/* Signed 128-bit accumulator (two's complement). Valid while every term's magnitude is below 2^124 and at
 * most four terms are added. */
typedef struct SuNumfAcc {
    uint64_t hi, lo;
} SuNumfAcc;

static inline void su_numf_acc_add(SuNumfAcc *acc, int64_t x, int64_t y, int negate) {
    uint64_t hi, lo;
    su_numf_mul_u64(su_numf_mag(x), su_numf_mag(y), &hi, &lo);
    if (((x < 0) != (y < 0)) != (negate != 0)) {
        lo = ~lo + 1u;
        hi = ~hi + (lo == 0u ? 1u : 0u);
    }
    acc->lo += lo;
    acc->hi += hi + (acc->lo < lo ? 1u : 0u);
}

static inline int64_t su_numf_acc_round(const SuNumfAcc *acc) {
    uint64_t hi = acc->hi, lo = acc->lo;
    int neg = (hi >> 63) != 0u;
    if (neg) {
        lo = ~lo + 1u;
        hi = ~hi + (lo == 0u ? 1u : 0u);
    }
    return su_numf_round32(neg, hi, lo);
}

/* ---- square root ---- */

/* floor(sqrt(hi:lo)) and the remainder n - root^2 (< 2^65: rem_hi is 0 or 1). Returns 0 when the fast
 * estimate is not usable (the caller then uses the reference). */
static inline int su_numf_isqrt_u128(uint64_t hi, uint64_t lo, uint64_t *root, uint64_t *rem_hi, uint64_t *rem_lo) {
    double d = (double)hi * 18446744073709551616.0 + (double)lo;
    double s = sqrt(d);
    uint64_t x, sh, sl;
    int k;
    if (!(s < 4503599627370496.0)) { /* 2^52: the estimate may be several units off */
        return 0;
    }
    x = (uint64_t)s;
    /* Down while x^2 > n, then up while (x+1)^2 <= n; at most two steps each from a 2^-52-accurate estimate. */
    for (k = 0;; k++) {
        su_numf_mul_u64(x, x, &sh, &sl);
        if (sh < hi || (sh == hi && sl <= lo)) {
            break;
        }
        if (k == 2 || x == 0u) {
            return 0;
        }
        x--;
    }
    for (k = 0;; k++) {
        uint64_t y = x + 1u, yh, yl;
        su_numf_mul_u64(y, y, &yh, &yl);
        if (yh > hi || (yh == hi && yl > lo)) {
            break;
        }
        if (k == 2) {
            return 0;
        }
        x = y;
        sh = yh;
        sl = yl;
    }
    *root = x;
    *rem_lo = lo - sl;
    *rem_hi = hi - sh - (lo < sl ? 1u : 0u);
    return 1;
}

/* RNE(sqrt(hi:lo)) saturated to INT64_MAX (the reference sqrt_sum_sat over a 128-bit sum). */
static inline int64_t su_numf_sqrt_sat(uint64_t hi, uint64_t lo) {
    uint64_t r, rh, rl;
    int of = 0;
    if (su_numf_isqrt_u128(hi, lo, &r, &rh, &rl)) {
        /* round up when n - r^2 > r; r < 2^52 here, so r + 1 cannot overflow */
        if (rh != 0u || rl > r) {
            r++;
        }
    } else {
        r = su_num_sqrt_rne_u128(hi, lo, &of);
    }
    if (of || r > (uint64_t)INT64_MAX) {
        return INT64_MAX;
    }
    return (int64_t)r;
}

/* ---- q32 scalars ---- */

static inline int64_t su_q32_addf(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b) {
        return INT64_MAX;
    }
    if (b < 0 && a < INT64_MIN - b) {
        return INT64_MIN;
    }
    return a + b;
}

static inline int64_t su_q32_subf(int64_t a, int64_t b) {
    if (b < 0 && a > INT64_MAX + b) {
        return INT64_MAX;
    }
    if (b > 0 && a < INT64_MIN + b) {
        return INT64_MIN;
    }
    return a - b;
}

static inline int64_t su_q32_mulf(int64_t a, int64_t b) {
    uint64_t hi, lo;
    su_numf_mul_u64(su_numf_mag(a), su_numf_mag(b), &hi, &lo);
    return su_numf_round32((a < 0) != (b < 0), hi, lo);
}

/* RNE((hi:lo) / d) with sign, saturating; the reference div_rne_u128 + sat_from_mag128. */
static inline int64_t su_numf_div_rne(int neg, uint64_t hi, uint64_t lo, uint64_t d) {
    uint64_t q, rem;
    if (hi >= d) {
        return neg ? INT64_MIN : INT64_MAX; /* quotient >= 2^64 */
    }
    q = su_numf_div_u128(hi, lo, d, &rem);
    if (rem > d - rem || (rem == d - rem && (q & 1u) != 0u)) {
        q++;
        if (q == 0u) {
            return neg ? INT64_MIN : INT64_MAX;
        }
    }
    return su_numf_sat_mag(neg, q);
}

static inline int64_t su_q32_divf(int64_t a, int64_t b) {
    uint64_t ma, mb;
    if (b == 0) {
        return a > 0 ? INT64_MAX : (a < 0 ? INT64_MIN : 0);
    }
    ma = su_numf_mag(a);
    mb = su_numf_mag(b);
    return su_numf_div_rne((a < 0) != (b < 0), ma >> 32, ma << 32, mb);
}

static inline int64_t su_q32_sqrtf(int64_t a) {
    uint64_t u;
    if (a <= 0) {
        return 0;
    }
    u = (uint64_t)a;
    return su_numf_sqrt_sat(u >> 32, u << 32); /* root < 2^48: never saturates */
}

static inline int64_t su_q32_rsqrtf(int64_t a) {
    if (a <= 0) {
        return INT64_MAX;
    }
    return su_q32_divf(SU_Q32_ONE, su_q32_sqrtf(a));
}

/* ---- vec3 ---- */

static inline SuV3 su_v3_addf(SuV3 a, SuV3 b) {
    SuV3 r;
    r.x = su_q32_addf(a.x, b.x);
    r.y = su_q32_addf(a.y, b.y);
    r.z = su_q32_addf(a.z, b.z);
    return r;
}

static inline SuV3 su_v3_subf(SuV3 a, SuV3 b) {
    SuV3 r;
    r.x = su_q32_subf(a.x, b.x);
    r.y = su_q32_subf(a.y, b.y);
    r.z = su_q32_subf(a.z, b.z);
    return r;
}

static inline SuV3 su_v3_scalef(SuV3 a, int64_t s) {
    SuV3 r;
    r.x = su_q32_mulf(a.x, s);
    r.y = su_q32_mulf(a.y, s);
    r.z = su_q32_mulf(a.z, s);
    return r;
}

static inline int su_numf_small3(SuV3 a) {
    return su_numf_small(a.x) && su_numf_small(a.y) && su_numf_small(a.z);
}

static inline int64_t su_v3_dotf(SuV3 a, SuV3 b) {
    SuNumfAcc acc = {0, 0};
    if (!su_numf_small3(a) || !su_numf_small3(b)) {
        return su_v3_dot(a, b);
    }
    su_numf_acc_add(&acc, a.x, b.x, 0);
    su_numf_acc_add(&acc, a.y, b.y, 0);
    su_numf_acc_add(&acc, a.z, b.z, 0);
    return su_numf_acc_round(&acc);
}

static inline SuV3 su_v3_crossf(SuV3 a, SuV3 b) {
    SuV3 r;
    SuNumfAcc acc;
    if (!su_numf_small3(a) || !su_numf_small3(b)) {
        return su_v3_cross(a, b);
    }
    acc.hi = acc.lo = 0;
    su_numf_acc_add(&acc, a.y, b.z, 0);
    su_numf_acc_add(&acc, a.z, b.y, 1);
    r.x = su_numf_acc_round(&acc);
    acc.hi = acc.lo = 0;
    su_numf_acc_add(&acc, a.z, b.x, 0);
    su_numf_acc_add(&acc, a.x, b.z, 1);
    r.y = su_numf_acc_round(&acc);
    acc.hi = acc.lo = 0;
    su_numf_acc_add(&acc, a.x, b.y, 0);
    su_numf_acc_add(&acc, a.y, b.x, 1);
    r.z = su_numf_acc_round(&acc);
    return r;
}

static inline int64_t su_v3_len_sqf(SuV3 a) {
    return su_v3_dotf(a, a);
}

/* Exact sum of the squares of n (3 or 4) small components into hi:lo. */
static inline void su_numf_sumsq(const int64_t *c, int n, uint64_t *hi, uint64_t *lo) {
    int i;
    *hi = 0;
    *lo = 0;
    for (i = 0; i < n; i++) {
        uint64_t m = su_numf_mag(c[i]), ph, pl;
        su_numf_mul_u64(m, m, &ph, &pl);
        *lo += pl;
        *hi += ph + (*lo < pl ? 1u : 0u);
    }
}

static inline int64_t su_v3_lenf(SuV3 a) {
    int64_t c[3];
    uint64_t hi, lo;
    if (!su_numf_small3(a)) {
        return su_v3_len(a);
    }
    c[0] = a.x;
    c[1] = a.y;
    c[2] = a.z;
    su_numf_sumsq(c, 3, &hi, &lo);
    return su_numf_sqrt_sat(hi, lo);
}

static inline SuV3 su_v3_normalizef(SuV3 a) {
    SuV3 r;
    int64_t len = su_v3_lenf(a);
    if (len == 0) {
        r.x = r.y = r.z = 0;
        return r;
    }
    r.x = su_q32_divf(a.x, len);
    r.y = su_q32_divf(a.y, len);
    r.z = su_q32_divf(a.z, len);
    return r;
}

static inline int64_t su_numf_scale_ratio(int64_t c, int64_t m, int64_t len) {
    uint64_t hi, lo;
    su_numf_mul_u64(su_numf_mag(c), (uint64_t)m, &hi, &lo);
    return su_numf_div_rne(c < 0, hi, lo, (uint64_t)len);
}

static inline SuV3 su_v3_clamp_lenf(SuV3 a, int64_t max_len) {
    SuV3 r;
    int64_t len;
    if (max_len <= 0) {
        r.x = r.y = r.z = 0;
        return r;
    }
    len = su_v3_lenf(a);
    if (len <= max_len) {
        return a;
    }
    r.x = su_numf_scale_ratio(a.x, max_len, len);
    r.y = su_numf_scale_ratio(a.y, max_len, len);
    r.z = su_numf_scale_ratio(a.z, max_len, len);
    return r;
}

/* ---- quaternion ---- */

static inline int su_numf_smallq(SuQuat a) {
    return su_numf_small(a.w) && su_numf_small(a.x) && su_numf_small(a.y) && su_numf_small(a.z);
}

static inline SuQuat su_quat_mulf(SuQuat a, SuQuat b) {
    SuQuat r;
    SuNumfAcc acc;
    if (!su_numf_smallq(a) || !su_numf_smallq(b)) {
        return su_quat_mul(a, b);
    }
    acc.hi = acc.lo = 0;
    su_numf_acc_add(&acc, a.w, b.w, 0);
    su_numf_acc_add(&acc, a.x, b.x, 1);
    su_numf_acc_add(&acc, a.y, b.y, 1);
    su_numf_acc_add(&acc, a.z, b.z, 1);
    r.w = su_numf_acc_round(&acc);
    acc.hi = acc.lo = 0;
    su_numf_acc_add(&acc, a.w, b.x, 0);
    su_numf_acc_add(&acc, a.x, b.w, 0);
    su_numf_acc_add(&acc, a.y, b.z, 0);
    su_numf_acc_add(&acc, a.z, b.y, 1);
    r.x = su_numf_acc_round(&acc);
    acc.hi = acc.lo = 0;
    su_numf_acc_add(&acc, a.w, b.y, 0);
    su_numf_acc_add(&acc, a.x, b.z, 1);
    su_numf_acc_add(&acc, a.y, b.w, 0);
    su_numf_acc_add(&acc, a.z, b.x, 0);
    r.y = su_numf_acc_round(&acc);
    acc.hi = acc.lo = 0;
    su_numf_acc_add(&acc, a.w, b.z, 0);
    su_numf_acc_add(&acc, a.x, b.y, 0);
    su_numf_acc_add(&acc, a.y, b.x, 1);
    su_numf_acc_add(&acc, a.z, b.w, 0);
    r.z = su_numf_acc_round(&acc);
    return r;
}

static inline SuQuat su_quat_conjf(SuQuat a) {
    SuQuat r;
    r.w = a.w;
    r.x = su_q32_neg(a.x);
    r.y = su_q32_neg(a.y);
    r.z = su_q32_neg(a.z);
    return r;
}

static inline SuQuat su_quat_normalizef(SuQuat a) {
    SuQuat r;
    int64_t c[4], len;
    uint64_t hi, lo;
    if (!su_numf_smallq(a)) {
        return su_quat_normalize(a);
    }
    c[0] = a.w;
    c[1] = a.x;
    c[2] = a.y;
    c[3] = a.z;
    su_numf_sumsq(c, 4, &hi, &lo);
    len = su_numf_sqrt_sat(hi, lo);
    if (len == 0) {
        r.w = SU_Q32_ONE;
        r.x = r.y = r.z = 0;
        return r;
    }
    r.w = su_q32_divf(a.w, len);
    r.x = su_q32_divf(a.x, len);
    r.y = su_q32_divf(a.y, len);
    r.z = su_q32_divf(a.z, len);
    return r;
}

static inline SuV3 su_quat_rotatef(SuQuat q, SuV3 v) {
    SuV3 u, t, c, r;
    u.x = q.x;
    u.y = q.y;
    u.z = q.z;
    t = su_v3_crossf(u, v);
    t = su_v3_addf(t, t);
    c = su_v3_crossf(u, t);
    r.x = su_q32_addf(su_q32_addf(v.x, su_q32_mulf(q.w, t.x)), c.x);
    r.y = su_q32_addf(su_q32_addf(v.y, su_q32_mulf(q.w, t.y)), c.y);
    r.z = su_q32_addf(su_q32_addf(v.z, su_q32_mulf(q.w, t.z)), c.z);
    return r;
}

static inline SuQuat su_quat_integratef(SuQuat q, SuV3 w_local, int64_t dt) {
    SuQuat om, p, q1;
    int64_t h = su_q32_mulf(dt, SU_Q32_HALF);
    om.w = 0;
    om.x = w_local.x;
    om.y = w_local.y;
    om.z = w_local.z;
    p = su_quat_mulf(q, om);
    q1.w = su_q32_addf(q.w, su_q32_mulf(p.w, h));
    q1.x = su_q32_addf(q.x, su_q32_mulf(p.x, h));
    q1.y = su_q32_addf(q.y, su_q32_mulf(p.y, h));
    q1.z = su_q32_addf(q.z, su_q32_mulf(p.z, h));
    return su_quat_normalizef(q1);
}

#endif
