/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d.query host operator (space3d_ops_v1). See space3d_ops.h and docs/SPACE3D.md. */
#include "space3d_ops.h"

#include <string.h>

#include "ir/su_num.h"
#include "ir/su_num_fast.h" /* exact inline equivalents (tests/num/test_num_fast.c) */
#include "space3d_geom.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

#define ONE SU_Q32_ONE
#define S3_NONE 0xFFFFFFFFu
/* Slack added to every broadphase bound (raw q32 units, about 1 micrometre): covers the rounding of the narrow
 * phase's frame changes, so the broadphase never drops a pair the narrow phase would report. */
#define S3_BOUNDS_SLACK 4096

#define R32(id, name) {id, SU_TYPE_U32, SU_ACCESS_READ, name}
#define W32(id, name) {id, SU_TYPE_U32, SU_ACCESS_WRITE, name}
#define RQ(id, name) {id, SU_TYPE_Q32, SU_ACCESS_READ, name ".lo"}, {(id) + 1u, SU_TYPE_Q32, SU_ACCESS_READ, name ".hi"}
#define WQ(id, name) {id, SU_TYPE_Q32, SU_ACCESS_WRITE, name ".lo"}, {(id) + 1u, SU_TYPE_Q32, SU_ACCESS_WRITE, name ".hi"}

/* Declared field table, ascending ID. A Q32 entry is a typed storage lane (host_decl.h): the pair (ID, ID+1) is
 * the low and high word of one q32 field, exactly the lanes an IR 0.3 capsule declaring that field binds. */
static const SuHostField k_fields[] = {
    R32(SPACE3D_F_KIND, "kind"),
    R32(SPACE3D_F_NEXT_GATE, "next_gate"),
    R32(SPACE3D_F_GATE_INDEX, "gate_index"),
    RQ(SPACE3D_F_POS_X, "pos_x"),
    RQ(SPACE3D_F_POS_Y, "pos_y"),
    RQ(SPACE3D_F_POS_Z, "pos_z"),
    RQ(SPACE3D_F_PREV_X, "prev_x"),
    RQ(SPACE3D_F_PREV_Y, "prev_y"),
    RQ(SPACE3D_F_PREV_Z, "prev_z"),
    RQ(SPACE3D_F_ROT_W, "rot_w"),
    RQ(SPACE3D_F_ROT_X, "rot_x"),
    RQ(SPACE3D_F_ROT_Y, "rot_y"),
    RQ(SPACE3D_F_ROT_Z, "rot_z"),
    RQ(SPACE3D_F_RADIUS, "radius"),
    RQ(SPACE3D_F_HALF_X, "half_x"),
    RQ(SPACE3D_F_HALF_Y, "half_y"),
    RQ(SPACE3D_F_HALF_Z, "half_z"),
    W32(SPACE3D_F_HIT_COUNT, "hit_count"),
    W32(SPACE3D_F_HIT_FLAGS, "hit_flags"),
    W32(42u, "c0_slot"),
    W32(43u, "c0_gen"),
    WQ(44u, "c0_toi"),
    WQ(46u, "c0_nx"),
    WQ(48u, "c0_ny"),
    WQ(50u, "c0_nz"),
    W32(52u, "c1_slot"),
    W32(53u, "c1_gen"),
    WQ(54u, "c1_toi"),
    WQ(56u, "c1_nx"),
    WQ(58u, "c1_ny"),
    WQ(60u, "c1_nz"),
    W32(SPACE3D_F_GATE_DIR, "gate_dir"),
    W32(SPACE3D_F_GATE_SLOT, "gate_slot"),
    WQ(SPACE3D_F_GATE_TOI, "gate_toi"),
};

#define S3_NFIELDS (sizeof k_fields / sizeof k_fields[0])

static Space3dCtx g_ctx;

static uint32_t query_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx);

/* Literal: SHA-256 of space3d_constants_text() (checked by test_space3d_ops). */
const SuHostOpDesc g_space3d_query_desc = {
    "space3d.query",
    SPACE3D_SCHEMA_ID,
    SPACE3D_OPS_VERSION,
    {0xd3, 0x7e, 0x6d, 0x92, 0xd8, 0x6a, 0xc6, 0x38, 0xcb, 0x1f, 0xc3, 0x58, 0x2f, 0x34, 0x8e, 0x2c,
     0x09, 0x85, 0x62, 0x29, 0x0d, 0x41, 0x20, 0x2a, 0x0b, 0x57, 0x77, 0x70, 0x7a, 0xa0, 0xd5, 0xfe},
    (uint32_t)S3_NFIELDS,
    k_fields,
    query_run,
    &g_ctx,
};

Space3dCtx *space3d_query_ctx(void) {
    return &g_ctx;
}

const char *space3d_constants_text(void) {
    return "space3d_ops_v1\n"
           "numeric_profile=fixed_q32_v1\n"
           "max_rows=65536\n"
           "cand_max=32\n"
           "contacts=2\n"
           "sweep_budget=4194304\n"
           "bounds_slack_raw=4096\n"
           "sweep_axis=largest_centre_spread,ties_x_y_z\n"
           "sweep_order=min_on_axis,slot\n"
           "cand_keep=first_in_sweep_order\n"
           "contact_order=toi,slot\n"
           "gate_lookup=gate_index_equals_next_gate,lowest_slot\n"
           "gate_side=front_ge_0\n"
           "gate_shapes=rect,disc,ellipse_q32_div\n"
           "none=4294967295\n";
}

/* ---------- scratch (static: no allocation in the tick) ---------- */

static uint8_t g_alive[SPACE3D_MAX_ROWS];
static uint32_t g_gen[SPACE3D_MAX_ROWS];
static int64_t g_lo[3][SPACE3D_MAX_ROWS];
static int64_t g_hi[3][SPACE3D_MAX_ROWS];
static uint32_t g_coll[SPACE3D_MAX_ROWS];
static uint32_t g_tmp[SPACE3D_MAX_ROWS];
static uint32_t g_cand[SPACE3D_MAX_ROWS][SPACE3D_CAND_MAX];
static uint8_t g_ncand[SPACE3D_MAX_ROWS];
static uint8_t g_flags[SPACE3D_MAX_ROWS];
static uint32_t g_gate_slot[SPACE3D_MAX_ROWS];
/* Temporal coherence of the sweep order (2026-10-18): the collidable set (ascending slots) and its sorted order
 * from the previous call. When the next call has the same set and axis, its sort starts from that order. */
static uint32_t g_prev_coll[SPACE3D_MAX_ROWS];
static uint32_t g_prev_order[SPACE3D_MAX_ROWS];
static uint32_t g_prev_n = 0;
static uint32_t g_prev_axis = 3u;

/* ---------- column access ---------- */

typedef struct Cols {
    const uint32_t *kind, *next_gate, *gate_index;
    const uint32_t *f[14][2]; /* q32 inputs: pos xyz, prev xyz, rot wxyz, radius, half xyz; [lo, hi] */
    uint32_t *hit_count, *hit_flags, *gate_dir, *gate_slot;
    uint32_t *c_slot[SPACE3D_CONTACTS], *c_gen[SPACE3D_CONTACTS];
    uint32_t *c_q[SPACE3D_CONTACTS][4][2]; /* toi, nx, ny, nz; [lo, hi] */
    uint32_t *gate_toi[2];
} Cols;

enum { F_POSX, F_POSY, F_POSZ, F_PREVX, F_PREVY, F_PREVZ, F_ROTW, F_ROTX, F_ROTY, F_ROTZ, F_RADIUS, F_HALFX,
       F_HALFY, F_HALFZ };

static const uint32_t *in_col(const SuInvocation *in, uint32_t id) {
    uint32_t i;
    for (i = 0; i < in->binding_count; i++) {
        if (in->bindings[i].field_id == id) {
            return (const uint32_t *)in->bindings[i].data;
        }
    }
    return NULL;
}

static uint32_t *out_col(SuOutputs *out, uint32_t id) {
    uint32_t i;
    for (i = 0; i < out->binding_count; i++) {
        if (out->bindings[i].field_id == id) {
            return (uint32_t *)out->bindings[i].data;
        }
    }
    return NULL;
}

static int bind_cols(const SuInvocation *in, SuOutputs *out, Cols *c) {
    static const uint32_t qid[14] = {SPACE3D_F_POS_X,  SPACE3D_F_POS_Y,  SPACE3D_F_POS_Z,  SPACE3D_F_PREV_X,
                                     SPACE3D_F_PREV_Y, SPACE3D_F_PREV_Z, SPACE3D_F_ROT_W,  SPACE3D_F_ROT_X,
                                     SPACE3D_F_ROT_Y,  SPACE3D_F_ROT_Z,  SPACE3D_F_RADIUS, SPACE3D_F_HALF_X,
                                     SPACE3D_F_HALF_Y, SPACE3D_F_HALF_Z};
    uint32_t i, k, j;
    int ok = 1;
    c->kind = in_col(in, SPACE3D_F_KIND);
    c->next_gate = in_col(in, SPACE3D_F_NEXT_GATE);
    c->gate_index = in_col(in, SPACE3D_F_GATE_INDEX);
    ok &= c->kind != NULL && c->next_gate != NULL && c->gate_index != NULL;
    for (i = 0; i < 14u; i++) {
        c->f[i][0] = in_col(in, qid[i]);
        c->f[i][1] = in_col(in, qid[i] + 1u);
        ok &= c->f[i][0] != NULL && c->f[i][1] != NULL;
    }
    c->hit_count = out_col(out, SPACE3D_F_HIT_COUNT);
    c->hit_flags = out_col(out, SPACE3D_F_HIT_FLAGS);
    c->gate_dir = out_col(out, SPACE3D_F_GATE_DIR);
    c->gate_slot = out_col(out, SPACE3D_F_GATE_SLOT);
    c->gate_toi[0] = out_col(out, SPACE3D_F_GATE_TOI);
    c->gate_toi[1] = out_col(out, SPACE3D_F_GATE_TOI + 1u);
    ok &= c->hit_count != NULL && c->hit_flags != NULL && c->gate_dir != NULL && c->gate_slot != NULL &&
          c->gate_toi[0] != NULL && c->gate_toi[1] != NULL;
    for (k = 0; k < SPACE3D_CONTACTS; k++) {
        uint32_t base = 42u + 10u * k;
        c->c_slot[k] = out_col(out, base);
        c->c_gen[k] = out_col(out, base + 1u);
        ok &= c->c_slot[k] != NULL && c->c_gen[k] != NULL;
        for (j = 0; j < 4u; j++) {
            c->c_q[k][j][0] = out_col(out, base + 2u + 2u * j);
            c->c_q[k][j][1] = out_col(out, base + 3u + 2u * j);
            ok &= c->c_q[k][j][0] != NULL && c->c_q[k][j][1] != NULL;
        }
    }
    return ok;
}

static int64_t rdq(const Cols *c, int f, uint32_t r) {
    return su_i64_from_bits(((uint64_t)c->f[f][1][r] << 32) | (uint64_t)c->f[f][0][r]);
}

static void wrq(uint32_t *const lanes[2], uint32_t r, int64_t v) {
    lanes[0][r] = (uint32_t)((uint64_t)v & 0xFFFFFFFFu);
    lanes[1][r] = (uint32_t)((uint64_t)v >> 32);
}

static SuV3 rd3(const Cols *c, int f, uint32_t r) {
    SuV3 v;
    v.x = rdq(c, f, r);
    v.y = rdq(c, f + 1, r);
    v.z = rdq(c, f + 2, r);
    return v;
}

static SuQuat rdrot(const Cols *c, uint32_t r) {
    SuQuat q;
    q.w = rdq(c, F_ROTW, r);
    q.x = rdq(c, F_ROTX, r);
    q.y = rdq(c, F_ROTY, r);
    q.z = rdq(c, F_ROTZ, r);
    return q;
}

static int is_collidable(uint32_t kind) {
    return kind >= SPACE3D_KIND_MOVER && kind <= SPACE3D_KIND_CAPSULE;
}

/* Capsule end points: pos -/+ rot * (0, 0, half_z). */
static void capsule_ends(const Cols *c, uint32_t r, SuV3 *a, SuV3 *b) {
    SuV3 axis = {0, 0, 0};
    SuV3 p = rd3(c, F_POSX, r);
    axis.z = rdq(c, F_HALFZ, r);
    axis = su_quat_rotatef(rdrot(c, r), axis);
    *a = su_v3_subf(p, axis);
    *b = su_v3_addf(p, axis);
}

static void bounds_union(SuV3 *lo, SuV3 *hi, SuV3 lo2, SuV3 hi2) {
    lo->x = lo2.x < lo->x ? lo2.x : lo->x;
    lo->y = lo2.y < lo->y ? lo2.y : lo->y;
    lo->z = lo2.z < lo->z ? lo2.z : lo->z;
    hi->x = hi2.x > hi->x ? hi2.x : hi->x;
    hi->y = hi2.y > hi->y ? hi2.y : hi->y;
    hi->z = hi2.z > hi->z ? hi2.z : hi->z;
}

static void row_bounds(const Cols *c, uint32_t r, uint32_t kind) {
    SuV3 lo, hi, lo2, hi2;
    int64_t rad = rdq(c, F_RADIUS, r);
    if (kind == SPACE3D_KIND_MOVER) {
        s3_bounds_sphere(rd3(c, F_PREVX, r), rad, &lo, &hi);
        s3_bounds_sphere(rd3(c, F_POSX, r), rad, &lo2, &hi2);
        bounds_union(&lo, &hi, lo2, hi2);
    } else if (kind == SPACE3D_KIND_SPHERE) {
        s3_bounds_sphere(rd3(c, F_POSX, r), rad, &lo, &hi);
    } else if (kind == SPACE3D_KIND_BOX) {
        s3_bounds_obb(rd3(c, F_POSX, r), rdrot(c, r), rd3(c, F_HALFX, r), &lo, &hi);
    } else {
        SuV3 a, b;
        capsule_ends(c, r, &a, &b);
        s3_bounds_sphere(a, rad, &lo, &hi);
        s3_bounds_sphere(b, rad, &lo2, &hi2);
        bounds_union(&lo, &hi, lo2, hi2);
    }
    g_lo[0][r] = su_q32_subf(lo.x, S3_BOUNDS_SLACK);
    g_lo[1][r] = su_q32_subf(lo.y, S3_BOUNDS_SLACK);
    g_lo[2][r] = su_q32_subf(lo.z, S3_BOUNDS_SLACK);
    g_hi[0][r] = su_q32_addf(hi.x, S3_BOUNDS_SLACK);
    g_hi[1][r] = su_q32_addf(hi.y, S3_BOUNDS_SLACK);
    g_hi[2][r] = su_q32_addf(hi.z, S3_BOUNDS_SLACK);
}

/* Stable bottom-up merge sort of idx[0..n) by (g_lo[axis][slot], slot). */
static void sort_by_min(uint32_t *idx, uint32_t n, uint32_t axis) {
    uint32_t width, i;
    uint32_t *src = idx, *dst = g_tmp;
    for (width = 1; width < n; width *= 2u) {
        for (i = 0; i < n; i += 2u * width) {
            uint32_t lo = i, mid = i + width < n ? i + width : n, hi = i + 2u * width < n ? i + 2u * width : n;
            uint32_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                int64_t ka = g_lo[axis][src[a]], kb = g_lo[axis][src[b]];
                if (ka < kb || (ka == kb && src[a] < src[b])) {
                    dst[k++] = src[a++];
                } else {
                    dst[k++] = src[b++];
                }
            }
            while (a < mid) {
                dst[k++] = src[a++];
            }
            while (b < hi) {
                dst[k++] = src[b++];
            }
        }
        {
            uint32_t *t = src;
            src = dst;
            dst = t;
        }
    }
    if (src != idx) {
        memcpy(idx, src, (size_t)n * sizeof *idx);
    }
}

/* (g_lo[axis][a], a) < (g_lo[axis][b], b): the strict total order of the sweep. */
static int sweep_less(uint32_t a, uint32_t b, uint32_t axis) {
    int64_t ka = g_lo[axis][a], kb = g_lo[axis][b];
    return ka < kb || (ka == kb && a < b);
}

/* Sorts g_coll[0..n) by the sweep order. The order is a strict total order, so every correct sort gives the same
 * permutation: when the set of collidables and the axis equal the previous call's, insertion sort starts from the
 * previous order (bounded work; past 8n moves it falls back); otherwise, and on fallback, the merge sort runs on
 * the ascending-slot list. */
static void sort_sweep(uint32_t n, uint32_t axis) {
    uint32_t i;
    int done = 0;
    if (n == g_prev_n && axis == g_prev_axis && n > 0u && memcmp(g_coll, g_prev_coll, (size_t)n * sizeof *g_coll) == 0) {
        uint64_t moves = 0, cap = 8u * (uint64_t)n;
        memcpy(g_tmp, g_prev_order, (size_t)n * sizeof *g_tmp);
        done = 1;
        for (i = 1; i < n && done; i++) {
            uint32_t key = g_tmp[i], j = i;
            while (j > 0 && sweep_less(key, g_tmp[j - 1u], axis)) {
                g_tmp[j] = g_tmp[j - 1u];
                j--;
                if (++moves > cap) {
                    done = 0;
                    break;
                }
            }
            g_tmp[j] = key;
        }
    }
    memcpy(g_prev_coll, g_coll, (size_t)n * sizeof *g_coll);
    if (done) {
        memcpy(g_coll, g_tmp, (size_t)n * sizeof *g_coll);
    } else {
        sort_by_min(g_coll, n, axis);
    }
    memcpy(g_prev_order, g_coll, (size_t)n * sizeof *g_coll);
    g_prev_n = n;
    g_prev_axis = axis;
}

static void add_cand(uint32_t m, uint32_t o) {
    if (g_ncand[m] < SPACE3D_CAND_MAX) {
        g_cand[m][g_ncand[m]++] = o;
    } else {
        g_flags[m] |= SPACE3D_FLAG_CAND_TRUNCATED;
    }
}

typedef struct Contact {
    uint32_t slot;
    int64_t toi;
    SuV3 n;
} Contact;

/* Narrow phase of mover m against candidate o. */
static S3Hit narrow(const Cols *c, uint32_t m, uint32_t o) {
    SuV3 c0 = rd3(c, F_PREVX, m);
    SuV3 v = su_v3_subf(rd3(c, F_POSX, m), c0);
    int64_t r = rdq(c, F_RADIUS, m);
    uint32_t ko = c->kind[o];
    if (ko == SPACE3D_KIND_MOVER) {
        SuV3 o0 = rd3(c, F_PREVX, o);
        SuV3 vo = su_v3_subf(rd3(c, F_POSX, o), o0);
        return s3_sweep_sphere_sphere(c0, su_v3_subf(v, vo), r, o0, rdq(c, F_RADIUS, o));
    }
    if (ko == SPACE3D_KIND_SPHERE) {
        return s3_sweep_sphere_sphere(c0, v, r, rd3(c, F_POSX, o), rdq(c, F_RADIUS, o));
    }
    if (ko == SPACE3D_KIND_BOX) {
        return s3_sweep_sphere_obb(c0, v, r, rd3(c, F_POSX, o), rdrot(c, o), rd3(c, F_HALFX, o));
    } else {
        SuV3 a, b;
        capsule_ends(c, o, &a, &b);
        return s3_sweep_sphere_capsule(c0, v, r, a, b, rdq(c, F_RADIUS, o));
    }
}

/* Index of the first gate (lowest slot) whose gate_index equals key, in g_gate_slot[0..ng) sorted by
 * (gate_index, slot); S3_NONE when none. */
static uint32_t find_gate(const Cols *c, uint32_t ng, uint32_t key) {
    uint32_t lo = 0, hi = ng;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (c->gate_index[g_gate_slot[mid]] < key) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < ng && c->gate_index[g_gate_slot[lo]] == key ? g_gate_slot[lo] : S3_NONE;
}

static void sort_gates(const Cols *c, uint32_t ng) {
    /* Insertion sort by (gate_index, slot): gates are few; the input is in ascending slot order. */
    uint32_t i, j;
    for (i = 1; i < ng; i++) {
        uint32_t key = g_gate_slot[i];
        j = i;
        while (j > 0 && c->gate_index[g_gate_slot[j - 1u]] > c->gate_index[key]) {
            g_gate_slot[j] = g_gate_slot[j - 1u];
            j--;
        }
        g_gate_slot[j] = key;
    }
}

static uint32_t query_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    Space3dCtx *ctx = (Space3dCtx *)host_ctx;
    Cols c;
    uint64_t rows64 = in->row_count;
    uint32_t rows, r, ncoll = 0, ngate = 0, axis = 0, i;
    uint64_t pairs = 0;
    int budget_hit = 0;
    Space3dStats st;

    if (rows64 > SPACE3D_MAX_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    rows = (uint32_t)rows64;
    if (!bind_cols(in, out, &c)) {
        return SU_E_SCHEMA_MISMATCH;
    }
    memset(&st, 0, sizeof st);
    st.rows = rows;
    if (rows > 0u && (su_world_alive_view(w, 0, rows, g_alive) != SU_OK ||
                      su_world_generation_view(w, 0, rows, g_gen) != SU_OK)) {
        return SU_E_INTERNAL;
    }

    /* Gather collidables (ascending slot) and gates; compute bounds. */
    for (r = 0; r < rows; r++) {
        uint32_t k = c.kind[r];
        g_ncand[r] = 0;
        g_flags[r] = 0;
        if (!g_alive[r]) {
            continue;
        }
        if (is_collidable(k)) {
            row_bounds(&c, r, k);
            g_coll[ncoll++] = r;
            st.movers += k == SPACE3D_KIND_MOVER;
        } else if (k >= SPACE3D_KIND_GATE_RECT && k <= SPACE3D_KIND_GATE_ELLIPSE) {
            g_gate_slot[ngate++] = r;
        }
    }
    st.collidables = ncoll;
    st.gates = ngate;
    sort_gates(&c, ngate);

    if (ctx != NULL && ctx->brute_force) {
        /* Test seam: no broadphase; the narrow phase below takes every other collidable (g_coll stays in
         * ascending slot order) with no candidate cap and no budget. */
    } else if (ncoll > 1u) {
        /* Sweep axis: largest spread of bound centres (lo + hi, saturating), ties to the lower axis. */
        int64_t best = -1;
        uint32_t a;
        for (a = 0; a < 3u; a++) {
            int64_t mn = INT64_MAX, mx = INT64_MIN, spread;
            for (i = 0; i < ncoll; i++) {
                int64_t cen = su_q32_addf(g_lo[a][g_coll[i]], g_hi[a][g_coll[i]]);
                mn = cen < mn ? cen : mn;
                mx = cen > mx ? cen : mx;
            }
            spread = su_q32_subf(mx, mn);
            if (spread > best) {
                best = spread;
                axis = a;
            }
        }
        sort_sweep(ncoll, axis);
        for (i = 0; i < ncoll && !budget_hit; i++) {
            uint32_t p = g_coll[i], j;
            for (j = i + 1u; j < ncoll; j++) {
                uint32_t q = g_coll[j];
                uint32_t a1 = (axis + 1u) % 3u, a2 = (axis + 2u) % 3u;
                int pm, qm;
                if (g_lo[axis][q] > g_hi[axis][p]) {
                    break;
                }
                if (++pairs > SPACE3D_SWEEP_BUDGET) {
                    budget_hit = 1;
                    break;
                }
                pm = c.kind[p] == SPACE3D_KIND_MOVER;
                qm = c.kind[q] == SPACE3D_KIND_MOVER;
                if (!pm && !qm) {
                    continue;
                }
                if (g_lo[a1][p] > g_hi[a1][q] || g_lo[a1][q] > g_hi[a1][p] || g_lo[a2][p] > g_hi[a2][q] ||
                    g_lo[a2][q] > g_hi[a2][p]) {
                    continue;
                }
                if (pm) {
                    add_cand(p, q);
                }
                if (qm) {
                    add_cand(q, p);
                }
            }
        }
        if (pairs > SPACE3D_SWEEP_BUDGET) {
            pairs = SPACE3D_SWEEP_BUDGET;
        }
    }
    st.sweep_axis = axis;
    st.sweep_pairs = pairs;

    /* Per row: narrow phase for movers, zeros for every other row. */
    for (r = 0; r < rows; r++) {
        Contact best[SPACE3D_CONTACTS];
        uint32_t nbest = 0, found = 0, k, j;
        uint32_t flags = g_flags[r];
        uint32_t gdir = SPACE3D_GATE_NONE, gslot = S3_NONE;
        int64_t gtoi = 0;
        int mover = g_alive[r] && c.kind[r] == SPACE3D_KIND_MOVER;
        int brute = ctx != NULL && ctx->brute_force;
        if (mover) {
            uint32_t n = brute ? ncoll : g_ncand[r];
            if (budget_hit) {
                flags |= SPACE3D_FLAG_SWEEP_BUDGET;
            }
            /* Candidates in ascending slot order (insertion sort, n <= SPACE3D_CAND_MAX). */
            for (k = 1; k < n && !brute; k++) {
                uint32_t key = g_cand[r][k];
                j = k;
                while (j > 0 && g_cand[r][j - 1u] > key) {
                    g_cand[r][j] = g_cand[r][j - 1u];
                    j--;
                }
                g_cand[r][j] = key;
            }
            st.candidates += n;
            for (k = 0; k < n; k++) {
                uint32_t o = brute ? g_coll[k] : g_cand[r][k];
                S3Hit h;
                if (o == r) {
                    continue;
                }
                h = narrow(&c, r, o);
                st.narrow_tests++;
                if (!h.hit) {
                    continue;
                }
                found++;
                /* Insert by (toi, slot); candidates arrive in ascending slot, so equal toi keeps slot order. */
                for (j = nbest; j > 0 && best[j - 1u].toi > h.toi; j--) {
                    if (j < SPACE3D_CONTACTS) {
                        best[j] = best[j - 1u];
                    }
                }
                if (j < SPACE3D_CONTACTS) {
                    best[j].slot = o;
                    best[j].toi = h.toi;
                    best[j].n = h.n;
                    if (nbest < SPACE3D_CONTACTS) {
                        nbest++;
                    }
                }
            }
            st.contacts += found;
            if (found > SPACE3D_CONTACTS) {
                flags |= SPACE3D_FLAG_CONTACT_TRUNCATED;
            }
            /* Gate crossing of the centre segment against the next gate. */
            {
                uint32_t g = find_gate(&c, ngate, c.next_gate[r]);
                if (g != S3_NONE) {
                    S3GateHit gh;
                    int shape = c.kind[g] == SPACE3D_KIND_GATE_DISC      ? S3_GATE_DISC
                                : c.kind[g] == SPACE3D_KIND_GATE_ELLIPSE ? S3_GATE_ELLIPSE
                                                                         : S3_GATE_RECT;
                    if (s3_segment_gate(rd3(&c, F_PREVX, r), rd3(&c, F_POSX, r), rd3(&c, F_POSX, g), rdrot(&c, g),
                                        rdq(&c, F_HALFX, g), rdq(&c, F_HALFY, g), rdq(&c, F_RADIUS, g), shape,
                                        &gh) != 0) {
                        gdir = gh.dir > 0 ? SPACE3D_GATE_FORWARD : SPACE3D_GATE_BACKWARD;
                        gslot = g;
                        gtoi = gh.toi;
                    }
                }
            }
        }
        c.hit_count[r] = nbest;
        c.hit_flags[r] = mover ? flags : 0u;
        for (k = 0; k < SPACE3D_CONTACTS; k++) {
            int have = k < nbest;
            c.c_slot[k][r] = have ? best[k].slot : S3_NONE;
            c.c_gen[k][r] = have ? g_gen[best[k].slot] : 0u;
            wrq(c.c_q[k][0], r, have ? best[k].toi : 0);
            wrq(c.c_q[k][1], r, have ? best[k].n.x : 0);
            wrq(c.c_q[k][2], r, have ? best[k].n.y : 0);
            wrq(c.c_q[k][3], r, have ? best[k].n.z : 0);
        }
        c.gate_dir[r] = gdir;
        c.gate_slot[r] = gslot;
        wrq(c.gate_toi, r, gtoi);
    }
    if (ctx != NULL) {
        ctx->last = st;
    }
    return SU_OK;
}
