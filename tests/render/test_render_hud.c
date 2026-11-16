/* Textured 2D HUD batches, per-batch nearest/linear sampling, scissor clipping, mixed font + sprite draw order,
 * skipped invalid batches, and V12 frame-descriptor compatibility, rendered offscreen and checked pixel by pixel.
 * Usage: test_render_hud [--no-validation]   (exit 77 without a usable Vulkan device) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdint.h>
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

static SuRenderer *g_r;
static uint8_t g_img[W * H * 4u];
static uint8_t g_ref[W * H * 4u];

static SuVec3f v3(float x, float y, float z) {
    SuVec3f v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static uint32_t rgba_u32(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
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

static void base_frame_desc(SuRenderFrameDesc *f, const float clear[4]) {
    memset(f, 0, sizeof *f);
    f->struct_size = sizeof *f;
    f->camera.rot.w = 1.0f;
    f->camera.fov_y = 1.0471976f;
    f->camera.near_z = 0.05f;
    f->light.sun_dir = v3(0, 0, -1);
    f->light.up = v3(0, 1, 0);
    f->light.tonemap = SU_TONEMAP_CLAMP;
    f->light.no_sky = 1u;
    f->light.exposure = 1.0f;
    if (clear != NULL) {
        memcpy(f->light.clear_color, clear, sizeof f->light.clear_color);
    } else {
        f->light.clear_color[3] = 1.0f;
    }
}

static int render_hud_frame(const SuHud *h, const SuHudBatch *batches, uint32_t batch_count, const float clear[4],
                            uint32_t output_alpha, uint8_t *out) {
    SuRenderFrameDesc f;
    uint32_t st;
    base_frame_desc(&f, clear);
    f.hud = h != NULL ? h->v : NULL;
    f.hud_vertex_count = h != NULL ? h->count : 0u;
    f.hud_batches = batches;
    f.hud_batch_count = batch_count;
    f.output_alpha = output_alpha;
    st = su_render_frame_ex(g_r, &f);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "frame failed: %s\n", su_render_last_error(g_r));
        return 0;
    }
    CHECK(su_render_readback(g_r, out, (size_t)W * H * 4u) == SU_OK);
    return 1;
}

static SuRenderHandle make_tex_ex(uint32_t fmt, uint32_t w, uint32_t h, uint32_t layers, const uint8_t *data,
                                  int wait_upload) {
    SuRenderTextureDesc d;
    SuRenderHandle hnd = 0;
    const uint8_t *mips[6] = {data, data, data, data, data, data};
    int k;
    memset(&d, 0, sizeof d);
    d.width = w;
    d.height = h;
    d.format = fmt;
    d.mip_count = 1u;
    d.mip_data = mips;
    d.flags = SU_RENDER_UPLOAD_COPY;
    d.layers = layers;
    CHECK(su_render_texture_create(g_r, &d, &hnd) == SU_OK);
    if (wait_upload) {
        for (k = 0; k < 40 && su_render_resource_state(g_r, hnd) == SU_RENDER_STATE_PENDING; k++) {
            SuRenderFrameDesc f;
            base_frame_desc(&f, NULL);
            CHECK(su_render_frame_ex(g_r, &f) == SU_OK);
        }
        CHECK(su_render_resource_state(g_r, hnd) == SU_RENDER_STATE_READY);
    }
    return hnd;
}

static void test_textured_quads(void) {
    /* 2x2 RGBA8 sRGB: TL red, TR green, BL blue, BR yellow */
    static const uint8_t quad_pixels[2 * 2 * 4] = {
        255, 0,   0,   255, 0,   255, 0,   255,
        0,   0,   255, 255, 255, 255, 0,   255,
    };
    static const uint8_t grey128[4] = {128, 128, 128, 255};
    static const uint8_t tint_tex_px[4] = {200, 100, 50, 255};
    static const uint8_t half_alpha_px[4] = {255, 0, 0, 128};
    SuRenderHandle tex_quad = make_tex_ex(SU_TEX_RGBA8_SRGB, 2, 2, 1, quad_pixels, 1);
    SuRenderHandle tex_srgb = make_tex_ex(SU_TEX_RGBA8_SRGB, 1, 1, 1, grey128, 1);
    SuRenderHandle tex_unorm = make_tex_ex(SU_TEX_RGBA8_UNORM, 1, 1, 1, grey128, 1);
    SuRenderHandle tex_tint = make_tex_ex(SU_TEX_RGBA8_SRGB, 1, 1, 1, tint_tex_px, 1);
    SuRenderHandle tex_alpha = make_tex_ex(SU_TEX_RGBA8_SRGB, 1, 1, 1, half_alpha_px, 1);
    SuHudVertex verts[64];
    SuHud h;
    SuHudBatch batches[8];
    SuRenderStats st;
    float clear_blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float sub_uv[4] = {0.5f, 0.0f, 1.0f, 0.5f}; /* top-right green texel */

    su_hud_begin(&h, verts, 64, W, H);
    /* batch 0: full 2x2 quad, nearest */
    batches[0] = su_hud_batch(tex_quad, h.count, &h);
    su_hud_sprite(&h, 16.0f, 16.0f, 64.0f, 64.0f, NULL, rgba_u32(255, 255, 255, 255));
    batches[0].vertex_count = h.count - batches[0].first_vertex;
    batches[0].flags = SU_HUD_BATCH_NEAREST;

    /* batch 1: sub-UV (top-right green texel) */
    batches[1] = su_hud_batch(tex_quad, h.count, &h);
    su_hud_sprite(&h, 96.0f, 16.0f, 40.0f, 40.0f, sub_uv, rgba_u32(255, 255, 255, 255));
    batches[1].vertex_count = h.count - batches[1].first_vertex;
    batches[1].flags = SU_HUD_BATCH_NEAREST;

    /* batch 2: vertex tinting on sRGB texture */
    batches[2] = su_hud_batch(tex_tint, h.count, &h);
    su_hud_sprite(&h, 150.0f, 16.0f, 40.0f, 40.0f, NULL, rgba_u32(128, 255, 128, 255));
    batches[2].vertex_count = h.count - batches[2].first_vertex;

    /* batch 3: sRGB 128 vs batch 4: UNORM 128 */
    batches[3] = su_hud_batch(tex_srgb, h.count, &h);
    su_hud_sprite(&h, 16.0f, 100.0f, 40.0f, 40.0f, NULL, rgba_u32(255, 255, 255, 255));
    batches[3].vertex_count = h.count - batches[3].first_vertex;

    batches[4] = su_hud_batch(tex_unorm, h.count, &h);
    su_hud_sprite(&h, 70.0f, 100.0f, 40.0f, 40.0f, NULL, rgba_u32(255, 255, 255, 255));
    batches[4].vertex_count = h.count - batches[4].first_vertex;

    /* batch 5: semi-transparent red over blue clear background */
    batches[5] = su_hud_batch(tex_alpha, h.count, &h);
    su_hud_sprite(&h, 130.0f, 100.0f, 40.0f, 40.0f, NULL, rgba_u32(255, 255, 255, 255));
    batches[5].vertex_count = h.count - batches[5].first_vertex;

    CHECK(render_hud_frame(&h, batches, 6u, clear_blue, SU_RENDER_OUTPUT_ALPHA_OPAQUE, g_img));
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    CHECK(st.hud_batches_drawn == 6u && st.hud_batches_skipped == 0u);

    /* Quadrant centers of (16, 16, 64, 64): TL (32,32), TR (64,32), BL (32,64), BR (64,64) */
    CHECK(rgb_near(px(g_img, 32, 32), 255, 0, 0, 2));
    CHECK(rgb_near(px(g_img, 64, 32), 0, 255, 0, 2));
    CHECK(rgb_near(px(g_img, 32, 64), 0, 0, 255, 2));
    CHECK(rgb_near(px(g_img, 64, 64), 255, 255, 0, 2));

    /* Sub-UV green everywhere inside (96..135, 16..55) */
    CHECK(rgb_near(px(g_img, 100, 20), 0, 255, 0, 2));
    CHECK(rgb_near(px(g_img, 130, 50), 0, 255, 0, 2));

    /* Vertex color tint: to_linear(128/255) * to_linear(200/255) -> sRGB */
    {
        int exp_r = srgb8(srgb_to_lin(128.0 / 255.0) * srgb_to_lin(200.0 / 255.0));
        int exp_g = srgb8(srgb_to_lin(100.0 / 255.0));
        int exp_b = srgb8(srgb_to_lin(128.0 / 255.0) * srgb_to_lin(50.0 / 255.0));
        CHECK(rgb_near(px(g_img, 170, 36), exp_r, exp_g, exp_b, 3));
    }

    /* sRGB vs UNORM */
    CHECK(rgb_near(px(g_img, 36, 120), 128, 128, 128, 2));
    {
        int exp_unorm = srgb8(128.0 / 255.0);
        CHECK(rgb_near(px(g_img, 90, 120), exp_unorm, exp_unorm, exp_unorm, 2));
    }

    /* Alpha blend over linear (0, 0, 1) background */
    {
        double a = 128.0 / 255.0;
        int exp_r = srgb8(a);
        int exp_b = srgb8(1.0 - a);
        CHECK(rgb_near(px(g_img, 150, 120), exp_r, 0, exp_b, 3));
    }

    /* Background outside any sprite remains pure blue */
    CHECK(rgb_near(px(g_img, 240, 240), 0, 0, 255, 2));

    /* Output alpha coverage preserved when HUD sprite is drawn over transparent premultiplied scene */
    su_hud_begin(&h, verts, 64, W, H);
    su_hud_sprite(&h, 16.0f, 16.0f, 48.0f, 48.0f, NULL, rgba_u32(255, 255, 255, 255));
    batches[0] = su_hud_batch(tex_quad, 0u, &h);
    CHECK(render_hud_frame(&h, batches, 1u, clear_blue, SU_RENDER_OUTPUT_ALPHA_PREMULTIPLIED, g_img));
    CHECK(px(g_img, 32, 32)[3] == 0u);

    CHECK(su_render_release(g_r, tex_quad) == SU_OK);
    CHECK(su_render_release(g_r, tex_srgb) == SU_OK);
    CHECK(su_render_release(g_r, tex_unorm) == SU_OK);
    CHECK(su_render_release(g_r, tex_tint) == SU_OK);
    CHECK(su_render_release(g_r, tex_alpha) == SU_OK);
    printf("hud textured quads: srgb/unorm, sub-uv, vertex tint, alpha blend and output_alpha ok\n");
}

static void test_nearest_vs_linear(void) {
    /* 2x1: left black, right white */
    static const uint8_t bw_px[2 * 4] = {0, 0, 0, 255, 255, 255, 255, 255};
    SuRenderHandle tex = make_tex_ex(SU_TEX_RGBA8_SRGB, 2, 1, 1, bw_px, 1);
    SuHudVertex verts[12];
    SuHud h;
    SuHudBatch bt[2];

    su_hud_begin(&h, verts, 12, W, H);
    /* Top band: nearest */
    bt[0] = su_hud_batch(tex, h.count, &h);
    su_hud_sprite(&h, 0.0f, 0.0f, 128.0f, 64.0f, NULL, rgba_u32(255, 255, 255, 255));
    bt[0].vertex_count = h.count - bt[0].first_vertex;
    bt[0].flags = SU_HUD_BATCH_NEAREST;

    /* Bottom band: linear (flags = 0) */
    bt[1] = su_hud_batch(tex, h.count, &h);
    su_hud_sprite(&h, 0.0f, 80.0f, 128.0f, 64.0f, NULL, rgba_u32(255, 255, 255, 255));
    bt[1].vertex_count = h.count - bt[1].first_vertex;

    CHECK(render_hud_frame(&h, bt, 2u, NULL, SU_RENDER_OUTPUT_ALPHA_OPAQUE, g_img));

    /* Nearest has crisp jump at x = 64 (texel centers are at 32 and 96) */
    CHECK(px(g_img, 62, 32)[0] < 5 && px(g_img, 66, 32)[0] > 250);
    /* Linear smoothly interpolates around the middle (x = 64 is halfway between texel centers 32 and 96) */
    CHECK(px(g_img, 63, 112)[0] > 50 && px(g_img, 63, 112)[0] < 230);
    CHECK(px(g_img, 64, 112)[0] > 50 && px(g_img, 64, 112)[0] < 230);

    CHECK(su_render_release(g_r, tex) == SU_OK);
    printf("hud sampling: nearest crisp edge vs linear gradient ok\n");
}

static void test_scissor(void) {
    static const uint8_t red_px[4] = {255, 0, 0, 255};
    SuRenderHandle tex_red = make_tex_ex(SU_TEX_RGBA8_SRGB, 1, 1, 1, red_px, 1);
    float clear_blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    SuHudVertex verts[48];
    SuHud h;
    SuHudBatch bt[5];
    SuRenderStats st;

    su_hud_begin(&h, verts, 48, W, H);
    /* batch 0: interior scissor [40..100, 50..90] */
    bt[0] = su_hud_batch(tex_red, h.count, &h);
    su_hud_sprite(&h, 0.0f, 0.0f, (float)W, (float)H, NULL, rgba_u32(255, 255, 255, 255));
    bt[0].vertex_count = h.count - bt[0].first_vertex;
    bt[0].flags = SU_HUD_BATCH_SCISSOR;
    bt[0].scissor_x = 40;
    bt[0].scissor_y = 50;
    bt[0].scissor_w = 60;
    bt[0].scissor_h = 40;

    /* batch 1: negative origin clamped to [0..30, 0..30] */
    bt[1] = su_hud_batch(tex_red, h.count, &h);
    su_hud_sprite(&h, 0.0f, 0.0f, (float)W, (float)H, NULL, rgba_u32(255, 255, 255, 255));
    bt[1].vertex_count = h.count - bt[1].first_vertex;
    bt[1].flags = SU_HUD_BATCH_SCISSOR;
    bt[1].scissor_x = -20;
    bt[1].scissor_y = -10;
    bt[1].scissor_w = 50;
    bt[1].scissor_h = 40;

    /* batch 2: oversized extents clamped to [W-20..W, H-15..H] */
    bt[2] = su_hud_batch(tex_red, h.count, &h);
    su_hud_sprite(&h, 0.0f, 0.0f, (float)W, (float)H, NULL, rgba_u32(255, 255, 255, 255));
    bt[2].vertex_count = h.count - bt[2].first_vertex;
    bt[2].flags = SU_HUD_BATCH_SCISSOR;
    bt[2].scissor_x = (int32_t)W - 20;
    bt[2].scissor_y = (int32_t)H - 15;
    bt[2].scissor_w = 500u;
    bt[2].scissor_h = 500u;

    /* batch 3: zero width -> culled cleanly */
    bt[3] = su_hud_batch(tex_red, h.count, &h);
    su_hud_sprite(&h, 110.0f, 110.0f, 40.0f, 40.0f, NULL, rgba_u32(255, 255, 255, 255));
    bt[3].vertex_count = h.count - bt[3].first_vertex;
    bt[3].flags = SU_HUD_BATCH_SCISSOR;
    bt[3].scissor_x = 110;
    bt[3].scissor_y = 110;
    bt[3].scissor_w = 0u;
    bt[3].scissor_h = 40u;

    /* batch 4: completely offscreen negative -> culled cleanly */
    bt[4] = su_hud_batch(tex_red, h.count, &h);
    su_hud_sprite(&h, 160.0f, 110.0f, 40.0f, 40.0f, NULL, rgba_u32(255, 255, 255, 255));
    bt[4].vertex_count = h.count - bt[4].first_vertex;
    bt[4].flags = SU_HUD_BATCH_SCISSOR;
    bt[4].scissor_x = -60;
    bt[4].scissor_y = 10;
    bt[4].scissor_w = 50u;
    bt[4].scissor_h = 50u;

    CHECK(render_hud_frame(&h, bt, 5u, clear_blue, SU_RENDER_OUTPUT_ALPHA_OPAQUE, g_img));
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    CHECK(st.hud_batches_drawn == 3u && st.hud_batches_skipped == 0u);

    /* Inside interior scissor */
    CHECK(rgb_near(px(g_img, 41, 51), 255, 0, 0, 2));
    CHECK(rgb_near(px(g_img, 98, 88), 255, 0, 0, 2));
    /* Just outside interior scissor */
    CHECK(rgb_near(px(g_img, 38, 60), 0, 0, 255, 2));
    CHECK(rgb_near(px(g_img, 101, 60), 0, 0, 255, 2));
    CHECK(rgb_near(px(g_img, 60, 48), 0, 0, 255, 2));
    CHECK(rgb_near(px(g_img, 60, 91), 0, 0, 255, 2));

    /* Negative origin clamped [0..29, 0..29] */
    CHECK(rgb_near(px(g_img, 0, 0), 255, 0, 0, 2));
    CHECK(rgb_near(px(g_img, 28, 28), 255, 0, 0, 2));
    CHECK(rgb_near(px(g_img, 31, 10), 0, 0, 255, 2));
    CHECK(rgb_near(px(g_img, 10, 31), 0, 0, 255, 2));

    /* Oversized extents clamped [W-20..W-1, H-15..H-1] */
    CHECK(rgb_near(px(g_img, W - 1u, H - 1u), 255, 0, 0, 2));
    CHECK(rgb_near(px(g_img, W - 19u, H - 14u), 255, 0, 0, 2));
    CHECK(rgb_near(px(g_img, W - 22u, H - 5u), 0, 0, 255, 2));
    CHECK(rgb_near(px(g_img, W - 5u, H - 17u), 0, 0, 255, 2));

    /* Culled zero-area / offscreen batches did not draw */
    CHECK(rgb_near(px(g_img, 125, 125), 0, 0, 255, 2));
    CHECK(rgb_near(px(g_img, 175, 125), 0, 0, 255, 2));

    CHECK(su_render_release(g_r, tex_red) == SU_OK);
    printf("hud scissor: interior clip, negative/oversized clamp and empty/offscreen cull ok\n");
}

static void test_mixed_and_order(void) {
    static const uint8_t green_px[4] = {0, 255, 0, 255};
    SuRenderHandle tex_green = make_tex_ex(SU_TEX_RGBA8_SRGB, 1, 1, 1, green_px, 1);
    SuHudVertex verts[256];
    SuHud h;
    SuHudBatch bt[3];
    SuRenderStats st;
    uint32_t text_lit = 0;
    uint32_t x;
    uint32_t y;

    su_hud_begin(&h, verts, 256, W, H);
    /* Batch 0 (built-in font/rect): red rectangle + white text */
    bt[0] = su_hud_batch(0u, h.count, &h);
    su_hud_rect(&h, 20.0f, 20.0f, 100.0f, 100.0f, rgba_u32(255, 0, 0, 255));
    su_hud_text(&h, 24.0f, 140.0f, 2.0f, rgba_u32(255, 255, 255, 255), "HI");
    bt[0].vertex_count = h.count - bt[0].first_vertex;

    /* Batch 1 (textured sprite): green over middle of red rect */
    bt[1] = su_hud_batch(tex_green, h.count, &h);
    su_hud_sprite(&h, 40.0f, 40.0f, 60.0f, 60.0f, NULL, rgba_u32(255, 255, 255, 255));
    bt[1].vertex_count = h.count - bt[1].first_vertex;

    /* Batch 2 (built-in rect): blue over center of green sprite */
    bt[2] = su_hud_batch(0u, h.count, &h);
    su_hud_rect(&h, 55.0f, 55.0f, 30.0f, 30.0f, rgba_u32(0, 0, 255, 255));
    bt[2].vertex_count = h.count - bt[2].first_vertex;

    CHECK(render_hud_frame(&h, bt, 3u, NULL, SU_RENDER_OUTPUT_ALPHA_OPAQUE, g_img));
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    CHECK(st.hud_batches_drawn == 3u && st.hud_batches_skipped == 0u);

    CHECK(rgb_near(px(g_img, 25, 25), 255, 0, 0, 2)); /* batch 0 red */
    CHECK(rgb_near(px(g_img, 45, 45), 0, 255, 0, 2)); /* batch 1 green */
    CHECK(rgb_near(px(g_img, 70, 70), 0, 0, 255, 2)); /* batch 2 blue */

    for (y = 140u; y < 156u; y++) {
        for (x = 24u; x < 50u; x++) {
            if (px(g_img, x, y)[0] > 200u) {
                text_lit++;
            }
        }
    }
    CHECK(text_lit > 20u);

    CHECK(su_render_release(g_r, tex_green) == SU_OK);
    printf("hud mixed batches: interleaved font/rects and textured sprites preserve draw order ok\n");
}

static void test_skips_and_compat(void) {
    static const uint8_t white_px[4] = {255, 255, 255, 255};
    static const uint8_t green_px[4] = {0, 255, 0, 255};
    SuRenderHandle tex_ready = make_tex_ex(SU_TEX_RGBA8_SRGB, 1, 1, 1, white_px, 1);
    SuRenderHandle tex_cube = make_tex_ex(SU_TEX_RGBA8_SRGB, 1, 1, 6, white_px, 1);
    SuRenderHandle tex_released = make_tex_ex(SU_TEX_RGBA8_SRGB, 1, 1, 1, green_px, 1);
    SuRenderHandle tex_pending;
    SuHudVertex verts[36];
    SuHud h;
    SuHudBatch bt[8];
    SuRenderStats st;
    SuRenderFrameDesc f;

    CHECK(su_render_release(g_r, tex_released) == SU_OK);

    su_hud_begin(&h, verts, 36, W, H);
    su_hud_sprite(&h, 10.0f, 10.0f, 30.0f, 30.0f, NULL, rgba_u32(255, 255, 255, 255));

    /* 0: invalid handle */
    bt[0] = su_hud_batch(0xDEADBEEFu, 0u, &h);
    /* 1: released handle */
    bt[1] = su_hud_batch(tex_released, 0u, &h);
    /* 2: cube texture (layers == 6, not 2D) */
    bt[2] = su_hud_batch(tex_cube, 0u, &h);
    /* 3: first_vertex out of range */
    bt[3] = su_hud_batch(tex_ready, 12u, &h);
    bt[3].vertex_count = 6u;
    /* 4: vertex_count overflow / past n_hud */
    bt[4] = su_hud_batch(tex_ready, 0u, &h);
    bt[4].vertex_count = 12u;
    /* 5: non-multiple of 3 */
    bt[5] = su_hud_batch(tex_ready, 0u, &h);
    bt[5].vertex_count = 4u;
    /* 6: empty batch (ignored, neither drawn nor skipped) */
    bt[6] = su_hud_batch(tex_ready, 0u, &h);
    bt[6].vertex_count = 0u;
    /* 7: valid batch */
    bt[7] = su_hud_batch(tex_ready, 0u, &h);

    CHECK(render_hud_frame(&h, bt, 8u, NULL, SU_RENDER_OUTPUT_ALPHA_OPAQUE, g_img));
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    CHECK(st.hud_batches_drawn == 1u && st.hud_batches_skipped == 6u);
    CHECK(rgb_near(px(g_img, 20, 20), 255, 255, 255, 2));

    /* Pending texture batch (2560 x 2048 = 20 MiB > 16 MiB budget) stays PENDING in frame 1 and is skipped */
    {
        size_t big_bytes = (size_t)2560u * 2048u * 4u;
        uint8_t *big_px = calloc(1, big_bytes);
        SuHudBatch pb;
        CHECK(big_px != NULL);
        tex_pending = make_tex_ex(SU_TEX_RGBA8_SRGB, 2560u, 2048u, 1u, big_px, 0);
        free(big_px);
        CHECK(su_render_resource_state(g_r, tex_pending) == SU_RENDER_STATE_PENDING);
        pb = su_hud_batch(tex_pending, 0u, &h);
        CHECK(render_hud_frame(&h, &pb, 1u, NULL, SU_RENDER_OUTPUT_ALPHA_OPAQUE, g_img));
        CHECK(su_render_resource_state(g_r, tex_pending) == SU_RENDER_STATE_PENDING);
        CHECK(su_render_stats(g_r, &st) == SU_OK);
        CHECK(st.hud_batches_drawn == 0u && st.hud_batches_skipped == 1u);
    }

    /* hud_batches == NULL with hud_batch_count > 0 returns SU_E_INVALID_ARGUMENT */
    base_frame_desc(&f, NULL);
    f.hud = h.v;
    f.hud_vertex_count = h.count;
    f.hud_batches = NULL;
    f.hud_batch_count = 1u;
    CHECK(su_render_frame_ex(g_r, &f) == SU_E_INVALID_ARGUMENT);

    /* struct_size == 0 returns SU_E_INVALID_ARGUMENT */
    f.struct_size = 0u;
    CHECK(su_render_frame_ex(g_r, &f) == SU_E_INVALID_ARGUMENT);

    /* Fallback path (hud_batch_count == 0) renders su_hud_rect identically */
    su_hud_begin(&h, verts, 36, W, H);
    su_hud_rect(&h, 10.0f, 10.0f, 40.0f, 40.0f, rgba_u32(255, 0, 0, 255));
    CHECK(render_hud_frame(&h, NULL, 0u, NULL, SU_RENDER_OUTPUT_ALPHA_OPAQUE, g_ref));
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    CHECK(st.hud_batches_drawn == 0u && st.hud_batches_skipped == 0u);
    CHECK(rgb_near(px(g_ref, 25, 25), 255, 0, 0, 2));

    /* V12 struct_size compatibility: ignores trailing hud_batches/hud_batch_count fields */
    base_frame_desc(&f, NULL);
    f.struct_size = SU_RENDER_FRAME_DESC_V12_SIZE;
    f.hud = h.v;
    f.hud_vertex_count = h.count;
    f.hud_batches = NULL;
    f.hud_batch_count = 99u; /* must be ignored when struct_size == V12 */
    CHECK(su_render_frame_ex(g_r, &f) == SU_OK);
    CHECK(su_render_readback(g_r, g_img, (size_t)W * H * 4u) == SU_OK);
    CHECK(memcmp(g_img, g_ref, (size_t)W * H * 4u) == 0);
    CHECK(su_render_stats(g_r, &st) == SU_OK);
    CHECK(st.hud_batches_drawn == 0u && st.hud_batches_skipped == 0u);

    CHECK(su_render_release(g_r, tex_ready) == SU_OK);
    CHECK(su_render_release(g_r, tex_cube) == SU_OK);
    CHECK(su_render_release(g_r, tex_pending) == SU_OK);
    printf("hud skips and compat: bad handles/ranges, invalid args, fallback and V12 struct_size ok\n");
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

    test_textured_quads();
    test_nearest_vs_linear();
    test_scissor();
    test_mixed_and_order();
    test_skips_and_compat();

    CHECK(su_render_stats(g_r, &st) == SU_OK);
    if (validation) {
        CHECK(getenv("SUTEKH_REQUIRE_VALIDATION") == NULL || st.validation_enabled != 0u);
        printf("validation %s: %u errors, %u warnings\n", st.validation_enabled ? "on" : "not available",
               (unsigned)st.validation_errors, (unsigned)st.validation_warnings);
        CHECK(st.validation_errors == 0u && st.validation_warnings == 0u);
    }
    su_render_destroy(g_r);
    printf("test_render_hud: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail != 0 ? 1 : 0;
}
