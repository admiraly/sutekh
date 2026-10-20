/* Layered paint material, offscreen, read back and checked pixel by pixel with procedurally generated textures:
 *   - each of the four mask channels recolours exactly its own region (switching one layer off changes only it);
 *   - layer order, lerp and multiply blends, opacity and the layer roughness / metalness follow the documented maths;
 *   - zero layers renders exactly like a PBR-lite material with the same inputs (golden-style identity);
 *   - glow x colour x intensity feeds HDR so bloom lights the surroundings; the per-instance multiplier scales it;
 *     a glow-tint layer recolours the glow it covers;
 *   - dirt darkens by amount x darken and roughens; AO scales ambient light only;
 *   - an environment cubemap: reflection of a bright patch falls as roughness rises, intensity scales, a uniform
 *     environment lights diffuse surfaces with its colour; the procedural sky bakes to a cube;
 *   - texture colour spaces are enforced per slot; a pack with a paint material renders like the same material
 *     created through the API; the validation layer reports nothing.
 *
 * Usage: test_render_paint [--no-validation]   (exit 77 without a usable Vulkan device) */
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
#define FOV 1.0471976f /* 60 degrees */

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

/* linear value of an sRGB code, as the attachment encoded it */
static double lin(int code) {
    double s = (double)code / 255.0;
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

/* Screen position of texture coordinate (u, v) on the quad (half size 1 at z = -3, camera at the origin). */
static int qx(float u) {
    return 128 + (int)floorf((2.0f * u - 1.0f) * 73.9f + 0.5f);
}

/* ---------- scene ---------- */

typedef struct Scene {
    float sun_dir[3];
    float sun_intensity;
    float ambient;
    float sky[3];           /* zenith = horizon = nadir */
    float exposure;
    float emissive;         /* per-instance glow multiplier */
    SuRenderHandle env;
    float env_intensity;
    SuRenderPost post;
    SuRenderShadow shadow;
} Scene;

static void scene_init(Scene *s) {
    memset(s, 0, sizeof *s);
    s->sun_dir[2] = -1.0f;
    s->sun_intensity = 3.0f;
    s->exposure = 1.0f;
    s->emissive = 1.0f;
}

static const SuRenderObject *g_obj = NULL; /* when set, render() draws these objects instead of the default one */
static uint32_t g_obj_count = 1;

static int render(const Scene *s, SuRenderHandle model, uint8_t *out) {
    SuRenderFrameDesc f;
    SuRenderObject o;
    int k;
    memset(&o, 0, sizeof o);
    o.pos = su_wpos(0.0, 0.0, -3.0);
    o.rot.w = 1.0f;
    o.scale = 1.0f;
    o.tint[0] = o.tint[1] = o.tint[2] = o.tint[3] = 1.0f;
    o.emissive = s->emissive;
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
    f.light.exposure = s->exposure;
    f.light.clear_color[3] = 1.0f;
    f.objects = g_obj != NULL ? g_obj : &o;
    f.object_count = g_obj != NULL ? g_obj_count : 1u;
    f.post = s->post;
    f.shadow = s->shadow;
    f.env = s->env;
    f.env_intensity = s->env_intensity;
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

/* ---------- resources ---------- */

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

static void quad_mesh(SuAssetVertex *v, uint32_t *idx) {
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
    }
    idx[0] = 0;
    idx[1] = 1;
    idx[2] = 2;
    idx[3] = 0;
    idx[4] = 2;
    idx[5] = 3;
}

static SuRenderHandle quad_model(SuRenderHandle mat) {
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuRenderMeshDesc mesh;
    SuRenderModelNode mn;
    SuRenderHandle msh = 0;
    SuRenderHandle model = 0;
    quad_mesh(v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6;
    sm.radius = 1.5f;
    memset(&mesh, 0, sizeof mesh);
    mesh.vertices = v;
    mesh.vertex_count = 4;
    mesh.indices = idx;
    mesh.index_count = 6;
    mesh.submeshes = &sm;
    mesh.submesh_count = 1;
    mesh.materials = &mat;
    mesh.radius = 1.5f;
    mesh.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_mesh_create(g_r, &mesh, &msh) == SU_OK);
    memset(&mn, 0, sizeof mn);
    mn.transform[0] = mn.transform[5] = mn.transform[10] = mn.transform[15] = 1.0f;
    mn.lod_mesh[0] = msh;
    mn.lod_screen[0] = 1.0f;
    mn.lod_count = 1;
    CHECK(su_render_model_create(g_r, &mn, 1, &model) == SU_OK);
    return model;
}

/* A paint material description with white factors and no layers. */
typedef struct Pm {
    SuRenderPaintDesc d;
} Pm;

static void pm_init(Pm *p, float base) {
    memset(p, 0, sizeof *p);
    su_asset_material_default(&p->d.factors);
    p->d.factors.base_color[0] = p->d.factors.base_color[1] = p->d.factors.base_color[2] = base;
    p->d.factors.metallic = 0.0f;
    p->d.factors.roughness = 1.0f;
    p->d.factors.emissive_strength = 1.0f;
    su_asset_paint_default(&p->d.paint);
}

static void layer_set(Pm *p, uint32_t k, float r, float g, float b, float rough, float metal, float opacity, uint32_t flags) {
    SuAssetPaintLayer *l = &p->d.paint.layer[k];
    l->color[0] = r;
    l->color[1] = g;
    l->color[2] = b;
    l->roughness = rough;
    l->metallic = metal;
    l->opacity = opacity;
    l->flags = flags;
}

/* Creates the material, a quad model of it, renders, and releases nothing (resources are tiny). */
static int shoot(const Scene *s, const Pm *p, uint8_t *out) {
    SuRenderHandle mat = 0;
    SuRenderHandle model;
    uint32_t st = su_render_paint_material_create(g_r, &p->d, &mat);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "paint material: %s\n", su_render_last_error(g_r));
        return 0;
    }
    model = quad_model(mat);
    return render(s, model, out);
}

static int near_enough(int a, int b) {
    return abs(a - b) <= 12;
}

static int same_px(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static const char *type_name(uint32_t t) {
    static const char *const names[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    return names[t <= SU_RENDER_DEVICE_CPU ? t : 0];
}

static void fill_rgba(uint8_t *d, uint32_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        d[i * 4u] = r;
        d[i * 4u + 1u] = g;
        d[i * 4u + 2u] = b;
        d[i * 4u + 3u] = a;
    }
}

/* ---------- tests ---------- */

static void test_mask_channels(void) {
    static uint8_t mask[64 * 64 * 4];
    static const float uv[4][2] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}};
    static const float lc[4][3] = {{1.0f, 0.1f, 0.1f}, {0.1f, 1.0f, 0.1f}, {0.1f, 0.1f, 1.0f}, {1.0f, 1.0f, 0.1f}};
    Scene sc;
    Pm p;
    static uint8_t base[W * H * 4];
    static uint8_t full[W * H * 4];
    uint32_t x;
    uint32_t y;
    uint32_t j;
    uint32_t k;
    scene_init(&sc);
    /* quadrant q drives channel q; a cross-shaped gap between them has no paint at all */
    memset(mask, 0, sizeof mask);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            int gap = (x >= 28 && x < 36) || (y >= 28 && y < 36);
            uint32_t q = (y >= 32 ? 2u : 0u) + (x >= 32 ? 1u : 0u);
            if (!gap) {
                mask[(y * 64 + x) * 4 + q] = 255;
            }
        }
    }
    pm_init(&p, 0.5f);
    p.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 64, 64, mask);
    for (k = 0; k < 4; k++) {
        layer_set(&p, k, lc[k][0], lc[k][1], lc[k][2], 1.0f, 0.0f, 1.0f, 0);
    }
    CHECK(shoot(&sc, &p, base)); /* layer_count 0 */
    p.d.paint.layer_count = 4;
    CHECK(shoot(&sc, &p, full));
    {
        const uint8_t *g = px(full, qx(0.5f), qx(0.5f));
        const uint8_t *b0 = px(base, qx(0.5f), qx(0.5f));
        CHECK(same_px(g, b0)); /* gap: mask 0 everywhere, the base surface exactly */
        printf("mask: gap pixel %d %d %d (base %d %d %d)\n", g[0], g[1], g[2], b0[0], b0[1], b0[2]);
    }
    for (k = 0; k < 4; k++) {
        const uint8_t *q = px(full, qx(uv[k][0]), qx(uv[k][1]));
        const uint8_t *b0 = px(base, qx(uv[k][0]), qx(uv[k][1]));
        printf("mask: region %u pixel %d %d %d (base %d %d %d)\n", k, q[0], q[1], q[2], b0[0], b0[1], b0[2]);
        CHECK(!same_px(q, b0));
        if (k == 0) {
            CHECK(q[0] > q[1] + 60 && q[0] > q[2] + 60);
        } else if (k == 1) {
            CHECK(q[1] > q[0] + 60 && q[1] > q[2] + 60);
        } else if (k == 2) {
            CHECK(q[2] > q[0] + 60 && q[2] > q[1] + 60);
        } else {
            CHECK(q[0] > q[2] + 60 && q[1] > q[2] + 60 && near_enough(q[0], q[1]));
        }
    }
    /* switching layer j off (opacity 0) changes exactly region j and nothing else */
    for (j = 0; j < 4; j++) {
        static uint8_t one[W * H * 4];
        p.d.paint.layer[j].opacity = 0.0f;
        CHECK(shoot(&sc, &p, one));
        p.d.paint.layer[j].opacity = 1.0f;
        for (k = 0; k < 4; k++) {
            const uint8_t *a = px(one, qx(uv[k][0]), qx(uv[k][1]));
            const uint8_t *f = px(full, qx(uv[k][0]), qx(uv[k][1]));
            const uint8_t *b0 = px(base, qx(uv[k][0]), qx(uv[k][1]));
            CHECK(k == j ? same_px(a, b0) : same_px(a, f));
        }
    }
}

/* Pixel (linear, 0..1) at a texture coordinate of the quad. */
static void lin_px(const uint8_t *img, float u, float v, double out[3]) {
    const uint8_t *p = px(img, qx(u), qx(v));
    out[0] = lin(p[0]);
    out[1] = lin(p[1]);
    out[2] = lin(p[2]);
}

static void test_blend_order(void) {
    static uint8_t mask[16 * 16 * 4];
    Scene sc;
    Pm p;
    static uint8_t a[W * H * 4];
    static uint8_t b[W * H * 4];
    static uint8_t black[W * H * 4];
    static uint8_t clean[W * H * 4];
    double pa[3];
    double pb[3];
    double pk[3];
    double pc[3];
    uint32_t c;
    scene_init(&sc);
    fill_rgba(mask, 16 * 16, 255, 255, 0, 0); /* channels 0 and 1 cover everything */
    pm_init(&p, 0.8f);
    p.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, mask);
    layer_set(&p, 0, 1.0f, 0.1f, 0.1f, 1.0f, 0.0f, 1.0f, 0);
    layer_set(&p, 1, 0.1f, 0.1f, 1.0f, 1.0f, 0.0f, 1.0f, 0);
    p.d.paint.layer_count = 2;
    CHECK(shoot(&sc, &p, a)); /* order 0, 1: layer 1 on top */
    p.d.paint.layer_order[0] = 1;
    p.d.paint.layer_order[1] = 0;
    CHECK(shoot(&sc, &p, b)); /* order 1, 0: layer 0 on top */
    {
        const uint8_t *pxa = px(a, 128, 128);
        const uint8_t *pxb = px(b, 128, 128);
        printf("order: [0,1] -> %d %d %d, [1,0] -> %d %d %d\n", pxa[0], pxa[1], pxa[2], pxb[0], pxb[1], pxb[2]);
        CHECK(pxa[2] > pxa[0] + 60);
        CHECK(pxb[0] > pxb[2] + 60);
    }
    /* pixel = A * kd + s for a rough dielectric under the sun (s: the specular term, independent of A) */
    pm_init(&p, 0.0f);
    CHECK(shoot(&sc, &p, black));
    pm_init(&p, 0.8f);
    CHECK(shoot(&sc, &p, clean));
    lin_px(black, 0.5f, 0.5f, pk);
    lin_px(clean, 0.5f, 0.5f, pc);
    {
        double kd[3];
        double exp_mul[3];
        double exp_half[3];
        static const float mulc[3] = {1.0f, 0.5f, 0.25f};
        static const float lc[3] = {0.2f, 0.9f, 0.4f};
        for (c = 0; c < 3; c++) {
            kd[c] = (pc[c] - pk[c]) / 0.8;
        }
        /* multiply */
        pm_init(&p, 0.8f);
        p.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, mask);
        layer_set(&p, 0, mulc[0], mulc[1], mulc[2], 1.0f, 0.0f, 1.0f, SU_PAINT_LAYER_MULTIPLY);
        p.d.paint.layer_count = 1;
        CHECK(shoot(&sc, &p, a));
        lin_px(a, 0.5f, 0.5f, pa);
        /* lerp at opacity 0.5 */
        pm_init(&p, 0.8f);
        p.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, mask);
        layer_set(&p, 0, lc[0], lc[1], lc[2], 1.0f, 0.0f, 0.5f, 0);
        p.d.paint.layer_count = 1;
        CHECK(shoot(&sc, &p, b));
        lin_px(b, 0.5f, 0.5f, pb);
        for (c = 0; c < 3; c++) {
            exp_mul[c] = 0.8 * (double)mulc[c] * kd[c] + pk[c];
            exp_half[c] = (0.8 + ((double)lc[c] - 0.8) * 0.5) * kd[c] + pk[c];
        }
        printf("blend: multiply measured %.4f %.4f %.4f expected %.4f %.4f %.4f\n", pa[0], pa[1], pa[2], exp_mul[0], exp_mul[1],
               exp_mul[2]);
        printf("blend: lerp 0.5 measured %.4f %.4f %.4f expected %.4f %.4f %.4f\n", pb[0], pb[1], pb[2], exp_half[0],
               exp_half[1], exp_half[2]);
        for (c = 0; c < 3; c++) {
            CHECK(fabs(pa[c] - exp_mul[c]) < 0.012);
            CHECK(fabs(pb[c] - exp_half[c]) < 0.012);
        }
    }
    /* tint: A = mix(albedo * base, albedo * C, w), independent of the base colour where w = 1 */
    {
        static uint8_t alb[16 * 16 * 4];
        static const float tc[3] = {0.9f, 0.6f, 0.2f};
        static const float b2[3] = {0.1f, 0.2f, 0.3f};
        double kd[3];
        double al = lin(188);
        double exp_full[3];
        double exp_half[3];
        SuRenderHandle t_alb;
        for (c = 0; c < 3; c++) {
            kd[c] = (pc[c] - pk[c]) / 0.8;
        }
        fill_rgba(alb, 16 * 16, 188, 188, 188, 255);
        t_alb = mk_tex(SU_TEX_RGBA8_SRGB, 16, 16, alb);
        pm_init(&p, 0.8f);
        p.d.textures[SU_PAINT_SLOT_ALBEDO] = t_alb;
        p.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, mask);
        layer_set(&p, 0, tc[0], tc[1], tc[2], 1.0f, 0.0f, 1.0f, SU_PAINT_LAYER_TINT);
        p.d.paint.layer_count = 1;
        CHECK(shoot(&sc, &p, a));
        lin_px(a, 0.5f, 0.5f, pa);
        for (c = 0; c < 3; c++) {
            p.d.factors.base_color[c] = b2[c]; /* another base colour: the fully tinted surface does not change */
        }
        CHECK(shoot(&sc, &p, b));
        CHECK(same_px(px(a, 128, 128), px(b, 128, 128)));
        for (c = 0; c < 3; c++) {
            p.d.factors.base_color[c] = 0.8f;
        }
        p.d.paint.layer[0].opacity = 0.5f;
        CHECK(shoot(&sc, &p, b));
        lin_px(b, 0.5f, 0.5f, pb);
        for (c = 0; c < 3; c++) {
            exp_full[c] = al * (double)tc[c] * kd[c] + pk[c];
            exp_half[c] = (al * 0.8 + (al * (double)tc[c] - al * 0.8) * 0.5) * kd[c] + pk[c];
        }
        printf("blend: tint measured %.4f %.4f %.4f expected %.4f %.4f %.4f\n", pa[0], pa[1], pa[2], exp_full[0], exp_full[1],
               exp_full[2]);
        printf("blend: tint 0.5 measured %.4f %.4f %.4f expected %.4f %.4f %.4f\n", pb[0], pb[1], pb[2], exp_half[0],
               exp_half[1], exp_half[2]);
        for (c = 0; c < 3; c++) {
            CHECK(fabs(pa[c] - exp_full[c]) < 0.012);
            CHECK(fabs(pb[c] - exp_half[c]) < 0.012);
        }
    }
}

/* PBR-lite and a paint material with no layers see the same inputs: the images are identical. */
static void test_zero_layers(void) {
    static uint8_t albedo[64 * 64 * 4];
    static uint8_t mr[64 * 64 * 4];
    static uint8_t nrm[64 * 64 * 4];
    static uint8_t ao[64 * 64 * 4];
    Scene sc;
    Pm p;
    SuRenderMaterialDesc md;
    SuRenderHandle t_alb;
    SuRenderHandle t_mr;
    SuRenderHandle t_n;
    SuRenderHandle t_ao;
    SuRenderHandle pbr = 0;
    uint32_t x;
    uint32_t y;
    int worst = 0;
    int differing = 0;
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            uint8_t *a = &albedo[(y * 64 + x) * 4];
            uint8_t *m = &mr[(y * 64 + x) * 4];
            uint8_t *n = &nrm[(y * 64 + x) * 4];
            uint8_t *o = &ao[(y * 64 + x) * 4];
            double bx = sin((double)x * 0.4) * 0.35;
            double by = cos((double)y * 0.3) * 0.35;
            a[0] = (uint8_t)(60 + x * 3);
            a[1] = (uint8_t)(200 - y * 2);
            a[2] = (uint8_t)(90 + ((x ^ y) & 31) * 4);
            a[3] = 255;
            m[0] = 0;
            m[1] = (uint8_t)(40 + y * 3);   /* roughness in G */
            m[2] = (uint8_t)((x * 4) & 255); /* metalness in B */
            m[3] = 255;
            n[0] = (uint8_t)(127.5 + bx * 127.5);
            n[1] = (uint8_t)(127.5 + by * 127.5);
            n[2] = 255;
            n[3] = 255;
            o[0] = (uint8_t)(120 + (x % 16) * 8);
            o[1] = o[2] = 0;
            o[3] = 255;
        }
    }
    scene_init(&sc);
    sc.sun_dir[0] = -0.4f;
    sc.sun_dir[1] = -0.3f;
    sc.sun_dir[2] = -0.86f;
    sc.sun_intensity = 2.5f;
    sc.ambient = 1.0f;
    sc.sky[0] = 0.2f;
    sc.sky[1] = 0.3f;
    sc.sky[2] = 0.5f;
    t_alb = mk_tex(SU_TEX_RGBA8_SRGB, 64, 64, albedo);
    t_mr = mk_tex(SU_TEX_RGBA8_UNORM, 64, 64, mr);
    t_n = mk_tex(SU_TEX_RGBA8_UNORM, 64, 64, nrm);
    t_ao = mk_tex(SU_TEX_RGBA8_UNORM, 64, 64, ao);
    memset(&md, 0, sizeof md);
    su_asset_material_default(&md.factors);
    md.factors.base_color[0] = 0.9f;
    md.factors.base_color[1] = 0.8f;
    md.factors.base_color[2] = 0.7f;
    md.factors.metallic = 1.0f;
    md.factors.roughness = 1.0f;
    md.factors.occlusion_strength = 0.8f;
    md.textures[SU_ASSET_SLOT_BASE_COLOR] = t_alb;
    md.textures[SU_ASSET_SLOT_METAL_ROUGH] = t_mr;
    md.textures[SU_ASSET_SLOT_NORMAL] = t_n;
    md.textures[SU_ASSET_SLOT_OCCLUSION] = t_ao;
    CHECK(su_render_material_create(g_r, &md, &pbr) == SU_OK);
    CHECK(render(&sc, quad_model(pbr), g_ref));
    pm_init(&p, 1.0f);
    p.d.factors = md.factors;
    p.d.factors.metallic = 1.0f;
    p.d.textures[SU_PAINT_SLOT_ALBEDO] = t_alb;
    p.d.textures[SU_PAINT_SLOT_NORMAL] = t_n;
    p.d.textures[SU_PAINT_SLOT_ROUGHNESS] = t_mr;
    p.d.textures[SU_PAINT_SLOT_MATERIAL] = t_mr;
    p.d.textures[SU_PAINT_SLOT_AO] = t_ao;
    p.d.paint.channel[SU_PAINT_CH_ROUGHNESS] = 1; /* G */
    p.d.paint.channel[SU_PAINT_CH_METAL] = 2;     /* B */
    p.d.paint.channel[SU_PAINT_CH_AO] = 0;        /* R */
    p.d.paint.layer_count = 0;
    layer_set(&p, 0, 0.0f, 1.0f, 0.0f, 0.2f, 1.0f, 1.0f, 0); /* a layer that exists in the data but is not counted */
    CHECK(shoot(&sc, &p, g_img));
    for (y = (uint32_t)qx(0.05f); y < (uint32_t)qx(0.95f); y++) {
        for (x = (uint32_t)qx(0.05f); x < (uint32_t)qx(0.95f); x++) {
            int c;
            for (c = 0; c < 3; c++) {
                int dlt = abs((int)px(g_img, (int)x, (int)y)[c] - (int)px(g_ref, (int)x, (int)y)[c]);
                worst = dlt > worst ? dlt : worst;
                differing += dlt != 0;
            }
        }
    }
    printf("zero layers vs PBR-lite: worst channel difference %d, %d differing channel values\n", worst, differing);
    CHECK(worst == 0);
    /* the picture is not flat (the textures and the normal map matter) */
    CHECK(!same_px(px(g_ref, qx(0.2f), qx(0.2f)), px(g_ref, qx(0.8f), qx(0.7f))));
    CHECK(!same_px(px(g_ref, qx(0.3f), qx(0.5f)), px(g_ref, qx(0.6f), qx(0.5f))));
    /* a BC5 (two-channel) normal map gives the picture of the RGBA8 one within compression error */
    {
        uint8_t *bc = malloc((size_t)su_asset_texture_level_size(SU_TEX_BC5_UNORM, 64, 64));
        if (bc != NULL) {
            SuRenderHandle t_bc;
            uint8_t flat[4] = {128, 128, 255, 255};
            static uint8_t img_bc[W * H * 4];
            static uint8_t img_flat[W * H * 4];
            double sum = 0;
            double sum_flat = 0;
            uint32_t cnt = 0;
            SuRenderTextureDesc td;
            const uint8_t *mips[1];
            CHECK(su_asset_bc_encode(SU_TEX_BC5_UNORM, nrm, 64, 64, bc) == SU_OK);
            mips[0] = bc;
            memset(&td, 0, sizeof td);
            td.width = td.height = 64;
            td.format = SU_TEX_BC5_UNORM;
            td.mip_count = 1;
            td.mip_data = mips;
            td.flags = SU_RENDER_UPLOAD_COPY;
            CHECK(su_render_texture_create(g_r, &td, &t_bc) == SU_OK);
            free(bc);
            p.d.textures[SU_PAINT_SLOT_NORMAL] = t_bc;
            CHECK(shoot(&sc, &p, img_bc));
            p.d.textures[SU_PAINT_SLOT_NORMAL] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, flat);
            CHECK(shoot(&sc, &p, img_flat));
            for (y = (uint32_t)qx(0.1f); y < (uint32_t)qx(0.9f); y++) {
                for (x = (uint32_t)qx(0.1f); x < (uint32_t)qx(0.9f); x++) {
                    sum += abs((int)px(img_bc, (int)x, (int)y)[1] - (int)px(g_ref, (int)x, (int)y)[1]);
                    sum_flat += abs((int)px(img_flat, (int)x, (int)y)[1] - (int)px(g_ref, (int)x, (int)y)[1]);
                    cnt++;
                }
            }
            printf("BC5 normal: mean abs difference to RGBA8 normal %.2f (flat normal: %.2f)\n", sum / cnt, sum_flat / cnt);
            if (su_render_resource_state(g_r, t_bc) == SU_RENDER_STATE_READY) {
                CHECK(sum / cnt < sum_flat / cnt);
                CHECK(sum / cnt < 8.0);
            }
        }
    }
}
static void test_glow(void) {
    static uint8_t glow[16 * 16 * 4];
    static uint8_t mask[16 * 16 * 4];
    Scene sc;
    Pm p;
    double v[3];
    static uint8_t img[W * H * 4];
    static uint8_t off[W * H * 4];
    uint32_t x;
    uint32_t y;
    static const float mults[4] = {0.0f, 0.5f, 1.0f, 2.0f};
    uint32_t k;
    fill_rgba(glow, 16 * 16, 128, 128, 128, 255); /* linear 0.2158 */
    scene_init(&sc);
    sc.sun_intensity = 0.0f;
    sc.ambient = 0.0f;
    pm_init(&p, 0.0f);
    p.d.factors.emissive[0] = p.d.factors.emissive[1] = p.d.factors.emissive[2] = 1.0f;
    p.d.factors.emissive_strength = 2.0f;
    p.d.textures[SU_PAINT_SLOT_GLOW] = mk_tex(SU_TEX_RGBA8_SRGB, 16, 16, glow);
    /* per-instance multiplier: the linear value is map x colour x intensity x multiplier */
    for (k = 0; k < 4; k++) {
        double want = lin(128) * 2.0 * mults[k];
        sc.emissive = mults[k];
        CHECK(shoot(&sc, &p, img));
        lin_px(img, 0.5f, 0.5f, v);
        printf("glow: instance multiplier %.1f -> linear %.4f (expected %.4f)\n", mults[k], v[1], want);
        CHECK(fabs(v[0] - fminf((float)want, 1.0f)) < 0.012 && fabs(v[2] - fminf((float)want, 1.0f)) < 0.012);
    }
    /* bloom: a glow of 8 x 0.2158 lights the pixels around the quad; with the instance multiplier at 0 nothing does */
    p.d.factors.emissive_strength = 8.0f;
    sc.emissive = 1.0f;
    sc.post.flags = SU_POST_BLOOM;
    sc.post.bloom_threshold = 1.0f;
    sc.post.bloom_intensity = 1.0f;
    sc.post.bloom_radius = 1.0f;
    CHECK(shoot(&sc, &p, img));
    sc.emissive = 0.0f;
    CHECK(shoot(&sc, &p, off));
    {
        int out_x = qx(1.0f) + 6; /* just outside the quad's right edge */
        int lit = px(img, out_x, 128)[1];
        int dark = px(off, out_x, 128)[1];
        printf("glow bloom: pixel 6 px outside the surface %d with glow, %d with the multiplier at 0\n", lit, dark);
        CHECK(lit > 10);
        CHECK(dark == 0);
        CHECK(px(img, out_x + 30, 128)[1] < lit);
        sc.post.flags = 0;
        CHECK(shoot(&sc, &p, off));
        sc.emissive = 1.0f;
        CHECK(shoot(&sc, &p, off));
        CHECK(px(off, out_x, 128)[1] == 0); /* no bloom: nothing outside the surface */
    }
    /* a glow-tint layer recolours the glow it covers: left half green x 3, right half the base colour x 2 */
    memset(mask, 0, sizeof mask);
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 8; x++) {
            mask[(y * 16 + x) * 4] = 255;
        }
    }
    memset(&sc, 0, sizeof sc);
    scene_init(&sc);
    sc.sun_intensity = 0.0f;
    pm_init(&p, 0.0f);
    p.d.factors.emissive[0] = 1.0f;
    p.d.factors.emissive_strength = 2.0f;
    p.d.textures[SU_PAINT_SLOT_GLOW] = mk_tex(SU_TEX_RGBA8_SRGB, 16, 16, glow);
    p.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, mask);
    layer_set(&p, 0, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, SU_PAINT_LAYER_GLOW_TINT);
    p.d.paint.layer[0].glow_color[1] = 1.0f;
    p.d.paint.layer[0].glow_strength = 3.0f;
    p.d.paint.layer_count = 1;
    CHECK(shoot(&sc, &p, img));
    lin_px(img, 0.2f, 0.5f, v);
    printf("glow tint: left %.3f %.3f %.3f (want 0 0.647 0)\n", v[0], v[1], v[2]);
    CHECK(v[0] < 0.01 && fabs(v[1] - lin(128) * 3.0) < 0.015 && v[2] < 0.01);
    lin_px(img, 0.8f, 0.5f, v);
    printf("glow tint: right %.3f %.3f %.3f (want 0.432 0 0)\n", v[0], v[1], v[2]);
    CHECK(fabs(v[0] - lin(128) * 2.0) < 0.012 && v[1] < 0.01 && v[2] < 0.01);
}

/* A linear texture in the glow slot is a one-channel mask: glow = mask channel x colour x intensity. */
static void test_glow_mask(void) {
    static uint8_t m8[16 * 16 * 4];
    static uint8_t bc4[4 * 4 * 8];
    static uint8_t img[W * H * 4];
    SuRenderDeviceInfo dev;
    Scene sc;
    Pm p;
    double v[3];
    double want = 128.0 / 255.0 * 1.5;
    uint32_t i;
    fill_rgba(m8, 16 * 16, 0, 128, 255, 0); /* channel g = 0.502 (linear: no sRGB decode) */
    scene_init(&sc);
    sc.sun_intensity = 0.0f;
    sc.ambient = 0.0f;
    pm_init(&p, 0.0f);
    p.d.factors.emissive[0] = 1.0f;
    p.d.factors.emissive[1] = 0.5f;
    p.d.factors.emissive[2] = 0.0f;
    p.d.factors.emissive_strength = 1.5f;
    p.d.textures[SU_PAINT_SLOT_GLOW] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, m8);
    p.d.paint.channel[SU_PAINT_CH_GLOW] = 1;
    CHECK(shoot(&sc, &p, img));
    lin_px(img, 0.5f, 0.5f, v);
    printf("glow mask: RGBA8 channel g -> %.4f %.4f %.4f (want %.4f %.4f 0)\n", v[0], v[1], v[2], want, want * 0.5);
    CHECK(fabs(v[0] - want) < 0.012 && fabs(v[1] - want * 0.5) < 0.012 && v[2] < 0.005);
    p.d.paint.channel[SU_PAINT_CH_GLOW] = 3; /* alpha is 0: no glow */
    CHECK(shoot(&sc, &p, img));
    lin_px(img, 0.5f, 0.5f, v);
    CHECK(v[0] < 0.005 && v[1] < 0.005);
    p.d.paint.channel[SU_PAINT_CH_GLOW] = SU_PAINT_CH_NONE; /* the map is not read: 1 */
    CHECK(shoot(&sc, &p, img));
    lin_px(img, 0.5f, 0.5f, v);
    CHECK(v[0] > 0.99 && fabs(v[1] - 0.75) < 0.012);
    p.d.paint.channel[SU_PAINT_CH_GLOW] = 6;
    {
        SuRenderHandle m = 0;
        CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT);
    }
    /* BC4 (a constant block: both endpoints 128, every index 0, so any decoder gives exactly 128 / 255) */
    CHECK(su_render_device(g_r, &dev) == SU_OK);
    if (!dev.bc_textures) {
        printf("glow mask: BC4 skipped (no native BC sampling)\n");
        return;
    }
    memset(bc4, 0, sizeof bc4);
    for (i = 0; i < 16; i++) {
        bc4[i * 8] = 128;
        bc4[i * 8 + 1] = 128;
    }
    p.d.textures[SU_PAINT_SLOT_GLOW] = mk_tex(SU_TEX_BC4_UNORM, 16, 16, bc4);
    p.d.paint.channel[SU_PAINT_CH_GLOW] = 0;
    CHECK(shoot(&sc, &p, img));
    lin_px(img, 0.5f, 0.5f, v);
    printf("glow mask: BC4 -> %.4f %.4f %.4f (want %.4f %.4f 0)\n", v[0], v[1], v[2], want, want * 0.5);
    CHECK(fabs(v[0] - want) < 0.012 && fabs(v[1] - want * 0.5) < 0.012 && v[2] < 0.005);
}

/* Per-object non-uniform scale (SuRenderObject.scale3). */
static void test_scale3(void) {
    static uint8_t a[W * H * 4];
    static uint8_t b[W * H * 4];
    Scene sc;
    Pm p;
    SuRenderObject o;
    SuRenderHandle mat = 0;
    SuRenderHandle model;
    SuRenderHandle stretched = 0;
    SuRenderStats st;
    SuRenderModelNode mn;
    int x;
    int y;
    int x0 = W;
    int x1 = -1;
    int y0 = H;
    int y1 = -1;
    scene_init(&sc);
    sc.sun_dir[0] = 0.5f; /* oblique light, so the normal matters */
    sc.sun_dir[2] = -0.8f;
    sc.ambient = 0.2f;
    sc.sky[0] = sc.sky[1] = sc.sky[2] = 0.5f;
    pm_init(&p, 0.8f);
    CHECK(su_render_paint_material_create(g_r, &p.d, &mat) == SU_OK);
    model = quad_model(mat);
    memset(&o, 0, sizeof o);
    o.pos = su_wpos(0.0, 0.0, -6.0);
    o.rot = su_quatf_axis_angle(v3(0, 1, 0), 0.6f);
    o.scale = 1.0f;
    o.tint[0] = o.tint[1] = o.tint[2] = o.tint[3] = 1.0f;
    o.emissive = 1.0f;
    o.model = model;
    g_obj = &o;
    /* scale3 (2, 2, 2) at scale 1 is exactly scale 2 */
    o.scale = 2.0f;
    CHECK(render(&sc, model, a));
    o.scale = 1.0f;
    o.scale3[0] = o.scale3[1] = o.scale3[2] = 2.0f;
    CHECK(render(&sc, model, b));
    CHECK(memcmp(a, b, sizeof a) == 0);
    /* stretch along the local x axis: the same image as a model whose node carries diag(2.5, 0.5, 1), which the
     * renderer already draws with the inverse-transpose normals */
    o.scale3[0] = 2.5f;
    o.scale3[1] = 0.5f;
    o.scale3[2] = 1.0f;
    CHECK(render(&sc, model, a));
    memset(&mn, 0, sizeof mn);
    {
        SuAssetVertex v[4];
        uint32_t idx[6];
        SuAssetSubmesh sm;
        SuRenderMeshDesc mesh;
        SuRenderHandle msh = 0;
        quad_mesh(v, idx);
        memset(&sm, 0, sizeof sm);
        sm.index_count = 6;
        sm.radius = 1.5f;
        memset(&mesh, 0, sizeof mesh);
        mesh.vertices = v;
        mesh.vertex_count = 4;
        mesh.indices = idx;
        mesh.index_count = 6;
        mesh.submeshes = &sm;
        mesh.submesh_count = 1;
        mesh.materials = &mat;
        mesh.radius = 1.5f;
        mesh.flags = SU_RENDER_UPLOAD_COPY;
        CHECK(su_render_mesh_create(g_r, &mesh, &msh) == SU_OK);
        mn.transform[0] = 2.5f;
        mn.transform[5] = 0.5f;
        mn.transform[10] = mn.transform[15] = 1.0f;
        mn.lod_mesh[0] = msh;
        mn.lod_screen[0] = 1.0f;
        mn.lod_count = 1;
        CHECK(su_render_model_create(g_r, &mn, 1, &stretched) == SU_OK);
    }
    o.scale3[0] = o.scale3[1] = o.scale3[2] = 0.0f;
    o.model = stretched;
    CHECK(render(&sc, stretched, b));
    CHECK(memcmp(a, b, sizeof a) == 0);
    o.model = model;
    /* facing the camera, the covered box is 2.5 x 0.5 of the unit quad's (73.9 px per unit at z = -3; here z = -6) */
    o.rot.x = o.rot.y = o.rot.z = 0.0f;
    o.rot.w = 1.0f;
    o.scale3[0] = 2.5f;
    o.scale3[1] = 0.5f;
    o.scale3[2] = 1.0f;
    CHECK(render(&sc, model, a));
    for (y = 0; y < (int)H; y++) {
        for (x = 0; x < (int)W; x++) {
            if (px(a, x, y)[0] > 0) {
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y > y1 ? y : y1;
            }
        }
    }
    printf("scale3: (2.5, 0.5, 1) covers x %d..%d, y %d..%d (expected %.1f x %.1f px)\n", x0, x1, y0, y1,
           2.5 * 73.9, 0.5 * 73.9);
    CHECK(abs((x1 - x0 + 1) - (int)(2.5 * 73.9 + 0.5)) <= 2);
    CHECK(abs((y1 - y0 + 1) - (int)(0.5 * 73.9 + 0.5)) <= 2);
    /* culling uses the largest axis: a quad centred off screen but stretched into view is drawn */
    o.pos = su_wpos(4.5, 0.0, -3.0);
    o.scale3[0] = 4.0f;
    o.scale3[1] = 1.0f;
    CHECK(render(&sc, model, a));
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.objects_culled == 0u);
    CHECK(px(a, W - 4, H / 2)[0] > 0);
    o.scale3[0] = o.scale3[1] = o.scale3[2] = 0.0f; /* uniform: its bounding sphere is outside, culled */
    CHECK(render(&sc, model, a));
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.objects_culled == 1u);
    /* a zero or negative axis (with another one set) is not drawn */
    o.pos = su_wpos(0.0, 0.0, -3.0);
    o.scale3[0] = -1.0f;
    o.scale3[1] = o.scale3[2] = 1.0f;
    CHECK(render(&sc, model, a));
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.objects_culled == 1u);
    g_obj = NULL;
}

/* Directional sun shadows: a small quad between the sun and a large one shades it; off, NO_SHADOW, strength. */
static void test_shadows(void) {
    static uint8_t off[W * H * 4];
    static uint8_t on[W * H * 4];
    static uint8_t img[W * H * 4];
    Scene sc;
    Pm p;
    SuRenderObject o[2];
    SuRenderHandle mat = 0;
    SuRenderHandle pbr = 0;
    SuRenderHandle ground;
    SuRenderHandle ground_pbr;
    SuRenderMaterialDesc md;
    SuRenderStats st;
    double lit;
    double dark;
    double v[3];
    uint32_t k;
    int pass;
    scene_init(&sc);
    sc.sun_dir[0] = 0.3f; /* the shadow of a caster 2 units in front falls 0.6 units to +x */
    sc.sun_dir[2] = -1.0f;
    sc.ambient = 0.25f;
    sc.sky[0] = sc.sky[1] = sc.sky[2] = 0.4f;
    pm_init(&p, 0.8f);
    CHECK(su_render_paint_material_create(g_r, &p.d, &mat) == SU_OK);
    ground = quad_model(mat);
    memset(&md, 0, sizeof md);
    su_asset_material_default(&md.factors);
    md.factors.base_color[0] = md.factors.base_color[1] = md.factors.base_color[2] = 0.8f;
    md.factors.metallic = 0.0f;
    md.factors.roughness = 1.0f;
    CHECK(su_render_material_create(g_r, &md, &pbr) == SU_OK);
    ground_pbr = quad_model(pbr);
    memset(o, 0, sizeof o);
    for (k = 0; k < 2u; k++) {
        o[k].rot.w = 1.0f;
        o[k].tint[0] = o[k].tint[1] = o[k].tint[2] = o[k].tint[3] = 1.0f;
        o[k].emissive = 1.0f;
        o[k].model = ground;
    }
    o[0].pos = su_wpos(0.0, 0.0, -6.0); /* the receiver: 6 x 6 units facing the camera */
    o[0].scale = 3.0f;
    o[1].pos = su_wpos(-0.6, 0.0, -4.0); /* the caster: 1 x 1, its shadow centred on the receiver */
    o[1].scale = 0.5f;
    g_obj = o;
    g_obj_count = 2;
    for (pass = 0; pass < 2; pass++) { /* paint receiver, then PBR-lite receiver */
        o[0].model = pass == 0 ? ground : ground_pbr;
        memset(&sc.shadow, 0, sizeof sc.shadow);
        CHECK(render(&sc, 0, off));
        CHECK(su_render_stats(g_r, &st) == SU_OK && st.shadow_size == 0u && st.shadow_casters == 0u);
        sc.shadow.flags = SU_SHADOW_ON;
        CHECK(render(&sc, 0, on));
        CHECK(su_render_stats(g_r, &st) == SU_OK);
        printf("shadow (%s receiver): map %u, %u casters, %u draws, texel %.4f units\n", pass == 0 ? "paint" : "PBR-lite",
               (unsigned)st.shadow_size, (unsigned)st.shadow_casters, (unsigned)st.shadow_draws, (double)st.shadow_texel);
        CHECK(st.shadow_size == 2048u && st.shadow_casters == 2u && st.shadow_draws >= 1u && st.shadow_texel > 0.0f);
        lin_px(off, 0.5f, 0.5f, v);
        lit = v[1];
        lin_px(on, 0.5f, 0.5f, v);
        dark = v[1];
        printf("shadow: centre of the receiver %.4f lit, %.4f in the shadow\n", lit, dark);
        CHECK(dark < lit * 0.5);
        /* far from the shadow (right part of the receiver) nothing changes */
        CHECK(same_px(px(on, 128 + 70, 128), px(off, 128 + 70, 128)));
        CHECK(same_px(px(on, 128 - 18, 128 + 70), px(off, 128 - 18, 128 + 70)));
        /* the caster's own lit face is unchanged */
        CHECK(same_px(px(on, 128 - 33, 128), px(off, 128 - 33, 128)));
        /* strength 0.5: half of the sun's contribution is removed */
        sc.shadow.strength = 0.5f;
        CHECK(render(&sc, 0, img));
        lin_px(img, 0.5f, 0.5f, v);
        printf("shadow: strength 0.5 -> %.4f (expected about %.4f)\n", v[1], (lit + dark) * 0.5);
        CHECK(fabs(v[1] - (lit + dark) * 0.5) < 0.02);
        sc.shadow.strength = 0.0f;
        /* a caster flagged NO_SHADOW throws none */
        o[1].flags = SU_RENDER_OBJECT_NO_SHADOW;
        CHECK(render(&sc, 0, img));
        CHECK(su_render_stats(g_r, &st) == SU_OK && st.shadow_casters == 1u);
        CHECK(same_px(px(img, 128, 128), px(off, 128, 128)));
        o[1].flags = 0;
        /* translucent objects do not cast either */
        o[1].tint[3] = 0.5f;
        CHECK(render(&sc, 0, img));
        CHECK(su_render_stats(g_r, &st) == SU_OK && st.shadow_casters == 1u);
        o[1].tint[3] = 1.0f;
        /* a fitted slice that ends before the receiver: no receivers, no shadow */
        sc.shadow.distance = 2.0f;
        CHECK(render(&sc, 0, img));
        CHECK(same_px(px(img, 128, 128), px(off, 128, 128)));
        /* a slice that reaches it, at another map size */
        sc.shadow.distance = 20.0f;
        sc.shadow.size = 1024;
        CHECK(render(&sc, 0, img));
        CHECK(su_render_stats(g_r, &st) == SU_OK && st.shadow_size == 1024u);
        lin_px(img, 0.5f, 0.5f, v);
        CHECK(v[1] < lit * 0.5);
        sc.shadow.distance = 0.0f;
        sc.shadow.size = 0;
        /* back off: identical to never having had shadows */
        memset(&sc.shadow, 0, sizeof sc.shadow);
        CHECK(render(&sc, 0, img));
        CHECK(memcmp(img, off, sizeof img) == 0);
    }
    /* a size that is not a power of two in 256..8192 is refused */
    sc.shadow.flags = SU_SHADOW_ON;
    sc.shadow.size = 300;
    {
        SuRenderFrameDesc f;
        memset(&f, 0, sizeof f);
        f.struct_size = sizeof f;
        f.camera.rot.w = 1.0f;
        f.camera.fov_y = FOV;
        f.camera.near_z = 0.05f;
        f.shadow = sc.shadow;
        CHECK(su_render_frame_ex(g_r, &f) == SU_E_INVALID_ARGUMENT);
    }
    g_obj = NULL;
    g_obj_count = 1;
}

/* Cascaded shadow maps: walls at 6, 40 and 150 units, each shadowed by its own caster; the cascades' texels grow with
 * distance and the nearest is much finer than a single map over the same range. */
static void test_shadow_cascades(void) {
    static uint8_t off[W * H * 4];
    static uint8_t on[W * H * 4];
    static const double ds[3] = {6.0, 40.0, 150.0};   /* wall distances */
    static const double sxs[3] = {60.0, 128.0, 196.0}; /* their screen columns */
    Scene sc;
    Pm p;
    SuRenderObject o[6];
    SuRenderHandle mat = 0;
    SuRenderHandle quad;
    SuRenderStats st;
    float single_texel;
    double f = 128.0 / tan(FOV * 0.5);
    uint32_t k;
    uint32_t c;
    scene_init(&sc);
    sc.sun_dir[0] = 1.0f; /* travelling away from the camera and to +x: a caster g in front shadows g to its right */
    sc.sun_dir[2] = -1.0f;
    sc.sun_intensity = 3.0f;
    sc.ambient = 0.25f;
    sc.sky[0] = sc.sky[1] = sc.sky[2] = 0.4f;
    pm_init(&p, 0.8f);
    CHECK(su_render_paint_material_create(g_r, &p.d, &mat) == SU_OK);
    quad = quad_model(mat);
    memset(o, 0, sizeof o);
    for (k = 0; k < 6u; k++) {
        o[k].rot.w = 1.0f;
        o[k].tint[0] = o[k].tint[1] = o[k].tint[2] = o[k].tint[3] = 1.0f;
        o[k].emissive = 1.0f;
        o[k].model = quad;
    }
    for (k = 0; k < 3u; k++) {
        double d = ds[k];
        double x = (sxs[k] - 128.0) / f * d;
        double g = 0.25 * d;
        o[k].pos = su_wpos(x, 0.0, -d); /* a wall facing the camera, 0.12 d half size */
        o[k].scale = (float)(0.12 * d);
        o[3u + k].pos = su_wpos(x - g, 0.0, -(d - g)); /* its caster, g in front: the shadow lands on the wall's centre */
        o[3u + k].scale = (float)(0.03 * d);
    }
    g_obj = o;
    g_obj_count = 6;
    memset(&sc.shadow, 0, sizeof sc.shadow);
    CHECK(render(&sc, 0, off));
    sc.shadow.flags = SU_SHADOW_ON;
    sc.shadow.distance = 200.0f;
    CHECK(render(&sc, 0, on));
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.shadow_cascades == 1u);
    single_texel = st.shadow_texel;
    sc.shadow.cascades = 4;
    CHECK(render(&sc, 0, on));
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    printf("cascades: %u, texels %.4f %.4f %.4f %.4f units (single map over the same range %.4f), %u caster instances\n",
           (unsigned)st.shadow_cascades, (double)st.shadow_cascade_texel[0], (double)st.shadow_cascade_texel[1],
           (double)st.shadow_cascade_texel[2], (double)st.shadow_cascade_texel[3], (double)single_texel,
           (unsigned)st.shadow_casters);
    CHECK(st.shadow_cascades == 4u && st.shadow_size == 2048u);
    for (c = 1; c < 4u; c++) {
        CHECK(st.shadow_cascade_texel[c] > st.shadow_cascade_texel[c - 1u]);
    }
    CHECK(st.shadow_cascade_texel[0] * 3.0f < single_texel);
    for (k = 0; k < 3u; k++) {
        int sx = (int)sxs[k];
        double lit = lin(px(off, sx, 128)[1]);
        double dark = lin(px(on, sx, 128)[1]);
        printf("cascades: wall at %.0f units, shadow centre %.4f lit, %.4f shadowed\n", ds[k], lit, dark);
        CHECK(dark < lit * 0.5);
        CHECK(same_px(px(on, sx, 128 + (int)(0.09 * f)), px(off, sx, 128 + (int)(0.09 * f)))); /* below the shadow */
    }
    /* cascades 1 is the single map; invalid counts and lambdas are refused */
    sc.shadow.cascades = 1;
    CHECK(render(&sc, 0, on));
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.shadow_cascades == 1u && st.shadow_texel == single_texel);
    {
        SuRenderFrameDesc fd;
        memset(&fd, 0, sizeof fd);
        fd.struct_size = sizeof fd;
        fd.camera.rot.w = 1.0f;
        fd.camera.fov_y = FOV;
        fd.camera.near_z = 0.05f;
        fd.shadow.flags = SU_SHADOW_ON;
        fd.shadow.cascades = 5;
        CHECK(su_render_frame_ex(g_r, &fd) == SU_E_INVALID_ARGUMENT);
        fd.shadow.cascades = 2;
        fd.shadow.split_lambda = 1.5f;
        CHECK(su_render_frame_ex(g_r, &fd) == SU_E_INVALID_ARGUMENT);
    }
    memset(&sc.shadow, 0, sizeof sc.shadow);
    CHECK(render(&sc, 0, on));
    CHECK(memcmp(on, off, sizeof on) == 0);
    g_obj = NULL;
    g_obj_count = 1;
}

/* Optional cost measurement (SUTEKH_SHADOW_BENCH=1): a 1920 x 1080 frame of 400 sphere objects (about 300 k triangles)
 * on a floor, GPU time per frame with shadows off, one 2048 map, and 2 / 4 cascades of 2048 and 1024. Prints only. */
static void bench_shadows(void) {
    SuRenderer *r = NULL;
    SuRenderDesc d;
    char msg[256];
    SuMeshVertex mv[2048];
    uint16_t mi[8192];
    static SuAssetVertex av[2048];
    static uint32_t ai[8192];
    uint32_t nv = 0;
    uint32_t ni = 0;
    uint32_t i;
    SuAssetSubmesh sm;
    SuRenderMeshDesc mesh;
    SuRenderModelNode mn;
    SuRenderHandle msh = 0;
    SuRenderHandle model = 0;
    SuRenderHandle mat = 0;
    SuRenderHandle floor_model;
    SuRenderMaterialDesc md;
    static SuRenderObject objs[401];
    SuRenderFrameDesc f;
    static const struct {
        uint32_t on, size, cascades;
        const char *name;
    } cases[6] = {{0, 0, 0, "off"}, {1, 2048, 0, "1 map 2048"}, {1, 2048, 2, "2 cascades 2048"},
                  {1, 2048, 4, "4 cascades 2048"}, {1, 1024, 4, "4 cascades 1024"}, {1, 1024, 2, "2 cascades 1024"}};
    uint32_t cs;
    if (getenv("SUTEKH_SHADOW_BENCH") == NULL) {
        return;
    }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.width = 1920;
    d.height = 1080;
    d.device_index = -1;
    if (su_render_create(&d, &r, msg) != SU_OK) {
        printf("bench: no renderer: %s\n", msg);
        return;
    }
    CHECK(su_mesh_build(SU_MESH_SPHERE, mv, 2048, &nv, mi, 8192, &ni) == SU_OK);
    for (i = 0; i < nv; i++) {
        memset(&av[i], 0, sizeof av[i]);
        memcpy(av[i].pos, mv[i].pos, sizeof av[i].pos);
        memcpy(av[i].normal, mv[i].normal, sizeof av[i].normal);
        av[i].tangent[0] = av[i].tangent[3] = 1.0f;
    }
    for (i = 0; i < ni; i++) {
        ai[i] = mi[i];
    }
    memset(&md, 0, sizeof md);
    su_asset_material_default(&md.factors);
    md.factors.metallic = 0.0f;
    CHECK(su_render_material_create(r, &md, &mat) == SU_OK);
    memset(&sm, 0, sizeof sm);
    sm.index_count = ni;
    sm.radius = 0.5f;
    memset(&mesh, 0, sizeof mesh);
    mesh.vertices = av;
    mesh.vertex_count = nv;
    mesh.indices = ai;
    mesh.index_count = ni;
    mesh.submeshes = &sm;
    mesh.submesh_count = 1;
    mesh.materials = &mat;
    mesh.radius = 0.5f;
    mesh.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_mesh_create(r, &mesh, &msh) == SU_OK);
    memset(&mn, 0, sizeof mn);
    mn.transform[0] = mn.transform[5] = mn.transform[10] = mn.transform[15] = 1.0f;
    mn.lod_mesh[0] = msh;
    mn.lod_screen[0] = 1.0f;
    mn.lod_count = 1;
    CHECK(su_render_model_create(r, &mn, 1, &model) == SU_OK);
    {
        SuAssetVertex qv[4];
        uint32_t qi[6];
        SuRenderHandle qm = 0;
        quad_mesh(qv, qi);
        memset(&sm, 0, sizeof sm);
        sm.index_count = 6;
        sm.radius = 1.5f;
        mesh.vertices = qv;
        mesh.vertex_count = 4;
        mesh.indices = qi;
        mesh.index_count = 6;
        mesh.radius = 1.5f;
        CHECK(su_render_mesh_create(r, &mesh, &qm) == SU_OK);
        mn.lod_mesh[0] = qm;
        CHECK(su_render_model_create(r, &mn, 1, &floor_model) == SU_OK);
    }
    memset(objs, 0, sizeof objs);
    for (i = 0; i < 401u; i++) {
        objs[i].rot.w = 1.0f;
        objs[i].tint[0] = objs[i].tint[1] = objs[i].tint[2] = objs[i].tint[3] = 1.0f;
        objs[i].emissive = 1.0f;
        objs[i].model = model;
        objs[i].scale = 1.5f;
        objs[i].pos = su_wpos((double)((int)(i % 20u) - 10) * 4.0, -1.0, -5.0 - (double)(i / 20u) * 6.0);
    }
    objs[400].model = floor_model;
    objs[400].pos = su_wpos(0.0, -2.0, -70.0);
    objs[400].rot = su_quatf_axis_angle(v3(1, 0, 0), -1.5707963f);
    objs[400].scale = 80.0f;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.camera.pos = su_wpos(0.0, 3.0, 0.0);
    f.camera.rot = su_quatf_axis_angle(v3(1, 0, 0), -0.25f);
    f.camera.fov_y = FOV;
    f.camera.near_z = 0.1f;
    f.light.sun_dir = v3(0.5f, -1.0f, -0.3f);
    f.light.sun_intensity = 3.0f;
    f.light.sun_color = v3(1, 1, 1);
    f.light.ambient = 0.2f;
    f.light.sky_zenith = f.light.sky_horizon = f.light.sky_nadir = v3(0.3f, 0.3f, 0.4f);
    f.light.tonemap = SU_TONEMAP_ACES;
    f.objects = objs;
    f.object_count = 401;
    for (cs = 0; cs < 6u; cs++) {
        double sum = 0.0;
        int k;
        int got = 0;
        SuRenderStats st;
        memset(&f.shadow, 0, sizeof f.shadow);
        f.shadow.flags = cases[cs].on ? SU_SHADOW_ON : 0u;
        f.shadow.size = cases[cs].size;
        f.shadow.cascades = cases[cs].cascades;
        f.shadow.distance = 150.0f;
        for (k = 0; k < 40; k++) {
            CHECK(su_render_frame_ex(r, &f) == SU_OK);
            CHECK(su_render_stats(r, &st) == SU_OK);
            if (k >= 10 && st.gpu_ms >= 0.0) {
                sum += st.gpu_ms;
                got++;
            }
        }
        printf("bench 1920x1080, 400 spheres (%u triangles each): %-16s GPU %.3f ms/frame, %u caster instances\n",
               (unsigned)(ni / 3u), cases[cs].name, got > 0 ? sum / got : -1.0, (unsigned)st.shadow_casters);
    }
    su_render_destroy(r);
}

static void test_dirt_ao(void) {
    static uint8_t dirt[16 * 16 * 4];
    static uint8_t ao[16 * 16 * 4];
    Scene sc;
    Pm p;
    static uint8_t black[W * H * 4];
    static uint8_t clean[W * H * 4];
    static uint8_t img[W * H * 4];
    double pk[3];
    double pc[3];
    double pd[3];
    double prev = 2.0;
    uint32_t x;
    uint32_t y;
    static const float amounts[4] = {0.0f, 0.25f, 0.5f, 1.0f};
    uint32_t k;
    scene_init(&sc);
    memset(dirt, 0, sizeof dirt);
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 8; x++) {
            dirt[(y * 16 + x) * 4] = 255; /* the left half is dirty */
        }
        for (x = 0; x < 16; x++) {
            dirt[(y * 16 + x) * 4 + 3] = 255;
        }
    }
    pm_init(&p, 0.0f);
    CHECK(shoot(&sc, &p, black));
    pm_init(&p, 0.8f);
    CHECK(shoot(&sc, &p, clean));
    lin_px(black, 0.25f, 0.5f, pk);
    lin_px(clean, 0.25f, 0.5f, pc);
    p.d.textures[SU_PAINT_SLOT_DIRT] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, dirt);
    p.d.paint.dirt_darken = 0.6f;
    p.d.paint.dirt_roughen = 0.5f;
    for (k = 0; k < 4; k++) {
        double ratio;
        p.d.paint.dirt_amount = amounts[k];
        CHECK(shoot(&sc, &p, img));
        lin_px(img, 0.25f, 0.5f, pd);
        ratio = (pd[1] - pk[1]) / (pc[1] - pk[1]);
        printf("dirt: amount %.2f darkens the dirty half to %.3f of clean (expected %.3f)\n", amounts[k], ratio,
               1.0 - amounts[k] * 0.6);
        CHECK(fabs(ratio - (1.0 - amounts[k] * 0.6)) < 0.03);
        CHECK(ratio < prev + 1e-9);
        prev = ratio;
        lin_px(img, 0.75f, 0.5f, pd); /* the clean half does not change */
        CHECK(fabs(pd[1] - pc[1]) < 1e-9);
        if (k == 0) {
            CHECK(same_px(px(img, qx(0.25f), 128), px(clean, qx(0.25f), 128)));
        }
    }
    /* AO scales ambient only */
    fill_rgba(ao, 16 * 16, 64, 64, 64, 255); /* 0.251 */
    memset(&sc, 0, sizeof sc);
    scene_init(&sc);
    sc.sun_intensity = 0.0f;
    sc.ambient = 1.0f;
    sc.sky[0] = sc.sky[1] = sc.sky[2] = 0.5f;
    pm_init(&p, 0.8f);
    CHECK(shoot(&sc, &p, clean));
    p.d.textures[SU_PAINT_SLOT_AO] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, ao);
    CHECK(shoot(&sc, &p, img));
    lin_px(clean, 0.5f, 0.5f, pc);
    lin_px(img, 0.5f, 0.5f, pd);
    printf("ao: ambient x %.3f (map 0.251, strength 1)\n", pd[1] / pc[1]);
    CHECK(fabs(pd[1] / pc[1] - 64.0 / 255.0) < 0.02);
    p.d.factors.occlusion_strength = 0.5f;
    CHECK(shoot(&sc, &p, img));
    lin_px(img, 0.5f, 0.5f, pd);
    printf("ao: ambient x %.3f (strength 0.5, expected %.3f)\n", pd[1] / pc[1], 1.0 - 0.5 * (1.0 - 64.0 / 255.0));
    CHECK(fabs(pd[1] / pc[1] - (1.0 - 0.5 * (1.0 - 64.0 / 255.0))) < 0.02);
    p.d.factors.occlusion_strength = 1.0f;
    sc.sun_intensity = 3.0f;
    sc.ambient = 0.0f;
    pm_init(&p, 0.8f);
    CHECK(shoot(&sc, &p, clean));
    p.d.textures[SU_PAINT_SLOT_AO] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, ao);
    CHECK(shoot(&sc, &p, img));
    CHECK(same_px(px(img, 128, 128), px(clean, 128, 128))); /* direct light is not occluded */
}

/* A bright patch of 20 around +Z (toward the camera) on a dark cube. */
static float *patch_faces(uint32_t size) {
    float *f = malloc((size_t)6u * size * size * 4u * sizeof *f);
    uint32_t i;
    uint32_t x;
    uint32_t y;
    if (f == NULL) {
        return NULL;
    }
    for (i = 0; i < 6u; i++) {
        for (y = 0; y < size; y++) {
            for (x = 0; x < size; x++) {
                SuVec3f d = su_env_texel_dir(i, size, x, y);
                float v = d.z > 0.985f ? 20.0f : 0.02f;
                float *o = &f[(((size_t)i * size + y) * size + x) * 4u];
                o[0] = o[1] = o[2] = v;
                o[3] = 1.0f;
            }
        }
    }
    return f;
}

static void test_environment(void) {
    Scene sc;
    Pm p;
    SuRenderHandle env = 0;
    SuRenderHandle uni = 0;
    SuRenderLighting sky;
    static uint8_t img[W * H * 4];
    float *faces;
    float *flat;
    uint32_t k;
    uint32_t i;
    double prev = 1e9;
    static const float roughs[5] = {0.1f, 0.3f, 0.5f, 0.7f, 0.9f};
    double v0;
    double v1;
    double v2;
    faces = patch_faces(32);
    flat = malloc((size_t)6u * 32u * 32u * 4u * sizeof *flat);
    CHECK(faces != NULL && flat != NULL);
    if (faces == NULL || flat == NULL) {
        free(faces);
        free(flat);
        return;
    }
    CHECK(su_render_env_create(g_r, 32, faces, &env) == SU_OK);
    for (i = 0; i < 6u * 32u * 32u; i++) {
        flat[i * 4u] = 0.25f;
        flat[i * 4u + 1u] = 0.5f;
        flat[i * 4u + 2u] = 0.75f;
        flat[i * 4u + 3u] = 1.0f;
    }
    CHECK(su_render_env_create(g_r, 32, flat, &uni) == SU_OK);
    {
        float dummy[4 * 6 * 4 * 4] = {0};
        SuRenderHandle bad = 0;
        CHECK(su_render_env_create(g_r, 4, dummy, &bad) == SU_E_INVALID_ARGUMENT);
        CHECK(su_render_env_create(g_r, 48, faces, &bad) == SU_E_INVALID_ARGUMENT);
        CHECK(su_render_env_create(g_r, 32, NULL, &bad) == SU_E_INVALID_ARGUMENT);
    }
    scene_init(&sc);
    sc.sun_intensity = 0.0f;
    sc.ambient = 1.0f;
    sc.exposure = 0.03f;
    sc.env = env;
    sc.env_intensity = 1.0f;
    /* a metal quad reflects the patch at its centre; the reflection is brighter the smoother the surface */
    for (k = 0; k < 5; k++) {
        pm_init(&p, 1.0f);
        p.d.factors.metallic = 1.0f;
        p.d.factors.roughness = roughs[k];
        CHECK(shoot(&sc, &p, img));
        v0 = lin(px(img, 128, 128)[1]);
        printf("env: metal roughness %.1f -> centre linear %.4f (byte %d)\n", roughs[k], v0, px(img, 128, 128)[1]);
        CHECK(v0 < prev);
        prev = v0;
    }
    /* intensity scales the light linearly (below clipping); no environment falls back to the (dark) sky ambient */
    pm_init(&p, 1.0f);
    p.d.factors.metallic = 1.0f;
    p.d.factors.roughness = 0.5f;
    CHECK(shoot(&sc, &p, img));
    v0 = lin(px(img, 128, 128)[1]);
    sc.env_intensity = 2.0f;
    CHECK(shoot(&sc, &p, img));
    v1 = lin(px(img, 128, 128)[1]);
    sc.env = 0;
    CHECK(shoot(&sc, &p, img));
    v2 = lin(px(img, 128, 128)[1]);
    printf("env: intensity 1 -> %.4f, 2 -> %.4f; no environment %.4f\n", v0, v1, v2);
    CHECK(fabs(v1 / v0 - 2.0) < 0.1);
    CHECK(v2 < v0 * 0.2);
    /* layer metalness and roughness: a smooth metal layer over a rough dielectric brightens the reflection */
    {
        static uint8_t mask[8 * 8 * 4];
        double lo;
        double hi;
        fill_rgba(mask, 64, 255, 0, 0, 0);
        sc.env = env;
        sc.env_intensity = 1.0f;
        pm_init(&p, 1.0f);
        CHECK(shoot(&sc, &p, img));
        lo = lin(px(img, 128, 128)[1]);
        p.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 8, 8, mask);
        layer_set(&p, 0, 1.0f, 1.0f, 1.0f, 0.15f, 1.0f, 1.0f, 0);
        p.d.paint.layer_count = 1;
        CHECK(shoot(&sc, &p, img));
        hi = lin(px(img, 128, 128)[1]);
        printf("env: rough dielectric %.4f, with a smooth metal layer %.4f\n", lo, hi);
        CHECK(hi > lo * 3.0);
        /* dirt roughens: the same smooth metal with full dirt reflects less */
        p.d.paint.dirt_amount = 1.0f;
        p.d.paint.dirt_darken = 0.0f;
        p.d.paint.dirt_roughen = 1.0f;
        CHECK(shoot(&sc, &p, img));
        v0 = lin(px(img, 128, 128)[1]);
        printf("env: with full dirt (darken 0, roughen 1) %.4f\n", v0);
        CHECK(v0 < hi * 0.5);
    }
    /* a uniform environment lights a rough dielectric with its colour: r : g : b = 1 : 2 : 3 */
    sc.env = uni;
    sc.exposure = 1.0f;
    pm_init(&p, 1.0f);
    p.d.factors.roughness = 1.0f;
    CHECK(shoot(&sc, &p, img));
    {
        double r = lin(px(img, 128, 128)[0]);
        double g = lin(px(img, 128, 128)[1]);
        double b = lin(px(img, 128, 128)[2]);
        printf("env: uniform (0.25, 0.5, 0.75) lights a dielectric to %.3f %.3f %.3f\n", r, g, b);
        CHECK(fabs(g / r - 2.0) < 0.12 && fabs(b / r - 3.0) < 0.2);
    }
    /* the procedural sky baked to a cube */
    memset(&sky, 0, sizeof sky);
    sky.sun_dir = v3(0, 0, -1);
    sky.sun_intensity = 5.0f;
    sky.sun_color = v3(1, 1, 1);
    sky.sky_zenith = v3(0.1f, 0.2f, 0.6f);
    sky.sky_horizon = v3(0.5f, 0.5f, 0.5f);
    sky.sky_nadir = v3(0.05f, 0.05f, 0.05f);
    sky.up = v3(0, 1, 0);
    CHECK(su_render_env_from_sky(g_r, 32, &sky, &sc.env) == SU_OK);
    sc.env_intensity = 1.0f;
    CHECK(shoot(&sc, &p, img));
    CHECK(su_render_resource_state(g_r, sc.env) == SU_RENDER_STATE_READY);
    printf("env: sky cube lights the quad to %d %d %d\n", px(img, 128, 128)[0], px(img, 128, 128)[1], px(img, 128, 128)[2]);
    CHECK(px(img, 128, 128)[0] > 20);
    CHECK(su_render_release(g_r, sc.env) == SU_OK);
    CHECK(su_render_release(g_r, env) == SU_OK);
    CHECK(su_render_release(g_r, uni) == SU_OK);
    /* a released handle is just ignored by a frame */
    sc.env = env;
    CHECK(shoot(&sc, &p, img));
    free(faces);
    free(flat);
}

static void test_color_spaces(void) {
    static uint8_t t4[4 * 4 * 4];
    Pm p;
    SuRenderHandle srgb;
    SuRenderHandle unorm;
    SuRenderHandle m = 0;
    uint32_t s;
    fill_rgba(t4, 16, 100, 100, 100, 255);
    srgb = mk_tex(SU_TEX_RGBA8_SRGB, 4, 4, t4);
    unorm = mk_tex(SU_TEX_RGBA8_UNORM, 4, 4, t4);
    for (s = 0; s < SU_PAINT_SLOT_COUNT; s++) {
        int wants_srgb = s == SU_PAINT_SLOT_ALBEDO || s == SU_PAINT_SLOT_GLOW;
        pm_init(&p, 1.0f);
        p.d.textures[s] = wants_srgb ? unorm : srgb;
        m = 0;
        if (s == SU_PAINT_SLOT_GLOW) { /* either kind: a linear glow texture is a mask */
            CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_OK && m != 0u);
            CHECK(su_render_release(g_r, m) == SU_OK);
        } else {
            CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT && m == 0u);
        }
        p.d.textures[s] = wants_srgb ? srgb : unorm;
        CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_OK && m != 0u);
        CHECK(su_render_release(g_r, m) == SU_OK);
    }
    pm_init(&p, 1.0f);
    p.d.paint.layer_count = 5;
    CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT);
    pm_init(&p, 1.0f);
    p.d.paint.layer_count = 2;
    p.d.paint.layer_order[0] = p.d.paint.layer_order[1] = 1;
    CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT);
    pm_init(&p, 1.0f);
    p.d.paint.layer_count = 1;
    p.d.paint.layer[0].flags = SU_PAINT_LAYER_MULTIPLY | SU_PAINT_LAYER_TINT; /* two blends at once */
    CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT);
    p.d.paint.layer[0].flags = SU_PAINT_LAYER_TINT | SU_PAINT_LAYER_GLOW_TINT;
    CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_OK);
    CHECK(su_render_release(g_r, m) == SU_OK);
    p.d.paint.layer[0].flags = 0x8u;
    CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT);
    pm_init(&p, 1.0f);
    p.d.paint.channel[SU_PAINT_CH_DIRT] = 5;
    CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT);
    pm_init(&p, 1.0f);
    p.d.paint.dirt_amount = NAN;
    CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT);
    pm_init(&p, 1.0f);
    p.d.textures[SU_PAINT_SLOT_AO] = 0x1234567890ull;
    CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT);
    printf("colour spaces: every slot refuses the wrong kind and accepts the right one\n");
}

/* A pack holding a paint material renders like the same material created through the API. */
static void test_pack(void) {
    static uint8_t mask[16 * 16 * 4];
    static uint8_t alb[16 * 16 * 4];
    SuAssetWriter *w = NULL;
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuAssetMaterial fm;
    SuAssetPaint ap;
    SuAssetModelNode node;
    const uint8_t *mips[1];
    uint8_t *bytes = NULL;
    size_t len = 0;
    SuAssetPak *pak = NULL;
    SuRenderPak rp;
    char msg[256];
    Scene sc;
    Pm p;
    static uint8_t viapak[W * H * 4];
    static uint8_t direct[W * H * 4];
    uint64_t id_alb = su_asset_id("t/albedo");
    uint64_t id_mask = su_asset_id("t/mask");
    uint32_t x;
    uint32_t y;
    uint32_t st;
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            uint8_t *m = &mask[(y * 16 + x) * 4];
            m[0] = x < 8 ? 255 : 0;
            m[1] = x >= 8 && y < 8 ? 255 : 0;
            m[2] = m[3] = 0;
            alb[(y * 16 + x) * 4] = (uint8_t)(100 + x * 6);
            alb[(y * 16 + x) * 4 + 1] = (uint8_t)(100 + y * 6);
            alb[(y * 16 + x) * 4 + 2] = 120;
            alb[(y * 16 + x) * 4 + 3] = 255;
        }
    }
    su_asset_material_default(&fm);
    fm.metallic = 0.0f;
    su_asset_paint_default(&ap);
    ap.texture[SU_PAINT_SLOT_ALBEDO] = id_alb;
    ap.texture[SU_PAINT_SLOT_MASK] = id_mask;
    ap.layer_count = 2;
    ap.layer[0].color[0] = 0.9f;
    ap.layer[0].color[1] = 0.1f;
    ap.layer[0].color[2] = 0.1f;
    ap.layer[1].color[0] = 0.1f;
    ap.layer[1].color[1] = 0.1f;
    ap.layer[1].color[2] = 0.9f;
    ap.layer[1].opacity = 0.7f;
    quad_mesh(v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6;
    sm.radius = 1.5f;
    sm.material = su_asset_id("t/mat");
    memset(&node, 0, sizeof node);
    node.transform[0] = node.transform[5] = node.transform[10] = node.transform[15] = 1.0f;
    node.lod_mesh[0] = su_asset_id("t/mesh");
    node.lod_screen[0] = 1.0f;
    node.lod_count = 1;
    CHECK(su_asset_writer_create(&w) == SU_OK);
    mips[0] = alb;
    CHECK(su_asset_writer_add_texture(w, id_alb, 16, 16, SU_TEX_RGBA8_SRGB, 0, 1, mips) == SU_OK);
    mips[0] = mask;
    CHECK(su_asset_writer_add_texture(w, id_mask, 16, 16, SU_TEX_RGBA8_UNORM, 0, 1, mips) == SU_OK);
    CHECK(su_asset_writer_add_paint_material(w, sm.material, &fm, &ap) == SU_OK);
    CHECK(su_asset_writer_add_mesh(w, node.lod_mesh[0], v, 4, idx, 6, &sm, 1) == SU_OK);
    CHECK(su_asset_writer_add_model(w, su_asset_id("t/model"), &node, 1) == SU_OK);
    CHECK(su_asset_writer_finish(w, &bytes, &len) == SU_OK);
    su_asset_writer_destroy(w);
    CHECK(su_asset_pak_open(bytes, len, SU_ASSET_PAK_VERIFY_ENTRIES, &pak, msg) == SU_OK);
    if (pak == NULL) {
        free(bytes);
        return;
    }
    {
        SuAssetPaint back;
        SuAssetMaterial plain;
        CHECK(su_asset_pak_paint(pak, sm.material, &back) == SU_OK && memcmp(&back, &ap, sizeof back) == 0);
        CHECK(su_asset_pak_material(pak, sm.material, &plain) == SU_OK && plain.metallic == 0.0f);
        CHECK(su_asset_pak_paint(pak, id_alb, &back) == SU_E_INVALID_ARGUMENT);
    }
    scene_init(&sc);
    st = su_render_upload_pak(g_r, pak, 0, &rp);
    CHECK(st == SU_OK);
    if (st == SU_OK) {
        int tries;
        for (tries = 0; tries < 8 && su_render_pak_state(g_r, &rp) == SU_RENDER_STATE_PENDING; tries++) {
            (void)render(&sc, rp.model, viapak);
        }
        CHECK(su_render_pak_state(g_r, &rp) == SU_RENDER_STATE_READY);
        CHECK(render(&sc, rp.model, viapak));
    }
    pm_init(&p, 1.0f);
    p.d.factors = fm;
    p.d.paint = ap;
    p.d.textures[SU_PAINT_SLOT_ALBEDO] = mk_tex(SU_TEX_RGBA8_SRGB, 16, 16, alb);
    p.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, mask);
    CHECK(shoot(&sc, &p, direct));
    CHECK(memcmp(viapak, direct, sizeof viapak) == 0);
    printf("pack: paint material through a cooked pack renders identically to the API (%s)\n",
           memcmp(viapak, direct, sizeof viapak) == 0 ? "identical" : "DIFFERENT");
    CHECK(!same_px(px(direct, qx(0.25f), qx(0.25f)), px(direct, qx(0.75f), qx(0.75f))));
    su_render_pak_release(g_r, &rp);
    su_asset_pak_close(pak);
    free(bytes);
    /* a pack with a texture of the wrong colour space does not open */
    {
        SuAssetWriter *w2 = NULL;
        uint8_t *b2 = NULL;
        size_t l2 = 0;
        SuAssetPak *p2 = NULL;
        SuAssetPaint bad = ap;
        bad.texture[SU_PAINT_SLOT_ALBEDO] = id_mask; /* a linear texture in an sRGB slot */
        CHECK(su_asset_writer_create(&w2) == SU_OK);
        mips[0] = mask;
        CHECK(su_asset_writer_add_texture(w2, id_mask, 16, 16, SU_TEX_RGBA8_UNORM, 0, 1, mips) == SU_OK);
        mips[0] = alb;
        CHECK(su_asset_writer_add_texture(w2, id_alb, 16, 16, SU_TEX_RGBA8_SRGB, 0, 1, mips) == SU_OK);
        CHECK(su_asset_writer_add_paint_material(w2, sm.material, &fm, &bad) == SU_OK);
        CHECK(su_asset_writer_finish(w2, &b2, &l2) == SU_OK);
        su_asset_writer_destroy(w2);
        msg[0] = '\0';
        CHECK(su_asset_pak_open(b2, l2, 0, &p2, msg) == SU_E_PARSE && p2 == NULL);
        printf("pack: wrong colour space rejected: %s\n", msg);
        free(b2);
    }
}

static void test_specular_color(void) {
    Pm paint;
    Scene scene;
    uint8_t texel[4] = {64, 128, 192, 255};
    uint8_t black[4] = {0, 0, 0, 255};
    uint8_t normal[4] = {204, 128, 255, 255};
    uint8_t spatial[16 * 16 * 4];
    float faces[6 * 8 * 8 * 4];
    double measured[3];
    uint32_t channel;
    uint32_t srgb;
    scene_init(&scene);
    for (srgb = 0; srgb < 2; srgb++) {
        pm_init(&paint, 0.3f);
        paint.d.textures[SU_PAINT_SLOT_SPECULAR] = mk_tex(srgb ? SU_TEX_RGBA8_SRGB : SU_TEX_RGBA8_UNORM, 1, 1, texel);
        for (channel = 0; channel < 3; channel++) paint.d.specular_factor[channel] = 0.5f;
        CHECK(shoot(&scene, &paint, g_ref));
        lin_px(g_ref, 0.5f, 0.5f, measured);
        for (channel = 0; channel < 3; channel++) {
            double f0 = (srgb ? lin(texel[channel]) : texel[channel] / 255.0) * 0.5;
            double expected = 3.0 * ((1.0 - f0) * 0.3 + f0 * 0.25) / 3.14159265;
            CHECK(fabs(measured[channel] - expected) < 0.009);
        }
        paint.d.factors.metallic = 1;
        CHECK(shoot(&scene, &paint, g_img));
        CHECK(memcmp(g_ref, g_img, sizeof g_img) == 0);
        scene.sun_intensity = 0;
        scene.ambient = 1;
        scene.sky[0] = scene.sky[1] = scene.sky[2] = 0.5f;
        CHECK(shoot(&scene, &paint, g_img));
        lin_px(g_img, 0.5f, 0.5f, measured);
        for (channel = 0; channel < 3; channel++) {
            double f0 = (srgb ? lin(texel[channel]) : texel[channel] / 255.0) * 0.5;
            double a004 = fmin(0.0, exp2(-9.28)) - 0.0275 + 0.0425;
            double brdf = f0 * (-1.04 * a004 - 0.572 + 1.04) + 1.04 * a004 + 0.022 - 0.04;
            double expected = 0.5 * (0.3 * (1.0 - brdf) + brdf);
            CHECK(fabs(measured[channel] - expected) < 0.009);
        }
        scene_init(&scene);
    }
    for (channel = 0; channel < 6 * 8 * 8 * 4; channel++) faces[channel] = 0.5f;
    scene.sun_intensity = 0;
    scene.ambient = 1;
    CHECK(su_render_env_create(g_r, 8, faces, &scene.env) == SU_OK);
    CHECK(shoot(&scene, &paint, g_img));
    lin_px(g_img, 0.5f, 0.5f, measured);
    for (channel = 0; channel < 3; channel++) {
        double f0 = lin(texel[channel]) * 0.5;
        double brdf = f0 * 0.4524 - 0.0024;
        CHECK(fabs(measured[channel] - 0.5 * (0.3 * (1.0 - brdf) + brdf)) < 0.009);
    }
    scene_init(&scene);
    paint.d.specular_channels = 3u | (2u << 8) | (1u << 16);
    CHECK(shoot(&scene, &paint, g_img));
    lin_px(g_img, 0.5f, 0.5f, measured);
    CHECK(measured[0] < measured[2]);
    paint.d.specular_channels = 0;
    paint.d.textures[SU_PAINT_SLOT_SPECULAR] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, texel);
    paint.d.specular_factor[0] = paint.d.specular_factor[1] = paint.d.specular_factor[2] = 1;
    paint.d.specular_channels = 4u | (5u << 8) | (4u << 16);
    CHECK(shoot(&scene, &paint, g_img));
    lin_px(g_img, 0.5f, 0.5f, measured);
    for (channel = 0; channel < 3; channel++) CHECK(fabs(measured[channel] - 0.75 / 3.14159265) < 0.009);
    paint.d.specular_channels = 0;
    paint.d.textures[SU_PAINT_SLOT_SPECULAR] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, black);
    paint.d.textures[SU_PAINT_SLOT_NORMAL] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, normal);
    CHECK(shoot(&scene, &paint, g_ref));
    paint.d.flags = SU_PAINT_NORMAL_Z_ONE;
    CHECK(shoot(&scene, &paint, g_img));
    {
        double nx = 204.0 / 255.0 * 2.0 - 1.0;
        double ny = 128.0 / 255.0 * 2.0 - 1.0;
        double reconstructed = sqrt(1.0 - nx * nx - ny * ny);
        double z_one = 1.0 / sqrt(1.0 + nx * nx + ny * ny);
        CHECK(fabs(lin(px(g_ref, 128, 128)[0]) - 0.9 * reconstructed / 3.14159265) < 0.009);
        CHECK(fabs(lin(px(g_img, 128, 128)[0]) - 0.9 * z_one / 3.14159265) < 0.009);
        CHECK(px(g_img, 128, 128)[0] > px(g_ref, 128, 128)[0]);
    }
    for (channel = 0; channel < 16 * 16; channel++) {
        memset(spatial + channel * 4, channel % 16 < 8 ? 64 : 192, 4);
    }
    pm_init(&paint, 0);
    paint.d.specular_factor[0] = paint.d.specular_factor[1] = paint.d.specular_factor[2] = 1;
    paint.d.textures[SU_PAINT_SLOT_SPECULAR] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, spatial);
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(fabs(lin(px(g_img, qx(0.25f), 128)[0]) - 0.75 * 64.0 / 255.0 / 3.14159265) < 0.009);
    CHECK(fabs(lin(px(g_img, qx(0.75f), 128)[0]) - 0.75 * 192.0 / 255.0 / 3.14159265) < 0.009);
    paint.d.extra_tex_info[0] = SU_ASSET_TEX_INFO(1, 0, 0);
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(abs((int)px(g_img, qx(0.25f), 128)[0] - (int)px(g_img, qx(0.75f), 128)[0]) < 3);
    printf("specular: linear/sRGB F0, factor, swizzle, independent diffuse, direct/ambient and normal conventions\n");
}

static void test_paint_detail(void) {
    Pm paint;
    Scene scene;
    uint8_t stripes[16 * 16 * 4];
    uint8_t mask[16 * 16 * 4];
    uint8_t white[4] = {255, 255, 255, 255};
    uint8_t black[4] = {0, 0, 0, 255};
    uint8_t tilted[4] = {204, 128, 255, 255};
    uint32_t row;
    uint32_t col;
    for (row = 0; row < 16; row++) {
        for (col = 0; col < 16; col++) {
            size_t offset = ((size_t)row * 16 + col) * 4;
            memset(stripes + offset, col < 8 ? 0 : 255, 4);
            memset(mask + offset, 0, 4);
            mask[offset + (col < 8 ? 0 : 1)] = 255;
        }
    }
    scene_init(&scene);
    pm_init(&paint, 0.4f);
    CHECK(shoot(&scene, &paint, g_ref));
    paint.d.flags = SU_PAINT_DETAIL_NORMAL_DARKEN;
    paint.d.textures[SU_PAINT_SLOT_DETAIL_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, stripes);
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(memcmp(g_ref, g_img, sizeof g_img) == 0);
    paint.d.detail_strength = 1;
    paint.d.detail_uv_scale[0] = paint.d.detail_uv_scale[1] = 2;
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(same_px(px(g_ref, qx(0.125f), 128), px(g_img, qx(0.125f), 128)));
    CHECK(px(g_img, qx(0.375f), 128)[0] < px(g_img, qx(0.125f), 128)[0] - 50);
    CHECK(same_px(px(g_ref, qx(0.625f), 128), px(g_img, qx(0.625f), 128)));
    paint.d.detail_rotation = 1.5707963268f;
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(px(g_img, 128, qx(0.125f))[0] < px(g_img, 128, qx(0.375f))[0] - 50);
    paint.d.detail_rotation = 0;
    paint.d.extra_tex_info[3] = SU_ASSET_TEX_INFO(1, SU_ASSET_WRAP_CLAMP, SU_ASSET_WRAP_CLAMP);
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(memcmp(g_ref, g_img, sizeof g_img) == 0);
    paint.d.extra_tex_info[3] = 0;
    paint.d.detail_strength = 0.5f;
    CHECK(shoot(&scene, &paint, g_img));
    {
        double f0 = 0.04;
        double expected = 3.0 * ((1 - f0) * 0.4 * 0.5 + f0 * 0.25) / 3.14159265;
        CHECK(fabs(lin(px(g_img, qx(0.375f), 128)[0]) - expected) < 0.012);
    }
    paint.d.detail_strength = 1;
    paint.d.textures[SU_PAINT_SLOT_DETAIL_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, white);
    paint.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, mask);
    paint.d.detail_layer_mask = 2;
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(same_px(px(g_ref, qx(0.25f), 128), px(g_img, qx(0.25f), 128)));
    CHECK(px(g_img, qx(0.75f), 128)[0] < px(g_img, qx(0.25f), 128)[0] - 50);
    paint.d.detail_layer_mask = 16;
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(memcmp(g_ref, g_img, sizeof g_img) == 0);
    pm_init(&paint, 0.3f);
    paint.d.textures[SU_PAINT_SLOT_SPECULAR] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, black);
    paint.d.specular_factor[0] = paint.d.specular_factor[1] = paint.d.specular_factor[2] = 1;
    paint.d.flags = SU_PAINT_DETAIL_NORMAL_DARKEN;
    paint.d.detail_strength = 1;
    paint.d.textures[SU_PAINT_SLOT_DETAIL_NORMAL] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, tilted);
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(fabs(lin(px(g_img, 128, 128)[0]) - 0.9 / sqrt(1.36) / 3.14159265) < 0.009);
    paint.d.detail_rotation = 1.5707963268f;
    scene.sun_dir[1] = 1;
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(fabs(lin(px(g_img, 128, 128)[0]) - 0.9 * 1.6 / sqrt(2.72) / 3.14159265) < 0.009);
    scene_init(&scene);
    pm_init(&paint, 0.3f);
    paint.d.flags = SU_PAINT_DETAIL_MATERIAL;
    paint.d.detail_strength = 1;
    paint.d.factors.metallic = 1;
    paint.d.textures[SU_PAINT_SLOT_MATERIAL] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, black);
    paint.d.textures[SU_PAINT_SLOT_DETAIL_MATERIAL] = mk_tex(SU_TEX_RGBA8_UNORM, 16, 16, stripes);
    CHECK(shoot(&scene, &paint, g_img));
    CHECK(px(g_img, qx(0.25f), 128)[0] > px(g_img, qx(0.75f), 128)[0] + 40);
    paint.d.textures[SU_PAINT_SLOT_MASK] = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, white);
    CHECK(shoot(&scene, &paint, g_ref));
    CHECK(abs((int)px(g_ref, qx(0.25f), 128)[0] - (int)px(g_ref, qx(0.75f), 128)[0]) < 3);
    printf("detail: zero strength, tiling, rotation, UV1, mask/strength, layer selectors, material and normal modes\n");
}

static void test_paint_extension_validation(void) {
    Pm paint;
    SuRenderHandle result = 0;
    uint32_t index;
    uint8_t texel[4] = {128, 128, 128, 255};
    for (index = 0; index < 12; index++) {
        pm_init(&paint, 0.3f);
        switch (index) {
        case 0: paint.d.flags = 8; break;
        case 1: paint.d.specular_factor[0] = NAN; break;
        case 2: paint.d.specular_factor[1] = -1; break;
        case 3: paint.d.specular_factor[2] = 2; break;
        case 4: paint.d.specular_channels = 6; break;
        case 5: paint.d.specular_channels = 1u << 24; break;
        case 6: paint.d.detail_strength = 2; break;
        case 7: paint.d.detail_uv_scale[0] = INFINITY; break;
        case 8: paint.d.detail_rotation = NAN; break;
        case 9: paint.d.detail_layer_mask = 32; break;
        case 10: paint.d.extra_tex_info[0] = 2; break;
        default: paint.d.extra_tex_info[1] = 3u << 8; break;
        }
        CHECK(su_render_paint_material_create(g_r, &paint.d, &result) == SU_E_INVALID_ARGUMENT);
    }
    for (index = SU_PAINT_SLOT_DETAIL_MATERIAL; index < SU_RENDER_PAINT_SLOT_COUNT; index++) {
        pm_init(&paint, 0.3f);
        paint.d.textures[index] = mk_tex(SU_TEX_RGBA8_SRGB, 1, 1, texel);
        CHECK(su_render_paint_material_create(g_r, &paint.d, &result) == SU_E_INVALID_ARGUMENT);
    }
}

static int timing_cmp(const void *left, const void *right) {
    double left_time = *(const double *)left;
    double right_time = *(const double *)right;
    return (left_time > right_time) - (left_time < right_time);
}

static void bench_paint(void) {
    SuRenderer *saved = g_r;
    SuRenderDesc desc;
    SuRenderFrameDesc frame;
    SuRenderObject object;
    SuRenderDeviceInfo device;
    char msg[256];
    uint32_t mode;
    uint32_t modes = 1;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.width = 1920;
    desc.height = 1080;
    desc.device_index = -1;
    CHECK(su_render_create(&desc, &g_r, msg) == SU_OK);
    CHECK(su_render_device(g_r, &device) == SU_OK);
    printf("paint benchmark device: %s; 1920x1080 fullscreen quad, validation off\n", device.name);
    memset(&frame, 0, sizeof frame);
    frame.struct_size = sizeof frame;
    frame.camera.rot.w = 1.0f;
    frame.camera.fov_y = FOV;
    frame.camera.near_z = 0.05f;
    frame.light.sun_dir = v3(0.3f, -0.4f, -1.0f);
    frame.light.sun_color = v3(1, 1, 1);
    frame.light.sun_intensity = 3.0f;
    frame.light.ambient = 0.2f;
    frame.light.sky_zenith = frame.light.sky_horizon = frame.light.sky_nadir = v3(0.3f, 0.4f, 0.5f);
    frame.light.no_sky = 1;
    frame.light.exposure = 1;
    memset(&object, 0, sizeof object);
    object.pos = su_wpos(0, 0, -3);
    object.rot.w = 1;
    object.scale = 4;
    object.tint[0] = object.tint[1] = object.tint[2] = object.tint[3] = 1;
    frame.objects = &object;
    frame.object_count = 1;
#ifdef SU_PAINT_NORMAL_Z_ONE
    modes = 5;
#endif
    for (mode = 0; mode < modes; mode++) {
        Pm paint;
        SuRenderHandle material = 0;
        SuRenderStats stats;
        double times[300];
        uint32_t count = 0;
        uint32_t sample;
        uint8_t texel[4] = {128, 160, 192, 255};
        SuRenderHandle texture = mk_tex(SU_TEX_RGBA8_UNORM, 1, 1, texel);
        pm_init(&paint, 0.4f);
        paint.d.textures[SU_PAINT_SLOT_NORMAL] = texture;
#ifdef SU_PAINT_NORMAL_Z_ONE
        if (mode == 1 || mode == 4) {
            paint.d.textures[SU_PAINT_SLOT_SPECULAR] = texture;
            paint.d.specular_factor[0] = paint.d.specular_factor[1] = paint.d.specular_factor[2] = 0.3f;
            paint.d.flags = SU_PAINT_NORMAL_Z_ONE;
        }
        if (mode >= 2) {
            paint.d.detail_strength = 0.5f;
            paint.d.detail_uv_scale[0] = paint.d.detail_uv_scale[1] = 26;
            paint.d.flags |= mode == 2 ? SU_PAINT_DETAIL_MATERIAL : SU_PAINT_DETAIL_NORMAL_DARKEN;
            paint.d.textures[SU_PAINT_SLOT_DETAIL_MATERIAL] = texture;
            paint.d.textures[SU_PAINT_SLOT_DETAIL_NORMAL] = texture;
            paint.d.textures[SU_PAINT_SLOT_DETAIL_MASK] = texture;
        }
#endif
        CHECK(su_render_paint_material_create(g_r, &paint.d, &material) == SU_OK);
        object.model = quad_model(material);
        for (sample = 0; sample < 340; sample++) {
            CHECK(su_render_frame_ex(g_r, &frame) == SU_OK);
            CHECK(su_render_stats(g_r, &stats) == SU_OK);
            if (sample >= 40 && stats.gpu_ms >= 0) {
                times[count++] = stats.gpu_ms;
            }
        }
        qsort(times, count, sizeof times[0], timing_cmp);
        CHECK(count == 300);
        if (count != 0) {
            printf("paint bench mode %u: n=%u median=%.6f p95=%.6f min=%.6f max=%.6f ms\n",
                   mode, count, times[count / 2], times[(count * 95u) / 100u], times[0], times[count - 1]);
        }
    }
    su_render_destroy(g_r);
    g_r = saved;
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
    if (getenv("SUTEKH_PAINT_BENCH") != NULL) {
        bench_paint();
        su_render_destroy(g_r);
        return g_fail ? 1 : 0;
    }
    test_mask_channels();
    test_blend_order();
    test_zero_layers();
    test_glow();
    test_glow_mask();
    test_scale3();
    test_shadows();
    test_shadow_cascades();
    bench_shadows();
    test_dirt_ao();
    test_environment();
    test_color_spaces();
    test_pack();
    test_specular_color();
    test_paint_detail();
    test_paint_extension_validation();
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    if (validation) {
        CHECK(getenv("SUTEKH_REQUIRE_VALIDATION") == NULL || st.validation_enabled != 0u); /* CI: the layer must be on */
        printf("validation %s: %u errors, %u warnings\n", st.validation_enabled ? "on" : "not available",
               st.validation_errors, st.validation_warnings);
        CHECK(st.validation_errors == 0u && st.validation_warnings == 0u);
    }
    su_render_destroy(g_r);
    printf("test_render_paint: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
