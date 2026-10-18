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
} Scene;

static void scene_init(Scene *s) {
    memset(s, 0, sizeof *s);
    s->sun_dir[2] = -1.0f;
    s->sun_intensity = 3.0f;
    s->exposure = 1.0f;
    s->emissive = 1.0f;
}

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
    f.objects = &o;
    f.object_count = 1;
    f.post = s->post;
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
        CHECK(su_render_paint_material_create(g_r, &p.d, &m) == SU_E_INVALID_ARGUMENT && m == 0u);
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
    test_mask_channels();
    test_blend_order();
    test_zero_layers();
    test_glow();
    test_dirt_ao();
    test_environment();
    test_color_spaces();
    test_pack();
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    if (validation) {
        printf("validation on: %u errors, %u warnings\n", st.validation_errors, st.validation_warnings);
        CHECK(st.validation_errors == 0u && st.validation_warnings == 0u);
    }
    su_render_destroy(g_r);
    printf("test_render_paint: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}