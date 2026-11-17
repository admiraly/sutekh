/* Stylized rendering offscreen tests (readback + pixel checks, Vulkan validation on):
 *   - per-vertex linear RGBA colors (COLOR_0) on PBR-lite and uncolored regression;
 *   - per-material UV transforms (scale, offset, rotation) on toon materials;
 *   - stylized toon shading: cel bands, shadow tint, Half-Lambert, specular band, Fresnel rim & lit mask,
 *     and SU_TOON_USE_VERTEX_COLOR;
 *   - cooked asset pack upload (su_render_upload_pak) of colored mesh + toon material matches direct API;
 *   - API validation refusals and zero Vulkan validation errors/warnings.
 *
 * Usage: test_render_stylized [--no-validation]   (exit 77 without a usable Vulkan device) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/asset.h"
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

#define W 256u
#define H 256u
#define FOV 1.0471976f

static SuRenderer *g_r;
static uint8_t g_img[W * H * 4];
static uint8_t g_ref[W * H * 4];

static SuVec3f v3(float x, float y, float z) {
    SuVec3f v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static const uint8_t *px(const uint8_t *img, int x, int y) {
    return img + ((size_t)y * W + (size_t)x) * 4u;
}

static int qxy(float t) {
    return 128 + (int)floorf((2.0f * t - 1.0f) * 73.9f + 0.5f);
}

typedef struct Scene {
    float sun_dir[3];
    float sun_intensity;
    float ambient;
    float sky[3];
    SuQuatf obj_rot;
} Scene;

static void scene_init(Scene *s) {
    memset(s, 0, sizeof *s);
    s->sun_dir[2] = -1.0f;
    s->sun_intensity = 3.0f;
    s->obj_rot.w = 1.0f;
}

static int render(const Scene *s, SuRenderHandle model, uint8_t *out) {
    SuRenderFrameDesc f;
    SuRenderObject o;
    int k;
    memset(&o, 0, sizeof o);
    o.pos = su_wpos(0.0, 0.0, -3.0);
    o.rot = s->obj_rot;
    o.scale = 1.0f;
    o.tint[0] = o.tint[1] = o.tint[2] = o.tint[3] = 1.0f;
    o.emissive = 1.0f;
    o.model = model;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.camera.rot.w = 1.0f;
    f.camera.fov_y = FOV;
    f.camera.near_z = 0.05f;
    f.light.sun_dir = v3(s->sun_dir[0], s->sun_dir[1], s->sun_dir[2]);
    f.light.sun_intensity = s->sun_intensity;
    f.light.sun_color = v3(1, 1, 1);
    f.light.sky_zenith = f.light.sky_horizon = f.light.sky_nadir = v3(s->sky[0], s->sky[1], s->sky[2]);
    f.light.ambient = s->ambient;
    f.light.tonemap = SU_TONEMAP_CLAMP;
    f.light.no_sky = 1;
    f.light.exposure = 1.0f;
    f.light.clear_color[3] = 1.0f;
    f.objects = &o;
    f.object_count = 1u;
    for (k = 0; k < 3; k++) {
        uint32_t st = su_render_frame_ex(g_r, &f);
        CHECK(st == SU_OK);
        if (st != SU_OK) {
            fprintf(stderr, "frame: %s\n", su_render_last_error(g_r));
            return 0;
        }
    }
    CHECK(su_render_readback(g_r, out, (size_t)W * H * 4u) == SU_OK);
    return 1;
}

static SuRenderHandle mk_tex(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *bytes) {
    SuRenderTextureDesc d;
    const uint8_t *mips[1];
    SuRenderHandle t = 0;
    mips[0] = bytes;
    memset(&d, 0, sizeof d);
    d.width = w;
    d.height = h;
    d.format = fmt;
    d.mip_count = 1;
    d.mip_data = mips;
    d.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_texture_create(g_r, &d, &t) == SU_OK);
    return t;
}

static void quad_geom(SuAssetVertex *v, uint32_t *idx) {
    static const float xy[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    int i;
    for (i = 0; i < 4; i++) {
        memset(&v[i], 0, sizeof v[i]);
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].normal[2] = 1.0f;
        v[i].tangent[0] = 1.0f;
        v[i].tangent[3] = 1.0f;
        v[i].uv0[0] = (xy[i][0] + 1.0f) * 0.5f;
        v[i].uv0[1] = (1.0f - xy[i][1]) * 0.5f;
        v[i].uv1[0] = v[i].uv0[1];
        v[i].uv1[1] = v[i].uv0[0];
    }
    idx[0] = 0;
    idx[1] = 1;
    idx[2] = 2;
    idx[3] = 0;
    idx[4] = 2;
    idx[5] = 3;
}

static SuRenderHandle quad_model(SuRenderHandle mat, const SuAssetVertexColor *colors) {
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuRenderModelNode mn;
    SuRenderHandle msh = 0;
    SuRenderHandle model = 0;
    quad_geom(v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6;
    sm.radius = 1.5f;
    if (colors != NULL) {
        SuRenderColoredMeshDesc cd;
        memset(&cd, 0, sizeof cd);
        cd.vertices = v;
        cd.colors = colors;
        cd.vertex_count = 4;
        cd.indices = idx;
        cd.index_count = 6;
        cd.submeshes = &sm;
        cd.submesh_count = 1;
        cd.materials = &mat;
        cd.radius = 1.5f;
        cd.flags = SU_RENDER_UPLOAD_COPY;
        CHECK(su_render_colored_mesh_create(g_r, &cd, &msh) == SU_OK);
    } else {
        SuRenderMeshDesc md;
        memset(&md, 0, sizeof md);
        md.vertices = v;
        md.vertex_count = 4;
        md.indices = idx;
        md.index_count = 6;
        md.submeshes = &sm;
        md.submesh_count = 1;
        md.materials = &mat;
        md.radius = 1.5f;
        md.flags = SU_RENDER_UPLOAD_COPY;
        CHECK(su_render_mesh_create(g_r, &md, &msh) == SU_OK);
    }
    memset(&mn, 0, sizeof mn);
    mn.transform[0] = mn.transform[5] = mn.transform[10] = mn.transform[15] = 1.0f;
    mn.lod_mesh[0] = msh;
    mn.lod_screen[0] = 1.0f;
    mn.lod_count = 1;
    CHECK(su_render_model_create(g_r, &mn, 1, &model) == SU_OK);
    return model;
}

static const char *type_name(uint32_t t) {
    static const char *const names[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    return names[t <= SU_RENDER_DEVICE_CPU ? t : 0];
}

/* ---------- 1. PBR-lite per-vertex color ---------- */

static void test_pbr_vertex_color(void) {
    /* Left vertices (0, 3) are red; right vertices (1, 2) are blue. */
    static const SuAssetVertexColor vc[4] = {
        {{1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.0f, 0.0f, 1.0f, 1.0f}},
        {{0.0f, 0.0f, 1.0f, 1.0f}},
        {{1.0f, 0.0f, 0.0f, 1.0f}},
    };
    Scene sc;
    SuRenderMaterialDesc md;
    SuRenderHandle mat = 0;
    SuRenderHandle m_col;
    SuRenderHandle m_plain;
    const uint8_t *l;
    const uint8_t *mid;
    const uint8_t *r;
    const uint8_t *pl;
    const uint8_t *pr;

    scene_init(&sc);
    memset(&md, 0, sizeof md);
    su_asset_material_default(&md.factors);
    md.factors.metallic = 0.0f;
    md.factors.roughness = 1.0f;
    CHECK(su_render_material_create(g_r, &md, &mat) == SU_OK);

    m_col = quad_model(mat, vc);
    m_plain = quad_model(mat, NULL);

    CHECK(render(&sc, m_col, g_img));
    l = px(g_img, qxy(0.15f), qxy(0.5f));
    mid = px(g_img, qxy(0.5f), qxy(0.5f));
    r = px(g_img, qxy(0.85f), qxy(0.5f));
    printf("pbr COLOR_0: left=%d,%d,%d mid=%d,%d,%d right=%d,%d,%d\n", l[0], l[1], l[2], mid[0], mid[1], mid[2], r[0],
           r[1], r[2]);
    CHECK(l[0] > l[2] + 80 && l[1] < 40);
    CHECK(r[2] > r[0] + 80 && r[1] < 40);
    CHECK(abs((int)mid[0] - (int)mid[2]) <= 16 && mid[0] > 80);

    md.factors.unlit = 1;
    CHECK(su_render_material_create(g_r, &md, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, vc), g_img));
    l = px(g_img, qxy(0.15f), qxy(0.5f));
    r = px(g_img, qxy(0.85f), qxy(0.5f));
    CHECK(l[0] > l[2] + 80 && l[1] == 0);
    CHECK(r[2] > r[0] + 80 && r[1] == 0);

    CHECK(render(&sc, m_plain, g_ref));
    pl = px(g_ref, qxy(0.15f), qxy(0.5f));
    pr = px(g_ref, qxy(0.85f), qxy(0.5f));
    CHECK(abs((int)pl[0] - (int)pr[0]) <= 2 && abs((int)pl[1] - (int)pl[2]) <= 2 && pl[0] > 180);
}

/* ---------- 2. Per-material UV transforms (scale, offset, rotation) ---------- */

static void test_uv_transform(void) {
    static uint8_t quad_tex[64 * 64 * 4];
    uint32_t x, y;
    SuRenderHandle tex;
    Scene sc;
    SuRenderToonDesc td;
    SuRenderHandle mat = 0;
    const uint8_t *tl, *tr, *bl, *br;

    for (y = 0; y < 64u; y++) {
        for (x = 0; x < 64u; x++) {
            uint8_t *p = &quad_tex[(y * 64u + x) * 4u];
            int right = x >= 32u;
            int bot = y >= 32u;
            if (!bot && !right) { /* red */
                p[0] = 255; p[1] = 0; p[2] = 0;
            } else if (!bot && right) { /* green */
                p[0] = 0; p[1] = 255; p[2] = 0;
            } else if (bot && !right) { /* blue */
                p[0] = 0; p[1] = 0; p[2] = 255;
            } else { /* yellow */
                p[0] = 255; p[1] = 255; p[2] = 0;
            }
            p[3] = 255;
        }
    }
    tex = mk_tex(SU_TEX_RGBA8_SRGB, 64, 64, quad_tex);
    scene_init(&sc);

    memset(&td, 0, sizeof td);
    su_asset_material_default(&td.factors);
    td.factors.unlit = 1;
    td.factors.tex_info[SU_ASSET_SLOT_BASE_COLOR] = SU_ASSET_TEX_INFO(0, SU_ASSET_WRAP_REPEAT, SU_ASSET_WRAP_REPEAT);
    td.textures[SU_ASSET_SLOT_BASE_COLOR] = tex;
    su_asset_toon_default(&td.toon);

    /* Identity UV transform */
    CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, NULL), g_img));
    tl = px(g_img, qxy(0.25f), qxy(0.25f));
    tr = px(g_img, qxy(0.75f), qxy(0.25f));
    bl = px(g_img, qxy(0.25f), qxy(0.75f));
    br = px(g_img, qxy(0.75f), qxy(0.75f));
    CHECK(tl[0] > 200 && tl[1] < 20 && tl[2] < 20);
    CHECK(tr[1] > 200 && tr[0] < 20 && tr[2] < 20);
    CHECK(bl[2] > 200 && bl[0] < 20 && bl[1] < 20);
    CHECK(br[0] > 200 && br[1] > 200 && br[2] < 20);

    /* Offset (+0.5, 0): swaps left and right columns */
    td.toon.uv_offset[0] = 0.5f;
    CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, NULL), g_img));
    tl = px(g_img, qxy(0.25f), qxy(0.25f));
    tr = px(g_img, qxy(0.75f), qxy(0.25f));
    CHECK(tl[1] > 200 && tl[0] < 20);
    CHECK(tr[0] > 200 && tr[1] < 20);
    td.toon.uv_offset[0] = 0.0f;

    /* Scale (2, 2): entire 4-quadrant texture repeats inside the top-left quarter [0..0.5]^2 */
    td.toon.uv_scale[0] = 2.0f;
    td.toon.uv_scale[1] = 2.0f;
    CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, NULL), g_img));
    tl = px(g_img, qxy(0.125f), qxy(0.125f));
    tr = px(g_img, qxy(0.375f), qxy(0.125f));
    bl = px(g_img, qxy(0.125f), qxy(0.375f));
    br = px(g_img, qxy(0.375f), qxy(0.375f));
    CHECK(tl[0] > 200 && tl[1] < 20 && tl[2] < 20);
    CHECK(tr[1] > 200 && tr[0] < 20 && tr[2] < 20);
    CHECK(bl[2] > 200 && bl[0] < 20 && bl[1] < 20);
    CHECK(br[0] > 200 && br[1] > 200 && br[2] < 20);

    /* Rotation (+PI/2): (u,v) -> (-v, u), mapping top-left -> green, top-right -> yellow, bot-left -> red, bot-right -> blue */
    td.toon.uv_scale[0] = 1.0f;
    td.toon.uv_scale[1] = 1.0f;
    td.toon.uv_rotation = 1.57079633f;
    CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, NULL), g_img));
    tl = px(g_img, qxy(0.25f), qxy(0.25f));
    tr = px(g_img, qxy(0.75f), qxy(0.25f));
    bl = px(g_img, qxy(0.25f), qxy(0.75f));
    br = px(g_img, qxy(0.75f), qxy(0.75f));
    CHECK(tl[1] > 200 && tl[0] < 20 && tl[2] < 20);
    CHECK(tr[0] > 200 && tr[1] > 200 && tr[2] < 20);
    CHECK(bl[0] > 200 && bl[1] < 20 && bl[2] < 20);
    CHECK(br[2] > 200 && br[0] < 20 && br[1] < 20);
}

/* ---------- 3. Toon cel shading, Half-Lambert, specular, rim, vertex color ---------- */

static void test_toon_shading(void) {
    static uint8_t split_nrm[64 * 64 * 4];
    uint32_t x, y;
    SuRenderHandle n_tex;
    Scene sc;
    SuRenderToonDesc td;
    SuRenderHandle mat = 0;

    /* Normal map: left half tilts away from sun (-0.8, 0, 0.6), right half faces sun (+0.6, 0, 0.8) */
    for (y = 0; y < 64u; y++) {
        for (x = 0; x < 64u; x++) {
            uint8_t *p = &split_nrm[(y * 64u + x) * 4u];
            if (x < 32u) {
                p[0] = (uint8_t)floorf((-0.8f * 0.5f + 0.5f) * 255.0f + 0.5f);
                p[1] = 128;
                p[2] = (uint8_t)floorf((0.6f * 0.5f + 0.5f) * 255.0f + 0.5f);
            } else {
                p[0] = (uint8_t)floorf((0.6f * 0.5f + 0.5f) * 255.0f + 0.5f);
                p[1] = 128;
                p[2] = (uint8_t)floorf((0.8f * 0.5f + 0.5f) * 255.0f + 0.5f);
            }
            p[3] = 255;
        }
    }
    n_tex = mk_tex(SU_TEX_RGBA8_UNORM, 64, 64, split_nrm);

    scene_init(&sc);
    /* Sun comes from (+0.8, 0, +0.6) */
    sc.sun_dir[0] = -0.8f;
    sc.sun_dir[1] = 0.0f;
    sc.sun_dir[2] = -0.6f;
    sc.sun_intensity = 1.0f;
    sc.ambient = 0.0f;

    memset(&td, 0, sizeof td);
    su_asset_material_default(&td.factors);
    td.textures[SU_ASSET_SLOT_NORMAL] = n_tex;
    su_asset_toon_default(&td.toon);
    td.toon.diffuse_bands = 1;
    td.toon.shade_threshold = 0.2f;
    td.toon.shade_softness = 0.01f;
    td.toon.shadow_color[0] = 0.1f;
    td.toon.shadow_color[1] = 0.2f;
    td.toon.shadow_color[2] = 0.7f;
    td.toon.specular_intensity = 0.0f;
    td.toon.rim_intensity = 0.0f;
    td.toon.flags = 0u; /* standard Lambert: left nl = -0.28 < 0.2 (shadow), right nl = +0.96 > 0.2 (lit) */

    CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, NULL), g_img));
    {
        const uint8_t *l = px(g_img, qxy(0.25f), qxy(0.5f));
        const uint8_t *r = px(g_img, qxy(0.75f), qxy(0.5f));
        printf("toon cel: left(shadow)=%d,%d,%d right(lit)=%d,%d,%d\n", l[0], l[1], l[2], r[0], r[1], r[2]);
        CHECK(l[2] > l[0] + 80);
        CHECK(r[0] > 230 && r[1] > 230 && r[2] > 230);
    }

    /* Half-Lambert lifts left nl = -0.28 * 0.5 + 0.5 = 0.36 > 0.2 -> becomes lit */
    td.toon.flags = SU_TOON_HALF_LAMBERT;
    CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, NULL), g_img));
    {
        const uint8_t *l = px(g_img, qxy(0.25f), qxy(0.5f));
        printf("toon half-lambert: left=%d,%d,%d\n", l[0], l[1], l[2]);
        CHECK(l[0] > 230 && l[1] > 230 && l[2] > 230);
    }

    /* Specular band: aligned view & sun gives sharp reddish specular highlight over dim base */
    scene_init(&sc);
    sc.sun_intensity = 1.0f;
    memset(&td, 0, sizeof td);
    su_asset_material_default(&td.factors);
    td.factors.base_color[0] = 0.1f;
    td.factors.base_color[1] = 0.1f;
    td.factors.base_color[2] = 0.1f;
    su_asset_toon_default(&td.toon);
    td.toon.diffuse_bands = 1;
    td.toon.shade_threshold = 0.0f;
    td.toon.specular_color[0] = 1.0f;
    td.toon.specular_color[1] = 0.0f;
    td.toon.specular_color[2] = 0.0f;
    td.toon.specular_threshold = 0.5f;
    td.toon.specular_softness = 0.01f;
    td.toon.specular_intensity = 0.8f;
    td.toon.glossiness = 16.0f;
    td.toon.rim_intensity = 0.0f;
    CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, NULL), g_img));
    {
        const uint8_t *c = px(g_img, 128, 128);
        printf("toon specular: center=%d,%d,%d\n", c[0], c[1], c[2]);
        CHECK(c[0] > c[1] + 100);
    }

    /* Fresnel rim + SU_TOON_RIM_LIT_MASK: angled quad facing away from sun */
    scene_init(&sc);
    sc.sun_dir[0] = 0.0f;
    sc.sun_dir[1] = 0.0f;
    sc.sun_dir[2] = 1.0f; /* back-lit: dot(n, l) < 0 so cel == 0 */
    sc.sun_intensity = 1.0f;
    sc.obj_rot = su_quatf_axis_angle(v3(0, 1, 0), 0.95f);
    memset(&td, 0, sizeof td);
    su_asset_material_default(&td.factors);
    td.factors.base_color[0] = td.factors.base_color[1] = td.factors.base_color[2] = 0.0f;
    td.factors.double_sided = 1;
    su_asset_toon_default(&td.toon);
    td.toon.diffuse_bands = 1;
    td.toon.shade_threshold = 0.5f;
    td.toon.shade_softness = 0.01f;
    td.toon.shadow_color[0] = td.toon.shadow_color[1] = td.toon.shadow_color[2] = 0.0f;
    td.toon.specular_intensity = 0.0f;
    td.toon.rim_color[0] = 0.0f;
    td.toon.rim_color[1] = 1.0f;
    td.toon.rim_color[2] = 0.0f;
    td.toon.rim_power = 1.5f;
    td.toon.rim_intensity = 1.0f;
    td.toon.flags = 0u; /* unmasked rim: appears even on shadow side */
    CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
    CHECK(render(&sc, quad_model(mat, NULL), g_img));
    {
        int g_unmasked = px(g_img, 128, 128)[1];
        td.toon.flags = SU_TOON_RIM_LIT_MASK;
        CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
        CHECK(render(&sc, quad_model(mat, NULL), g_img));
        {
            int g_masked = px(g_img, 128, 128)[1];
            printf("toon rim: unmasked=%d masked=%d\n", g_unmasked, g_masked);
            CHECK(g_unmasked > 80 && g_masked < 10);
        }
    }

    /* SU_TOON_USE_VERTEX_COLOR toggle on colored quad */
    {
        static const SuAssetVertexColor vc[4] = {
            {{1.0f, 0.0f, 0.0f, 1.0f}},
            {{0.0f, 1.0f, 0.0f, 1.0f}},
            {{0.0f, 1.0f, 0.0f, 1.0f}},
            {{1.0f, 0.0f, 0.0f, 1.0f}},
        };
        SuRenderHandle cm_off, cm_on;
        scene_init(&sc);
        sc.sun_intensity = 1.0f;
        memset(&td, 0, sizeof td);
        su_asset_material_default(&td.factors);
        su_asset_toon_default(&td.toon);
        td.toon.specular_intensity = 0.0f;
        td.toon.rim_intensity = 0.0f;
        td.toon.flags = SU_TOON_HALF_LAMBERT;
        CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
        cm_off = quad_model(mat, vc);
        CHECK(render(&sc, cm_off, g_img));
        CHECK(abs((int)px(g_img, qxy(0.2f), 128)[0] - (int)px(g_img, qxy(0.2f), 128)[1]) <= 2);

        td.toon.flags = SU_TOON_HALF_LAMBERT | SU_TOON_USE_VERTEX_COLOR;
        CHECK(su_render_toon_material_create(g_r, &td, &mat) == SU_OK);
        cm_on = quad_model(mat, vc);
        CHECK(render(&sc, cm_on, g_img));
        CHECK(px(g_img, qxy(0.15f), 128)[0] > px(g_img, qxy(0.15f), 128)[1] + 80);
        CHECK(px(g_img, qxy(0.85f), 128)[1] > px(g_img, qxy(0.85f), 128)[0] + 80);
    }
}

/* ---------- 4. Pack upload roundtrip ---------- */

static void test_stylized_pack_upload(void) {
    static uint8_t tex[4 * 4 * 4];
    static const SuAssetVertexColor vc[4] = {
        {{1.0f, 0.5f, 0.25f, 1.0f}},
        {{0.25f, 0.5f, 1.0f, 1.0f}},
        {{0.5f, 1.0f, 0.5f, 1.0f}},
        {{1.0f, 1.0f, 0.25f, 1.0f}},
    };
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuAssetMaterial f;
    SuAssetToon toon;
    SuAssetModelNode node;
    const uint8_t *mips[1] = {tex};
    uint64_t id_tex = su_asset_id("stylized/tex");
    uint64_t id_mat = su_asset_id("stylized/mat");
    uint64_t id_mesh = su_asset_id("stylized/mesh");
    uint64_t id_mod = su_asset_id("stylized/model");
    SuAssetWriter *w = NULL;
    uint8_t *bytes = NULL;
    size_t len = 0;
    SuAssetPak *pak = NULL;
    char msg[256];
    SuRenderPak rp;
    Scene sc;
    uint32_t i;
    int worst = 0;

    for (i = 0; i < 16u; i++) {
        tex[i * 4u + 0u] = (uint8_t)(80u + (i & 3u) * 50u);
        tex[i * 4u + 1u] = (uint8_t)(200u - (i >> 2) * 35u);
        tex[i * 4u + 2u] = 160u;
        tex[i * 4u + 3u] = 255u;
    }
    quad_geom(v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6u;
    sm.material = id_mat;
    su_asset_material_default(&f);
    f.base_color[0] = 0.9f;
    f.base_color[1] = 0.85f;
    f.base_color[2] = 0.95f;
    f.texture[SU_ASSET_SLOT_BASE_COLOR] = id_tex;
    f.tex_info[SU_ASSET_SLOT_BASE_COLOR] = SU_ASSET_TEX_INFO(0, SU_ASSET_WRAP_REPEAT, SU_ASSET_WRAP_REPEAT);

    su_asset_toon_default(&toon);
    toon.uv_scale[0] = 2.0f;
    toon.uv_scale[1] = 1.5f;
    toon.uv_offset[0] = 0.15f;
    toon.uv_rotation = 0.4f;
    toon.shadow_color[0] = 0.2f;
    toon.shadow_color[1] = 0.2f;
    toon.shadow_color[2] = 0.5f;
    toon.diffuse_bands = 3u;
    toon.specular_intensity = 0.5f;
    toon.rim_intensity = 0.4f;
    toon.flags = SU_TOON_HALF_LAMBERT | SU_TOON_USE_VERTEX_COLOR;

    memset(&node, 0, sizeof node);
    node.transform[0] = node.transform[5] = node.transform[10] = node.transform[15] = 1.0f;
    node.lod_count = 1u;
    node.lod_mesh[0] = id_mesh;
    node.lod_screen[0] = 1.0f;

    CHECK(su_asset_writer_create(&w) == SU_OK);
    CHECK(su_asset_writer_add_texture(w, id_tex, 4, 4, SU_TEX_RGBA8_SRGB, 0, 1, mips) == SU_OK);
    CHECK(su_asset_writer_add_toon_material(w, id_mat, &f, &toon) == SU_OK);
    CHECK(su_asset_writer_add_colored_mesh(w, id_mesh, v, vc, 4u, idx, 6u, &sm, 1u) == SU_OK);
    CHECK(su_asset_writer_add_model(w, id_mod, &node, 1u) == SU_OK);
    CHECK(su_asset_writer_finish(w, &bytes, &len) == SU_OK);
    su_asset_writer_destroy(w);

    CHECK(su_asset_pak_open(bytes, len, 0, &pak, msg) == SU_OK);
    memset(&rp, 0, sizeof rp);
    CHECK(su_render_upload_pak(g_r, pak, id_mod, &rp) == SU_OK);
    CHECK(rp.textures == 1u && rp.materials == 1u && rp.meshes == 1u && rp.models == 1u && rp.model != 0u);

    scene_init(&sc);
    sc.sun_dir[0] = -0.3f;
    sc.sun_dir[1] = -0.2f;
    sc.sun_dir[2] = -0.93f;
    sc.sun_intensity = 1.2f;
    sc.ambient = 0.2f;
    sc.sky[0] = sc.sky[1] = sc.sky[2] = 0.5f;
    CHECK(render(&sc, rp.model, g_img));
    CHECK(su_render_pak_state(g_r, &rp) == SU_RENDER_STATE_READY);

    /* Direct API reference */
    {
        SuRenderToonDesc td;
        SuRenderHandle t_api = mk_tex(SU_TEX_RGBA8_SRGB, 4, 4, tex);
        SuRenderHandle m_api = 0;
        memset(&td, 0, sizeof td);
        td.factors = f;
        td.toon = toon;
        td.textures[SU_ASSET_SLOT_BASE_COLOR] = t_api;
        CHECK(su_render_toon_material_create(g_r, &td, &m_api) == SU_OK);
        CHECK(render(&sc, quad_model(m_api, vc), g_ref));
    }

    for (i = 0; i < W * H * 4u; i++) {
        int d = abs((int)g_img[i] - (int)g_ref[i]);
        worst = d > worst ? d : worst;
    }
    printf("pack upload vs direct API: worst channel diff %d\n", worst);
    CHECK(worst == 0);

    su_render_pak_release(g_r, &rp);
    su_asset_pak_close(pak);
    free(bytes);
}

/* ---------- 5. API validation ---------- */

static void test_stylized_validation(void) {
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuRenderColoredMeshDesc cd;
    SuRenderToonDesc td;
    SuRenderHandle h = 0;

    quad_geom(v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6;
    memset(&cd, 0, sizeof cd);
    cd.vertices = v;
    cd.vertex_count = 4;
    cd.indices = idx;
    cd.index_count = 6;
    cd.submeshes = &sm;
    cd.submesh_count = 1;
    cd.colors = NULL;
    CHECK(su_render_colored_mesh_create(g_r, &cd, &h) == SU_E_INVALID_ARGUMENT);

    memset(&td, 0, sizeof td);
    su_asset_material_default(&td.factors);
    su_asset_toon_default(&td.toon);
    td.toon.diffuse_bands = 0u;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.toon.diffuse_bands = 2u;
    td.toon.glossiness = 0.5f;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.toon.glossiness = 16.0f;
    td.toon.uv_scale[0] = NAN;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.toon.uv_scale[0] = 1.0f;
    td.toon.flags = 0x10u;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.toon.flags = SU_TOON_HALF_LAMBERT;
    td.toon.reserved0 = 1u;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.toon.reserved0 = 0u;
    td.toon.reserved[2] = 1u;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.toon.reserved[2] = 0u;
    td.factors.tex_info[0] = 0x10000u;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.factors.tex_info[0] = 0u;
    td.factors.double_sided = 2u;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.factors.double_sided = 0u;
    td.factors.unlit = 2u;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
    td.factors.unlit = 0u;
    {
        static const uint8_t pixel[4] = {128, 128, 255, 255};
        SuRenderHandle srgb = mk_tex(SU_TEX_RGBA8_SRGB, 1, 1, pixel);
        td.textures[SU_ASSET_SLOT_NORMAL] = srgb;
        CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
        td.textures[SU_ASSET_SLOT_NORMAL] = 0u;
        td.textures[SU_ASSET_SLOT_OCCLUSION] = srgb;
        CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
        td.textures[SU_ASSET_SLOT_OCCLUSION] = 0u;
        td.textures[SU_ASSET_SLOT_METAL_ROUGH] = srgb;
        CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
        td.textures[SU_ASSET_SLOT_METAL_ROUGH] = 0u;
        CHECK(su_render_release(g_r, srgb) == SU_OK);
        {
            SuRenderHandle linear = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, pixel);
            td.textures[SU_ASSET_SLOT_BASE_COLOR] = linear;
            CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
            td.textures[SU_ASSET_SLOT_BASE_COLOR] = 0u;
            td.textures[SU_ASSET_SLOT_EMISSIVE] = linear;
            CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
            td.textures[SU_ASSET_SLOT_EMISSIVE] = 0u;
            CHECK(su_render_release(g_r, linear) == SU_OK);
        }
    }
    td.textures[0] = 0xDEADBEEFu;
    CHECK(su_render_toon_material_create(g_r, &td, &h) == SU_E_INVALID_ARGUMENT);
}

int main(int argc, char **argv) {
    int validation = 1;
    SuRenderDeviceInfo devs[8];
    SuRenderDeviceInfo used;
    uint32_t ndev = 0;
    char msg[256];
    uint32_t i;
    int usable = 0;
    SuRenderDesc d;
    SuRenderStats st;
    uint32_t code;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--no-validation") == 0) {
            validation = 0;
        }
    }
    msg[0] = '\0';
    if (su_render_enumerate(devs, 8, &ndev, msg) != SU_OK) {
        printf("SKIP: Vulkan unavailable: %s\n", msg);
        return 77;
    }
    for (i = 0; i < ndev && i < 8u; i++) {
        usable |= devs[i].meets_baseline != 0u;
    }
    if (!usable) {
        printf("SKIP: no Vulkan device meets the baseline\n");
        return 77;
    }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.flags = validation ? SU_RENDER_FLAG_VALIDATION : 0u;
    d.width = W;
    d.height = H;
    d.device_index = -1;
    code = su_render_create(&d, &g_r, msg);
    CHECK(code == SU_OK);
    if (code != SU_OK) {
        fprintf(stderr, "create: %s\n", msg);
        return 1;
    }
    CHECK(su_render_device(g_r, &used) == SU_OK);
    printf("rendering on: %s (%s)\n", used.name, type_name(used.type));

    test_pbr_vertex_color();
    test_uv_transform();
    test_toon_shading();
    test_stylized_pack_upload();
    test_stylized_validation();

    CHECK(su_render_stats(g_r, &st) == SU_OK);
    if (validation) {
        CHECK(getenv("SUTEKH_REQUIRE_VALIDATION") == NULL || st.validation_enabled != 0u);
        printf("validation %s: %u errors, %u warnings\n", st.validation_enabled ? "on" : "not available",
               st.validation_errors, st.validation_warnings);
        CHECK(st.validation_errors == 0u && st.validation_warnings == 0u);
    }
    su_render_destroy(g_r);
    printf("test_render_stylized: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
