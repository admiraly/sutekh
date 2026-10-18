/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d narrow phase (profiles/space3d/native/space3d_geom.c): hand-derived known answers, and seeded random
 * sweeps checked against an independent double-precision oracle (earliest root of the signed distance by dense
 * sampling and bisection). Floating point appears only in this test, as the oracle; the engine code is integer.
 * Cases whose closest approach is within the grazing band of the oracle are counted but not compared. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"
#include "space3d_geom.h"

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

#define ONE SU_Q32_ONE
#define Q(x) ((int64_t)(x) * ONE)

static SuV3 V(int64_t x, int64_t y, int64_t z) {
    SuV3 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static const SuQuat QID = {ONE, 0, 0, 0};

static int near_q(int64_t a, int64_t b, int64_t tol) {
    return (a > b ? a - b : b - a) <= tol;
}

/* ---------- known answers ---------- */

static void test_known(void) {
    S3Hit h;
    S3GateHit g;
    int64_t t08 = INT64_C(3435973837); /* RNE(0.8 * 2^32) */
    /* Sphere vs sphere, head on: R = 2, contact after 8 of 10 m. */
    h = s3_sweep_sphere_sphere(V(Q(-10), 0, 0), V(Q(10), 0, 0), ONE, V(0, 0, 0), ONE);
    CHECK(h.hit && h.toi == t08 && h.n.x == -ONE && h.n.y == 0 && h.n.z == 0);
    /* Miss above, graze exactly (disc = 0), moving away, start overlapped, zero displacement. */
    h = s3_sweep_sphere_sphere(V(Q(-10), Q(3), 0), V(Q(10), 0, 0), ONE, V(0, 0, 0), ONE);
    CHECK(!h.hit);
    h = s3_sweep_sphere_sphere(V(Q(-10), Q(2), 0), V(Q(20), 0, 0), ONE, V(0, 0, 0), ONE);
    CHECK(h.hit && h.toi == ONE / 2 && h.n.y == ONE);
    h = s3_sweep_sphere_sphere(V(Q(-10), 0, 0), V(Q(-10), 0, 0), ONE, V(0, 0, 0), ONE);
    CHECK(!h.hit);
    h = s3_sweep_sphere_sphere(V(Q(1), 0, 0), V(Q(10), 0, 0), ONE, V(0, 0, 0), ONE);
    CHECK(h.hit && h.toi == 0 && h.n.x == ONE);
    h = s3_sweep_sphere_sphere(V(0, 0, 0), V(0, 0, 0), ONE, V(0, 0, 0), ONE);
    CHECK(h.hit && h.toi == 0 && h.n.z == ONE); /* coincident centres: defined normal +z */
    h = s3_sweep_sphere_sphere(V(Q(5), 0, 0), V(0, 0, 0), ONE, V(0, 0, 0), ONE);
    CHECK(!h.hit);
    /* Too short: stops 1 m before contact. */
    h = s3_sweep_sphere_sphere(V(Q(-10), 0, 0), V(Q(7), 0, 0), ONE, V(0, 0, 0), ONE);
    CHECK(!h.hit);
    /* Capsule along y from -5 to 5, radius 1: cylinder side and end cap. */
    h = s3_sweep_sphere_capsule(V(Q(-10), 0, 0), V(Q(10), 0, 0), ONE, V(0, Q(-5), 0), V(0, Q(5), 0), ONE);
    CHECK(h.hit && h.toi == t08 && h.n.x == -ONE && h.n.y == 0);
    h = s3_sweep_sphere_capsule(V(0, Q(20), 0), V(0, Q(-20), 0), ONE, V(0, Q(-5), 0), V(0, Q(5), 0), ONE);
    CHECK(h.hit && h.toi == su_q32_div(Q(13), Q(20)) && h.n.y == ONE);
    h = s3_sweep_sphere_capsule(V(Q(-10), Q(8), 0), V(Q(20), 0, 0), ONE, V(0, Q(-5), 0), V(0, Q(5), 0), ONE);
    CHECK(!h.hit); /* passes 3 m above the cap centre, R = 2 */
    /* Box half (1,1,1) at the origin; face hit at x = -2. */
    h = s3_sweep_sphere_obb(V(Q(-10), 0, 0), V(Q(10), 0, 0), ONE, V(0, 0, 0), QID, V(ONE, ONE, ONE));
    CHECK(h.hit && h.toi == t08 && h.n.x == -ONE && h.n.y == 0 && h.n.z == 0);
    /* Same box seen from +z with a 0.5 m offset: face +z. */
    h = s3_sweep_sphere_obb(V(Q(1) / 2, 0, Q(12)), V(0, 0, Q(-20)), ONE, V(0, 0, 0), QID, V(ONE, ONE, ONE));
    CHECK(h.hit && h.toi == su_q32_div(Q(10), Q(20)) && h.n.z == ONE);
    /* Passes the corner diagonally outside the rounded corner: no hit (distance 1.0 + gap). */
    h = s3_sweep_sphere_obb(V(Q(-10), Q(3), Q(3)), V(Q(20), 0, 0), ONE, V(0, 0, 0), QID, V(ONE, ONE, ONE));
    CHECK(!h.hit);
    /* Start inside the box: toi 0, least-penetration axis. */
    h = s3_sweep_sphere_obb(V(0, 0, Q(1) / 2), V(Q(1), 0, 0), ONE, V(0, 0, 0), QID, V(Q(2), Q(2), ONE));
    CHECK(h.hit && h.toi == 0 && h.n.z == ONE);
    /* Gate at the origin, normal +z, rect 5 x 5. */
    CHECK(s3_segment_gate(V(ONE, ONE, -ONE), V(ONE, ONE, ONE), V(0, 0, 0), QID, Q(5), Q(5), 0, 0, &g) == 1);
    CHECK(g.dir == 1 && g.toi == ONE / 2 && g.lx == ONE && g.ly == ONE);
    CHECK(s3_segment_gate(V(ONE, ONE, ONE), V(ONE, ONE, -ONE), V(0, 0, 0), QID, Q(5), Q(5), 0, 0, &g) == -1);
    CHECK(g.dir == -1 && g.toi == ONE / 2);
    CHECK(s3_segment_gate(V(Q(6), 0, -ONE), V(Q(6), 0, ONE), V(0, 0, 0), QID, Q(5), Q(5), 0, 0, &g) == 0);
    CHECK(s3_segment_gate(V(0, 0, -ONE), V(0, 0, 0), V(0, 0, 0), QID, Q(5), Q(5), 0, 0, &g) == 1 && g.toi == ONE);
    CHECK(s3_segment_gate(V(0, 0, 0), V(0, 0, ONE), V(0, 0, 0), QID, Q(5), Q(5), 0, 0, &g) == 0); /* starts on front */
    CHECK(s3_segment_gate(V(0, 0, -ONE), V(0, 0, -1), V(0, 0, 0), QID, Q(5), Q(5), 0, 0, &g) == 0);
    /* Disc radius 5: (3,3) inside (18 <= 25), (4,4) outside (32 > 25). */
    CHECK(s3_segment_gate(V(Q(3), Q(3), -ONE), V(Q(3), Q(3), ONE), V(0, 0, 0), QID, 0, 0, Q(5), 1, &g) == 1);
    CHECK(s3_segment_gate(V(Q(4), Q(4), -ONE), V(Q(4), Q(4), ONE), V(0, 0, 0), QID, 0, 0, Q(5), 1, &g) == 0);
    {
        /* Gate rotated 90 degrees about y: normal = +x (local z -> world x). Crossing along +x is forward. */
        SuQuat q;
        q.w = su_q32_sqrt(ONE / 2);
        q.x = 0;
        q.y = q.w;
        q.z = 0;
        CHECK(s3_segment_gate(V(Q(-1), 0, 0), V(Q(3), 0, 0), V(0, 0, 0), q, Q(5), Q(5), 0, 0, &g) == 1);
        CHECK(near_q(g.toi, ONE / 4, 4));
        CHECK(s3_segment_gate(V(Q(1), 0, 0), V(Q(-1), 0, 0), V(0, 0, 0), q, Q(5), Q(5), 0, 0, &g) == -1);
        CHECK(s3_segment_gate(V(0, 0, Q(-1)), V(0, 0, Q(1)), V(0, 0, 0), q, Q(5), Q(5), 0, 0, &g) == 0);
    }
    /* Bounds. */
    {
        SuV3 lo, hi;
        SuQuat q;
        s3_bounds_sphere(V(Q(1), Q(2), Q(3)), ONE, &lo, &hi);
        CHECK(lo.x == 0 && hi.z == Q(4));
        q.w = su_q32_sqrt(ONE / 2);
        q.x = 0;
        q.y = 0;
        q.z = q.w; /* 90 degrees about z: half (2, 1, 1) spans 1 in x and 2 in y */
        s3_bounds_obb(V(0, 0, 0), q, V(Q(2), ONE, ONE), &lo, &hi);
        CHECK(near_q(hi.x, ONE, 8) && near_q(hi.y, Q(2), 8) && near_q(hi.z, ONE, 8) && hi.x >= ONE && hi.y >= Q(2));
    }
}

/* ---------- double-precision oracle ---------- */

typedef struct D3 {
    double x, y, z;
} D3;

static double qd(int64_t q) {
    return (double)q / 4294967296.0;
}

static D3 dv(SuV3 v) {
    D3 r;
    r.x = qd(v.x);
    r.y = qd(v.y);
    r.z = qd(v.z);
    return r;
}

static D3 dadd(D3 a, D3 b) { D3 r = {a.x + b.x, a.y + b.y, a.z + b.z}; return r; }
static D3 dsub(D3 a, D3 b) { D3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static D3 dsc(D3 a, double s) { D3 r = {a.x * s, a.y * s, a.z * s}; return r; }
static double ddot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double dlen(D3 a) { return sqrt(ddot(a, a)); }

/* Rotate by a unit quaternion (w, x, y, z) in double. */
static D3 drot(const double q[4], D3 v) {
    D3 u = {q[1], q[2], q[3]};
    D3 t = {2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    D3 c = {u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
    return dadd(dadd(v, dsc(t, q[0])), c);
}

typedef struct Shape {
    int kind; /* 0 sphere, 1 capsule, 2 box */
    D3 c, a, b, half;
    double r, q[4], qc[4];
} Shape;

static double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Distance from p to the shape's core (sphere centre, capsule segment, box), minus nothing. */
static double core_dist(const Shape *s, D3 p) {
    if (s->kind == 0) {
        return dlen(dsub(p, s->c));
    }
    if (s->kind == 1) {
        D3 d = dsub(s->b, s->a);
        double t = clampd(ddot(dsub(p, s->a), d) / ddot(d, d), 0, 1);
        return dlen(dsub(p, dadd(s->a, dsc(d, t))));
    } else {
        D3 l = drot(s->qc, dsub(p, s->c));
        D3 cl = {clampd(l.x, -s->half.x, s->half.x), clampd(l.y, -s->half.y, s->half.y),
                 clampd(l.z, -s->half.z, s->half.z)};
        return dlen(dsub(l, cl));
    }
}

/* Earliest t in [0, 1] with core_dist(c0 + t v) <= R, by 4096-step sampling and 60 bisection steps. Returns -1
 * when none; *minf receives the minimum sampled f = dist - R (grazing detection). */
static double oracle_toi(const Shape *s, D3 c0, D3 v, double R, double *minf) {
    const int n = 4096;
    double prev_t = 0, f0 = core_dist(s, c0) - R;
    int i;
    *minf = f0;
    if (f0 <= 0) {
        return 0;
    }
    for (i = 1; i <= n; i++) {
        double t = (double)i / n;
        double f = core_dist(s, dadd(c0, dsc(v, t))) - R;
        if (f < *minf) {
            *minf = f;
        }
        if (f <= 0) {
            double lo = prev_t, hi = t;
            int k;
            for (k = 0; k < 60; k++) {
                double mid = 0.5 * (lo + hi);
                if (core_dist(s, dadd(c0, dsc(v, mid))) - R <= 0) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            return hi;
        }
        prev_t = t;
    }
    return -1;
}

static uint64_t g_rng = 1;
static uint64_t rng(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Uniform q32 in [-span, span] metres (span integer). */
static int64_t rq(int64_t span) {
    uint64_t r = rng() % (uint64_t)(2 * span * ONE + 1);
    return (int64_t)r - span * ONE;
}

static int64_t rpos(int64_t lo, int64_t hi) { /* q32 in [lo, hi] metres */
    return lo * ONE + (int64_t)(rng() % (uint64_t)((hi - lo) * ONE + 1));
}

static void test_random(const char *label, int kind, int cases) {
    int i, compared = 0, grazing = 0, hits = 0, mism = 0;
    double worst_t = 0, worst_n = 0;
    for (i = 0; i < cases; i++) {
        Shape s;
        SuV3 c0 = V(rq(30), rq(30), rq(30)), vq, sc = V(rq(3), rq(3), rq(3)), sa, sb, half;
        SuQuat q = QID;
        int64_t r = rpos(0, 3) + ONE / 4, rs = rpos(0, 4) + ONE / 4;
        S3Hit h;
        double minf, tref, R;
        D3 target;
        memset(&s, 0, sizeof s);
        s.kind = kind;
        /* Aim at the obstacle about half the time so hits are common. */
        target = dsub(dv(V(rq(4), rq(4), rq(4))), dv(c0));
        {
            double L = 5 + qd(rpos(0, 40));
            double tl = dlen(target);
            D3 dd = (rng() & 1) ? dsc(target, L / (tl > 0 ? tl : 1)) : dv(V(rq(25), rq(25), rq(25)));
            vq = V((int64_t)llround(dd.x * 4294967296.0), (int64_t)llround(dd.y * 4294967296.0),
                   (int64_t)llround(dd.z * 4294967296.0));
        }
        if (kind == 0) {
            s.c = dv(sc);
            h = s3_sweep_sphere_sphere(c0, vq, r, sc, rs);
            R = qd(r) + qd(rs);
        } else if (kind == 1) {
            sa = V(rq(10), rq(10), rq(10));
            sb = V(rq(10), rq(10), rq(10));
            s.a = dv(sa);
            s.b = dv(sb);
            h = s3_sweep_sphere_capsule(c0, vq, r, sa, sb, rs);
            R = qd(r) + qd(rs);
        } else {
            SuQuat q0;
            q0.w = rq(1);
            q0.x = rq(1);
            q0.y = rq(1);
            q0.z = rq(1);
            q = su_quat_normalize(q0);
            half = V(rpos(0, 8) + ONE / 8, rpos(0, 8) + ONE / 8, rpos(0, 8) + ONE / 8);
            s.c = dv(sc);
            s.half = dv(half);
            s.q[0] = qd(q.w); s.q[1] = qd(q.x); s.q[2] = qd(q.y); s.q[3] = qd(q.z);
            s.qc[0] = s.q[0]; s.qc[1] = -s.q[1]; s.qc[2] = -s.q[2]; s.qc[3] = -s.q[3];
            h = s3_sweep_sphere_obb(c0, vq, r, sc, q, half);
            R = qd(r);
        }
        tref = oracle_toi(&s, dv(c0), dv(vq), R, &minf);
        /* Grazing band: the oracle's closest approach within 1e-4 m of contact, or a tangential entry. */
        if (fabs(minf) < 1e-4 || (tref >= 0 && tref < 1e-9 && minf > -1e-4)) {
            grazing++;
            continue;
        }
        compared++;
        if ((tref >= 0) != (h.hit != 0)) {
            mism++;
            if (mism <= 5) {
                fprintf(stderr, "%s case %d: oracle %s (t %.9f, minf %.3g), engine %s (t %.9f)\n", label, i,
                        tref >= 0 ? "hit" : "miss", tref, minf, h.hit ? "hit" : "miss", qd(h.toi));
            }
            continue;
        }
        if (tref < 0) {
            continue;
        }
        hits++;
        {
            double L = dlen(dv(vq));
            double dt = fabs(qd(h.toi) - tref) * L; /* metres along the path */
            D3 p = dadd(dv(c0), dsc(dv(vq), tref));
            D3 nref;
            double nerr;
            if (kind == 0) {
                nref = dsub(p, s.c);
            } else if (kind == 1) {
                D3 d = dsub(s.b, s.a);
                double t = clampd(ddot(dsub(p, s.a), d) / ddot(d, d), 0, 1);
                nref = dsub(p, dadd(s.a, dsc(d, t)));
            } else {
                D3 l = drot(s.qc, dsub(p, s.c));
                D3 cl = {clampd(l.x, -s.half.x, s.half.x), clampd(l.y, -s.half.y, s.half.y),
                         clampd(l.z, -s.half.z, s.half.z)};
                nref = drot(s.q, dsub(l, cl));
            }
            if (dlen(nref) < 1e-9) {
                continue; /* starts inside the core: normal is a convention, checked by known answers */
            }
            nref = dsc(nref, 1.0 / dlen(nref));
            nerr = dlen(dsub(dv(h.n), nref));
            if (dt > worst_t) worst_t = dt;
            if (nerr > worst_n) worst_n = nerr;
            if (dt > 1e-5 || nerr > 1e-4) {
                mism++;
                if (mism <= 5) {
                    fprintf(stderr, "%s case %d: toi err %.3g m, normal err %.3g\n", label, i, dt, nerr);
                }
            }
        }
    }
    printf("%s: %d cases, %d compared (%d hits), %d grazing skipped, %d mismatches, worst toi err %.3g m, "
           "worst normal err %.3g\n", label, cases, compared, hits, grazing, mism, worst_t, worst_n);
    CHECK(mism == 0);
    CHECK(hits > cases / 10);
}

int main(void) {
    test_known();
    g_rng = UINT64_C(20261018);
    test_random("sphere_sphere", 0, 20000);
    test_random("sphere_capsule", 1, 20000);
    test_random("sphere_obb", 2, 20000);
    printf("test_space3d_geom: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
