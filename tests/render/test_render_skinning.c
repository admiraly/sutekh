/* Skinned meshes and per-frame joint matrix palettes (sutekh/render.h, sutekh/asset.h), offscreen and pixel-checked:
 *   - invalid skinned mesh descriptors and out-of-bounds skin_offset/skin_count are refused cleanly;
 *   - skin_count == 0 renders at the bind pose;
 *   - single-joint translation, two-joint linear blend (0.5 / 0.5), and dynamic SSBO growth (> 4096 matrices)
 *     deform vertices as specified;
 *   - joint rotation transforms vertex normals (lit vs unlit) and works with layered paint materials;
 *   - skinned shadow casters cast sun shadows from their deformed pose;
 *   - cooking/packing a skinned mesh through su_asset_writer_add_skinned_mesh and su_render_upload_pak renders cleanly
 *     with zero Vulkan validation errors or warnings.
 *
 * Usage: test_render_skinning [--no-validation]   (exit 77 without a usable Vulkan device) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <float.h>
#include <stddef.h>
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

static SuMat4f mat_translate(float x, float y, float z) {
    SuMat4f m = su_mat4f_identity();
    m.m[12] = x;
    m.m[13] = y;
    m.m[14] = z;
    return m;
}

static SuMat4f mat_rotate_y(float rad) {
    SuMat4f m = su_mat4f_identity();
    float c = cosf(rad);
    float s = sinf(rad);
    m.m[0] = c;
    m.m[2] = -s;
    m.m[8] = s;
    m.m[10] = c;
    return m;
}

static void make_quad(float half, SuAssetVertex *v, uint32_t *idx) {
    const float xy[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    int i;
    for (i = 0; i < 4; i++) {
        memset(&v[i], 0, sizeof v[i]);
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].normal[2] = 1.0f;
        v[i].tangent[0] = 1.0f;
        v[i].tangent[3] = 1.0f;
        v[i].uv0[0] = (xy[i][0] / half + 1.0f) * 0.5f;
        v[i].uv0[1] = (1.0f - xy[i][1] / half) * 0.5f;
        v[i].uv1[0] = v[i].uv0[0];
        v[i].uv1[1] = v[i].uv0[1];
    }
    idx[0] = 0;
    idx[1] = 1;
    idx[2] = 2;
    idx[3] = 0;
    idx[4] = 2;
    idx[5] = 3;
}

static SuRenderHandle make_skinned_model(float half, float radius, SuRenderHandle mat, const SuAssetSkinVertex *skin,
                                         uint32_t joint_count) {
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuRenderSkinnedMeshDesc sd;
    SuRenderModelNode mn;
    SuRenderHandle msh = 0;
    SuRenderHandle model = 0;
    make_quad(half, v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6;
    sm.radius = radius;
    memset(&sd, 0, sizeof sd);
    sd.vertices = v;
    sd.vertex_count = 4;
    sd.indices = idx;
    sd.index_count = 6;
    sd.submeshes = &sm;
    sd.submesh_count = 1;
    sd.materials = &mat;
    sd.skin = skin;
    sd.joint_count = joint_count;
    sd.radius = radius;
    sd.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_skinned_mesh_create(g_r, &sd, &msh) == SU_OK);
    memset(&mn, 0, sizeof mn);
    mn.transform[0] = mn.transform[5] = mn.transform[10] = mn.transform[15] = 1.0f;
    mn.lod_mesh[0] = msh;
    mn.lod_screen[0] = 1.0f;
    mn.lod_count = 1;
    CHECK(su_render_model_create(g_r, &mn, 1, &model) == SU_OK);
    return model;
}

static SuRenderHandle make_unskinned_model(float half, float radius, SuRenderHandle mat) {
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuRenderMeshDesc md;
    SuRenderModelNode mn;
    SuRenderHandle msh = 0;
    SuRenderHandle model = 0;
    make_quad(half, v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6;
    sm.radius = radius;
    memset(&md, 0, sizeof md);
    md.vertices = v;
    md.vertex_count = 4;
    md.indices = idx;
    md.index_count = 6;
    md.submeshes = &sm;
    md.submesh_count = 1;
    md.materials = &mat;
    md.radius = radius;
    md.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_mesh_create(g_r, &md, &msh) == SU_OK);
    memset(&mn, 0, sizeof mn);
    mn.transform[0] = mn.transform[5] = mn.transform[10] = mn.transform[15] = 1.0f;
    mn.lod_mesh[0] = msh;
    mn.lod_screen[0] = 1.0f;
    mn.lod_count = 1;
    CHECK(su_render_model_create(g_r, &mn, 1, &model) == SU_OK);
    return model;
}

static void init_frame(SuRenderFrameDesc *f, const SuRenderObject *objs, uint32_t nobj, const SuMat4f *mats,
                       uint32_t nmats) {
    memset(f, 0, sizeof *f);
    f->struct_size = sizeof *f;
    f->camera.rot.w = 1.0f;
    f->camera.fov_y = FOV;
    f->camera.near_z = 0.05f;
    f->light.sun_dir = v3(0.0f, 0.0f, -1.0f);
    f->light.sun_intensity = 3.0f;
    f->light.sun_color = v3(1.0f, 1.0f, 1.0f);
    f->light.tonemap = SU_TONEMAP_CLAMP;
    f->light.no_sky = 1;
    f->light.exposure = 1.0f;
    f->light.clear_color[3] = 1.0f;
    f->objects = objs;
    f->object_count = nobj;
    f->skin_matrices = mats;
    f->skin_matrix_count = nmats;
}

static void init_obj(SuRenderObject *o, SuRenderHandle model, float x, float y, float z, uint32_t skin_offset,
                     uint32_t skin_count) {
    memset(o, 0, sizeof *o);
    o->pos = su_wpos(x, y, z);
    o->rot.w = 1.0f;
    o->scale = 1.0f;
    o->tint[0] = o->tint[1] = o->tint[2] = o->tint[3] = 1.0f;
    o->model = model;
    o->skin_offset = skin_offset;
    o->skin_count = skin_count;
}

static int render_frames(const SuRenderFrameDesc *f, uint8_t *out) {
    int k;
    for (k = 0; k < 3; k++) {
        uint32_t st = su_render_frame_ex(g_r, f);
        CHECK(st == SU_OK);
        if (st != SU_OK) {
            fprintf(stderr, "frame failed: %s\n", su_render_last_error(g_r));
            return 0;
        }
    }
    CHECK(su_render_readback(g_r, out, (size_t)W * H * 4u) == SU_OK);
    return 1;
}

static void test_skin_validation_and_bounds(void) {
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuAssetSkinVertex sk[4];
    SuRenderSkinnedMeshDesc sd;
    SuRenderHandle msh = 0;
    SuRenderHandle model = 0;
    SuRenderObject o;
    SuRenderFrameDesc f;
    SuMat4f mats[2];
    int i;
    make_quad(0.25f, v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6;
    sm.radius = 1.0f;
    memset(sk, 0, sizeof sk);
    for (i = 0; i < 4; i++) {
        sk[i].weights[0] = 1.0f;
    }
    memset(&sd, 0, sizeof sd);
    sd.vertices = v;
    sd.vertex_count = 4;
    sd.indices = idx;
    sd.index_count = 6;
    sd.submeshes = &sm;
    sd.submesh_count = 1;
    sd.skin = sk;
    sd.joint_count = 2;
    sd.radius = 1.0f;
    sd.flags = SU_RENDER_UPLOAD_COPY;

    sd.skin = NULL;
    CHECK(su_render_skinned_mesh_create(g_r, &sd, &msh) == SU_E_INVALID_ARGUMENT);
    sd.skin = sk;
    sd.joint_count = 0u;
    CHECK(su_render_skinned_mesh_create(g_r, &sd, &msh) == SU_E_INVALID_ARGUMENT);
    sd.joint_count = SU_ASSET_MAX_JOINTS + 1u;
    CHECK(su_render_skinned_mesh_create(g_r, &sd, &msh) == SU_E_INVALID_ARGUMENT);
    sd.joint_count = 2u;
    sk[1].joints[0] = 2u;
    CHECK(su_render_skinned_mesh_create(g_r, &sd, &msh) == SU_E_INVALID_ARGUMENT);
    sk[1].joints[0] = 0u;
    sk[2].weights[0] = -0.5f;
    CHECK(su_render_skinned_mesh_create(g_r, &sd, &msh) == SU_E_INVALID_ARGUMENT);
    sk[2].weights[0] = NAN;
    CHECK(su_render_skinned_mesh_create(g_r, &sd, &msh) == SU_E_INVALID_ARGUMENT);
    sk[2].weights[0] = 1.0f;

    model = make_skinned_model(0.25f, 1.0f, 0u, sk, 2u);
    mats[0] = mats[1] = su_mat4f_identity();
    init_obj(&o, model, 0.0f, 0.0f, -3.0f, 0u, 2u);
    init_frame(&f, &o, 1u, NULL, 2u);
    CHECK(su_render_frame_ex(g_r, &f) == SU_E_INVALID_ARGUMENT);

    init_frame(&f, &o, 1u, mats, 1u);
    CHECK(su_render_frame_ex(g_r, &f) == SU_E_INVALID_ARGUMENT);

    /* The exact v12 allocation has no readable palette tail (ASan guards the boundary). */
    {
        size_t size = offsetof(SuRenderFrameDesc, skin_matrices);
        SuRenderFrameDesc *old = malloc(size);
        CHECK(old != NULL);
        o.skin_count = 0u;
        init_frame(&f, &o, 1u, NULL, 0u);
        f.struct_size = (uint32_t)size;
        if (old != NULL) {
            memcpy(old, &f, size);
            CHECK(su_render_frame_ex(g_r, old) == SU_OK);
            free(old);
        }
    }
    init_frame(&f, &o, 1u, mats, UINT32_MAX);
    CHECK(su_render_frame_ex(g_r, &f) == SU_E_INVALID_ARGUMENT);

    o.skin_offset = UINT32_MAX - 1u;
    o.skin_count = 4u;
    init_frame(&f, &o, 1u, mats, 2u);
    CHECK(su_render_frame_ex(g_r, &f) == SU_E_INVALID_ARGUMENT);
}

static void test_skin_bind_pose_and_deform(void) {
    SuAssetSkinVertex sk_j0[4];
    SuAssetSkinVertex sk_blend[4];
    SuRenderMaterialDesc md;
    SuRenderHandle mat = 0;
    SuRenderHandle mod_j0;
    SuRenderHandle mod_blend;
    SuRenderObject o;
    SuRenderFrameDesc f;
    SuMat4f *big_mats;
    SuMat4f mats[2];
    int i;
    memset(&md, 0, sizeof md);
    su_asset_material_default(&md.factors);
    md.factors.base_color[0] = 0.9f;
    md.factors.base_color[1] = 0.2f;
    md.factors.base_color[2] = 0.2f;
    md.factors.metallic = 0.0f;
    md.factors.roughness = 1.0f;
    CHECK(su_render_material_create(g_r, &md, &mat) == SU_OK);

    memset(sk_j0, 0, sizeof sk_j0);
    memset(sk_blend, 0, sizeof sk_blend);
    for (i = 0; i < 4; i++) {
        sk_j0[i].joints[0] = 0u;
        sk_j0[i].weights[0] = 1.0f;
        sk_blend[i].joints[0] = 0u;
        sk_blend[i].joints[1] = 1u;
        sk_blend[i].weights[0] = FLT_MAX;
        sk_blend[i].weights[1] = FLT_MAX;
    }
    mod_j0 = make_skinned_model(0.25f, 3.0f, mat, sk_j0, 2u);
    mod_blend = make_skinned_model(0.25f, 3.0f, mat, sk_blend, 2u);

    /* 1. skin_count == 0 -> bind pose at (0, 0, -3) -> center (128, 128) lit, (202, 128) dark */
    init_obj(&o, mod_j0, 0.0f, 0.0f, -3.0f, 0u, 0u);
    init_frame(&f, &o, 1u, NULL, 0u);
    if (render_frames(&f, g_img)) {
        CHECK(px(g_img, 128, 128)[0] > 180u);
        CHECK(px(g_img, 202, 128)[0] < 10u);
    }

    /* 2. single-joint translation +1.0 in X -> (202, 128) lit, (128, 128) dark */
    mats[0] = mat_translate(1.0f, 0.0f, 0.0f);
    mats[1] = su_mat4f_identity();
    init_obj(&o, mod_j0, 0.0f, 0.0f, -3.0f, 0u, 1u);
    init_frame(&f, &o, 1u, mats, 2u);
    if (render_frames(&f, g_img)) {
        CHECK(px(g_img, 128, 128)[0] < 10u);
        CHECK(px(g_img, 202, 128)[0] > 180u);
    }

    /* 3. two-joint 50/50 blend (I and +2.0 in X -> +1.0 in X) -> (202, 128) lit, (128, 128) dark */
    mats[0] = su_mat4f_identity();
    mats[1] = mat_translate(2.0f, 0.0f, 0.0f);
    init_obj(&o, mod_blend, 0.0f, 0.0f, -3.0f, 0u, 2u);
    init_frame(&f, &o, 1u, mats, 2u);
    if (render_frames(&f, g_img)) {
        CHECK(px(g_img, 128, 128)[0] < 10u);
        CHECK(px(g_img, 202, 128)[0] > 180u);
    }

    /* A tight bind-pose sphere lies outside the camera, but the posed quad is at its center. */
    {
        SuRenderHandle tight = make_skinned_model(0.25f, 0.36f, mat, sk_j0, 2u);
        mats[0] = mat_translate(-20.0f, 0.0f, 0.0f);
        init_obj(&o, tight, 20.0f, 0.0f, -3.0f, 0u, 1u);
        init_frame(&f, &o, 1u, mats, 2u);
        if (render_frames(&f, g_img)) CHECK(px(g_img, 128, 128)[0] > 180u);
        o.skin_count = 0u;
        if (render_frames(&f, g_img)) CHECK(px(g_img, 128, 128)[0] < 10u);
    }

    /* 4. dynamic SSBO growth beyond initial 4096 matrices -> offset 4900 translates -1.0 in X -> (54, 128) lit */
    big_mats = calloc(5000u, sizeof *big_mats);
    CHECK(big_mats != NULL);
    if (big_mats != NULL) {
        for (i = 0; i < 5000; i++) {
            big_mats[i] = su_mat4f_identity();
        }
        big_mats[4900] = mat_translate(-1.0f, 0.0f, 0.0f);
        init_obj(&o, mod_j0, 0.0f, 0.0f, -3.0f, 4900u, 2u);
        init_frame(&f, &o, 1u, big_mats, 5000u);
        if (render_frames(&f, g_img)) {
            CHECK(px(g_img, 128, 128)[0] < 10u);
            CHECK(px(g_img, 54, 128)[0] > 180u);
        }
        free(big_mats);
    }
}

static void test_skin_normal_rotation_and_paint(void) {
    SuAssetSkinVertex sk[4];
    SuRenderPaintDesc pd;
    SuRenderHandle pmat = 0;
    SuRenderHandle mod;
    SuRenderObject o;
    SuRenderFrameDesc f;
    SuMat4f rot_lit;
    SuMat4f rot_dark;
    int i;
    memset(sk, 0, sizeof sk);
    for (i = 0; i < 4; i++) {
        sk[i].joints[0] = 0u;
        sk[i].weights[0] = 1.0f;
    }
    memset(&pd, 0, sizeof pd);
    su_asset_material_default(&pd.factors);
    pd.factors.base_color[0] = pd.factors.base_color[1] = pd.factors.base_color[2] = 0.8f;
    pd.factors.metallic = 0.0f;
    pd.factors.roughness = 1.0f;
    su_asset_paint_default(&pd.paint);
    pd.paint.layer_count = 1u;
    pd.paint.layer_order[0] = 0u;
    pd.paint.layer[0].color[0] = 0.9f;
    pd.paint.layer[0].color[1] = 0.9f;
    pd.paint.layer[0].color[2] = 0.9f;
    pd.paint.layer[0].roughness = 1.0f;
    pd.paint.layer[0].opacity = 1.0f;
    pd.paint.layer[0].flags = SU_PAINT_LAYER_TINT;
    CHECK(su_render_paint_material_create(g_r, &pd, &pmat) == SU_OK);

    mod = make_skinned_model(0.4f, 1.0f, pmat, sk, 1u);
    /* Sun comes from +X (sun_dir = -X): rotating normal (0,0,+1) by -pi/4 around Y yields (-sin, 0, cos) or +pi/4 yields (+sin, 0, cos).
     * mat_rotate_y(+pi/4) maps (0,0,1) -> (+sin(pi/4), 0, cos(pi/4)), facing +X toward the sun. */
    rot_lit = mat_rotate_y(0.78539816f);
    rot_dark = mat_rotate_y(-0.78539816f);

    init_obj(&o, mod, 0.0f, 0.0f, -3.0f, 0u, 1u);
    init_frame(&f, &o, 1u, &rot_lit, 1u);
    f.light.sun_dir = v3(-1.0f, 0.0f, 0.0f);
    if (render_frames(&f, g_img)) {
        init_frame(&f, &o, 1u, &rot_dark, 1u);
        f.light.sun_dir = v3(-1.0f, 0.0f, 0.0f);
        if (render_frames(&f, g_ref)) {
            CHECK(px(g_img, 128, 128)[0] > 140u);
            CHECK(px(g_ref, 128, 128)[0] < 20u);
        }
    }
}

static void test_skin_shadow(void) {
    SuAssetSkinVertex sk[4];
    SuRenderMaterialDesc md;
    SuRenderHandle mat = 0;
    SuRenderHandle receiver;
    SuRenderHandle caster;
    SuRenderObject objs[2];
    SuRenderFrameDesc f;
    SuMat4f shift;
    int i;
    memset(&md, 0, sizeof md);
    su_asset_material_default(&md.factors);
    md.factors.base_color[0] = md.factors.base_color[1] = md.factors.base_color[2] = 0.8f;
    md.factors.metallic = 0.0f;
    md.factors.roughness = 1.0f;
    CHECK(su_render_material_create(g_r, &md, &mat) == SU_OK);

    memset(sk, 0, sizeof sk);
    for (i = 0; i < 4; i++) {
        sk[i].joints[0] = 0u;
        sk[i].weights[0] = 1.0f;
    }
    receiver = make_unskinned_model(3.0f, 5.0f, mat);
    caster = make_skinned_model(0.45f, 3.0f, mat, sk, 1u);

    init_obj(&objs[0], receiver, 0.0f, 0.0f, -5.0f, 0u, 0u);
    init_obj(&objs[1], caster, 0.0f, 0.0f, -3.0f, 0u, 0u);

    /* Unskinned rest pose: caster at x=0, z=-3 with sun_dir=(+1,0,-1)/sqrt2 casts shadow at x=+2, z=-5.
     * Receiver pixel at x=+1, y=0, z=-5 (screen x=172, y=128) is lit. */
    init_frame(&f, objs, 2u, NULL, 0u);
    f.light.sun_dir = v3(0.7071068f, 0.0f, -0.7071068f);
    f.light.ambient = 0.05f;
    f.shadow.flags = SU_SHADOW_ON;
    f.shadow.strength = 1.0f;
    f.shadow.distance = 20.0f;
    f.shadow.cascades = 1u;
    if (render_frames(&f, g_ref)) {
        /* Deform caster by (-1, 0, 0): caster moves to x=-1, z=-3, shadow moves to x=+1, z=-5 (screen x=172, y=128). */
        shift = mat_translate(-1.0f, 0.0f, 0.0f);
        objs[1].skin_offset = 0u;
        objs[1].skin_count = 1u;
        f.skin_matrices = &shift;
        f.skin_matrix_count = 1u;
        if (render_frames(&f, g_img)) {
            CHECK(px(g_ref, 172, 128)[0] > 140u);
            CHECK(px(g_img, 172, 128)[0] < 60u);
        }
    }
}

static void test_skin_pak_roundtrip(void) {
    SuAssetWriter *w = NULL;
    SuAssetVertex v[4];
    uint32_t idx[6];
    SuAssetSubmesh sm;
    SuAssetSkinVertex sk[4];
    SuAssetMaterial m;
    SuAssetModelNode mn;
    uint64_t id_mat = su_asset_id("skin_pak/mat");
    uint64_t id_mesh = su_asset_id("skin_pak/mesh");
    uint64_t id_model = su_asset_id("skin_pak/model");
    uint8_t *pak_bytes = NULL;
    size_t pak_len = 0;
    SuAssetPak *pak = NULL;
    SuRenderPak rpak;
    char msg[256];
    SuRenderObject o;
    SuRenderFrameDesc f;
    SuMat4f shift;
    int i;
    make_quad(0.25f, v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 6;
    sm.material = id_mat;
    sm.radius = 2.0f;
    memset(sk, 0, sizeof sk);
    for (i = 0; i < 4; i++) {
        sk[i].joints[0] = 0u;
        sk[i].weights[0] = 1.0f;
    }
    su_asset_material_default(&m);
    m.base_color[0] = 0.2f;
    m.base_color[1] = 0.9f;
    m.base_color[2] = 0.2f;
    m.metallic = 0.0f;
    m.roughness = 1.0f;
    memset(&mn, 0, sizeof mn);
    mn.transform[0] = mn.transform[5] = mn.transform[10] = mn.transform[15] = 1.0f;
    mn.lod_mesh[0] = id_mesh;
    mn.lod_screen[0] = 1.0f;
    mn.lod_count = 1u;
    CHECK(su_asset_writer_create(&w) == SU_OK);
    CHECK(su_asset_writer_add_material(w, id_mat, &m) == SU_OK);
    CHECK(su_asset_writer_add_skinned_mesh(w, id_mesh, v, 4, idx, 6, &sm, 1, sk, 1u) == SU_OK);
    CHECK(su_asset_writer_add_model(w, id_model, &mn, 1) == SU_OK);
    CHECK(su_asset_writer_finish(w, &pak_bytes, &pak_len) == SU_OK);
    su_asset_writer_destroy(w);
    CHECK(pak_bytes != NULL && su_asset_pak_open(pak_bytes, pak_len, 0, &pak, msg) == SU_OK);
    free(pak_bytes);
    if (pak != NULL) {
        memset(&rpak, 0, sizeof rpak);
        CHECK(su_render_upload_pak(g_r, pak, id_model, &rpak) == SU_OK);
        shift = mat_translate(1.0f, 0.0f, 0.0f);
        init_obj(&o, rpak.model, 0.0f, 0.0f, -3.0f, 0u, 1u);
        init_frame(&f, &o, 1u, &shift, 1u);
        if (render_frames(&f, g_img)) {
            CHECK(su_render_pak_state(g_r, &rpak) == SU_RENDER_STATE_READY);
            CHECK(px(g_img, 128, 128)[1] < 10u);
            CHECK(px(g_img, 202, 128)[1] > 180u);
        }
        su_render_pak_release(g_r, &rpak);
        su_asset_pak_close(pak);
    }
}

int main(int argc, char **argv) {
    int validation = 1;
    SuRenderDeviceInfo devs[8];
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

    test_skin_validation_and_bounds();
    test_skin_bind_pose_and_deform();
    test_skin_normal_rotation_and_paint();
    test_skin_shadow();
    test_skin_pak_roundtrip();

    CHECK(su_render_stats(g_r, &st) == SU_OK);
    if (validation) {
        CHECK(getenv("SUTEKH_REQUIRE_VALIDATION") == NULL || st.validation_enabled != 0u);
        printf("validation %s: %u errors, %u warnings\n", st.validation_enabled ? "on" : "not available",
               st.validation_errors, st.validation_warnings);
        CHECK(st.validation_errors == 0u && st.validation_warnings == 0u);
    }
    su_render_destroy(g_r);
    printf("test_render_skinning: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
