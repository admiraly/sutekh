/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d narrow phase. See space3d_geom.h. Every function is a fixed sequence of fixed_q32_v1 operations
 * (src/ir/su_num.h); no floating point. Sweeps are solved in distance along the unit direction u of the
 * displacement (length L), which keeps quadratic coefficients at length^2 scale instead of length^4. */
#include "space3d_geom.h"

#define ONE SU_Q32_ONE

static SuV3 v3(int64_t x, int64_t y, int64_t z) {
    SuV3 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static int v3_is_zero(SuV3 a) {
    return a.x == 0 && a.y == 0 && a.z == 0;
}

static int64_t v3_comp(SuV3 a, int i) {
    return i == 0 ? a.x : (i == 1 ? a.y : a.z);
}

static void v3_set(SuV3 *a, int i, int64_t v) {
    if (i == 0) {
        a->x = v;
    } else if (i == 1) {
        a->y = v;
    } else {
        a->z = v;
    }
}

/* Unit vector of a, or +z for the zero vector (a defined normal for degenerate contacts). */
static SuV3 unit_or_up(SuV3 a) {
    SuV3 n = su_v3_normalize(a);
    return v3_is_zero(n) ? v3(0, 0, ONE) : n;
}

static int64_t clamp_q(int64_t x, int64_t lo, int64_t hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* toi = s / L clamped to [0, ONE]; 0 for L <= 0. */
static int64_t toi_of(int64_t s, int64_t len) {
    if (len <= 0 || s <= 0) {
        return 0;
    }
    return clamp_q(su_q32_div(s, len), 0, ONE);
}

/* Earliest distance s in [0, len] along unit u at which |m + s u| = R (m = start minus sphere centre).
 * Touching or overlapping at the start gives s = 0. Returns 1 on a hit. */
static int ray_sphere(SuV3 m, SuV3 u, int64_t len, int64_t R, int64_t *s_out) {
    int64_t c = su_q32_sub(su_v3_dot(m, m), su_q32_mul(R, R));
    int64_t b, disc, s;
    if (c <= 0) {
        *s_out = 0;
        return 1;
    }
    if (len <= 0) {
        return 0;
    }
    b = su_v3_dot(m, u);
    if (b >= 0) {
        return 0;
    }
    disc = su_q32_sub(su_q32_mul(b, b), c);
    if (disc < 0) {
        return 0;
    }
    s = su_q32_sub(su_q32_neg(b), su_q32_sqrt(disc));
    if (s < 0) {
        s = 0;
    }
    if (s > len) {
        return 0;
    }
    *s_out = s;
    return 1;
}

/* Earliest distance s in [0, len] at which the moving point touches the capsule of the segment a-b (axis unit ax,
 * length H) with radius R. ma = start - a, mb = start - b. Cylinder first, then the two end spheres; the
 * smallest s wins (equal s keeps the earlier candidate). */
static int ray_capsule(SuV3 ma, SuV3 mb, SuV3 u, int64_t len, SuV3 ax, int64_t H, int64_t R, int64_t *s_out) {
    int found = 0;
    int64_t best = 0, s;
    int64_t m_ax = su_v3_dot(ma, ax);
    int64_t u_ax = su_v3_dot(u, ax);
    SuV3 mp = su_v3_sub(ma, su_v3_scale(ax, m_ax));
    SuV3 up = su_v3_sub(u, su_v3_scale(ax, u_ax));
    int64_t A = su_v3_dot(up, up);
    int64_t B = su_v3_dot(mp, up);
    int64_t C = su_q32_sub(su_v3_dot(mp, mp), su_q32_mul(R, R));
    if (C <= 0 && m_ax >= 0 && m_ax <= H) {
        *s_out = 0;
        return 1;
    }
    if (A > 0 && B < 0 && len > 0) {
        int64_t disc = su_q32_sub(su_q32_mul(B, B), su_q32_mul(A, C));
        if (disc >= 0) {
            s = su_q32_div(su_q32_sub(su_q32_neg(B), su_q32_sqrt(disc)), A);
            if (s < 0) {
                s = 0;
            }
            if (s <= len) {
                int64_t axial = su_q32_add(m_ax, su_q32_mul(s, u_ax));
                if (axial >= 0 && axial <= H) {
                    best = s;
                    found = 1;
                }
            }
        }
    }
    if (ray_sphere(ma, u, len, R, &s) && (!found || s < best)) {
        best = s;
        found = 1;
    }
    if (ray_sphere(mb, u, len, R, &s) && (!found || s < best)) {
        best = s;
        found = 1;
    }
    if (found) {
        *s_out = best;
    }
    return found;
}

/* Normal at the moving centre P against the segment a + k ax, k in [0, H]. */
static SuV3 capsule_normal(SuV3 P, SuV3 a, SuV3 ax, int64_t H) {
    int64_t k = clamp_q(su_v3_dot(su_v3_sub(P, a), ax), 0, H);
    return unit_or_up(su_v3_sub(P, su_v3_add(a, su_v3_scale(ax, k))));
}

S3Hit s3_sweep_sphere_sphere(SuV3 c0, SuV3 v, int64_t r, SuV3 s, int64_t rs) {
    S3Hit h;
    int64_t R = su_q32_add(r, rs);
    SuV3 m = su_v3_sub(c0, s);
    int64_t len = su_v3_len(v);
    SuV3 u = su_v3_normalize(v);
    int64_t d = 0;
    h.hit = 0;
    h.toi = 0;
    h.n = v3(0, 0, 0);
    if (!ray_sphere(m, u, len, R, &d)) {
        return h;
    }
    h.hit = 1;
    h.toi = toi_of(d, len);
    h.n = unit_or_up(su_v3_add(m, su_v3_scale(u, d)));
    return h;
}

S3Hit s3_sweep_sphere_capsule(SuV3 c0, SuV3 v, int64_t r, SuV3 a, SuV3 b, int64_t rc) {
    S3Hit h;
    SuV3 d = su_v3_sub(b, a);
    int64_t H = su_v3_len(d);
    int64_t R, len, s = 0;
    SuV3 ax, u;
    if (H == 0) {
        return s3_sweep_sphere_sphere(c0, v, r, a, rc);
    }
    h.hit = 0;
    h.toi = 0;
    h.n = v3(0, 0, 0);
    ax = su_v3_normalize(d);
    R = su_q32_add(r, rc);
    len = su_v3_len(v);
    u = su_v3_normalize(v);
    if (!ray_capsule(su_v3_sub(c0, a), su_v3_sub(c0, b), u, len, ax, H, R, &s)) {
        return h;
    }
    h.hit = 1;
    h.toi = toi_of(s, len);
    h.n = capsule_normal(su_v3_add(c0, su_v3_scale(u, s)), a, ax, H);
    return h;
}

S3Hit s3_sweep_sphere_obb(SuV3 c0, SuV3 v, int64_t r, SuV3 center, SuQuat q, SuV3 half) {
    S3Hit h;
    SuQuat qc = su_quat_conj(q);
    SuV3 p = su_quat_rotate(qc, su_v3_sub(c0, center));
    int64_t len = su_v3_len(v);
    SuV3 u = su_v3_normalize(su_quat_rotate(qc, v));
    SuV3 cl, dv, hp, nl = v3(0, 0, 0);
    int64_t tmin = 0, tmax = len, s = 0;
    int axis = -1, sgn = 0, i, outc = 0;
    int out[3];

    h.hit = 0;
    h.toi = 0;
    h.n = v3(0, 0, 0);
    /* Touching or overlapping at the start. */
    cl = v3(clamp_q(p.x, su_q32_neg(half.x), half.x), clamp_q(p.y, su_q32_neg(half.y), half.y),
            clamp_q(p.z, su_q32_neg(half.z), half.z));
    dv = su_v3_sub(p, cl);
    if (su_v3_dot(dv, dv) <= su_q32_mul(r, r)) {
        if (v3_is_zero(dv)) {
            /* Centre inside the box: the axis of least penetration, ties to the lower axis. */
            int64_t best = INT64_MAX;
            int k = 0;
            for (i = 0; i < 3; i++) {
                int64_t pen = su_q32_sub(v3_comp(half, i), su_q32_abs(v3_comp(p, i)));
                if (pen < best) {
                    best = pen;
                    k = i;
                }
            }
            v3_set(&nl, k, v3_comp(p, k) >= 0 ? ONE : -ONE);
        } else {
            nl = unit_or_up(dv);
        }
        h.hit = 1;
        h.n = unit_or_up(su_quat_rotate(q, nl));
        return h;
    }
    if (len <= 0) {
        return h;
    }
    /* Slab test against the box expanded by r. */
    for (i = 0; i < 3; i++) {
        int64_t pi = v3_comp(p, i), ui = v3_comp(u, i);
        int64_t e = su_q32_add(v3_comp(half, i), r);
        int64_t t1, t2;
        int si;
        if (ui == 0) {
            if (pi < su_q32_neg(e) || pi > e) {
                return h;
            }
            continue;
        }
        t1 = su_q32_div(su_q32_sub(su_q32_neg(e), pi), ui);
        t2 = su_q32_div(su_q32_sub(e, pi), ui);
        si = ui > 0 ? -1 : 1;
        if (t1 > t2) {
            int64_t t = t1;
            t1 = t2;
            t2 = t;
        }
        if (t1 > tmin) {
            tmin = t1;
            axis = i;
            sgn = si;
        }
        if (t2 < tmax) {
            tmax = t2;
        }
        if (tmin > tmax) {
            return h;
        }
    }
    /* Voronoi region of the entry point on the expanded box: face, edge or vertex. */
    hp = su_v3_add(p, su_v3_scale(u, tmin));
    for (i = 0; i < 3; i++) {
        int64_t c = v3_comp(hp, i), hh = v3_comp(half, i);
        out[i] = c < su_q32_neg(hh) ? -1 : (c > hh ? 1 : 0);
        outc += out[i] != 0;
    }
    if (outc <= 1) {
        s = tmin;
    } else {
        /* Edge (two axes outside) or vertex (three): capsules of radius r along the box edges at that corner. */
        SuV3 corner = v3(out[0] * half.x, out[1] * half.y, out[2] * half.z);
        int found = 0, k;
        int64_t best = 0;
        for (k = 0; k < 3; k++) {
            SuV3 a = corner, b = corner, ax = v3(0, 0, 0);
            int64_t H, sk = 0;
            if (outc == 2 && out[k] != 0) {
                continue; /* edge case: only the edge along the inside axis */
            }
            if (outc == 2) {
                v3_set(&a, k, su_q32_neg(v3_comp(half, k)));
                v3_set(&b, k, v3_comp(half, k));
            } else {
                v3_set(&b, k, su_q32_neg(v3_comp(corner, k)));
            }
            H = su_q32_sub(v3_comp(b, k), v3_comp(a, k));
            if (H < 0) {
                SuV3 t = a;
                a = b;
                b = t;
                H = su_q32_neg(H);
            }
            v3_set(&ax, k, ONE);
            if (H == 0) {
                if (!ray_sphere(su_v3_sub(p, a), u, len, r, &sk)) {
                    continue;
                }
            } else if (!ray_capsule(su_v3_sub(p, a), su_v3_sub(p, b), u, len, ax, H, r, &sk)) {
                continue;
            }
            if (!found || sk < best) {
                best = sk;
                found = 1;
            }
        }
        if (!found) {
            return h;
        }
        s = best;
    }
    /* Normal from the closest box point to the contact centre, independent of which feature produced s (a
     * near-tie between an edge and its corner sphere must not tilt the normal). A contact centre on or inside
     * the box surface after rounding falls back to the entry face. */
    hp = su_v3_add(p, su_v3_scale(u, s));
    nl = su_v3_sub(hp, v3(clamp_q(hp.x, su_q32_neg(half.x), half.x), clamp_q(hp.y, su_q32_neg(half.y), half.y),
                          clamp_q(hp.z, su_q32_neg(half.z), half.z)));
    if (v3_is_zero(nl) && axis >= 0) {
        v3_set(&nl, axis, sgn > 0 ? ONE : -ONE);
    }
    nl = unit_or_up(nl);
    h.hit = 1;
    h.toi = toi_of(s, len);
    h.n = unit_or_up(su_quat_rotate(q, nl));
    return h;
}

int s3_segment_gate(SuV3 p0, SuV3 p1, SuV3 c, SuQuat q, int64_t half_x, int64_t half_y, int64_t radius, int disc,
                    S3GateHit *out) {
    SuV3 nrm = su_quat_rotate(q, v3(0, 0, ONE));
    SuV3 xa = su_quat_rotate(q, v3(ONE, 0, 0));
    SuV3 ya = su_quat_rotate(q, v3(0, ONE, 0));
    int64_t d0 = su_v3_dot(su_v3_sub(p0, c), nrm);
    int64_t d1 = su_v3_dot(su_v3_sub(p1, c), nrm);
    int dir = 0;
    int64_t t, lx, ly;
    SuV3 rel;
    int inside;
    out->dir = 0;
    out->toi = 0;
    out->lx = 0;
    out->ly = 0;
    if (d0 < 0 && d1 >= 0) {
        dir = 1;
    } else if (d0 >= 0 && d1 < 0) {
        dir = -1;
    } else {
        return 0;
    }
    t = clamp_q(su_q32_div(d0, su_q32_sub(d0, d1)), 0, ONE);
    rel = su_v3_sub(su_v3_add(p0, su_v3_scale(su_v3_sub(p1, p0), t)), c);
    lx = su_v3_dot(rel, xa);
    ly = su_v3_dot(rel, ya);
    if (disc) {
        inside = su_q32_add(su_q32_mul(lx, lx), su_q32_mul(ly, ly)) <= su_q32_mul(radius, radius);
    } else {
        inside = su_q32_abs(lx) <= half_x && su_q32_abs(ly) <= half_y;
    }
    if (!inside) {
        return 0;
    }
    out->dir = dir;
    out->toi = t;
    out->lx = lx;
    out->ly = ly;
    return dir;
}

void s3_bounds_sphere(SuV3 c, int64_t r, SuV3 *lo, SuV3 *hi) {
    *lo = v3(su_q32_sub(c.x, r), su_q32_sub(c.y, r), su_q32_sub(c.z, r));
    *hi = v3(su_q32_add(c.x, r), su_q32_add(c.y, r), su_q32_add(c.z, r));
}

void s3_bounds_obb(SuV3 c, SuQuat q, SuV3 half, SuV3 *lo, SuV3 *hi) {
    SuV3 a0 = su_quat_rotate(q, v3(ONE, 0, 0));
    SuV3 a1 = su_quat_rotate(q, v3(0, ONE, 0));
    SuV3 a2 = su_quat_rotate(q, v3(0, 0, ONE));
    SuV3 e;
    int i;
    for (i = 0; i < 3; i++) {
        int64_t ext = su_q32_add(su_q32_add(su_q32_mul(su_q32_abs(v3_comp(a0, i)), half.x),
                                            su_q32_mul(su_q32_abs(v3_comp(a1, i)), half.y)),
                                 su_q32_mul(su_q32_abs(v3_comp(a2, i)), half.z));
        /* One raw unit of slack per axis covers the rounding of the rotated axes. */
        v3_set(&e, i, su_q32_add(ext, 1));
    }
    *lo = su_v3_sub(c, e);
    *hi = su_v3_add(c, e);
}
