/* Fixed-step host clock (include/sutekh/clock.h): exact tick counts, interpolation factor, the clamp and
 * determinism, against integer reference arithmetic computed here. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sutekh/clock.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define NS UINT64_C(1000000000)

static void test_init(void) {
    SuHostClock c;
    CHECK(su_host_clock_init(NULL, 120, 8) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_clock_init(&c, 0, 8) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_clock_init(&c, SU_HOST_TICK_RATE_MAX + 1u, 8) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_clock_init(&c, 120, 0) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_clock_init(&c, 120, 8) == SU_OK);
    CHECK(c.hz == 120u && c.max_ticks == 8u && c.accumulator == 0u && c.total_ticks == 0u && c.dropped_ticks == 0u);
    CHECK(su_host_clock_alpha(&c) == 0.0);
    CHECK(su_host_clock_advance(NULL, NS, NULL) == 0u);
}

/* One second in 1 ms frames at 120 Hz: exactly 120 ticks, nothing left over, no drift over 100 seconds. */
static void test_no_drift(void) {
    SuHostClock c;
    uint64_t ticks = 0;
    uint32_t i;
    (void)su_host_clock_init(&c, 120, 8);
    for (i = 0; i < 100000u; i++) {
        ticks += su_host_clock_advance(&c, UINT64_C(1000000), NULL);
    }
    CHECK(ticks == 12000u);
    CHECK(c.accumulator == 0u);
    CHECK(c.total_ticks == 12000u && c.dropped_ticks == 0u);
}

/* Frames of exactly one tick in whole nanoseconds (8,333,333 ns at 120 Hz) fall behind by 1/3 ns per frame:
 * the tick that the lost thirds add up to arrives exactly when the integer arithmetic says. */
static void test_exact_boundaries(void) {
    SuHostClock c;
    double alpha = -1.0;
    uint64_t total_ns = 0;
    uint64_t ticks = 0;
    uint32_t i;
    (void)su_host_clock_init(&c, 120, 8);
    CHECK(su_host_clock_advance(&c, UINT64_C(8333333), &alpha) == 0u); /* 999,999,960 of 10^9 units */
    CHECK(c.accumulator == UINT64_C(999999960));
    CHECK(alpha > 0.99999995 && alpha < 1.0);
    CHECK(su_host_clock_advance(&c, 1u, &alpha) == 1u); /* +120 units: 1,000,000,080 */
    CHECK(c.accumulator == 80u);
    (void)su_host_clock_init(&c, 120, 8);
    for (i = 0; i < 1000u; i++) {
        uint32_t n = su_host_clock_advance(&c, UINT64_C(8333333), NULL);
        total_ns += UINT64_C(8333333);
        ticks += n;
        /* Reference: floor(total_ns * 120 / 10^9) ticks so far; total_ns * 120 < 2^63 here. */
        CHECK(ticks == total_ns * 120u / NS);
        CHECK(c.accumulator == total_ns * 120u % NS);
    }
}

/* alpha is the accumulated fraction of a tick after the returned ticks. */
static void test_alpha(void) {
    SuHostClock c;
    double alpha = -1.0;
    (void)su_host_clock_init(&c, 100, 8); /* 10 ms ticks */
    CHECK(su_host_clock_advance(&c, UINT64_C(25000000), &alpha) == 2u);
    CHECK(alpha == 0.5);
    CHECK(su_host_clock_alpha(&c) == 0.5);
    CHECK(su_host_clock_advance(&c, UINT64_C(2500000), &alpha) == 0u);
    CHECK(alpha == 0.75);
    CHECK(su_host_clock_advance(&c, UINT64_C(2500000), &alpha) == 1u);
    CHECK(alpha == 0.0);
}

/* A stall returns at most max_ticks, drops the excess whole ticks and keeps the fraction. */
static void test_clamp(void) {
    SuHostClock c;
    double alpha = -1.0;
    (void)su_host_clock_init(&c, 120, 5);
    CHECK(su_host_clock_advance(&c, NS + UINT64_C(4166667), &alpha) == 5u); /* 120.5000004 ticks due */
    CHECK(c.dropped_ticks == 115u);
    CHECK(c.total_ticks == 5u);
    CHECK(c.accumulator == UINT64_C(500000040));
    CHECK(alpha > 0.5 && alpha < 0.5000001);
    /* Elapsed values that would overflow ns * hz saturate instead of wrapping. */
    CHECK(su_host_clock_advance(&c, UINT64_MAX, &alpha) == 5u);
    CHECK(c.dropped_ticks > 115u);
    CHECK(c.accumulator < NS);
    (void)su_host_clock_init(&c, SU_HOST_TICK_RATE_MAX, 1);
    CHECK(su_host_clock_advance(&c, UINT64_MAX, NULL) == 1u);
    CHECK(c.accumulator < NS);
}

/* Same elapsed sequence, same outputs; total ticks plus dropped ticks equal the exact count. */
static void test_determinism(void) {
    SuHostClock a;
    SuHostClock b;
    uint64_t x = UINT64_C(0x9E3779B97F4A7C15);
    uint64_t total_ns = 0;
    uint32_t i;
    (void)su_host_clock_init(&a, 120, 4);
    (void)su_host_clock_init(&b, 120, 4);
    for (i = 0; i < 20000u; i++) {
        uint64_t dt;
        double fa = 0.0;
        double fb = 0.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        dt = UINT64_C(1000000) + x % UINT64_C(40000000); /* 1..41 ms frames, stalls included */
        total_ns += dt;
        CHECK(su_host_clock_advance(&a, dt, &fa) == su_host_clock_advance(&b, dt, &fb));
        CHECK(fa == fb);
    }
    CHECK(a.total_ticks == b.total_ticks && a.dropped_ticks == b.dropped_ticks && a.accumulator == b.accumulator);
    CHECK(a.total_ticks + a.dropped_ticks == total_ns * 120u / NS);
    CHECK(a.accumulator == total_ns * 120u % NS);
}

int main(void) {
    test_init();
    test_no_drift();
    test_exact_boundaries();
    test_alpha();
    test_clamp();
    test_determinism();
    printf("test_clock: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
