/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Numeric profile fixed_q32_v1 (src/ir/su_num.c): hand-authored known answers, exact algebraic checks of the
 * wide-integer helpers, and properties of the vector and quaternion operations.
 *
 *   test_num                      run the checks; exit 0 only when every check passed
 *   test_num --vectors N SEED     print N random operation records per operation, one per line, for the
 *                                 independent big-integer model (tests/num/q32_model.py). The records
 *                                 carry inputs and the C result; the model recomputes the result.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_I64(got, want)                                                        \
    do {                                                                            \
        int64_t g_ = (got), w_ = (want);                                            \
        g_checks++;                                                                 \
        if (g_ != w_) {                                                             \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: %s = %" PRId64 ", want %" PRId64 "\n", __FILE__, __LINE__, #got, g_, w_); \
        }                                                                           \
    } while (0)

#define ONE SU_Q32_ONE
#define HALF SU_Q32_HALF

/* splitmix64: the seeded generator of the random checks and the vector dump. */
static uint64_t g_rng = 0;
static uint64_t rng_next(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Biased i64 operand: edges, small values, values near 1.0 and the physics range, and raw random bits. */
static int64_t rnd_i64(void) {
    static const int64_t edges[] = {0, 1, -1, 2, -2, INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1,
                                    ONE, -ONE, HALF, -HALF, ONE + 1, ONE - 1, 3 * HALF, -3 * HALF,
                                    INT64_C(0x7FFFFFFF), INT64_C(-0x80000000), INT64_C(0x100000000000)};
    uint64_t sel = rng_next() % 8u;
    uint64_t r = rng_next();
    switch (sel) {
    case 0:
        return edges[r % (sizeof edges / sizeof edges[0])];
    case 1:
        return su_i64_from_bits(r);
    case 2: /* |x| < 2^20 raw (tiny fractions) */
        return su_i64_from_bits(r) >> 43;
    case 3: /* |x| < 2^40 raw (up to 256 m) */
        return su_i64_from_bits(r) >> 23;
    case 4: /* |x| < 2^50 raw (up to 262 km) */
        return su_i64_from_bits(r) >> 13;
    case 5: /* integers and halves */
        return (su_i64_from_bits(r) >> 40) * HALF;
    case 6: /* |x| < 2^56 raw */
        return su_i64_from_bits(r) >> 7;
    default:
        return su_i64_from_bits(r) >> (r % 63u);
    }
}

static uint32_t rnd_shift(void) {
    uint64_t r = rng_next();
    return (r & 1u) != 0u ? (uint32_t)(r >> 1) % 70u : (uint32_t)(r >> 1);
}

/* ---------- known answers ---------- */

static void test_known_q32(void) {
    /* add/sub/neg/abs saturation */
    CHECK_I64(su_q32_add(INT64_MAX, 1), INT64_MAX);
    CHECK_I64(su_q32_add(INT64_MIN, -1), INT64_MIN);
    CHECK_I64(su_q32_add(ONE, ONE), 2 * ONE);
    CHECK_I64(su_q32_sub(INT64_MIN, 1), INT64_MIN);
    CHECK_I64(su_q32_sub(INT64_MAX, -1), INT64_MAX);
    CHECK_I64(su_q32_sub(0, INT64_MIN), INT64_MAX);
    CHECK_I64(su_q32_neg(INT64_MIN), INT64_MAX);
    CHECK_I64(su_q32_abs(INT64_MIN), INT64_MAX);
    CHECK_I64(su_q32_abs(-ONE), ONE);
    /* mul: exact, ties to even, saturation */
    CHECK_I64(su_q32_mul(3 * HALF, 2 * ONE), 3 * ONE);
    CHECK_I64(su_q32_mul(HALF, HALF), ONE / 4);
    CHECK_I64(su_q32_mul(1, HALF), 0);   /* 0.5 raw -> even 0 */
    CHECK_I64(su_q32_mul(3, HALF), 2);   /* 1.5 raw -> even 2 */
    CHECK_I64(su_q32_mul(5, HALF), 2);   /* 2.5 raw -> even 2 */
    CHECK_I64(su_q32_mul(-3, HALF), -2);
    CHECK_I64(su_q32_mul(-1, HALF), 0);
    CHECK_I64(su_q32_mul(-5, HALF), -2);
    CHECK_I64(su_q32_mul(INT64_MAX, 2 * ONE), INT64_MAX);
    CHECK_I64(su_q32_mul(INT64_MIN, 2 * ONE), INT64_MIN);
    CHECK_I64(su_q32_mul(INT64_MIN, -ONE), INT64_MAX);
    CHECK_I64(su_q32_mul(INT64_MIN, ONE), INT64_MIN);
    CHECK_I64(su_q32_mul(-ONE, -ONE), ONE);
    /* div */
    CHECK_I64(su_q32_div(ONE, 3 * ONE), INT64_C(1431655765));
    CHECK_I64(su_q32_div(2 * ONE, 3 * ONE), INT64_C(2863311531));
    CHECK_I64(su_q32_div(-ONE, 3 * ONE), INT64_C(-1431655765));
    CHECK_I64(su_q32_div(ONE, -3 * ONE), INT64_C(-1431655765));
    CHECK_I64(su_q32_div(7 * ONE, 2 * ONE), 7 * HALF);
    CHECK_I64(su_q32_div(1, 2 * ONE), 0);     /* 0.5 raw -> 0 */
    CHECK_I64(su_q32_div(3, 2 * ONE), 2);     /* 1.5 raw -> 2 */
    CHECK_I64(su_q32_div(ONE, 0), INT64_MAX);
    CHECK_I64(su_q32_div(-ONE, 0), INT64_MIN);
    CHECK_I64(su_q32_div(0, 0), 0);
    CHECK_I64(su_q32_div(INT64_MIN, -ONE), INT64_MAX);
    CHECK_I64(su_q32_div(INT64_MIN, ONE), INT64_MIN);
    CHECK_I64(su_q32_div(INT64_MAX, HALF), INT64_MAX);
    CHECK_I64(su_q32_div(INT64_MAX, INT64_MAX), ONE);
    CHECK_I64(su_q32_div(INT64_MIN, INT64_MIN), ONE);
    /* sqrt and rsqrt */
    CHECK_I64(su_q32_sqrt(4 * ONE), 2 * ONE);
    CHECK_I64(su_q32_sqrt(ONE), ONE);
    CHECK_I64(su_q32_sqrt(2 * ONE), INT64_C(6074001000)); /* sqrt(2) * 2^32 = 6074000999.95 */
    CHECK_I64(su_q32_sqrt(1), 65536);
    CHECK_I64(su_q32_sqrt(0), 0);
    CHECK_I64(su_q32_sqrt(-ONE), 0);
    CHECK_I64(su_q32_sqrt(ONE / 4), HALF);
    CHECK_I64(su_q32_sqrt(INT64_MAX), INT64_C(199032864766430)); /* round(sqrt((2^63-1) * 2^32)) */
    CHECK_I64(su_q32_rsqrt(4 * ONE), HALF);
    CHECK_I64(su_q32_rsqrt(ONE), ONE);
    CHECK_I64(su_q32_rsqrt(0), INT64_MAX);
    CHECK_I64(su_q32_rsqrt(-5), INT64_MAX);
    CHECK_I64(su_q32_rsqrt(1), INT64_C(281474976710656)); /* 1 / 2^-16 = 2^16 -> raw 2^48 */
    /* shifts */
    CHECK_I64(su_q32_shl(ONE, 3), 8 * ONE);
    CHECK_I64(su_q32_shl(ONE, 31), INT64_MAX);
    CHECK_I64(su_q32_shl(ONE, 30), INT64_C(1) << 62);
    CHECK_I64(su_q32_shl(-ONE, 31), INT64_MIN);
    CHECK_I64(su_q32_shl(-1, 63), INT64_MIN);
    CHECK_I64(su_q32_shl(1, 63), INT64_MAX);
    CHECK_I64(su_q32_shl(-2, 63), INT64_MIN);
    CHECK_I64(su_q32_shl(0, 200), 0);
    CHECK_I64(su_q32_shl(5, 64), INT64_MAX);
    CHECK_I64(su_q32_shl(-5, 64), INT64_MIN);
    CHECK_I64(su_q32_shr(-ONE, 1), -HALF);
    CHECK_I64(su_q32_shr(-1, 1), -1);
    CHECK_I64(su_q32_shr(-1, 64), -1);
    CHECK_I64(su_q32_shr(7, 64), 0);
    /* conversions */
    CHECK_I64(su_q32_from_i32(-3), -3 * ONE);
    CHECK_I64(su_q32_from_i32(INT32_MIN), INT64_MIN);
    CHECK_I64(su_q32_from_i64(INT64_C(1) << 31), INT64_MAX);
    CHECK_I64(su_q32_from_i64(-(INT64_C(1) << 31)), INT64_MIN);
    CHECK_I64(su_q32_from_i64(-(INT64_C(1) << 31) - 1), INT64_MIN);
    CHECK_I64(su_q32_from_i64(7), 7 * ONE);
    CHECK_I64(su_q32_to_i32_floor(-HALF), -1);
    CHECK_I64(su_q32_to_i32_floor(3 * HALF), 1);
    CHECK_I64(su_q32_to_i32_floor(INT64_MIN), INT32_MIN);
    CHECK_I64(su_q32_to_i32_floor(INT64_MAX), INT32_MAX);
    CHECK_I64(su_q32_to_i32_round(5 * HALF), 2);
    CHECK_I64(su_q32_to_i32_round(7 * HALF), 4);
    CHECK_I64(su_q32_to_i32_round(-5 * HALF), -2);
    CHECK_I64(su_q32_to_i32_round(-7 * HALF), -4);
    CHECK_I64(su_q32_to_i32_round(-HALF), 0);
    CHECK_I64(su_q32_to_i32_round(HALF + 1), 1);
    CHECK_I64(su_q32_to_i32_round(INT64_MAX), INT32_MAX);
    CHECK_I64(su_q32_to_i32_round(INT64_MIN), INT32_MIN);
    CHECK_I64(su_q32_to_i64_floor(-1), -1);
    {
        int64_t v = 0;
        CHECK(su_q32_from_ratio(1, 120, &v) && v == INT64_C(35791394));
        CHECK(su_q32_from_ratio(3, 2, &v) && v == 3 * HALF);
        CHECK(su_q32_from_ratio(-1, 3, &v) && v == INT64_C(-1431655765));
        CHECK(su_q32_from_ratio(1, UINT64_C(1) << 33, &v) && v == 0);        /* 0.5 raw -> 0 */
        CHECK(su_q32_from_ratio(3, UINT64_C(1) << 33, &v) && v == 2);        /* 1.5 raw -> 2 */
        CHECK(su_q32_from_ratio(INT64_MAX, UINT64_C(1) << 32, &v) && v == INT64_MAX);
        CHECK(su_q32_from_ratio(INT64_MIN, UINT64_C(1) << 32, &v) && v == INT64_MIN);
        CHECK(!su_q32_from_ratio(INT64_C(1) << 31, 1, &v));
        CHECK(su_q32_from_ratio(-(INT64_C(1) << 31), 1, &v) && v == INT64_MIN);
        CHECK(!su_q32_from_ratio(1, 0, &v));
    }
}

static void test_known_int(void) {
    CHECK_I64(su_i32_add(INT32_MAX, 1), INT32_MIN);
    CHECK_I64(su_i32_sub(INT32_MIN, 1), INT32_MAX);
    CHECK_I64(su_i32_mul(INT32_MAX, 2), -2);
    CHECK_I64(su_i32_mul(65536, 65536), 0);
    CHECK_I64(su_i32_div(7, -2), -3);
    CHECK_I64(su_i32_div(-7, 2), -3);
    CHECK_I64(su_i32_div(INT32_MIN, -1), INT32_MIN);
    CHECK_I64(su_i32_div(5, 0), 0);
    CHECK_I64(su_i32_neg(INT32_MIN), INT32_MIN);
    CHECK_I64(su_i32_abs(INT32_MIN), INT32_MIN);
    CHECK_I64(su_i32_abs(-4), 4);
    CHECK_I64(su_i32_shl(1, 31), INT32_MIN);
    CHECK_I64(su_i32_shl(3, 32), 0);
    CHECK_I64(su_i32_shr(-5, 1), -3);
    CHECK_I64(su_i32_shr(-1, 31), -1);
    CHECK_I64(su_i32_shr(-1, 99), -1);
    CHECK_I64(su_i32_shr(INT32_MAX, 99), 0);
    CHECK_I64(su_i32_from_bits(0xFFFFFFFFu), -1);
    CHECK_I64(su_i32_from_bits(0x80000000u), INT32_MIN);
    CHECK_I64(su_i64_add(INT64_MAX, 1), INT64_MIN);
    CHECK_I64(su_i64_mul(INT64_MIN, -1), INT64_MIN);
    CHECK_I64(su_i64_div(INT64_MIN, -1), INT64_MIN);
    CHECK_I64(su_i64_div(-9, 4), -2);
    CHECK_I64(su_i64_div(1, 0), 0);
    CHECK_I64(su_i64_neg(INT64_MIN), INT64_MIN);
    CHECK_I64(su_i64_shl(1, 63), INT64_MIN);
    CHECK_I64(su_i64_shl(1, 64), 0);
    CHECK_I64(su_i64_shr(INT64_MIN, 63), -1);
    CHECK_I64(su_i64_shr(-9, 2), -3);
    CHECK_I64(su_i64_from_bits(UINT64_C(0x8000000000000000)), INT64_MIN);
}

static void test_known_vec(void) {
    SuV3 a = {ONE, 2 * ONE, 3 * ONE}, b = {4 * ONE, 5 * ONE, 6 * ONE}, r;
    SuQuat q, p;
    CHECK_I64(su_v3_dot(a, b), 32 * ONE);
    r = su_v3_cross(a, b);
    CHECK(r.x == -3 * ONE && r.y == 6 * ONE && r.z == -3 * ONE);
    {
        SuV3 v = {3 * ONE, 0, 4 * ONE};
        SuV3 n = su_v3_normalize(v);
        SuV3 c = su_v3_clamp_len(v, 10 * ONE);
        CHECK_I64(su_v3_len(v), 5 * ONE);
        CHECK_I64(su_v3_len_sq(v), 25 * ONE);
        CHECK(n.x == su_q32_div(3 * ONE, 5 * ONE) && n.y == 0 && n.z == su_q32_div(4 * ONE, 5 * ONE));
        CHECK(c.x == v.x && c.y == v.y && c.z == v.z);
        c = su_v3_clamp_len(v, ONE);
        CHECK(c.x == su_q32_div(3 * ONE, 5 * ONE) && c.z == su_q32_div(4 * ONE, 5 * ONE));
        c = su_v3_clamp_len(v, 0);
        CHECK(c.x == 0 && c.y == 0 && c.z == 0);
        c = su_v3_clamp_len(v, -ONE);
        CHECK(c.x == 0 && c.y == 0 && c.z == 0);
    }
    {
        SuV3 z = {0, 0, 0};
        SuV3 n = su_v3_normalize(z);
        CHECK(n.x == 0 && n.y == 0 && n.z == 0);
        CHECK_I64(su_v3_len(z), 0);
    }
    {
        /* The length does not overflow at 100 km: sum of squares is exact. */
        SuV3 far = {100000 * ONE, -100000 * ONE, 100000 * ONE};
        int64_t len = su_v3_len(far);
        CHECK(len == INT64_C(743910157351872)); /* round(sqrt(3) * 1e5 * 2^32) */
        CHECK_I64(su_v3_len_sq(far), INT64_MAX); /* 3e10 m^2 does not fit Q32.32: saturates */
    }
    {
        SuV3 m = {INT64_MIN, INT64_MIN, INT64_MIN};
        CHECK_I64(su_v3_len(m), INT64_MAX);
        CHECK_I64(su_v3_dot(m, m), INT64_MAX);
    }
    /* Quaternions: identity, 90 degree rotation about z, Hamilton product. */
    q.w = ONE;
    q.x = q.y = q.z = 0;
    r = su_quat_rotate(q, a);
    CHECK(r.x == a.x && r.y == a.y && r.z == a.z);
    {
        int64_t s = su_q32_sqrt(HALF); /* sqrt(0.5) */
        SuV3 xa = {ONE, 0, 0};
        SuV3 rx;
        q.w = s;
        q.x = 0;
        q.y = 0;
        q.z = s;
        rx = su_quat_rotate(q, xa);
        CHECK(rx.x >= -2 && rx.x <= 2 && rx.y >= ONE - 4 && rx.y <= ONE + 4 && rx.z == 0);
        p = su_quat_mul(q, q); /* 180 degrees about z: (0, 0, 0, 1) */
        CHECK(p.w >= -2 && p.w <= 2 && p.x == 0 && p.y == 0 && p.z >= ONE - 4 && p.z <= ONE + 4);
        p = su_quat_mul(q, su_quat_conj(q));
        CHECK(p.w >= ONE - 4 && p.w <= ONE + 4 && p.x == 0 && p.y == 0 && p.z == 0);
    }
    {
        SuQuat i = {0, ONE, 0, 0}, j = {0, 0, ONE, 0}, k;
        k = su_quat_mul(i, j); /* i * j = k */
        CHECK(k.w == 0 && k.x == 0 && k.y == 0 && k.z == ONE);
        k = su_quat_mul(j, i); /* j * i = -k */
        CHECK(k.w == 0 && k.x == 0 && k.y == 0 && k.z == -ONE);
        k = su_quat_mul(i, i); /* i * i = -1 */
        CHECK(k.w == -ONE && k.x == 0 && k.y == 0 && k.z == 0);
    }
    {
        SuQuat z = {0, 0, 0, 0};
        SuQuat n = su_quat_normalize(z);
        SuQuat big = {INT64_MIN, INT64_MIN, INT64_MIN, INT64_MIN};
        CHECK(n.w == ONE && n.x == 0 && n.y == 0 && n.z == 0);
        n = su_quat_normalize(big); /* sum of squares is 2^128: length saturates, components about -1.0 */
        CHECK(n.w == su_q32_div(INT64_MIN, INT64_MAX) && n.x == n.w && n.y == n.w && n.z == n.w);
    }
    {
        /* Zero angular velocity keeps a unit quaternion (up to one normalization). */
        SuQuat id = {ONE, 0, 0, 0};
        SuV3 w0 = {0, 0, 0};
        SuQuat o = su_quat_integrate(id, w0, ONE / 120);
        CHECK(o.w == ONE && o.x == 0 && o.y == 0 && o.z == 0);
    }
}

/* ---------- exact algebraic checks of the wide helpers ---------- */

static int u128_le(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl) {
    return ah < bh || (ah == bh && al <= bl);
}

static void test_wide_helpers(void) {
    int i;
    g_rng = UINT64_C(20261018);
    for (i = 0; i < 200000; i++) {
        uint64_t d = rng_next() >> (rng_next() % 64u);
        uint64_t hi = rng_next() >> (rng_next() % 64u);
        uint64_t lo = rng_next();
        uint64_t q, rem, ph, pl, sh, sl;
        if (d == 0u) {
            d = 1u;
        }
        hi %= d; /* divlu precondition */
        q = su_num_divlu(hi, lo, d, &rem);
        /* q * d + rem == hi:lo and rem < d */
        su_num_mul_u64(q, d, &ph, &pl);
        sl = pl + rem;
        sh = ph + (sl < pl ? 1u : 0u);
        if (!(sh == hi && sl == lo && rem < d)) {
            CHECK(0);
            fprintf(stderr, "divlu %016" PRIx64 ":%016" PRIx64 " / %016" PRIx64 "\n", hi, lo, d);
            break;
        }
    }
    CHECK(i == 200000);
    for (i = 0; i < 200000; i++) {
        uint64_t hi = rng_next() >> (rng_next() % 65u > 63u ? 63u : rng_next() % 64u);
        uint64_t lo = rng_next();
        uint64_t r = su_num_isqrt_u128(hi, lo);
        uint64_t ph, pl;
        int ok;
        /* r^2 <= n < (r + 1)^2 */
        su_num_mul_u64(r, r, &ph, &pl);
        ok = u128_le(ph, pl, hi, lo);
        if (ok && r != UINT64_MAX) {
            uint64_t r1 = r + 1u;
            su_num_mul_u64(r1, r1, &ph, &pl);
            ok = !u128_le(ph, pl, hi, lo);
        }
        if (!ok) {
            CHECK(0);
            fprintf(stderr, "isqrt %016" PRIx64 ":%016" PRIx64 " -> %" PRIu64 "\n", hi, lo, r);
            break;
        }
    }
    CHECK(i == 200000);
    CHECK(su_num_isqrt_u128(UINT64_MAX, UINT64_MAX) == UINT64_MAX);
    CHECK(su_num_isqrt_u128(0, 0) == 0);
    CHECK(su_num_isqrt_u128(0, 3) == 1);
    CHECK(su_num_isqrt_u128(0, 4) == 2);
    {
        int of = 0;
        CHECK(su_num_sqrt_rne_u128(UINT64_MAX, UINT64_MAX, &of) == UINT64_MAX && of == 1);
        CHECK(su_num_sqrt_rne_u128(0, 2, &of) == 1 && of == 0);  /* 1.414 */
        CHECK(su_num_sqrt_rne_u128(0, 3, &of) == 2 && of == 0);  /* 1.732 */
        CHECK(su_num_sqrt_rne_u128(0, 6, &of) == 2 && of == 0);  /* 2.449 */
        CHECK(su_num_sqrt_rne_u128(0, 7, &of) == 3 && of == 0);  /* 2.645 */
    }
}

/* ---------- properties ---------- */

static void test_properties(void) {
    int i;
    g_rng = UINT64_C(0x5EED0001);
    for (i = 0; i < 100000; i++) {
        int64_t a = rnd_i64(), b = rnd_i64();
        /* Symmetry of RNE: mul and div are odd functions in each argument (away from saturation). */
        if (a != INT64_MIN && b != INT64_MIN) {
            int64_t m = su_q32_mul(a, b);
            int64_t mn = su_q32_mul(-a, b);
            if (m != INT64_MAX && m != INT64_MIN) {
                CHECK(mn == -m);
            }
            if (b != 0) {
                int64_t d = su_q32_div(a, b);
                int64_t dn = su_q32_div(a, -b);
                if (d != INT64_MAX && d != INT64_MIN) {
                    CHECK(dn == -d);
                }
            }
        }
        CHECK(su_q32_mul(a, b) == su_q32_mul(b, a));
        CHECK(su_q32_mul(a, ONE) == a);
        CHECK(su_q32_div(a, ONE) == a);
        /* sqrt is monotone and squares back within one rounding step for moderate inputs. */
        if (a > 0 && a < (INT64_C(1) << 62)) {
            int64_t s = su_q32_sqrt(a);
            CHECK(su_q32_sqrt(a + 1) >= s);
        }
    }
}

/* Unit-norm stays inside a tolerance band over 1,000,000 integration steps (120 Hz, varied rates). */
static void test_integrate_drift(void) {
    SuQuat q = {ONE, 0, 0, 0};
    int64_t dt = 0;
    int64_t worst = 0;
    int i;
    CHECK(su_q32_from_ratio(1, 120, &dt));
    g_rng = UINT64_C(0xD21F7);
    for (i = 0; i < 1000000; i++) {
        SuV3 w;
        w.x = su_i64_from_bits(rng_next()) >> 30; /* |w| < 8 rad/s per axis */
        w.y = su_i64_from_bits(rng_next()) >> 30;
        w.z = su_i64_from_bits(rng_next()) >> 30;
        q = su_quat_integrate(q, w, dt);
        if ((i & 1023) == 0 || i == 999999) {
            SuV3 vx = {q.x, q.y, q.z};
            int64_t n2 = su_q32_add(su_q32_mul(q.w, q.w), su_v3_dot(vx, vx));
            int64_t err = n2 > ONE ? n2 - ONE : ONE - n2;
            if (err > worst) {
                worst = err;
            }
        }
    }
    /* |q|^2 within 2^-26 of 1 (64 raw ulps). The observed worst is printed for the handoff. */
    printf("integrate_drift: 1000000 steps, worst | |q|^2 - 1 | = %" PRId64 " raw (2^-32 units)\n", worst);
    CHECK(worst <= 64);
}

/* ---------- vector dump for the independent model ---------- */

static void dump_vectors(int n, uint64_t seed) {
    int i;
    g_rng = seed;
    for (i = 0; i < n; i++) {
        int64_t a = rnd_i64(), b = rnd_i64(), c = rnd_i64();
        uint32_t s = rnd_shift();
        int32_t ia = (int32_t)(uint32_t)rng_next(), ib = (int32_t)(uint32_t)rng_next();
        SuV3 va = {a, b, c}, vb = {rnd_i64(), rnd_i64(), rnd_i64()}, vr;
        SuQuat qa = {rnd_i64(), a, b, c}, qb = {rnd_i64(), rnd_i64(), rnd_i64(), rnd_i64()}, qr;
        int64_t den = rnd_i64();
        int64_t rv = 0;
        int ok;
        if ((i & 3) == 0) { /* moderate operands too, where nothing saturates */
            ia = su_i32_from_bits((uint32_t)rng_next()) >> 8;
        }
        printf("i32_add %d %d %d\n", ia, ib, su_i32_add(ia, ib));
        printf("i32_sub %d %d %d\n", ia, ib, su_i32_sub(ia, ib));
        printf("i32_mul %d %d %d\n", ia, ib, su_i32_mul(ia, ib));
        printf("i32_div %d %d %d\n", ia, ib >> (s % 32u), su_i32_div(ia, ib >> (s % 32u)));
        printf("i32_neg %d %d\n", ia, su_i32_neg(ia));
        printf("i32_abs %d %d\n", ia, su_i32_abs(ia));
        printf("i32_shl %d %u %d\n", ia, s, su_i32_shl(ia, s));
        printf("i32_shr %d %u %d\n", ia, s, su_i32_shr(ia, s));
        printf("i64_add %" PRId64 " %" PRId64 " %" PRId64 "\n", a, b, su_i64_add(a, b));
        printf("i64_sub %" PRId64 " %" PRId64 " %" PRId64 "\n", a, b, su_i64_sub(a, b));
        printf("i64_mul %" PRId64 " %" PRId64 " %" PRId64 "\n", a, b, su_i64_mul(a, b));
        printf("i64_div %" PRId64 " %" PRId64 " %" PRId64 "\n", a, b, su_i64_div(a, b));
        printf("i64_neg %" PRId64 " %" PRId64 "\n", a, su_i64_neg(a));
        printf("i64_abs %" PRId64 " %" PRId64 "\n", a, su_i64_abs(a));
        printf("i64_shl %" PRId64 " %u %" PRId64 "\n", a, s, su_i64_shl(a, s));
        printf("i64_shr %" PRId64 " %u %" PRId64 "\n", a, s, su_i64_shr(a, s));
        printf("q32_add %" PRId64 " %" PRId64 " %" PRId64 "\n", a, b, su_q32_add(a, b));
        printf("q32_sub %" PRId64 " %" PRId64 " %" PRId64 "\n", a, b, su_q32_sub(a, b));
        printf("q32_mul %" PRId64 " %" PRId64 " %" PRId64 "\n", a, b, su_q32_mul(a, b));
        printf("q32_div %" PRId64 " %" PRId64 " %" PRId64 "\n", a, b, su_q32_div(a, b));
        printf("q32_neg %" PRId64 " %" PRId64 "\n", a, su_q32_neg(a));
        printf("q32_abs %" PRId64 " %" PRId64 "\n", a, su_q32_abs(a));
        printf("q32_shl %" PRId64 " %u %" PRId64 "\n", a, s, su_q32_shl(a, s));
        printf("q32_shr %" PRId64 " %u %" PRId64 "\n", a, s, su_q32_shr(a, s));
        printf("q32_sqrt %" PRId64 " %" PRId64 "\n", a, su_q32_sqrt(a));
        printf("q32_rsqrt %" PRId64 " %" PRId64 "\n", a, su_q32_rsqrt(a));
        printf("q32_from_i32 %d %" PRId64 "\n", ia, su_q32_from_i32(ia));
        printf("q32_from_i64 %" PRId64 " %" PRId64 "\n", a >> (s % 64u), su_q32_from_i64(a >> (s % 64u)));
        printf("q32_to_i32_floor %" PRId64 " %d\n", a, su_q32_to_i32_floor(a));
        printf("q32_to_i32_round %" PRId64 " %d\n", a, su_q32_to_i32_round(a));
        printf("q32_to_i64_floor %" PRId64 " %" PRId64 "\n", a, su_q32_to_i64_floor(a));
        ok = su_q32_from_ratio(b >> (s % 64u), (uint64_t)den, &rv);
        printf("q32_from_ratio %" PRId64 " %" PRIu64 " %d %" PRId64 "\n", b >> (s % 64u), (uint64_t)den, ok,
               ok ? rv : 0);
        printf("v3_dot %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\n",
               va.x, va.y, va.z, vb.x, vb.y, vb.z, su_v3_dot(va, vb));
        vr = su_v3_cross(va, vb);
        printf("v3_cross %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
               " %" PRId64 " %" PRId64 "\n", va.x, va.y, va.z, vb.x, vb.y, vb.z, vr.x, vr.y, vr.z);
        printf("v3_len %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\n", va.x, va.y, va.z, su_v3_len(va));
        vr = su_v3_normalize(va);
        printf("v3_normalize %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\n", va.x,
               va.y, va.z, vr.x, vr.y, vr.z);
        vr = su_v3_clamp_len(va, c);
        printf("v3_clamp_len %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
               "\n", va.x, va.y, va.z, c, vr.x, vr.y, vr.z);
        qr = su_quat_mul(qa, qb);
        printf("quat_mul %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
               " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\n", qa.w, qa.x, qa.y, qa.z, qb.w, qb.x,
               qb.y, qb.z, qr.w, qr.x, qr.y, qr.z);
        qr = su_quat_normalize(qa);
        printf("quat_normalize %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
               " %" PRId64 "\n", qa.w, qa.x, qa.y, qa.z, qr.w, qr.x, qr.y, qr.z);
        vr = su_quat_rotate(qb, vb);
        printf("quat_rotate %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
               " %" PRId64 " %" PRId64 " %" PRId64 "\n", qb.w, qb.x, qb.y, qb.z, vb.x, vb.y, vb.z, vr.x, vr.y, vr.z);
        qr = su_quat_integrate(qa, vb, c);
        printf("quat_integrate %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
               " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\n", qa.w, qa.x, qa.y, qa.z, vb.x, vb.y,
               vb.z, c, qr.w, qr.x, qr.y, qr.z);
    }
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--vectors") == 0) {
        dump_vectors(atoi(argv[2]), (uint64_t)strtoull(argv[3], NULL, 10));
        return 0;
    }
    test_known_q32();
    test_known_int();
    test_known_vec();
    test_wide_helpers();
    test_properties();
    test_integrate_drift();
    printf("test_num: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
