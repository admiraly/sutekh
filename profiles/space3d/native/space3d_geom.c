/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d narrow phase. See space3d_geom.h. Every function is a fixed sequence of fixed_q32_v1 operations
 * (src/ir/su_num.h); no floating point. Sweeps are solved in distance along the unit direction u of the
 * displacement (length L), which keeps quadratic coefficients at length^2 scale instead of length^4. */
#include "space3d_geom.h"

#include "ir/su_num_fast.h" /* exact inline equivalents of su_num.h (tests/num/test_num_fast.c) */

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

/* s * h for a sign s in {-1, 0, +1}, as the wrapping product: -1 * INT64_MIN is INT64_MIN. A half extent is an
 * arbitrary q32 lane value, so the plain int64_t product could overflow (undefined behaviour); this is the value
 * every supported compiler produced for it (determinism matrix finding D1, tests/determinism/test_det_regress.c). */
static int64_t sign_mul(int s, int64_t h) {
    return s > 0 ? h : s < 0 ? su_i64_from_bits((uint64_t)0 - (uint64_t)h) : 0;
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
    SuV3 n = su_v3_normalizef(a);
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
    return clamp_q(su_q32_divf(s, len), 0, ONE);
}

/* Earliest distance s in [0, len] along unit u at which |m + s u| = R (m = start minus sphere centre).
 * Touching or overlapping at the start gives s = 0. Returns 1 on a hit. */
static int ray_sphere(SuV3 m, SuV3 u, int64_t len, int64_t R, int64_t *s_out) {
    int64_t c = su_q32_subf(su_v3_dotf(m, m), su_q32_mulf(R, R));
    int64_t b, disc, s;
    if (c <= 0) {
        *s_out = 0;
        return 1;
    }
    if (len <= 0) {
        return 0;
    }
    b = su_v3_dotf(m, u);
    if (b >= 0) {
        return 0;
    }
    disc = su_q32_subf(su_q32_mulf(b, b), c);
    if (disc < 0) {
        return 0;
    }
    s = su_q32_subf(su_q32_neg(b), su_q32_sqrtf(disc));
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
    int64_t m_ax = su_v3_dotf(ma, ax);
    int64_t u_ax = su_v3_dotf(u, ax);
    SuV3 mp = su_v3_subf(ma, su_v3_scalef(ax, m_ax));
    SuV3 up = su_v3_subf(u, su_v3_scalef(ax, u_ax));
    int64_t A = su_v3_dotf(up, up);
    int64_t B = su_v3_dotf(mp, up);
    int64_t C = su_q32_subf(su_v3_dotf(mp, mp), su_q32_mulf(R, R));
    if (C <= 0 && m_ax >= 0 && m_ax <= H) {
        *s_out = 0;
        return 1;
    }
    if (A > 0 && B < 0 && len > 0) {
        int64_t disc = su_q32_subf(su_q32_mulf(B, B), su_q32_mulf(A, C));
        if (disc >= 0) {
            s = su_q32_divf(su_q32_subf(su_q32_neg(B), su_q32_sqrtf(disc)), A);
            if (s < 0) {
                s = 0;
            }
            if (s <= len) {
                int64_t axial = su_q32_addf(m_ax, su_q32_mulf(s, u_ax));
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
    int64_t k = clamp_q(su_v3_dotf(su_v3_subf(P, a), ax), 0, H);
    return unit_or_up(su_v3_subf(P, su_v3_addf(a, su_v3_scalef(ax, k))));
}

S3Hit s3_sweep_sphere_sphere(SuV3 c0, SuV3 v, int64_t r, SuV3 s, int64_t rs) {
    S3Hit h;
    int64_t R = su_q32_addf(r, rs);
    SuV3 m = su_v3_subf(c0, s);
    int64_t len = su_v3_lenf(v);
    SuV3 u = su_v3_normalizef(v);
    int64_t d = 0;
    h.hit = 0;
    h.toi = 0;
    h.n = v3(0, 0, 0);
    if (!ray_sphere(m, u, len, R, &d)) {
        return h;
    }
    h.hit = 1;
    h.toi = toi_of(d, len);
    h.n = unit_or_up(su_v3_addf(m, su_v3_scalef(u, d)));
    return h;
}

S3Hit s3_sweep_sphere_capsule(SuV3 c0, SuV3 v, int64_t r, SuV3 a, SuV3 b, int64_t rc) {
    S3Hit h;
    SuV3 d = su_v3_subf(b, a);
    int64_t H = su_v3_lenf(d);
    int64_t R, len, s = 0;
    SuV3 ax, u;
    if (H == 0) {
        return s3_sweep_sphere_sphere(c0, v, r, a, rc);
    }
    h.hit = 0;
    h.toi = 0;
    h.n = v3(0, 0, 0);
    ax = su_v3_normalizef(d);
    R = su_q32_addf(r, rc);
    len = su_v3_lenf(v);
    u = su_v3_normalizef(v);
    if (!ray_capsule(su_v3_subf(c0, a), su_v3_subf(c0, b), u, len, ax, H, R, &s)) {
        return h;
    }
    h.hit = 1;
    h.toi = toi_of(s, len);
    h.n = capsule_normal(su_v3_addf(c0, su_v3_scalef(u, s)), a, ax, H);
    return h;
}

S3Hit s3_sweep_sphere_obb(SuV3 c0, SuV3 v, int64_t r, SuV3 center, SuQuat q, SuV3 half) {
    S3Hit h;
    SuQuat qc = su_quat_conjf(q);
    SuV3 p = su_quat_rotatef(qc, su_v3_subf(c0, center));
    int64_t len = su_v3_lenf(v);
    SuV3 u = su_v3_normalizef(su_quat_rotatef(qc, v));
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
    dv = su_v3_subf(p, cl);
    if (su_v3_dotf(dv, dv) <= su_q32_mulf(r, r)) {
        if (v3_is_zero(dv)) {
            /* Centre inside the box: the axis of least penetration, ties to the lower axis. */
            int64_t best = INT64_MAX;
            int k = 0;
            for (i = 0; i < 3; i++) {
                int64_t pen = su_q32_subf(v3_comp(half, i), su_q32_abs(v3_comp(p, i)));
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
        h.n = unit_or_up(su_quat_rotatef(q, nl));
        return h;
    }
    if (len <= 0) {
        return h;
    }
    /* Slab test against the box expanded by r. */
    for (i = 0; i < 3; i++) {
        int64_t pi = v3_comp(p, i), ui = v3_comp(u, i);
        int64_t e = su_q32_addf(v3_comp(half, i), r);
        int64_t t1, t2;
        int si;
        if (ui == 0) {
            if (pi < su_q32_neg(e) || pi > e) {
                return h;
            }
            continue;
        }
        t1 = su_q32_divf(su_q32_subf(su_q32_neg(e), pi), ui);
        t2 = su_q32_divf(su_q32_subf(e, pi), ui);
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
    hp = su_v3_addf(p, su_v3_scalef(u, tmin));
    for (i = 0; i < 3; i++) {
        int64_t c = v3_comp(hp, i), hh = v3_comp(half, i);
        out[i] = c < su_q32_neg(hh) ? -1 : (c > hh ? 1 : 0);
        outc += out[i] != 0;
    }
    if (outc <= 1) {
        s = tmin;
    } else {
        /* Edge (two axes outside) or vertex (three): capsules of radius r along the box edges at that corner. */
        SuV3 corner = v3(sign_mul(out[0], half.x), sign_mul(out[1], half.y), sign_mul(out[2], half.z));
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
            H = su_q32_subf(v3_comp(b, k), v3_comp(a, k));
            if (H < 0) {
                SuV3 t = a;
                a = b;
                b = t;
                H = su_q32_neg(H);
            }
            v3_set(&ax, k, ONE);
            if (H == 0) {
                if (!ray_sphere(su_v3_subf(p, a), u, len, r, &sk)) {
                    continue;
                }
            } else if (!ray_capsule(su_v3_subf(p, a), su_v3_subf(p, b), u, len, ax, H, r, &sk)) {
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
    hp = su_v3_addf(p, su_v3_scalef(u, s));
    nl = su_v3_subf(hp, v3(clamp_q(hp.x, su_q32_neg(half.x), half.x), clamp_q(hp.y, su_q32_neg(half.y), half.y),
                          clamp_q(hp.z, su_q32_neg(half.z), half.z)));
    if (v3_is_zero(nl) && axis >= 0) {
        v3_set(&nl, axis, sgn > 0 ? ONE : -ONE);
    }
    nl = unit_or_up(nl);
    h.hit = 1;
    h.toi = toi_of(s, len);
    h.n = unit_or_up(su_quat_rotatef(q, nl));
    return h;
}

/* ---------- capsule movers (2026-10-18) ----------
 * Parallelogram P = {p00 + alpha d1 + beta e : alpha, beta in [0, 1]} with p00 = a - e0, d1 = b - a, e = e0 - e1:
 * the set of offsets (obstacle core point) - (mover core offset). The mover's reference point is within R of the
 * obstacle core exactly when it is within R of P. Edges in fixed order: (p00, +d1), (p00 + e, +d1), (p00, +e),
 * (p00 + d1, +e). The face is used when d1 x e is nonzero. */
typedef struct Para {
    SuV3 p00, d1, e, n;
    int64_t den; /* |d1 x e| as dot(d1 x e, n) */
    int face;
} Para;

static Para para_make(SuV3 a, SuV3 b, SuV3 e0, SuV3 e1) {
    Para p;
    SuV3 nn;
    p.p00 = su_v3_subf(a, e0);
    p.d1 = su_v3_subf(b, a);
    p.e = su_v3_subf(e0, e1);
    nn = su_v3_crossf(p.d1, p.e);
    p.n = su_v3_normalizef(nn);
    p.den = su_v3_dotf(nn, p.n);
    p.face = !v3_is_zero(p.n) && p.den > 0;
    return p;
}

static void para_edge(const Para *p, int k, SuV3 *pa, SuV3 *pb) {
    SuV3 o = k == 1 ? su_v3_addf(p->p00, p->e) : (k == 3 ? su_v3_addf(p->p00, p->d1) : p->p00);
    *pa = o;
    *pb = su_v3_addf(o, k < 2 ? p->d1 : p->e);
}

/* w = point - p00: inside the parallelogram's prism (alpha, beta in [0, ONE]). */
static int para_inside(const Para *p, SuV3 w) {
    int64_t al = su_q32_divf(su_v3_dotf(su_v3_crossf(w, p->e), p->n), p->den);
    int64_t be = su_q32_divf(su_v3_dotf(su_v3_crossf(p->d1, w), p->n), p->den);
    return al >= 0 && al <= ONE && be >= 0 && be <= ONE;
}

/* Earliest s in [0, len] at which c0 + s u is within R of the segment pa-pb. */
static int ray_seg(SuV3 c0, SuV3 u, int64_t len, SuV3 pa, SuV3 pb, int64_t R, int64_t *s) {
    SuV3 d = su_v3_subf(pb, pa);
    int64_t H = su_v3_lenf(d);
    if (H == 0) {
        return ray_sphere(su_v3_subf(c0, pa), u, len, R, s);
    }
    return ray_capsule(su_v3_subf(c0, pa), su_v3_subf(c0, pb), u, len, su_v3_normalizef(d), H, R, s);
}

/* Earliest s in [0, len] at which c0 + s u is within R of the parallelogram: face slab first, then the four edges;
 * the smallest s wins (equal s keeps the earlier feature). */
static int ray_para(const Para *p, SuV3 c0, SuV3 u, int64_t len, int64_t R, int64_t *s_out) {
    int found = 0, k;
    int64_t best = 0, s = 0;
    if (p->face) {
        SuV3 w0 = su_v3_subf(c0, p->p00);
        int64_t h0 = su_v3_dotf(w0, p->n), dn = su_v3_dotf(u, p->n);
        int64_t nR = su_q32_neg(R);
        if (h0 >= nR && h0 <= R) {
            if (para_inside(p, w0)) {
                *s_out = 0;
                return 1;
            }
        } else if (len > 0 && ((h0 > R && dn < 0) || (h0 < nR && dn > 0))) {
            s = h0 > R ? su_q32_divf(su_q32_subf(h0, R), su_q32_neg(dn)) : su_q32_divf(su_q32_subf(nR, h0), dn);
            if (s < 0) {
                s = 0;
            }
            if (s <= len && para_inside(p, su_v3_addf(w0, su_v3_scalef(u, s)))) {
                best = s;
                found = 1;
            }
        }
    }
    for (k = 0; k < 4; k++) {
        SuV3 pa, pb;
        para_edge(p, k, &pa, &pb);
        if (ray_seg(c0, u, len, pa, pb, R, &s) && (!found || s < best)) {
            best = s;
            found = 1;
        }
    }
    if (found) {
        *s_out = best;
    }
    return found;
}

/* P minus its closest point on the parallelogram (face first, then the edges; the smallest squared length wins,
 * ties to the earlier feature). */
static SuV3 para_offset(const Para *p, SuV3 P) {
    SuV3 best = v3(0, 0, 0);
    int64_t bd = 0;
    int found = 0, k;
    if (p->face) {
        SuV3 w = su_v3_subf(P, p->p00);
        if (para_inside(p, w)) {
            best = su_v3_scalef(p->n, su_v3_dotf(w, p->n));
            bd = su_v3_dotf(best, best);
            found = 1;
        }
    }
    for (k = 0; k < 4; k++) {
        SuV3 pa, pb, d, off;
        int64_t H, dd;
        para_edge(p, k, &pa, &pb);
        d = su_v3_subf(pb, pa);
        H = su_v3_lenf(d);
        if (H == 0) {
            off = su_v3_subf(P, pa);
        } else {
            SuV3 ax = su_v3_normalizef(d);
            int64_t t = clamp_q(su_v3_dotf(su_v3_subf(P, pa), ax), 0, H);
            off = su_v3_subf(P, su_v3_addf(pa, su_v3_scalef(ax, t)));
        }
        dd = su_v3_dotf(off, off);
        if (!found || dd < bd) {
            best = off;
            bd = dd;
            found = 1;
        }
    }
    return best;
}

S3Hit s3_sweep_capsule_capsule(SuV3 c0, SuV3 v, int64_t r, SuV3 e0, SuV3 e1, SuV3 a, SuV3 b, int64_t rc) {
    S3Hit h;
    Para p = para_make(a, b, e0, e1);
    int64_t len = su_v3_lenf(v), s = 0;
    SuV3 u = su_v3_normalizef(v);
    h.hit = 0;
    h.toi = 0;
    h.n = v3(0, 0, 0);
    if (!ray_para(&p, c0, u, len, su_q32_addf(r, rc), &s)) {
        return h;
    }
    h.hit = 1;
    h.toi = toi_of(s, len);
    h.n = unit_or_up(para_offset(&p, su_v3_addf(c0, su_v3_scalef(u, s))));
    return h;
}

S3Hit s3_sweep_capsule_sphere(SuV3 c0, SuV3 v, int64_t r, SuV3 e0, SuV3 e1, SuV3 s, int64_t rs) {
    return s3_sweep_capsule_capsule(c0, v, r, e0, e1, s, s, rs);
}

/* Box corner (sx, sy, sz in {-1, +1}) in world space. */
static SuV3 box_corner(SuV3 center, SuQuat q, SuV3 half, int sx, int sy, int sz) {
    return su_v3_addf(center,
                      su_quat_rotatef(q, v3(sign_mul(sx, half.x), sign_mul(sy, half.y), sign_mul(sz, half.z))));
}

/* Edge j (0..11) of the box: axis j / 4, the other two signs from j % 4. */
static void box_edge(SuV3 center, SuQuat q, SuV3 half, int j, SuV3 *pa, SuV3 *pb) {
    int ax = j / 4, s1 = (j & 1) ? 1 : -1, s2 = (j & 2) ? 1 : -1;
    int sg[3];
    sg[ax] = -1;
    sg[(ax + 1) % 3] = s1;
    sg[(ax + 2) % 3] = s2;
    *pa = box_corner(center, q, half, sg[0], sg[1], sg[2]);
    sg[ax] = 1;
    *pb = box_corner(center, q, half, sg[0], sg[1], sg[2]);
}

/* Slab clip of the segment A-B against the box (box frame). Returns 1 when they intersect and the chord's mid
 * parameter in *tm (q32 in [0, ONE]). */
static int seg_box_clip(SuV3 A, SuV3 B, SuV3 center, SuQuat q, SuV3 half, int64_t *tm) {
    SuQuat qc = su_quat_conjf(q);
    SuV3 pa = su_quat_rotatef(qc, su_v3_subf(A, center));
    SuV3 d = su_quat_rotatef(qc, su_v3_subf(B, A));
    int64_t t0 = 0, t1 = ONE;
    int i;
    for (i = 0; i < 3; i++) {
        int64_t p = v3_comp(pa, i), di = v3_comp(d, i), hh = v3_comp(half, i);
        int64_t ta, tb;
        if (di == 0) {
            if (p < su_q32_neg(hh) || p > hh) {
                return 0;
            }
            continue;
        }
        ta = su_q32_divf(su_q32_subf(su_q32_neg(hh), p), di);
        tb = su_q32_divf(su_q32_subf(hh, p), di);
        if (ta > tb) {
            int64_t t = ta;
            ta = tb;
            tb = t;
        }
        t0 = ta > t0 ? ta : t0;
        t1 = tb < t1 ? tb : t1;
        if (t0 > t1) {
            return 0;
        }
    }
    *tm = su_q32_addf(t0, su_q32_subf(t1, t0) / 2);
    return 1;
}

S3Hit s3_sweep_capsule_obb(SuV3 c0, SuV3 v, int64_t r, SuV3 e0, SuV3 e1, SuV3 center, SuQuat q, SuV3 half) {
    S3Hit h, hk;
    SuV3 A = su_v3_addf(c0, e0), B = su_v3_addf(c0, e1), u;
    int64_t tm = 0, len, s = 0;
    int j;
    if (seg_box_clip(A, B, center, q, half, &tm)) {
        SuV3 mid = su_v3_addf(A, su_v3_scalef(su_v3_subf(B, A), tm));
        h = s3_sweep_sphere_obb(mid, v3(0, 0, 0), r, center, q, half);
        h.hit = 1;
        h.toi = 0;
        return h;
    }
    h = s3_sweep_sphere_obb(A, v, r, center, q, half);
    hk = s3_sweep_sphere_obb(B, v, r, center, q, half);
    if (hk.hit && (!h.hit || hk.toi < h.toi)) {
        h = hk;
    }
    len = su_v3_lenf(v);
    u = su_v3_normalizef(v);
    for (j = 0; j < 12; j++) {
        SuV3 pa, pb;
        Para p;
        int64_t toi;
        box_edge(center, q, half, j, &pa, &pb);
        p = para_make(pa, pb, e0, e1);
        if (!ray_para(&p, c0, u, len, r, &s)) {
            continue;
        }
        toi = toi_of(s, len);
        if (!h.hit || toi < h.toi) {
            h.hit = 1;
            h.toi = toi;
            h.n = unit_or_up(para_offset(&p, su_v3_addf(c0, su_v3_scalef(u, s))));
        }
    }
    return h;
}

int64_t s3_dist_seg_seg(SuV3 p, SuV3 e0, SuV3 e1, SuV3 a, SuV3 b) {
    Para pp = para_make(a, b, e0, e1);
    return su_v3_lenf(para_offset(&pp, p));
}

static int64_t box_point_dist(SuV3 P, SuV3 center, SuQuat q, SuV3 half) {
    SuV3 l = su_quat_rotatef(su_quat_conjf(q), su_v3_subf(P, center));
    SuV3 cl = v3(clamp_q(l.x, su_q32_neg(half.x), half.x), clamp_q(l.y, su_q32_neg(half.y), half.y),
                 clamp_q(l.z, su_q32_neg(half.z), half.z));
    return su_v3_lenf(su_v3_subf(l, cl));
}

int64_t s3_dist_seg_obb(SuV3 p, SuV3 e0, SuV3 e1, SuV3 center, SuQuat q, SuV3 half) {
    SuV3 A = su_v3_addf(p, e0), B = su_v3_addf(p, e1);
    int64_t tm = 0, d, dk;
    int j;
    if (seg_box_clip(A, B, center, q, half, &tm)) {
        return 0;
    }
    d = box_point_dist(A, center, q, half);
    dk = box_point_dist(B, center, q, half);
    d = dk < d ? dk : d;
    for (j = 0; j < 12; j++) {
        SuV3 pa, pb;
        box_edge(center, q, half, j, &pa, &pb);
        dk = s3_dist_seg_seg(p, e0, e1, pa, pb);
        d = dk < d ? dk : d;
    }
    return d;
}

int s3_segment_gate(SuV3 p0, SuV3 p1, SuV3 c, SuQuat q, int64_t half_x, int64_t half_y, int64_t radius, int shape,
                    S3GateHit *out) {
    /* The in-plane axes are needed only after a side change: computed below (pure, so the order is free). */
    SuV3 nrm = su_quat_rotatef(q, v3(0, 0, ONE));
    SuV3 xa, ya;
    int64_t d0 = su_v3_dotf(su_v3_subf(p0, c), nrm);
    int64_t d1 = su_v3_dotf(su_v3_subf(p1, c), nrm);
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
    xa = su_quat_rotatef(q, v3(ONE, 0, 0));
    ya = su_quat_rotatef(q, v3(0, ONE, 0));
    t = clamp_q(su_q32_divf(d0, su_q32_subf(d0, d1)), 0, ONE);
    rel = su_v3_subf(su_v3_addf(p0, su_v3_scalef(su_v3_subf(p1, p0), t)), c);
    lx = su_v3_dotf(rel, xa);
    ly = su_v3_dotf(rel, ya);
    if (shape == S3_GATE_DISC) {
        inside = su_q32_addf(su_q32_mulf(lx, lx), su_q32_mulf(ly, ly)) <= su_q32_mulf(radius, radius);
    } else if (shape == S3_GATE_ELLIPSE) {
        if (half_x <= 0 || half_y <= 0) {
            inside = 0;
        } else {
            int64_t u = su_q32_divf(lx, half_x), v = su_q32_divf(ly, half_y);
            inside = su_q32_addf(su_q32_mulf(u, u), su_q32_mulf(v, v)) <= ONE;
        }
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
    *lo = v3(su_q32_subf(c.x, r), su_q32_subf(c.y, r), su_q32_subf(c.z, r));
    *hi = v3(su_q32_addf(c.x, r), su_q32_addf(c.y, r), su_q32_addf(c.z, r));
}

void s3_bounds_obb(SuV3 c, SuQuat q, SuV3 half, SuV3 *lo, SuV3 *hi) {
    SuV3 a0 = su_quat_rotatef(q, v3(ONE, 0, 0));
    SuV3 a1 = su_quat_rotatef(q, v3(0, ONE, 0));
    SuV3 a2 = su_quat_rotatef(q, v3(0, 0, ONE));
    SuV3 e;
    int i;
    for (i = 0; i < 3; i++) {
        int64_t ext = su_q32_addf(su_q32_addf(su_q32_mulf(su_q32_abs(v3_comp(a0, i)), half.x),
                                            su_q32_mulf(su_q32_abs(v3_comp(a1, i)), half.y)),
                                 su_q32_mulf(su_q32_abs(v3_comp(a2, i)), half.z));
        /* One raw unit of slack per axis covers the rounding of the rotated axes. */
        v3_set(&e, i, su_q32_addf(ext, 1));
    }
    *lo = su_v3_subf(c, e);
    *hi = su_v3_addf(c, e);
}

/* ---------- triangles (static meshes, 2026-10-20) ---------- */
S3Tri s3_tri_make(SuV3 a, SuV3 b, SuV3 c) {
    S3Tri t;
    t.a = a;
    t.b = b;
    t.c = c;
    t.n = su_v3_normalizef(su_v3_crossf(su_v3_subf(b, a), su_v3_subf(c, a)));
    return t;
}

/* The projection of p along n lies in the closed triangle (each edge function (q - p0) x (p - p0) . n >= 0).
 * Always 0 for a degenerate triangle (zero normal). */
static int tri_inside(const S3Tri *t, SuV3 p) {
    if (v3_is_zero(t->n)) {
        return 0;
    }
    if (su_v3_dotf(su_v3_crossf(su_v3_subf(t->b, t->a), su_v3_subf(p, t->a)), t->n) < 0) {
        return 0;
    }
    if (su_v3_dotf(su_v3_crossf(su_v3_subf(t->c, t->b), su_v3_subf(p, t->b)), t->n) < 0) {
        return 0;
    }
    return su_v3_dotf(su_v3_crossf(su_v3_subf(t->a, t->c), su_v3_subf(p, t->c)), t->n) >= 0;
}

static void tri_edge(const S3Tri *t, int k, SuV3 *pa, SuV3 *pb) {
    *pa = k == 0 ? t->a : (k == 1 ? t->b : t->c);
    *pb = k == 0 ? t->b : (k == 1 ? t->c : t->a);
}

/* P minus its closest point on the segment pa-pb. */
static SuV3 seg_offset(SuV3 P, SuV3 pa, SuV3 pb) {
    SuV3 d = su_v3_subf(pb, pa);
    int64_t H = su_v3_lenf(d);
    SuV3 ax;
    int64_t k;
    if (H == 0) {
        return su_v3_subf(P, pa);
    }
    ax = su_v3_normalizef(d);
    k = clamp_q(su_v3_dotf(su_v3_subf(P, pa), ax), 0, H);
    return su_v3_subf(P, su_v3_addf(pa, su_v3_scalef(ax, k)));
}

SuV3 s3_tri_offset(const S3Tri *t, SuV3 P) {
    SuV3 best = v3(0, 0, 0);
    int64_t bd = 0;
    int k;
    if (tri_inside(t, P)) {
        return su_v3_scalef(t->n, su_v3_dotf(su_v3_subf(P, t->a), t->n));
    }
    for (k = 0; k < 3; k++) {
        SuV3 pa, pb, off;
        int64_t dd;
        tri_edge(t, k, &pa, &pb);
        off = seg_offset(P, pa, pb);
        dd = su_v3_dotf(off, off);
        if (k == 0 || dd < bd) {
            best = off;
            bd = dd;
        }
    }
    return best;
}

/* Unit normal from an offset (triangle point -> P); when it is zero, the face normal on P's side (+n on the
 * plane), or +z for a degenerate triangle. */
static SuV3 tri_normal(const S3Tri *t, SuV3 off, SuV3 P) {
    SuV3 n = su_v3_normalizef(off);
    if (!v3_is_zero(n)) {
        return n;
    }
    if (v3_is_zero(t->n)) {
        return v3(0, 0, ONE);
    }
    return su_v3_dotf(su_v3_subf(P, t->a), t->n) >= 0 ? t->n : su_v3_scalef(t->n, -ONE);
}

/* Earliest s in [0, len] at which c0 + s u is within R of the triangle: face slab first, then the three edges;
 * the smallest s wins (equal s keeps the earlier feature). */
static int ray_tri(const S3Tri *t, SuV3 c0, SuV3 u, int64_t len, int64_t R, int64_t *s_out) {
    int found = 0, k;
    int64_t best = 0, s = 0;
    if (!v3_is_zero(t->n)) {
        int64_t h0 = su_v3_dotf(su_v3_subf(c0, t->a), t->n), dn = su_v3_dotf(u, t->n);
        int64_t nR = su_q32_neg(R);
        if (h0 >= nR && h0 <= R) {
            if (tri_inside(t, c0)) {
                *s_out = 0;
                return 1;
            }
        } else if (len > 0 && ((h0 > R && dn < 0) || (h0 < nR && dn > 0))) {
            s = h0 > R ? su_q32_divf(su_q32_subf(h0, R), su_q32_neg(dn)) : su_q32_divf(su_q32_subf(nR, h0), dn);
            if (s < 0) {
                s = 0;
            }
            if (s <= len && tri_inside(t, su_v3_addf(c0, su_v3_scalef(u, s)))) {
                best = s;
                found = 1;
            }
        }
    }
    for (k = 0; k < 3; k++) {
        SuV3 pa, pb;
        tri_edge(t, k, &pa, &pb);
        if (ray_seg(c0, u, len, pa, pb, R, &s) && (!found || s < best)) {
            best = s;
            found = 1;
        }
    }
    if (found) {
        *s_out = best;
    }
    return found;
}

S3Hit s3_sweep_sphere_tri(SuV3 c0, SuV3 v, int64_t r, const S3Tri *t) {
    S3Hit h;
    int64_t len = su_v3_lenf(v), s = 0;
    SuV3 u = su_v3_normalizef(v), pc;
    h.hit = 0;
    h.toi = 0;
    h.n = v3(0, 0, 0);
    if (!ray_tri(t, c0, u, len, r, &s)) {
        return h;
    }
    pc = su_v3_addf(c0, su_v3_scalef(u, s));
    h.hit = 1;
    h.toi = toi_of(s, len);
    h.n = tri_normal(t, s3_tri_offset(t, pc), pc);
    return h;
}

/* The segment A-B crosses the (non-degenerate) triangle: its end points are on opposite sides of the plane, or one
 * is on it, and the crossing point is inside. A segment in the plane does not count (its contacts are the end
 * spheres' and the edges'). */
static int seg_tri_cross(const S3Tri *t, SuV3 A, SuV3 B) {
    int64_t d0, d1, tt;
    if (v3_is_zero(t->n)) {
        return 0;
    }
    d0 = su_v3_dotf(su_v3_subf(A, t->a), t->n);
    d1 = su_v3_dotf(su_v3_subf(B, t->a), t->n);
    if ((d0 > 0 && d1 > 0) || (d0 < 0 && d1 < 0) || d0 == d1) {
        return 0;
    }
    tt = clamp_q(su_q32_divf(d0, su_q32_subf(d0, d1)), 0, ONE);
    return tri_inside(t, su_v3_addf(A, su_v3_scalef(su_v3_subf(B, A), tt)));
}

S3Hit s3_sweep_capsule_tri(SuV3 c0, SuV3 v, int64_t r, SuV3 e0, SuV3 e1, const S3Tri *t) {
    S3Hit h, hk;
    SuV3 A = su_v3_addf(c0, e0), B = su_v3_addf(c0, e1), u;
    int64_t len, s = 0;
    int k;
    if (seg_tri_cross(t, A, B)) {
        h.hit = 1;
        h.toi = 0;
        h.n = tri_normal(t, v3(0, 0, 0), c0);
        return h;
    }
    h = s3_sweep_sphere_tri(A, v, r, t);
    hk = s3_sweep_sphere_tri(B, v, r, t);
    if (hk.hit && (!h.hit || hk.toi < h.toi)) {
        h = hk;
    }
    len = su_v3_lenf(v);
    u = su_v3_normalizef(v);
    for (k = 0; k < 3; k++) {
        SuV3 pa, pb;
        Para p;
        int64_t toi;
        tri_edge(t, k, &pa, &pb);
        p = para_make(pa, pb, e0, e1);
        if (!ray_para(&p, c0, u, len, r, &s)) {
            continue;
        }
        toi = toi_of(s, len);
        if (!h.hit || toi < h.toi) {
            SuV3 pc = su_v3_addf(c0, su_v3_scalef(u, s));
            h.hit = 1;
            h.toi = toi;
            h.n = tri_normal(t, para_offset(&p, pc), pc);
        }
    }
    return h;
}

int64_t s3_dist_seg_tri(SuV3 p, SuV3 e0, SuV3 e1, const S3Tri *t) {
    SuV3 A = su_v3_addf(p, e0), B = su_v3_addf(p, e1);
    int64_t d, dk;
    int k;
    if (seg_tri_cross(t, A, B)) {
        return 0;
    }
    d = su_v3_lenf(s3_tri_offset(t, A));
    if (e0.x == e1.x && e0.y == e1.y && e0.z == e1.z) {
        return d;
    }
    dk = su_v3_lenf(s3_tri_offset(t, B));
    d = dk < d ? dk : d;
    for (k = 0; k < 3; k++) {
        SuV3 pa, pb;
        tri_edge(t, k, &pa, &pb);
        dk = s3_dist_seg_seg(p, e0, e1, pa, pb);
        d = dk < d ? dk : d;
    }
    return d;
}
