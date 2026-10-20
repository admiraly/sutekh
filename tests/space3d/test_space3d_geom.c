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
    /* Ellipse, semi-axes 10 (local x) and 4 (local y): (9,0), (0,3.9), (6,3) inside (0.9225); (0,4.5), (7,3)
     * outside (1.0525); equal semi-axes agree with the disc; a zero semi-axis admits nothing. */
    {
        static const int64_t pts[][3] = {{9, 0, 1}, {0, 39, 1}, {6, 3, 1}, {0, 45, 0}, {7, 3, 0}, {10, 0, 1}, {0, 4, 1}};
        size_t k;
        for (k = 0; k < sizeof pts / sizeof pts[0]; k++) {
            int64_t x = Q(pts[k][0]), y = pts[k][1] >= 10 ? Q(pts[k][1]) / 10 : Q(pts[k][1]);
            int r = s3_segment_gate(V(x, y, -ONE), V(x, y, ONE), V(0, 0, 0), QID, Q(10), Q(4), 0, S3_GATE_ELLIPSE, &g);
            CHECK(r == (pts[k][2] ? 1 : 0));
            if (r != (pts[k][2] ? 1 : 0)) {
                fprintf(stderr, "ellipse point %zu\n", k);
            }
        }
        for (k = 0; k < 40; k++) {
            int64_t x = Q((int64_t)k % 7) - Q(3), y = Q((int64_t)k / 7) - Q(3);
            S3GateHit g2;
            int re = s3_segment_gate(V(x, y, -ONE), V(x, y, ONE), V(0, 0, 0), QID, Q(5), Q(5), 0, S3_GATE_ELLIPSE, &g);
            int rd = s3_segment_gate(V(x, y, -ONE), V(x, y, ONE), V(0, 0, 0), QID, 0, 0, Q(5), S3_GATE_DISC, &g2);
            CHECK(re == rd);
        }
        CHECK(s3_segment_gate(V(0, 0, -ONE), V(0, 0, ONE), V(0, 0, 0), QID, 0, Q(4), 0, S3_GATE_ELLIPSE, &g) == 0);
    }
    {
        /* Ellipse rotated 90 degrees about z: local x (semi-axis 10) lies along world y. */
        SuQuat q;
        q.w = su_q32_sqrt(ONE / 2);
        q.x = 0;
        q.y = 0;
        q.z = q.w;
        CHECK(s3_segment_gate(V(0, Q(9), -ONE), V(0, Q(9), ONE), V(0, 0, 0), q, Q(10), Q(4), 0, S3_GATE_ELLIPSE, &g) == 1);
        CHECK(near_q(g.lx, Q(9), 16) && near_q(g.ly, 0, 16));
        CHECK(s3_segment_gate(V(Q(9), 0, -ONE), V(Q(9), 0, ONE), V(0, 0, 0), q, Q(10), Q(4), 0, S3_GATE_ELLIPSE, &g) == 0);
    }
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
    int kind; /* 0 sphere, 1 capsule, 2 box, 3 triangle (a, b, t2) */
    D3 c, a, b, half, t2;
    double r, q[4], qc[4];
} Shape;

/* Closest point of triangle a-b-c to p (Ericson, RTCD 5.1.5), double. */
static D3 dtri_closest(D3 p, D3 a, D3 b, D3 c) {
    D3 ab = dsub(b, a), ac = dsub(c, a), ap = dsub(p, a), bp, cp;
    double d1 = ddot(ab, ap), d2 = ddot(ac, ap), d3, d4, d5, d6, va, vb, vc, v, w, den;
    if (d1 <= 0 && d2 <= 0) return a;
    bp = dsub(p, b);
    d3 = ddot(ab, bp);
    d4 = ddot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return dadd(a, dsc(ab, d1 / (d1 - d3)));
    cp = dsub(p, c);
    d5 = ddot(ab, cp);
    d6 = ddot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return dadd(a, dsc(ac, d2 / (d2 - d6)));
    va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        return dadd(b, dsc(dsub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    }
    den = 1.0 / (va + vb + vc);
    v = vb * den;
    w = vc * den;
    return dadd(a, dadd(dsc(ab, v), dsc(ac, w)));
}

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
    } else if (s->kind == 3) {
        return dlen(dsub(p, dtri_closest(p, s->a, s->b, s->t2)));
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
        } else if (kind == 3) {
            SuV3 ta = V(rq(10), rq(10), rq(10)), tb = V(rq(10), rq(10), rq(10)), tc = V(rq(10), rq(10), rq(10));
            S3Tri t = s3_tri_make(ta, tb, tc);
            s.a = dv(ta);
            s.b = dv(tb);
            s.t2 = dv(tc);
            h = s3_sweep_sphere_tri(c0, vq, r, &t);
            R = qd(r);
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
            } else if (kind == 3) {
                nref = dsub(p, dtri_closest(p, s.a, s.b, s.t2));
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

/* ---------- triangles (2026-10-20) ---------- */

static void test_tri_known(void) {
    /* Triangle (0,0,0) (10,0,0) (0,10,0): normal +z. */
    S3Tri t = s3_tri_make(V(0, 0, 0), V(Q(10), 0, 0), V(0, Q(10), 0));
    S3Tri dg = s3_tri_make(V(0, 0, 0), V(0, 0, 0), V(0, Q(10), 0));
    S3Hit h, hc;
    CHECK(t.n.x == 0 && t.n.y == 0 && t.n.z == ONE);
    CHECK(dg.n.x == 0 && dg.n.y == 0 && dg.n.z == 0);
    /* 1. Face from above and below: sphere r 1 at (2, 2, +-5) moving 10 towards the plane: toi 0.4, normal +-z. */
    h = s3_sweep_sphere_tri(V(Q(2), Q(2), Q(5)), V(0, 0, -Q(10)), ONE, &t);
    CHECK(h.hit && near_q(h.toi, su_q32_div(Q(2), Q(5)), 2) && h.n.z == ONE && h.n.x == 0 && h.n.y == 0);
    h = s3_sweep_sphere_tri(V(Q(2), Q(2), -Q(5)), V(0, 0, Q(10)), ONE, &t);
    CHECK(h.hit && near_q(h.toi, su_q32_div(Q(2), Q(5)), 2) && h.n.z == -ONE);
    /* 2. Edge ab (y = 0) in the plane: from (5, -3, 0) moving +y by 10: contact at y = -1, toi 0.2, normal -y. */
    h = s3_sweep_sphere_tri(V(Q(5), -Q(3), 0), V(0, Q(10), 0), ONE, &t);
    CHECK(h.hit && near_q(h.toi, su_q32_div(ONE, Q(5)), 2) && h.n.y == -ONE && h.n.x == 0 && h.n.z == 0);
    /* 3. Miss beside the hypotenuse; start contact (centre 0.5 above the face): toi 0, normal +z. */
    CHECK(!s3_sweep_sphere_tri(V(Q(8), Q(8), Q(5)), V(0, 0, -Q(10)), ONE, &t).hit);
    h = s3_sweep_sphere_tri(V(Q(2), Q(2), ONE / 2), V(Q(5), 0, 0), ONE, &t);
    CHECK(h.hit && h.toi == 0 && h.n.z == ONE);
    /* 4. Degenerate triangle: only its edges collide, like the segment (0,0,0)-(0,10,0). */
    h = s3_sweep_sphere_tri(V(-Q(5), Q(4), 0), V(Q(10), 0, 0), ONE, &dg);
    hc = s3_sweep_sphere_capsule(V(-Q(5), Q(4), 0), V(Q(10), 0, 0), ONE, V(0, 0, 0), V(0, Q(10), 0), 0);
    CHECK(h.hit && hc.hit && h.toi == hc.toi && h.n.x == hc.n.x && h.n.y == hc.n.y && h.n.z == hc.n.z);
    /* 5. Capsule lying flat (segment x +- 3) at (2, 2, 5) falling by 10: face contact at z = 1, toi 0.4. */
    h = s3_sweep_capsule_tri(V(Q(2), Q(2), Q(5)), V(0, 0, -Q(10)), ONE, V(-Q(3), 0, 0), V(Q(3), 0, 0), &t);
    CHECK(h.hit && near_q(h.toi, su_q32_div(Q(2), Q(5)), 2) && h.n.z == ONE);
    /* 6. Segment crossing the face at the start: toi 0, face normal on the reference point's side. */
    h = s3_sweep_capsule_tri(V(Q(2), Q(2), ONE / 4), V(ONE, 0, 0), ONE / 10, V(0, 0, -Q(3)), V(0, 0, Q(3)), &t);
    CHECK(h.hit && h.toi == 0 && h.n.z == ONE);
    /* 7. Edge against segment: vertical segment (z +- 3) at (-5, 5, 0) moving +x by 10 reaches edge ca (x = 0)
     * at x = -1: toi 0.4, normal -x (a sphere mover at the same point also hits; one 5 m above misses). */
    h = s3_sweep_capsule_tri(V(-Q(5), Q(5), 0), V(Q(10), 0, 0), ONE, V(0, 0, -Q(3)), V(0, 0, Q(3)), &t);
    CHECK(h.hit && near_q(h.toi, su_q32_div(Q(2), Q(5)), 2) && h.n.x == -ONE && h.n.y == 0 && h.n.z == 0);
    CHECK(!s3_sweep_sphere_tri(V(-Q(5), Q(5), Q(5)), V(Q(10), 0, 0), ONE, &t).hit);
    h = s3_sweep_capsule_tri(V(-Q(5), Q(5), Q(5)), V(Q(10), 0, 0), ONE, V(0, 0, -Q(5)), V(0, 0, Q(1)), &t);
    CHECK(h.hit && near_q(h.toi, su_q32_div(Q(2), Q(5)), 2) && h.n.x == -ONE);
    /* 8. Distances: point above the face 5; past vertex a at (-3, -4, 0): 5; a segment through the face: 0;
     * offsets point from the triangle to the point. */
    CHECK(near_q(s3_dist_seg_tri(V(Q(2), Q(2), Q(5)), V(0, 0, 0), V(0, 0, 0), &t), Q(5), 2));
    CHECK(near_q(s3_dist_seg_tri(V(-Q(3), -Q(4), 0), V(0, 0, 0), V(0, 0, 0), &t), Q(5), 4));
    CHECK(s3_dist_seg_tri(V(Q(2), Q(2), 0), V(0, 0, -ONE), V(0, 0, ONE), &t) == 0);
    {
        SuV3 o = s3_tri_offset(&t, V(Q(2), Q(2), -Q(5)));
        CHECK(o.x == 0 && o.y == 0 && o.z == -Q(5));
    }
}

/* ---------- capsule movers (2026-10-18) ---------- */

static void test_capsule_known(void) {
    S3Hit h;
    SuV3 ez0 = V(0, 0, -Q(2)), ez1 = V(0, 0, Q(2));
    /* 1. Mover segment z in [-2, 2], r 1, from x = -10 along +x by 20, against a sphere (r 1) at (0, 0, 3): the
     * top end is 1 below the centre, R = 2, so contact at horizontal distance sqrt(3): s = 10 - sqrt(3),
     * toi = (10 - sqrt 3) / 20 = 0.41340; normal (-sqrt 3, 0, -1) / 2. A sphere mover on the same path misses. */
    h = s3_sweep_capsule_sphere(V(-Q(10), 0, 0), V(Q(20), 0, 0), ONE, ez0, ez1, V(0, 0, Q(3)), ONE);
    CHECK(h.hit && fabs(qd(h.toi) - (10.0 - sqrt(3.0)) / 20.0) < 1e-8);
    CHECK(fabs(qd(h.n.x) + sqrt(3.0) / 2) < 1e-8 && h.n.y == 0 && fabs(qd(h.n.z) + 0.5) < 1e-8);
    CHECK(!s3_sweep_sphere_sphere(V(-Q(10), 0, 0), V(Q(20), 0, 0), ONE, V(0, 0, Q(3)), ONE).hit);
    /* 2. Crossed capsules (face of the parallelogram): obstacle along y at x = 5, rc 1; mover r 1 from x = 0
     * by +10: contact at x = 3, toi 0.3 exactly representable as s / L = 3 / 10; normal -x. */
    h = s3_sweep_capsule_capsule(V(0, 0, 0), V(Q(10), 0, 0), ONE, ez0, ez1, V(Q(5), -Q(3), 0), V(Q(5), Q(3), 0), ONE);
    CHECK(h.hit && near_q(h.toi, su_q32_div(Q(3), Q(10)), 2) && h.n.x == -ONE && h.n.y == 0 && h.n.z == 0);
    /* 3. Parallel capsules (edges): the obstacle along z at x = 5 from z = -1 to 1. Same answer. */
    h = s3_sweep_capsule_capsule(V(0, 0, 0), V(Q(10), 0, 0), ONE, ez0, ez1, V(Q(5), 0, -ONE), V(Q(5), 0, ONE), ONE);
    CHECK(h.hit && near_q(h.toi, su_q32_div(Q(3), Q(10)), 2) && h.n.x == -ONE && h.n.y == 0 && h.n.z == 0);
    /* 4. Box edge: unit box at (5, 0, 0); mover segment along y in [-3, 3] at height z = 1.5, r 1, from x = 0 by
     * +10. Its line meets the box's top edge x = 4, z = 1 at distance 1 when 4 - x = sqrt(0.75): toi 0.31340,
     * normal (-sqrt 0.75, 0, 0.5). The end spheres (|y| = 3 > 1) hit later. */
    h = s3_sweep_capsule_obb(V(0, 0, Q(3) / 2), V(Q(10), 0, 0), ONE, V(0, -Q(3), 0), V(0, Q(3), 0), V(Q(5), 0, 0), QID,
                             V(ONE, ONE, ONE));
    CHECK(h.hit && fabs(qd(h.toi) - (4.0 - sqrt(0.75)) / 10.0) < 1e-8);
    CHECK(fabs(qd(h.n.x) + sqrt(0.75)) < 1e-7 && fabs(qd(h.n.y)) < 1e-7 && fabs(qd(h.n.z) - 0.5) < 1e-7);
    /* 5. Box face via an end sphere: the same mover at z = 0 along x (segment x in [-2, 2]) hits the face x = 4
     * when its front end (x + 2) is 1 away: x = 1, toi 0.1, normal -x. */
    h = s3_sweep_capsule_obb(V(0, 0, 0), V(Q(10), 0, 0), ONE, V(-Q(2), 0, 0), V(Q(2), 0, 0), V(Q(5), 0, 0), QID,
                             V(ONE, ONE, ONE));
    CHECK(h.hit && near_q(h.toi, su_q32_div(ONE, Q(10)), 2) && h.n.x == -ONE && h.n.y == 0 && h.n.z == 0);
    /* 6. Start crossing: a long segment through the box (end points outside, r 0.1): toi 0, normal of the chord
     * middle (inside: least penetration axis). */
    h = s3_sweep_capsule_obb(V(Q(5), 0, ONE / 2), V(ONE, 0, 0), ONE / 10, V(0, -Q(5), 0), V(0, Q(5), 0),
                             V(Q(5), 0, 0), QID, V(ONE, ONE, ONE));
    CHECK(h.hit && h.toi == 0 && h.n.z == ONE);
    /* 7. Moving away from the sphere (and z0 = z1 = 0 against the sphere sweep): no hit; equal results. */
    h = s3_sweep_capsule_sphere(V(-Q(10), 0, 0), V(-Q(20), 0, 0), ONE, ez0, ez1, V(0, 0, Q(3)), ONE);
    CHECK(!h.hit);
    {
        S3Hit hs = s3_sweep_sphere_sphere(V(-Q(10), 0, 0), V(Q(10), 0, 0), ONE, V(0, 0, 0), ONE);
        h = s3_sweep_capsule_sphere(V(-Q(10), 0, 0), V(Q(10), 0, 0), ONE, V(0, 0, 0), V(0, 0, 0), V(0, 0, 0), ONE);
        CHECK(h.hit && hs.hit && h.toi == hs.toi && h.n.x == hs.n.x && h.n.y == hs.n.y && h.n.z == hs.n.z);
    }
    /* 8. Core distances: crossed segments 3 apart; the segment of case 4 at x = 3 is sqrt(1 + 0.25) from the edge. */
    CHECK(near_q(s3_dist_seg_seg(V(0, 0, 0), ez0, ez1, V(Q(3), -ONE, 0), V(Q(3), ONE, 0)), Q(3), 4));
    CHECK(fabs(qd(s3_dist_seg_obb(V(Q(3), 0, Q(3) / 2), V(0, -Q(3), 0), V(0, Q(3), 0), V(Q(5), 0, 0), QID,
                                  V(ONE, ONE, ONE))) - sqrt(1.25)) < 1e-8);
    CHECK(s3_dist_seg_obb(V(Q(5), 0, 0), V(0, -Q(5), 0), V(0, Q(5), 0), V(Q(5), 0, 0), QID, V(ONE, ONE, ONE)) == 0);
}

/* Double-precision reference for a capsule mover: the core distance f(t) = dist(segment(t), core) is convex in t and
 * in the segment parameter (distance to a convex set from an affine family), so nested ternary searches give its
 * minimum exactly enough, and bisection on [0, argmin] gives the first root of f - R. */
typedef struct CapRef {
    const Shape *s;
    D3 c0, v, e0, e1;
} CapRef;

static double cap_seg_dist(const CapRef *c, double t, double *lam_out) {
    D3 base = dadd(c->c0, dsc(c->v, t));
    D3 d = dsub(c->e1, c->e0);
    double lo = 0, hi = 1, l;
    int k;
    for (k = 0; k < 80; k++) {
        double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
        double f1 = core_dist(c->s, dadd(base, dadd(c->e0, dsc(d, m1))));
        double f2 = core_dist(c->s, dadd(base, dadd(c->e0, dsc(d, m2))));
        if (f1 <= f2) {
            hi = m2;
        } else {
            lo = m1;
        }
    }
    l = 0.5 * (lo + hi);
    if (lam_out != NULL) {
        *lam_out = l;
    }
    return core_dist(c->s, dadd(base, dadd(c->e0, dsc(d, l))));
}

static double cap_oracle(const CapRef *c, double R, double *minf) {
    double lo = 0, hi = 1, tmin;
    int k;
    if (cap_seg_dist(c, 0, NULL) - R <= 0) {
        *minf = cap_seg_dist(c, 0, NULL) - R;
        return 0;
    }
    for (k = 0; k < 80; k++) {
        double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
        if (cap_seg_dist(c, m1, NULL) <= cap_seg_dist(c, m2, NULL)) {
            hi = m2;
        } else {
            lo = m1;
        }
    }
    tmin = 0.5 * (lo + hi);
    *minf = cap_seg_dist(c, tmin, NULL) - R;
    if (*minf > 0) {
        return -1;
    }
    lo = 0;
    hi = tmin;
    for (k = 0; k < 70; k++) {
        double mid = 0.5 * (lo + hi);
        if (cap_seg_dist(c, mid, NULL) - R <= 0) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

/* Closest point of the core to p (double). */
static D3 core_closest(const Shape *s, D3 p) {
    if (s->kind == 0) {
        return s->c;
    }
    if (s->kind == 1) {
        D3 d = dsub(s->b, s->a);
        double t = clampd(ddot(dsub(p, s->a), d) / ddot(d, d), 0, 1);
        return dadd(s->a, dsc(d, t));
    } else if (s->kind == 3) {
        return dtri_closest(p, s->a, s->b, s->t2);
    } else {
        D3 l = drot(s->qc, dsub(p, s->c));
        D3 cl = {clampd(l.x, -s->half.x, s->half.x), clampd(l.y, -s->half.y, s->half.y),
                 clampd(l.z, -s->half.z, s->half.z)};
        return dadd(s->c, drot(s->q, cl));
    }
}

static void test_capsule_random(const char *label, int kind, int cases) {
    int i, compared = 0, grazing = 0, hits = 0, mism = 0;
    double worst_t = 0, worst_n = 0;
    for (i = 0; i < cases; i++) {
        Shape s;
        CapRef cr;
        SuV3 c0 = V(rq(30), rq(30), rq(30)), vq, sc = V(rq(3), rq(3), rq(3)), sa, sb, half, e0, e1;
        SuQuat q = QID, qm, q0;
        int64_t r = rpos(0, 3) + ONE / 4, rs = rpos(0, 4) + ONE / 4;
        int64_t z0 = -rpos(0, 8), z1 = rpos(0, 8);
        S3Hit h;
        double minf, tref, R;
        D3 target;
        memset(&s, 0, sizeof s);
        s.kind = kind;
        q0.w = rq(1);
        q0.x = rq(1);
        q0.y = rq(1);
        q0.z = rq(1);
        qm = su_quat_normalize(q0);
        e0 = su_quat_rotate(qm, V(0, 0, z0));
        e1 = su_quat_rotate(qm, V(0, 0, z1));
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
            h = s3_sweep_capsule_sphere(c0, vq, r, e0, e1, sc, rs);
            R = qd(r) + qd(rs);
        } else if (kind == 1) {
            sa = V(rq(10), rq(10), rq(10));
            sb = V(rq(10), rq(10), rq(10));
            s.a = dv(sa);
            s.b = dv(sb);
            h = s3_sweep_capsule_capsule(c0, vq, r, e0, e1, sa, sb, rs);
            R = qd(r) + qd(rs);
        } else if (kind == 3) {
            SuV3 ta = V(rq(10), rq(10), rq(10)), tb = V(rq(10), rq(10), rq(10)), tc = V(rq(10), rq(10), rq(10));
            S3Tri t = s3_tri_make(ta, tb, tc);
            s.a = dv(ta);
            s.b = dv(tb);
            s.t2 = dv(tc);
            h = s3_sweep_capsule_tri(c0, vq, r, e0, e1, &t);
            R = qd(r);
            if (i % 4 == 0) {
                /* Core distance against the reference (segment to triangle at the start). */
                CapRef cd;
                double dref, dgot;
                cd.s = &s;
                cd.c0 = dv(c0);
                cd.v = dv(V(0, 0, 0));
                cd.e0 = dv(e0);
                cd.e1 = dv(e1);
                dref = cap_seg_dist(&cd, 0, NULL);
                dgot = qd(s3_dist_seg_tri(c0, e0, e1, &t));
                if (fabs(dref - dgot) > 1e-5) {
                    mism++;
                    if (mism <= 5) {
                        fprintf(stderr, "%s case %d: distance %.9f, reference %.9f\n", label, i, dgot, dref);
                    }
                }
            }
        } else {
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
            h = s3_sweep_capsule_obb(c0, vq, r, e0, e1, sc, q, half);
            R = qd(r);
        }
        cr.s = &s;
        cr.c0 = dv(c0);
        cr.v = dv(vq);
        cr.e0 = dv(e0);
        cr.e1 = dv(e1);
        tref = cap_oracle(&cr, R, &minf);
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
            double L = dlen(dv(vq)), lam, nerr;
            double dt = fabs(qd(h.toi) - tref) * L;
            D3 p, nref;
            double dcore = cap_seg_dist(&cr, tref, &lam);
            p = dadd(dadd(cr.c0, dsc(cr.v, tref)), dadd(cr.e0, dsc(dsub(cr.e1, cr.e0), lam)));
            nref = dsub(p, core_closest(&s, p));
            if (dt > worst_t) worst_t = dt;
            if (dt > 1e-5) {
                mism++;
                if (mism <= 5) {
                    fprintf(stderr, "%s case %d: toi err %.3g m\n", label, i, dt);
                }
                continue;
            }
            if (dcore < 1e-6 || tref == 0) {
                continue; /* cores intersect or start contact: the normal is a convention, checked by known answers */
            }
            nref = dsc(nref, 1.0 / dlen(nref));
            nerr = dlen(dsub(dv(h.n), nref));
            if (nerr > worst_n) worst_n = nerr;
            if (nerr > 1e-4) {
                mism++;
                if (mism <= 5) {
                    fprintf(stderr, "%s case %d: normal err %.3g\n", label, i, nerr);
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
    test_capsule_known();
    g_rng = UINT64_C(20261018);
    test_random("sphere_sphere", 0, 20000);
    test_random("sphere_capsule", 1, 20000);
    test_random("sphere_obb", 2, 20000);
    test_capsule_random("capsule_sphere", 0, 6000);
    test_capsule_random("capsule_capsule", 1, 6000);
    test_capsule_random("capsule_obb", 2, 6000);
    test_tri_known();
    test_random("sphere_tri", 3, 20000);
    test_capsule_random("capsule_tri", 3, 6000);
    printf("test_space3d_geom: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
