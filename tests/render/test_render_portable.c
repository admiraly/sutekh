/* Portable presentation helpers added for asset scenes (sutekh/render.h), no GPU: Q32.32 camera-relative positions
 * against float32 (precision far from the origin), reversed-Z projection depths, frustum planes, LOD selection,
 * quaternion helpers, ribbon trails, billboards, the HUD builder and the font atlas. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdio.h>
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

static SuVec3f v3(float x, float y, float z) {
    SuVec3f r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static void test_world_positions(void) {
    /* 1 mm at 10 000 km from the origin */
    SuWorldPos origin = su_wpos(1.0e7, -2.0e6, 3.5e6);
    SuWorldPos a = su_wpos(1.0e7 + 0.001, -2.0e6 - 0.002, 3.5e6 + 0.0005);
    SuVec3f d = su_wpos_delta(a, origin);
    float naive = (float)(1.0e7 + 0.001) - (float)1.0e7;
    SuWorldPos m;
    CHECK(fabsf(d.x - 0.001f) < 1e-7f && fabsf(d.y + 0.002f) < 1e-7f && fabsf(d.z - 0.0005f) < 1e-7f);
    CHECK(naive == 0.0f); /* float32 loses the millimetre entirely at this distance */
    printf("1 mm at 10000 km: camera-relative %.7f m, naive float32 %.7f m\n", (double)d.x, (double)naive);
    CHECK(memcmp(&a, &(SuWorldPos){a.x, a.y, a.z}, sizeof a) == 0);
    m = su_wpos_lerp(origin, a, 0.0f);
    CHECK(m.x == origin.x && m.y == origin.y && m.z == origin.z);
    m = su_wpos_lerp(origin, a, 1.0f);
    CHECK(m.x == a.x && m.y == a.y && m.z == a.z);
    m = su_wpos_lerp(su_wpos(0, 0, 0), su_wpos(10, -4, 2), 0.25f);
    CHECK(m.x == su_wpos(2.5, 0, 0).x && m.y == su_wpos(0, -1, 0).y && m.z == su_wpos(0, 0, 0.5).z);
    m = su_wpos_add(origin, v3(1.5f, 0.0f, -0.25f));
    d = su_wpos_delta(m, origin);
    CHECK(d.x == 1.5f && d.y == 0.0f && d.z == -0.25f);
}

static void test_projection_and_frustum(void) {
    float f = 1.0f / tanf(0.5f);
    SuMat4f p = su_mat4f_perspective_rz(1.0f, 2.0f, 0.1f);
    SuFrustum fr;
    float w;
    SuVec3f c;
    c = su_mat4f_transform_point(p, v3(0, 0, -0.1f), &w);
    CHECK(fabsf(c.z - 1.0f) < 1e-6f); /* depth 1 at the near plane */
    c = su_mat4f_transform_point(p, v3(0, 0, -0.2f), &w);
    CHECK(fabsf(c.z - 0.5f) < 1e-6f);
    c = su_mat4f_transform_point(p, v3(0, 0, -1.0e6f), &w);
    CHECK(c.z > 0.0f && c.z < 1e-6f); /* approaches 0, stays positive */
    c = su_mat4f_transform_point(p, v3(1.0f, 1.0f, -1.0f), &w);
    CHECK(fabsf(c.x - f / 2.0f) < 1e-5f && fabsf(c.y + f) < 1e-5f); /* aspect 2, y down */
    su_frustum_from_matrix(&fr, p, 1);
    CHECK(fr.count == 5u);
    CHECK(su_frustum_sphere(&fr, v3(0, 0, -10), 1.0f) == 1);
    CHECK(su_frustum_sphere(&fr, v3(0, 0, 10), 1.0f) == 0);        /* behind */
    CHECK(su_frustum_sphere(&fr, v3(0, 0, 0.5f), 1.0f) == 1);      /* around the camera */
    CHECK(su_frustum_sphere(&fr, v3(0, 0, -1.0e7f), 1.0f) == 1);   /* infinite far */
    CHECK(su_frustum_sphere(&fr, v3(0, 20, -10), 1.0f) == 0);      /* above (half-angle 0.5 rad) */
    /* a sphere just touching the top plane from outside: the plane through the origin at angle 0.5 rad */
    {
        float y = 10.0f * tanf(0.5f);
        float dist = 1.0f * cosf(0.5f); /* distance from (0, y + 1, -10) to the plane is cos(0.5) */
        CHECK(su_frustum_sphere(&fr, v3(0, y + 1.0f, -10), dist * 1.001f) == 1);
        CHECK(su_frustum_sphere(&fr, v3(0, y + 1.0f, -10), dist * 0.999f) == 0);
    }
    su_frustum_from_matrix(&fr, su_mat4f_perspective(1.0f, 1.0f, 1.0f, 100.0f), 0);
    CHECK(fr.count == 6u && su_frustum_sphere(&fr, v3(0, 0, -200), 1.0f) == 0 && su_frustum_sphere(&fr, v3(0, 0, -50), 1.0f));
}

static void test_lod(void) {
    float lod[4] = {1.0f, 0.25f, 0.08f, 0.02f};
    float py = 1.0f / tanf(0.5f);
    CHECK(su_lod_select(lod, 4, 1.0f, 0.5f, py) == 0u);           /* inside */
    CHECK(su_lod_select(lod, 4, 1.0f, py / 0.5f, py) == 0u);      /* fraction 0.5 */
    CHECK(su_lod_select(lod, 4, 1.0f, py / 0.2f, py) == 1u);      /* 0.2 */
    CHECK(su_lod_select(lod, 4, 1.0f, py / 0.05f, py) == 2u);     /* 0.05 */
    CHECK(su_lod_select(lod, 4, 1.0f, py / 0.01f, py) == 3u);
    CHECK(su_lod_select(lod, 2, 1.0f, py / 0.01f, py) == 1u);     /* only two LODs */
    CHECK(fabsf(su_lod_screen_fraction(2.0f, 10.0f, py) - 0.2f * py) < 1e-6f);
}

static void test_quat(void) {
    SuQuatf q = su_quatf_axis_angle(v3(0, 1, 0), 1.5707963f);
    SuVec3f r = su_quatf_rotate(q, v3(1, 0, 0));
    SuQuatf q2 = su_quatf_mul(q, q);
    SuMat4f m = su_mat4f_from_quat(q);
    CHECK(fabsf(r.x) < 1e-6f && fabsf(r.z + 1.0f) < 1e-6f); /* +x turns to -z around +y */
    r = su_quatf_rotate(q2, v3(1, 0, 0));
    CHECK(fabsf(r.x + 1.0f) < 1e-6f);
    r = su_mat4f_transform_point(m, v3(1, 0, 0), NULL);
    CHECK(fabsf(r.z + 1.0f) < 1e-6f);
}

static void test_trail_and_fx(void) {
    SuTrailPoint pts[16];
    SuTrail t;
    SuFxVertex v[256];
    SuFxBuffer b;
    float c0[4] = {1, 1, 1, 1};
    float c1[4] = {0, 0, 0, 0};
    SuWorldPos cam = su_wpos(1.0e6, 0, 0);
    int i;
    su_trail_init(&t, pts, 16, 1.0f, 0.5f);
    for (i = 0; i <= 20; i++) {
        /* 0.1 m per 0.01 s along -z: a point is stored every 0.5 m (every 5 pushes); the head follows */
        su_trail_push(&t, su_wpos_add(cam, v3(0, -2.0f, -10.0f - 0.1f * (float)i)), 0.01 * i);
    }
    CHECK(t.count == 6u); /* stored at i = 0, 4, 8, 12, 16 and the head at 20 */
    memset(&b, 0, sizeof b);
    b.v = v;
    b.cap = 256;
    su_trail_build(&t, 0.2, cam, 0.4f, 0.0f, c0, c1, &b);
    CHECK(b.count == 6u * (t.count - 1u) && b.dropped == 0u);
    /* the head end: width 0.4 across x (the ribbon faces the camera), full colour, v = 0 */
    {
        float wx = fabsf(v[0].pos[0] - v[1].pos[0]);
        CHECK(fabsf(wx - 0.4f) < 1e-3f && v[0].color[0] == 1.0f && v[0].uv[1] == 0.0f);
        CHECK(fabsf(v[0].pos[2] + 12.0f) < 1e-3f); /* newest point: -10 - 0.1 * 20 */
    }
    /* lifetime: 1.5 s later everything has expired */
    su_trail_push(&t, su_wpos_add(cam, v3(0, 0, -50)), 1.7);
    CHECK(t.count == 1u);
    b.count = 0;
    su_trail_build(&t, 1.7, cam, 0.4f, 0.0f, c0, c1, &b);
    CHECK(b.count == 0u);
    /* billboard: 6 vertices around the point, marked v < 0 */
    b.count = 0;
    su_fx_billboard(&b, v3(0, 0, -5), 1.0f, 0.0f, c0, su_mat4f_identity());
    CHECK(b.count == 6u && v[0].uv[1] < 0.0f && fabsf(v[0].pos[0] + 1.0f) < 1e-6f && fabsf(v[2].pos[1] - 1.0f) < 1e-6f);
    b.cap = 8; /* room for one quad only */
    su_fx_beam(&b, v3(0, 0, -1), v3(0, 0, -9), 0.1f, c0);
    CHECK(b.count == 6u && b.dropped == 2u);
}

static void test_hud_and_font(void) {
    SuHudVertex hv[64];
    SuHud h;
    uint8_t atlas[SU_FONT_ATLAS_W * SU_FONT_ATLAS_H];
    float w;
    float hh;
    const uint8_t *g;
    int x;
    int y;
    su_hud_begin(&h, hv, 64, 640, 360);
    su_hud_rect(&h, 10, 20, 30, 40, 0xFF0000FFu);
    CHECK(h.count == 6u && hv[0].pos[0] == 10.0f && hv[2].pos[0] == 40.0f && hv[2].pos[1] == 60.0f);
    CHECK(hv[0].uv[0] == hv[2].uv[0]); /* a solid texel */
    w = su_hud_text(&h, 0, 0, 2.0f, 0xFFFFFFFFu, "AB\nC D");
    CHECK(h.count == 6u + 6u * 4u); /* spaces and newlines draw nothing */
    CHECK(w == 2.0f * 6.0f * 3.0f);
    su_hud_measure("AB\nC D", 2.0f, &w, &hh);
    CHECK(w == 36.0f && hh == (9.0f + 7.0f) * 2.0f);
    w = su_hud_textf(&h, 0, 0, 1.0f, 0xFFFFFFFFu, "%d km/h", 345);
    CHECK(w == 6.0f * 8.0f && h.count == 60u); /* 5 of 7 glyphs fit in 64 vertices */
    CHECK(h.dropped == 2u);
    su_font_atlas(atlas);
    g = su_font_glyph('A');
    for (y = 0; y < 7; y++) {
        for (x = 0; x < 5; x++) {
            int bit = (g[y] >> (4 - x)) & 1;
            int cell_x = (('A' - 32) % 16) * 8;
            int cell_y = (('A' - 32) / 16) * 8;
            CHECK((atlas[(cell_y + y) * SU_FONT_ATLAS_W + cell_x + x] != 0) == bit);
        }
    }
    CHECK(g[0] == 0x0e && g[3] == 0x1f); /* .###. and ##### */
    CHECK(su_font_glyph('\x01') == su_font_glyph('?'));
    CHECK(atlas[(5 * 8 + 4) * SU_FONT_ATLAS_W + 15 * 8 + 4] == 255u); /* solid cell */
    CHECK(atlas[(0 * 8 + 3) * SU_FONT_ATLAS_W + 0 * 8 + 3] == 0u);    /* space */
}

int main(void) {
    test_world_positions();
    test_projection_and_frustum();
    test_lod();
    test_quat();
    test_trail_and_fx();
    test_hud_and_font();
    printf("test_render_portable: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
