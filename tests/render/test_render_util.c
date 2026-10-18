/* Portable presentation helpers (sutekh/render.h) without a GPU: projection and view matrices against hand-derived
 * values, quaternions, cameras, procedural meshes (closed, outward-facing, expected volume), debug-line building
 * and the PPM writer.
 *
 * Usage: test_render_util   (writes test_render_util.ppm in the working directory) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/render.h"

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

static int near(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static SuVec3f v3(float x, float y, float z) {
    SuVec3f r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static void test_projection(void) {
    /* Camera at the origin looking down -z, 90 degree fov, aspect 1, near 1, far 101. */
    SuMat4f p = su_mat4f_perspective(1.5707963f, 1.0f, 1.0f, 101.0f);
    SuMat4f v = su_mat4f_look_at(v3(0, 0, 0), v3(0, 0, -1), v3(0, 1, 0));
    SuMat4f vp = su_mat4f_mul(p, v);
    float w = 0.0f;
    SuVec3f q;
    /* On the near plane: depth 0; on the far plane: depth 1. */
    q = su_mat4f_transform_point(vp, v3(0, 0, -1), &w);
    CHECK(near(q.z, 0.0f, 1e-6f) && near(w, 1.0f, 1e-6f));
    q = su_mat4f_transform_point(vp, v3(0, 0, -101), &w);
    CHECK(near(q.z, 1.0f, 1e-5f));
    /* Up in the world is up on screen: Vulkan y is down, so +y world -> negative clip y. At 45 degrees: -1. */
    q = su_mat4f_transform_point(vp, v3(0, 2, -2), NULL);
    CHECK(near(q.y, -1.0f, 1e-5f) && near(q.x, 0.0f, 1e-6f));
    q = su_mat4f_transform_point(vp, v3(3, 0, -3), NULL);
    CHECK(near(q.x, 1.0f, 1e-5f));
    /* A camera elsewhere: from (0,0,5) toward the origin, the origin is centred at depth between 0 and 1. */
    v = su_mat4f_look_at(v3(0, 0, 5), v3(0, 0, 0), v3(0, 1, 0));
    vp = su_mat4f_mul(p, v);
    q = su_mat4f_transform_point(vp, v3(0, 0, 0), &w);
    CHECK(near(q.x, 0.0f, 1e-6f) && near(q.y, 0.0f, 1e-6f) && q.z > 0.0f && q.z < 1.0f && near(w, 5.0f, 1e-5f));
    /* Identity and multiplication. */
    {
        SuMat4f i = su_mat4f_identity();
        SuMat4f m = su_mat4f_mul(i, vp);
        CHECK(memcmp(&m, &vp, sizeof m) == 0);
    }
}

static void test_quat_and_cameras(void) {
    SuQuatf yaw90 = {0.0f, 0.70710678f, 0.0f, 0.70710678f}; /* +90 degrees about +y */
    SuQuatf id = {0, 0, 0, 1};
    SuVec3f r = su_quatf_rotate(yaw90, v3(0, 0, -1));
    SuQuatf h;
    SuChaseCamera cc;
    SuFreeCamera fc;
    SuMat4f view;
    SuVec3f q;
    int i;
    CHECK(near(r.x, -1.0f, 1e-5f) && near(r.y, 0.0f, 1e-5f) && near(r.z, 0.0f, 1e-5f)); /* forward turns left */
    h = su_quatf_nlerp(id, yaw90, 0.5f);
    r = su_quatf_rotate(h, v3(0, 0, -1));
    CHECK(near(r.x, -0.70710678f, 1e-4f) && near(r.z, -0.70710678f, 1e-4f));
    {
        SuQuatf neg = {-yaw90.x, -yaw90.y, -yaw90.z, -yaw90.w}; /* the same rotation: the shorter arc is taken */
        SuQuatf h2 = su_quatf_nlerp(id, neg, 0.5f);
        SuVec3f r2 = su_quatf_rotate(h2, v3(0, 0, -1));
        CHECK(near(r2.x, r.x, 1e-5f) && near(r2.z, r.z, 1e-5f));
    }
    r = su_vec3f_lerp(v3(0, 0, 0), v3(2, 4, -6), 0.25f);
    CHECK(near(r.x, 0.5f, 1e-6f) && near(r.y, 1.0f, 1e-6f) && near(r.z, -1.5f, 1e-6f));
    /* Chase camera: first update snaps; the target sits in front of the camera at screen centre. */
    memset(&cc, 0, sizeof cc);
    cc.offset = v3(0, 2, 8);
    cc.look_ahead = v3(0, 0, -10);
    cc.stiffness = 5.0f;
    view = su_chase_camera_update(&cc, v3(100, 0, 0), yaw90, 0.0f);
    /* (0, 2, 8) turned +90 degrees about y is (8, 2, 0). */
    CHECK(near(cc.eye.x, 108.0f, 1e-3f) && near(cc.eye.y, 2.0f, 1e-3f) && near(cc.eye.z, 0.0f, 1e-3f));
    q = su_mat4f_transform_point(view, cc.at, NULL);
    CHECK(near(q.x, 0.0f, 1e-3f) && near(q.y, 0.0f, 1e-3f) && q.z < 0.0f);
    /* Smoothing converges without overshoot, independent of the step split: 10 x 0.01 s == 1 x 0.1 s. */
    {
        SuChaseCamera a = cc;
        SuChaseCamera b = cc;
        for (i = 0; i < 10; i++) {
            (void)su_chase_camera_update(&a, v3(110, 0, 0), yaw90, 0.01f);
        }
        (void)su_chase_camera_update(&b, v3(110, 0, 0), yaw90, 0.1f);
        CHECK(near(a.eye.x, b.eye.x, 1e-3f) && a.eye.x > cc.eye.x && a.eye.x < 118.0f);
    }
    /* Free camera: yaw +90 degrees looks down -x; moving forward 3 goes to x = -3. */
    memset(&fc, 0, sizeof fc);
    view = su_free_camera_update(&fc, v3(0, 0, 0), 1.5707963f, 0.0f);
    (void)su_free_camera_update(&fc, v3(0, 0, 3), 0.0f, 0.0f);
    CHECK(near(fc.pos.x, -3.0f, 1e-4f) && near(fc.pos.z, 0.0f, 1e-4f));
    (void)su_free_camera_update(&fc, v3(0, 0, 0), 0.0f, 5.0f);
    CHECK(near(fc.pitch, 1.5f, 1e-6f));
    (void)view;
}

static void check_mesh(uint32_t mesh, float want_volume, float tol) {
    SuMeshVertex v[1024];
    uint16_t idx[4096];
    uint32_t nv = 0;
    uint32_t ni = 0;
    uint32_t t;
    double vol = 0.0;
    int outward = 1;
    CHECK(su_mesh_build(mesh, NULL, 0, &nv, NULL, 0, &ni) == SU_OK && nv > 0u && ni % 3u == 0u);
    CHECK(su_mesh_build(mesh, v, nv - 1u, &nv, idx, ni, &ni) == SU_E_INVALID_ARGUMENT);
    CHECK(su_mesh_build(mesh, v, 1024, &nv, idx, 4096, &ni) == SU_OK);
    for (t = 0; t < ni; t += 3u) {
        const float *a = v[idx[t]].pos;
        const float *b = v[idx[t + 1u]].pos;
        const float *c = v[idx[t + 2u]].pos;
        double cx = (double)b[1] * c[2] - (double)b[2] * c[1];
        double cy = (double)b[2] * c[0] - (double)b[0] * c[2];
        double cz = (double)b[0] * c[1] - (double)b[1] * c[0];
        /* Geometric normal against the stored normal (degenerate pole triangles are skipped). */
        double ex = (double)b[0] - a[0], ey = (double)b[1] - a[1], ez = (double)b[2] - a[2];
        double fx = (double)c[0] - a[0], fy = (double)c[1] - a[1], fz = (double)c[2] - a[2];
        double nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
        double area = sqrt(nx * nx + ny * ny + nz * nz);
        vol += ((double)a[0] * cx + (double)a[1] * cy + (double)a[2] * cz) / 6.0;
        if (area > 1e-6) { /* sphere pole triangles are degenerate up to float rounding */
            double d = nx * v[idx[t]].normal[0] + ny * v[idx[t]].normal[1] + nz * v[idx[t]].normal[2];
            if (d <= 0.0) {
                outward = 0;
            }
        }
    }
    CHECK(outward);
    if (!outward) {
        fprintf(stderr, "  mesh %u has an inward-facing triangle\n", (unsigned)mesh);
    }
    CHECK(fabs(vol - want_volume) <= tol);
    printf("mesh %u: %u vertices, %u indices, volume %.4f\n", (unsigned)mesh, (unsigned)nv, (unsigned)ni, vol);
}

static void test_meshes(void) {
    uint32_t nv = 0;
    uint32_t ni = 0;
    check_mesh(SU_MESH_BOX, 1.0f, 1e-6f);
    /* 24 x 16 UV sphere of radius 0.5: a little under 4/3 pi 0.125 = 0.5236. */
    check_mesh(SU_MESH_SPHERE, 0.515f, 0.01f);
    check_mesh(SU_MESH_SHIP, 0.042f, 1e-5f); /* tetrahedra N-L-R-T and N-L-R-B: 0.45 * (0.18 + 0.1) / 3 */
    CHECK(su_mesh_build(SU_MESH_COUNT, NULL, 0, &nv, NULL, 0, &ni) == SU_E_INVALID_ARGUMENT);
}

static void test_debug_lines(void) {
    SuRenderLineVertex buf[64];
    SuDebugLines d;
    SuQuatf id = {0, 0, 0, 1};
    memset(&d, 0, sizeof d);
    d.v = buf;
    d.cap = 64;
    su_debug_line(&d, v3(0, 0, 0), v3(1, 0, 0), 0xFFFFFFFFu);
    CHECK(d.count == 2u && buf[1].pos[0] == 1.0f && buf[0].rgba == 0xFFFFFFFFu);
    su_debug_vector(&d, v3(0, 0, 0), v3(0, 2, 0), 1u);
    CHECK(d.count == 12u);
    su_debug_ellipse(&d, v3(0, 0, 0), v3(2, 0, 0), v3(0, 1, 0), 8, 2u);
    CHECK(d.count == 28u);
    CHECK(near(buf[12].pos[0], 2.0f, 1e-5f) && near(buf[13].pos[1], 0.70710678f, 1e-5f));
    su_debug_axes(&d, v3(0, 0, 0), id, 1.0f);
    CHECK(d.count == 34u);
    su_debug_box(&d, v3(0, 0, 0), v3(1, 1, 1), 3u);
    CHECK(d.count == 58u && d.dropped == 0u);
    su_debug_plane(&d, v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), 2, 4u); /* 6 lines + vector (5 lines): 3 fit */
    CHECK(d.count == 64u && d.dropped == 8u);
}

static void test_ppm(void) {
    uint8_t px[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 1, 2, 3, 4};
    uint8_t back[64];
    FILE *f;
    size_t n;
    CHECK(su_render_write_ppm("test_render_util.ppm", px, 2, 2) == SU_OK);
    f = fopen("test_render_util.ppm", "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        n = fread(back, 1, sizeof back, f);
        fclose(f);
        CHECK(n == 11u + 12u && memcmp(back, "P6\n2 2\n255\n", 11) == 0 && back[11] == 255u && back[22] == 3u);
    }
    CHECK(su_render_write_ppm(NULL, px, 2, 2) == SU_E_INVALID_ARGUMENT);
}

static void test_env(void) {
    enum { S = 16 };
    static float faces[6 * S * S * 4];
    static uint16_t half[6 * S * S * 4 * 2];
    uint32_t f;
    uint32_t x;
    uint32_t y;
    int round_trip = 1;
    uint64_t off = 0;
    uint32_t m;
    int uniform_ok = 1;
    SuRenderLighting l;
    CHECK(su_env_mip_count(8) == 2u && su_env_mip_count(16) == 3u && su_env_mip_count(64) == 5u && su_env_mip_count(1024) == 9u);
    CHECK(su_env_mip_count(7) == 0u && su_env_mip_count(4) == 0u && su_env_mip_count(2048) == 0u);
    CHECK(su_env_half_count(16) == 6u * 4u * (16u * 16u + 8u * 8u + 4u * 4u));
    for (f = 0; f < 6u; f++) {
        for (y = 0; y < S; y++) {
            for (x = 0; x < S; x++) {
                SuVec3f d = su_env_texel_dir(f, S, x, y);
                uint32_t f2 = 9;
                uint32_t x2 = 99;
                uint32_t y2 = 99;
                su_env_dir_texel(d, S, &f2, &x2, &y2);
                round_trip &= f2 == f && x2 == x && y2 == y;
                CHECK(fabsf(d.x * d.x + d.y * d.y + d.z * d.z - 1.0f) < 1e-5f);
            }
        }
    }
    CHECK(round_trip);
    {
        SuVec3f d = su_env_texel_dir(0, S, S / 2u, S / 2u);
        CHECK(d.x > 0.99f);
        d = su_env_texel_dir(3, S, S / 2u, S / 2u);
        CHECK(d.y < -0.99f);
        d = su_env_texel_dir(4, S, S / 2u, S / 2u);
        CHECK(d.z > 0.99f);
    }
    /* a uniform environment stays uniform through every mip (half floats: 1/1024 relative) */
    for (f = 0; f < 6u * S * S; f++) {
        faces[f * 4u] = 0.25f;
        faces[f * 4u + 1u] = 2.0f;
        faces[f * 4u + 2u] = 8.0f;
        faces[f * 4u + 3u] = 1.0f;
    }
    CHECK(su_env_prefilter(S, faces, half) == 1u);
    for (m = 0; m < 3u; m++) {
        uint64_t n = 6ull * (S >> m) * (S >> m);
        uint64_t i;
        for (i = 0; i < n; i++) {
            const uint16_t *h = &half[(off + i) * 4u];
            /* decode 0.25 = 0x3400, 2 = 0x4000, 8 = 0x4800 */
            uniform_ok &= h[0] == 0x3400u && h[1] == 0x4000u && h[2] == 0x4800u && h[3] == 0x3C00u;
        }
        off += n;
    }
    CHECK(uniform_ok);
    CHECK(su_env_prefilter(S, NULL, half) == 0u && su_env_prefilter(7, faces, half) == 0u);
    /* a bright texel spreads: the peak falls with the mip (roughness) while the energy stays */
    memset(faces, 0, sizeof faces);
    for (f = 0; f < 6u * S * S; f++) {
        faces[f * 4u] = faces[f * 4u + 1u] = faces[f * 4u + 2u] = 0.01f;
    }
    faces[(((size_t)4 * S + 8) * S + 8) * 4u] = 100.0f;
    CHECK(su_env_prefilter(S, faces, half) == 1u);
    {
        /* mip 0 keeps it exactly (100 -> half 0x5640), mip 1 spreads over the neighbourhood at lower peak */
        CHECK(half[(((size_t)4 * S + 8) * S + 8) * 4u] == 0x5640u);
        {
            size_t o1 = (size_t)6 * S * S * 4u;
            uint16_t p1 = half[o1 + (((size_t)4 * (S / 2) + 4) * (S / 2) + 4) * 4u];
            CHECK(p1 > 0x3C00u && p1 < 0x5640u);
        }
    }
    memset(&l, 0, sizeof l);
    l.sky_zenith.z = 1.0f;
    l.sky_horizon.y = 1.0f;
    l.sky_nadir.x = 1.0f;
    l.up.y = 1.0f;
    l.sun_dir.y = -1.0f;
    l.sun_color.x = l.sun_color.y = l.sun_color.z = 1.0f;
    l.sun_intensity = 10.0f;
    su_env_sky_faces(S, &l, faces);
    {
        /* +Y face centre looks along up: the zenith colour (plus no sun: the sun is above, glow along +Y) */
        const float *c = &faces[(((size_t)2 * S + 8) * S + 8) * 4u];
        CHECK(c[2] > 1.5f && c[0] > 0.5f && fabsf(c[0] - c[1]) < 0.01f); /* zenith blue plus the white sun glow overhead */
        c = &faces[(((size_t)3 * S + 8) * S + 8) * 4u];
        CHECK(c[0] > 0.9f && c[0] < 1.1f && c[2] < 0.1f); /* nadir red */
    }
    printf("env: texel/direction round trip, mip counts, uniform chain and sky faces ok\n");
}
int main(void) {
    test_projection();
    test_quat_and_cameras();
    test_meshes();
    test_debug_lines();
    test_ppm();
    test_env();
    printf("test_render_util: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
