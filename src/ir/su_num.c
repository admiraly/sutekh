/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Reference implementation of numeric profile fixed_q32_v1. See su_num.h and docs/NUMERIC_FIXED_Q32.md.
 *
 * Wide intermediates use explicit 64-bit limbs (U128, U192) so that MSVC, clang and gcc build the same
 * code. Signed values are handled as sign and magnitude: the magnitude of INT64_MIN is 2^63, which fits a
 * uint64_t. Sums of signed products are accumulated as two unsigned sums (positive and negative terms)
 * and subtracted once, which keeps every step free of signed overflow. */
#include "ir/su_num.h"

#include <stdint.h>

/* ---- limb helpers ---- */

typedef struct U128 {
    uint64_t hi, lo;
} U128;

typedef struct U192 {
    uint64_t w[3]; /* w[0] least significant */
} U192;

void su_num_mul_u64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32;
    uint64_t b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    *lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static int u128_lt(U128 a, U128 b) {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

static U128 u128_sub(U128 a, U128 b) {
    U128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo ? 1u : 0u);
    return r;
}

static U192 u192_add(U192 a, U192 b) {
    U192 r;
    uint64_t c = 0;
    int i;
    for (i = 0; i < 3; i++) {
        uint64_t s = a.w[i] + b.w[i];
        uint64_t c1 = s < a.w[i] ? 1u : 0u;
        uint64_t s2 = s + c;
        uint64_t c2 = s2 < s ? 1u : 0u;
        r.w[i] = s2;
        c = c1 + c2;
    }
    return r;
}

/* a - b; requires a >= b. */
static U192 u192_sub(U192 a, U192 b) {
    U192 r;
    uint64_t borrow = 0;
    int i;
    for (i = 0; i < 3; i++) {
        uint64_t d = a.w[i] - b.w[i];
        uint64_t b1 = a.w[i] < b.w[i] ? 1u : 0u;
        uint64_t d2 = d - borrow;
        uint64_t b2 = d < borrow ? 1u : 0u;
        r.w[i] = d2;
        borrow = b1 + b2;
    }
    return r;
}

static int u192_cmp(U192 a, U192 b) {
    int i;
    for (i = 2; i >= 0; i--) {
        if (a.w[i] != b.w[i]) {
            return a.w[i] < b.w[i] ? -1 : 1;
        }
    }
    return 0;
}

static U192 u192_from_u64_product(uint64_t a, uint64_t b) {
    U192 r;
    su_num_mul_u64(a, b, &r.w[1], &r.w[0]);
    r.w[2] = 0;
    return r;
}

/* Magnitude of a signed 64-bit value (2^63 for INT64_MIN). */
static uint64_t mag64(int64_t a) {
    return a < 0 ? (uint64_t)0 - (uint64_t)a : (uint64_t)a;
}

/* Signed value from sign and magnitude, saturating. neg != 0 means negative. */
static int64_t sat_from_mag(int neg, uint64_t m) {
    if (!neg) {
        return m > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)m;
    }
    if (m >= (uint64_t)1 << 63) {
        return INT64_MIN;
    }
    return -(int64_t)m;
}

/* Same with a 128-bit magnitude. */
static int64_t sat_from_mag128(int neg, U128 m) {
    if (m.hi != 0u) {
        return neg ? INT64_MIN : INT64_MAX;
    }
    return sat_from_mag(neg, m.lo);
}

int32_t su_i32_from_bits(uint32_t u) {
    if (u <= (uint32_t)INT32_MAX) {
        return (int32_t)u;
    }
    return -(int32_t)(~u) - 1;
}

int64_t su_i64_from_bits(uint64_t u) {
    if (u <= (uint64_t)INT64_MAX) {
        return (int64_t)u;
    }
    return -(int64_t)(~u) - 1;
}

/* ---- 128 / 64 division (Hacker's Delight divlu, 64-bit limbs split into 32-bit digits) ---- */

static unsigned nlz64(uint64_t x) {
    unsigned n = 0;
    if (x == 0u) {
        return 64u;
    }
    if (x <= UINT64_C(0x00000000FFFFFFFF)) {
        n += 32u;
        x <<= 32;
    }
    if (x <= UINT64_C(0x0000FFFFFFFFFFFF)) {
        n += 16u;
        x <<= 16;
    }
    if (x <= UINT64_C(0x00FFFFFFFFFFFFFF)) {
        n += 8u;
        x <<= 8;
    }
    if (x <= UINT64_C(0x0FFFFFFFFFFFFFFF)) {
        n += 4u;
        x <<= 4;
    }
    if (x <= UINT64_C(0x3FFFFFFFFFFFFFFF)) {
        n += 2u;
        x <<= 2;
    }
    if (x <= UINT64_C(0x7FFFFFFFFFFFFFFF)) {
        n += 1u;
    }
    return n;
}

uint64_t su_num_divlu(uint64_t u1, uint64_t u0, uint64_t v, uint64_t *rem) {
    const uint64_t b = (uint64_t)1 << 32;
    uint64_t un1, un0, vn1, vn0, q1, q0, un32, un21, un10, rhat;
    unsigned s = nlz64(v);
    v <<= s;
    vn1 = v >> 32;
    vn0 = v & 0xFFFFFFFFu;
    un32 = s == 0u ? u1 : (u1 << s) | (u0 >> (64u - s));
    un10 = u0 << s;
    un1 = un10 >> 32;
    un0 = un10 & 0xFFFFFFFFu;

    q1 = un32 / vn1;
    rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > b * rhat + un1) {
        q1--;
        rhat += vn1;
        if (rhat >= b) {
            break;
        }
    }
    un21 = un32 * b + un1 - q1 * v;

    q0 = un21 / vn1;
    rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) {
        q0--;
        rhat += vn1;
        if (rhat >= b) {
            break;
        }
    }
    if (rem != NULL) {
        *rem = (un21 * b + un0 - q0 * v) >> s;
    }
    return q1 * b + q0;
}

/* RNE(n / d) for a 128-bit n and a nonzero 64-bit d; the quotient is 128-bit. */
static U128 div_rne_u128(U128 n, uint64_t d) {
    U128 q;
    uint64_t r1, rem;
    q.hi = n.hi / d;
    r1 = n.hi % d;
    q.lo = su_num_divlu(r1, n.lo, d, &rem);
    if (rem > d - rem || (rem == d - rem && (q.lo & 1u) != 0u)) {
        q.lo++;
        if (q.lo == 0u) {
            q.hi++;
        }
    }
    return q;
}

/* ---- square root ---- */

/* floor(sqrt(t)) of a 64-bit value: integer Newton from 2^ceil(bits/2) >= sqrt(t), which decreases to the
 * root and stops when a step no longer decreases (x <= 2^32 and t / x < 2^63, so the sum cannot overflow). */
static uint64_t isqrt64(uint64_t t) {
    uint64_t x, y;
    if (t < 2u) {
        return t;
    }
    x = (uint64_t)1 << ((65u - nlz64(t)) / 2u);
    for (;;) {
        y = (x + t / x) >> 1;
        if (y >= x) {
            return x;
        }
        x = y;
    }
}

/* floor(sqrt(hi:lo)) and the remainder n - root^2 (at most 2 * root, so it fits 65 bits: U128).
 *
 * The result is the exact integer square root; only the method is chosen for speed. The value is normalized
 * (m = n * 4^k with the top word at least 2^62), the root of the top word gives a 32-bit-accurate estimate
 * R = isqrt64(top) * 2^32 <= sqrt(m), and integer Newton steps x' = floor((x + floor(m / x)) / 2) finish it: the
 * first step from any x >= 1 gives x' >= isqrt(m) (integer AM-GM), and from above the sequence decreases to
 * isqrt(m), stopping when it no longer decreases. isqrt(n) = floor(isqrt(m) / 2^k). The bit-serial reference
 * (su_num_isqrt_u128_ref) is kept for the tests. */
static uint64_t isqrt_rem_u128(uint64_t hi, uint64_t lo, U128 *rem_out) {
    uint64_t root, mh, ml, x, q, r0, rem;
    unsigned s;
    U128 sq, n;
    if (hi == 0u) {
        root = isqrt64(lo);
    } else {
        s = nlz64(hi) & ~1u; /* even shift: m = n * 4^(s/2) */
        mh = s == 0u ? hi : (hi << s) | (lo >> (64u - s));
        ml = lo << s;
        if (mh == UINT64_MAX) {
            x = UINT64_MAX; /* m >= (2^64 - 1) * 2^64: isqrt(m) = 2^64 - 1 */
        } else {
            r0 = isqrt64(mh) << 32; /* <= sqrt(m), relative error below 2^-31 */
            if (mh >= r0) {
                x = UINT64_MAX; /* m / r0 would not fit 64 bits; start from above instead */
            } else {
                uint64_t sh, sl;
                q = su_num_divlu(mh, ml, r0, &rem);
                x = (r0 >> 1) + (q >> 1) + (r0 & q & 1u);
                /* x >= isqrt(m) and, from a 2^-31-accurate start, x - sqrt(m) < 5: decrement while x^2 > m. */
                for (;;) {
                    su_num_mul_u64(x, x, &sh, &sl);
                    if (sh < mh || (sh == mh && sl <= ml)) {
                        break;
                    }
                    x--;
                }
            }
            for (;;) {
                uint64_t y;
                if (mh >= x) {
                    break; /* m / x >= 2^64 > x: the next step cannot decrease */
                }
                q = su_num_divlu(mh, ml, x, &rem);
                y = (x >> 1) + (q >> 1) + (x & q & 1u);
                if (y >= x) {
                    break;
                }
                x = y;
            }
        }
        root = x >> (s / 2u);
    }
    if (rem_out != NULL) {
        n.hi = hi;
        n.lo = lo;
        su_num_mul_u64(root, root, &sq.hi, &sq.lo);
        *rem_out = u128_sub(n, sq);
    }
    return root;
}

/* Bit-serial reference of isqrt_rem_u128 (restoring method over 128 bits). */
static uint64_t isqrt_rem_u128_ref(uint64_t hi, uint64_t lo, U128 *rem_out) {
    uint64_t root = 0;
    U128 rem = {0, 0};
    int i;
    int top = 63;
    /* Skip leading zero bit pairs (pair i holds bits 2i+1 and 2i of the 128-bit value). */
    while (top >= 0) {
        uint64_t pair = top >= 32 ? (hi >> (2 * (top - 32))) & 3u : (lo >> (2 * top)) & 3u;
        if (pair != 0u) {
            break;
        }
        top--;
    }
    for (i = top; i >= 0; i--) {
        uint64_t pair = i >= 32 ? (hi >> (2 * (i - 32))) & 3u : (lo >> (2 * i)) & 3u;
        U128 trial;
        /* rem = rem * 4 + pair; rem < 2^66 here, so the shift cannot lose bits. */
        rem.hi = (rem.hi << 2) | (rem.lo >> 62);
        rem.lo = (rem.lo << 2) | pair;
        /* trial = 4 * root + 1 (root < 2^63 while bits remain, so trial < 2^65). */
        trial.hi = root >> 62;
        trial.lo = (root << 2) | 1u;
        root <<= 1;
        if (!u128_lt(rem, trial)) {
            rem = u128_sub(rem, trial);
            root |= 1u;
        }
    }
    if (rem_out != NULL) {
        *rem_out = rem;
    }
    return root;
}

uint64_t su_num_isqrt_u128(uint64_t hi, uint64_t lo) {
    return isqrt_rem_u128(hi, lo, NULL);
}

uint64_t su_num_isqrt_u128_ref(uint64_t hi, uint64_t lo) {
    return isqrt_rem_u128_ref(hi, lo, NULL);
}

/* RNE(sqrt(n)): round up when n - r^2 > r. A tie is impossible: (r + 1/2)^2 is not an integer. */
uint64_t su_num_sqrt_rne_u128(uint64_t hi, uint64_t lo, int *overflow) {
    U128 rem;
    uint64_t r = isqrt_rem_u128(hi, lo, &rem);
    U128 rr;
    rr.hi = 0;
    rr.lo = r;
    if (overflow != NULL) {
        *overflow = 0;
    }
    if (u128_lt(rr, rem)) {
        if (r == UINT64_MAX) {
            if (overflow != NULL) {
                *overflow = 1;
            }
            return r;
        }
        r++;
    }
    return r;
}

/* ---- signed sum-of-products accumulator ---- */

typedef struct Acc {
    U192 pos, neg;
} Acc;

static void acc_init(Acc *a) {
    a->pos.w[0] = a->pos.w[1] = a->pos.w[2] = 0;
    a->neg = a->pos;
}

/* acc += sign * x * y, sign = +1 when negate == 0, -1 otherwise. */
static void acc_add(Acc *acc, int64_t x, int64_t y, int negate) {
    U192 p = u192_from_u64_product(mag64(x), mag64(y));
    int neg = ((x < 0) != (y < 0)) != (negate != 0);
    if (neg) {
        acc->neg = u192_add(acc->neg, p);
    } else {
        acc->pos = u192_add(acc->pos, p);
    }
}

/* Signed exact value of the accumulator as sign and magnitude. */
static U192 acc_value(const Acc *acc, int *neg) {
    if (u192_cmp(acc->pos, acc->neg) >= 0) {
        *neg = 0;
        return u192_sub(acc->pos, acc->neg);
    }
    *neg = 1;
    return u192_sub(acc->neg, acc->pos);
}

/* RNE(acc / 2^32), saturating. */
static int64_t acc_round32(const Acc *acc) {
    int neg;
    U192 m = acc_value(acc, &neg);
    uint64_t frac = m.w[0] & 0xFFFFFFFFu;
    U128 q;
    q.lo = (m.w[0] >> 32) | (m.w[1] << 32);
    q.hi = (m.w[1] >> 32) | (m.w[2] << 32);
    if ((m.w[2] >> 32) != 0u) {
        return neg ? INT64_MIN : INT64_MAX;
    }
    if (frac > 0x80000000u || (frac == 0x80000000u && (q.lo & 1u) != 0u)) {
        q.lo++;
        if (q.lo == 0u) {
            q.hi++;
        }
    }
    return sat_from_mag128(neg, q);
}

/* RNE(sqrt(sum)) of an unsigned 192-bit sum of squares, saturating to INT64_MAX. */
static int64_t sqrt_sum_sat(U192 s) {
    int of = 0;
    uint64_t r;
    if (s.w[2] != 0u) {
        return INT64_MAX; /* sqrt >= 2^64 */
    }
    r = su_num_sqrt_rne_u128(s.w[1], s.w[0], &of);
    if (of || r > (uint64_t)INT64_MAX) {
        return INT64_MAX;
    }
    return (int64_t)r;
}

/* ---- i32 ---- */

int32_t su_i32_add(int32_t a, int32_t b) {
    return su_i32_from_bits((uint32_t)a + (uint32_t)b);
}

int32_t su_i32_sub(int32_t a, int32_t b) {
    return su_i32_from_bits((uint32_t)a - (uint32_t)b);
}

int32_t su_i32_mul(int32_t a, int32_t b) {
    return su_i32_from_bits((uint32_t)((uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b));
}

int32_t su_i32_div(int32_t a, int32_t b) {
    if (b == 0) {
        return 0;
    }
    if (a == INT32_MIN && b == -1) {
        return INT32_MIN;
    }
    return a / b;
}

int32_t su_i32_neg(int32_t a) {
    return su_i32_from_bits((uint32_t)0 - (uint32_t)a);
}

int32_t su_i32_abs(int32_t a) {
    return a < 0 ? su_i32_neg(a) : a;
}

int32_t su_i32_shl(int32_t a, uint32_t n) {
    if (n >= 32u) {
        return 0;
    }
    return su_i32_from_bits((uint32_t)a << n);
}

int32_t su_i32_shr(int32_t a, uint32_t n) {
    if (n >= 32u) {
        return a < 0 ? -1 : 0;
    }
    return a >= 0 ? a >> n : ~((~a) >> n);
}

/* ---- i64 ---- */

int64_t su_i64_add(int64_t a, int64_t b) {
    return su_i64_from_bits((uint64_t)a + (uint64_t)b);
}

int64_t su_i64_sub(int64_t a, int64_t b) {
    return su_i64_from_bits((uint64_t)a - (uint64_t)b);
}

int64_t su_i64_mul(int64_t a, int64_t b) {
    return su_i64_from_bits((uint64_t)a * (uint64_t)b);
}

int64_t su_i64_div(int64_t a, int64_t b) {
    if (b == 0) {
        return 0;
    }
    if (a == INT64_MIN && b == -1) {
        return INT64_MIN;
    }
    return a / b;
}

int64_t su_i64_neg(int64_t a) {
    return su_i64_from_bits((uint64_t)0 - (uint64_t)a);
}

int64_t su_i64_abs(int64_t a) {
    return a < 0 ? su_i64_neg(a) : a;
}

int64_t su_i64_shl(int64_t a, uint32_t n) {
    if (n >= 64u) {
        return 0;
    }
    return su_i64_from_bits((uint64_t)a << n);
}

int64_t su_i64_shr(int64_t a, uint32_t n) {
    if (n >= 64u) {
        return a < 0 ? -1 : 0;
    }
    return a >= 0 ? a >> n : ~((~a) >> n);
}

/* ---- q32 ---- */

int64_t su_q32_add(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b) {
        return INT64_MAX;
    }
    if (b < 0 && a < INT64_MIN - b) {
        return INT64_MIN;
    }
    return a + b;
}

int64_t su_q32_sub(int64_t a, int64_t b) {
    if (b < 0 && a > INT64_MAX + b) {
        return INT64_MAX;
    }
    if (b > 0 && a < INT64_MIN + b) {
        return INT64_MIN;
    }
    return a - b;
}

int64_t su_q32_neg(int64_t a) {
    return a == INT64_MIN ? INT64_MAX : -a;
}

int64_t su_q32_abs(int64_t a) {
    return a < 0 ? su_q32_neg(a) : a;
}

int64_t su_q32_mul(int64_t a, int64_t b) {
    Acc acc;
    acc_init(&acc);
    acc_add(&acc, a, b, 0);
    return acc_round32(&acc);
}

int64_t su_q32_div(int64_t a, int64_t b) {
    uint64_t ma, mb;
    U128 n;
    if (b == 0) {
        return a > 0 ? INT64_MAX : (a < 0 ? INT64_MIN : 0);
    }
    ma = mag64(a);
    mb = mag64(b);
    n.hi = ma >> 32;
    n.lo = ma << 32;
    return sat_from_mag128((a < 0) != (b < 0), div_rne_u128(n, mb));
}

int64_t su_q32_shl(int64_t a, uint32_t n) {
    if (a == 0) {
        return 0;
    }
    if (n >= 64u) {
        return a > 0 ? INT64_MAX : INT64_MIN;
    }
    if (a > (INT64_MAX >> n)) {
        return INT64_MAX;
    }
    if (a < su_i64_shr(INT64_MIN, n)) {
        return INT64_MIN;
    }
    return su_i64_from_bits((uint64_t)a << n);
}

int64_t su_q32_shr(int64_t a, uint32_t n) {
    return su_i64_shr(a, n);
}

int64_t su_q32_sqrt(int64_t a) {
    uint64_t u;
    if (a <= 0) {
        return 0;
    }
    u = (uint64_t)a;
    /* sqrt(a / 2^32) * 2^32 = sqrt(a * 2^32); a * 2^32 < 2^95, root < 2^48. */
    return (int64_t)su_num_sqrt_rne_u128(u >> 32, u << 32, NULL);
}

int64_t su_q32_rsqrt(int64_t a) {
    if (a <= 0) {
        return INT64_MAX;
    }
    return su_q32_div(SU_Q32_ONE, su_q32_sqrt(a));
}

/* ---- conversions ---- */

int64_t su_q32_from_i32(int32_t a) {
    return su_i64_from_bits((uint64_t)(int64_t)a << 32);
}

int64_t su_q32_from_i64(int64_t a) {
    return su_q32_shl(a, 32u);
}

int32_t su_q32_to_i32_floor(int64_t a) {
    return (int32_t)su_i64_shr(a, 32u);
}

int32_t su_q32_to_i32_round(int64_t a) {
    int64_t q = su_i64_shr(a, 32u);
    uint64_t frac = (uint64_t)a & 0xFFFFFFFFu;
    if (frac > 0x80000000u || (frac == 0x80000000u && ((uint64_t)q & 1u) != 0u)) {
        q++;
    }
    return q > INT32_MAX ? INT32_MAX : (int32_t)q;
}

int64_t su_q32_to_i64_floor(int64_t a) {
    return su_i64_shr(a, 32u);
}

int su_q32_from_ratio(int64_t num, uint64_t den, int64_t *out) {
    uint64_t m;
    U128 n, q;
    int neg = num < 0;
    if (den == 0u) {
        return 0;
    }
    m = mag64(num);
    n.hi = m >> 32;
    n.lo = m << 32;
    q = div_rne_u128(n, den);
    if (q.hi != 0u) {
        return 0;
    }
    if (!neg && q.lo > (uint64_t)INT64_MAX) {
        return 0;
    }
    if (neg && q.lo > ((uint64_t)1 << 63)) {
        return 0;
    }
    *out = sat_from_mag(neg, q.lo);
    return 1;
}

/* ---- vec3 ---- */

SuV3 su_v3_add(SuV3 a, SuV3 b) {
    SuV3 r;
    r.x = su_q32_add(a.x, b.x);
    r.y = su_q32_add(a.y, b.y);
    r.z = su_q32_add(a.z, b.z);
    return r;
}

SuV3 su_v3_sub(SuV3 a, SuV3 b) {
    SuV3 r;
    r.x = su_q32_sub(a.x, b.x);
    r.y = su_q32_sub(a.y, b.y);
    r.z = su_q32_sub(a.z, b.z);
    return r;
}

SuV3 su_v3_neg(SuV3 a) {
    SuV3 r;
    r.x = su_q32_neg(a.x);
    r.y = su_q32_neg(a.y);
    r.z = su_q32_neg(a.z);
    return r;
}

SuV3 su_v3_scale(SuV3 a, int64_t s) {
    SuV3 r;
    r.x = su_q32_mul(a.x, s);
    r.y = su_q32_mul(a.y, s);
    r.z = su_q32_mul(a.z, s);
    return r;
}

int64_t su_v3_dot(SuV3 a, SuV3 b) {
    Acc acc;
    acc_init(&acc);
    acc_add(&acc, a.x, b.x, 0);
    acc_add(&acc, a.y, b.y, 0);
    acc_add(&acc, a.z, b.z, 0);
    return acc_round32(&acc);
}

SuV3 su_v3_cross(SuV3 a, SuV3 b) {
    SuV3 r;
    Acc acc;
    acc_init(&acc);
    acc_add(&acc, a.y, b.z, 0);
    acc_add(&acc, a.z, b.y, 1);
    r.x = acc_round32(&acc);
    acc_init(&acc);
    acc_add(&acc, a.z, b.x, 0);
    acc_add(&acc, a.x, b.z, 1);
    r.y = acc_round32(&acc);
    acc_init(&acc);
    acc_add(&acc, a.x, b.y, 0);
    acc_add(&acc, a.y, b.x, 1);
    r.z = acc_round32(&acc);
    return r;
}

int64_t su_v3_len_sq(SuV3 a) {
    return su_v3_dot(a, a);
}

int64_t su_v3_len(SuV3 a) {
    U192 s = u192_from_u64_product(mag64(a.x), mag64(a.x));
    s = u192_add(s, u192_from_u64_product(mag64(a.y), mag64(a.y)));
    s = u192_add(s, u192_from_u64_product(mag64(a.z), mag64(a.z)));
    return sqrt_sum_sat(s);
}

SuV3 su_v3_normalize(SuV3 a) {
    SuV3 r;
    int64_t len = su_v3_len(a);
    if (len == 0) {
        r.x = r.y = r.z = 0;
        return r;
    }
    r.x = su_q32_div(a.x, len);
    r.y = su_q32_div(a.y, len);
    r.z = su_q32_div(a.z, len);
    return r;
}

/* RNE(c * m / len) for |c| <= len, m > 0, len > 0. */
static int64_t scale_ratio(int64_t c, int64_t m, int64_t len) {
    U128 n;
    su_num_mul_u64(mag64(c), (uint64_t)m, &n.hi, &n.lo);
    return sat_from_mag128(c < 0, div_rne_u128(n, (uint64_t)len));
}

SuV3 su_v3_clamp_len(SuV3 a, int64_t max_len) {
    SuV3 r;
    int64_t len;
    if (max_len <= 0) {
        r.x = r.y = r.z = 0;
        return r;
    }
    len = su_v3_len(a);
    if (len <= max_len) {
        return a;
    }
    r.x = scale_ratio(a.x, max_len, len);
    r.y = scale_ratio(a.y, max_len, len);
    r.z = scale_ratio(a.z, max_len, len);
    return r;
}

/* ---- quaternion ---- */

SuQuat su_quat_mul(SuQuat a, SuQuat b) {
    SuQuat r;
    Acc acc;
    acc_init(&acc);
    acc_add(&acc, a.w, b.w, 0);
    acc_add(&acc, a.x, b.x, 1);
    acc_add(&acc, a.y, b.y, 1);
    acc_add(&acc, a.z, b.z, 1);
    r.w = acc_round32(&acc);
    acc_init(&acc);
    acc_add(&acc, a.w, b.x, 0);
    acc_add(&acc, a.x, b.w, 0);
    acc_add(&acc, a.y, b.z, 0);
    acc_add(&acc, a.z, b.y, 1);
    r.x = acc_round32(&acc);
    acc_init(&acc);
    acc_add(&acc, a.w, b.y, 0);
    acc_add(&acc, a.x, b.z, 1);
    acc_add(&acc, a.y, b.w, 0);
    acc_add(&acc, a.z, b.x, 0);
    r.y = acc_round32(&acc);
    acc_init(&acc);
    acc_add(&acc, a.w, b.z, 0);
    acc_add(&acc, a.x, b.y, 0);
    acc_add(&acc, a.y, b.x, 1);
    acc_add(&acc, a.z, b.w, 0);
    r.z = acc_round32(&acc);
    return r;
}

SuQuat su_quat_conj(SuQuat a) {
    SuQuat r;
    r.w = a.w;
    r.x = su_q32_neg(a.x);
    r.y = su_q32_neg(a.y);
    r.z = su_q32_neg(a.z);
    return r;
}

SuQuat su_quat_normalize(SuQuat a) {
    SuQuat r;
    U192 s = u192_from_u64_product(mag64(a.w), mag64(a.w));
    int64_t len;
    s = u192_add(s, u192_from_u64_product(mag64(a.x), mag64(a.x)));
    s = u192_add(s, u192_from_u64_product(mag64(a.y), mag64(a.y)));
    s = u192_add(s, u192_from_u64_product(mag64(a.z), mag64(a.z)));
    len = sqrt_sum_sat(s);
    if (len == 0) {
        r.w = SU_Q32_ONE;
        r.x = r.y = r.z = 0;
        return r;
    }
    r.w = su_q32_div(a.w, len);
    r.x = su_q32_div(a.x, len);
    r.y = su_q32_div(a.y, len);
    r.z = su_q32_div(a.z, len);
    return r;
}

SuV3 su_quat_rotate(SuQuat q, SuV3 v) {
    SuV3 u, t, c, r;
    u.x = q.x;
    u.y = q.y;
    u.z = q.z;
    t = su_v3_cross(u, v);
    t = su_v3_add(t, t);
    c = su_v3_cross(u, t);
    r.x = su_q32_add(su_q32_add(v.x, su_q32_mul(q.w, t.x)), c.x);
    r.y = su_q32_add(su_q32_add(v.y, su_q32_mul(q.w, t.y)), c.y);
    r.z = su_q32_add(su_q32_add(v.z, su_q32_mul(q.w, t.z)), c.z);
    return r;
}

SuQuat su_quat_integrate(SuQuat q, SuV3 w_local, int64_t dt) {
    SuQuat om, p, q1;
    int64_t h = su_q32_mul(dt, SU_Q32_HALF);
    om.w = 0;
    om.x = w_local.x;
    om.y = w_local.y;
    om.z = w_local.z;
    p = su_quat_mul(q, om);
    q1.w = su_q32_add(q.w, su_q32_mul(p.w, h));
    q1.x = su_q32_add(q.x, su_q32_mul(p.x, h));
    q1.y = su_q32_add(q.y, su_q32_mul(p.y, h));
    q1.z = su_q32_add(q.z, su_q32_mul(p.z, h));
    return su_quat_normalize(q1);
}
