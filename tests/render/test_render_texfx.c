/* Textured effects, DDS textures and the cube-map sky, offscreen, read back and checked pixel by pixel:
 *   - a textured billboard shows its texture (quadrant colours at the right corners), per-sprite colour x intensity
 *     (HDR, clamped by the tonemap), atlas frames select the right region, sRGB and linear formats differ exactly as the
 *     format says, additive and premultiplied-alpha blending produce the documented values, an RGBA16F texture feeds
 *     bloom;
 *   - axis-aligned billboards: the lit area equals the projection of the quad whose long axis is the given world axis, at
 *     many camera positions around it (azimuth and elevation), not just straight on;
 *   - textured ribbons: U along the trail by age and by distance with a scrolling offset (wrapping), V across;
 *   - DDS files generated in memory (BC1, BC7 sRGB and linear, BC6H cube) parsed and drawn; the cube drawn as the sky
 *     background (each face along its axis, intensity, rotation, lod) both as a raw BC6H cube texture and as an environment
 *     built from the DDS; the CPU BC6H decoder equals the GPU's BC6H sampling on random blocks of every mode;
 *   - skipped batches are counted, frames without the new fields are unchanged, and the validation layer reports
 *     nothing.
 * Usage: test_render_texfx [--no-validation]   (exit 77 without a usable Vulkan device) */
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
#include "sutekh/dds.h"
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
#define NEAR_Z 0.05f

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

static const uint8_t *px(const uint8_t *img, uint32_t x, uint32_t y) {
    return img + ((size_t)y * W + x) * 4u;
}

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

static double srgb_to_lin(double s) {
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static int near_i(int a, int b, int tol) {
    return abs(a - b) <= tol;
}

static int rgb_near(const uint8_t *p, int r, int g, int b, int tol) {
    return near_i(p[0], r, tol) && near_i(p[1], g, tol) && near_i(p[2], b, tol);
}

/* ---------- scene and rendering ---------- */

typedef struct Scene {
    SuWorldPos cam_pos;
    SuQuatf cam_rot;
    float clear[4];
    float exposure;
    SuRenderPost post;
    const SuFxVertex *fx_tex;
    uint32_t fx_tex_count;
    const SuFxBatch *batches;
    uint32_t batch_count;
    SuRenderHandle sky_cube;
    SuRenderHandle env;
    float sky_intensity;
    SuQuatf sky_rot;
    float sky_lod;
    int sky;
    const SuRenderInstance *inst;      /* debug instances (opaque geometry for the depth buffer) */
    uint32_t inst_count;
} Scene;

static void scene_init(Scene *s) {
    memset(s, 0, sizeof *s);
    s->cam_rot.w = 1.0f;
    s->clear[3] = 1.0f;
    s->exposure = 1.0f;
}

static int render(const Scene *s, uint8_t *out) {
    SuRenderFrameDesc f;
    int k;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.camera.pos = s->cam_pos;
    f.camera.rot = s->cam_rot;
    f.camera.fov_y = FOV;
    f.camera.near_z = NEAR_Z;
    f.light.sun_dir = v3(0, 0, -1);
    f.light.tonemap = SU_TONEMAP_CLAMP;
    f.light.no_sky = s->sky ? 0u : 1u;
    f.light.up = v3(0, 1, 0);
    f.light.sky_zenith = v3(0.0f, 0.0f, 0.5f);
    f.light.sky_horizon = v3(0.5f, 0.0f, 0.0f);
    f.light.sky_nadir = v3(0.0f, 0.5f, 0.0f);
    f.light.exposure = s->exposure;
    memcpy(f.light.clear_color, s->clear, sizeof s->clear);
    f.fx_tex = s->fx_tex;
    f.fx_tex_vertex_count = s->fx_tex_count;
    f.fx_batches = s->batches;
    f.fx_batch_count = s->batch_count;
    f.sky_cube = s->sky_cube;
    f.env = s->env;
    f.sky_intensity = s->sky_intensity;
    f.sky_rot = s->sky_rot;
    f.sky_lod = s->sky_lod;
    f.post = s->post;
    f.debug_instances = s->inst;
    f.debug_instance_count = s->inst_count;
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

static SuFxBuffer fxbuf(SuFxVertex *store, uint32_t cap) {
    SuFxBuffer b;
    b.v = store;
    b.cap = cap;
    b.count = 0;
    b.dropped = 0;
    return b;
}

static SuRenderHandle make_tex_ex(uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, const uint8_t *const *data,
                                  uint32_t layers) {
    SuRenderTextureDesc d;
    SuRenderHandle hnd = 0;
    int k;
    memset(&d, 0, sizeof d);
    d.width = w;
    d.height = h;
    d.format = fmt;
    d.mip_count = mips;
    d.mip_data = data;
    d.flags = SU_RENDER_UPLOAD_COPY;
    d.layers = layers;
    CHECK(su_render_texture_create(g_r, &d, &hnd) == SU_OK);
    for (k = 0; k < 40 && su_render_resource_state(g_r, hnd) == SU_RENDER_STATE_PENDING; k++) {
        Scene s;
        scene_init(&s);
        (void)render(&s, g_ref);
    }
    return hnd;
}

static SuRenderHandle make_tex(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *data) {
    const uint8_t *m[1];
    m[0] = data;
    return make_tex_ex(fmt, w, h, 1, m, 1);
}

static void wait_ready(SuRenderHandle h) {
    int k;
    for (k = 0; k < 40 && su_render_resource_state(g_r, h) == SU_RENDER_STATE_PENDING; k++) {
        Scene s;
        scene_init(&s);
        (void)render(&s, g_ref);
    }
}

static SuMat4f camera_view(const Scene *s) {
    SuRenderCamera c;
    memset(&c, 0, sizeof c);
    c.rot = s->cam_rot;
    c.fov_y = FOV;
    c.near_z = NEAR_Z;
    return su_render_camera_view(&c);
}

/* Draws one batch of whatever the callback built into fb. */
static const uint8_t white4[4 * 4] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};

/* ---------- the tests ---------- */

static SuQuatf look_quat(SuVec3f dir) {
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    float yaw;
    float pitch;
    SuQuatf qy;
    SuQuatf qp;
    SuQuatf q;
    dir.x /= len;
    dir.y /= len;
    dir.z /= len;
    yaw = atan2f(-dir.x, -dir.z);
    pitch = asinf(dir.y);
    qy = su_quatf_axis_angle(v3(0, 1, 0), yaw);
    qp = su_quatf_axis_angle(v3(1, 0, 0), pitch);
    q = su_quatf_mul(qy, qp);
    {
        SuVec3f f = su_quatf_rotate(q, v3(0, 0, -1));
        CHECK(fabsf(f.x - dir.x) < 1e-4f && fabsf(f.y - dir.y) < 1e-4f && fabsf(f.z - dir.z) < 1e-4f);
    }
    return q;
}

static void test_quadrants_and_colour(void) {
    uint8_t tex[8 * 8 * 4];
    SuFxVertex vtx[256];
    SuFxBuffer fb = fxbuf(vtx, 256);
    SuFxBatch bt;
    SuRenderHandle h;
    Scene s;
    uint32_t x;
    uint32_t y;
    SuRenderStats st;
    float one[4] = {1, 1, 1, 1};
    SuMat4f view;
    for (y = 0; y < 8u; y++) {
        for (x = 0; x < 8u; x++) {
            uint8_t *p = tex + (y * 8u + x) * 4u;
            int right = x >= 4u;
            int bottom = y >= 4u;
            p[0] = (uint8_t)(!right && !bottom ? 255 : (right && bottom ? 255 : 0));
            p[1] = (uint8_t)(right && !bottom ? 255 : (right && bottom ? 255 : 0));
            p[2] = (uint8_t)(!right && bottom ? 255 : 0);
            p[3] = 255;
        }
    }
    h = make_tex(SU_TEX_RGBA8_UNORM, 8, 8, tex);
    CHECK(su_render_resource_state(g_r, h) == SU_RENDER_STATE_READY);
    scene_init(&s);
    view = camera_view(&s);
    su_fx_sprite(&fb, v3(0, 0, -4), 1.5f, 1.5f, 0.0f, one, NULL, view);
    bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
    CHECK(bt.vertex_count == 6u && bt.first_vertex == 0u);
    s.fx_tex = vtx;
    s.fx_tex_count = fb.count;
    s.batches = &bt;
    s.batch_count = 1;
    CHECK(render(&s, g_img));
    /* half-size 1.5 at distance 4 with a 60 degree field: 128 * 1.5 / (4 * tan 30) = 83 px; quadrant centres 41 px out */
    CHECK(rgb_near(px(g_img, 128 - 41, 128 - 41), 255, 0, 0, 1));    /* top left: red */
    CHECK(rgb_near(px(g_img, 128 + 41, 128 - 41), 0, 255, 0, 1));    /* top right: green */
    CHECK(rgb_near(px(g_img, 128 - 41, 128 + 41), 0, 0, 255, 1));    /* bottom left: blue */
    CHECK(rgb_near(px(g_img, 128 + 41, 128 + 41), 255, 255, 0, 1));  /* bottom right: yellow */
    CHECK(rgb_near(px(g_img, 10, 10), 0, 0, 0, 0) && rgb_near(px(g_img, 245, 245), 0, 0, 0, 0));
    CHECK(rgb_near(px(g_img, 128 - 90, 128), 0, 0, 0, 0)); /* just outside the quad */
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.fx_batches_drawn == 1u && st.fx_tex_vertices == 6u &&
          st.fx_batches_skipped == 0u);

    /* rotation by 90 degrees about the view axis turns the texture: top left red goes to the bottom left */
    fb.count = 0;
    su_fx_sprite(&fb, v3(0, 0, -4), 1.5f, 1.5f, 1.5707964f, one, NULL, view);
    s.fx_tex_count = fb.count;
    CHECK(render(&s, g_img));
    CHECK(rgb_near(px(g_img, 128 - 41, 128 + 41), 255, 0, 0, 1));

    /* per-sprite colour x intensity: two sprites in one batch with different tints */
    {
        float half[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        float hot[4] = {4.0f, 4.0f, 4.0f, 1.0f};
        float dim_alpha[4] = {1.0f, 1.0f, 1.0f, 0.5f};
        fb.count = 0;
        su_fx_sprite(&fb, v3(-1.2f, 0, -4), 0.8f, 0.8f, 0.0f, half, NULL, view);
        su_fx_sprite(&fb, v3(1.2f, 0, -4), 0.8f, 0.8f, 0.0f, hot, NULL, view);
        bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
        s.fx_tex_count = fb.count;
        CHECK(render(&s, g_img));
        /* sprite centres at +-1.2 / (4 * 0.57735) * 128 = +-66.5 px; its quadrants' centres 18 px further out */
        CHECK(rgb_near(px(g_img, 128 - 66 - 18, 128 - 18), srgb8(0.5), 0, 0, 1));   /* red x 0.5 */
        CHECK(rgb_near(px(g_img, 128 - 66 + 18, 128 - 18), 0, srgb8(0.5), 0, 1));   /* green x 0.5 */
        CHECK(rgb_near(px(g_img, 128 + 66 - 18, 128 - 18), 255, 0, 0, 0));          /* x 4 clamps */
        fb.count = 0;
        su_fx_sprite(&fb, v3(0, 0, -4), 1.5f, 1.5f, 0.0f, dim_alpha, NULL, view);
        s.fx_tex_count = fb.count;
        CHECK(render(&s, g_img));
        CHECK(rgb_near(px(g_img, 128 - 41, 128 - 41), srgb8(0.5), 0, 0, 1)); /* alpha scales the additive result */
    }
    CHECK(su_render_release(g_r, h) == SU_OK);
}

static void test_atlas(void) {
    uint8_t tex[16 * 16 * 4];
    SuFxVertex vtx[64];
    SuFxBuffer fb = fxbuf(vtx, 64);
    SuFxBatch bt;
    SuRenderHandle h;
    Scene s;
    uint32_t x;
    uint32_t y;
    uint32_t frame;
    float one[4] = {1, 1, 1, 1};
    float rect[4];
    SuMat4f view;
    for (y = 0; y < 16u; y++) {
        for (x = 0; x < 16u; x++) {
            uint8_t *p = tex + (y * 16u + x) * 4u;
            p[0] = (uint8_t)((x / 4u) * 60u + 30u);
            p[1] = (uint8_t)((y / 4u) * 60u + 30u);
            p[2] = 90;
            p[3] = 255;
        }
    }
    su_fx_atlas_rect(4, 4, 6, rect);
    CHECK(rect[0] == 0.5f && rect[1] == 0.25f && rect[2] == 0.75f && rect[3] == 0.5f);
    su_fx_atlas_rect(4, 4, 6 + 16, rect);
    CHECK(rect[0] == 0.5f && rect[1] == 0.25f);
    su_fx_atlas_rect(0, 0, 5, rect);
    CHECK(rect[0] == 0.0f && rect[3] == 1.0f);
    su_fx_atlas_rect(2, 1, 1, rect);
    CHECK(rect[0] == 0.5f && rect[1] == 0.0f && rect[2] == 1.0f && rect[3] == 1.0f);
    h = make_tex(SU_TEX_RGBA8_UNORM, 16, 16, tex);
    scene_init(&s);
    view = camera_view(&s);
    for (frame = 0; frame < 16u + 16u; frame += 5u) {
        uint32_t f = frame % 16u;
        fb.count = 0;
        su_fx_atlas_rect(4, 4, frame, rect);
        su_fx_sprite(&fb, v3(0, 0, -4), 1.0f, 1.0f, 0.0f, one, rect, view);
        bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
        s.fx_tex = vtx;
        s.fx_tex_count = fb.count;
        s.batches = &bt;
        s.batch_count = 1;
        CHECK(render(&s, g_img));
        /* the sprite is 55 px wide each way; a pixel near its centre (offset 8 px so every frame is in the interior) */
        /* UNORM texture: linear values, encoded to sRGB by the output */
        CHECK(rgb_near(px(g_img, 128 + 8, 128 - 8), srgb8((double)((f % 4u) * 60u + 30u) / 255.0),
                       srgb8((double)((f / 4u) * 60u + 30u) / 255.0), srgb8(90.0 / 255.0), 1));
        CHECK(rgb_near(px(g_img, 128 - 8, 128 + 8), srgb8((double)((f % 4u) * 60u + 30u) / 255.0),
                       srgb8((double)((f / 4u) * 60u + 30u) / 255.0), srgb8(90.0 / 255.0), 1));
    }
    CHECK(su_render_release(g_r, h) == SU_OK);
}

static void test_colour_space_and_blend(void) {
    uint8_t grey[4 * 4];
    uint8_t prem[4 * 4];
    SuFxVertex vtx[64];
    SuFxBuffer fb = fxbuf(vtx, 64);
    SuFxBatch bt[2];
    SuRenderHandle hl;
    SuRenderHandle hs;
    SuRenderHandle hp;
    Scene s;
    float one[4] = {1, 1, 1, 1};
    SuMat4f view;
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        grey[i * 4u] = grey[i * 4u + 1u] = grey[i * 4u + 2u] = 128;
        grey[i * 4u + 3u] = 255;
        prem[i * 4u] = 128; /* premultiplied: colour (1, 0, 0) at alpha 0.5 */
        prem[i * 4u + 1u] = 0;
        prem[i * 4u + 2u] = 0;
        prem[i * 4u + 3u] = 128;
    }
    hl = make_tex(SU_TEX_RGBA8_UNORM, 2, 2, grey);
    hs = make_tex(SU_TEX_RGBA8_SRGB, 2, 2, grey);
    scene_init(&s);
    view = camera_view(&s);
    /* the same bytes: read as linear 128/255 they come out as sRGB 188; read as sRGB they decode and re-encode to 128 */
    su_fx_sprite(&fb, v3(-1.2f, 0, -4), 0.8f, 0.8f, 0.0f, one, NULL, view);
    bt[0] = su_fx_batch(hl, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
    su_fx_sprite(&fb, v3(1.2f, 0, -4), 0.8f, 0.8f, 0.0f, one, NULL, view);
    bt[1] = su_fx_batch(hs, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 6, &fb);
    s.fx_tex = vtx;
    s.fx_tex_count = fb.count;
    s.batches = bt;
    s.batch_count = 2;
    CHECK(render(&s, g_img));
    CHECK(rgb_near(px(g_img, 128 - 66, 128), srgb8(128.0 / 255.0), srgb8(128.0 / 255.0), srgb8(128.0 / 255.0), 1));
    CHECK(rgb_near(px(g_img, 128 - 66, 128), 188, 188, 188, 1));
    CHECK(rgb_near(px(g_img, 128 + 66, 128), 128, 128, 128, 1));

    /* premultiplied alpha over a blue clear colour: (0.5, 0, 0) + blue * (1 - 0.5); additive adds 0.5*0.5 red... */
    hp = make_tex(SU_TEX_RGBA8_UNORM, 2, 2, prem);
    fb.count = 0;
    su_fx_sprite(&fb, v3(-1.2f, 0, -4), 0.8f, 0.8f, 0.0f, one, NULL, view);
    bt[0] = su_fx_batch(hp, SU_FX_BLEND_PREMUL, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
    su_fx_sprite(&fb, v3(1.2f, 0, -4), 0.8f, 0.8f, 0.0f, one, NULL, view);
    bt[1] = su_fx_batch(hp, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 6, &fb);
    s.fx_tex_count = fb.count;
    s.clear[2] = 1.0f;
    CHECK(render(&s, g_img));
    {
        double c = 128.0 / 255.0;
        /* premultiplied: red = c, blue = dst * (1 - c) */
        CHECK(rgb_near(px(g_img, 128 - 66, 128), srgb8(c), 0, srgb8(1.0 - c), 1));
        /* additive: red = c * a (alpha multiplies), blue unchanged at 1 */
        CHECK(rgb_near(px(g_img, 128 + 66, 128), srgb8(c * c), 0, 255, 1));
    }
    /* colour alpha scales a premultiplied sprite as a whole */
    {
        float half_a[4] = {1.0f, 1.0f, 1.0f, 0.5f};
        fb.count = 0;
        su_fx_sprite(&fb, v3(-1.2f, 0, -4), 0.8f, 0.8f, 0.0f, half_a, NULL, view);
        bt[0] = su_fx_batch(hp, SU_FX_BLEND_PREMUL, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
        s.fx_tex_count = fb.count;
        s.batch_count = 1;
        CHECK(render(&s, g_img));
        {
            double c = 128.0 / 255.0;
            double a = c * 0.5;
            CHECK(rgb_near(px(g_img, 128 - 66, 128), srgb8(c * 0.5), 0, srgb8(1.0 - a), 1));
        }
    }
    CHECK(su_render_release(g_r, hl) == SU_OK && su_render_release(g_r, hs) == SU_OK && su_render_release(g_r, hp) == SU_OK);
}

static void test_hdr_texture_bloom(void) {
    uint16_t texels[4 * 4];
    const uint8_t *m[1];
    SuRenderHandle h;
    SuFxVertex vtx[64];
    SuFxBuffer fb = fxbuf(vtx, 64);
    SuFxBatch bt;
    Scene s;
    float one[4] = {1, 1, 1, 1};
    uint8_t off[W * H * 4];
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        texels[i * 4u] = 0x4400;      /* 4.0 */
        texels[i * 4u + 1u] = 0x4400;
        texels[i * 4u + 2u] = 0x4400;
        texels[i * 4u + 3u] = 0x3C00; /* 1.0 */
    }
    m[0] = (const uint8_t *)texels;
    h = make_tex_ex(SU_TEX_RGBA16F, 2, 2, 1, m, 1);
    CHECK(su_render_resource_state(g_r, h) == SU_RENDER_STATE_READY);
    scene_init(&s);
    su_fx_sprite(&fb, v3(0, 0, -4), 0.5f, 0.5f, 0.0f, one, NULL, camera_view(&s));
    bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
    s.fx_tex = vtx;
    s.fx_tex_count = fb.count;
    s.batches = &bt;
    s.batch_count = 1;
    s.exposure = 0.125f; /* 4 x 0.125 = 0.5 */
    CHECK(render(&s, off));
    CHECK(rgb_near(px(off, 128, 128), srgb8(0.5), srgb8(0.5), srgb8(0.5), 1));
    CHECK(px(off, 128 + 60, 128)[0] == 0); /* sprite half width is 16 px: no light far to the side without bloom */
    s.exposure = 1.0f;
    CHECK(render(&s, off));
    CHECK(rgb_near(px(off, 128, 128), 255, 255, 255, 0)); /* HDR 4.0 clamps */
    s.post.flags = SU_POST_BLOOM;
    s.post.bloom_intensity = 1.0f;
    s.post.bloom_threshold = 1.0f;
    CHECK(render(&s, g_img));
    CHECK(px(g_img, 128 + 40, 128)[0] > 0 && px(off, 128 + 40, 128)[0] == 0); /* the HDR sprite feeds bloom */
    CHECK(su_render_release(g_r, h) == SU_OK);
}

typedef struct Box {
    int x0, y0, x1, y1;
    long lit;
} Box;

static Box lit_box(const uint8_t *img, int thr) {
    Box b;
    uint32_t x;
    uint32_t y;
    b.x0 = b.y0 = 100000;
    b.x1 = b.y1 = -1;
    b.lit = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            const uint8_t *p = px(img, x, y);
            if (p[0] > thr || p[1] > thr || p[2] > thr) {
                b.lit++;
                b.x0 = (int)x < b.x0 ? (int)x : b.x0;
                b.y0 = (int)y < b.y0 ? (int)y : b.y0;
                b.x1 = (int)x > b.x1 ? (int)x : b.x1;
                b.y1 = (int)y > b.y1 ? (int)y : b.y1;
            }
        }
    }
    return b;
}

static void project_box(const Scene *s, const SuVec3f *pts, uint32_t n, double *x0, double *y0, double *x1, double *y1,
                        int *behind, double *area) {
    double sxs[8];
    double sys[8];
    SuMat4f view = camera_view(s);
    SuMat4f proj = su_mat4f_perspective_rz(FOV, (float)W / (float)H, NEAR_Z);
    SuMat4f vp = su_mat4f_mul(proj, view);
    uint32_t i;
    *x0 = *y0 = 1e30;
    *x1 = *y1 = -1e30;
    *behind = 0;
    for (i = 0; i < n; i++) {
        float w = 0.0f;
        SuVec3f c = su_mat4f_transform_point(vp, pts[i], &w);
        double sx = ((double)c.x * 0.5 + 0.5) * W;
        double sy = ((double)c.y * 0.5 + 0.5) * H;
        if (w <= 0.0f) {
            *behind = 1;
        }
        *x0 = sx < *x0 ? sx : *x0;
        *x1 = sx > *x1 ? sx : *x1;
        *y0 = sy < *y0 ? sy : *y0;
        *y1 = sy > *y1 ? sy : *y1;
        sxs[i] = sx;
        sys[i] = sy;
    }
    *area = 0.0;
    for (i = 0; i < n; i++) {
        uint32_t j = (i + 1u) % n;
        *area += sxs[i] * sys[j] - sxs[j] * sys[i];
    }
    *area = fabs(*area) * 0.5;
}

static void test_axis_billboards(void) {
    SuRenderHandle h;
    SuFxVertex vtx[64];
    SuFxBuffer fb;
    SuFxBatch bt;
    Scene s;
    float one[4] = {1, 1, 1, 1};
    const SuVec3f axes[3] = {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.3f, 1.0f, -0.4f}};
    uint32_t ai;
    int worst = 0;
    int compared = 0;
    int az;
    int el;
    h = make_tex(SU_TEX_RGBA8_UNORM, 2, 2, white4);
    for (ai = 0; ai < 3u; ai++) {
        SuVec3f a = axes[ai];
        float al = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
        a.x /= al;
        a.y /= al;
        a.z /= al;
        for (el = 0; el < 3; el++) {
            for (az = 0; az < 360; az += 30) {
                float phi = (float)az * 0.0174532925f;
                float theta = (float)(el - 1) * 0.6f; /* -34, 0, +34 degrees */
                SuVec3f cam = v3(7.0f * sinf(phi) * cosf(theta), 7.0f * sinf(theta), 7.0f * cosf(phi) * cosf(theta));
                SuVec3f rel = v3(-cam.x, -cam.y, -cam.z);
                SuVec3f view_dir = v3(-cam.x / 7.0f, -cam.y / 7.0f, -cam.z / 7.0f);
                float ax_dot = a.x * view_dir.x + a.y * view_dir.y + a.z * view_dir.z;
                SuVec3f side;
                SuVec3f corners[4];
                double x0;
                double y0;
                double x1;
                double y1;
                int behind;
                double area;
                Box b;
                float hl = 1.2f;
                float hw = 0.5f;
                scene_init(&s);
                s.cam_pos = su_wpos(cam.x, cam.y, cam.z);
                s.cam_rot = look_quat(view_dir);
                fb = fxbuf(vtx, 64);
                su_fx_sprite_axis(&fb, rel, a, hl, hw, one, NULL);
                CHECK(fb.count == 6u);
                /* the quad as the documentation defines it: centre +- along * axis, +- across * (axis x view) */
                side = v3(a.y * rel.z - a.z * rel.y, a.z * rel.x - a.x * rel.z, a.x * rel.y - a.y * rel.x);
                {
                    float sl = sqrtf(side.x * side.x + side.y * side.y + side.z * side.z);
                    if (sl < 1e-4f) {
                        continue; /* looking along the axis: degenerate by definition */
                    }
                    side = v3(side.x / sl * hw, side.y / sl * hw, side.z / sl * hw);
                }
                corners[0] = v3(rel.x - a.x * hl - side.x, rel.y - a.y * hl - side.y, rel.z - a.z * hl - side.z);
                corners[1] = v3(rel.x - a.x * hl + side.x, rel.y - a.y * hl + side.y, rel.z - a.z * hl + side.z);
                corners[2] = v3(rel.x + a.x * hl + side.x, rel.y + a.y * hl + side.y, rel.z + a.z * hl + side.z);
                corners[3] = v3(rel.x + a.x * hl - side.x, rel.y + a.y * hl - side.y, rel.z + a.z * hl - side.z);
                bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
                s.fx_tex = vtx;
                s.fx_tex_count = fb.count;
                s.batches = &bt;
                s.batch_count = 1;
                CHECK(render(&s, g_img));
                project_box(&s, corners, 4, &x0, &y0, &x1, &y1, &behind, &area);
                b = lit_box(g_img, 8);
                if (fabsf(ax_dot) > 0.97f) {
                    continue; /* nearly end-on: a sliver a pixel or two wide, rasterisation dominates */
                }
                compared++;
                CHECK(!behind && b.lit > 0);
                {
                    int dx0 = abs(b.x0 - (int)ceil(x0 - 0.5));
                    int dx1 = abs(b.x1 - (int)floor(x1 - 0.5));
                    int dy0 = abs(b.y0 - (int)ceil(y0 - 0.5));
                    int dy1 = abs(b.y1 - (int)floor(y1 - 0.5));
                    int d = dx0 > dx1 ? dx0 : dx1;
                    d = dy0 > d ? dy0 : d;
                    d = dy1 > d ? dy1 : d;
                    worst = d > worst ? d : worst;
                    if (d > 3 || fabs((double)b.lit - area) > 0.06 * area + 30.0) {
                        fprintf(stderr, "axis %u el %d az %d: lit box %d,%d..%d,%d expected %.1f,%.1f..%.1f,%.1f\n", (unsigned)ai,
                                el, az, b.x0, b.y0, b.x1, b.y1, x0, y0, x1, y1);
                    }
                    /* the extreme pixel of a rotated rectangle's corner may be uncovered: allow a few pixels; a quad that
                     * ignored the axis would be off by tens of pixels, and the area pins the shape down */
                    CHECK(d <= 3);
                    CHECK(fabs((double)b.lit - area) <= 0.06 * area + 30.0);
                }
            }
        }
    }
    printf("axis billboards: %d camera positions compared to the projected quad, worst edge error %d px\n", compared, worst);
    CHECK(compared > 40);
    /* the quad stays aligned with the axis: for the vertical axis the lit area is symmetric left/right of the centre
     * column and the same height at every azimuth */
    {
        int h0 = -1;
        int same = 1;
        for (az = 0; az < 360; az += 45) {
            float phi = (float)az * 0.0174532925f;
            SuVec3f cam = v3(7.0f * sinf(phi), 0.0f, 7.0f * cosf(phi));
            Box b;
            scene_init(&s);
            s.cam_pos = su_wpos(cam.x, cam.y, cam.z);
            s.cam_rot = look_quat(v3(-cam.x, 0.0f, -cam.z));
            fb = fxbuf(vtx, 64);
            su_fx_sprite_axis(&fb, v3(-cam.x, -cam.y, -cam.z), v3(0, 1, 0), 1.2f, 0.5f, one, NULL);
            bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
            s.fx_tex = vtx;
            s.fx_tex_count = fb.count;
            s.batches = &bt;
            s.batch_count = 1;
            CHECK(render(&s, g_img));
            b = lit_box(g_img, 8);
            if (h0 < 0) {
                h0 = b.y1 - b.y0;
            }
            same &= b.y1 - b.y0 == h0 && abs((b.x0 + b.x1) - 255) <= 1;
        }
        CHECK(same);
    }
    CHECK(su_render_release(g_r, h) == SU_OK);
}

static void test_ribbon(void) {
    uint8_t tex[64 * 16 * 4];
    SuRenderHandle h;
    SuFxVertex vtx[256];
    SuFxBuffer fb;
    SuFxBatch bt;
    SuTrail tr;
    SuTrailPoint pts[8];
    Scene s;
    float head[4] = {1, 1, 1, 1};
    float tail[4] = {1, 1, 1, 1};
    SuTrailTex tt;
    uint32_t x;
    uint32_t y;
    double xh;
    double xt;
    int i;
    for (y = 0; y < 16u; y++) {
        for (x = 0; x < 64u; x++) {
            uint8_t *p = tex + (y * 64u + x) * 4u;
            p[0] = (uint8_t)(x * 4u);          /* u ramp */
            p[1] = (uint8_t)(y * 16u + 8u);    /* v ramp */
            p[2] = 0;
            p[3] = 255;
        }
    }
    h = make_tex(SU_TEX_RGBA8_UNORM, 64, 16, tex);
    su_trail_init(&tr, pts, 8, 2.0f, 0.1f);
    su_trail_push(&tr, su_wpos(2, 0, -5), 0.0);   /* oldest, on the right? the head is the newest */
    su_trail_push(&tr, su_wpos(-2, 0, -5), 1.0);  /* newest: the head, on the left */
    scene_init(&s);
    s.cam_rot.w = 1.0f;
    /* by age: now = 1, ages 1 s (right end, k = 0.5) and 0 (left end, k = 0) with lifetime 2; u_scale 2 so u runs 0..1 */
    memset(&tt, 0, sizeof tt);
    tt.mode = SU_TRAIL_U_AGE;
    tt.u_scale = 2.0f;
    tt.v0 = 0.0f;
    tt.v1 = 1.0f;
    for (i = 0; i < 3; i++) {
        float offset = i == 0 ? 0.0f : (i == 1 ? 0.25f : 0.75f);
        int xs;
        tt.u_offset = offset;
        fb = fxbuf(vtx, 256);
        su_trail_build_tex(&tr, 1.0, su_wpos(0, 0, 0), 0.8f, 0.8f, head, tail, &tt, &fb);
        CHECK(fb.count == 6u);
        bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_REPEAT, SU_FX_WRAP_CLAMP, 0, &fb);
        s.fx_tex = vtx;
        s.fx_tex_count = fb.count;
        s.batches = &bt;
        s.batch_count = 1;
        CHECK(render(&s, g_img));
        /* the ends at x = -2 (u = offset) and x = +2 (u = offset + 1) at depth 5: 128 * 2 / (5 * tan 30) = 88.6 px */
        xh = 128.0 - 88.6;
        xt = 128.0 + 88.6;
        for (xs = 40; xs <= 215; xs += 25) {
            double u = offset + ((double)xs + 0.5 - xh) / (xt - xh);
            double fu = u - floor(u);
            double want = fu * 64.0 - 0.5; /* texel coordinate of the sample, wrapping */
            int expect;
            const uint8_t *p = px(g_img, (uint32_t)xs, 128);
            /* texels hold 4 * i; the filtered value at texel coordinate want (wrap) */
            double t0 = floor(want);
            double fr = want - t0;
            int i0 = ((int)t0 + 64) % 64;
            int i1 = (i0 + 1) % 64;
            double lin = (double)(i0 * 4) * (1.0 - fr) + (double)(i1 * 4) * fr;
            expect = (int)floor(srgb8(lin / 255.0) + 0.0);
            /* the ramp is linear in the texture and sRGB-encoded on output (alpha-free additive path) */
            CHECK(near_i(p[0], expect, 6) || (i0 >= 62 || i0 <= 1)); /* the wrap seam: texels differ by a full ramp */
        }
    }
    /* scrolling: the same screen column gives a different, offset ramp value and the shift equals the offset */
    {
        const uint8_t *p0;
        const uint8_t *p1;
        int r0;
        int r1;
        tt.u_offset = 0.0f;
        fb = fxbuf(vtx, 256);
        su_trail_build_tex(&tr, 1.0, su_wpos(0, 0, 0), 0.8f, 0.8f, head, tail, &tt, &fb);
        bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_REPEAT, SU_FX_WRAP_CLAMP, 0, &fb);
        s.fx_tex = vtx;
        s.fx_tex_count = fb.count;
        s.batches = &bt;
        CHECK(render(&s, g_ref));
        tt.u_offset = 0.125f;
        fb = fxbuf(vtx, 256);
        su_trail_build_tex(&tr, 1.0, su_wpos(0, 0, 0), 0.8f, 0.8f, head, tail, &tt, &fb);
        CHECK(render(&s, g_img));
        p0 = px(g_ref, 100, 128);
        p1 = px(g_img, 100, 128);
        r0 = (int)lround(srgb_to_lin(p0[0] / 255.0) * 255.0);
        r1 = (int)lround(srgb_to_lin(p1[0] / 255.0) * 255.0);
        CHECK(near_i(r1 - r0, (int)(0.125 * 252.0), 5)); /* 0.125 of a ramp of 252 levels */
    }
    /* V across: the green ramp runs from one edge of the ribbon to the other and flips with v0, v1 */
    {
        int g_top;
        int g_bot;
        tt.u_offset = 0.0f;
        tt.v0 = 0.0f;
        tt.v1 = 1.0f;
        fb = fxbuf(vtx, 256);
        su_trail_build_tex(&tr, 1.0, su_wpos(0, 0, 0), 0.8f, 0.8f, head, tail, &tt, &fb);
        bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_REPEAT, SU_FX_WRAP_CLAMP, 0, &fb);
        s.fx_tex_count = fb.count;
        CHECK(render(&s, g_img));
        /* ribbon width 0.8 at depth 5: 128 * 0.4 / (5 * tan 30) = 17.7 px each way from the middle row */
        g_top = px(g_img, 128, 128 - 15)[1];
        g_bot = px(g_img, 128, 128 + 14)[1];
        CHECK(g_top != g_bot && abs(g_top - g_bot) > 100);
        tt.v0 = 1.0f;
        tt.v1 = 0.0f;
        fb = fxbuf(vtx, 256);
        su_trail_build_tex(&tr, 1.0, su_wpos(0, 0, 0), 0.8f, 0.8f, head, tail, &tt, &fb);
        s.fx_tex_count = fb.count;
        CHECK(render(&s, g_img));
        CHECK(near_i(px(g_img, 128, 128 - 15)[1], g_bot, 3));
        CHECK(near_i(px(g_img, 128, 128 + 14)[1], g_top, 3));
    }
    /* by distance: u advances by distance * u_scale; the ribbon is 4 units long, u_scale 0.25 gives u = 0..1 */
    {
        int xs;
        tt.mode = SU_TRAIL_U_DISTANCE;
        tt.u_scale = 0.25f;
        tt.u_offset = 0.0f;
        tt.v0 = 0.0f;
        tt.v1 = 1.0f;
        fb = fxbuf(vtx, 256);
        su_trail_build_tex(&tr, 1.0, su_wpos(0, 0, 0), 0.8f, 0.8f, head, tail, &tt, &fb);
        bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_REPEAT, SU_FX_WRAP_CLAMP, 0, &fb);
        s.fx_tex_count = fb.count;
        CHECK(render(&s, g_img));
        /* the head (left, newest) has distance 0: ramp rises toward the right; the middle is half way */
        xs = 128;
        CHECK(near_i((int)lround(srgb_to_lin(px(g_img, (uint32_t)xs, 128)[0] / 255.0) * 255.0), 126, 8));
        CHECK(near_i((int)lround(srgb_to_lin(px(g_img, 60, 128)[0] / 255.0) * 255.0), (int)(252.0 * (60.5 - 39.4) / 177.2), 8));
    }
    CHECK(su_render_release(g_r, h) == SU_OK);
    /* the plain trail builder and the textured one produce the same geometry and colours (only uv differ) */
    {
        SuFxVertex a[64];
        SuFxVertex b[64];
        SuFxBuffer fa = fxbuf(a, 64);
        SuFxBuffer fbb = fxbuf(b, 64);
        uint32_t k;
        int same = 1;
        float ch[4] = {2, 1, 0.5f, 1};
        float ct[4] = {0, 0, 0, 0};
        su_trail_build(&tr, 1.0, su_wpos(0, 0, 0), 0.8f, 0.2f, ch, ct, &fa);
        memset(&tt, 0, sizeof tt);
        tt.mode = SU_TRAIL_U_AGE;
        su_trail_build_tex(&tr, 1.0, su_wpos(0, 0, 0), 0.8f, 0.2f, ch, ct, &tt, &fbb);
        CHECK(fa.count == fbb.count && fa.count == 6u);
        for (k = 0; k < fa.count; k++) {
            same &= memcmp(a[k].pos, b[k].pos, sizeof a[k].pos) == 0;
            same &= memcmp(a[k].color, b[k].color, sizeof a[k].color) == 0;
        }
        CHECK(same);
    }
}

/* ---------- DDS ---------- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* DX10-header DDS writer: dxgi format, w x h, mips, cube; pixels copied after the headers. */
static size_t write_dds(uint8_t *out, uint32_t dxgi, uint32_t w, uint32_t h, uint32_t mips, int cube, const uint8_t *pixels,
                        size_t pixel_bytes) {
    memset(out, 0, 148);
    put32(out, 0x20534444u);
    put32(out + 4, 124);
    put32(out + 8, 0x1007u | (mips > 1u ? 0x20000u : 0u));
    put32(out + 12, h);
    put32(out + 16, w);
    put32(out + 28, mips);
    put32(out + 76, 32);
    put32(out + 80, 4);
    put32(out + 84, 0x30315844u); /* DX10 */
    put32(out + 108, 0x1000u | (mips > 1u ? 0x400008u : 0u));
    put32(out + 112, cube ? (0x200u | 0xFC00u) : 0u);
    put32(out + 128, dxgi);
    put32(out + 132, 3);
    put32(out + 136, cube ? 4u : 0u);
    put32(out + 140, 1);
    memcpy(out + 148, pixels, pixel_bytes);
    return 148u + pixel_bytes;
}

static void put_bits(uint8_t b[16], uint32_t *pos, uint32_t v, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if ((v >> i) & 1u) {
            b[*pos >> 3] = (uint8_t)(b[*pos >> 3] | (1u << (*pos & 7u)));
        }
        (*pos)++;
    }
}

static void bc6h_solid(uint8_t b[16], uint32_t r, uint32_t g, uint32_t bl) {
    uint32_t pos = 0;
    memset(b, 0, 16);
    put_bits(b, &pos, 0x03u, 5);
    put_bits(b, &pos, r, 10);
    put_bits(b, &pos, g, 10);
    put_bits(b, &pos, bl, 10);
    put_bits(b, &pos, r, 10);
    put_bits(b, &pos, g, 10);
    put_bits(b, &pos, bl, 10);
}

/* BC7 mode 6 block with one colour: 7 bit endpoints with p-bit 1 (so the 8 bit value is (x << 1) | 1), indices 0. */
static void bc7_solid(uint8_t b[16], uint32_t r, uint32_t g, uint32_t bl) {
    uint32_t pos = 0;
    memset(b, 0, 16);
    put_bits(b, &pos, 0x40u, 7); /* mode 6: six zero bits then a one */
    put_bits(b, &pos, r, 7);
    put_bits(b, &pos, r, 7);
    put_bits(b, &pos, g, 7);
    put_bits(b, &pos, g, 7);
    put_bits(b, &pos, bl, 7);
    put_bits(b, &pos, bl, 7);
    put_bits(b, &pos, 127, 7);
    put_bits(b, &pos, 127, 7);
    put_bits(b, &pos, 1, 1);
    put_bits(b, &pos, 1, 1);
}

static void bc1_solid(uint8_t b[8], uint32_t r5, uint32_t g6, uint32_t b5) {
    uint16_t c = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    memset(b, 0, 8);
    b[0] = b[2] = (uint8_t)c;
    b[1] = b[3] = (uint8_t)(c >> 8);
}

static void test_dds_2d(void) {
    static uint8_t file[2048];
    uint8_t bc7[16];
    SuDdsImage img;
    char msg[160];
    SuRenderHandle h;
    SuFxVertex vtx[64];
    SuFxBuffer fb;
    SuFxBatch bt;
    Scene s;
    float one[4] = {1, 1, 1, 1};
    SuMat4f view;
    uint8_t pixels[8 * 4 * 2 + 8 + 40];
    size_t n;
    uint32_t i;
    scene_init(&s);
    view = camera_view(&s);
    /* BC1 8x8 with a second mip, all colour (r5 20, g6 40, b5 10) -> 8 bit (165, 162, 82), plus 40 bytes of metadata */
    for (i = 0; i < 4u; i++) {
        bc1_solid(pixels + i * 8u, 20, 40, 10);
    }
    bc1_solid(pixels + 32, 20, 40, 10);
    memset(pixels + 40, 0xCC, 40);
    n = write_dds(file, 71, 8, 8, 2, 0, pixels, 40u + 40u);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.format == SU_TEX_BC1_UNORM && img.trailing_bytes == 40u);
    {
        int k;
        for (k = 0; k < 2; k++) {
            uint32_t cs = k == 0 ? SU_DDS_CS_LINEAR : SU_DDS_CS_SRGB;
            CHECK(su_dds_parse(file, n, cs, &img, msg) == SU_OK);
            h = 0;
            CHECK(su_render_texture_create_dds(g_r, &img, SU_RENDER_UPLOAD_COPY, &h) == SU_OK);
            wait_ready(h);
            if (su_render_resource_state(g_r, h) != SU_RENDER_STATE_READY) {
                printf("BC1 not sampleable here; skipped\n");
                (void)su_render_release(g_r, h);
                continue;
            }
            fb = fxbuf(vtx, 64);
            su_fx_sprite(&fb, v3(0, 0, -4), 1.0f, 1.0f, 0.0f, one, NULL, view);
            bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
            s.fx_tex = vtx;
            s.fx_tex_count = fb.count;
            s.batches = &bt;
            s.batch_count = 1;
            CHECK(render(&s, g_img));
            if (k == 0) { /* linear: the bytes are linear values, the output encodes them */
                CHECK(rgb_near(px(g_img, 128, 128), srgb8(165.0 / 255.0), srgb8(162.0 / 255.0), srgb8(82.0 / 255.0), 1));
            } else {      /* sRGB: decoded then re-encoded: the bytes come back */
                CHECK(rgb_near(px(g_img, 128, 128), 165, 162, 82, 2));
            }
            CHECK(su_render_release(g_r, h) == SU_OK);
        }
    }
    /* BC7 4x4, endpoint value (x << 1 | 1): (201, 101, 41) */
    bc7_solid(bc7, 100, 50, 20);
    n = write_dds(file, 98, 4, 4, 1, 0, bc7, 16);
    {
        int k;
        for (k = 0; k < 2; k++) {
            uint32_t cs = k == 0 ? SU_DDS_CS_LINEAR : SU_DDS_CS_SRGB;
            CHECK(su_dds_parse(file, n, cs, &img, msg) == SU_OK);
            h = 0;
            CHECK(su_render_texture_create_dds(g_r, &img, SU_RENDER_UPLOAD_COPY, &h) == SU_OK);
            wait_ready(h);
            if (su_render_resource_state(g_r, h) != SU_RENDER_STATE_READY) {
                (void)su_render_release(g_r, h);
                continue;
            }
            fb = fxbuf(vtx, 64);
            su_fx_sprite(&fb, v3(0, 0, -4), 1.0f, 1.0f, 0.0f, one, NULL, view);
            bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
            s.fx_tex = vtx;
            s.fx_tex_count = fb.count;
            s.batches = &bt;
            s.batch_count = 1;
            CHECK(render(&s, g_img));
            if (k == 0) {
                CHECK(rgb_near(px(g_img, 128, 128), srgb8(201.0 / 255.0), srgb8(101.0 / 255.0), srgb8(41.0 / 255.0), 1));
            } else {
                CHECK(rgb_near(px(g_img, 128, 128), 201, 101, 41, 2));
            }
            CHECK(su_render_release(g_r, h) == SU_OK);
        }
    }
}

/* the half-float value of a solid BC6H texel with 10 bit endpoint x (the specification's formula) */
static float solid_value(uint32_t x10) {
    uint32_t unq = x10 == 0u ? 0u : (x10 == 1023u ? 0xFFFFu : ((x10 << 16) + 0x8000u) >> 10);
    return su_half_to_float((uint16_t)((unq * 31u) >> 6));
}

static SuQuatf quat_inv(SuQuatf q) {
    q.x = -q.x;
    q.y = -q.y;
    q.z = -q.z;
    return q;
}

static void test_cube_sky(void) {
    /* six faces of 8x8 BC6H with two mips (4 blocks + 1 block), face f solid (x_f, y_f, z_f) */
    static uint8_t cube[6 * (64 + 16)];
    static uint8_t file[1024];
    static const uint32_t fx[6] = {400, 350, 380, 450, 360, 420};
    static const uint32_t fy[6] = {360, 420, 440, 370, 450, 390};
    static const uint32_t fz[6] = {440, 380, 350, 430, 400, 460};
    static const SuVec3f dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    SuDdsImage img;
    char msg[160];
    SuRenderHandle raw = 0;
    SuRenderHandle env = 0;
    SuRenderDeviceInfo dev;
    Scene s;
    uint32_t f;
    uint32_t b;
    size_t n;
    int which;
    for (f = 0; f < 6u; f++) {
        for (b = 0; b < 5u; b++) {
            bc6h_solid(cube + f * 80u + b * 16u, fx[f], fy[f], fz[f]);
        }
    }
    n = write_dds(file, 95, 8, 8, 2, 1, cube, sizeof cube);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.faces == 6u && img.format == SU_TEX_BC6H_UF16);
    CHECK(su_render_device(g_r, &dev) == SU_OK);
    printf("BC textures sampled natively: %u\n", (unsigned)dev.bc_textures);
    CHECK(su_render_texture_create_dds(g_r, &img, SU_RENDER_UPLOAD_COPY, &raw) == SU_OK);
    CHECK(su_render_env_from_dds(g_r, &img, &env) == SU_OK);
    wait_ready(raw);
    wait_ready(env);
    CHECK(su_render_resource_state(g_r, env) == SU_RENDER_STATE_READY);
    if (dev.bc_textures) {
        CHECK(su_render_resource_state(g_r, raw) == SU_RENDER_STATE_READY);
    } else {
        CHECK(su_render_resource_state(g_r, raw) == SU_RENDER_STATE_FAILED);
    }
    for (which = 0; which < 2; which++) {
        SuRenderHandle cube_h = which == 0 ? raw : env;
        if (su_render_resource_state(g_r, cube_h) != SU_RENDER_STATE_READY) {
            continue;
        }
        /* each face along its axis, then intensity, then a rotated sky */
        for (f = 0; f < 6u; f++) {
            scene_init(&s);
            s.sky = 1;
            s.sky_cube = cube_h;
            s.cam_rot = look_quat(dirs[f]);
            CHECK(render(&s, g_img));
            CHECK(rgb_near(px(g_img, 128, 128), srgb8(solid_value(fx[f])), srgb8(solid_value(fy[f])), srgb8(solid_value(fz[f])), 1));
            /* a 60 degree lens looks at one face only, corner to corner */
            CHECK(rgb_near(px(g_img, 6, 7), srgb8(solid_value(fx[f])), srgb8(solid_value(fy[f])), srgb8(solid_value(fz[f])), 1));
            if (f == 0u) {
                scene_init(&s);
                s.sky = 1;
                s.sky_cube = cube_h;
                s.sky_intensity = 2.0f;
                s.cam_rot = look_quat(dirs[0]);
                CHECK(render(&s, g_img));
                CHECK(rgb_near(px(g_img, 128, 128), srgb8(2.0 * solid_value(fx[0])), srgb8(2.0 * solid_value(fy[0])),
                               srgb8(2.0 * solid_value(fz[0])), 1));
                s.sky_intensity = 0.5f;
                CHECK(render(&s, g_img));
                CHECK(rgb_near(px(g_img, 128, 128), srgb8(0.5 * solid_value(fx[0])), srgb8(0.5 * solid_value(fy[0])),
                               srgb8(0.5 * solid_value(fz[0])), 1));
            }
        }
        /* rotation: yawing the sky 90 degrees about +y moves the cube's contents; a camera looking down -z sees the
         * cube direction conj(q) * (0,0,-1) */
        {
            int a;
            for (a = 0; a < 4; a++) {
                SuQuatf q = su_quatf_axis_angle(v3(0, 1, 0), (float)a * 1.5707964f);
                SuVec3f d = su_quatf_rotate(quat_inv(q), v3(0, 0, -1));
                uint32_t face = 0;
                float best = 0.0f;
                scene_init(&s);
                s.sky = 1;
                s.sky_cube = cube_h;
                s.sky_rot = q;
                for (f = 0; f < 6u; f++) {
                    float dd = d.x * dirs[f].x + d.y * dirs[f].y + d.z * dirs[f].z;
                    if (dd > best) {
                        best = dd;
                        face = f;
                    }
                }
                CHECK(render(&s, g_img));
                CHECK(rgb_near(px(g_img, 128, 128), srgb8(solid_value(fx[face])), srgb8(solid_value(fy[face])),
                               srgb8(solid_value(fz[face])), 1));
            }
        }
        /* lod picks a coarser mip: the raw cube's mip 1 is its constant faces again, while an environment's last mip is
         * its roughest prefilter (a blur across faces) */
        scene_init(&s);
        s.sky = 1;
        s.sky_cube = cube_h;
        s.sky_lod = 1.0f;
        CHECK(render(&s, g_img));
        if (which == 0) {
            CHECK(rgb_near(px(g_img, 128, 128), srgb8(solid_value(fx[5])), srgb8(solid_value(fy[5])), srgb8(solid_value(fz[5])), 1));
        } else {
            CHECK(!rgb_near(px(g_img, 128, 128), srgb8(solid_value(fx[5])), srgb8(solid_value(fy[5])), srgb8(solid_value(fz[5])), 0));
        }
    }
    /* the background is the cube, not the procedural sky: compare against the same frame without it */
    scene_init(&s);
    s.sky = 1;
    CHECK(render(&s, g_ref));
    CHECK(px(g_ref, 128, 128)[0] != px(g_img, 128, 128)[0] || px(g_ref, 128, 128)[2] != px(g_img, 128, 128)[2]);
    /* a released cube is not drawn: the procedural sky is, byte for byte */
    {
        SuRenderHandle dead = raw;
        CHECK(su_render_release(g_r, raw) == SU_OK);
        s.sky_cube = dead;
        CHECK(render(&s, g_img));
        CHECK(memcmp(g_img, g_ref, sizeof g_img) == 0);
        s.sky_cube = 0;
        CHECK(render(&s, g_img));
        CHECK(memcmp(g_img, g_ref, sizeof g_img) == 0);
    }
    CHECK(su_render_release(g_r, env) == SU_OK);
    /* env_from_dds refuses a 2D file */
    {
        uint8_t bc1b[8];
        SuDdsImage flat;
        SuRenderHandle none = 0;
        bc1_solid(bc1b, 1, 2, 3);
        n = write_dds(file, 71, 4, 4, 1, 0, bc1b, 8);
        CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &flat, msg) == SU_OK);
        CHECK(su_render_env_from_dds(g_r, &flat, &none) == SU_E_INVALID_ARGUMENT);
    }
}

/* CPU BC6H decode against the GPU's sampling of the same blocks, every mode, signed and unsigned. */
static void test_bc6h_gpu(void) {
    static uint8_t blocks[8 * 8 * 16];
    static uint8_t file[1024 * 8 + 148];
    SuDdsImage img;
    char msg[160];
    SuRenderDeviceInfo dev;
    uint32_t seed = 4242;
    int sg;
    int round;
    long compared = 0;
    long worst = 0;
    (void)su_render_device(g_r, &dev);
    if (!dev.bc_textures) {
        printf("BC6H GPU comparison skipped: no BCn sampling on this device\n");
        return;
    }
    /* A software rasterizer's BC6H decode is not a hardware reference (Mesa lavapipe differs from the spec decode by
     * up to ~13 levels here). The CPU decoder itself is checked by test_asset_dds. */
    if (dev.type == SU_RENDER_DEVICE_CPU) {
        printf("BC6H GPU comparison skipped: software rasterizer (%s) is not a BC6H reference\n", dev.name);
        return;
    }
    for (sg = 0; sg < 2; sg++) {
        for (round = 0; round < 12; round++) {
            Scene s;
            SuFxVertex vtx[64];
            SuFxBuffer fb = fxbuf(vtx, 64);
            SuFxBatch bt;
            SuRenderHandle h = 0;
            float one[4] = {1, 1, 1, 1};
            uint32_t i;
            uint32_t bx;
            uint32_t by;
            size_t n;
            /* 64 blocks of 32 x 32 texels: random bytes, the mode of block i forced to cycle through all modes */
            for (i = 0; i < sizeof blocks; i++) {
                seed = seed * 1664525u + 1013904223u;
                blocks[i] = (uint8_t)(seed >> 20);
            }
            for (i = 0; i < 64u; i++) {
                static const uint8_t mv[14] = {0x00, 0x01, 0x02, 0x06, 0x0A, 0x0E, 0x12, 0x16, 0x1A, 0x1E, 0x03, 0x07, 0x0B, 0x0F};
                uint32_t m = (uint32_t)(i + (uint32_t)round * 5u) % 14u;
                blocks[i * 16u] = (uint8_t)((blocks[i * 16u] & (mv[m] < 2u ? 0xFCu : 0xE0u)) | mv[m]);
            }
            n = write_dds(file, sg ? 96u : 95u, 32, 32, 1, 0, blocks, sizeof blocks);
            CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK);
            CHECK(su_render_texture_create_dds(g_r, &img, SU_RENDER_UPLOAD_COPY, &h) == SU_OK);
            wait_ready(h);
            CHECK(su_render_resource_state(g_r, h) == SU_RENDER_STATE_READY);
            scene_init(&s);
            /* 224 px for 32 texels: 7 px per texel, texel centres on pixel centres (distance 4, half size from the lens) */
            su_fx_sprite(&fb, v3(0, 0, -4), 4.0f * 0.57735027f * (112.0f / 128.0f), 4.0f * 0.57735027f * (112.0f / 128.0f), 0.0f,
                         one, NULL, camera_view(&s));
            bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
            s.fx_tex = vtx;
            s.fx_tex_count = fb.count;
            s.batches = &bt;
            s.batch_count = 1;
            s.exposure = 1.0f / 64.0f; /* scales the half range into 0..1 so more of it is visible */
            CHECK(render(&s, g_img));
            {
                static float lvl[32 * 32 * 4];
                CHECK(su_dds_decode_rgba32f(&img, 0, 0, lvl) == SU_OK);
                for (by = 0; by < 32u; by++) {
                    for (bx = 0; bx < 32u; bx++) {
                        const float *c = lvl + ((size_t)by * 32u + bx) * 4u;
                        const uint8_t *p = px(g_img, 16u + 7u * bx + 3u, 16u + 7u * by + 3u);
                        uint32_t ch;
                        for (ch = 0; ch < 3u; ch++) {
                            /* the sprite's additive blend clamps negatives away (signed format), exposure scales */
                            double lin = (double)c[ch] / 64.0;
                            int want = lin <= 0.0 ? 0 : srgb8(lin > 1.0 ? 1.0 : lin);
                            int d = abs((int)p[ch] - want);
                            worst = d > worst ? d : worst;
                            compared++;
                            if (d > 2) {
                                static int shown = 0;
                                if (shown++ < 10) {
                                    fprintf(stderr, "BC6H %s round %d texel %u,%u ch %u: gpu %d cpu %d (value %g)\n",
                                            sg ? "signed" : "unsigned", round, (unsigned)bx, (unsigned)by, (unsigned)ch, p[ch],
                                            want, c[ch]);
                                }
                            }
                            CHECK(d <= 2);
                        }
                    }
                }
            }
            CHECK(su_render_release(g_r, h) == SU_OK);
        }
    }
    printf("BC6H: %ld channel values compared, CPU decoder vs GPU sampling, worst difference %ld levels of 255\n", compared,
           worst);
}

/* Soft particles: a sprite in front of an opaque wall fades with its distance to it. */
static void test_soft_particles(void) {
    uint8_t white[4 * 4];
    SuFxVertex vtx[64];
    SuFxBuffer fb = fxbuf(vtx, 64);
    SuFxBatch bt;
    SuRenderInstance wall;
    SuRenderHandle h;
    Scene s;
    float col[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    static const float gaps[4] = {0.25f, 0.5f, 1.0f, 3.0f};
    uint32_t i;
    memset(white, 255, sizeof white);
    h = make_tex(SU_TEX_RGBA8_UNORM, 2, 2, white);
    memset(&wall, 0, sizeof wall);
    wall.pos[2] = -16.0f; /* a 20 unit black box: its front face is the plane z = -6, filling the view centre */
    wall.scale = 20.0f;
    wall.rot[3] = 1.0f;
    wall.color[3] = 1.0f;
    wall.mesh = SU_MESH_BOX;
    for (i = 0; i < 4u; i++) {
        double fade = gaps[i] / 2.0 > 1.0 ? 1.0 : gaps[i] / 2.0;
        double got;
        uint32_t k;
        scene_init(&s);
        s.inst = &wall;
        s.inst_count = 1;
        fb.count = 0;
        /* left: in front of the wall; right of the wall's edge nothing is behind it (half the sprite over the clear) */
        su_fx_sprite(&fb, v3(0, 0, -6.0f + gaps[i]), 0.6f, 0.6f, 0.0f, col, NULL, camera_view(&s));
        bt = su_fx_batch(h, (i & 1u) != 0u ? SU_FX_BLEND_PREMUL : SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
        s.fx_tex = vtx;
        s.fx_tex_count = fb.count;
        s.batches = &bt;
        s.batch_count = 1;
        for (k = 0; k < 2u; k++) {
            bt.soft_distance = k == 0u ? 0.0f : 2.0f;
            CHECK(render(&s, g_img));
            got = srgb_to_lin((double)px(g_img, W / 2, H / 2)[0] / 255.0);
            printf("soft: gap %.2f, soft distance %.0f -> %.4f (expected %.4f)\n", gaps[i], (double)bt.soft_distance, got,
                   k == 0u ? 0.5 : 0.5 * fade);
            CHECK(fabs(got - (k == 0u ? 0.5 : 0.5 * fade)) < 0.01);
        }
    }
    /* nothing behind the sprite (the clear colour): no fade whatever the distance */
    scene_init(&s);
    fb.count = 0;
    su_fx_sprite(&fb, v3(0, 0, -6.0f), 0.6f, 0.6f, 0.0f, col, NULL, camera_view(&s));
    bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
    bt.soft_distance = 100.0f;
    s.fx_tex = vtx;
    s.fx_tex_count = fb.count;
    s.batches = &bt;
    s.batch_count = 1;
    CHECK(render(&s, g_img));
    CHECK(near_i(px(g_img, W / 2, H / 2)[0], srgb8(0.5), 1));
    CHECK(su_render_release(g_r, h) == SU_OK);
}

/* SU_FX_BATCH_SORT: quads of sorted batches interleave back to front across batches. */
static void test_fx_sort(void) {
    uint8_t red[4 * 4];
    uint8_t blue[4 * 4];
    uint8_t green[4 * 4];
    SuFxVertex vtx[64];
    SuFxBuffer fb = fxbuf(vtx, 64);
    SuFxBatch bt[3];
    SuRenderHandle hr;
    SuRenderHandle hb;
    SuRenderHandle hg;
    SuRenderStats st;
    Scene s;
    float one[4] = {1, 1, 1, 1};
    uint32_t i;
    for (i = 0; i < 4u; i++) { /* opaque premultiplied texels */
        red[i * 4u] = 255;
        red[i * 4u + 1u] = red[i * 4u + 2u] = 0;
        blue[i * 4u + 2u] = 255;
        blue[i * 4u] = blue[i * 4u + 1u] = 0;
        green[i * 4u + 1u] = 255;
        green[i * 4u] = green[i * 4u + 2u] = 0;
        red[i * 4u + 3u] = blue[i * 4u + 3u] = green[i * 4u + 3u] = 255;
    }
    hr = make_tex(SU_TEX_RGBA8_UNORM, 2, 2, red);
    hb = make_tex(SU_TEX_RGBA8_UNORM, 2, 2, blue);
    hg = make_tex(SU_TEX_RGBA8_UNORM, 2, 2, green);
    scene_init(&s);
    /* batch 0 (green, unsorted, drawn first); batch 1 red at z -4 (small) and -8 (large); batch 2 blue at -6 (medium)
     * and -10 (largest). Back to front: blue -10, red -8, blue -6, red -4: rings blue, red, blue, red from the edge. */
    su_fx_sprite(&fb, v3(0, 0, -12), 10.0f, 10.0f, 0.0f, one, NULL, camera_view(&s));
    bt[0] = su_fx_batch(hg, SU_FX_BLEND_PREMUL, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
    su_fx_sprite(&fb, v3(0, 0, -4), 0.4f, 0.4f, 0.0f, one, NULL, camera_view(&s));
    su_fx_sprite(&fb, v3(0, 0, -8), 2.4f, 2.4f, 0.0f, one, NULL, camera_view(&s));
    bt[1] = su_fx_batch(hr, SU_FX_BLEND_PREMUL, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 6, &fb);
    su_fx_sprite(&fb, v3(0, 0, -6), 1.2f, 1.2f, 0.0f, one, NULL, camera_view(&s));
    su_fx_sprite(&fb, v3(0, 0, -10), 4.0f, 4.0f, 0.0f, one, NULL, camera_view(&s));
    bt[2] = su_fx_batch(hb, SU_FX_BLEND_PREMUL, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 18, &fb);
    s.fx_tex = vtx;
    s.fx_tex_count = fb.count;
    s.batches = bt;
    s.batch_count = 3;
    /* unsorted: batch order, so blue (drawn last) covers the centre */
    CHECK(render(&s, g_img));
    CHECK(rgb_near(px(g_img, W / 2, H / 2), 0, 0, 255, 1));
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.fx_quads_sorted == 0u && st.fx_sort_draws == 0u);
    /* sorted: centre red (z -4), then a blue ring (-6), a red ring (-8), a blue ring (-10), green outside (unsorted, first) */
    bt[1].flags = bt[2].flags = SU_FX_BATCH_SORT;
    CHECK(render(&s, g_img));
    {
        /* screen radius of a half size h at distance z: h / z * (H / 2) / tan(fov / 2) */
        static const float zs[4] = {4.0f, 6.0f, 8.0f, 10.0f};
        static const float hs[4] = {0.4f, 1.2f, 2.4f, 4.0f};
        static const int want_red[4] = {1, 0, 1, 0};
        double inner = 0.0;
        uint32_t k;
        for (k = 0; k < 4u; k++) {
            double outer = hs[k] / zs[k] * (H / 2.0) / tan(FOV / 2.0);
            int x = W / 2 + (int)((inner + outer) / 2.0);
            const uint8_t *p = px(g_img, (uint32_t)x, H / 2);
            printf("sort: ring %u at x %d -> %d %d %d (want %s)\n", k, x, p[0], p[1], p[2], want_red[k] ? "red" : "blue");
            CHECK(want_red[k] ? rgb_near(p, 255, 0, 0, 1) : rgb_near(p, 0, 0, 255, 1));
            inner = outer;
        }
        CHECK(rgb_near(px(g_img, 4, 4), 0, 255, 0, 1));
    }
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.fx_quads_sorted == 4u && st.fx_sort_draws == 4u &&
          st.fx_batches_drawn == 3u);
    /* a sorted batch that is not made of quads is drawn in place, unsorted */
    bt[2].vertex_count = 9;
    CHECK(render(&s, g_img));
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.fx_quads_sorted == 2u && st.fx_batches_drawn == 3u);
    CHECK(su_render_release(g_r, hr) == SU_OK && su_render_release(g_r, hb) == SU_OK && su_render_release(g_r, hg) == SU_OK);
}

/* CPU decoders of the LDR block formats against the GPU's sampling of random blocks (BC1, BC2, BC3, BC4, BC5, BC7 with
 * every mode). Drawn additively, so the pixel is the texel's rgb x alpha. */
static void test_bc_ldr_gpu(void) {
    static const struct {
        uint32_t dxgi;
        uint32_t block;
        double tol; /* linear codes: hardware interpolates BC1..BC5 at its own precision; BC7 is exact */
        const char *name;
    } fmts[6] = {{71, 8, 3.5, "BC1"}, {74, 16, 3.5, "BC2"}, {77, 16, 3.5, "BC3"},
                 {80, 8, 3.5, "BC4"}, {83, 16, 3.5, "BC5"}, {98, 16, 1.0, "BC7"}};
    static uint8_t blocks[8 * 8 * 16];
    static uint8_t file[1024 * 8 + 148];
    SuDdsImage img;
    char msg[160];
    SuRenderDeviceInfo dev;
    uint32_t seed = 777;
    uint32_t fi;
    long compared = 0;
    (void)su_render_device(g_r, &dev);
    if (!dev.bc_textures || dev.type == SU_RENDER_DEVICE_CPU) {
        printf("BC1-BC7 GPU comparison skipped: %s\n", dev.bc_textures ? "software rasterizer is not a reference" : "no BCn sampling");
        return;
    }
    for (fi = 0; fi < 6u; fi++) {
        int round;
        long worst = 0;
        for (round = 0; round < (fmts[fi].dxgi == 98u ? 8 : 2); round++) {
            Scene s;
            SuFxVertex vtx[64];
            SuFxBuffer fb = fxbuf(vtx, 64);
            SuFxBatch bt;
            SuRenderHandle h = 0;
            float one[4] = {1, 1, 1, 1};
            static float lvl[32 * 32 * 4];
            size_t bytes = 64u * fmts[fi].block;
            uint32_t i;
            uint32_t bx;
            uint32_t by;
            size_t n;
            for (i = 0; i < bytes; i++) {
                seed = seed * 1664525u + 1013904223u;
                blocks[i] = (uint8_t)(seed >> 20);
            }
            /* BC3 alpha, BC4 and BC5 channel blocks in the eight-value mode (a0 > a1): the six-value mode's fifths are
             * approximated differently by some hardware (seen on Pascal: about 3/16, 6/16, 10/16, 13/16); the CPU
             * decoder follows the specification, checked by test_asset_dds */
            for (i = 0; i < 64u && (fmts[fi].dxgi == 77u || fmts[fi].dxgi == 80u || fmts[fi].dxgi == 83u); i++) {
                uint32_t half;
                for (half = 0; half < (fmts[fi].dxgi == 83u ? 2u : 1u); half++) {
                    uint8_t *e = blocks + i * fmts[fi].block + half * 8u;
                    if (e[0] < e[1]) {
                        uint8_t tmp = e[0];
                        e[0] = e[1];
                        e[1] = tmp;
                    } else if (e[0] == e[1]) {
                        e[0] = (uint8_t)(e[0] | 1u);
                        e[1] = (uint8_t)(e[1] & 0xFEu);
                    }
                }
            }
            if (fmts[fi].dxgi == 98u) { /* force block i to mode (i + round) % 8 */
                for (i = 0; i < 64u; i++) {
                    uint32_t m = (i + (uint32_t)round) % 8u;
                    blocks[i * 16u] = (uint8_t)((blocks[i * 16u] & (uint32_t)(0xFEu << m)) | (1u << m));
                }
            }
            n = write_dds(file, fmts[fi].dxgi, 32, 32, 1, 0, blocks, bytes);
            CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK);
            CHECK(su_render_texture_create_dds(g_r, &img, SU_RENDER_UPLOAD_COPY, &h) == SU_OK);
            wait_ready(h);
            CHECK(su_render_resource_state(g_r, h) == SU_RENDER_STATE_READY);
            CHECK(su_dds_decode_rgba32f(&img, 0, 0, lvl) == SU_OK);
            scene_init(&s);
            su_fx_sprite(&fb, v3(0, 0, -4), 4.0f * 0.57735027f * (112.0f / 128.0f), 4.0f * 0.57735027f * (112.0f / 128.0f), 0.0f,
                         one, NULL, camera_view(&s));
            bt = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
            s.fx_tex = vtx;
            s.fx_tex_count = fb.count;
            s.batches = &bt;
            s.batch_count = 1;
            CHECK(render(&s, g_img));
            for (by = 0; by < 32u; by++) {
                for (bx = 0; bx < 32u; bx++) {
                    const float *c = lvl + ((size_t)by * 32u + bx) * 4u;
                    const uint8_t *p = px(g_img, 16u + 7u * bx + 3u, 16u + 7u * by + 3u);
                    uint32_t ch;
                    for (ch = 0; ch < 3u; ch++) {
                        double tol = fmts[fi].tol / 255.0;
                        double lin = (double)c[ch] * (double)c[3];
                        int lo = srgb8(lin - tol < 0.0 ? 0.0 : lin - tol) - 1;
                        int hi = srgb8(lin + tol > 1.0 ? 1.0 : lin + tol) + 1;
                        int want = srgb8(lin);
                        int d = abs((int)p[ch] - want);
                        worst = d > worst ? d : worst;
                        compared++;
                        if (p[ch] < lo || p[ch] > hi) {
                            static int shown = 0;
                            if (shown++ < 10) {
                                fprintf(stderr, "%s round %d texel %u,%u ch %u: gpu %d cpu %d (rgba %g %g %g %g)\n",
                                        fmts[fi].name, round, (unsigned)bx, (unsigned)by, (unsigned)ch, p[ch], want, c[0],
                                        c[1], c[2], c[3]);
                            }
                        }
                        CHECK(p[ch] >= lo && p[ch] <= hi);
                    }
                }
            }
            CHECK(su_render_release(g_r, h) == SU_OK);
        }
        printf("%s: CPU decoder vs GPU sampling, worst difference %ld levels of 255 (sRGB output)\n", fmts[fi].name, worst);
    }
    printf("LDR BC: %ld channel values compared\n", compared);
}

/* Environment cubes from LDR DDS files and from cubes larger than the size cap (box-downsampled on the CPU). */
static void test_env_ldr_and_downsample(void) {
    static uint8_t px32[6 * 32 * 32 * 4];
    static uint8_t file[148 + 6 * 32 * 32 * 4];
    static float dec[32 * 32 * 4];
    static float faces[6 * 8 * 8 * 4];
    SuDdsImage img;
    char msg[160];
    SuRenderHandle a = 0;
    SuRenderHandle b = 0;
    SuRenderHandle none = 0;
    Scene s;
    uint32_t seed = 31337;
    uint32_t i;
    uint32_t f;
    size_t n;
    static uint8_t img_a[W * H * 4];
    for (i = 0; i < sizeof px32; i++) {
        seed = seed * 1664525u + 1013904223u;
        px32[i] = (uint8_t)(seed >> 24);
    }
    /* RGBA8 sRGB cube of 32: capped at 8, every output texel is the mean of a 4 x 4 block of the decoded (linear) face */
    n = write_dds(file, 29, 32, 32, 1, 1, px32, sizeof px32);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.faces == 6u);
    CHECK(su_render_env_from_dds_ex(g_r, &img, 8, &a) == SU_OK);
    for (f = 0; f < 6u; f++) {
        uint32_t x;
        uint32_t y;
        CHECK(su_dds_decode_rgba32f(&img, f, 0, dec) == SU_OK);
        for (y = 0; y < 8u; y++) {
            for (x = 0; x < 8u; x++) {
                float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                uint32_t ii;
                uint32_t jj;
                uint32_t c;
                for (jj = 0; jj < 4u; jj++) {
                    for (ii = 0; ii < 4u; ii++) {
                        for (c = 0; c < 4u; c++) {
                            acc[c] += dec[((size_t)(y * 4u + jj) * 32u + x * 4u + ii) * 4u + c];
                        }
                    }
                }
                for (c = 0; c < 4u; c++) {
                    faces[(((size_t)f * 8u + y) * 8u + x) * 4u + c] = acc[c] * (1.0f / 16.0f);
                }
            }
        }
    }
    CHECK(su_render_env_create(g_r, 8, faces, &b) == SU_OK);
    wait_ready(a);
    wait_ready(b);
    CHECK(su_render_resource_state(g_r, a) == SU_RENDER_STATE_READY && su_render_resource_state(g_r, b) == SU_RENDER_STATE_READY);
    scene_init(&s);
    s.sky = 1;
    s.sky_cube = a;
    CHECK(render(&s, img_a));
    s.sky_cube = b;
    CHECK(render(&s, g_img));
    CHECK(memcmp(img_a, g_img, sizeof g_img) == 0);
    printf("env: 32 px RGBA8 sRGB cube capped at 8 matches the box-downsampled faces (%s)\n",
           memcmp(img_a, g_img, sizeof g_img) == 0 ? "identical" : "DIFFERENT");
    CHECK(su_render_release(g_r, a) == SU_OK);
    CHECK(su_render_release(g_r, b) == SU_OK);
    /* the same file uncapped is a 32 environment; a cap that is not a power of two in 8..1024 is refused */
    CHECK(su_render_env_from_dds(g_r, &img, &a) == SU_OK);
    CHECK(su_render_release(g_r, a) == SU_OK);
    CHECK(su_render_env_from_dds_ex(g_r, &img, 100, &none) == SU_E_INVALID_ARGUMENT && none == 0u);
    CHECK(su_render_env_from_dds_ex(g_r, &img, 2048, &none) == SU_E_INVALID_ARGUMENT);
    /* LDR block cubes decode on the CPU whatever the device: BC2 (DXT3-style) and BC7, 16 px */
    {
        static uint8_t bc[6 * 16 * 16];
        static const uint32_t dx[2] = {74, 98};
        uint32_t k;
        for (k = 0; k < 2u; k++) {
            for (i = 0; i < 6u * 16u; i++) {
                uint8_t *blk = bc + i * 16u;
                if (dx[k] == 74u) {
                    memset(blk, 0xFF, 8);
                    blk[8] = blk[10] = 0x00;
                    blk[9] = blk[11] = 0x80; /* c0 = c1: red 132 */
                    memset(blk + 12, 0, 4);
                } else {
                    bc7_solid(blk, 66, 0, 0); /* red 133 */
                }
            }
            n = write_dds(file, dx[k], 16, 16, 1, 1, bc, sizeof bc);
            CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK);
            CHECK(su_render_env_from_dds(g_r, &img, &a) == SU_OK);
            wait_ready(a);
            scene_init(&s);
            s.sky = 1;
            s.sky_cube = a;
            CHECK(render(&s, g_img));
            {
                const uint8_t *c = px(g_img, W / 2, H / 2);
                printf("env: %s cube of red %d -> sky pixel %d %d %d\n", k == 0 ? "BC2" : "BC7", k == 0 ? 132 : 133, c[0], c[1],
                       c[2]);
                int gb = k == 0 ? 0 : srgb8(1.0 / 255.0); /* BC7: 7-bit 0 with p-bit 1 is 1 */
                CHECK(abs((int)c[0] - srgb8((k == 0 ? 132.0 : 133.0) / 255.0)) <= 2 && abs((int)c[1] - gb) <= 1 &&
                      abs((int)c[2] - gb) <= 1);
            }
            CHECK(su_render_release(g_r, a) == SU_OK);
        }
    }
}

/* GPU environment prefilter against the CPU reference: every mip drawn as the sky from three directions. */
static void test_env_gpu(void) {
    static uint8_t a[W * H * 4];
    static const SuVec3f dirs[3] = {{0, 0, -1}, {1, 0.3f, 0}, {-0.2f, 1, 0.4f}};
    SuRenderLighting sky;
    SuRenderHandle hc = 0;
    SuRenderHandle hg = 0;
    SuRenderStats st;
    SuRenderDeviceInfo dev;
    Scene s;
    float *faces;
    uint32_t size = 64;
    uint32_t mips = su_env_mip_count(size);
    uint32_t m;
    uint32_t d;
    uint32_t i;
    uint32_t seed = 99;
    double cpu_ms;
    double gpu_ms;
    int worst = 0;
    double sum = 0.0;
    long n = 0;
    (void)su_render_device(g_r, &dev);
    faces = malloc((size_t)6u * 1024u * 1024u * 4u * sizeof *faces);
    CHECK(faces != NULL);
    if (faces == NULL) {
        return;
    }
    memset(&sky, 0, sizeof sky);
    sky.sun_dir = v3(0.3f, -0.5f, -0.8f);
    sky.sun_intensity = 4.0f;
    sky.sun_color = v3(1, 0.9f, 0.8f);
    sky.sky_zenith = v3(0.1f, 0.2f, 0.6f);
    sky.sky_horizon = v3(0.6f, 0.5f, 0.4f);
    sky.sky_nadir = v3(0.05f, 0.05f, 0.08f);
    sky.up = v3(0, 1, 0);
    su_env_sky_faces(size, &sky, faces);
    for (i = 0; i < 6u * size * size; i++) { /* texture detail and a few HDR hot spots */
        seed = seed * 1664525u + 1013904223u;
        faces[i * 4u + 0u] *= 0.6f + 0.8f * (float)(seed >> 24) / 255.0f;
        faces[i * 4u + 1u] *= 0.6f + 0.8f * (float)((seed >> 16) & 255u) / 255.0f;
        if ((seed & 0x3FFu) == 0u) {
            faces[i * 4u + 0u] = faces[i * 4u + 1u] = faces[i * 4u + 2u] = 40.0f;
        }
    }
    CHECK(su_render_env_create_ex(g_r, size, faces, SU_ENV_CPU, &hc) == SU_OK);
    CHECK(su_render_stats(g_r, &st) == SU_OK && st.env_prefilter_gpu == 0u);
    cpu_ms = st.env_prefilter_ms;
    CHECK(su_render_env_create_ex(g_r, size, faces, 0, &hg) == SU_OK);
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    gpu_ms = st.env_prefilter_ms;
    {
        SuRenderHandle bad = 0;
        CHECK(su_render_env_create_ex(g_r, size, faces, 0x80u, &bad) == SU_E_INVALID_ARGUMENT && bad == 0u);
    }
    printf("env %u: CPU prefilter %.1f ms, %s %.1f ms\n", (unsigned)size, cpu_ms, st.env_prefilter_gpu ? "GPU" : "CPU (no GPU path)",
           gpu_ms);
    if (!st.env_prefilter_gpu) {
        CHECK(su_render_release(g_r, hc) == SU_OK && su_render_release(g_r, hg) == SU_OK);
        free(faces);
        return;
    }
    wait_ready(hc);
    CHECK(su_render_resource_state(g_r, hg) == SU_RENDER_STATE_READY); /* READY on return */
    for (m = 0; m < mips; m++) {
        for (d = 0; d < 3u; d++) {
            long k;
            scene_init(&s);
            s.sky = 1;
            s.cam_rot = look_quat(dirs[d]);
            s.sky_lod = (float)m;
            s.exposure = 0.5f;
            s.sky_cube = hc;
            CHECK(render(&s, a));
            s.sky_cube = hg;
            CHECK(render(&s, g_img));
            for (k = 0; k < (long)(W * H); k++) {
                int c;
                for (c = 0; c < 3; c++) {
                    int df = abs((int)a[k * 4 + c] - (int)g_img[k * 4 + c]);
                    worst = df > worst ? df : worst;
                    sum += df;
                    n++;
                }
            }
        }
    }
    printf("env GPU vs CPU prefilter, %u mips x 3 views: mean %.3f, worst %d levels of 255\n", (unsigned)mips, sum / (double)n,
           worst);
    CHECK(sum / (double)n < (dev.type == SU_RENDER_DEVICE_CPU ? 2.0 : 1.0));
    CHECK(worst <= (dev.type == SU_RENDER_DEVICE_CPU ? 40 : 8));
    CHECK(su_render_release(g_r, hc) == SU_OK && su_render_release(g_r, hg) == SU_OK);
    /* the cost at the sizes that matter (1024: the CPU path takes seconds; skipped on software rasterizers) */
    for (size = 256; size <= (dev.type == SU_RENDER_DEVICE_CPU ? 256u : 1024u); size *= 4u) {
        su_env_sky_faces(size, &sky, faces);
        CHECK(su_render_env_create(g_r, size, faces, &hg) == SU_OK);
        CHECK(su_render_stats(g_r, &st) == SU_OK);
        printf("env %u: GPU prefilter %.1f ms (upload included)\n", (unsigned)size, st.env_prefilter_ms);
        CHECK(su_render_release(g_r, hg) == SU_OK);
        if (size == 256u) {
            CHECK(su_render_env_create_ex(g_r, size, faces, SU_ENV_CPU, &hc) == SU_OK);
            CHECK(su_render_stats(g_r, &st) == SU_OK);
            printf("env %u: CPU prefilter %.1f ms\n", (unsigned)size, st.env_prefilter_ms);
            CHECK(su_render_release(g_r, hc) == SU_OK);
        }
    }
    free(faces);
}

static void test_skips_and_unchanged(void) {
    Scene s;
    SuFxVertex vtx[64];
    SuFxBuffer fb = fxbuf(vtx, 64);
    SuFxBatch bt[4];
    SuRenderHandle h;
    SuRenderHandle cubeh = 0;
    SuRenderStats st;
    float one[4] = {1, 1, 1, 1};
    uint8_t a[W * H * 4];
    SuRenderTextureDesc d;
    uint8_t face[4] = {1, 2, 3, 255};
    const uint8_t *cm[6];
    uint32_t i;
    h = make_tex(SU_TEX_RGBA8_UNORM, 2, 2, white4);
    for (i = 0; i < 6u; i++) {
        cm[i] = face;
    }
    memset(&d, 0, sizeof d);
    d.width = d.height = 1;
    d.format = SU_TEX_RGBA8_UNORM;
    d.mip_count = 1;
    d.mip_data = cm;
    d.layers = 6;
    d.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_texture_create(g_r, &d, &cubeh) == SU_OK);
    wait_ready(cubeh);
    scene_init(&s);
    su_fx_sprite(&fb, v3(0, 0, -4), 1.0f, 1.0f, 0.0f, one, NULL, camera_view(&s));
    bt[0] = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);
    bt[1] = su_fx_batch(h + 12345u, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);   /* stale handle */
    bt[2] = su_fx_batch(cubeh, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &fb);        /* a cube, not 2D */
    bt[3] = su_fx_batch(h, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 600, &fb);          /* past the buffer */
    s.fx_tex = vtx;
    s.fx_tex_count = fb.count;
    s.batches = bt;
    s.batch_count = 4;
    CHECK(render(&s, g_img));
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    CHECK(st.fx_batches_drawn == 1u);
    CHECK(st.fx_batches_skipped == 2u); /* the empty range of bt[3] is silently nothing */
    CHECK(px(g_img, 128, 128)[0] == 255);
    /* a frame that uses none of the new fields is unchanged by the presence of resources and equals itself */
    scene_init(&s);
    CHECK(render(&s, a));
    CHECK(render(&s, g_img));
    CHECK(memcmp(a, g_img, sizeof a) == 0);
    CHECK(px(a, 128, 128)[0] == 0 && px(a, 128, 128)[2] == 0);
    CHECK(su_render_release(g_r, h) == SU_OK && su_render_release(g_r, cubeh) == SU_OK);
    /* textures: layers other than 0, 1, 6 and non-square cubes are refused */
    d.layers = 4;
    CHECK(su_render_texture_create(g_r, &d, &h) == SU_E_INVALID_ARGUMENT);
    d.layers = 6;
    d.width = 2;
    d.height = 1;
    CHECK(su_render_texture_create(g_r, &d, &h) == SU_E_INVALID_ARGUMENT);
}

static const char *type_name(uint32_t t) {
    return t == SU_RENDER_DEVICE_DISCRETE ? "discrete" : t == SU_RENDER_DEVICE_INTEGRATED ? "integrated"
                                                       : t == SU_RENDER_DEVICE_CPU        ? "cpu"
                                                                                          : "other";
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
    test_quadrants_and_colour();
    test_atlas();
    test_colour_space_and_blend();
    test_hdr_texture_bloom();
    test_axis_billboards();
    test_ribbon();
    test_dds_2d();
    test_cube_sky();
    test_bc6h_gpu();
    test_soft_particles();
    test_fx_sort();
    test_bc_ldr_gpu();
    test_env_ldr_and_downsample();
    test_env_gpu();
    test_skips_and_unchanged();
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    if (validation) {
        printf("validation %s: %u errors, %u warnings\n", st.validation_enabled ? "on" : "not available",
               (unsigned)st.validation_errors, (unsigned)st.validation_warnings);
        CHECK(st.validation_errors == 0u && st.validation_warnings == 0u);
    }
    su_render_destroy(g_r);
    printf("test_render_texfx: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail != 0 ? 1 : 0;
}
