/* HDR bloom, exposure and auto exposure, offscreen, read back and checked pixel by pixel:
 *   - a bright additive billboard, a ribbon trail and an emissive mesh (per-material and per-instance emissive
 *     multipliers) all bleed light around themselves with bloom, and not at all without it;
 *   - the falloff is monotone with distance and left/right symmetric;
 *   - bloom intensity 0 (and a threshold above every pixel) gives byte-identical output to the no-bloom frame;
 *   - manual exposure scales the linear value as expected (checked against the sRGB encoding);
 *   - auto exposure brings dim and bright uniform scenes to the same output, is clamped, and is deterministic;
 *   - the glare helper adds streaks; odd and tiny sizes; the validation layer reports nothing.
 *
 * Usage: test_render_bloom [--no-validation]   (exit 77 without a usable Vulkan device) */
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
static uint32_t g_w = W;
static uint32_t g_h = H;
static uint8_t g_img[1024 * 1024 * 4];
static uint8_t g_ref[1024 * 1024 * 4];

static SuVec3f v3(float x, float y, float z) {
    SuVec3f v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static const uint8_t *px(const uint8_t *img, uint32_t x, uint32_t y) {
    return img + ((size_t)y * g_w + x) * 4u;
}

/* sRGB encoding of a linear value, as the attachment applies it */
static int srgb8(double l) {
    double s = l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    if (s < 0.0) {
        s = 0.0;
    }
    if (s > 1.0) {
        s = 1.0;
    }
    return (int)floor(s * 255.0 + 0.5);
}

static int near_i(int a, int b, int tol) {
    return abs(a - b) <= tol;
}

typedef struct Scene {
    float clear[4];
    float exposure;
    SuRenderPost post;
    const SuFxVertex *fx;
    uint32_t fx_count;
    const SuRenderObject *objs;
    uint32_t obj_count;
    const SuHudVertex *hud;
    uint32_t hud_count;
} Scene;

static void scene_init(Scene *s) {
    memset(s, 0, sizeof *s);
    s->clear[3] = 1.0f;
    s->exposure = 1.0f;
}

/* Draws a few frames (uploads need one) and reads the last one back into out. */
static int render(const Scene *s, uint8_t *out) {
    SuRenderFrameDesc f;
    int k;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.camera.rot.w = 1.0f;
    f.camera.fov_y = FOV;
    f.camera.near_z = 0.05f;
    f.light.sun_dir = v3(0, 0, -1);
    f.light.tonemap = SU_TONEMAP_CLAMP;
    f.light.no_sky = 1;
    f.light.exposure = s->exposure;
    memcpy(f.light.clear_color, s->clear, sizeof s->clear);
    f.fx = s->fx;
    f.fx_vertex_count = s->fx_count;
    f.objects = s->objs;
    f.object_count = s->obj_count;
    f.post = s->post;
    f.hud = s->hud;
    f.hud_vertex_count = s->hud_count;
    for (k = 0; k < 3; k++) {
        uint32_t st = su_render_frame_ex(g_r, &f);
        CHECK(st == SU_OK);
        if (st != SU_OK) {
            fprintf(stderr, "frame: %s\n", su_render_last_error(g_r));
            return 0;
        }
    }
    CHECK(su_render_readback(g_r, out, (size_t)g_w * g_h * 4u) == SU_OK);
    return 1;
}

static SuRenderPost bloom_post(float threshold, float knee, float intensity, float radius) {
    SuRenderPost p;
    memset(&p, 0, sizeof p);
    p.flags = SU_POST_BLOOM;
    p.bloom_threshold = threshold;
    p.bloom_knee = knee;
    p.bloom_intensity = intensity;
    p.bloom_radius = radius;
    return p;
}

static SuFxBuffer fxbuf(SuFxVertex *store, uint32_t cap) {
    SuFxBuffer b;
    b.v = store;
    b.cap = cap;
    b.count = 0;
    b.dropped = 0;
    return b;
}

/* ---------- emissive mesh ---------- */

static void box(SuAssetVertex *v, uint32_t *idx, float h) {
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int f;
    for (f = 0; f < 6; f++) {
        float u[3];
        float w[3];
        int c;
        u[0] = n[f][1] != 0.0f || n[f][2] != 0.0f ? 1.0f : 0.0f;
        u[1] = n[f][0] != 0.0f ? 1.0f : 0.0f;
        u[2] = 0.0f;
        w[0] = n[f][1] * u[2] - n[f][2] * u[1];
        w[1] = n[f][2] * u[0] - n[f][0] * u[2];
        w[2] = n[f][0] * u[1] - n[f][1] * u[0];
        for (c = 0; c < 4; c++) {
            float a = (c == 1 || c == 2) ? 1.0f : -1.0f;
            float b = c >= 2 ? 1.0f : -1.0f;
            SuAssetVertex *x = &v[f * 4 + c];
            int k;
            memset(x, 0, sizeof *x);
            for (k = 0; k < 3; k++) {
                x->pos[k] = h * (n[f][k] + a * u[k] + b * w[k]);
                x->normal[k] = n[f][k];
                x->tangent[k] = u[k];
            }
            x->tangent[3] = 1.0f;
            x->uv0[0] = 0.5f + 0.5f * a;
            x->uv0[1] = 0.5f - 0.5f * b;
        }
        idx[f * 6 + 0] = (uint32_t)f * 4u + 0u;
        idx[f * 6 + 1] = (uint32_t)f * 4u + 1u;
        idx[f * 6 + 2] = (uint32_t)f * 4u + 2u;
        idx[f * 6 + 3] = (uint32_t)f * 4u + 0u;
        idx[f * 6 + 4] = (uint32_t)f * 4u + 2u;
        idx[f * 6 + 5] = (uint32_t)f * 4u + 3u;
    }
}

/* A black cube of half-size 0.5 whose material emits a grey of emissive * strength. */
static SuRenderHandle make_cube(float emissive, float strength) {
    SuAssetVertex v[24];
    uint32_t idx[36];
    SuAssetSubmesh sm;
    SuRenderMaterialDesc md;
    SuRenderMeshDesc mesh;
    SuRenderModelNode mn;
    SuRenderHandle mat = 0;
    SuRenderHandle msh = 0;
    SuRenderHandle model = 0;
    box(v, idx, 0.5f);
    memset(&md, 0, sizeof md);
    su_asset_material_default(&md.factors);
    md.factors.base_color[0] = md.factors.base_color[1] = md.factors.base_color[2] = 0.0f;
    md.factors.metallic = 0.0f;
    md.factors.emissive[0] = md.factors.emissive[1] = md.factors.emissive[2] = emissive;
    md.factors.emissive_strength = strength;
    CHECK(su_render_material_create(g_r, &md, &mat) == SU_OK);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 36;
    sm.radius = 0.87f;
    memset(&mesh, 0, sizeof mesh);
    mesh.vertices = v;
    mesh.vertex_count = 24;
    mesh.indices = idx;
    mesh.index_count = 36;
    mesh.submeshes = &sm;
    mesh.submesh_count = 1;
    mesh.materials = &mat;
    mesh.radius = 0.87f;
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

static SuRenderObject cube_obj(SuRenderHandle model, float emissive_mul) {
    SuRenderObject o;
    memset(&o, 0, sizeof o);
    o.pos = su_wpos(0.0, 0.0, -5.0);
    o.rot.w = 1.0f;
    o.scale = 1.0f;
    o.tint[0] = o.tint[1] = o.tint[2] = o.tint[3] = 1.0f;
    o.emissive = emissive_mul;
    o.model = model;
    return o;
}

static const char *type_name(uint32_t t) {
    static const char *const names[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    return names[t <= SU_RENDER_DEVICE_CPU ? t : 0];
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
    Scene sc;
    SuFxVertex fxv[4096];
    SuFxBuffer fb;
    float hot[4] = {6.0f, 6.0f, 6.0f, 1.0f};
    uint32_t cx = W / 2u;
    uint32_t cy = H / 2u;
    uint32_t code;
    uint32_t verr = 0;
    uint32_t vwarn = 0;

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
    printf("rendering on: %s (%s), bloom supported: %u\n", used.name, type_name(used.type), (unsigned)used.bloom);
    if (!used.bloom) {
        /* the documented fallback: requests are ignored and the frame is tonemapped plain */
        scene_init(&sc);
        sc.post = bloom_post(1.0f, 0.5f, 1.0f, 1.0f);
        CHECK(render(&sc, g_img));
        CHECK(su_render_stats(g_r, &st) == SU_OK && st.bloom_levels_active == 0u);
        su_render_destroy(g_r);
        printf("test_render_bloom: %ld checks, %ld failures (device cannot filter the HDR format; fallback only)\n", g_checks,
               g_fail);
        return g_fail ? 1 : 0;
    }

    /* ---------- billboard: bloom falloff ---------- */
    {
        SuMat4f view = su_render_camera_view(&(SuRenderCamera){su_wpos(0, 0, 0), {0, 0, 0, 1}, FOV, 0.05f});
        int dx[6] = {20, 28, 40, 60, 90, 120};
        int right[6];
        int left[6];
        int off[6];
        int k;
        fb = fxbuf(fxv, 4096);
        su_fx_billboard(&fb, v3(0, 0, -5), 0.3f, 0.0f, hot, view);
        scene_init(&sc);
        sc.fx = fxv;
        sc.fx_count = fb.count;
        CHECK(fb.count == 6u && fb.dropped == 0u);
        CHECK(render(&sc, g_ref)); /* no post */
        for (k = 0; k < 6; k++) {
            off[k] = px(g_ref, cx + (uint32_t)dx[k], cy)[1];
        }
        CHECK(px(g_ref, cx, cy)[1] == 255);
        CHECK(off[0] == 0 && off[1] == 0 && off[2] == 0 && off[5] == 0); /* nothing leaks without bloom */
        sc.post = bloom_post(1.0f, 0.5f, 1.0f, 1.0f);
        CHECK(render(&sc, g_img));
        CHECK(su_render_stats(g_r, &st) == SU_OK);
        CHECK(st.bloom_levels_active == 6u);
        CHECK(st.post_passes > 6u);
        for (k = 0; k < 6; k++) {
            right[k] = px(g_img, cx + (uint32_t)dx[k], cy)[1];
            left[k] = px(g_img, cx - 1u - (uint32_t)dx[k], cy)[1];
        }
        printf("billboard bloom falloff (green, dx = 20 28 40 60 90 120): %d %d %d %d %d %d\n", right[0], right[1], right[2],
               right[3], right[4], right[5]);
        CHECK(right[0] > 0);
        CHECK(right[0] > right[1] && right[1] > right[2] && right[2] > right[3]);
        CHECK(right[3] >= right[4] && right[4] >= right[5]);
        CHECK(right[1] > 0 && right[2] > 0 && right[3] > 0);
        for (k = 0; k < 6; k++) {
            CHECK(near_i(left[k], right[k], 1)); /* symmetric */
        }
        CHECK(px(g_img, cx, cy)[1] == 255);
        /* vertical symmetry too */
        CHECK(near_i(px(g_img, cx, cy - 41u)[1], px(g_img, cx, cy + 40u)[1], 1));
        /* bigger radius spreads further */
        sc.post = bloom_post(1.0f, 0.5f, 1.0f, 2.5f);
        CHECK(render(&sc, g_ref));
        CHECK(px(g_ref, cx + 90u, cy)[1] >= px(g_img, cx + 90u, cy)[1]);
        /* more intensity, more glow */
        sc.post = bloom_post(1.0f, 0.5f, 3.0f, 1.0f);
        CHECK(render(&sc, g_ref));
        CHECK(px(g_ref, cx + 28u, cy)[1] > px(g_img, cx + 28u, cy)[1]);
        /* a threshold above the billboard's brightness: no bloom, output identical to no post */
        sc.post = bloom_post(100.0f, 0.5f, 1.0f, 1.0f);
        CHECK(render(&sc, g_ref));
        memset(&sc.post, 0, sizeof sc.post);
        CHECK(render(&sc, g_img));
        CHECK(memcmp(g_ref, g_img, (size_t)W * H * 4u) == 0);
        /* intensity 0 with the full chain running equals the no-bloom frame, byte for byte */
        sc.post = bloom_post(1.0f, 0.5f, 0.0f, 1.0f);
        CHECK(render(&sc, g_ref));
        CHECK(su_render_stats(g_r, &st) == SU_OK && st.bloom_levels_active == 6u);
        CHECK(memcmp(g_ref, g_img, (size_t)W * H * 4u) == 0);
        printf("intensity 0 vs no bloom: %s\n", memcmp(g_ref, g_img, (size_t)W * H * 4u) == 0 ? "identical" : "DIFFERENT");
    }

    /* ---------- exposure ---------- */
    {
        static const float ex[4] = {0.5f, 1.0f, 2.0f, 4.0f};
        int k;
        for (k = 0; k < 4; k++) {
            scene_init(&sc);
            sc.clear[0] = sc.clear[1] = sc.clear[2] = 0.1f;
            sc.exposure = ex[k];
            CHECK(render(&sc, g_img));
            CHECK(near_i(px(g_img, 10, 10)[0], srgb8(0.1 * (double)ex[k]), 1));
            /* the same through the bloom output pass (chain running, intensity 1, nothing above the threshold) */
            sc.post = bloom_post(1.0f, 0.5f, 1.0f, 1.0f);
            CHECK(render(&sc, g_ref));
            CHECK(memcmp(g_ref, g_img, (size_t)W * H * 4u) == 0);
            printf("exposure %.1f: 0.1 -> %d (expected %d)\n", (double)ex[k], px(g_img, 10, 10)[0], srgb8(0.1 * (double)ex[k]));
        }
    }

    /* ---------- auto exposure ---------- */
    {
        static const float clears[3] = {0.02f, 0.18f, 0.6f};
        int out[3];
        int k;
        for (k = 0; k < 3; k++) {
            scene_init(&sc);
            sc.clear[0] = sc.clear[1] = sc.clear[2] = clears[k];
            sc.post.flags = SU_POST_AUTO_EXPOSURE;
            CHECK(render(&sc, g_img));
            out[k] = px(g_img, 100, 100)[1];
            CHECK(render(&sc, g_ref)); /* deterministic: same bytes again */
            CHECK(memcmp(g_ref, g_img, (size_t)W * H * 4u) == 0);
        }
        printf("auto exposure (key 0.18): scenes 0.02 0.18 0.6 -> %d %d %d (expected %d)\n", out[0], out[1], out[2], srgb8(0.18));
        for (k = 0; k < 3; k++) {
            CHECK(near_i(out[k], srgb8(0.18), 2));
        }
        /* clamped: scene 0.02 needs 9x; allow at most 4x -> 0.08 */
        scene_init(&sc);
        sc.clear[0] = sc.clear[1] = sc.clear[2] = 0.02f;
        sc.post.flags = SU_POST_AUTO_EXPOSURE;
        sc.post.exposure_max = 4.0f;
        CHECK(render(&sc, g_img));
        CHECK(near_i(px(g_img, 100, 100)[1], srgb8(0.08), 2));
        /* manual exposure multiplies the automatic one */
        sc.post.exposure_max = 0.0f;
        sc.post.auto_key = 0.09f;
        CHECK(render(&sc, g_img));
        CHECK(near_i(px(g_img, 100, 100)[1], srgb8(0.09), 2));
        sc.exposure = 2.0f;
        CHECK(render(&sc, g_img));
        CHECK(near_i(px(g_img, 100, 100)[1], srgb8(0.18), 2));
    }

    /* ---------- the HUD is drawn after the bloom output pass, untouched by exposure ---------- */
    {
        SuHudVertex hv[64];
        SuHud hud;
        scene_init(&sc);
        sc.clear[0] = sc.clear[1] = sc.clear[2] = 0.02f;
        sc.post = bloom_post(1.0f, 0.5f, 1.0f, 1.0f);
        sc.post.flags |= SU_POST_AUTO_EXPOSURE;
        su_hud_begin(&hud, hv, 64, W, H);
        su_hud_rect(&hud, 100.0f, 100.0f, 20.0f, 20.0f, 0xFF0000FFu);
        sc.hud = hv;
        sc.hud_count = hud.count;
        CHECK(hud.count == 6u);
        CHECK(render(&sc, g_img));
        CHECK(px(g_img, 110, 110)[0] == 255 && px(g_img, 110, 110)[1] == 0 && px(g_img, 110, 110)[2] == 0);
        CHECK(px(g_img, 50, 50)[0] == px(g_img, 200, 200)[0] && px(g_img, 50, 50)[0] > 0);
    }

    /* ---------- ribbon trail ---------- */
    {
        SuTrailPoint pts[16];
        SuTrail tr;
        float head[4] = {5.0f, 3.0f, 1.0f, 1.0f};
        float tail[4] = {2.0f, 1.0f, 0.5f, 1.0f};
        int k;
        int with = 0;
        int without = 0;
        su_trail_init(&tr, pts, 16, 5.0f, 0.1f);
        for (k = 0; k <= 10; k++) {
            su_trail_push(&tr, su_wpos(-1.0 + 0.2 * k, 0.0, -5.0), 0.1 * k);
        }
        fb = fxbuf(fxv, 4096);
        su_trail_build(&tr, 1.0, su_wpos(0, 0, 0), 0.25f, 0.08f, head, tail, &fb);
        CHECK(fb.count > 0u);
        scene_init(&sc);
        sc.fx = fxv;
        sc.fx_count = fb.count;
        CHECK(render(&sc, g_ref));
        without = px(g_ref, cx, cy - 26u)[0];
        sc.post = bloom_post(1.0f, 0.5f, 1.0f, 1.0f);
        CHECK(render(&sc, g_img));
        with = px(g_img, cx, cy - 26u)[0];
        printf("trail: 26 px above the ribbon, red %d without bloom, %d with\n", without, with);
        CHECK(without == 0 && with > 0);
    }

    /* ---------- emissive mesh: per-material and per-instance multipliers ---------- */
    {
        SuRenderHandle m1 = make_cube(2.0f, 1.0f);
        SuRenderHandle m3 = make_cube(2.0f, 3.0f);
        SuRenderHandle m0 = make_cube(2.0f, 0.0f);
        SuRenderObject o[1];
        int halo_base;
        int halo_inst;
        int halo_mat;
        int halo_zero;
        int halo_mul0;
        o[0] = cube_obj(m1, 1.0f);
        scene_init(&sc);
        sc.objs = o;
        sc.obj_count = 1;
        CHECK(render(&sc, g_ref));
        CHECK(px(g_ref, cx, cy)[1] == 255); /* emissive 2 saturates */
        CHECK(px(g_ref, cx + 40u, cy)[1] == 0);
        sc.post = bloom_post(1.0f, 0.5f, 1.0f, 1.0f);
        CHECK(render(&sc, g_img));
        halo_base = px(g_img, cx + 40u, cy)[1];
        o[0] = cube_obj(m1, 4.0f);
        CHECK(render(&sc, g_img));
        halo_inst = px(g_img, cx + 40u, cy)[1];
        o[0] = cube_obj(m3, 1.0f);
        CHECK(render(&sc, g_img));
        halo_mat = px(g_img, cx + 40u, cy)[1];
        o[0] = cube_obj(m0, 1.0f);
        CHECK(render(&sc, g_img));
        halo_zero = px(g_img, cx + 40u, cy)[1];
        o[0] = cube_obj(m1, 0.0f);
        CHECK(render(&sc, g_img));
        halo_mul0 = px(g_img, cx + 40u, cy)[1];
        printf("emissive cube, halo 40 px right of the centre: base %d, instance x4 %d, material strength 3 %d, "
               "strength 0 %d, instance x0 %d\n",
               halo_base, halo_inst, halo_mat, halo_zero, halo_mul0);
        CHECK(halo_base > 0);
        CHECK(halo_inst > halo_base);
        CHECK(halo_mat > halo_base);
        CHECK(halo_zero == 0 && halo_mul0 == 0);
    }

    /* ---------- glare helper ---------- */
    {
        SuMat4f view = su_render_camera_view(&(SuRenderCamera){su_wpos(0, 0, 0), {0, 0, 0, 1}, FOV, 0.05f});
        float sun[4] = {20.0f, 20.0f, 20.0f, 1.0f};
        int streak_on;
        int streak_off;
        int diag_on;
        fb = fxbuf(fxv, 4096);
        su_fx_glare(&fb, v3(0, 0, -5), 0.12f, 1.0f, sun, view);
        CHECK(fb.count > 6u && fb.count <= 72u && fb.dropped == 0u);
        scene_init(&sc);
        sc.fx = fxv;
        sc.fx_count = fb.count;
        CHECK(render(&sc, g_img));
        streak_on = px(g_img, cx + 30u, cy)[1];
        diag_on = px(g_img, cx + 30u, cy + 30u)[1];
        fb = fxbuf(fxv, 4096);
        su_fx_billboard(&fb, v3(0, 0, -5), 0.12f, 0.0f, sun, view);
        sc.fx_count = fb.count;
        CHECK(render(&sc, g_img));
        streak_off = px(g_img, cx + 30u, cy)[1];
        printf("glare: 30 px right of a bright core %d with streaks, %d core only; diagonal %d\n", streak_on, streak_off, diag_on);
        CHECK(streak_on > streak_off);
        CHECK(streak_on > diag_on);
    }

    /* ---------- other sizes: fresh renderers (the offscreen target cannot be resized; the window path rebuilds the
     * same targets through the same function) ---------- */
    {
        SuMat4f view = su_render_camera_view(&(SuRenderCamera){su_wpos(0, 0, 0), {0, 0, 0, 1}, FOV, 0.05f});
        uint32_t k;
        static const uint32_t sizes[5][2] = {{321u, 177u}, {64u, 48u}, {16u, 8u}, {7u, 5u}, {1280u, 720u}};
        CHECK(su_render_stats(g_r, &st) == SU_OK);
        CHECK(getenv("SUTEKH_REQUIRE_VALIDATION") == NULL || st.validation_enabled != 0u); /* CI: the layer must be on */
        verr += st.validation_errors;
        vwarn += st.validation_warnings;
        su_render_destroy(g_r);
        g_r = NULL;
        fb = fxbuf(fxv, 4096);
        su_fx_billboard(&fb, v3(0, 0, -5), 1.0f, 0.0f, hot, view);
        for (k = 0; k < 5u; k++) {
            d.width = sizes[k][0];
            d.height = sizes[k][1];
            CHECK(su_render_create(&d, &g_r, msg) == SU_OK);
            if (g_r == NULL) {
                break;
            }
            scene_init(&sc);
            sc.fx = fxv;
            sc.fx_count = fb.count;
            sc.post = bloom_post(1.0f, 0.5f, 1.0f, 1.0f);
            sc.post.flags |= SU_POST_AUTO_EXPOSURE;
            g_w = sizes[k][0];
            g_h = sizes[k][1];
            CHECK(render(&sc, g_img));
            CHECK(su_render_stats(g_r, &st) == SU_OK);
            printf("%ux%u: bloom levels active %u, passes %u, centre %d\n", g_w, g_h, st.bloom_levels_active, st.post_passes,
                   px(g_img, g_w / 2u, g_h / 2u)[1]);
            if (g_w / 2u >= 4u && g_h / 2u >= 4u) {
                CHECK(st.bloom_levels_active >= 2u);
            } else {
                CHECK(st.bloom_levels_active == 0u); /* too small for a chain: plain tonemap, no crash */
            }
            CHECK(px(g_img, g_w / 2u, g_h / 2u)[1] > 100);
            verr += st.validation_errors;
            vwarn += st.validation_warnings;
            su_render_destroy(g_r);
            g_r = NULL;
        }
    }

    if (validation) {
        printf("validation on: %u errors, %u warnings\n", verr, vwarn);
        CHECK(verr == 0u && vwarn == 0u);
    }
    printf("test_render_bloom: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
