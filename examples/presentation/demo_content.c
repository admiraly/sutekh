/* Procedural content for the presentation demo, cooked into a pack with the asset writer (sutekh/asset.h): a ship
 * hull with an engine glow, a gate ring with light strips, a station (hub, ring, spokes, window band) and an
 * asteroid with two LODs. Everything is generated here; no external art. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "demo_content.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/render.h"

#define PI_F 3.14159265f

uint64_t demo_id(const char *name) {
    char b[128];
    size_t n = strlen(name);
    if (n > 100u) {
        n = 100u;
    }
    memcpy(b, "demo/", 5);
    memcpy(b + 5, name, n);
    b[5 + n] = '\0';
    return su_asset_id(b);
}

static void perp(const float *n, float *t) {
    float a[3] = {fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
    float l;
    t[0] = n[1] * a[2] - n[2] * a[1];
    t[1] = n[2] * a[0] - n[0] * a[2];
    t[2] = n[0] * a[1] - n[1] * a[0];
    l = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    t[0] /= l;
    t[1] /= l;
    t[2] /= l;
    t[3] = 1.0f;
}

/* A built-in procedural mesh (sutekh/render.h) as asset vertices. */
static uint32_t builtin(uint32_t mesh, SuAssetVertex **v, uint32_t **idx, uint32_t *nv, uint32_t *ni) {
    SuMeshVertex *mv;
    uint16_t *mi;
    uint32_t i;
    (void)su_mesh_build(mesh, NULL, 0, nv, NULL, 0, ni);
    mv = malloc(*nv * sizeof *mv);
    mi = malloc(*ni * sizeof *mi);
    *v = calloc(*nv, sizeof **v);
    *idx = malloc(*ni * sizeof **idx);
    if (mv == NULL || mi == NULL || *v == NULL || *idx == NULL ||
        su_mesh_build(mesh, mv, *nv, nv, mi, *ni, ni) != SU_OK) {
        free(mv);
        free(mi);
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < *nv; i++) {
        memcpy((*v)[i].pos, mv[i].pos, sizeof mv[i].pos);
        memcpy((*v)[i].normal, mv[i].normal, sizeof mv[i].normal);
        perp(mv[i].normal, (*v)[i].tangent);
    }
    for (i = 0; i < *ni; i++) {
        (*idx)[i] = mi[i];
    }
    free(mv);
    free(mi);
    return SU_OK;
}

/* Torus around +z (in the xy plane): ring radius big, tube radius small. */
static uint32_t torus(float big, float small, uint32_t seg, uint32_t sides, SuAssetVertex **v, uint32_t **idx,
                      uint32_t *nv, uint32_t *ni) {
    uint32_t i;
    uint32_t j;
    uint32_t k = 0;
    *nv = (seg + 1u) * (sides + 1u);
    *ni = seg * sides * 6u;
    *v = calloc(*nv, sizeof **v);
    *idx = malloc(*ni * sizeof **idx);
    if (*v == NULL || *idx == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i <= seg; i++) {
        float a = 2.0f * PI_F * (float)i / (float)seg;
        for (j = 0; j <= sides; j++) {
            float b = 2.0f * PI_F * (float)j / (float)sides;
            SuAssetVertex *x = &(*v)[i * (sides + 1u) + j];
            float n[3] = {cosf(a) * cosf(b), sinf(a) * cosf(b), sinf(b)};
            x->pos[0] = cosf(a) * big + n[0] * small;
            x->pos[1] = sinf(a) * big + n[1] * small;
            x->pos[2] = n[2] * small;
            memcpy(x->normal, n, sizeof n);
            x->tangent[0] = -sinf(a);
            x->tangent[1] = cosf(a);
            x->tangent[2] = 0.0f;
            x->tangent[3] = 1.0f;
            x->uv0[0] = (float)i / (float)seg * 8.0f;
            x->uv0[1] = (float)j / (float)sides;
        }
    }
    for (i = 0; i < seg; i++) {
        for (j = 0; j < sides; j++) {
            uint32_t a0 = i * (sides + 1u) + j;
            uint32_t a1 = (i + 1u) * (sides + 1u) + j;
            (*idx)[k++] = a0;
            (*idx)[k++] = a1;
            (*idx)[k++] = a1 + 1u;
            (*idx)[k++] = a0;
            (*idx)[k++] = a1 + 1u;
            (*idx)[k++] = a0 + 1u;
        }
    }
    return SU_OK;
}

/* Lumpy rock: radial noise on a built-in mesh, normals recomputed from the faces. */
static void lumpy(SuAssetVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, float amount) {
    uint32_t i;
    for (i = 0; i < nv; i++) {
        float *p = v[i].pos;
        float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        float d[3];
        float k;
        if (l < 1e-6f) {
            continue;
        }
        d[0] = p[0] / l;
        d[1] = p[1] / l;
        d[2] = p[2] / l;
        k = 1.0f + amount * (sinf(3.1f * d[0] + 1.7f) * sinf(4.3f * d[1] + 0.3f) * sinf(5.7f * d[2] + 2.1f) +
                             0.5f * sinf(9.0f * d[0] + 7.0f * d[2]));
        p[0] = d[0] * 0.5f * k;
        p[1] = d[1] * 0.5f * k;
        p[2] = d[2] * 0.5f * k;
        v[i].normal[0] = v[i].normal[1] = v[i].normal[2] = 0.0f;
    }
    for (i = 0; i + 2u < ni; i += 3u) {
        float *a = v[idx[i]].pos;
        float *b = v[idx[i + 1u]].pos;
        float *c = v[idx[i + 2u]].pos;
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        int j;
        int q;
        for (j = 0; j < 3; j++) {
            for (q = 0; q < 3; q++) {
                v[idx[i + (uint32_t)j]].normal[q] += n[q];
            }
        }
    }
    for (i = 0; i < nv; i++) {
        float *n = v[i].normal;
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0.0f) {
            n[0] /= l;
            n[1] /= l;
            n[2] /= l;
        } else {
            n[1] = 1.0f;
        }
        perp(n, v[i].tangent);
    }
}

static uint32_t add_mesh(SuAssetWriter *w, const char *name, SuAssetVertex *v, uint32_t nv, uint32_t *idx, uint32_t ni,
                         const char *material) {
    SuAssetSubmesh sm;
    uint32_t st;
    memset(&sm, 0, sizeof sm);
    sm.index_count = ni;
    sm.material = material != NULL ? demo_id(material) : 0u;
    st = su_asset_writer_add_mesh(w, demo_id(name), v, nv, idx, ni, &sm, 1);
    free(v);
    free(idx);
    return st;
}

static void mat(SuAssetWriter *w, const char *name, float r, float g, float b, float metal, float rough, float er,
                float eg, float eb, float strength) {
    SuAssetMaterial m;
    su_asset_material_default(&m);
    m.base_color[0] = r;
    m.base_color[1] = g;
    m.base_color[2] = b;
    m.metallic = metal;
    m.roughness = rough;
    m.emissive[0] = er;
    m.emissive[1] = eg;
    m.emissive[2] = eb;
    m.emissive_strength = strength;
    (void)su_asset_writer_add_material(w, demo_id(name), &m);
}

/* Node transform: scale (sx, sy, sz), then rotation about +y by ry and about +z by rz, then translation. */
static void xform(SuAssetModelNode *n, const char *mesh, float tx, float ty, float tz, float sx, float sy, float sz,
                  float ry, float rz) {
    float cy = cosf(ry);
    float sny = sinf(ry);
    float cz = cosf(rz);
    float snz = sinf(rz);
    /* R = Rz * Ry */
    float r[9] = {cz * cy, snz * cy, -sny, -snz, cz, 0.0f, cz * sny, snz * sny, cy}; /* column-major */
    memset(n, 0, sizeof *n);
    n->transform[0] = r[0] * sx;
    n->transform[1] = r[1] * sx;
    n->transform[2] = r[2] * sx;
    n->transform[4] = r[3] * sy;
    n->transform[5] = r[4] * sy;
    n->transform[6] = r[5] * sy;
    n->transform[8] = r[6] * sz;
    n->transform[9] = r[7] * sz;
    n->transform[10] = r[8] * sz;
    n->transform[12] = tx;
    n->transform[13] = ty;
    n->transform[14] = tz;
    n->transform[15] = 1.0f;
    n->lod_mesh[0] = demo_id(mesh);
    n->lod_screen[0] = 1.0f;
    n->lod_count = 1;
}

/* The gate ring as a layered paint material: panel albedo, a paint mask (red bands, amber bands, every other panel
 * plain), a glow map along the tube and a dirt map; all generated here, nothing loaded. */
static uint32_t paint_gate(SuAssetWriter *w) {
    enum { N = 16 };
    static uint8_t alb[N * N * 4];
    static uint8_t mask[N * N * 4];
    static uint8_t glow[N * N * 4];
    static uint8_t dirt[N * N * 4];
    const uint8_t *mips[1];
    SuAssetMaterial m;
    SuAssetPaint p;
    uint32_t x;
    uint32_t y;
    uint32_t st;
    for (y = 0; y < N; y++) {
        for (x = 0; x < N; x++) {
            uint8_t *a = &alb[(y * N + x) * 4u];
            uint8_t *k = &mask[(y * N + x) * 4u];
            uint8_t *g = &glow[(y * N + x) * 4u];
            uint8_t *d = &dirt[(y * N + x) * 4u];
            int seam = x % 8u == 0u || y % 8u == 0u;
            a[0] = a[1] = a[2] = seam ? 60 : 170;
            a[3] = 255;
            k[0] = (x % 8u) < 3u && !seam ? 255 : 0;           /* band 0 */
            k[1] = (x % 8u) >= 5u && !seam ? 255 : 0;          /* band 1 */
            k[2] = k[3] = 0;
            g[0] = g[1] = g[2] = y % 8u == 4u ? 255 : 0;       /* a glowing line around the tube */
            g[3] = 255;
            d[0] = (uint8_t)(((x * 37u + y * 91u) % 17u) * 15u);
            d[1] = d[2] = 0;
            d[3] = 255;
        }
    }
    mips[0] = alb;
    st = su_asset_writer_add_texture(w, demo_id("tex/gate_albedo"), N, N, SU_TEX_RGBA8_SRGB, 0, 1, mips);
    mips[0] = mask;
    st = st == SU_OK ? su_asset_writer_add_texture(w, demo_id("tex/gate_mask"), N, N, SU_TEX_RGBA8_UNORM, 0, 1, mips) : st;
    mips[0] = glow;
    st = st == SU_OK ? su_asset_writer_add_texture(w, demo_id("tex/gate_glow"), N, N, SU_TEX_RGBA8_SRGB, 0, 1, mips) : st;
    mips[0] = dirt;
    st = st == SU_OK ? su_asset_writer_add_texture(w, demo_id("tex/gate_dirt"), N, N, SU_TEX_RGBA8_UNORM, 0, 1, mips) : st;
    su_asset_material_default(&m);
    m.metallic = 1.0f;
    m.roughness = 0.5f;
    m.emissive[0] = m.emissive[1] = m.emissive[2] = 1.0f;
    m.emissive_strength = 2.0f;
    su_asset_paint_default(&p);
    p.texture[SU_PAINT_SLOT_ALBEDO] = demo_id("tex/gate_albedo");
    p.texture[SU_PAINT_SLOT_MASK] = demo_id("tex/gate_mask");
    p.texture[SU_PAINT_SLOT_GLOW] = demo_id("tex/gate_glow");
    p.texture[SU_PAINT_SLOT_DIRT] = demo_id("tex/gate_dirt");
    p.texture[SU_PAINT_SLOT_MATERIAL] = 0;
    p.dirt_amount = 0.8f;
    p.layer_count = 2;
    p.layer[0].color[0] = 0.75f;
    p.layer[0].color[1] = 0.06f;
    p.layer[0].color[2] = 0.04f;
    p.layer[0].roughness = 0.35f;
    p.layer[0].metallic = 0.1f;
    p.layer[1].color[0] = 0.9f;
    p.layer[1].color[1] = 0.65f;
    p.layer[1].color[2] = 0.08f;
    p.layer[1].roughness = 0.3f;
    p.layer[1].metallic = 0.2f;
    p.layer[1].flags = SU_PAINT_LAYER_GLOW_TINT;
    p.layer[1].glow_color[0] = 1.0f;
    p.layer[1].glow_color[1] = 0.55f;
    p.layer[1].glow_color[2] = 0.1f;
    p.layer[1].glow_strength = 5.0f;
    return st == SU_OK ? su_asset_writer_add_paint_material(w, demo_id("mat/gate"), &m, &p) : st;
}

uint8_t *demo_build_pack(size_t *len) {
    SuAssetWriter *w = NULL;
    SuAssetVertex *v;
    uint32_t *idx;
    uint32_t nv;
    uint32_t ni;
    SuAssetModelNode nodes[16];
    uint8_t *out = NULL;
    uint32_t st = SU_OK;
    int i;
    if (su_asset_writer_create(&w) != SU_OK) {
        return NULL;
    }
    mat(w, "mat/hull", 0.8f, 0.82f, 0.85f, 0.7f, 0.35f, 0, 0, 0, 1);
    mat(w, "mat/engine", 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.55f, 0.15f, 6.0f);
    if (paint_gate(w) != SU_OK) {
        su_asset_writer_destroy(w);
        return NULL;
    }
    mat(w, "mat/gate_light", 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.2f, 0.6f, 1.0f, 5.0f);
    mat(w, "mat/station", 0.45f, 0.46f, 0.5f, 0.3f, 0.6f, 0, 0, 0, 1);
    mat(w, "mat/window", 0.1f, 0.1f, 0.1f, 0.0f, 0.5f, 1.0f, 0.85f, 0.5f, 3.0f);
    mat(w, "mat/rock", 0.32f, 0.29f, 0.26f, 0.0f, 0.9f, 0, 0, 0, 1);
    st = builtin(SU_MESH_SHIP, &v, &idx, &nv, &ni);
    st = st == SU_OK ? add_mesh(w, "mesh/hull", v, nv, idx, ni, "mat/hull") : st;
    st = st == SU_OK ? builtin(SU_MESH_SPHERE, &v, &idx, &nv, &ni) : st;
    st = st == SU_OK ? add_mesh(w, "mesh/glow", v, nv, idx, ni, "mat/engine") : st;
    st = st == SU_OK ? torus(6.0f, 0.35f, 64, 12, &v, &idx, &nv, &ni) : st;
    st = st == SU_OK ? add_mesh(w, "mesh/gate_ring", v, nv, idx, ni, "mat/gate") : st;
    st = st == SU_OK ? torus(6.0f, 0.08f, 64, 6, &v, &idx, &nv, &ni) : st;
    st = st == SU_OK ? add_mesh(w, "mesh/gate_light", v, nv, idx, ni, "mat/gate_light") : st;
    st = st == SU_OK ? torus(40.0f, 3.0f, 96, 16, &v, &idx, &nv, &ni) : st;
    st = st == SU_OK ? add_mesh(w, "mesh/station_ring", v, nv, idx, ni, "mat/station") : st;
    st = st == SU_OK ? torus(40.0f, 0.5f, 96, 6, &v, &idx, &nv, &ni) : st;
    st = st == SU_OK ? add_mesh(w, "mesh/station_windows", v, nv, idx, ni, "mat/window") : st;
    st = st == SU_OK ? builtin(SU_MESH_SPHERE, &v, &idx, &nv, &ni) : st;
    st = st == SU_OK ? add_mesh(w, "mesh/station_hub", v, nv, idx, ni, "mat/station") : st;
    st = st == SU_OK ? builtin(SU_MESH_BOX, &v, &idx, &nv, &ni) : st;
    st = st == SU_OK ? add_mesh(w, "mesh/beam", v, nv, idx, ni, "mat/station") : st;
    st = st == SU_OK ? builtin(SU_MESH_SPHERE, &v, &idx, &nv, &ni) : st;
    if (st == SU_OK) {
        lumpy(v, nv, idx, ni, 0.25f);
        st = add_mesh(w, "mesh/rock0", v, nv, idx, ni, "mat/rock");
    }
    st = st == SU_OK ? builtin(SU_MESH_BOX, &v, &idx, &nv, &ni) : st;
    if (st == SU_OK) {
        lumpy(v, nv, idx, ni, 0.25f);
        st = add_mesh(w, "mesh/rock1", v, nv, idx, ni, "mat/rock");
    }
    /* models */
    if (st == SU_OK) {
        xform(&nodes[0], "mesh/hull", 0, 0, 0, 1, 1, 1, 0, 0);
        xform(&nodes[1], "mesh/glow", 0, 0, 0.45f, 0.22f, 0.12f, 0.12f, 0, 0);
        st = su_asset_writer_add_model(w, demo_id("model/ship"), nodes, 2);
    }
    if (st == SU_OK) {
        xform(&nodes[0], "mesh/gate_ring", 0, 0, 0, 1, 1, 1, 0, 0);
        xform(&nodes[1], "mesh/gate_light", 0, 0, 0.36f, 1, 1, 1, 0, 0);
        xform(&nodes[2], "mesh/gate_light", 0, 0, -0.36f, 1, 1, 1, 0, 0);
        st = su_asset_writer_add_model(w, demo_id("model/gate"), nodes, 3);
    }
    if (st == SU_OK) {
        /* the station spins about its own z axis (the host rotates the object) */
        xform(&nodes[0], "mesh/station_hub", 0, 0, 0, 14, 14, 18, 0, 0);
        xform(&nodes[1], "mesh/station_ring", 0, 0, 0, 1, 1, 1, 0, 0);
        xform(&nodes[2], "mesh/station_windows", 0, 0, 2.8f, 1, 1, 1, 0, 0);
        xform(&nodes[3], "mesh/station_windows", 0, 0, -2.8f, 1, 1, 1, 0, 0);
        for (i = 0; i < 4; i++) {
            xform(&nodes[4 + i], "mesh/beam", 0, 0, 0, 80, 2.0f, 2.0f, 0, PI_F * 0.25f * (float)i);
        }
        st = su_asset_writer_add_model(w, demo_id("model/station"), nodes, 8);
    }
    if (st == SU_OK) {
        xform(&nodes[0], "mesh/rock0", 0, 0, 0, 1, 1, 1, 0, 0);
        nodes[0].lod_mesh[1] = demo_id("mesh/rock1");
        nodes[0].lod_screen[1] = 0.03f;
        nodes[0].lod_count = 2;
        st = su_asset_writer_add_model(w, demo_id("model/rock"), nodes, 1);
    }
    if (st == SU_OK) {
        st = su_asset_writer_finish(w, &out, len);
    }
    su_asset_writer_destroy(w);
    return st == SU_OK ? out : NULL;
}
