/* Regression fixtures of the determinism matrix (docs/DETERMINISM_MATRIX.md, section "Findings").
 *
 * D1 (space3d box sign products): s3_sweep_sphere_obb (edge/vertex region), box_corner and through it
 * s3_sweep_capsule_obb and s3_dist_seg_obb multiplied a sign (-1, 0, +1) by a box half extent read from a world lane,
 * as a plain int64_t product. A half extent of INT64_MIN (any q32 lane value is a legal input) made -1 * INT64_MIN a
 * signed overflow: undefined behaviour, which a compiler may compile differently. The product is now the wrapping
 * product (profiles/space3d/native/space3d_geom.c, sign_mul), the value every compiler produced before for these
 * inputs on x86-64 (two's complement imul/neg), so no defined result changed. The known answers below were taken
 * from the pre-fix code built by GCC 16.2 -O3 and Clang 22.1 -O3 (identical) and are checked here on every build; a
 * UBSan build (-fsanitize=undefined) reported the overflow before the fix and is clean after it.
 *
 * Usage: test_det_regress            check the known answers
 *        test_det_regress --print    print the answer lines */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "space3d_geom.h"

#define ONE INT64_C(4294967296)

static SuV3 v3(int64_t x, int64_t y, int64_t z) {
    SuV3 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static SuQuat quat(int64_t w, int64_t x, int64_t y, int64_t z) {
    SuQuat q;
    q.w = w;
    q.x = x;
    q.y = y;
    q.z = z;
    return q;
}

static unsigned long long u(int64_t v) {
    uint64_t r;
    memcpy(&r, &v, sizeof r);
    return (unsigned long long)r;
}

static void hit_line(char *out, size_t cap, const char *name, S3Hit h) {
    snprintf(out, cap, "%s hit=%d toi=%016llx n=%016llx,%016llx,%016llx", name, h.hit, u(h.toi), u(h.n.x), u(h.n.y),
             u(h.n.z));
}

/* Known answers (pre-fix GCC 16.2 -O3 == Clang 22.1 -O3; see the header). */
static const char *const k_expect[] = {
#include "regress_space3d_half_min.inc"
};

int main(int argc, char **argv) {
    const int64_t halves[] = {INT64_MIN, INT64_MIN + 1, INT64_MAX, -ONE, 0, 3 * ONE};
    const SuQuat rots[] = {quat(ONE, 0, 0, 0), quat(3037000500, 0, 0, 3037000500)};
    char line[256];
    unsigned n = 0, bad = 0;
    unsigned hi, ri, k;
    int print = argc > 1 && strcmp(argv[1], "--print") == 0;
    const unsigned expect_n = (unsigned)(sizeof k_expect / sizeof k_expect[0]);

    for (hi = 0; hi < sizeof halves / sizeof halves[0]; hi++) {
        for (ri = 0; ri < sizeof rots / sizeof rots[0]; ri++) {
            for (k = 0; k < 3u; k++) {
                SuV3 half = v3(k == 0u ? halves[hi] : 2 * ONE, k == 1u ? halves[hi] : ONE, halves[hi]);
                SuV3 c0 = v3(10 * ONE, 9 * ONE, -8 * ONE);
                SuV3 vel = v3(-20 * ONE, -18 * ONE, 16 * ONE);
                SuV3 e0 = v3(0, 0, -ONE), e1 = v3(0, 0, ONE);
                SuV3 center = v3(0, 0, 0);
                char name[64];
                const char *got[3];
                char l0[256], l1[256], l2[256];
                unsigned i;
                snprintf(name, sizeof name, "half%u.rot%u.axis%u", hi, ri, k);
                hit_line(l0, sizeof l0, "sphere_obb", s3_sweep_sphere_obb(c0, vel, ONE / 2, center, rots[ri], half));
                hit_line(l1, sizeof l1, "capsule_obb",
                         s3_sweep_capsule_obb(c0, vel, ONE / 2, e0, e1, center, rots[ri], half));
                snprintf(l2, sizeof l2, "dist_seg_obb %016llx",
                         u(s3_dist_seg_obb(c0, e0, e1, center, rots[ri], half)));
                got[0] = l0;
                got[1] = l1;
                got[2] = l2;
                for (i = 0; i < 3u; i++) {
                    snprintf(line, sizeof line, "%s %s", name, got[i]);
                    if (print) {
                        printf("    \"%s\",\n", line);
                    } else if (n >= expect_n || strcmp(line, k_expect[n]) != 0) {
                        fprintf(stderr, "MISMATCH\n  expected: %s\n  got:      %s\n", n < expect_n ? k_expect[n] : "-",
                                line);
                        bad++;
                    }
                    n++;
                }
            }
        }
    }
    if (print) {
        return 0;
    }
    if (n != expect_n) {
        fprintf(stderr, "%u answers, %u expected\n", n, expect_n);
        bad++;
    }
    printf("test_det_regress: %u known answers, %u mismatches\n", n, bad);
    return bad == 0u ? 0 : 1;
}
