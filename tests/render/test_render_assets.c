/* Asset rendering, offscreen, pixel-checked: a procedural pack (writer) uploaded through su_render_upload_pak with
 * a small budget so uploads span frames; camera-relative frames far from the world origin; emissive and unlit
 * materials with exact expected pixels; sRGB texture sampling; distance LOD; frustum culling of a debris field;
 * instanced batching; mirrored nodes with a winding-flipped mesh; ACES tonemapping; the HUD and additive effects;
 * release with deferred retirement and stale handles; the persistent pipeline cache (absent, loaded, rejected).
 *
 * Usage: test_render_assets WORKDIR   (exit 77 without a usable Vulkan device) */
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

static char g_dir[512];
static uint8_t g_px[W * H * 4];

static const char *path_of(const char *name) {
    static char buf[1024];
    snprintf(buf, sizeof buf, "%s/%s", g_dir, name);
    return buf;
}

/* ---------- procedural content ---------- */

/* Axis-aligned box -h..h, 24 vertices, counter-clockwise outside. flip: reversed winding (for mirrored nodes). */
static void box(SuAssetVertex *v, uint32_t *idx, float h, int flip) {
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int f;
    for (f = 0; f < 6; f++) {
        float u[3];
        float w[3];
        int c;
        /* u, w span the face with u x w = n */
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
            x->tangent[3] = flip ? -1.0f : 1.0f;
            x->uv0[0] = 0.5f + 0.5f * a;
            x->uv0[1] = 0.5f - 0.5f * b;
        }
        idx[f * 6 + 0] = (uint32_t)f * 4u + 0u;
        idx[f * 6 + 1] = (uint32_t)f * 4u + (flip ? 2u : 1u);
        idx[f * 6 + 2] = (uint32_t)f * 4u + (flip ? 1u : 2u);
        idx[f * 6 + 3] = (uint32_t)f * 4u + 0u;
        idx[f * 6 + 4] = (uint32_t)f * 4u + (flip ? 3u : 2u);
        idx[f * 6 + 5] = (uint32_t)f * 4u + (flip ? 2u : 3u);
    }
}

static uint64_t ID(const char *n) {
    char b[128];
    snprintf(b, sizeof b, "t/%s", n);
    return su_asset_id(b);
}

static void emit_mat(SuAssetMaterial *m, float r, float g, float b) {
    su_asset_material_default(m);
    m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.0f;
    m->metallic = 0.0f;
    m->emissive[0] = r;
    m->emissive[1] = g;
    m->emissive[2] = b;
}

static void node(SuAssetModelNode *n, uint64_t lod0, uint64_t lod1) {
    memset(n, 0, sizeof *n);
    n->transform[0] = n->transform[5] = n->transform[10] = n->transform[15] = 1.0f;
    n->lod_mesh[0] = lod0;
    n->lod_screen[0] = 1.0f;
    n->lod_count = 1;
    if (lod1 != 0u) {
        n->lod_mesh[1] = lod1;
        n->lod_screen[1] = 0.05f;
        n->lod_count = 2;
    }
}

static uint64_t g_tex_bytes;

static uint8_t *build_pack(size_t *len) {
    SuAssetWriter *w = NULL;
    SuAssetVertex v[24];
    SuAssetVertex vf[24];
    uint32_t idx[36];
    uint32_t idxf[36];
    SuAssetSubmesh sm;
    SuAssetMaterial m;
    SuAssetModelNode nd;
    uint8_t *levels[10];
    uint32_t i;
    uint32_t lv;
    uint8_t *out = NULL;
    static uint8_t checker[64 * 64 * 4];
    static uint8_t checker1[32 * 32 * 4];
    const uint8_t *cm[2] = {checker, checker1};
    box(v, idx, 0.5f, 0);
    box(vf, idxf, 0.5f, 1);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 36;
    (void)su_asset_writer_create(&w);
    /* a big texture (512 x 512 with 10 mips, about 1.4 MB): several frames at a 256 KiB budget */
    g_tex_bytes = 0;
    for (lv = 0; lv < 10u; lv++) {
        uint32_t s = 512u >> lv;
        levels[lv] = malloc((size_t)s * s * 4u);
        for (i = 0; i < s * s * 4u; i++) {
            levels[lv][i] = (uint8_t)(i * 31u + lv);
        }
        g_tex_bytes += (uint64_t)s * s * 4u;
    }
    CHECK(su_asset_writer_add_texture(w, ID("tex/big"), 512, 512, SU_TEX_RGBA8_SRGB, 0, 10,
                                      (const uint8_t *const *)levels) == SU_OK);
    for (lv = 0; lv < 10u; lv++) {
        free(levels[lv]);
    }
    /* a 2 x 2 checker of 32-pixel cells: sRGB (200, 40, 40) and (40, 40, 200) */
    for (i = 0; i < 64u * 64u; i++) {
        int cell = (int)(((i % 64u) / 32u + (i / 64u) / 32u) & 1u);
        checker[i * 4] = cell ? 40 : 200;
        checker[i * 4 + 1] = 40;
        checker[i * 4 + 2] = cell ? 200 : 40;
        checker[i * 4 + 3] = 255;
    }
    for (i = 0; i < 32u * 32u; i++) {
        checker1[i * 4] = 120;
        checker1[i * 4 + 1] = 40;
        checker1[i * 4 + 2] = 120;
        checker1[i * 4 + 3] = 255;
    }
    CHECK(su_asset_writer_add_texture(w, ID("tex/checker"), 64, 64, SU_TEX_RGBA8_SRGB, 0, 2, cm) == SU_OK);
    g_tex_bytes += sizeof checker + sizeof checker1;
    emit_mat(&m, 0.0f, 1.0f, 0.0f);
    CHECK(su_asset_writer_add_material(w, ID("mat/green"), &m) == SU_OK);
    emit_mat(&m, 1.0f, 0.0f, 0.0f);
    CHECK(su_asset_writer_add_material(w, ID("mat/red"), &m) == SU_OK);
    emit_mat(&m, 1.0f, 1.0f, 1.0f);
    CHECK(su_asset_writer_add_material(w, ID("mat/one"), &m) == SU_OK);
    su_asset_material_default(&m);
    m.unlit = 1;
    m.texture[SU_ASSET_SLOT_BASE_COLOR] = ID("tex/checker");
    m.tex_info[SU_ASSET_SLOT_BASE_COLOR] = SU_ASSET_TEX_INFO(0, SU_ASSET_WRAP_CLAMP, SU_ASSET_WRAP_CLAMP);
    CHECK(su_asset_writer_add_material(w, ID("mat/checker"), &m) == SU_OK);
    su_asset_material_default(&m);
    m.metallic = 0.0f;
    m.roughness = 0.5f;
    m.texture[SU_ASSET_SLOT_BASE_COLOR] = ID("tex/big");
    CHECK(su_asset_writer_add_material(w, ID("mat/big"), &m) == SU_OK);
    sm.material = ID("mat/green");
    CHECK(su_asset_writer_add_mesh(w, ID("mesh/green"), v, 24, idx, 36, &sm, 1) == SU_OK);
    sm.material = ID("mat/red");
    CHECK(su_asset_writer_add_mesh(w, ID("mesh/red"), v, 24, idx, 36, &sm, 1) == SU_OK);
    sm.material = ID("mat/one");
    CHECK(su_asset_writer_add_mesh(w, ID("mesh/one"), v, 24, idx, 36, &sm, 1) == SU_OK);
    sm.material = ID("mat/checker");
    CHECK(su_asset_writer_add_mesh(w, ID("mesh/checker"), v, 24, idx, 36, &sm, 1) == SU_OK);
    sm.material = ID("mat/big");
    CHECK(su_asset_writer_add_mesh(w, ID("mesh/big"), v, 24, idx, 36, &sm, 1) == SU_OK);
    sm.material = 0; /* the renderer default material */
    CHECK(su_asset_writer_add_mesh(w, ID("mesh/plain"), v, 24, idx, 36, &sm, 1) == SU_OK);
    node(&nd, ID("mesh/green"), ID("mesh/red"));
    CHECK(su_asset_writer_add_model(w, ID("model/lod"), &nd, 1) == SU_OK);
    node(&nd, ID("mesh/one"), 0);
    CHECK(su_asset_writer_add_model(w, ID("model/one"), &nd, 1) == SU_OK);
    node(&nd, ID("mesh/checker"), 0);
    CHECK(su_asset_writer_add_model(w, ID("model/checker"), &nd, 1) == SU_OK);
    node(&nd, ID("mesh/big"), 0);
    CHECK(su_asset_writer_add_model(w, ID("model/big"), &nd, 1) == SU_OK);
    node(&nd, ID("mesh/plain"), 0);
    CHECK(su_asset_writer_add_model(w, ID("model/plain"), &nd, 1) == SU_OK);
    /* mirrored (lit, default material): x scale -1 with the winding-flipped mesh shows its outside; with the
     * unflipped mesh the outside is culled and the inside (normals facing away from the light) shows */
    CHECK(su_asset_writer_add_mesh(w, ID("mesh/plain_flipped"), vf, 24, idxf, 36, &sm, 1) == SU_OK);
    node(&nd, ID("mesh/plain_flipped"), 0);
    nd.transform[0] = -1.0f;
    CHECK(su_asset_writer_add_model(w, ID("model/mirror_ok"), &nd, 1) == SU_OK);
    node(&nd, ID("mesh/plain"), 0);
    nd.transform[0] = -1.0f;
    CHECK(su_asset_writer_add_model(w, ID("model/mirror_bad"), &nd, 1) == SU_OK);
    CHECK(su_asset_writer_finish(w, &out, len) == SU_OK);
    su_asset_writer_destroy(w);
    return out;
}

/* ---------- frames ---------- */

static SuWorldPos g_cam;

static void base_frame(SuRenderFrameDesc *f) {
    memset(f, 0, sizeof *f);
    f->struct_size = sizeof *f;
    f->camera.pos = g_cam;
    f->camera.rot.w = 1.0f;
    f->camera.fov_y = FOV;
    f->camera.near_z = 0.05f;
    f->light.sun_dir.z = -1.0f;
    f->light.tonemap = SU_TONEMAP_CLAMP;
    f->light.no_sky = 1;
}

static SuRenderObject obj_at(SuRenderHandle model, double dx, double dy, double dz, float scale) {
    SuRenderObject o;
    SuVec3f d;
    memset(&o, 0, sizeof o);
    d.x = (float)dx;
    d.y = (float)dy;
    d.z = (float)dz;
    o.pos = su_wpos_add(g_cam, d);
    o.rot.w = 1.0f;
    o.scale = scale;
    o.tint[0] = o.tint[1] = o.tint[2] = o.tint[3] = 1.0f;
    o.emissive = 1.0f;
    o.model = model;
    return o;
}

static int draw(SuRenderer *r, const SuRenderFrameDesc *f) {
    uint32_t st = su_render_frame_ex(r, f);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "frame: %s\n", su_render_last_error(r));
        return 0;
    }
    CHECK(su_render_readback(r, g_px, sizeof g_px) == SU_OK);
    return 1;
}

static const uint8_t *px(uint32_t x, uint32_t y) {
    return &g_px[(y * W + x) * 4u];
}

static int is(const uint8_t *p, int r, int g, int b, int tol) {
    return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol;
}

/* Screen pixel of a camera-relative point (identity camera rotation). */
static void project(double x, double y, double z, int *sx, int *sy) {
    double f = 1.0 / tan(FOV * 0.5);
    *sx = (int)floor((f * x / -z + 1.0) * 0.5 * W);
    *sy = (int)floor((-f * y / -z + 1.0) * 0.5 * H);
}

static double srgb(double l) {
    return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}

static SuRenderer *make(uint32_t *status, uint32_t budget) {
    SuRenderDesc d;
    SuRenderer *r = NULL;
    char msg[256];
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.width = W;
    d.height = H;
    d.device_index = -1;
    d.pipeline_cache_path = path_of("pipelines.cache");
    d.upload_budget_bytes = budget;
    d.max_draw_instances = 8192;
    if (getenv("SUTEKH_RENDER_VALIDATION") != NULL) {
        d.flags |= SU_RENDER_FLAG_VALIDATION;
    }
    *status = su_render_create(&d, &r, msg);
    if (*status != SU_OK) {
        printf("renderer unavailable: %s\n", msg);
    }
    return r;
}

int main(int argc, char **argv) {
    SuRenderer *r;
    SuRenderStats st;
    uint32_t status;
    size_t len = 0;
    uint8_t *bytes;
    SuAssetPak *pak = NULL;
    SuRenderPak rp;
    SuRenderFrameDesc f;
    SuRenderObject objs[2048];
    uint32_t frames_to_ready = 0;
    int i;
    char msg[256];
    if (argc != 2) {
        fprintf(stderr, "usage: test_render_assets WORKDIR\n");
        return 2;
    }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    (void)remove(path_of("pipelines.cache"));
    r = make(&status, 256u << 10);
    if (r == NULL) {
        return status == SU_E_UNSUPPORTED_METHOD ? 77 : 1;
    }
    {
        SuRenderDeviceInfo info;
        (void)su_render_device(r, &info);
        printf("rendering on: %s\n", info.name);
    }
    (void)su_render_stats(r, &st);
    CHECK(st.pipeline_cache == 3u); /* no file yet */
    printf("pipelines created in %.2f ms (cold cache)\n", st.pipeline_create_ms);
    /* far from the origin: 1000 km on x, 3 km up, -7 km on z (float32 spacing there is 6 cm) */
    g_cam = su_wpos(1000000.0, 3000.0, -7000.0);

    bytes = build_pack(&len);
    CHECK(su_asset_pak_open(bytes, len, 0, &pak, msg) == SU_OK);
    CHECK(su_render_upload_pak(r, pak, ID("model/lod"), &rp) == SU_OK);
    CHECK(rp.textures == 2u && rp.materials == 5u && rp.meshes == 7u && rp.models == 7u && rp.model == su_render_pak_handle(&rp, ID("model/lod")));
    CHECK(su_render_pak_state(r, &rp) == SU_RENDER_STATE_PENDING);

    /* uploads span frames at 256 KiB per frame; nothing is drawn before its mesh is READY */
    base_frame(&f);
    objs[0] = obj_at(su_render_pak_handle(&rp, ID("model/big")), 0, 0, -3, 1.0f);
    f.objects = objs;
    f.object_count = 1;
    while (su_render_pak_state(r, &rp) == SU_RENDER_STATE_PENDING && frames_to_ready < 100u) {
        CHECK(su_render_frame_ex(r, &f) == SU_OK);
        (void)su_render_stats(r, &st);
        CHECK(st.upload_bytes_frame <= (256u << 10));
        if (frames_to_ready == 0u) {
            CHECK(st.nodes_pending == 1u && st.nodes_drawn == 0u); /* the mesh comes after the textures */
        }
        frames_to_ready++;
    }
    (void)su_render_stats(r, &st);
    printf("pack uploaded in %u frames, %llu bytes (textures %llu)\n", (unsigned)frames_to_ready,
           (unsigned long long)st.upload_bytes_total, (unsigned long long)g_tex_bytes);
    CHECK(su_render_pak_state(r, &rp) == SU_RENDER_STATE_READY);
    CHECK(frames_to_ready >= 6u && st.upload_bytes_total >= g_tex_bytes && st.uploads_pending == 0u);

    /* emissive green cube 3 m ahead: exact (0, 255, 0) at the centre, black corners (no sky, no ambient) */
    base_frame(&f);
    objs[0] = obj_at(rp.model, 0, 0, -3, 1.0f);
    f.objects = objs;
    f.object_count = 1;
    if (draw(r, &f)) {
        CHECK(is(px(W / 2, H / 2), 0, 255, 0, 0));
        CHECK(is(px(2, 2), 0, 0, 0, 0));
        (void)su_render_stats(r, &st);
        CHECK(st.lod_nodes[0] == 1u && st.nodes_drawn == 1u);
    }
    /* placement far from the origin: a small cube 2 m right and 1 m up at 10 m lands where doubles say (scale 0.4
     * keeps LOD 0: fraction 0.35 * 1.73 / 10 = 0.06 > 0.05) */
    objs[0] = obj_at(rp.model, 2.0, 1.0, -10.0, 0.4f);
    if (draw(r, &f)) {
        int sx;
        int sy;
        project(2.0, 1.0, -10.0, &sx, &sy);
        CHECK(is(px((uint32_t)sx, (uint32_t)sy), 0, 255, 0, 0));
        if (getenv("SUTEKH_TEST_DEBUG") != NULL) {
            uint32_t x, y, n = 0, sxs = 0, sys = 0;
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) if (px(x, y)[1] > 128) { n++; sxs += x; sys += y; }
            printf("debug: expected %d,%d; green pixels %u centred at %u,%u\n", sx, sy, n, n ? sxs / n : 0, n ? sys / n : 0);
        }
        CHECK(is(px((uint32_t)sx + 12u, (uint32_t)sy), 0, 0, 0, 0)); /* 0.2 m half size = about 4.4 px at 10 m */
    }
    /* LOD: the same model farther away uses LOD 1 (red): projected fraction 1.73 * 1.73 / 100 < 0.05 */
    objs[0] = obj_at(rp.model, 0, 0, -100, 2.0f);
    if (draw(r, &f)) {
        (void)su_render_stats(r, &st);
        CHECK(st.lod_nodes[1] == 1u && st.lod_nodes[0] == 0u);
        CHECK(is(px(W / 2, H / 2), 255, 0, 0, 0));
    }
    /* unlit sRGB texture: cells keep their bytes through sampling, linear HDR and the sRGB output */
    objs[0] = obj_at(su_render_pak_handle(&rp, ID("model/checker")), 0, 0, -1.1, 1.0f);
    if (draw(r, &f)) {
        CHECK(is(px(W / 2 - 40u, H / 2 - 40u), 200, 40, 40, 2)); /* top-left cell */
        CHECK(is(px(W / 2 + 40u, H / 2 - 40u), 40, 40, 200, 2)); /* top-right cell */
    }
    /* a BC1 sRGB texture made by the cook-time encoder, created directly through the resource API, unlit:
     * (206, 40, 41) is exactly representable in 5:6:5, so it samples back to the same bytes */
    {
        uint8_t solid[8 * 8 * 4];
        uint8_t bc0[32];
        uint8_t bc1[8];
        uint8_t bc2[8];
        const uint8_t *mips[4];
        SuRenderTextureDesc td;
        SuRenderMaterialDesc md;
        SuRenderMeshDesc mdsc;
        SuRenderModelNode mn;
        SuAssetVertex bv[24];
        uint32_t bi[36];
        SuAssetSubmesh bs;
        SuRenderHandle tex = 0;
        SuRenderHandle mat = 0;
        SuRenderHandle mesh = 0;
        SuRenderHandle model = 0;
        int k;
        for (k = 0; k < 64; k++) {
            solid[k * 4] = 206;
            solid[k * 4 + 1] = 40;
            solid[k * 4 + 2] = 41;
            solid[k * 4 + 3] = 255;
        }
        CHECK(su_asset_bc_encode(SU_TEX_BC1_SRGB, solid, 8, 8, bc0) == SU_OK);
        CHECK(su_asset_bc_encode(SU_TEX_BC1_SRGB, solid, 4, 4, bc1) == SU_OK);
        CHECK(su_asset_bc_encode(SU_TEX_BC1_SRGB, solid, 2, 2, bc2) == SU_OK);
        mips[0] = bc0;
        mips[1] = bc1;
        mips[2] = bc2;
        mips[3] = bc2; /* 1 x 1: the same single block */
        memset(&td, 0, sizeof td);
        td.width = td.height = 8;
        td.format = SU_TEX_BC1_SRGB;
        td.mip_count = 4;
        td.mip_data = mips;
        td.flags = SU_RENDER_UPLOAD_COPY;
        CHECK(su_render_texture_create(r, &td, &tex) == SU_OK);
        memset(&md, 0, sizeof md);
        su_asset_material_default(&md.factors);
        md.factors.unlit = 1;
        md.textures[SU_ASSET_SLOT_BASE_COLOR] = tex;
        CHECK(su_render_material_create(r, &md, &mat) == SU_OK);
        box(bv, bi, 0.5f, 0);
        memset(&bs, 0, sizeof bs);
        bs.index_count = 36;
        bs.radius = 0.87f;
        memset(&mdsc, 0, sizeof mdsc);
        mdsc.vertices = bv;
        mdsc.vertex_count = 24;
        mdsc.indices = bi;
        mdsc.index_count = 36;
        mdsc.submeshes = &bs;
        mdsc.submesh_count = 1;
        mdsc.materials = &mat;
        mdsc.radius = 0.87f;
        mdsc.flags = SU_RENDER_UPLOAD_COPY;
        CHECK(su_render_mesh_create(r, &mdsc, &mesh) == SU_OK);
        memset(&mn, 0, sizeof mn);
        mn.transform[0] = mn.transform[5] = mn.transform[10] = mn.transform[15] = 1.0f;
        mn.lod_mesh[0] = mesh;
        mn.lod_screen[0] = 1.0f;
        mn.lod_count = 1;
        CHECK(su_render_model_create(r, &mn, 1, &model) == SU_OK);
        objs[0] = obj_at(model, 0, 0, -3, 1.0f);
        f.light.tonemap = SU_TONEMAP_CLAMP;
        for (k = 0; k < 2; k++) { /* the first frame uploads, the second draws with everything READY */
            CHECK(su_render_frame_ex(r, &f) == SU_OK);
        }
        if (su_render_resource_state(r, tex) == SU_RENDER_STATE_READY) {
            if (draw(r, &f)) {
                CHECK(is(px(W / 2, H / 2), 206, 40, 41, 1));
            }
        } else {
            printf("BC1 not sampleable on this device: texture state %u (%s); pixel check not run\n",
                   (unsigned)su_render_resource_state(r, tex), su_render_last_error(r));
            CHECK(su_render_resource_state(r, tex) == SU_RENDER_STATE_FAILED);
        }
        CHECK(su_render_release(r, model) == SU_OK && su_render_release(r, mesh) == SU_OK &&
              su_render_release(r, mat) == SU_OK && su_render_release(r, tex) == SU_OK);
    }
    /* ACES: an emission of 1.0 maps to (2.51 + 0.03) / (2.43 + 0.59 + 0.14) = 0.8038, sRGB 231 */
    objs[0] = obj_at(su_render_pak_handle(&rp, ID("model/one")), 0, 0, -3, 1.0f);
    f.light.tonemap = SU_TONEMAP_ACES;
    if (draw(r, &f)) {
        int want = (int)lround(srgb(2.54 / 3.16) * 255.0);
        CHECK(is(px(W / 2, H / 2), want, want, want, 1));
    }
    /* lit default material: brighter facing the sun than turned away, and the sky shows when enabled */
    base_frame(&f);
    f.light.tonemap = SU_TONEMAP_ACES;
    f.light.sun_dir.x = 0.0f;
    f.light.sun_dir.y = 0.0f;
    f.light.sun_dir.z = -1.0f; /* travels away from the camera: lights the faces the camera sees */
    f.light.sun_color.x = f.light.sun_color.y = f.light.sun_color.z = 1.0f;
    f.light.sun_intensity = 3.0f;
    f.light.ambient = 0.0f;
    f.objects = objs;
    f.object_count = 1;
    objs[0] = obj_at(su_render_pak_handle(&rp, ID("model/plain")), 0, 0, -3, 1.0f);
    if (draw(r, &f)) {
        uint8_t lit = px(W / 2, H / 2)[0];
        f.light.sun_dir.z = 1.0f; /* from behind the cube */
        if (draw(r, &f)) {
            CHECK(lit > 100u && px(W / 2, H / 2)[0] < 10u);
            if (getenv("SUTEKH_TEST_DEBUG") != NULL) {
                printf("debug: lit %u, back %u\n", lit, px(W / 2, H / 2)[0]);
            }
        }
        f.light.sun_dir.z = -1.0f;
        objs[0] = obj_at(su_render_pak_handle(&rp, ID("model/mirror_ok")), 0, 0, -3, 1.0f);
        if (draw(r, &f)) {
            CHECK(px(W / 2, H / 2)[0] == lit); /* same lit front face as the unmirrored cube */
        }
        objs[0] = obj_at(su_render_pak_handle(&rp, ID("model/mirror_bad")), 0, 0, -3, 1.0f);
        if (draw(r, &f)) {
            CHECK(px(W / 2, H / 2)[0] < 10u); /* the outside is culled; the inside faces away from the light */
        }
    }
    f.light.no_sky = 0;
    f.light.sky_zenith.z = 0.6f;
    f.light.sky_zenith.y = 0.2f;
    f.light.sky_horizon.x = f.light.sky_horizon.y = f.light.sky_horizon.z = 0.05f;
    f.light.up.y = 1.0f;
    f.object_count = 0;
    f.camera.rot = su_quatf_axis_angle((SuVec3f){1.0f, 0.0f, 0.0f}, 1.5707963f); /* look up */
    if (draw(r, &f)) {
        int k;
        int sky = 0;
        for (k = 0; k < 5; k++) { /* a star may land on one of them */
            const uint8_t *p = px(W / 2 + 7u * (uint32_t)k, H / 2);
            sky += p[2] > 120u && p[2] > p[0] + 60u;
        }
        CHECK(sky >= 4);
    }

    /* debris field: 2000 cubes in a ring around the camera; about one sixth in the 60 degree frustum */
    base_frame(&f);
    for (i = 0; i < 2000; i++) {
        double a = (double)i * 6.283185307179586 / 2000.0;
        objs[i] = obj_at(rp.model, 40.0 * sin(a), 0.5 * sin(a * 7.0), -40.0 * cos(a), 0.3f);
    }
    f.objects = objs;
    f.object_count = 2000;
    if (draw(r, &f)) {
        (void)su_render_stats(r, &st);
        printf("debris: %u objects, %u culled, %u drawn, %u draw instances, %u draws, scene cpu %.3f ms\n", st.objects,
               st.objects_culled, st.nodes_drawn, st.draw_instances, st.draws, st.scene_cpu_ms);
        CHECK(st.objects == 2000u && st.objects_culled + st.nodes_drawn == 2000u);
        CHECK(st.nodes_drawn > 250u && st.nodes_drawn < 450u); /* 60 degrees of 360, plus partial spheres */
        CHECK(st.lod_nodes[1] == st.nodes_drawn);               /* small at 40 m: all at LOD 1 */
        CHECK(st.draws == 2u);                                  /* one instanced draw + the tonemap draw */
    }

    /* HUD and effects: a white rectangle, text, and an additive billboard on black */
    {
        SuHudVertex hv[2048];
        SuHud hud;
        SuFxVertex fv[64];
        SuFxBuffer fx;
        float c[4] = {2.0f, 1.0f, 0.25f, 1.0f};
        SuVec3f at = {0.0f, 0.0f, -5.0f};
        su_hud_begin(&hud, hv, 2048, W, H);
        su_hud_rect(&hud, 10, 10, 20, 10, 0xFFFFFFFFu);
        (void)su_hud_text(&hud, 10, 40, 2.0f, 0xFF00FFFFu, "LAP 2/3");
        memset(&fx, 0, sizeof fx);
        fx.v = fv;
        fx.cap = 64;
        base_frame(&f);
        su_fx_billboard(&fx, at, 1.0f, 0.0f, c, su_render_camera_view(&f.camera));
        f.hud = hud.v;
        f.hud_vertex_count = hud.count;
        f.fx = fx.v;
        f.fx_vertex_count = fx.count;
        if (draw(r, &f)) {
            int lit = 0;
            int x;
            int y;
            CHECK(is(px(15, 15), 255, 255, 255, 0));
            CHECK(is(px(5, 5), 0, 0, 0, 0));
            for (y = 40; y < 56; y++) {
                for (x = 10; x < 94; x++) {
                    lit += is(px((uint32_t)x, (uint32_t)y), 255, 255, 0, 0);
                }
            }
            CHECK(lit > 60); /* yellow text pixels */
            CHECK(px(W / 2, H / 2)[0] > 200u && px(W / 2, H / 2)[2] < px(W / 2, H / 2)[0]); /* hot centre */
            CHECK(is(px(W / 2 + 60u, H / 2), 0, 0, 0, 0));                                  /* soft edge ends */
        }
    }

    /* release: handles die at once, GPU objects after their frames */
    {
        SuRenderHandle m = rp.model;
        su_render_pak_release(r, &rp);
        CHECK(su_render_resource_state(r, m) == SU_RENDER_STATE_INVALID);
        CHECK(su_render_release(r, m) == SU_E_STALE_HANDLE);
        base_frame(&f);
        objs[0] = obj_at(m, 0, 0, -3, 1.0f); /* a stale handle in a frame is skipped, not dereferenced */
        f.objects = objs;
        f.object_count = 1;
        for (i = 0; i < 3; i++) {
            CHECK(su_render_frame_ex(r, &f) == SU_OK);
        }
        (void)su_render_stats(r, &st);
        CHECK(st.resources == 0u && st.retired_pending == 0u);
    }
    su_asset_pak_close(pak);
    free(bytes);
    (void)su_render_stats(r, &st);
    CHECK(st.validation_errors == 0u);
    printf("validation %s (%u errors, %u warnings)\n", st.validation_enabled ? "on" : "not available",
           st.validation_errors, st.validation_warnings);
    su_render_destroy(r); /* saves the pipeline cache */

    /* the cache: loaded on the next start, rejected when corrupt */
    r = make(&status, 0);
    CHECK(r != NULL);
    if (r != NULL) {
        (void)su_render_stats(r, &st);
        CHECK(st.pipeline_cache == 1u);
        printf("pipelines created in %.2f ms (warm cache)\n", st.pipeline_create_ms);
        su_render_destroy(r);
    }
    {
        FILE *fh = fopen(path_of("pipelines.cache"), "r+b");
        CHECK(fh != NULL);
        if (fh != NULL) {
            int c;
            (void)fseek(fh, 100, SEEK_SET);
            c = fgetc(fh);
            (void)fseek(fh, 100, SEEK_SET);
            (void)fputc(c ^ 0x55, fh);
            fclose(fh);
        }
        r = make(&status, 0);
        CHECK(r != NULL);
        if (r != NULL) {
            (void)su_render_stats(r, &st);
            CHECK(st.pipeline_cache == 2u);
            su_render_destroy(r);
        }
    }
    printf("test_render_assets: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
