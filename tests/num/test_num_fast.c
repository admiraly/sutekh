/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * su_num_fast.h against su_num.c: every fast function must return the reference bits for every input. Inputs:
 * an edge set (0, +-1, the 2^62 fast-path boundary, the extremes, 1 and 1/2 in q32) in every combination for the
 * binary scalar functions, then seeded random values drawn at every magnitude 2^0..2^63 (so both the fast paths
 * and the reference fallbacks run), plus racer-scale vectors and unit quaternions.
 *
 *   test_num_fast [iterations per function, default 200000] [seed] */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "ir/su_num.h"
#include "ir/su_num_fast.h"

static uint64_t g_rng;
static uint64_t g_checks, g_fail;

static uint64_t rng(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static const int64_t EDGE[] = {0,
                               1,
                               -1,
                               2,
                               SU_Q32_ONE,
                               -SU_Q32_ONE,
                               SU_Q32_HALF,
                               -SU_Q32_HALF,
                               INT64_C(0x3FFFFFFFFFFFFFFF),
                               -INT64_C(0x4000000000000000),
                               INT64_C(0x4000000000000000),
                               -INT64_C(0x4000000000000001),
                               INT64_MAX,
                               INT64_MIN,
                               INT64_MAX - 1,
                               INT64_MIN + 1,
                               INT64_C(0x7FFFFFFF),
                               INT64_C(0x80000000),
                               INT64_C(0xFFFFFFFF),
                               INT64_C(0x100000001),
                               INT64_C(0x000FFFFFFFFFFFFF),
                               INT64_C(0x0010000000000000)};
#define NEDGE (sizeof EDGE / sizeof EDGE[0])

/* Random value at a random magnitude; 1 in 8 an edge value. */
static int64_t rv(void) {
    uint64_t r = rng();
    if ((r & 7u) == 0u) {
        return EDGE[(r >> 3) % NEDGE];
    }
    return su_i64_from_bits(rng()) >> (r >> 8) % 64u;
}

/* Racer-scale q32: |x| < 2^47 (32 km). */
static int64_t rq(void) {
    return su_i64_from_bits(rng()) >> (16u + rng() % 40u);
}

static SuV3 v3(int racer) {
    SuV3 v;
    v.x = racer ? rq() : rv();
    v.y = racer ? rq() : rv();
    v.z = racer ? rq() : rv();
    return v;
}

static SuQuat qt(int racer) {
    SuQuat q;
    q.w = racer ? rq() : rv();
    q.x = racer ? rq() : rv();
    q.y = racer ? rq() : rv();
    q.z = racer ? rq() : rv();
    if (racer == 2) {
        q = su_quat_normalize(q);
    }
    return q;
}

#define EQ(name, got, want, a, b)                                                                              \
    do {                                                                                                       \
        g_checks++;                                                                                            \
        if ((got) != (want)) {                                                                                 \
            if (g_fail++ < 20u) {                                                                              \
                fprintf(stderr, "FAIL %s(%" PRId64 ", %" PRId64 "): got %" PRId64 " want %" PRId64 "\n", name, \
                        (int64_t)(a), (int64_t)(b), (int64_t)(got), (int64_t)(want));                          \
            }                                                                                                  \
        }                                                                                                      \
    } while (0)

static void v3eq(const char *name, SuV3 g, SuV3 w, int64_t a) {
    EQ(name, g.x, w.x, a, 0);
    EQ(name, g.y, w.y, a, 1);
    EQ(name, g.z, w.z, a, 2);
}

static void qeq(const char *name, SuQuat g, SuQuat w, int64_t a) {
    EQ(name, g.w, w.w, a, 0);
    EQ(name, g.x, w.x, a, 1);
    EQ(name, g.y, w.y, a, 2);
    EQ(name, g.z, w.z, a, 3);
}

static void scalars(int64_t a, int64_t b) {
    EQ("add", su_q32_addf(a, b), su_q32_add(a, b), a, b);
    EQ("sub", su_q32_subf(a, b), su_q32_sub(a, b), a, b);
    EQ("mul", su_q32_mulf(a, b), su_q32_mul(a, b), a, b);
    EQ("div", su_q32_divf(a, b), su_q32_div(a, b), a, b);
    EQ("sqrt", su_q32_sqrtf(a), su_q32_sqrt(a), a, 0);
    EQ("rsqrt", su_q32_rsqrtf(a), su_q32_rsqrt(a), a, 0);
}

int main(int argc, char **argv) {
    uint64_t iters = argc > 1 ? strtoull(argv[1], NULL, 10) : 200000u;
    uint64_t i;
    size_t x, y;
    g_rng = argc > 2 ? strtoull(argv[2], NULL, 10) : 1u;

    for (x = 0; x < NEDGE; x++) {
        for (y = 0; y < NEDGE; y++) {
            scalars(EDGE[x], EDGE[y]);
        }
    }
    for (i = 0; i < iters; i++) {
        int racer = (int)(i % 3u); /* 0 any magnitude, 1 racer scale, 2 racer scale with unit quaternions */
        int64_t a = racer ? rq() : rv(), b = racer ? rq() : rv(), m = rv();
        SuV3 va = v3(racer), vb = v3(racer);
        SuQuat qa = qt(racer), qb = qt(racer);
        scalars(a, b);
        v3eq("v3_add", su_v3_addf(va, vb), su_v3_add(va, vb), va.x);
        v3eq("v3_sub", su_v3_subf(va, vb), su_v3_sub(va, vb), va.x);
        v3eq("v3_scale", su_v3_scalef(va, b), su_v3_scale(va, b), va.x);
        EQ("v3_dot", su_v3_dotf(va, vb), su_v3_dot(va, vb), va.x, vb.x);
        v3eq("v3_cross", su_v3_crossf(va, vb), su_v3_cross(va, vb), va.x);
        EQ("v3_len", su_v3_lenf(va), su_v3_len(va), va.x, va.y);
        EQ("v3_len_sq", su_v3_len_sqf(va), su_v3_len_sq(va), va.x, va.y);
        v3eq("v3_normalize", su_v3_normalizef(va), su_v3_normalize(va), va.x);
        v3eq("v3_clamp_len", su_v3_clamp_lenf(va, m), su_v3_clamp_len(va, m), m);
        v3eq("v3_clamp_len_b", su_v3_clamp_lenf(va, b < 0 ? -(b / 2) : b), su_v3_clamp_len(va, b < 0 ? -(b / 2) : b), b);
        qeq("quat_mul", su_quat_mulf(qa, qb), su_quat_mul(qa, qb), qa.w);
        qeq("quat_conj", su_quat_conjf(qa), su_quat_conj(qa), qa.w);
        qeq("quat_normalize", su_quat_normalizef(qa), su_quat_normalize(qa), qa.w);
        v3eq("quat_rotate", su_quat_rotatef(qa, va), su_quat_rotate(qa, va), qa.w);
        qeq("quat_integrate", su_quat_integratef(qa, va, b), su_quat_integrate(qa, va, b), b);
        qeq("quat_integrate_dt", su_quat_integratef(qa, va, SU_Q32_ONE / 120), su_quat_integrate(qa, va, SU_Q32_ONE / 120),
            qa.x);
    }
    printf("num_fast: %" PRIu64 " checks, %" PRIu64 " failures\n", g_checks, g_fail);
    return g_fail == 0u ? 0 : 1;
}
