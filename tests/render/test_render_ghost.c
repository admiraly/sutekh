/* Translucent instances, offscreen with pixel readback: SuRenderObject.tint[3] < 1 draws any object, whatever its
 * material's alpha mode, in the blended pass (depth-tested, no depth write, back to front), with exact expected
 * pixels. Unlit white materials make the output the tint itself, so expected values are plain sRGB encodings.
 *
 * Usage: test_render_ghost WORKDIR   (exit 77 without a usable Vulkan device; SUTEKH_RENDER_VALIDATION=1 turns on
 * the validation layer, with VK_ADD_LAYER_PATH pointing at a build of it) */
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

#define W 128u
#define H 128u

static uint8_t g_px[W * H * 4];
static SuWorldPos g_cam;

static uint64_t ID(const char *n) {
    char b[128];
    snprintf(b, sizeof b, "ghost/%s", n);
    return su_asset_id(b);
}

/* Axis-aligned box -h..h, 24 vertices, counter-clockwise outside. */
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
        }
        idx[f * 6 + 0] = (uint32_t)f * 4u + 0u;
        idx[f * 6 + 1] = (uint32_t)f * 4u + 1u;
        idx[f * 6 + 2] = (uint32_t)f * 4u + 2u;
        idx[f * 6 + 3] = (uint32_t)f * 4u + 0u;
        idx[f * 6 + 4] = (uint32_t)f * 4u + 2u;
        idx[f * 6 + 5] = (uint32_t)f * 4u + 3u;
    }
}

static void add_model(SuAssetWriter *w, const char *name, uint32_t mode, float base_alpha) {
    SuAssetVertex v[24];
    uint32_t idx[36];
    SuAssetSubmesh sm;
    SuAssetMaterial m;
    SuAssetModelNode nd;
    char buf[64];
    box(v, idx, 0.5f);
    su_asset_material_default(&m);
    m.unlit = 1;
    m.alpha_mode = mode;
    m.alpha_cutoff = 0.5f;
    m.base_color[3] = base_alpha;
    snprintf(buf, sizeof buf, "mat/%s", name);
    CHECK(su_asset_writer_add_material(w, ID(buf), &m) == SU_OK);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 36;
    sm.material = ID(buf);
    snprintf(buf, sizeof buf, "mesh/%s", name);
    CHECK(su_asset_writer_add_mesh(w, ID(buf), v, 24, idx, 36, &sm, 1) == SU_OK);
    memset(&nd, 0, sizeof nd);
    nd.transform[0] = nd.transform[5] = nd.transform[10] = nd.transform[15] = 1.0f;
    nd.lod_mesh[0] = ID(buf);
    nd.lod_screen[0] = 1.0f;
    nd.lod_count = 1;
    snprintf(buf, sizeof buf, "model/%s", name);
    CHECK(su_asset_writer_add_model(w, ID(buf), &nd, 1) == SU_OK);
}

static void base_frame(SuRenderFrameDesc *f) {
    memset(f, 0, sizeof *f);
    f->struct_size = sizeof *f;
    f->camera.pos = g_cam;
    f->camera.rot.w = 1.0f;
    f->camera.fov_y = 1.0471976f;
    f->camera.near_z = 0.05f;
    f->light.sun_dir.z = -1.0f;
    f->light.tonemap = SU_TONEMAP_CLAMP;
    f->light.no_sky = 1;
}

static SuRenderObject obj(SuRenderHandle model, double z, float scale, float r, float g, float b, float a) {
    SuRenderObject o;
    SuVec3f d;
    memset(&o, 0, sizeof o);
    d.x = 0.0f;
    d.y = 0.0f;
    d.z = (float)z;
    o.pos = su_wpos_add(g_cam, d);
    o.rot.w = 1.0f;
    o.scale = scale;
    o.tint[0] = r;
    o.tint[1] = g;
    o.tint[2] = b;
    o.tint[3] = a;
    o.emissive = 1.0f;
    o.model = model;
    return o;
}

static const uint8_t *center(void) {
    return &g_px[((H / 2u) * W + W / 2u) * 4u];
}

static int is(const uint8_t *p, int r, int g, int b) {
    return abs(p[0] - r) <= 2 && abs(p[1] - g) <= 2 && abs(p[2] - b) <= 2;
}

static int srgb8(double l) {
    double s = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    return (int)floor(s * 255.0 + 0.5);
}

static int draw(SuRenderer *r, const SuRenderObject *o, uint32_t n) {
    SuRenderFrameDesc f;
    uint32_t st;
    base_frame(&f);
    f.objects = o;
    f.object_count = n;
    st = su_render_frame_ex(r, &f);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "frame: %s\n", su_render_last_error(r));
        return 0;
    }
    CHECK(su_render_readback(r, g_px, sizeof g_px) == SU_OK);
    return 1;
}

int main(int argc, char **argv) {
    SuRenderDesc d;
    SuRenderer *r = NULL;
    SuRenderStats st;
    SuAssetWriter *w = NULL;
    SuAssetPak *pak = NULL;
    SuRenderPak rp;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char msg[256];
    uint32_t status;
    uint32_t spin = 0;
    SuRenderHandle opaque;
    SuRenderHandle mask;
    SuRenderHandle blend;
    SuRenderObject o[4];
    int g50 = srgb8(0.5);
    int g25 = srgb8(0.25);
    (void)argv;
    if (argc != 2) {
        fprintf(stderr, "usage: test_render_ghost WORKDIR\n");
        return 2;
    }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.width = W;
    d.height = H;
    d.device_index = -1;
    d.max_draw_instances = 256;
    if (getenv("SUTEKH_RENDER_VALIDATION") != NULL) {
        d.flags |= SU_RENDER_FLAG_VALIDATION;
    }
    status = su_render_create(&d, &r, msg);
    if (status != SU_OK) {
        printf("renderer unavailable: %s\n", msg);
        return status == SU_E_UNSUPPORTED_METHOD ? 77 : 1;
    }
    g_cam = su_wpos(250000.0, -1200.0, 90000.0);

    CHECK(su_asset_writer_create(&w) == SU_OK);
    add_model(w, "opaque", SU_ASSET_ALPHA_OPAQUE, 1.0f);
    add_model(w, "mask", SU_ASSET_ALPHA_MASK, 1.0f);
    add_model(w, "blend", SU_ASSET_ALPHA_BLEND, 0.5f);
    CHECK(su_asset_writer_finish(w, &bytes, &len) == SU_OK);
    su_asset_writer_destroy(w);
    CHECK(su_asset_pak_open(bytes, len, 0, &pak, msg) == SU_OK);
    CHECK(su_render_upload_pak(r, pak, ID("model/opaque"), &rp) == SU_OK);
    while (su_render_pak_state(r, &rp) == SU_RENDER_STATE_PENDING && spin++ < 100u) {
        o[0] = obj(su_render_pak_handle(&rp, ID("model/opaque")), -3, 1.0f, 1, 1, 1, 1);
        (void)draw(r, o, 1);
    }
    CHECK(su_render_pak_state(r, &rp) == SU_RENDER_STATE_READY);
    opaque = su_render_pak_handle(&rp, ID("model/opaque"));
    mask = su_render_pak_handle(&rp, ID("model/mask"));
    blend = su_render_pak_handle(&rp, ID("model/blend"));
    printf("expected sRGB values: 0.5 -> %d, 0.25 -> %d\n", g50, g25);

    /* A. fully opaque tint: unchanged behaviour, exact tint colour */
    o[0] = obj(opaque, -3, 1.0f, 0, 1, 0, 1.0f);
    if (draw(r, o, 1)) {
        CHECK(is(center(), 0, 255, 0));
        (void)su_render_stats(r, &st);
        CHECK(st.objects_culled == 0u && st.draws >= 1u);
    }
    /* B. a ghost over the empty background: half the colour */
    o[0].tint[3] = 0.5f;
    if (draw(r, o, 1)) {
        CHECK(is(center(), 0, g50, 0));
        CHECK(is(&g_px[(4u * W + 4u) * 4u], 0, 0, 0)); /* the corners stay clear */
    }
    /* B2. the tint colour tints (lit factor channels), alpha 0.25 */
    o[0] = obj(opaque, -3, 1.0f, 1, 0.5f, 0, 0.25f);
    if (draw(r, o, 1)) {
        CHECK(is(center(), srgb8(0.25), srgb8(0.125), 0));
    }
    /* C. a ghost in front of an opaque backdrop: the two mix (0.5 red + 0.5 green) */
    o[0] = obj(opaque, -6, 6.0f, 1, 0, 0, 1.0f);
    o[1] = obj(opaque, -3, 1.0f, 0, 1, 0, 0.5f);
    if (draw(r, o, 2)) {
        CHECK(is(center(), g50, g50, 0));
    }
    /* C2. the same with the array order swapped (the ghost is still drawn after the opaque pass) */
    o[0] = obj(opaque, -3, 1.0f, 0, 1, 0, 0.5f);
    o[1] = obj(opaque, -6, 6.0f, 1, 0, 0, 1.0f);
    if (draw(r, o, 2)) {
        CHECK(is(center(), g50, g50, 0));
    }
    /* D. an opaque object in front hides the ghost behind it (depth test) */
    o[0] = obj(opaque, -4, 1.0f, 0, 1, 0, 0.5f);
    o[1] = obj(opaque, -2, 1.0f, 1, 0, 0, 1.0f);
    if (draw(r, o, 2)) {
        CHECK(is(center(), 255, 0, 0));
    }
    /* E. two ghosts, far green behind near red: far over black = 0.5 g, then near red 0.5 over it = (0.5, 0.25, 0);
     * the result must not depend on the submission order */
    {
        int k;
        for (k = 0; k < 2; k++) {
            SuRenderObject near_g = obj(opaque, -2.5, 0.5f, 1, 0, 0, 0.5f);
            SuRenderObject far_g = obj(opaque, -4, 1.0f, 0, 1, 0, 0.5f);
            o[0] = k == 0 ? near_g : far_g;
            o[1] = k == 0 ? far_g : near_g;
            if (draw(r, o, 2)) {
                CHECK(is(center(), g50, g25, 0));
            }
        }
    }
    /* E2. three ghosts in a shuffled order: blue (far, 0.5), green (mid), red (near): back to front */
    {
        SuRenderObject red = obj(opaque, -2.2, 0.3f, 1, 0, 0, 0.5f);
        SuRenderObject green = obj(opaque, -3, 0.6f, 0, 1, 0, 0.5f);
        SuRenderObject blue = obj(opaque, -4.5, 1.0f, 0, 0, 1, 0.5f);
        /* blue: b 0.5; green over: g .5, b .25; red over: r .5, g .25, b .125 */
        o[0] = green;
        o[1] = red;
        o[2] = blue;
        if (draw(r, o, 3)) {
            CHECK(is(center(), srgb8(0.5), srgb8(0.25), srgb8(0.125)));
        }
    }
    /* F. alpha 0 (and NaN) draws nothing and counts as culled; so does a negative alpha */
    o[0] = obj(opaque, -3, 1.0f, 0, 1, 0, 0.0f);
    if (draw(r, o, 1)) {
        CHECK(is(center(), 0, 0, 0));
        (void)su_render_stats(r, &st);
        CHECK(st.objects_culled == 1u && st.draw_instances == 0u);
    }
    o[0].tint[3] = NAN;
    if (draw(r, o, 1)) {
        CHECK(is(center(), 0, 0, 0));
    }
    o[0].tint[3] = -1.0f;
    if (draw(r, o, 1)) {
        CHECK(is(center(), 0, 0, 0));
    }
    /* G. a masked material's ghost: alpha 0.4 is under the 0.5 cutoff, yet it blends instead of being discarded */
    o[0] = obj(mask, -3, 1.0f, 0, 1, 0, 0.4f);
    if (draw(r, o, 1)) {
        CHECK(is(center(), 0, srgb8(0.4), 0));
    }
    /* G2. a masked material at alpha 1 is still a plain masked draw */
    o[0].tint[3] = 1.0f;
    if (draw(r, o, 1)) {
        CHECK(is(center(), 0, 255, 0));
    }
    /* H. a blend material (base alpha 0.5) with a 0.5 tint alpha: 0.25; with tint alpha 1: 0.5 */
    o[0] = obj(blend, -3, 1.0f, 1, 1, 1, 0.5f);
    if (draw(r, o, 1)) {
        CHECK(is(center(), g25, g25, g25));
    }
    o[0].tint[3] = 1.0f;
    if (draw(r, o, 1)) {
        CHECK(is(center(), g50, g50, g50));
    }
    /* I. mixing a natural blend object and a ghost: sorted together by distance (near ghost over far blend) */
    o[0] = obj(blend, -4, 1.0f, 0, 0, 1, 1.0f);       /* far: blue, alpha 0.5 */
    o[1] = obj(opaque, -2.5, 0.5f, 1, 0, 0, 0.5f);    /* near: red ghost */
    if (draw(r, o, 2)) {
        CHECK(is(center(), g50, 0, g25));
    }
    o[0] = obj(blend, -2.5, 0.5f, 1, 0, 0, 1.0f);     /* now the blend object is near */
    o[1] = obj(opaque, -4, 1.0f, 0, 0, 1, 0.5f);
    if (draw(r, o, 2)) {
        CHECK(is(center(), g50, 0, g25));
    }
    /* J. determinism: the same frame twice gives the same pixels */
    {
        static uint8_t first[W * H * 4];
        o[0] = obj(opaque, -2.5, 0.5f, 1, 0, 0, 0.5f);
        o[1] = obj(opaque, -4, 1.0f, 0, 1, 0, 0.5f);
        if (draw(r, o, 2)) {
            memcpy(first, g_px, sizeof first);
            if (draw(r, o, 2)) {
                CHECK(memcmp(first, g_px, sizeof first) == 0);
            }
        }
    }
    (void)su_render_stats(r, &st);
    if (st.validation_enabled != 0u) {
        CHECK(st.validation_errors == 0u && st.validation_warnings == 0u);
        printf("validation layer on: %u errors, %u warnings\n", (unsigned)st.validation_errors, (unsigned)st.validation_warnings);
    } else {
        printf("validation layer off\n");
    }
    su_render_pak_release(r, &rp);
    su_asset_pak_close(pak);
    free(bytes);
    su_render_destroy(r);
    printf("test_render_ghost: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
