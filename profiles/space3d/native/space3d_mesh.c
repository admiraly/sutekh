/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d static meshes. See space3d_mesh.h and docs/SPACE3D.md ("Static meshes"). Integer only: the BVHs are
 * built from exact vertex bounds by a deterministic median split, and the queries are fixed sequences of
 * fixed_q32_v1 operations (su_num_fast.h) whose results do not depend on the traversal order. */
#include "space3d_mesh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"
#include "ir/su_num_fast.h"
#include "sutekh/status.h"

#define ONE SU_Q32_ONE
#define MAX_COORD (INT64_C(1) << 52) /* 2^20 m */
#define MIN_SCALE (INT64_C(1) << 16) /* 2^-16 */
#define MAX_SCALE (INT64_C(1) << 48) /* 2^16 */
#define STACK_MAX 128u

typedef struct S3Node {
    SuV3 lo, hi;
    uint32_t first; /* leaf: first item (in BVH order); inner: index of the right child (left = this + 1) */
    uint32_t count; /* leaf: items (> 0); inner: 0 */
} S3Node;

typedef struct S3Geom {
    uint32_t ntris, nnodes;
    S3Tri *tri;       /* BVH order */
    uint32_t *tri_id; /* index in the blob's triangle list */
    S3Node *node;
} S3Geom;

typedef struct S3Part {
    uint32_t geom, id; /* id: index in the blob's part list */
    SuV3 pos;
    SuQuat rot;
    int64_t scale;
    SuV3 lo, hi; /* mesh-frame bounds */
} S3Part;

typedef struct S3Comp {
    uint32_t first, count; /* sub-shapes [first, first + count) of S3MeshSet.sub */
} S3Comp;

typedef struct S3Mesh {
    uint32_t first, nparts; /* parts [first, first + nparts) of S3MeshSet.part, in this mesh's BVH order */
    uint32_t node0;         /* first node in S3MeshSet.top (valid when nparts > 0) */
    SuV3 lo, hi;
} S3Mesh;

struct S3MeshSet {
    uint32_t refs;
    uint32_t ngeom, npart, nmesh, ntris_total;
    S3Geom *geom;
    S3Part *part;
    S3Mesh *mesh;
    S3Node *top;
    uint32_t ncomp, nsub; /* compound shapes (S3MESH02) */
    S3SubShape *sub;
    S3Comp *comp;
    int brute;
};

static SuV3 v3(int64_t x, int64_t y, int64_t z) {
    SuV3 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static int64_t comp(SuV3 a, uint32_t i) {
    return i == 0u ? a.x : (i == 1u ? a.y : a.z);
}

static int v3_zero(SuV3 a) {
    return a.x == 0 && a.y == 0 && a.z == 0;
}

static SuV3 v3_min(SuV3 a, SuV3 b) {
    return v3(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z);
}

static SuV3 v3_max(SuV3 a, SuV3 b) {
    return v3(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z);
}

static SuV3 v3_divs(SuV3 a, int64_t s) {
    if (s == ONE) {
        return a;
    }
    return v3(su_q32_divf(a.x, s), su_q32_divf(a.y, s), su_q32_divf(a.z, s));
}

static SuV3 v3_pad(SuV3 a, int64_t p) {
    return v3(su_q32_addf(a.x, p), su_q32_addf(a.y, p), su_q32_addf(a.z, p));
}

/* ---------- blob reading ---------- */

typedef struct Rd {
    const uint8_t *p;
    size_t len, off;
    int bad;
} Rd;

static uint32_t rd_u32(Rd *r) {
    uint32_t v;
    if (r->bad || r->len - r->off < 4u) {
        r->bad = 1;
        return 0;
    }
    v = (uint32_t)r->p[r->off] | ((uint32_t)r->p[r->off + 1u] << 8) | ((uint32_t)r->p[r->off + 2u] << 16) |
        ((uint32_t)r->p[r->off + 3u] << 24);
    r->off += 4u;
    return v;
}

static int64_t rd_i64(Rd *r) {
    uint64_t lo = rd_u32(r);
    uint64_t hi = rd_u32(r);
    return su_i64_from_bits(lo | (hi << 32));
}

static int coord_ok(int64_t c) {
    return c >= -MAX_COORD && c <= MAX_COORD;
}

static int v3_ok(SuV3 a) {
    return coord_ok(a.x) && coord_ok(a.y) && coord_ok(a.z);
}

static int sub_coord_ok(SuV3 a) {
    return a.x >= -S3MESH_COMPOUND_COORD && a.x <= S3MESH_COMPOUND_COORD && a.y >= -S3MESH_COMPOUND_COORD &&
           a.y <= S3MESH_COMPOUND_COORD && a.z >= -S3MESH_COMPOUND_COORD && a.z <= S3MESH_COMPOUND_COORD;
}

static void say(char *why, size_t cap, const char *msg, uint32_t index) {
    if (why != NULL && cap > 0u) {
        (void)snprintf(why, cap, "%s (index %u)", msg, (unsigned)index);
    }
}

/* ---------- BVH build ---------- */

typedef struct Build {
    const SuV3 *lo, *hi; /* item bounds (by item index) */
    uint32_t *idx, *tmp; /* item order being built */
    S3Node *node;
    uint32_t nn;
} Build;

static int64_t item_key(const Build *b, uint32_t item, uint32_t axis) {
    return su_q32_addf(comp(b->lo[item], axis), comp(b->hi[item], axis));
}

/* Stable merge sort of idx[first, first + n) by (key on axis, item index). */
static void sort_items(Build *b, uint32_t first, uint32_t n, uint32_t axis) {
    uint32_t width, i;
    uint32_t *src = b->idx + first, *dst = b->tmp + first;
    for (width = 1; width < n; width *= 2u) {
        for (i = 0; i < n; i += 2u * width) {
            uint32_t lo = i, mid = i + width < n ? i + width : n, hi = i + 2u * width < n ? i + 2u * width : n;
            uint32_t x = lo, y = mid, k = lo;
            while (x < mid && y < hi) {
                int64_t kx = item_key(b, src[x], axis), ky = item_key(b, src[y], axis);
                if (kx < ky || (kx == ky && src[x] < src[y])) {
                    dst[k++] = src[x++];
                } else {
                    dst[k++] = src[y++];
                }
            }
            while (x < mid) {
                dst[k++] = src[x++];
            }
            while (y < hi) {
                dst[k++] = src[y++];
            }
        }
        {
            uint32_t *t = src;
            src = dst;
            dst = t;
        }
    }
    if (src != b->idx + first) {
        memcpy(b->idx + first, src, (size_t)n * sizeof *src);
    }
}

/* Node over idx[first, first + count): leaf at <= S3MESH_LEAF items, else split at the median of the centroid
 * order on the axis of largest centroid spread (ties to the lower axis; equal centroids: split by position). */
static uint32_t build_node(Build *b, uint32_t first, uint32_t count) {
    uint32_t me = b->nn++, i, axis = 0, mid;
    S3Node *nd = &b->node[me];
    int64_t best = -1;
    nd->lo = b->lo[b->idx[first]];
    nd->hi = b->hi[b->idx[first]];
    for (i = 1; i < count; i++) {
        nd->lo = v3_min(nd->lo, b->lo[b->idx[first + i]]);
        nd->hi = v3_max(nd->hi, b->hi[b->idx[first + i]]);
    }
    if (count <= S3MESH_LEAF) {
        nd->first = first;
        nd->count = count;
        return me;
    }
    for (i = 0; i < 3u; i++) {
        int64_t mn = INT64_MAX, mx = INT64_MIN, spread;
        uint32_t k;
        for (k = 0; k < count; k++) {
            int64_t c = item_key(b, b->idx[first + k], i);
            mn = c < mn ? c : mn;
            mx = c > mx ? c : mx;
        }
        spread = su_q32_subf(mx, mn);
        if (spread > best) {
            best = spread;
            axis = i;
        }
    }
    if (best > 0) {
        sort_items(b, first, count, axis);
    }
    mid = count / 2u;
    nd->count = 0;
    (void)build_node(b, first, mid);
    b->node[me].first = build_node(b, first + mid, count - mid);
    return me;
}

/* Builds the BVH of n items (bounds lo/hi) into node[] (capacity 2n); idx receives the item order. */
static uint32_t build_bvh(const SuV3 *lo, const SuV3 *hi, uint32_t n, uint32_t *idx, uint32_t *tmp, S3Node *node) {
    Build b;
    uint32_t i;
    b.lo = lo;
    b.hi = hi;
    b.idx = idx;
    b.tmp = tmp;
    b.node = node;
    b.nn = 0;
    for (i = 0; i < n; i++) {
        idx[i] = i;
    }
    (void)build_node(&b, 0, n);
    return b.nn;
}

/* ---------- set lifetime ---------- */

static void set_free(S3MeshSet *s) {
    uint32_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; s->geom != NULL && i < s->ngeom; i++) {
        free(s->geom[i].tri);
        free(s->geom[i].tri_id);
        free(s->geom[i].node);
    }
    free(s->geom);
    free(s->part);
    free(s->mesh);
    free(s->top);
    free(s->sub);
    free(s->comp);
    free(s);
}

void s3mesh_set_retain(S3MeshSet *s) {
    if (s != NULL) {
        s->refs++;
    }
}

void s3mesh_set_release(S3MeshSet *s) {
    if (s != NULL && --s->refs == 0u) {
        set_free(s);
    }
}

static int quat_unit(SuQuat q) {
    int64_t n2 = su_q32_addf(su_q32_addf(su_q32_mulf(q.w, q.w), su_q32_mulf(q.x, q.x)),
                             su_q32_addf(su_q32_mulf(q.y, q.y), su_q32_mulf(q.z, q.z)));
    int64_t d = su_q32_subf(n2, ONE);
    return d >= -(ONE >> 16) && d <= (ONE >> 16);
}

/* Geometry g from the blob: triangles, normals, BVH. */
static uint32_t load_geom(Rd *r, S3Geom *g, uint32_t nv, uint32_t nt, uint32_t gi, char *why, size_t cap) {
    SuV3 *v = NULL, *lo = NULL, *hi = NULL;
    uint32_t(*t)[3] = NULL;
    uint32_t *tmp = NULL, i;
    uint32_t st = SU_E_RESOURCE_LIMIT;
    v = (SuV3 *)malloc((size_t)(nv > 0u ? nv : 1u) * sizeof *v);
    t = (uint32_t(*)[3])malloc((size_t)nt * sizeof *t);
    lo = (SuV3 *)malloc((size_t)nt * sizeof *lo);
    hi = (SuV3 *)malloc((size_t)nt * sizeof *hi);
    tmp = (uint32_t *)malloc((size_t)nt * sizeof *tmp);
    g->tri = (S3Tri *)malloc((size_t)nt * sizeof *g->tri);
    g->tri_id = (uint32_t *)malloc((size_t)nt * sizeof *g->tri_id);
    g->node = (S3Node *)malloc((size_t)nt * 2u * sizeof *g->node);
    if (v == NULL || t == NULL || lo == NULL || hi == NULL || tmp == NULL || g->tri == NULL || g->tri_id == NULL ||
        g->node == NULL) {
        say(why, cap, "out of memory for geometry", gi);
        goto done;
    }
    st = SU_E_PARSE;
    for (i = 0; i < nv; i++) {
        v[i].x = rd_i64(r);
        v[i].y = rd_i64(r);
        v[i].z = rd_i64(r);
        if (!r->bad && !v3_ok(v[i])) {
            say(why, cap, "vertex coordinate beyond 2^20 m", gi);
            goto done;
        }
    }
    for (i = 0; i < nt; i++) {
        uint32_t k;
        for (k = 0; k < 3u; k++) {
            t[i][k] = rd_u32(r);
            if (!r->bad && t[i][k] >= nv) {
                say(why, cap, "triangle vertex index out of range in geometry", gi);
                goto done;
            }
        }
    }
    if (r->bad) {
        say(why, cap, "blob ends inside geometry", gi);
        goto done;
    }
    for (i = 0; i < nt; i++) {
        SuV3 a = v[t[i][0]], b = v[t[i][1]], c = v[t[i][2]];
        lo[i] = v3_min(a, v3_min(b, c));
        hi[i] = v3_max(a, v3_max(b, c));
    }
    g->ntris = nt;
    g->nnodes = build_bvh(lo, hi, nt, g->tri_id, tmp, g->node);
    for (i = 0; i < nt; i++) {
        uint32_t k = g->tri_id[i];
        g->tri[i] = s3_tri_make(v[t[k][0]], v[t[k][1]], v[t[k][2]]);
    }
    st = SU_OK;
done:
    free(v);
    free(t);
    free(lo);
    free(hi);
    free(tmp);
    return st;
}

uint32_t s3mesh_set_create(const uint8_t *data, size_t len, S3MeshSet **out, char *why, size_t cap) {
    Rd r;
    S3MeshSet *s;
    uint32_t i, st = SU_E_PARSE, *nv = NULL, *nt = NULL, *tmp = NULL, *order = NULL;
    uint64_t tv = 0, tt = 0, top_cap = 0, hdr = 20u, tail = 0;
    int v2;
    SuV3 *plo = NULL, *phi = NULL;
    S3Part *raw = NULL;
    if (why != NULL && cap > 0u) {
        why[0] = '\0';
    }
    if (out == NULL || (data == NULL && len > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    v2 = len >= 8u && memcmp(data, S3MESH_MAGIC2, 8u) == 0;
    if (v2) {
        hdr = 28u;
    }
    if (len < hdr || (!v2 && memcmp(data, S3MESH_MAGIC, 8u) != 0)) {
        say(why, cap, "not an S3MESH01 or S3MESH02 blob (magic)", 0);
        return SU_E_PARSE;
    }
    s = (S3MeshSet *)calloc(1u, sizeof *s);
    if (s == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    s->refs = 1u;
    r.p = data;
    r.len = len;
    r.off = 8u;
    r.bad = 0;
    s->ngeom = rd_u32(&r);
    s->npart = rd_u32(&r);
    s->nmesh = rd_u32(&r);
    if (v2) {
        s->ncomp = rd_u32(&r);
        s->nsub = rd_u32(&r);
        if (s->ncomp > S3MESH_MAX_COMPOUNDS || s->nsub > S3MESH_MAX_SUBSHAPES) {
            say(why, cap, "too many compounds or sub-shapes", 0);
            st = SU_E_RESOURCE_LIMIT;
            goto fail;
        }
        tail = 56u * (uint64_t)s->nsub + 8u * (uint64_t)s->ncomp;
    }
    if (s->ngeom > S3MESH_MAX_GEOMS || s->npart > S3MESH_MAX_PARTS || s->nmesh > S3MESH_MAX_MESHES) {
        say(why, cap, "too many geometries, parts or meshes", 0);
        st = SU_E_RESOURCE_LIMIT;
        goto fail;
    }
    nv = (uint32_t *)calloc((size_t)s->ngeom + 1u, sizeof *nv);
    nt = (uint32_t *)calloc((size_t)s->ngeom + 1u, sizeof *nt);
    s->geom = (S3Geom *)calloc((size_t)s->ngeom + 1u, sizeof *s->geom);
    if (nv == NULL || nt == NULL || s->geom == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto fail;
    }
    for (i = 0; i < s->ngeom; i++) {
        nv[i] = rd_u32(&r);
        nt[i] = rd_u32(&r);
        tv += nv[i];
        tt += nt[i];
        if (!r.bad && nt[i] == 0u) {
            say(why, cap, "geometry without triangles", i);
            goto fail;
        }
    }
    if (r.bad) {
        say(why, cap, "blob ends inside the geometry table", 0);
        goto fail;
    }
    if (tv > S3MESH_MAX_VERTS || tt > S3MESH_MAX_TRIS) {
        say(why, cap, "more than 2^24 vertices or triangles", 0);
        st = SU_E_RESOURCE_LIMIT;
        goto fail;
    }
    /* Exact size check before reading the arrays. */
    if ((uint64_t)len != hdr + 8u * (uint64_t)s->ngeom + 24u * tv + 12u * tt + 68u * (uint64_t)s->npart +
                            8u * (uint64_t)s->nmesh + tail) {
        say(why, cap, "blob size does not match its tables", 0);
        goto fail;
    }
    s->ntris_total = (uint32_t)tt;
    for (i = 0; i < s->ngeom; i++) {
        st = load_geom(&r, &s->geom[i], nv[i], nt[i], i, why, cap);
        if (st != SU_OK) {
            goto fail;
        }
    }
    st = SU_E_PARSE;
    raw = (S3Part *)calloc((size_t)s->npart + 1u, sizeof *raw);
    s->part = (S3Part *)calloc((size_t)s->npart + 1u, sizeof *s->part);
    plo = (SuV3 *)calloc((size_t)s->npart + 1u, sizeof *plo);
    phi = (SuV3 *)calloc((size_t)s->npart + 1u, sizeof *phi);
    order = (uint32_t *)calloc((size_t)s->npart + 1u, sizeof *order);
    tmp = (uint32_t *)calloc((size_t)s->npart + 1u, sizeof *tmp);
    s->mesh = (S3Mesh *)calloc((size_t)s->nmesh + 1u, sizeof *s->mesh);
    if (raw == NULL || s->part == NULL || plo == NULL || phi == NULL || order == NULL || tmp == NULL ||
        s->mesh == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto fail;
    }
    for (i = 0; i < s->npart; i++) {
        S3Part *p = &raw[i];
        const S3Geom *g;
        SuV3 c, h, glo, ghi;
        p->id = i;
        p->geom = rd_u32(&r);
        p->pos.x = rd_i64(&r);
        p->pos.y = rd_i64(&r);
        p->pos.z = rd_i64(&r);
        p->rot.w = rd_i64(&r);
        p->rot.x = rd_i64(&r);
        p->rot.y = rd_i64(&r);
        p->rot.z = rd_i64(&r);
        p->scale = rd_i64(&r);
        if (r.bad) {
            say(why, cap, "blob ends inside the part table", i);
            goto fail;
        }
        if (p->geom >= s->ngeom || !v3_ok(p->pos) || !quat_unit(p->rot) || p->scale < MIN_SCALE ||
            p->scale > MAX_SCALE) {
            say(why, cap, "part with a bad geometry index, position, rotation (not unit) or scale", i);
            goto fail;
        }
        g = &s->geom[p->geom];
        glo = g->node[0].lo;
        ghi = g->node[0].hi;
        /* Centre and half extents of the geometry bounds, scaled (half rounded up), then the rotated box. */
        h = v3((su_q32_subf(ghi.x, glo.x) + 1) / 2, (su_q32_subf(ghi.y, glo.y) + 1) / 2,
               (su_q32_subf(ghi.z, glo.z) + 1) / 2);
        c = v3(su_q32_addf(glo.x, h.x), su_q32_addf(glo.y, h.y), su_q32_addf(glo.z, h.z));
        if (p->scale != ONE) {
            c = su_v3_scalef(c, p->scale);
            h = v3_pad(su_v3_scalef(h, p->scale), 1);
        }
        s3_bounds_obb(su_v3_addf(p->pos, su_quat_rotatef(p->rot, c)), p->rot, h, &p->lo, &p->hi);
        p->lo = v3_pad(p->lo, -S3MESH_PAD);
        p->hi = v3_pad(p->hi, S3MESH_PAD);
        if (!v3_ok(p->lo) || !v3_ok(p->hi)) {
            say(why, cap, "part bounds beyond 2^20 m in the mesh frame", i);
            goto fail;
        }
        plo[i] = p->lo;
        phi[i] = p->hi;
    }
    for (i = 0; i < s->nmesh; i++) {
        S3Mesh *m = &s->mesh[i];
        m->first = rd_u32(&r);
        m->nparts = rd_u32(&r);
        if (r.bad) {
            say(why, cap, "blob ends inside the mesh table", i);
            goto fail;
        }
        if ((uint64_t)m->first + m->nparts > s->npart) {
            say(why, cap, "mesh part range beyond the part table", i);
            goto fail;
        }
        top_cap += 2u * (uint64_t)m->nparts;
    }
    if (v2) {
        s->sub = (S3SubShape *)calloc((size_t)s->nsub + 1u, sizeof *s->sub);
        s->comp = (S3Comp *)calloc((size_t)s->ncomp + 1u, sizeof *s->comp);
        if (s->sub == NULL || s->comp == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            goto fail;
        }
        for (i = 0; i < s->nsub; i++) {
            S3SubShape *u = &s->sub[i];
            int64_t la, lb;
            u->a.x = rd_i64(&r);
            u->a.y = rd_i64(&r);
            u->a.z = rd_i64(&r);
            u->b.x = rd_i64(&r);
            u->b.y = rd_i64(&r);
            u->b.z = rd_i64(&r);
            u->r = rd_i64(&r);
            if (r.bad) {
                say(why, cap, "blob ends inside the sub-shape table", i);
                goto fail;
            }
            if (!sub_coord_ok(u->a) || !sub_coord_ok(u->b) || u->r < 0 || u->r > S3MESH_COMPOUND_COORD) {
                say(why, cap, "sub-shape coordinate or radius beyond 4096 m (or a negative radius)", i);
                goto fail;
            }
            /* reach: max(|a|, |b|) rounded up (su_v3_lenf may round down by a raw unit) */
            la = su_q32_addf(su_v3_lenf(u->a), 1);
            lb = su_q32_addf(su_v3_lenf(u->b), 1);
            u->reach = la > lb ? la : lb;
        }
        for (i = 0; i < s->ncomp; i++) {
            S3Comp *cp = &s->comp[i];
            cp->first = rd_u32(&r);
            cp->count = rd_u32(&r);
            if (r.bad) {
                say(why, cap, "blob ends inside the compound table", i);
                goto fail;
            }
            if (cp->count == 0u || cp->count > S3MESH_COMPOUND_MAX || (uint64_t)cp->first + cp->count > s->nsub) {
                say(why, cap, "compound with 0 or more than 8 sub-shapes, or a range beyond the sub-shape table", i);
                goto fail;
            }
        }
    }
    if (r.off != r.len) {
        say(why, cap, "trailing bytes", 0);
        goto fail;
    }
    s->top = (S3Node *)calloc((size_t)top_cap + 1u, sizeof *s->top);
    if (s->top == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto fail;
    }
    /* Per mesh: its own copy of its parts in BVH order (parts may be shared between meshes in the blob). */
    {
        uint32_t nodes = 0, slot = 0;
        S3Part *dst = (S3Part *)calloc((size_t)(top_cap / 2u) + 1u, sizeof *dst);
        if (dst == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            goto fail;
        }
        free(s->part);
        s->part = dst;
        for (i = 0; i < s->nmesh; i++) {
            S3Mesh *m = &s->mesh[i];
            uint32_t k, n;
            if (m->nparts == 0u) {
                m->first = slot;
                continue;
            }
            n = build_bvh(plo + m->first, phi + m->first, m->nparts, order, tmp, s->top + nodes);
            for (k = 0; k < m->nparts; k++) {
                dst[slot + k] = raw[m->first + order[k]];
            }
            /* Leaves index the mesh's own part slice: shift them to absolute slots. */
            for (k = 0; k < n; k++) {
                if (s->top[nodes + k].count > 0u) {
                    s->top[nodes + k].first += slot;
                } else {
                    s->top[nodes + k].first += nodes;
                }
            }
            m->node0 = nodes;
            m->lo = s->top[nodes].lo;
            m->hi = s->top[nodes].hi;
            m->first = slot;
            nodes += n;
            slot += m->nparts;
        }
    }
    free(nv);
    free(nt);
    free(raw);
    free(plo);
    free(phi);
    free(order);
    free(tmp);
    *out = s;
    return SU_OK;
fail:
    free(nv);
    free(nt);
    free(raw);
    free(plo);
    free(phi);
    free(order);
    free(tmp);
    set_free(s);
    return st;
}

uint32_t s3mesh_count(const S3MeshSet *s) {
    return s != NULL ? s->nmesh : 0u;
}

int s3mesh_valid(const S3MeshSet *s, uint32_t mesh) {
    return s != NULL && mesh < s->nmesh && s->mesh[mesh].nparts > 0u;
}

void s3mesh_bounds(const S3MeshSet *s, uint32_t mesh, SuV3 *lo, SuV3 *hi) {
    *lo = s->mesh[mesh].lo;
    *hi = s->mesh[mesh].hi;
}

void s3mesh_set_brute(S3MeshSet *s, int brute) {
    if (s != NULL) {
        s->brute = brute;
    }
}

uint32_t s3mesh_triangles(const S3MeshSet *s) {
    return s != NULL ? s->ntris_total : 0u;
}

uint32_t s3mesh_compound_count(const S3MeshSet *s) {
    return s != NULL ? s->ncomp : 0u;
}

const S3SubShape *s3mesh_compound(const S3MeshSet *s, uint32_t index, uint32_t *n) {
    if (s == NULL || index >= s->ncomp) {
        *n = 0;
        return NULL;
    }
    *n = s->comp[index].count;
    return s->sub + s->comp[index].first;
}

/* ---------- queries ---------- */

/* Slab test of c0 + t v, t in [0, 1], against [lo - emax, hi - emin] (the box swept back by the mover's extent).
 * Returns 1 with the entry t (q32) when the ray meets the box no later than tlimit. */
static int slab(SuV3 c0, SuV3 v, SuV3 lo, SuV3 hi, SuV3 emin, SuV3 emax, int64_t tlimit, int64_t *tenter) {
    int64_t t0 = 0, t1 = ONE;
    uint32_t i;
    for (i = 0; i < 3u; i++) {
        int64_t p = comp(c0, i), d = comp(v, i);
        int64_t l = su_q32_subf(comp(lo, i), comp(emax, i)), h = su_q32_subf(comp(hi, i), comp(emin, i));
        int64_t ta, tb;
        if (d == 0) {
            if (p < l || p > h) {
                return 0;
            }
            continue;
        }
        ta = su_q32_divf(su_q32_subf(l, p), d);
        tb = su_q32_divf(su_q32_subf(h, p), d);
        if (ta > tb) {
            int64_t t = ta;
            ta = tb;
            tb = t;
        }
        t0 = ta > t0 ? ta : t0;
        t1 = tb < t1 ? tb : t1;
        if (t0 > t1) {
            return 0;
        }
    }
    if (t0 > tlimit) {
        return 0;
    }
    *tenter = t0;
    return 1;
}

typedef struct Sweep {
    SuV3 c0, v, e0, e1; /* in the current frame */
    int64_t r;
    int seg;
    SuV3 emin, emax; /* min(e0, e1) - r - pad, max(e0, e1) + r + pad */
} Sweep;

static void sweep_ext(Sweep *q, int64_t pad) {
    int64_t rp = su_q32_addf(q->r, pad);
    q->emin = v3_pad(v3_min(q->e0, q->e1), su_q32_neg(rp));
    q->emax = v3_pad(v3_max(q->e0, q->e1), rp);
}

typedef struct Best {
    S3Hit h;      /* normal in the frame of the part that produced it, until rotated out */
    uint32_t part; /* blob part id */
    uint32_t tri;  /* blob triangle id */
    int have;
    int64_t tmax; /* search limit while there is no hit (s3mesh_sweep_lim) */
} Best;

static int better(const Best *b, int64_t toi, uint32_t part, uint32_t tri) {
    if (!b->have || toi < b->h.toi) {
        return 1;
    }
    if (toi > b->h.toi) {
        return 0;
    }
    return part < b->part || (part == b->part && tri < b->tri);
}

/* Plane reject: the swept core (convex hull of the start and end core points) is farther than r + pad from the
 * triangle's plane on one side. Degenerate triangles are never rejected here. */
static int plane_reject(const S3Tri *t, const Sweep *q, int64_t pad) {
    SuV3 p[4];
    uint32_t i, n = q->seg ? 4u : 2u, above = 0, below = 0;
    int64_t lim = su_q32_addf(q->r, pad), nlim = su_q32_neg(lim);
    if (v3_zero(t->n)) {
        return 0;
    }
    p[0] = su_v3_addf(q->c0, q->e0);
    p[1] = su_v3_addf(p[0], q->v);
    p[2] = su_v3_addf(q->c0, q->e1);
    p[3] = su_v3_addf(p[2], q->v);
    for (i = 0; i < n; i++) {
        int64_t h = su_v3_dotf(su_v3_subf(p[i], t->a), t->n);
        above += h > lim;
        below += h < nlim;
    }
    return above == n || below == n;
}

/* Sweep against one geometry in its own frame; keeps the best hit (normal rotated to the mesh frame by prot). */
static void geom_sweep(const S3MeshSet *s, const S3Geom *g, const Sweep *q, int64_t pad, uint32_t part_id,
                       SuQuat prot, Best *best, S3MeshStats *st) {
    uint32_t stack[STACK_MAX];
    int64_t enter[STACK_MAX];
    uint32_t sp = 0;
    stack[sp] = 0;
    enter[sp++] = 0;
    while (sp > 0u) {
        uint32_t ni = stack[--sp];
        const S3Node *nd = &g->node[ni];
        int64_t lim = best->have ? best->h.toi : best->tmax, te = 0;
        if (!s->brute && enter[sp] > lim) {
            continue;
        }
        if (st != NULL) {
            st->nodes++;
        }
        if (nd->count > 0u) {
            uint32_t k;
            for (k = nd->first; k < nd->first + nd->count; k++) {
                const S3Tri *t = &g->tri[k];
                S3Hit h;
                if (!s->brute && plane_reject(t, q, pad)) {
                    continue;
                }
                if (st != NULL) {
                    st->tri_tests++;
                }
                h = q->seg ? s3_sweep_capsule_tri(q->c0, q->v, q->r, q->e0, q->e1, t)
                           : s3_sweep_sphere_tri(q->c0, q->v, q->r, t);
                if (h.hit && better(best, h.toi, part_id, g->tri_id[k])) {
                    best->h = h;
                    best->h.n = su_quat_rotatef(prot, h.n);
                    best->part = part_id;
                    best->tri = g->tri_id[k];
                    best->have = 1;
                }
            }
            continue;
        }
        {
            uint32_t ch[2], k, n = 0;
            int64_t tc[2] = {0, 0};
            ch[0] = ni + 1u;
            ch[1] = nd->first;
            for (k = 0; k < 2u; k++) {
                const S3Node *c = &g->node[ch[k]];
                if (s->brute) {
                    tc[n] = 0;
                    ch[n++] = ch[k];
                } else if (slab(q->c0, q->v, c->lo, c->hi, q->emin, q->emax, lim, &te)) {
                    tc[n] = te;
                    ch[n++] = ch[k];
                }
            }
            /* Push the later entry first so the earlier one is visited first (pruning only; order-free result). */
            if (n == 2u && tc[1] > tc[0]) {
                uint32_t tu = ch[0];
                int64_t tt = tc[0];
                ch[0] = ch[1];
                tc[0] = tc[1];
                ch[1] = tu;
                tc[1] = tt;
            }
            for (k = 0; k < n && sp < STACK_MAX; k++) {
                stack[sp] = ch[k];
                enter[sp++] = tc[k];
            }
        }
    }
}

S3Hit s3mesh_sweep(const S3MeshSet *s, uint32_t mesh, SuV3 pos, SuQuat rot, SuV3 c0, SuV3 v, int64_t r, SuV3 e0,
                   SuV3 e1, S3MeshStats *st) {
    return s3mesh_sweep_lim(s, mesh, pos, rot, c0, v, r, e0, e1, ONE, st);
}

S3Hit s3mesh_sweep_lim(const S3MeshSet *s, uint32_t mesh, SuV3 pos, SuQuat rot, SuV3 c0, SuV3 v, int64_t r, SuV3 e0,
                       SuV3 e1, int64_t tmax, S3MeshStats *st) {
    S3Hit out;
    Best best;
    Sweep qm;
    const S3Mesh *m;
    SuQuat qc;
    uint32_t stack[STACK_MAX];
    uint32_t sp = 0;
    memset(&best, 0, sizeof best);
    best.tmax = tmax;
    out.hit = 0;
    out.toi = 0;
    out.n = v3(0, 0, 0);
    if (!s3mesh_valid(s, mesh)) {
        return out;
    }
    m = &s->mesh[mesh];
    qc = su_quat_conjf(rot);
    qm.c0 = su_quat_rotatef(qc, su_v3_subf(c0, pos));
    qm.v = su_quat_rotatef(qc, v);
    qm.e0 = su_quat_rotatef(qc, e0);
    qm.e1 = su_quat_rotatef(qc, e1);
    qm.r = r;
    qm.seg = !v3_zero(e0) || !v3_zero(e1);
    sweep_ext(&qm, S3MESH_PAD);
    stack[sp++] = m->node0;
    while (sp > 0u) {
        uint32_t ni = stack[--sp];
        const S3Node *nd = &s->top[ni];
        int64_t lim = best.have ? best.h.toi : tmax, te = 0;
        if (st != NULL) {
            st->nodes++;
        }
        if (!s->brute && !slab(qm.c0, qm.v, nd->lo, nd->hi, qm.emin, qm.emax, lim, &te)) {
            continue;
        }
        if (nd->count > 0u) {
            uint32_t k;
            for (k = nd->first; k < nd->first + nd->count; k++) {
                const S3Part *p = &s->part[k];
                Sweep qp;
                SuQuat pc = su_quat_conjf(p->rot);
                int64_t pad = p->scale == ONE ? S3MESH_PAD : su_q32_divf(S3MESH_PAD, p->scale);
                if (!s->brute && !slab(qm.c0, qm.v, p->lo, p->hi, qm.emin, qm.emax, lim, &te)) {
                    continue;
                }
                qp.c0 = v3_divs(su_quat_rotatef(pc, su_v3_subf(qm.c0, p->pos)), p->scale);
                qp.v = v3_divs(su_quat_rotatef(pc, qm.v), p->scale);
                qp.e0 = v3_divs(su_quat_rotatef(pc, qm.e0), p->scale);
                qp.e1 = v3_divs(su_quat_rotatef(pc, qm.e1), p->scale);
                qp.r = p->scale == ONE ? r : su_q32_divf(r, p->scale);
                qp.seg = qm.seg;
                sweep_ext(&qp, pad);
                geom_sweep(s, &s->geom[p->geom], &qp, pad, p->id, p->rot, &best, st);
            }
            continue;
        }
        if (sp + 2u <= STACK_MAX) {
            stack[sp++] = nd->first;
            stack[sp++] = ni + 1u;
        }
    }
    if (!best.have) {
        return out;
    }
    out.hit = 1;
    out.toi = best.h.toi;
    out.n = su_v3_normalizef(su_quat_rotatef(rot, best.h.n));
    if (v3_zero(out.n)) {
        out.n = v3(0, 0, ONE);
    }
    return out;
}

/* Lower bound of the distance between two boxes (0 when they overlap). */
static int64_t box_gap(SuV3 alo, SuV3 ahi, SuV3 blo, SuV3 bhi) {
    SuV3 g;
    uint32_t i;
    int64_t c[3];
    for (i = 0; i < 3u; i++) {
        int64_t d1 = su_q32_subf(comp(blo, i), comp(ahi, i)), d2 = su_q32_subf(comp(alo, i), comp(bhi, i));
        int64_t d = d1 > d2 ? d1 : d2;
        c[i] = d > 0 ? d : 0;
    }
    g = v3(c[0], c[1], c[2]);
    return su_v3_lenf(g);
}

/* Smallest core distance from the segment to the geometry (its frame, geometry units) into *mn (initially
 * INT64_MAX), skipping only nodes whose box gap exceeds min(*mn, prune) + pad: every triangle whose distance is
 * below prune is visited, so *mn is the geometry's exact minimum whenever that minimum is below prune. */
static void geom_dist(const S3MeshSet *s, const S3Geom *g, SuV3 p, SuV3 e0, SuV3 e1, int64_t pad, int64_t prune,
                      int64_t *mn, S3MeshStats *st) {
    uint32_t stack[STACK_MAX];
    uint32_t sp = 0;
    SuV3 a = su_v3_addf(p, e0), b = su_v3_addf(p, e1);
    SuV3 slo = v3_min(a, b), shi = v3_max(a, b);
    stack[sp++] = 0;
    while (sp > 0u) {
        uint32_t ni = stack[--sp];
        const S3Node *nd = &g->node[ni];
        if (st != NULL) {
            st->nodes++;
        }
        if (!s->brute && box_gap(slo, shi, nd->lo, nd->hi) > su_q32_addf(*mn < prune ? *mn : prune, pad)) {
            continue;
        }
        if (nd->count > 0u) {
            uint32_t k;
            for (k = nd->first; k < nd->first + nd->count; k++) {
                const S3Tri *t = &g->tri[k];
                int64_t d;
                if (!s->brute && !(t->n.x == 0 && t->n.y == 0 && t->n.z == 0)) {
                    /* Plane lower bound: both segment ends on one side of the triangle's plane. */
                    int64_t h0 = su_v3_dotf(su_v3_subf(a, t->a), t->n), h1 = su_v3_dotf(su_v3_subf(b, t->a), t->n);
                    if ((h0 > 0 && h1 > 0) || (h0 < 0 && h1 < 0)) {
                        int64_t lb = h0 > 0 ? (h0 < h1 ? h0 : h1) : su_q32_neg(h0 > h1 ? h0 : h1);
                        if (lb > su_q32_addf(*mn < prune ? *mn : prune, pad)) {
                            continue;
                        }
                    }
                }
                if (st != NULL) {
                    st->tri_tests++;
                }
                d = s3_dist_seg_tri(p, e0, e1, t);
                if (d < *mn) {
                    *mn = d;
                }
            }
            continue;
        }
        if (sp + 2u <= STACK_MAX) {
            /* Nearer child last (visited first), so the minimum shrinks early; the result does not depend on it. */
            uint32_t l = ni + 1u, r = nd->first;
            if (!s->brute && box_gap(slo, shi, g->node[l].lo, g->node[l].hi) >
                                 box_gap(slo, shi, g->node[r].lo, g->node[r].hi)) {
                stack[sp++] = l;
                stack[sp++] = r;
            } else {
                stack[sp++] = r;
                stack[sp++] = l;
            }
        }
    }
}

int64_t s3mesh_dist(const S3MeshSet *s, uint32_t mesh, SuV3 pos, SuQuat rot, SuV3 p, SuV3 e0, SuV3 e1,
                    int64_t limit, S3MeshStats *st) {
    const S3Mesh *m;
    SuQuat qc;
    SuV3 pm, e0m, e1m, a, b, slo, shi;
    int64_t best = su_q32_addf(limit < 0 ? 0 : limit, 1);
    uint32_t stack[STACK_MAX];
    uint32_t sp = 0;
    if (!s3mesh_valid(s, mesh)) {
        return best;
    }
    m = &s->mesh[mesh];
    qc = su_quat_conjf(rot);
    pm = su_quat_rotatef(qc, su_v3_subf(p, pos));
    e0m = su_quat_rotatef(qc, e0);
    e1m = su_quat_rotatef(qc, e1);
    a = su_v3_addf(pm, e0m);
    b = su_v3_addf(pm, e1m);
    slo = v3_min(a, b);
    shi = v3_max(a, b);
    stack[sp++] = m->node0;
    while (sp > 0u) {
        uint32_t ni = stack[--sp];
        const S3Node *nd = &s->top[ni];
        if (st != NULL) {
            st->nodes++;
        }
        if (!s->brute && box_gap(slo, shi, nd->lo, nd->hi) > su_q32_addf(best, S3MESH_PAD)) {
            continue;
        }
        if (nd->count > 0u) {
            uint32_t k;
            for (k = nd->first; k < nd->first + nd->count; k++) {
                const S3Part *pt = &s->part[k];
                SuQuat pc = su_quat_conjf(pt->rot);
                int64_t pad = pt->scale == ONE ? S3MESH_PAD : su_q32_divf(S3MESH_PAD, pt->scale);
                int64_t prune = pt->scale == ONE ? best : su_q32_divf(best, pt->scale);
                int64_t mn = INT64_MAX;
                if (!s->brute && box_gap(slo, shi, pt->lo, pt->hi) > su_q32_addf(best, S3MESH_PAD)) {
                    continue;
                }
                geom_dist(s, &s->geom[pt->geom], v3_divs(su_quat_rotatef(pc, su_v3_subf(pm, pt->pos)), pt->scale),
                          v3_divs(su_quat_rotatef(pc, e0m), pt->scale), v3_divs(su_quat_rotatef(pc, e1m), pt->scale),
                          pad, prune, &mn, st);
                if (mn != INT64_MAX) {
                    int64_t d = pt->scale == ONE ? mn : su_q32_mulf(mn, pt->scale);
                    if (d < best) {
                        best = d;
                    }
                }
            }
            continue;
        }
        if (sp + 2u <= STACK_MAX) {
            stack[sp++] = nd->first;
            stack[sp++] = ni + 1u;
        }
    }
    return best;
}
