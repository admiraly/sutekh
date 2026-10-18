/* Cooked asset packs (sutekh/asset.h): stable ids, the writer, the validating reader, and the background loader.
 * No third-party code; always built. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ir/su_sha256.h"
#include "su_thread.h"
#include "sutekh/asset.h"

_Static_assert(sizeof(SuAssetVertex) == 56, "vertex layout");
_Static_assert(sizeof(SuAssetSubmesh) == 32, "submesh layout");
_Static_assert(sizeof(SuAssetMeshHeader) == 64, "mesh header layout");
_Static_assert(sizeof(SuAssetTextureHeader) == 64, "texture header layout");
_Static_assert(sizeof(SuAssetMip) == 16, "mip layout");
_Static_assert(sizeof(SuAssetMaterial) == 128, "material layout");
_Static_assert(sizeof(SuAssetModelNode) == 128, "model node layout");
_Static_assert(sizeof(SuAssetModelHeader) == 64, "model header layout");

#define HEADER_BYTES 64u
#define ENTRY_BYTES 64u
#define MAX_ENTRIES 1000000u
static const uint8_t MAGIC[8] = {'S', 'U', 'A', 'P', 'A', 'K', 0, 1};

static void msgf(char *msg, const char *fmt, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(msg, 256, fmt, ap);
        va_end(ap);
    }
}

static int host_is_little_endian(void) {
    const uint16_t one = 1u;
    uint8_t b;
    memcpy(&b, &one, 1);
    return b == 1u;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

uint64_t su_asset_id(const char *name) {
    static const char prefix[] = "sutekh.asset.v1:";
    SuSha256 s;
    uint8_t d[32];
    uint64_t id;
    su_sha256_init(&s);
    su_sha256_update(&s, prefix, sizeof prefix - 1u);
    if (name != NULL) {
        su_sha256_update(&s, name, strlen(name));
    }
    su_sha256_final(&s, d);
    id = rd64(d);
    return id != 0u ? id : 1u;
}

uint64_t su_asset_texture_level_size(uint32_t format, uint32_t width, uint32_t height) {
    uint64_t bw = ((uint64_t)width + 3u) / 4u;
    uint64_t bh = ((uint64_t)height + 3u) / 4u;
    switch (format) {
    case SU_TEX_RGBA8_UNORM:
    case SU_TEX_RGBA8_SRGB:
        return (uint64_t)width * height * 4u;
    case SU_TEX_BC1_UNORM:
    case SU_TEX_BC1_SRGB:
    case SU_TEX_BC4_UNORM:
        return bw * bh * 8u;
    case SU_TEX_BC3_UNORM:
    case SU_TEX_BC3_SRGB:
    case SU_TEX_BC5_UNORM:
    case SU_TEX_BC7_UNORM:
    case SU_TEX_BC7_SRGB:
        return bw * bh * 16u;
    default:
        return 0;
    }
}

const char *su_asset_texture_format_name(uint32_t format) {
    static const char *names[SU_TEX_FORMAT_COUNT] = {"unknown", "rgba8", "rgba8_srgb", "bc1", "bc1_srgb", "bc3",
                                                     "bc3_srgb", "bc4", "bc5", "bc7", "bc7_srgb"};
    return format < SU_TEX_FORMAT_COUNT ? names[format] : "unknown";
}

void su_asset_material_default(SuAssetMaterial *m) {
    memset(m, 0, sizeof *m);
    m->base_color[0] = m->base_color[1] = m->base_color[2] = m->base_color[3] = 1.0f;
    m->emissive_strength = 1.0f;
    m->metallic = 1.0f;
    m->roughness = 1.0f;
    m->normal_scale = 1.0f;
    m->occlusion_strength = 1.0f;
    m->alpha_cutoff = 0.5f;
}

static int finite3(const float *v) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static int finite_n(const float *v, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (!isfinite(v[i])) {
            return 0;
        }
    }
    return 1;
}

static uint32_t max_mips(uint32_t w, uint32_t h) {
    uint32_t m = w > h ? w : h;
    uint32_t n = 1;
    while (m > 1u) {
        m >>= 1;
        n++;
    }
    return n;
}

/* ---------- writer ---------- */

typedef struct WEntry {
    uint64_t id;
    uint32_t type;
    uint8_t *data;
    size_t size;
} WEntry;

struct SuAssetWriter {
    WEntry *e;
    uint32_t n;
    uint32_t cap;
};

uint32_t su_asset_writer_create(SuAssetWriter **out) {
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = calloc(1, sizeof **out);
    return *out != NULL ? SU_OK : SU_E_RESOURCE_LIMIT;
}

static void writer_clear(SuAssetWriter *w) {
    uint32_t i;
    for (i = 0; i < w->n; i++) {
        free(w->e[i].data);
    }
    w->n = 0;
}

void su_asset_writer_destroy(SuAssetWriter *w) {
    if (w != NULL) {
        writer_clear(w);
        free(w->e);
        free(w);
    }
}

/* Takes ownership of data (freed on failure too). */
static uint32_t writer_put(SuAssetWriter *w, uint64_t id, uint32_t type, uint8_t *data, size_t size) {
    uint32_t i;
    if (id == 0u) {
        free(data);
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < w->n; i++) {
        if (w->e[i].id == id) {
            free(data);
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (w->n == w->cap) {
        uint32_t nc = w->cap ? w->cap * 2u : 16u;
        WEntry *ne = realloc(w->e, (size_t)nc * sizeof *ne);
        if (ne == NULL) {
            free(data);
            return SU_E_RESOURCE_LIMIT;
        }
        w->e = ne;
        w->cap = nc;
    }
    w->e[w->n].id = id;
    w->e[w->n].type = type;
    w->e[w->n].data = data;
    w->e[w->n].size = size;
    w->n++;
    return SU_OK;
}

static void sphere_of(const SuAssetVertex *v, const uint32_t *idx, uint32_t first, uint32_t count, float c[3],
                      float *r) {
    float lo[3] = {INFINITY, INFINITY, INFINITY};
    float hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    float rr = 0.0f;
    uint32_t i;
    int k;
    for (i = 0; i < count; i++) {
        const float *p = v[idx[first + i]].pos;
        for (k = 0; k < 3; k++) {
            lo[k] = p[k] < lo[k] ? p[k] : lo[k];
            hi[k] = p[k] > hi[k] ? p[k] : hi[k];
        }
    }
    for (k = 0; k < 3; k++) {
        c[k] = count > 0u ? 0.5f * (lo[k] + hi[k]) : 0.0f;
    }
    for (i = 0; i < count; i++) {
        const float *p = v[idx[first + i]].pos;
        float dx = p[0] - c[0];
        float dy = p[1] - c[1];
        float dz = p[2] - c[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        rr = d > rr ? d : rr;
    }
    *r = rr * 1.0001f + 1e-6f;
}

uint32_t su_asset_writer_add_mesh(SuAssetWriter *w, uint64_t id, const SuAssetVertex *vertices, uint32_t vertex_count,
                                  const uint32_t *indices, uint32_t index_count, const SuAssetSubmesh *submeshes,
                                  uint32_t submesh_count) {
    SuAssetMeshHeader h;
    SuAssetSubmesh *sm;
    uint8_t *buf;
    size_t size;
    uint32_t i;
    int k;
    if (w == NULL || vertices == NULL || indices == NULL || submeshes == NULL || vertex_count == 0u ||
        index_count < 3u || index_count % 3u != 0u || submesh_count == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < index_count; i++) {
        if (indices[i] >= vertex_count) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    for (i = 0; i < submesh_count; i++) {
        if (submeshes[i].index_count == 0u || submeshes[i].index_count % 3u != 0u ||
            submeshes[i].first_index % 3u != 0u || submeshes[i].first_index > index_count ||
            submeshes[i].index_count > index_count - submeshes[i].first_index) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    for (i = 0; i < vertex_count; i++) {
        if (!finite_n(vertices[i].pos, 14u)) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    size = sizeof h + (size_t)submesh_count * sizeof *sm + (size_t)vertex_count * sizeof *vertices +
           (size_t)index_count * 4u;
    buf = calloc(1, size);
    if (buf == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memset(&h, 0, sizeof h);
    h.vertex_count = vertex_count;
    h.index_count = index_count;
    h.submesh_count = submesh_count;
    for (k = 0; k < 3; k++) {
        h.aabb_min[k] = INFINITY;
        h.aabb_max[k] = -INFINITY;
    }
    for (i = 0; i < vertex_count; i++) {
        for (k = 0; k < 3; k++) {
            h.aabb_min[k] = vertices[i].pos[k] < h.aabb_min[k] ? vertices[i].pos[k] : h.aabb_min[k];
            h.aabb_max[k] = vertices[i].pos[k] > h.aabb_max[k] ? vertices[i].pos[k] : h.aabb_max[k];
        }
    }
    sphere_of(vertices, indices, 0, index_count, h.center, &h.radius);
    memcpy(buf, &h, sizeof h);
    sm = (SuAssetSubmesh *)(buf + sizeof h);
    for (i = 0; i < submesh_count; i++) {
        sm[i] = submeshes[i];
        sphere_of(vertices, indices, sm[i].first_index, sm[i].index_count, sm[i].center, &sm[i].radius);
    }
    memcpy(buf + sizeof h + (size_t)submesh_count * sizeof *sm, vertices, (size_t)vertex_count * sizeof *vertices);
    memcpy(buf + sizeof h + (size_t)submesh_count * sizeof *sm + (size_t)vertex_count * sizeof *vertices, indices,
           (size_t)index_count * 4u);
    return writer_put(w, id, SU_ASSET_MESH, buf, size);
}

uint32_t su_asset_writer_add_texture(SuAssetWriter *w, uint64_t id, uint32_t width, uint32_t height, uint32_t format,
                                     uint32_t flags, uint32_t mip_count, const uint8_t *const *mip_data) {
    SuAssetTextureHeader h;
    SuAssetMip mips[SU_ASSET_MAX_MIPS];
    uint8_t *buf;
    size_t size;
    uint64_t at;
    uint32_t i;
    if (w == NULL || mip_data == NULL || width == 0u || height == 0u || width > 16384u || height > 16384u ||
        su_asset_texture_level_size(format, 1, 1) == 0u || mip_count == 0u || mip_count > max_mips(width, height) ||
        (flags & ~SU_ASSET_TEX_FLAG_NORMAL_MAP) != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    at = sizeof h + (uint64_t)mip_count * sizeof mips[0];
    for (i = 0; i < mip_count; i++) {
        uint32_t lw = width >> i ? width >> i : 1u;
        uint32_t lh = height >> i ? height >> i : 1u;
        if (mip_data[i] == NULL) {
            return SU_E_INVALID_ARGUMENT;
        }
        mips[i].offset = at;
        mips[i].size = su_asset_texture_level_size(format, lw, lh);
        at += mips[i].size;
    }
    size = (size_t)at;
    buf = calloc(1, size);
    if (buf == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memset(&h, 0, sizeof h);
    h.width = width;
    h.height = height;
    h.format = format;
    h.mip_count = mip_count;
    h.flags = flags;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, mips, (size_t)mip_count * sizeof mips[0]);
    for (i = 0; i < mip_count; i++) {
        memcpy(buf + mips[i].offset, mip_data[i], (size_t)mips[i].size);
    }
    return writer_put(w, id, SU_ASSET_TEXTURE, buf, size);
}

uint32_t su_asset_writer_add_material(SuAssetWriter *w, uint64_t id, const SuAssetMaterial *m) {
    uint8_t *buf;
    if (w == NULL || m == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    buf = malloc(sizeof *m);
    if (buf == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(buf, m, sizeof *m);
    return writer_put(w, id, SU_ASSET_MATERIAL, buf, sizeof *m);
}

static void xform_point(const float *m, const float *p, float *out) {
    int k;
    for (k = 0; k < 3; k++) {
        out[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
    }
}

static float max_scale(const float *m) {
    float s = 0.0f;
    int c;
    for (c = 0; c < 3; c++) {
        float l = sqrtf(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]);
        s = l > s ? l : s;
    }
    return s;
}

/* Finds a mesh payload already added to the writer (the model's bounds need it). */
static const SuAssetMeshHeader *writer_mesh(const SuAssetWriter *w, uint64_t id) {
    uint32_t i;
    for (i = 0; i < w->n; i++) {
        if (w->e[i].id == id && w->e[i].type == SU_ASSET_MESH) {
            return (const SuAssetMeshHeader *)w->e[i].data;
        }
    }
    return NULL;
}

uint32_t su_asset_writer_add_model(SuAssetWriter *w, uint64_t id, const SuAssetModelNode *nodes, uint32_t node_count) {
    SuAssetModelHeader h;
    uint8_t *buf;
    size_t size;
    uint32_t i;
    int k;
    if (w == NULL || nodes == NULL || node_count == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&h, 0, sizeof h);
    h.node_count = node_count;
    for (k = 0; k < 3; k++) {
        h.aabb_min[k] = INFINITY;
        h.aabb_max[k] = -INFINITY;
    }
    for (i = 0; i < node_count; i++) {
        const SuAssetMeshHeader *mh;
        float c[3];
        float r;
        if (nodes[i].lod_count == 0u || nodes[i].lod_count > SU_ASSET_MAX_LODS || !finite_n(nodes[i].transform, 16u)) {
            return SU_E_INVALID_ARGUMENT;
        }
        mh = writer_mesh(w, nodes[i].lod_mesh[0]);
        if (mh == NULL) {
            return SU_E_INVALID_ARGUMENT; /* add meshes before the model that places them */
        }
        xform_point(nodes[i].transform, mh->center, c);
        r = mh->radius * max_scale(nodes[i].transform);
        for (k = 0; k < 3; k++) {
            h.aabb_min[k] = c[k] - r < h.aabb_min[k] ? c[k] - r : h.aabb_min[k];
            h.aabb_max[k] = c[k] + r > h.aabb_max[k] ? c[k] + r : h.aabb_max[k];
        }
    }
    {
        float rr = 0.0f;
        for (k = 0; k < 3; k++) {
            h.center[k] = 0.5f * (h.aabb_min[k] + h.aabb_max[k]);
        }
        for (i = 0; i < node_count; i++) {
            const SuAssetMeshHeader *mh = writer_mesh(w, nodes[i].lod_mesh[0]);
            float c[3];
            float d;
            xform_point(nodes[i].transform, mh->center, c);
            d = sqrtf((c[0] - h.center[0]) * (c[0] - h.center[0]) + (c[1] - h.center[1]) * (c[1] - h.center[1]) +
                      (c[2] - h.center[2]) * (c[2] - h.center[2])) +
                mh->radius * max_scale(nodes[i].transform);
            rr = d > rr ? d : rr;
        }
        h.radius = rr * 1.0001f + 1e-6f;
    }
    size = sizeof h + (size_t)node_count * sizeof *nodes;
    buf = malloc(size);
    if (buf == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, nodes, (size_t)node_count * sizeof *nodes);
    return writer_put(w, id, SU_ASSET_MODEL, buf, size);
}

static int cmp_entry(const void *a, const void *b) {
    const WEntry *x = a;
    const WEntry *y = b;
    return x->id < y->id ? -1 : x->id > y->id;
}

uint32_t su_asset_writer_finish(SuAssetWriter *w, uint8_t **bytes, size_t *len) {
    uint64_t at;
    uint8_t *out;
    uint32_t i;
    SuSha256 s;
    if (w == NULL || bytes == NULL || len == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!host_is_little_endian()) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    qsort(w->e, w->n, sizeof *w->e, cmp_entry);
    at = HEADER_BYTES + (uint64_t)w->n * ENTRY_BYTES;
    for (i = 0; i < w->n; i++) {
        at = (at + 15u) & ~(uint64_t)15u;
        at += w->e[i].size;
    }
    out = calloc(1, (size_t)at);
    if (out == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(out, MAGIC, 8);
    wr32(out + 8, SU_ASSET_PAK_VERSION);
    wr32(out + 12, w->n);
    wr64(out + 16, at);
    at = HEADER_BYTES + (uint64_t)w->n * ENTRY_BYTES;
    for (i = 0; i < w->n; i++) {
        uint8_t *e = out + HEADER_BYTES + (size_t)i * ENTRY_BYTES;
        at = (at + 15u) & ~(uint64_t)15u;
        wr64(e, w->e[i].id);
        wr32(e + 8, w->e[i].type);
        wr32(e + 12, 1u);
        wr64(e + 16, at);
        wr64(e + 24, w->e[i].size);
        su_sha256_init(&s);
        su_sha256_update(&s, w->e[i].data, w->e[i].size);
        su_sha256_final(&s, e + 32);
        memcpy(out + at, w->e[i].data, w->e[i].size);
        at += w->e[i].size;
    }
    su_sha256_init(&s);
    su_sha256_update(&s, out + HEADER_BYTES, (size_t)at - HEADER_BYTES);
    su_sha256_final(&s, out + 24);
    *bytes = out;
    *len = (size_t)at;
    writer_clear(w);
    return SU_OK;
}

/* ---------- reader ---------- */

struct SuAssetPak {
    uint8_t *owned;
    const uint8_t *bytes;
    size_t len;
    uint32_t count;
    SuAssetEntry *entries;
};

void su_asset_pak_close(SuAssetPak *pak) {
    if (pak != NULL) {
        free(pak->owned);
        free(pak->entries);
        free(pak);
    }
}

static int find_index(const SuAssetPak *p, uint64_t id) {
    uint32_t lo = 0;
    uint32_t hi = p->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (p->entries[mid].id < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < p->count && p->entries[lo].id == id ? (int)lo : -1;
}

static int ref_ok(const SuAssetPak *p, uint64_t id, uint32_t type) {
    int i;
    if (id == 0u) {
        return 1;
    }
    i = find_index(p, id);
    return i >= 0 && p->entries[i].type == type;
}

static const uint8_t *payload(const SuAssetPak *p, uint32_t i) {
    return p->bytes + p->entries[i].offset;
}

static uint32_t check_mesh(const SuAssetPak *p, uint32_t ei, char *msg) {
    const uint8_t *b = payload(p, ei);
    uint64_t size = p->entries[ei].size;
    SuAssetMeshHeader h;
    const SuAssetSubmesh *sm;
    const SuAssetVertex *v;
    const uint32_t *idx;
    uint64_t want;
    uint32_t i;
    if (size < sizeof h) {
        msgf(msg, "mesh %016llx: payload too small", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    memcpy(&h, b, sizeof h);
    want = sizeof h + (uint64_t)h.submesh_count * sizeof *sm + (uint64_t)h.vertex_count * sizeof *v +
           (uint64_t)h.index_count * 4u;
    if (h.vertex_count == 0u || h.index_count < 3u || h.index_count % 3u != 0u || h.submesh_count == 0u ||
        h.flags != 0u || want != size || !finite3(h.aabb_min) || !finite3(h.aabb_max) || !finite3(h.center) ||
        !isfinite(h.radius) || h.radius < 0.0f) {
        msgf(msg, "mesh %016llx: inconsistent header", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    sm = (const SuAssetSubmesh *)(b + sizeof h);
    v = (const SuAssetVertex *)(b + sizeof h + (size_t)h.submesh_count * sizeof *sm);
    idx = (const uint32_t *)(b + sizeof h + (size_t)h.submesh_count * sizeof *sm + (size_t)h.vertex_count * sizeof *v);
    for (i = 0; i < h.submesh_count; i++) {
        if (sm[i].index_count == 0u || sm[i].index_count % 3u != 0u || sm[i].first_index % 3u != 0u ||
            sm[i].first_index > h.index_count || sm[i].index_count > h.index_count - sm[i].first_index ||
            !finite3(sm[i].center) || !isfinite(sm[i].radius) || !ref_ok(p, sm[i].material, SU_ASSET_MATERIAL)) {
            msgf(msg, "mesh %016llx: submesh %u invalid (range or material reference)",
                 (unsigned long long)p->entries[ei].id, (unsigned)i);
            return SU_E_PARSE;
        }
    }
    for (i = 0; i < h.vertex_count; i++) {
        if (!finite_n(v[i].pos, 14u)) {
            msgf(msg, "mesh %016llx: vertex %u is not finite", (unsigned long long)p->entries[ei].id, (unsigned)i);
            return SU_E_PARSE;
        }
    }
    for (i = 0; i < h.index_count; i++) {
        if (idx[i] >= h.vertex_count) {
            msgf(msg, "mesh %016llx: index %u out of range", (unsigned long long)p->entries[ei].id, (unsigned)i);
            return SU_E_PARSE;
        }
    }
    return SU_OK;
}

static uint32_t check_texture(const SuAssetPak *p, uint32_t ei, char *msg) {
    const uint8_t *b = payload(p, ei);
    uint64_t size = p->entries[ei].size;
    SuAssetTextureHeader h;
    uint64_t at;
    uint32_t i;
    if (size < sizeof h) {
        msgf(msg, "texture %016llx: payload too small", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    memcpy(&h, b, sizeof h);
    if (h.width == 0u || h.height == 0u || h.width > 16384u || h.height > 16384u ||
        su_asset_texture_level_size(h.format, 1, 1) == 0u || h.mip_count == 0u ||
        h.mip_count > max_mips(h.width, h.height) || (h.flags & ~SU_ASSET_TEX_FLAG_NORMAL_MAP) != 0u ||
        size < sizeof h + (uint64_t)h.mip_count * sizeof(SuAssetMip)) {
        msgf(msg, "texture %016llx: inconsistent header", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    at = sizeof h + (uint64_t)h.mip_count * sizeof(SuAssetMip);
    for (i = 0; i < h.mip_count; i++) {
        SuAssetMip m;
        uint32_t lw = h.width >> i ? h.width >> i : 1u;
        uint32_t lh = h.height >> i ? h.height >> i : 1u;
        memcpy(&m, b + sizeof h + (size_t)i * sizeof m, sizeof m);
        if (m.offset != at || m.size != su_asset_texture_level_size(h.format, lw, lh) || m.size > size - at) {
            msgf(msg, "texture %016llx: mip %u has a wrong offset or size", (unsigned long long)p->entries[ei].id,
                 (unsigned)i);
            return SU_E_PARSE;
        }
        at += m.size;
    }
    if (at != size) {
        msgf(msg, "texture %016llx: trailing bytes", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    return SU_OK;
}

static uint32_t check_material(const SuAssetPak *p, uint32_t ei, char *msg) {
    SuAssetMaterial m;
    uint32_t i;
    if (p->entries[ei].size != sizeof m) {
        msgf(msg, "material %016llx: wrong size", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    memcpy(&m, payload(p, ei), sizeof m);
    if (!finite_n(m.base_color, 13u) || m.alpha_mode > SU_ASSET_ALPHA_BLEND || m.double_sided > 1u || m.unlit > 1u) {
        msgf(msg, "material %016llx: invalid factors or modes", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    for (i = 0; i < SU_ASSET_SLOT_COUNT; i++) {
        uint32_t t = m.tex_info[i];
        if (!ref_ok(p, m.texture[i], SU_ASSET_TEXTURE) || (t & 0xFFu) > 1u || ((t >> 8) & 0xFu) > 2u ||
            ((t >> 12) & 0xFu) > 2u || (t >> 16) != 0u) {
            msgf(msg, "material %016llx: texture slot %u invalid", (unsigned long long)p->entries[ei].id, (unsigned)i);
            return SU_E_PARSE;
        }
    }
    return SU_OK;
}

static uint32_t check_model(const SuAssetPak *p, uint32_t ei, char *msg) {
    const uint8_t *b = payload(p, ei);
    SuAssetModelHeader h;
    uint32_t i;
    uint32_t k;
    if (p->entries[ei].size < sizeof h) {
        msgf(msg, "model %016llx: payload too small", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    memcpy(&h, b, sizeof h);
    if (h.node_count == 0u || p->entries[ei].size != sizeof h + (uint64_t)h.node_count * sizeof(SuAssetModelNode) ||
        !finite3(h.center) || !isfinite(h.radius) || !finite3(h.aabb_min) || !finite3(h.aabb_max)) {
        msgf(msg, "model %016llx: inconsistent header", (unsigned long long)p->entries[ei].id);
        return SU_E_PARSE;
    }
    for (i = 0; i < h.node_count; i++) {
        SuAssetModelNode n;
        memcpy(&n, b + sizeof h + (size_t)i * sizeof n, sizeof n);
        if (n.lod_count == 0u || n.lod_count > SU_ASSET_MAX_LODS || !finite_n(n.transform, 16u)) {
            msgf(msg, "model %016llx: node %u invalid", (unsigned long long)p->entries[ei].id, (unsigned)i);
            return SU_E_PARSE;
        }
        for (k = 0; k < SU_ASSET_MAX_LODS; k++) {
            int used = k < n.lod_count;
            if ((used && (n.lod_mesh[k] == 0u || !ref_ok(p, n.lod_mesh[k], SU_ASSET_MESH))) ||
                (!used && n.lod_mesh[k] != 0u) ||
                (used && k > 0u && !(n.lod_screen[k] > 0.0f && n.lod_screen[k] < n.lod_screen[k - 1u]))) {
                msgf(msg, "model %016llx: node %u LOD %u invalid", (unsigned long long)p->entries[ei].id, (unsigned)i,
                     (unsigned)k);
                return SU_E_PARSE;
            }
        }
    }
    return SU_OK;
}

uint32_t su_asset_pak_open(const void *bytes, size_t len, uint32_t flags, SuAssetPak **out, char message[256]) {
    SuAssetPak *p;
    const uint8_t *b;
    uint64_t prev_end;
    uint32_t i;
    uint32_t st = SU_OK;
    SuSha256 s;
    uint8_t d[32];
    if (message != NULL) {
        message[0] = '\0';
    }
    if (out == NULL || (bytes == NULL && len > 0u) || (flags & ~(SU_ASSET_PAK_BORROW | SU_ASSET_PAK_VERIFY_ENTRIES)) != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (!host_is_little_endian()) {
        msgf(message, "packs are little-endian; this host is not");
        return SU_E_UNSUPPORTED_METHOD;
    }
    if ((flags & SU_ASSET_PAK_BORROW) != 0u && ((uintptr_t)bytes & 15u) != 0u) {
        msgf(message, "borrowed pack bytes must be 16-byte aligned");
        return SU_E_INVALID_ARGUMENT;
    }
    p = calloc(1, sizeof *p);
    if (p == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    if ((flags & SU_ASSET_PAK_BORROW) != 0u) {
        p->bytes = bytes;
    } else {
        p->owned = malloc(len > 0u ? len : 1u); /* malloc is 16-byte aligned on the supported 64-bit targets */
        if (p->owned == NULL || ((uintptr_t)p->owned & 15u) != 0u) {
            su_asset_pak_close(p);
            return SU_E_RESOURCE_LIMIT;
        }
        if (len > 0u) {
            memcpy(p->owned, bytes, len);
        }
        p->bytes = p->owned;
    }
    p->len = len;
    b = p->bytes;
    if (len < HEADER_BYTES || memcmp(b, MAGIC, 8) != 0) {
        msgf(message, "not a Sutekh asset pack (magic)");
        su_asset_pak_close(p);
        return SU_E_PARSE;
    }
    if (rd32(b + 8) != SU_ASSET_PAK_VERSION) {
        msgf(message, "pack version %u; this build reads %u", (unsigned)rd32(b + 8), SU_ASSET_PAK_VERSION);
        su_asset_pak_close(p);
        return SU_E_PARSE;
    }
    p->count = rd32(b + 12);
    if (rd64(b + 16) != (uint64_t)len || rd64(b + 56) != 0u || p->count > MAX_ENTRIES ||
        (uint64_t)HEADER_BYTES + (uint64_t)p->count * ENTRY_BYTES > (uint64_t)len) {
        msgf(message, "pack header: size or entry count inconsistent with %llu bytes", (unsigned long long)len);
        su_asset_pak_close(p);
        return SU_E_PARSE;
    }
    su_sha256_init(&s);
    su_sha256_update(&s, b + HEADER_BYTES, len - HEADER_BYTES);
    su_sha256_final(&s, d);
    if (memcmp(d, b + 24, 32) != 0) {
        msgf(message, "pack content hash differs from the header");
        su_asset_pak_close(p);
        return SU_E_PARSE;
    }
    p->entries = calloc(p->count > 0u ? p->count : 1u, sizeof *p->entries);
    if (p->entries == NULL) {
        su_asset_pak_close(p);
        return SU_E_RESOURCE_LIMIT;
    }
    prev_end = HEADER_BYTES + (uint64_t)p->count * ENTRY_BYTES;
    for (i = 0; i < p->count; i++) {
        const uint8_t *e = b + HEADER_BYTES + (size_t)i * ENTRY_BYTES;
        SuAssetEntry *en = &p->entries[i];
        en->id = rd64(e);
        en->type = rd32(e + 8);
        en->type_version = rd32(e + 12);
        en->offset = rd64(e + 16);
        en->size = rd64(e + 24);
        memcpy(en->sha256, e + 32, 32);
        if (en->id == 0u || (i > 0u && en->id <= p->entries[i - 1u].id) || en->type < SU_ASSET_MESH ||
            en->type > SU_ASSET_MODEL || en->type_version != 1u || (en->offset & 15u) != 0u || en->offset < prev_end ||
            en->offset > (uint64_t)len || en->size > (uint64_t)len - en->offset) {
            msgf(message, "entry %u: id order, type, version or byte range invalid", (unsigned)i);
            su_asset_pak_close(p);
            return SU_E_PARSE;
        }
        prev_end = en->offset + en->size;
        if ((flags & SU_ASSET_PAK_VERIFY_ENTRIES) == 0u) {
            continue;
        }
        su_sha256_init(&s);
        su_sha256_update(&s, b + en->offset, (size_t)en->size);
        su_sha256_final(&s, d);
        if (memcmp(d, en->sha256, 32) != 0) {
            msgf(message, "entry %u (%016llx): payload hash differs", (unsigned)i, (unsigned long long)en->id);
            su_asset_pak_close(p);
            return SU_E_PARSE;
        }
    }
    for (i = 0; i < p->count && st == SU_OK; i++) {
        switch (p->entries[i].type) {
        case SU_ASSET_MESH:
            st = check_mesh(p, i, message);
            break;
        case SU_ASSET_TEXTURE:
            st = check_texture(p, i, message);
            break;
        case SU_ASSET_MATERIAL:
            st = check_material(p, i, message);
            break;
        default:
            st = check_model(p, i, message);
            break;
        }
    }
    if (st != SU_OK) {
        su_asset_pak_close(p);
        return st;
    }
    *out = p;
    return SU_OK;
}

uint32_t su_asset_pak_open_file(const char *path, SuAssetPak **out, char message[256]) {
    FILE *f;
    long n;
    uint8_t *buf;
    SuAssetPak *p;
    uint32_t st;
    if (path == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    f = fopen(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        if (f != NULL) {
            fclose(f);
        }
        msgf(message, "cannot read %s", path);
        return SU_E_IO;
    }
    buf = malloc((size_t)n > 0u ? (size_t)n : 1u);
    if (buf == NULL) {
        fclose(f);
        return SU_E_RESOURCE_LIMIT;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        msgf(message, "short read from %s", path);
        return SU_E_IO;
    }
    fclose(f);
    if (((uintptr_t)buf & 15u) != 0u) {
        st = su_asset_pak_open(buf, (size_t)n, 0, out, message);
        free(buf);
        return st;
    }
    st = su_asset_pak_open(buf, (size_t)n, SU_ASSET_PAK_BORROW, &p, message);
    if (st != SU_OK) {
        free(buf);
        return st;
    }
    p->owned = buf; /* adopt the file buffer */
    *out = p;
    return SU_OK;
}

uint32_t su_asset_pak_entry_count(const SuAssetPak *pak) {
    return pak != NULL ? pak->count : 0u;
}

uint32_t su_asset_pak_entry(const SuAssetPak *pak, uint32_t index, SuAssetEntry *out) {
    if (pak == NULL || out == NULL || index >= pak->count) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = pak->entries[index];
    return SU_OK;
}

uint32_t su_asset_pak_find(const SuAssetPak *pak, uint64_t id, uint32_t type, uint32_t *index) {
    int i;
    if (pak == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    i = find_index(pak, id);
    if (i < 0 || (type != 0u && pak->entries[i].type != type)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (index != NULL) {
        *index = (uint32_t)i;
    }
    return SU_OK;
}

void su_asset_pak_digest(const SuAssetPak *pak, uint8_t sha256[32]) {
    if (pak != NULL && sha256 != NULL) {
        memcpy(sha256, pak->bytes + 24, 32);
    }
}

const uint8_t *su_asset_pak_bytes(const SuAssetPak *pak, size_t *len) {
    if (pak == NULL) {
        return NULL;
    }
    if (len != NULL) {
        *len = pak->len;
    }
    return pak->bytes;
}

uint32_t su_asset_pak_mesh(const SuAssetPak *pak, uint64_t id, SuAssetMeshView *out) {
    uint32_t i;
    const uint8_t *b;
    if (out == NULL || su_asset_pak_find(pak, id, SU_ASSET_MESH, &i) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    b = payload(pak, i);
    out->id = id;
    out->header = (const SuAssetMeshHeader *)b;
    out->submeshes = (const SuAssetSubmesh *)(b + sizeof(SuAssetMeshHeader));
    out->vertices = (const SuAssetVertex *)(b + sizeof(SuAssetMeshHeader) +
                                            (size_t)out->header->submesh_count * sizeof(SuAssetSubmesh));
    out->indices = (const uint32_t *)(out->vertices + out->header->vertex_count);
    return SU_OK;
}

uint32_t su_asset_pak_texture(const SuAssetPak *pak, uint64_t id, SuAssetTextureView *out) {
    uint32_t i;
    uint32_t m;
    const uint8_t *b;
    if (out == NULL || su_asset_pak_find(pak, id, SU_ASSET_TEXTURE, &i) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    b = payload(pak, i);
    memset(out, 0, sizeof *out);
    out->id = id;
    out->header = (const SuAssetTextureHeader *)b;
    for (m = 0; m < out->header->mip_count; m++) {
        SuAssetMip mip;
        memcpy(&mip, b + sizeof(SuAssetTextureHeader) + (size_t)m * sizeof mip, sizeof mip);
        out->mip_data[m] = b + mip.offset;
        out->mip_size[m] = mip.size;
    }
    return SU_OK;
}

uint32_t su_asset_pak_material(const SuAssetPak *pak, uint64_t id, SuAssetMaterial *out) {
    uint32_t i;
    if (out == NULL || su_asset_pak_find(pak, id, SU_ASSET_MATERIAL, &i) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    memcpy(out, payload(pak, i), sizeof *out);
    return SU_OK;
}

uint32_t su_asset_pak_model(const SuAssetPak *pak, uint64_t id, SuAssetModelView *out) {
    uint32_t i;
    const uint8_t *b;
    if (out == NULL || su_asset_pak_find(pak, id, SU_ASSET_MODEL, &i) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    b = payload(pak, i);
    out->id = id;
    out->header = (const SuAssetModelHeader *)b;
    out->nodes = (const SuAssetModelNode *)(b + sizeof(SuAssetModelHeader));
    return SU_OK;
}

/* ---------- background loader ---------- */

#define LOADER_CAP 64u

typedef struct LoadReq {
    char *path;
    uint64_t user;
} LoadReq;

struct SuAssetLoader {
    SuAssetCookFn cook;
    SuMutex mu;
    SuCond cv;
    SuThread thread;
    LoadReq queue[LOADER_CAP];
    uint32_t q_head;
    uint32_t q_count;
    SuAssetLoadResult *done[LOADER_CAP];
    uint32_t d_head;
    uint32_t d_count;
    uint32_t busy;
    int stop;
};

static double wall_ms(void) {
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s);
    size_t b = strlen(suffix);
    size_t i;
    if (a < b) {
        return 0;
    }
    for (i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != suffix[i]) {
            return 0;
        }
    }
    return 1;
}

static void load_one(SuAssetLoader *l, const LoadReq *rq, SuAssetLoadResult *res) {
    double t0 = wall_ms();
    memset(res, 0, sizeof *res);
    res->user = rq->user;
    if (ends_with(rq->path, ".gltf") || ends_with(rq->path, ".glb")) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (l->cook == NULL) {
            res->status = SU_E_UNSUPPORTED_METHOD;
            msgf(res->message, "%s: no glTF importer in this build", rq->path);
        } else {
            res->status = l->cook(rq->path, NULL, &bytes, &len, NULL, res->message);
            if (res->status == SU_OK) {
                res->status = su_asset_pak_open(bytes, len, 0, &res->pak, res->message);
            }
            free(bytes);
        }
    } else {
        res->status = su_asset_pak_open_file(rq->path, &res->pak, res->message);
    }
    res->load_ms = wall_ms() - t0;
}

static void *loader_main(void *arg) {
    SuAssetLoader *l = arg;
    su_mutex_lock(&l->mu);
    for (;;) {
        LoadReq rq;
        SuAssetLoadResult *res;
        while (!l->stop && l->q_count == 0u) {
            su_cond_wait(&l->cv, &l->mu);
        }
        if (l->stop) {
            break;
        }
        rq = l->queue[l->q_head];
        l->q_head = (l->q_head + 1u) % LOADER_CAP;
        l->q_count--;
        l->busy = 1;
        su_mutex_unlock(&l->mu);
        res = malloc(sizeof *res);
        if (res != NULL) {
            load_one(l, &rq, res);
        }
        free(rq.path);
        su_mutex_lock(&l->mu);
        l->busy = 0;
        if (res != NULL) {
            l->done[(l->d_head + l->d_count) % LOADER_CAP] = res;
            l->d_count++;
        }
    }
    su_mutex_unlock(&l->mu);
    return NULL;
}

uint32_t su_asset_loader_create(SuAssetCookFn cook, SuAssetLoader **out) {
    SuAssetLoader *l;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    l = calloc(1, sizeof *l);
    if (l == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    l->cook = cook;
    if (su_mutex_init(&l->mu) != 0) {
        free(l);
        return SU_E_RESOURCE_LIMIT;
    }
    if (su_cond_init(&l->cv) != 0) {
        su_mutex_destroy(&l->mu);
        free(l);
        return SU_E_RESOURCE_LIMIT;
    }
    if (su_thread_create(&l->thread, loader_main, l) != 0) {
        su_cond_destroy(&l->cv);
        su_mutex_destroy(&l->mu);
        free(l);
        return SU_E_RESOURCE_LIMIT;
    }
    *out = l;
    return SU_OK;
}

void su_asset_loader_destroy(SuAssetLoader *l) {
    uint32_t i;
    if (l == NULL) {
        return;
    }
    su_mutex_lock(&l->mu);
    l->stop = 1;
    su_cond_broadcast(&l->cv);
    su_mutex_unlock(&l->mu);
    (void)su_thread_join(l->thread);
    for (i = 0; i < l->q_count; i++) {
        free(l->queue[(l->q_head + i) % LOADER_CAP].path);
    }
    for (i = 0; i < l->d_count; i++) {
        SuAssetLoadResult *r = l->done[(l->d_head + i) % LOADER_CAP];
        su_asset_pak_close(r->pak);
        free(r);
    }
    su_cond_destroy(&l->cv);
    su_mutex_destroy(&l->mu);
    free(l);
}

uint32_t su_asset_loader_request(SuAssetLoader *l, const char *path, uint64_t user) {
    char *copy;
    size_t n;
    uint32_t st = SU_OK;
    if (l == NULL || path == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    n = strlen(path);
    copy = malloc(n + 1u);
    if (copy == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(copy, path, n + 1u);
    su_mutex_lock(&l->mu);
    if (l->q_count + l->busy + l->d_count >= LOADER_CAP) {
        st = SU_E_RESOURCE_LIMIT;
    } else {
        l->queue[(l->q_head + l->q_count) % LOADER_CAP].path = copy;
        l->queue[(l->q_head + l->q_count) % LOADER_CAP].user = user;
        l->q_count++;
        su_cond_signal(&l->cv);
    }
    su_mutex_unlock(&l->mu);
    if (st != SU_OK) {
        free(copy);
    }
    return st;
}

int su_asset_loader_poll(SuAssetLoader *l, SuAssetLoadResult *out) {
    SuAssetLoadResult *r = NULL;
    if (l == NULL || out == NULL) {
        return 0;
    }
    su_mutex_lock(&l->mu);
    if (l->d_count > 0u) {
        r = l->done[l->d_head];
        l->d_head = (l->d_head + 1u) % LOADER_CAP;
        l->d_count--;
    }
    su_mutex_unlock(&l->mu);
    if (r == NULL) {
        return 0;
    }
    *out = *r;
    free(r);
    return 1;
}

uint32_t su_asset_loader_pending(SuAssetLoader *l) {
    uint32_t n;
    if (l == NULL) {
        return 0;
    }
    su_mutex_lock(&l->mu);
    n = l->q_count + l->busy;
    su_mutex_unlock(&l->mu);
    return n;
}
