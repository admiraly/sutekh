/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d static meshes (profiles/space3d/native/space3d_mesh.c):
 *   - blob validation: every malformed table is refused with SU_E_PARSE / SU_E_RESOURCE_LIMIT and a reason, for
 *     S3MESH01 and for S3MESH02 (compound shapes; accessors and reach);
 *   - s3mesh_sweep_lim against the unlimited sweep (in the culled-vs-brute cases);
 *   - a box made of 12 triangles answers like the analytic oriented box (s3_sweep_*_obb) for sphere and capsule
 *     movers starting outside it, at random poses and part scales (toi within 1e-5 m, normals within 1e-4);
 *   - culled traversal equals brute force (every part, every triangle) bit for bit on random triangle soups with
 *     several rotated, scaled parts: sweeps (sphere and capsule movers) and distances;
 *   - no tunnelling: zero-thickness walls at 1820 m/s (15.17 m per 120 Hz tick) against small spheres and a
 *     ship-sized capsule, from random offsets and angles: every crossing reports the first contact.
 * Floating point appears only in this test (references); the engine is integer. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"
#include "s3mesh_blob.h"
#include "space3d_geom.h"
#include "space3d_mesh.h"
#include "sutekh/status.h"

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

static const SuQuat QID = {ONE, 0, 0, 0};

static SuV3 V(int64_t x, int64_t y, int64_t z) {
    SuV3 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static double qd(int64_t q) {
    return (double)q / 4294967296.0;
}

static uint64_t g_rng = 1;
static uint64_t rng(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static int64_t rq(int64_t span) { /* uniform q32 in [-span, span] m */
    uint64_t r = rng() % (uint64_t)(2 * span * ONE + 1);
    return (int64_t)r - span * ONE;
}

static int64_t rpos(int64_t lo, int64_t hi) { /* q32 in [lo, hi] m */
    return lo * ONE + (int64_t)(rng() % (uint64_t)((hi - lo) * ONE + 1));
}

static SuQuat rquat(void) {
    SuQuat q0;
    q0.w = rq(1);
    q0.x = rq(1);
    q0.y = rq(1);
    q0.z = rq(1);
    return su_quat_normalize(q0);
}

static S3MeshSet *make_set(const TGeom *g, uint32_t ng, const TPart *p, uint32_t np, const TMesh *m, uint32_t nm) {
    TBlob b;
    S3MeshSet *s = NULL;
    char why[200];
    uint32_t st;
    tb_build(&b, g, ng, p, np, m, nm);
    st = s3mesh_set_create(b.p, b.n, &s, why, sizeof why);
    if (st != SU_OK) {
        fprintf(stderr, "make_set: %u %s\n", (unsigned)st, why);
    }
    free(b.p);
    return s;
}

/* ---------- blob validation ---------- */

static uint32_t try_blob(const TBlob *b, char *why, size_t cap) {
    S3MeshSet *s = NULL;
    uint32_t st = s3mesh_set_create(b->p, b->n, &s, why, cap);
    s3mesh_set_release(s);
    return st;
}

static void test_blob(void) {
    SuV3 v[3];
    uint32_t t[3] = {0, 1, 2}, st;
    TGeom g;
    TPart p;
    TMesh m;
    TBlob b;
    char why[200];
    S3MeshSet *s = NULL;
    v[0] = V(0, 0, 0);
    v[1] = V(Q(10), 0, 0);
    v[2] = V(0, Q(10), 0);
    g.nv = 3;
    g.nt = 1;
    g.v = v;
    g.t = t;
    p.geom = 0;
    p.pos = V(0, 0, 0);
    p.rot = QID;
    p.scale = ONE;
    m.first = 0;
    m.count = 1;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(s3mesh_set_create(b.p, b.n, &s, why, sizeof why) == SU_OK && s != NULL);
    CHECK(s3mesh_count(s) == 1u && s3mesh_valid(s, 0) && !s3mesh_valid(s, 1) && s3mesh_triangles(s) == 1u);
    s3mesh_set_release(s);
    /* Truncated (every prefix), trailing byte, bad magic. */
    {
        size_t n = b.n, k;
        int all = 1;
        for (k = 0; k < n; k++) {
            TBlob c = b;
            c.n = k;
            all &= try_blob(&c, why, sizeof why) != SU_OK;
        }
        CHECK(all);
        b.p[0] = 'X';
        CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE && strstr(why, "magic") != NULL);
        b.p[0] = 'S';
        tb_u32(&b, 0);
        CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
        free(b.p);
    }
    /* Vertex index out of range. */
    t[2] = 3;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE && strstr(why, "index") != NULL);
    free(b.p);
    t[2] = 2;
    /* Coordinate beyond 2^20 m. */
    v[1].x = (INT64_C(1) << 52) + 1;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    v[1].x = Q(10);
    /* Part: geometry index, scale 0, non-unit rotation. */
    p.geom = 1;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    p.geom = 0;
    p.scale = 0;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    p.scale = ONE;
    p.rot.w = ONE + ONE / 100;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    p.rot = QID;
    /* Mesh part range beyond the table; geometry without triangles. */
    m.count = 2;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    m.count = 1;
    g.nt = 0;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    g.nt = 1;
    /* An empty mesh is valid data and never collides. */
    m.count = 0;
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    st = s3mesh_set_create(b.p, b.n, &s, why, sizeof why);
    CHECK(st == SU_OK && !s3mesh_valid(s, 0));
    CHECK(!s3mesh_sweep(s, 0, V(0, 0, 0), QID, V(0, 0, Q(5)), V(0, 0, -Q(10)), ONE, V(0, 0, 0), V(0, 0, 0), NULL).hit);
    s3mesh_set_release(s);
    free(b.p);
}

/* ---------- S3MESH02: compound table (2026-10-20) ---------- */

static void test_blob2(void) {
    SuV3 v[3];
    uint32_t t[3] = {0, 1, 2}, n = 0;
    TGeom g;
    TPart p;
    TMesh m, c[2];
    TSub u[3];
    TBlob b;
    char why[200];
    S3MeshSet *s = NULL;
    const S3SubShape *sh;
    v[0] = V(0, 0, 0);
    v[1] = V(Q(10), 0, 0);
    v[2] = V(0, Q(10), 0);
    g.nv = 3;
    g.nt = 1;
    g.v = v;
    g.t = t;
    p.geom = 0;
    p.pos = V(0, 0, 0);
    p.rot = QID;
    p.scale = ONE;
    m.first = 0;
    m.count = 1;
    u[0].a = V(0, 0, -Q(8));
    u[0].b = V(0, 0, Q(8));
    u[0].r = Q(2);
    u[1].a = V(Q(3), Q(4), 0); /* a sphere: a == b, reach 5 */
    u[1].b = u[1].a;
    u[1].r = ONE;
    u[2].a = V(-Q(6), 0, 0);
    u[2].b = V(Q(6), 0, Q(1));
    u[2].r = ONE / 2;
    c[0].first = 0;
    c[0].count = 2;
    c[1].first = 1;
    c[1].count = 2;
    tb_build2(&b, &g, 1, &p, 1, &m, 1, u, 3, c, 2);
    CHECK(s3mesh_set_create(b.p, b.n, &s, why, sizeof why) == SU_OK && s != NULL);
    CHECK(s3mesh_count(s) == 1u && s3mesh_valid(s, 0) && s3mesh_compound_count(s) == 2u);
    sh = s3mesh_compound(s, 0, &n);
    CHECK(sh != NULL && n == 2u && sh[0].r == Q(2) && sh[0].reach >= Q(8) && sh[0].reach <= Q(8) + 2);
    CHECK(sh != NULL && sh[1].reach >= Q(5) && sh[1].reach <= Q(5) + 2);
    sh = s3mesh_compound(s, 1, &n);
    CHECK(sh != NULL && n == 2u && sh[1].r == ONE / 2);
    CHECK(s3mesh_compound(s, 2, &n) == NULL && n == 0u);
    CHECK(s3mesh_compound(NULL, 0, &n) == NULL && n == 0u && s3mesh_compound_count(NULL) == 0u);
    s3mesh_set_release(s);
    /* Every truncated prefix, a trailing byte. */
    {
        size_t k;
        int all = 1;
        for (k = 0; k < b.n; k++) {
            TBlob x = b;
            x.n = k;
            all &= try_blob(&x, why, sizeof why) != SU_OK;
        }
        CHECK(all);
        tb_u32(&b, 0);
        CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
        free(b.p);
    }
    /* A compound with 0 or 9 sub-shapes, or a range beyond the table. */
    c[1].count = 0;
    tb_build2(&b, &g, 1, &p, 1, &m, 1, u, 3, c, 2);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE && strstr(why, "compound") != NULL);
    free(b.p);
    c[1].count = 9;
    tb_build2(&b, &g, 1, &p, 1, &m, 1, u, 3, c, 2);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    c[1].count = 3;
    tb_build2(&b, &g, 1, &p, 1, &m, 1, u, 3, c, 2);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    c[1].count = 2;
    /* Coordinate beyond 4096 m, negative or too large radius. */
    u[2].b.y = S3MESH_COMPOUND_COORD + 1;
    tb_build2(&b, &g, 1, &p, 1, &m, 1, u, 3, c, 2);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE && strstr(why, "sub-shape") != NULL);
    free(b.p);
    u[2].b.y = 0;
    u[2].r = -1;
    tb_build2(&b, &g, 1, &p, 1, &m, 1, u, 3, c, 2);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    u[2].r = S3MESH_COMPOUND_COORD + 1;
    tb_build2(&b, &g, 1, &p, 1, &m, 1, u, 3, c, 2);
    CHECK(try_blob(&b, why, sizeof why) == SU_E_PARSE);
    free(b.p);
    u[2].r = ONE / 2;
    /* Compounds only (no geometry, parts or meshes); an S3MESH01 set has no compounds. */
    tb_build2(&b, &g, 0, &p, 0, &m, 0, u, 3, c, 2);
    CHECK(s3mesh_set_create(b.p, b.n, &s, why, sizeof why) == SU_OK && s3mesh_count(s) == 0u &&
          s3mesh_compound_count(s) == 2u);
    s3mesh_set_release(s);
    free(b.p);
    tb_build(&b, &g, 1, &p, 1, &m, 1);
    CHECK(s3mesh_set_create(b.p, b.n, &s, why, sizeof why) == SU_OK && s3mesh_compound_count(s) == 0u &&
          s3mesh_compound(s, 0, &n) == NULL);
    s3mesh_set_release(s);
    free(b.p);
}

/* ---------- box made of triangles vs the analytic box ---------- */

static void test_box(void) {
    int i, hits = 0, compared = 0, mism = 0;
    double worst_t = 0, worst_n = 0;
    for (i = 0; i < 6000; i++) {
        SuV3 half = V(rpos(1, 12), rpos(1, 12), rpos(1, 12)), verts[8], pos = V(rq(5), rq(5), rq(5)), c0, v, e0, e1;
        uint32_t tris[36];
        SuQuat rot = rquat(), prot = (i % 3 == 0) ? QID : rquat(), mrot;
        int64_t scale = (i % 2 == 0) ? ONE : rpos(0, 3) + ONE / 2, r = rpos(0, 3) + ONE / 4;
        SuV3 gh = (scale == ONE) ? half : V(su_q32_div(half.x, scale), su_q32_div(half.y, scale),
                                             su_q32_div(half.z, scale));
        SuV3 ppos = V(rq(3), rq(3), rq(3));
        TGeom g;
        TPart p;
        TMesh m;
        S3MeshSet *s;
        S3Hit hm, hb;
        int cap = (i & 1) != 0;
        SuV3 bh, bc;
        tbox_verts(gh, verts);
        tbox_tris(tris);
        g.nv = 8;
        g.nt = 12;
        g.v = verts;
        g.t = tris;
        p.geom = 0;
        p.pos = ppos;
        p.rot = prot;
        p.scale = scale;
        m.first = 0;
        m.count = 1;
        s = make_set(&g, 1, &p, 1, &m, 1);
        CHECK(s != NULL);
        if (s == NULL) {
            continue;
        }
        /* The analytic equivalent: box of half gh * scale at pos + rot * ppos, orientation rot * prot. */
        bh = scale == ONE ? gh : su_v3_scale(gh, scale);
        bc = su_v3_add(pos, su_quat_rotate(rot, ppos));
        mrot = su_quat_mul(rot, prot);
        c0 = V(rq(40), rq(40), rq(40));
        v = V(rq(40), rq(40), rq(40));
        if (rng() & 1) {
            v = su_v3_sub(bc, c0);
            v = su_v3_scale(v, ONE + ONE / 2);
        }
        e0 = cap ? su_quat_rotate(rquat(), V(0, 0, -rpos(0, 8))) : V(0, 0, 0);
        e1 = cap ? su_v3_neg(e0) : V(0, 0, 0);
        if (cap) {
            e1 = su_v3_add(e1, V(rq(2), rq(2), rq(2)));
        }
        /* Start well outside (the mesh is a surface, the analytic box a solid). */
        if ((cap ? s3_dist_seg_obb(c0, e0, e1, bc, mrot, bh)
                 : s3_dist_seg_obb(c0, V(0, 0, 0), V(0, 0, 0), bc, mrot, bh)) < su_q32_add(r, ONE / 100)) {
            s3mesh_set_release(s);
            continue;
        }
        hm = s3mesh_sweep(s, 0, pos, rot, c0, v, r, e0, e1, NULL);
        hb = cap ? s3_sweep_capsule_obb(c0, v, r, e0, e1, bc, mrot, bh) : s3_sweep_sphere_obb(c0, v, r, bc, mrot, bh);
        compared++;
        if (hm.hit != hb.hit) {
            mism++;
            if (mism <= 5) {
                fprintf(stderr, "box case %d: mesh %d (toi %.9f) box %d (toi %.9f) cap %d\n", i, hm.hit, qd(hm.toi),
                        hb.hit, qd(hb.toi), cap);
            }
        } else if (hm.hit) {
            double L = qd(su_v3_len(v)), dt = fabs(qd(hm.toi) - qd(hb.toi)) * L;
            double dn = sqrt(pow(qd(hm.n.x) - qd(hb.n.x), 2) + pow(qd(hm.n.y) - qd(hb.n.y), 2) +
                             pow(qd(hm.n.z) - qd(hb.n.z), 2));
            hits++;
            if (dt > worst_t) worst_t = dt;
            if (dn > worst_n) worst_n = dn;
            if (dt > 1e-5 || dn > 1e-4) {
                mism++;
                if (mism <= 5) {
                    fprintf(stderr, "box case %d: toi err %.3g m, normal err %.3g (cap %d)\n", i, dt, dn, cap);
                }
            }
        }
        s3mesh_set_release(s);
    }
    printf("mesh box vs obb: %d compared, %d hits, %d mismatches, worst toi err %.3g m, worst normal err %.3g%c",
           compared, hits, mism, worst_t, worst_n, 10);
    CHECK(mism == 0 && hits > 500);
}

/* ---------- culled traversal equals brute force ---------- */

static void test_brute(void) {
    enum { NT = 400, NV = 3 * NT };
    static SuV3 verts[NV], verts2[NV];
    static uint32_t tris[3 * NT];
    int i, seed, sweeps = 0, hits = 0, dists = 0, within = 0, mism = 0;
    uint64_t culled_tests = 0, brute_tests = 0;
    for (seed = 1; seed <= 12; seed++) {
        TGeom g[2];
        TPart p[4];
        TMesh m[2];
        S3MeshSet *s;
        uint32_t k;
        g_rng = (uint64_t)seed * UINT64_C(7919);
        for (k = 0; k < NV; k += 3) {
            /* Small triangles spread in a 60 m cube (soup), and a second geometry of large thin slivers. */
            SuV3 c = V(rq(30), rq(30), rq(30));
            verts[k] = su_v3_add(c, V(rq(3), rq(3), rq(3)));
            verts[k + 1u] = su_v3_add(c, V(rq(3), rq(3), rq(3)));
            verts[k + 2u] = su_v3_add(c, V(rq(3), rq(3), rq(3)));
            verts2[k] = V(rq(30), rq(30), rq(30));
            verts2[k + 1u] = V(rq(30), rq(30), rq(30));
            verts2[k + 2u] = su_v3_add(verts2[k], V(rq(1), rq(1), rq(1)));
        }
        for (k = 0; k < 3u * NT; k++) {
            tris[k] = k;
        }
        tris[5] = tris[4]; /* one degenerate triangle */
        g[0].nv = NV;
        g[0].nt = NT;
        g[0].v = verts;
        g[0].t = tris;
        g[1] = g[0];
        g[1].v = verts2;
        for (k = 0; k < 4u; k++) {
            p[k].geom = k & 1u;
            p[k].pos = V(rq(20), rq(20), rq(20));
            p[k].rot = rquat();
            p[k].scale = k == 0u ? ONE : rpos(0, 2) + ONE / 2;
        }
        m[0].first = 0;
        m[0].count = 4;
        m[1].first = 2;
        m[1].count = 1;
        s = make_set(g, 2, p, 4, m, 2);
        CHECK(s != NULL);
        if (s == NULL) {
            continue;
        }
        for (i = 0; i < 300; i++) {
            uint32_t mesh = (uint32_t)(rng() % 2u);
            SuV3 pos = V(rq(10), rq(10), rq(10)), c0 = V(rq(60), rq(60), rq(60)), v = V(rq(50), rq(50), rq(50));
            SuQuat rot = rquat();
            int64_t r = rpos(0, 4) + ONE / 8, lim = rpos(0, 30);
            int cap = (i % 3) != 0;
            SuV3 e0 = cap ? su_quat_rotate(rquat(), V(0, 0, rpos(0, 10))) : V(0, 0, 0);
            SuV3 e1 = cap ? su_quat_rotate(rquat(), V(0, 0, -rpos(0, 10))) : V(0, 0, 0);
            S3MeshStats sc, sb;
            S3Hit h1, h2;
            int64_t d1, d2;
            memset(&sc, 0, sizeof sc);
            memset(&sb, 0, sizeof sb);
            h1 = s3mesh_sweep(s, mesh, pos, rot, c0, v, r, e0, e1, &sc);
            d1 = s3mesh_dist(s, mesh, pos, rot, c0, e0, e1, lim, &sc);
            s3mesh_set_brute(s, 1);
            h2 = s3mesh_sweep(s, mesh, pos, rot, c0, v, r, e0, e1, &sb);
            d2 = s3mesh_dist(s, mesh, pos, rot, c0, e0, e1, lim, &sb);
            s3mesh_set_brute(s, 0);
            /* s3mesh_sweep_lim (2026-10-20): a limit at or after the first hit finds that hit exactly; a limit
             * before it finds nothing earlier than it. */
            {
                int64_t tl = rpos(0, 1);
                S3Hit h3 = s3mesh_sweep_lim(s, mesh, pos, rot, c0, v, r, e0, e1, h1.hit ? h1.toi : tl, NULL);
                S3Hit h4 = s3mesh_sweep_lim(s, mesh, pos, rot, c0, v, r, e0, e1, tl > ONE ? ONE : tl, NULL);
                if (h3.hit != h1.hit || h3.toi != h1.toi || h3.n.x != h1.n.x || h3.n.y != h1.n.y ||
                    h3.n.z != h1.n.z || (h4.hit && (!h1.hit || h4.toi < h1.toi)) ||
                    (h1.hit && h1.toi <= tl && (!h4.hit || h4.toi != h1.toi))) {
                    mism++;
                }
            }
            culled_tests += sc.tri_tests;
            brute_tests += sb.tri_tests;
            sweeps++;
            hits += h1.hit;
            dists++;
            within += d2 <= lim;
            if (h1.hit != h2.hit || h1.toi != h2.toi || h1.n.x != h2.n.x || h1.n.y != h2.n.y || h1.n.z != h2.n.z ||
                ((d1 <= lim || d2 <= lim) && d1 != d2)) {
                mism++;
                if (mism <= 5) {
                    fprintf(stderr, "brute seed %d case %d: hit %d/%d toi %.9f/%.9f dist %.6f/%.6f (lim %.3f)\n",
                            seed, i, h1.hit, h2.hit, qd(h1.toi), qd(h2.toi), qd(d1), qd(d2), qd(lim));
                }
            }
        }
        s3mesh_set_release(s);
    }
    printf("mesh culled vs brute: %d sweeps (%d hits), %d distances (%d within the limit), %d mismatches; "
           "triangle tests %" PRIu64 " culled vs %" PRIu64 " brute%c",
           sweeps, hits, dists, within, mism, culled_tests, brute_tests, 10);
    CHECK(mism == 0 && hits > sweeps / 10 && within > dists / 10 && culled_tests * 4u < brute_tests);
}

/* ---------- tunnelling ---------- */

static void test_tunnel(void) {
    /* One square wall of two zero-thickness triangles, 400 x 400 m, in the plane x = 0 of its mesh frame. */
    SuV3 verts[4];
    uint32_t tris[6] = {0, 1, 2, 0, 2, 3};
    TGeom g;
    TPart p;
    TMesh m;
    S3MeshSet *s;
    int i, crossings = 0, missed = 0;
    double worst = 0;
    const int64_t step = su_q32_div(Q(1820), Q(120)); /* 15.1667 m per tick */
    verts[0] = V(0, -Q(200), -Q(200));
    verts[1] = V(0, Q(200), -Q(200));
    verts[2] = V(0, Q(200), Q(200));
    verts[3] = V(0, -Q(200), Q(200));
    g.nv = 4;
    g.nt = 2;
    g.v = verts;
    g.t = tris;
    p.geom = 0;
    p.pos = V(0, 0, 0);
    p.rot = QID;
    p.scale = ONE;
    m.first = 0;
    m.count = 1;
    s = make_set(&g, 1, &p, 1, &m, 1);
    CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    for (i = 0; i < 4000; i++) {
        /* A random direction within 75 degrees of the wall normal, length one tick at 1820 m/s; start so the
         * path crosses the plane somewhere inside the tick (start at -frac * step along the direction). */
        SuQuat rot = rquat();
        SuV3 pos = V(rq(1000), rq(1000), rq(1000));
        double ang = qd(rpos(0, 75)) * 3.141592653589793 / 180.0, az = qd(rpos(0, 360)) * 3.141592653589793 / 180.0;
        SuV3 dirl = V((int64_t)llround(cos(ang) * 4294967296.0), (int64_t)llround(sin(ang) * cos(az) * 4294967296.0),
                      (int64_t)llround(sin(ang) * sin(az) * 4294967296.0));
        int cap = (i & 1) != 0;
        int64_t r = cap ? Q(24) : ONE / 4;
        SuV3 e0l = cap ? V(rq(4), rq(4), -Q(16)) : V(0, 0, 0), e1l = cap ? V(rq(4), rq(4), Q(2)) : V(0, 0, 0);
        SuV3 vl = su_v3_scale(dirl, step), c0l;
        double frac = qd(rpos(0, 1));
        double nx = qd(dirl.x), front, expect;
        S3Hit h;
        /* The most negative x of the mover's swept shape at the start: the core end with the larger x leads. */
        front = (qd(e0l.x) > qd(e1l.x) ? qd(e0l.x) : qd(e1l.x)) + qd(r);
        c0l = V((int64_t)llround((-front - frac * 15.0 * nx) * 4294967296.0), rq(100), rq(100));
        if (cap && fabs(qd(c0l.y)) > 150) {
            c0l.y /= 2;
        }
        /* World: mesh at pos with rot; transform the local path. */
        h = s3mesh_sweep(s, 0, pos, rot, su_v3_add(pos, su_quat_rotate(rot, c0l)), su_quat_rotate(rot, vl), r,
                         su_quat_rotate(rot, e0l), su_quat_rotate(rot, e1l), NULL);
        crossings++;
        /* Expected first contact: the leading core end reaches x = -r (face contact; the path stays inside the
         * wall's extent): distance along the path frac * 15 * nx / nx = frac * 15... in x: travel (-r - (c0.x +
         * maxe)) / nx. */
        expect = (-qd(r) - (qd(c0l.x) + (qd(e0l.x) > qd(e1l.x) ? qd(e0l.x) : qd(e1l.x)))) / nx;
        if (!h.hit) {
            missed++;
            if (missed <= 5) {
                fprintf(stderr, "tunnel case %d: missed (cap %d)\n", i, cap);
            }
            continue;
        }
        {
            double got = qd(h.toi) * qd(su_v3_len(vl));
            double err = fabs(got - expect);
            if (err > worst) worst = err;
            if (err > 1e-4) {
                missed++;
                if (missed <= 5) {
                    fprintf(stderr, "tunnel case %d: contact at %.6f m, expected %.6f (cap %d)\n", i, got, expect,
                            cap);
                }
            }
        }
    }
    printf("mesh tunnelling at 1820 m/s: %d crossings, %d missed or wrong, worst contact error %.3g m%c", crossings,
           missed, worst, 10);
    CHECK(missed == 0);
    s3mesh_set_release(s);
}

/* ---------- known answers ---------- */

static void test_known(void) {
    /* Unit-scale geometry (one triangle) at part scale 100 placed 50 m up: the triangle (0,0,0) (1,0,0) (0,1,0)
     * becomes a 100 m triangle in the plane z = 50; a sphere r 2 from (20, 20, 60) moving -z by 20 touches it at
     * z = 52: toi 0.4, normal +z. Its distance from (20, 20, 60) is 10; from (-30, 20, 50) it is 30 (edge x = 0). */
    SuV3 verts[3];
    uint32_t tris[3] = {0, 1, 2};
    TGeom g;
    TPart p;
    TMesh m;
    S3MeshSet *s;
    S3Hit h;
    verts[0] = V(0, 0, 0);
    verts[1] = V(ONE, 0, 0);
    verts[2] = V(0, ONE, 0);
    g.nv = 3;
    g.nt = 1;
    g.v = verts;
    g.t = tris;
    p.geom = 0;
    p.pos = V(0, 0, Q(50));
    p.rot = QID;
    p.scale = Q(100);
    m.first = 0;
    m.count = 1;
    s = make_set(&g, 1, &p, 1, &m, 1);
    CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    h = s3mesh_sweep(s, 0, V(0, 0, 0), QID, V(Q(20), Q(20), Q(60)), V(0, 0, -Q(20)), Q(2), V(0, 0, 0), V(0, 0, 0),
                     NULL);
    CHECK(h.hit && llabs(h.toi - su_q32_div(Q(2), Q(5))) <= 4 && h.n.z == ONE && h.n.x == 0 && h.n.y == 0);
    CHECK(llabs(s3mesh_dist(s, 0, V(0, 0, 0), QID, V(Q(20), Q(20), Q(60)), V(0, 0, 0), V(0, 0, 0), Q(100), NULL) -
                Q(10)) <= 64);
    CHECK(llabs(s3mesh_dist(s, 0, V(0, 0, 0), QID, V(-Q(30), Q(20), Q(50)), V(0, 0, 0), V(0, 0, 0), Q(100), NULL) -
                Q(30)) <= 64);
    CHECK(s3mesh_dist(s, 0, V(0, 0, 0), QID, V(-Q(30), Q(20), Q(50)), V(0, 0, 0), V(0, 0, 0), Q(10), NULL) > Q(10));
    /* The same with the mesh placed 1000 m along x and turned 180 degrees about z: the triangle spans x in
     * [900, 1000]; a mover coming from above at (980, -20, 60) hits it the same way. */
    {
        SuQuat qz;
        qz.w = 0;
        qz.x = 0;
        qz.y = 0;
        qz.z = ONE;
        h = s3mesh_sweep(s, 0, V(Q(1000), 0, 0), qz, V(Q(980), -Q(20), Q(60)), V(0, 0, -Q(20)), Q(2), V(0, 0, 0),
                         V(0, 0, 0), NULL);
        CHECK(h.hit && llabs(h.toi - su_q32_div(Q(2), Q(5))) <= 4 && h.n.z == ONE);
        /* A capsule lying along x (segment x +/- 30) beyond the hypotenuse, moving away from it: no contact. */
        h = s3mesh_sweep(s, 0, V(0, 0, 0), QID, V(Q(80), Q(80), Q(52)), V(Q(20), Q(20), 0), Q(2), V(-Q(30), 0, 0),
                         V(Q(30), 0, 0), NULL);
        CHECK(!h.hit);
    }
    s3mesh_set_release(s);
}

int main(void) {
    g_rng = UINT64_C(20261020);
    test_known();
    test_blob();
    test_blob2();
    test_box();
    test_brute();
    test_tunnel();
    printf("test_space3d_mesh: %d checks, %d failures%c", g_checks, g_failures, 10);
    return g_failures == 0 ? 0 : 1;
}
