/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Test helper: writes S3MESH01 blobs (profiles/space3d/native/space3d_mesh.h) from in-memory tables. Header-only,
 * static functions; used by test_space3d_mesh.c, test_space3d_ops.c and the host tests. */
#ifndef SU_TESTS_S3MESH_BLOB_H
#define SU_TESTS_S3MESH_BLOB_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"

typedef struct TBlob {
    uint8_t *p;
    size_t n, cap;
} TBlob;

typedef struct TGeom {
    uint32_t nv, nt;
    const SuV3 *v;
    const uint32_t *t; /* 3 per triangle */
} TGeom;

typedef struct TPart {
    uint32_t geom;
    SuV3 pos;
    SuQuat rot;
    int64_t scale;
} TPart;

typedef struct TMesh {
    uint32_t first, count;
} TMesh;

static inline void tb_put(TBlob *b, const void *src, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 256u;
        while (c < b->n + n) {
            c *= 2u;
        }
        b->p = (uint8_t *)realloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

static inline void tb_u32(TBlob *b, uint32_t v) {
    uint8_t x[4];
    x[0] = (uint8_t)(v & 0xFFu);
    x[1] = (uint8_t)((v >> 8) & 0xFFu);
    x[2] = (uint8_t)((v >> 16) & 0xFFu);
    x[3] = (uint8_t)((v >> 24) & 0xFFu);
    tb_put(b, x, 4u);
}

static inline void tb_i64(TBlob *b, int64_t v) {
    uint64_t u = (uint64_t)v;
    tb_u32(b, (uint32_t)(u & 0xFFFFFFFFu));
    tb_u32(b, (uint32_t)(u >> 32));
}

/* Writes a blob; the caller frees out->p. */
static inline void tb_build(TBlob *out, const TGeom *g, uint32_t ng, const TPart *p, uint32_t np, const TMesh *m,
                     uint32_t nm) {
    uint32_t i, k;
    memset(out, 0, sizeof *out);
    tb_put(out, "S3MESH01", 8u);
    tb_u32(out, ng);
    tb_u32(out, np);
    tb_u32(out, nm);
    for (i = 0; i < ng; i++) {
        tb_u32(out, g[i].nv);
        tb_u32(out, g[i].nt);
    }
    for (i = 0; i < ng; i++) {
        for (k = 0; k < g[i].nv; k++) {
            tb_i64(out, g[i].v[k].x);
            tb_i64(out, g[i].v[k].y);
            tb_i64(out, g[i].v[k].z);
        }
        for (k = 0; k < 3u * g[i].nt; k++) {
            tb_u32(out, g[i].t[k]);
        }
    }
    for (i = 0; i < np; i++) {
        tb_u32(out, p[i].geom);
        tb_i64(out, p[i].pos.x);
        tb_i64(out, p[i].pos.y);
        tb_i64(out, p[i].pos.z);
        tb_i64(out, p[i].rot.w);
        tb_i64(out, p[i].rot.x);
        tb_i64(out, p[i].rot.y);
        tb_i64(out, p[i].rot.z);
        tb_i64(out, p[i].scale);
    }
    for (i = 0; i < nm; i++) {
        tb_u32(out, m[i].first);
        tb_u32(out, m[i].count);
    }
}

/* The 12 triangles of the box [-h, h] (outward winding) over tbox_verts' 8 vertices. */
static inline void tbox_tris(uint32_t out[36]) {
    static const uint8_t k[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                  2, 3, 7, 2, 7, 6, 1, 2, 6, 1, 6, 5, 0, 4, 7, 0, 7, 3};
    uint32_t i;
    for (i = 0; i < 36u; i++) {
        out[i] = k[i];
    }
}

static inline void tbox_verts(SuV3 h, SuV3 out[8]) {
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        out[i].x = (i == 1u || i == 2u || i == 5u || i == 6u) ? h.x : -h.x;
        out[i].y = (i == 2u || i == 3u || i == 6u || i == 7u) ? h.y : -h.y;
        out[i].z = i >= 4u ? h.z : -h.z;
    }
}

#endif
