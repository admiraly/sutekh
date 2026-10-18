/* Cooked asset packs (sutekh/asset.h) without the importer: stable ids against independently computed vectors,
 * writer/reader round trip, byte-identical determinism, every single-byte corruption refused, structural
 * corruptions with recomputed hashes refused with a reason, a random corruption fuzz, borrowed and file opens, and
 * the background loader.
 *
 * Usage: test_asset_pak   (writes scratch files in the working directory) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ir/su_sha256.h"
#include "sutekh/asset.h"

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

static uint64_t ID_MESH;
static uint64_t ID_MESH_LOD;
static uint64_t ID_TEX;
static uint64_t ID_MAT;
static uint64_t ID_MODEL;

/* A unit cube: 24 vertices, 36 indices, two submeshes (one with the material, one with the default). */
static void cube(SuAssetVertex *v, uint32_t *idx) {
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int f;
    for (f = 0; f < 6; f++) {
        float u[3] = {n[f][1] != 0.0f ? 1.0f : 0.0f, 0.0f, 0.0f};
        float w[3];
        int c;
        if (u[0] == 0.0f) {
            u[1] = 1.0f;
            if (n[f][1] != 0.0f) {
                u[1] = 0.0f;
                u[2] = 1.0f;
            }
        }
        if (n[f][0] != 0.0f) {
            u[0] = 0.0f;
            u[1] = 1.0f;
            u[2] = 0.0f;
        }
        w[0] = n[f][1] * u[2] - n[f][2] * u[1];
        w[1] = n[f][2] * u[0] - n[f][0] * u[2];
        w[2] = n[f][0] * u[1] - n[f][1] * u[0];
        for (c = 0; c < 4; c++) {
            float a = (c == 1 || c == 2) ? 0.5f : -0.5f;
            float b = c >= 2 ? 0.5f : -0.5f;
            SuAssetVertex *x = &v[f * 4 + c];
            int k;
            memset(x, 0, sizeof *x);
            for (k = 0; k < 3; k++) {
                x->pos[k] = 0.5f * n[f][k] + a * w[k] + b * u[k];
                x->normal[k] = n[f][k];
                x->tangent[k] = w[k];
            }
            x->tangent[3] = 1.0f;
            x->uv0[0] = a + 0.5f;
            x->uv0[1] = b + 0.5f;
            x->uv1[0] = x->uv0[1];
            x->uv1[1] = x->uv0[0];
        }
        idx[f * 6 + 0] = (uint32_t)f * 4u + 0u;
        idx[f * 6 + 1] = (uint32_t)f * 4u + 1u;
        idx[f * 6 + 2] = (uint32_t)f * 4u + 2u;
        idx[f * 6 + 3] = (uint32_t)f * 4u + 0u;
        idx[f * 6 + 4] = (uint32_t)f * 4u + 2u;
        idx[f * 6 + 5] = (uint32_t)f * 4u + 3u;
    }
}

/* Builds the reference pack; order 0 or 1 adds the resources in different orders. */
static uint8_t *build(int order, size_t *len) {
    SuAssetWriter *w = NULL;
    SuAssetVertex v[24];
    uint32_t idx[36];
    SuAssetSubmesh sm[2];
    SuAssetMaterial m;
    SuAssetModelNode node[2];
    uint8_t mip0[8 * 4 * 4];
    uint8_t mip1[4 * 2 * 4];
    uint8_t mip2[2 * 1 * 4];
    uint8_t mip3[1 * 1 * 4];
    const uint8_t *mips[4] = {mip0, mip1, mip2, mip3};
    uint8_t *out = NULL;
    int i;
    cube(v, idx);
    memset(sm, 0, sizeof sm);
    sm[0].first_index = 0;
    sm[0].index_count = 18;
    sm[0].material = ID_MAT;
    sm[1].first_index = 18;
    sm[1].index_count = 18;
    for (i = 0; i < (int)sizeof mip0; i++) {
        mip0[i] = (uint8_t)(i * 7);
    }
    memset(mip1, 0x40, sizeof mip1);
    memset(mip2, 0x80, sizeof mip2);
    memset(mip3, 0xC0, sizeof mip3);
    su_asset_material_default(&m);
    m.base_color[1] = 0.5f;
    m.emissive[0] = 2.0f;
    m.texture[SU_ASSET_SLOT_BASE_COLOR] = ID_TEX;
    m.tex_info[SU_ASSET_SLOT_BASE_COLOR] = SU_ASSET_TEX_INFO(1, SU_ASSET_WRAP_CLAMP, SU_ASSET_WRAP_MIRROR);
    memset(node, 0, sizeof node);
    for (i = 0; i < 2; i++) {
        node[i].transform[0] = node[i].transform[5] = node[i].transform[10] = node[i].transform[15] = 1.0f;
        node[i].lod_mesh[0] = ID_MESH;
        node[i].lod_screen[0] = 1.0f;
        node[i].lod_count = 1;
    }
    node[1].transform[12] = 10.0f;
    node[1].lod_mesh[1] = ID_MESH_LOD;
    node[1].lod_screen[1] = 0.25f;
    node[1].lod_count = 2;
    if (su_asset_writer_create(&w) != SU_OK) {
        return NULL;
    }
    if (order == 0) {
        CHECK(su_asset_writer_add_texture(w, ID_TEX, 8, 4, SU_TEX_RGBA8_SRGB, 0, 4, mips) == SU_OK);
        CHECK(su_asset_writer_add_material(w, ID_MAT, &m) == SU_OK);
        CHECK(su_asset_writer_add_mesh(w, ID_MESH, v, 24, idx, 36, sm, 2) == SU_OK);
        CHECK(su_asset_writer_add_mesh(w, ID_MESH_LOD, v, 24, idx, 36, sm + 1, 1) == SU_OK);
        CHECK(su_asset_writer_add_model(w, ID_MODEL, node, 2) == SU_OK);
    } else {
        CHECK(su_asset_writer_add_mesh(w, ID_MESH_LOD, v, 24, idx, 36, sm + 1, 1) == SU_OK);
        CHECK(su_asset_writer_add_mesh(w, ID_MESH, v, 24, idx, 36, sm, 2) == SU_OK);
        CHECK(su_asset_writer_add_model(w, ID_MODEL, node, 2) == SU_OK);
        CHECK(su_asset_writer_add_material(w, ID_MAT, &m) == SU_OK);
        CHECK(su_asset_writer_add_texture(w, ID_TEX, 8, 4, SU_TEX_RGBA8_SRGB, 0, 4, mips) == SU_OK);
    }
    CHECK(su_asset_writer_finish(w, &out, len) == SU_OK);
    su_asset_writer_destroy(w);
    return out;
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Recomputes every entry hash and the content hash (a forger who knows the format). */
static void rehash(uint8_t *b, size_t len) {
    uint32_t n = rd32(b + 12);
    uint32_t i;
    SuSha256 s;
    for (i = 0; i < n; i++) {
        uint8_t *e = b + 64 + (size_t)i * 64u;
        uint64_t off = rd64(e + 16);
        uint64_t size = rd64(e + 24);
        if (off <= len && size <= len - off) {
            su_sha256_init(&s);
            su_sha256_update(&s, b + off, (size_t)size);
            su_sha256_final(&s, e + 32);
        }
    }
    su_sha256_init(&s);
    su_sha256_update(&s, b + 64, len - 64u);
    su_sha256_final(&s, b + 24);
}

static uint64_t entry_offset(const uint8_t *b, uint64_t id) {
    uint32_t n = rd32(b + 12);
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (rd64(b + 64 + (size_t)i * 64u) == id) {
            return rd64(b + 64 + (size_t)i * 64u + 16u);
        }
    }
    return 0;
}

static void test_ids(void) {
    /* Vectors computed independently with Python hashlib: first 8 bytes (little-endian) of
     * sha256("sutekh.asset.v1:" + name). */
    CHECK(su_asset_id("foo") == UINT64_C(0x9fd51f0cb83faf04));
    CHECK(su_asset_id("ship/mesh/hull") == UINT64_C(0x878a187a3a7b1a86));
    CHECK(su_asset_id("") == UINT64_C(0xb42e8ca347da37fb));
    CHECK(su_asset_id("foo") != su_asset_id("Foo"));
    CHECK(su_asset_texture_level_size(SU_TEX_RGBA8_SRGB, 3, 5) == 60u);
    CHECK(su_asset_texture_level_size(SU_TEX_BC1_UNORM, 5, 5) == 32u);
    CHECK(su_asset_texture_level_size(SU_TEX_BC7_SRGB, 1, 1) == 16u);
    CHECK(su_asset_texture_level_size(99, 4, 4) == 0u);
}

static void test_roundtrip(void) {
    size_t la = 0;
    size_t lb = 0;
    uint8_t *a = build(0, &la);
    uint8_t *b = build(1, &lb);
    SuAssetPak *p = NULL;
    char msg[256];
    SuAssetMeshView mv;
    SuAssetTextureView tv;
    SuAssetMaterial mat;
    SuAssetModelView model;
    SuAssetEntry e = {0};
    uint8_t d[32];
    uint8_t d2[32];
    uint32_t i;
    SuSha256 s;
    CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        return;
    }
    CHECK(la == lb && memcmp(a, b, la) == 0); /* the writer sorts: insertion order does not matter */
    CHECK(su_asset_pak_open(a, la, 0, &p, msg) == SU_OK);
    if (p == NULL) {
        fprintf(stderr, "open: %s\n", msg);
        free(a);
        free(b);
        return;
    }
    CHECK(su_asset_pak_entry_count(p) == 5u);
    for (i = 1; i < 5u; i++) {
        SuAssetEntry prev;
        CHECK(su_asset_pak_entry(p, i - 1u, &prev) == SU_OK && su_asset_pak_entry(p, i, &e) == SU_OK);
        CHECK(prev.id < e.id && (e.offset & 15u) == 0u);
    }
    su_asset_pak_digest(p, d);
    su_sha256_init(&s);
    su_sha256_update(&s, a + 64, la - 64u);
    su_sha256_final(&s, d2);
    CHECK(memcmp(d, d2, 32) == 0);
    CHECK(su_asset_pak_mesh(p, ID_MESH, &mv) == SU_OK);
    CHECK(mv.header->vertex_count == 24u && mv.header->index_count == 36u && mv.header->submesh_count == 2u);
    CHECK(fabsf(mv.header->aabb_min[0] + 0.5f) < 1e-6f && fabsf(mv.header->aabb_max[2] - 0.5f) < 1e-6f);
    CHECK(fabsf(mv.header->radius - sqrtf(0.75f)) < 1e-3f);
    CHECK(mv.submeshes[0].material == ID_MAT && mv.submeshes[1].material == 0u && mv.submeshes[1].first_index == 18u);
    CHECK(mv.indices[35] == 23u && mv.vertices[5].normal[0] == -1.0f);
    CHECK(((uintptr_t)mv.vertices & 7u) == 0u);
    CHECK(su_asset_pak_texture(p, ID_TEX, &tv) == SU_OK);
    CHECK(tv.header->width == 8u && tv.header->height == 4u && tv.header->mip_count == 4u &&
          tv.header->format == SU_TEX_RGBA8_SRGB);
    CHECK(tv.mip_size[0] == 128u && tv.mip_size[3] == 4u && tv.mip_data[0][9] == 63u && tv.mip_data[3][0] == 0xC0u);
    CHECK(su_asset_pak_material(p, ID_MAT, &mat) == SU_OK);
    CHECK(mat.base_color[1] == 0.5f && mat.emissive[0] == 2.0f && mat.texture[0] == ID_TEX &&
          mat.tex_info[0] == SU_ASSET_TEX_INFO(1, 1, 2));
    CHECK(su_asset_pak_model(p, ID_MODEL, &model) == SU_OK);
    CHECK(model.header->node_count == 2u && model.nodes[1].lod_count == 2u && model.nodes[1].lod_mesh[1] == ID_MESH_LOD);
    /* bounds: cubes at x = 0 and x = 10, radius sqrt(0.75) each */
    CHECK(fabsf(model.header->center[0] - 5.0f) < 1e-3f && fabsf(model.header->radius - (5.0f + sqrtf(0.75f))) < 1e-2f);
    CHECK(su_asset_pak_mesh(p, ID_TEX, &mv) == SU_E_INVALID_ARGUMENT);   /* wrong type */
    CHECK(su_asset_pak_find(p, 12345u, 0, &i) == SU_E_INVALID_ARGUMENT); /* unknown id */
    su_asset_pak_close(p);

    /* borrowed bytes: must be 16-byte aligned */
    {
        uint8_t *al = malloc(la + 32u);
        uint8_t *at = (uint8_t *)(((uintptr_t)al + 15u) & ~(uintptr_t)15u);
        memcpy(at, a, la);
        CHECK(su_asset_pak_open(at, la, SU_ASSET_PAK_BORROW, &p, msg) == SU_OK);
        su_asset_pak_close(p);
        memmove(at + 1, at, la);
        CHECK(su_asset_pak_open(at + 1, la, SU_ASSET_PAK_BORROW, &p, msg) == SU_E_INVALID_ARGUMENT);
        CHECK(su_asset_pak_open(at + 1, la, 0, &p, msg) == SU_OK); /* copied: alignment does not matter */
        su_asset_pak_close(p);
        free(al);
    }
    free(a);
    free(b);
}

static void test_writer_refusals(void) {
    SuAssetWriter *w = NULL;
    SuAssetVertex v[24];
    uint32_t idx[36];
    SuAssetSubmesh sm;
    SuAssetMaterial m;
    uint8_t px[4] = {1, 2, 3, 4};
    const uint8_t *mips[2] = {px, px};
    cube(v, idx);
    memset(&sm, 0, sizeof sm);
    sm.index_count = 36;
    su_asset_material_default(&m);
    CHECK(su_asset_writer_create(&w) == SU_OK);
    CHECK(su_asset_writer_add_material(w, 0, &m) == SU_E_INVALID_ARGUMENT);
    CHECK(su_asset_writer_add_material(w, 7, &m) == SU_OK);
    CHECK(su_asset_writer_add_material(w, 7, &m) == SU_E_INVALID_ARGUMENT); /* duplicate */
    idx[5] = 24;
    CHECK(su_asset_writer_add_mesh(w, 8, v, 24, idx, 36, &sm, 1) == SU_E_INVALID_ARGUMENT);
    idx[5] = 3;
    CHECK(su_asset_writer_add_mesh(w, 8, v, 24, idx, 35, &sm, 1) == SU_E_INVALID_ARGUMENT);
    sm.index_count = 39;
    CHECK(su_asset_writer_add_mesh(w, 8, v, 24, idx, 36, &sm, 1) == SU_E_INVALID_ARGUMENT);
    sm.index_count = 36;
    v[3].pos[1] = NAN;
    CHECK(su_asset_writer_add_mesh(w, 8, v, 24, idx, 36, &sm, 1) == SU_E_INVALID_ARGUMENT);
    v[3].pos[1] = 0.5f;
    CHECK(su_asset_writer_add_texture(w, 9, 1, 1, SU_TEX_RGBA8_UNORM, 0, 2, mips) == SU_E_INVALID_ARGUMENT); /* 2 mips of 1x1 */
    CHECK(su_asset_writer_add_texture(w, 9, 1, 1, 77, 0, 1, mips) == SU_E_INVALID_ARGUMENT);
    {
        SuAssetModelNode n;
        memset(&n, 0, sizeof n);
        n.transform[0] = n.transform[5] = n.transform[10] = n.transform[15] = 1.0f;
        n.lod_count = 1;
        n.lod_mesh[0] = 8; /* not added */
        CHECK(su_asset_writer_add_model(w, 10, &n, 1) == SU_E_INVALID_ARGUMENT);
    }
    su_asset_writer_destroy(w);
}

/* Every single-byte change of a valid pack is refused (the header is fully checked and the content hash covers the
 * rest). */
static void test_every_byte(void) {
    size_t len = 0;
    uint8_t *a = build(0, &len);
    size_t i;
    long accepted = 0;
    char msg[256];
    for (i = 0; i < len; i++) {
        SuAssetPak *p = NULL;
        a[i] ^= 0x5Au;
        if (su_asset_pak_open(a, len, 0, &p, msg) == SU_OK) {
            accepted++;
            su_asset_pak_close(p);
        }
        a[i] ^= 0x5Au;
    }
    CHECK(accepted == 0);
    printf("single-byte corruptions: %zu tried, %ld accepted\n", len, accepted);
    {
        /* an entry hash that disagrees with its payload, content hash recomputed: the default open trusts the content
         * hash (the bytes are consistent); SU_ASSET_PAK_VERIFY_ENTRIES finds the stale entry hash */
        SuAssetPak *p = NULL;
        SuSha256 s;
        a[64 + 32] ^= 1u;
        su_sha256_init(&s);
        su_sha256_update(&s, a + 64, len - 64u);
        su_sha256_final(&s, a + 24);
        CHECK(su_asset_pak_open(a, len, 0, &p, msg) == SU_OK);
        su_asset_pak_close(p);
        CHECK(su_asset_pak_open(a, len, SU_ASSET_PAK_VERIFY_ENTRIES, &p, msg) == SU_E_PARSE &&
              strstr(msg, "payload hash") != NULL);
        a[64 + 32] ^= 1u;
        su_sha256_init(&s);
        su_sha256_update(&s, a + 64, len - 64u);
        su_sha256_final(&s, a + 24);
        CHECK(su_asset_pak_open(a, len, SU_ASSET_PAK_VERIFY_ENTRIES, &p, msg) == SU_OK);
        su_asset_pak_close(p);
    }
    {
        SuAssetPak *p = NULL;
        CHECK(su_asset_pak_open(a, len - 1u, 0, &p, msg) == SU_E_PARSE);
        CHECK(su_asset_pak_open(a, 10, 0, &p, msg) == SU_E_PARSE);
        CHECK(su_asset_pak_open(a, 0, 0, &p, msg) == SU_E_PARSE);
    }
    free(a);
}

typedef struct Forge {
    const char *what;
    uint64_t id;
    size_t at;      /* offset inside the payload */
    uint32_t value; /* written as u32 */
    const char *expect;
} Forge;

/* Structural corruptions with every hash recomputed: refused by the layout and reference checks, with a reason. */
static void test_forged(void) {
    size_t len = 0;
    uint8_t *ref = build(0, &len);
    const Forge cases[] = {
        {"index out of range", 0, 64u + 64u + 24u * 56u, 24u, "index"},
        {"submesh past the end", 0, 64u + 4u, 40u, "submesh"},
        {"vertex count", 0, 0u, 25u, "header"},
        {"material -> missing texture", 0, 64u, 0x1234u, "texture slot"},
        {"submesh -> missing material", 0, 64u + 8u, 0x99u, "submesh"},
        {"texture mip size", 0, 64u + 8u, 129u, "mip"},
        {"texture format", 0, 8u, 99u, "header"},
        {"model -> missing mesh", 0, 64u + 64u, 0x77u, "LOD"},
        {"model LOD screen not decreasing", 0, 64u + 128u + 96u + 4u, 0x3F800000u, "LOD"},
        {"material alpha mode", 0, 52u, 7u, "modes"},
    };
    uint64_t ids[10];
    size_t k;
    ids[0] = ids[1] = ids[2] = ID_MESH;
    ids[3] = ID_MAT;
    ids[4] = ID_MESH;
    ids[5] = ids[6] = ID_TEX;
    ids[7] = ids[8] = ID_MODEL;
    ids[9] = ID_MAT;
    for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        uint8_t *b = malloc(len);
        SuAssetPak *p = NULL;
        char msg[256];
        uint64_t off;
        uint32_t st;
        memcpy(b, ref, len);
        off = entry_offset(b, ids[k]) + cases[k].at;
        b[off] = (uint8_t)cases[k].value;
        b[off + 1u] = (uint8_t)(cases[k].value >> 8);
        b[off + 2u] = (uint8_t)(cases[k].value >> 16);
        b[off + 3u] = (uint8_t)(cases[k].value >> 24);
        rehash(b, len);
        msg[0] = '\0';
        st = su_asset_pak_open(b, len, 0, &p, msg);
        CHECK(st == SU_E_PARSE);
        CHECK(strstr(msg, cases[k].expect) != NULL);
        if (st != SU_E_PARSE || strstr(msg, cases[k].expect) == NULL) {
            fprintf(stderr, "  forged case '%s': status %u, message '%s'\n", cases[k].what, (unsigned)st, msg);
        }
        su_asset_pak_close(p);
        free(b);
    }
    {
        /* The control: the untouched reference passes after a rehash too. */
        SuAssetPak *p = NULL;
        rehash(ref, len);
        CHECK(su_asset_pak_open(ref, len, 0, &p, NULL) == SU_OK);
        su_asset_pak_close(p);
    }
    free(ref);
}

static uint64_t g_rng = 0x9E3779B97F4A7C15u;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

/* Random corruptions (1-8 bytes anywhere past the header, hashes recomputed): nothing crashes, and every accepted
 * pack's views stay inside its bytes. */
static void test_fuzz(void) {
    size_t len = 0;
    uint8_t *ref = build(0, &len);
    long accepted = 0;
    long rejected = 0;
    int it;
    for (it = 0; it < 3000; it++) {
        uint8_t *b = malloc(len);
        SuAssetPak *p = NULL;
        uint32_t nb = 1u + rnd() % 8u;
        uint32_t j;
        memcpy(b, ref, len);
        for (j = 0; j < nb; j++) {
            size_t at = 64u + rnd() % (uint32_t)(len - 64u);
            b[at] = (uint8_t)rnd();
        }
        rehash(b, len);
        if (su_asset_pak_open(b, len, 0, &p, NULL) == SU_OK) {
            uint32_t n = su_asset_pak_entry_count(p);
            uint32_t i;
            size_t plen = 0;
            const uint8_t *base = su_asset_pak_bytes(p, &plen);
            accepted++;
            for (i = 0; i < n; i++) {
                SuAssetEntry e = {0};
                SuAssetMeshView mv;
                (void)su_asset_pak_entry(p, i, &e);
                if (e.type == SU_ASSET_MESH && su_asset_pak_mesh(p, e.id, &mv) == SU_OK) {
                    const uint8_t *end = (const uint8_t *)(mv.indices + mv.header->index_count);
                    uint32_t q;
                    CHECK(end <= base + plen);
                    for (q = 0; q < mv.header->index_count; q++) {
                        CHECK(mv.indices[q] < mv.header->vertex_count);
                    }
                }
            }
            su_asset_pak_close(p);
        } else {
            rejected++;
        }
        free(b);
    }
    printf("fuzz: 3000 forged packs, %ld accepted (layout-valid changes such as float values), %ld rejected\n", accepted,
           rejected);
    CHECK(rejected > 0);
    free(ref);
}

static double wall_s(void) {
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int write_file(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    int ok = f != NULL && fwrite(b, 1, n, f) == n;
    if (f != NULL) {
        ok = fclose(f) == 0 && ok;
    }
    return ok;
}

static void test_files_and_loader(void) {
    size_t len = 0;
    uint8_t *a = build(0, &len);
    SuAssetPak *p = NULL;
    SuAssetLoader *l = NULL;
    char msg[256];
    int got[5] = {0, 0, 0, 0, 0};
    uint32_t status[5];
    double t0;
    CHECK(write_file("asset_pak_ok.supak", a, len));
    a[100] ^= 1u;
    CHECK(write_file("asset_pak_bad.supak", a, len));
    a[100] ^= 1u;
    CHECK(su_asset_pak_open_file("asset_pak_ok.supak", &p, msg) == SU_OK);
    su_asset_pak_close(p);
    CHECK(su_asset_pak_open_file("asset_pak_bad.supak", &p, msg) == SU_E_PARSE);
    CHECK(su_asset_pak_open_file("asset_pak_missing.supak", &p, msg) == SU_E_IO);
    CHECK(su_asset_loader_create(NULL, &l) == SU_OK);
    CHECK(su_asset_loader_request(l, "asset_pak_ok.supak", 0) == SU_OK);
    CHECK(su_asset_loader_request(l, "asset_pak_bad.supak", 1) == SU_OK);
    CHECK(su_asset_loader_request(l, "asset_pak_missing.supak", 2) == SU_OK);
    CHECK(su_asset_loader_request(l, "model.gltf", 3) == SU_OK); /* no cook function installed */
    CHECK(su_asset_loader_request(l, "asset_pak_ok.supak", 4) == SU_OK);
    t0 = wall_s();
    while ((got[0] + got[1] + got[2] + got[3] + got[4]) < 5 && wall_s() - t0 < 20.0) {
        SuAssetLoadResult r;
        if (su_asset_loader_poll(l, &r)) {
            CHECK(r.user < 5u && !got[r.user]);
            if (r.user < 5u) {
                got[r.user] = 1;
                status[r.user] = r.status;
            }
            CHECK((r.status == SU_OK) == (r.pak != NULL));
            if (r.pak != NULL) {
                SuAssetMeshView mv;
                CHECK(su_asset_pak_mesh(r.pak, ID_MESH, &mv) == SU_OK);
                su_asset_pak_close(r.pak);
            }
        }
    }
    CHECK(got[0] && got[1] && got[2] && got[3] && got[4]);
    if (got[0] && got[1] && got[2] && got[3] && got[4]) {
        CHECK(status[0] == SU_OK && status[1] == SU_E_PARSE && status[2] == SU_E_IO &&
              status[3] == SU_E_UNSUPPORTED_METHOD && status[4] == SU_OK);
    }
    CHECK(su_asset_loader_pending(l) == 0u);
    {
        /* Bounded queue: 64 requests in flight at most; destroy with pending work frees everything. */
        int i;
        uint32_t refused = 0;
        for (i = 0; i < 70; i++) {
            refused += su_asset_loader_request(l, "asset_pak_ok.supak", 100u + (uint64_t)i) == SU_E_RESOURCE_LIMIT;
        }
        CHECK(refused >= 6u);
    }
    su_asset_loader_destroy(l);
    (void)remove("asset_pak_ok.supak");
    (void)remove("asset_pak_bad.supak");
    free(a);
}

int main(void) {
    ID_MESH = su_asset_id("test/mesh/cube");
    ID_MESH_LOD = su_asset_id("test/mesh/cube_lod1");
    ID_TEX = su_asset_id("test/tex/checker.srgb");
    ID_MAT = su_asset_id("test/mat/paint");
    ID_MODEL = su_asset_id("test/model");
    test_ids();
    test_roundtrip();
    test_writer_refusals();
    test_every_byte();
    test_forged();
    test_fuzz();
    test_files_and_loader();
    printf("test_asset_pak: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
