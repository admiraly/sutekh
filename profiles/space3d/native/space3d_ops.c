/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d.query host operator (space3d_ops_v1). See space3d_ops.h and docs/SPACE3D.md. */
#include "space3d_ops.h"

#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"
#include "ir/su_num_fast.h" /* exact inline equivalents (tests/num/test_num_fast.c) */
#include "space3d_geom.h"
#include "space3d_mesh.h"
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
    /* space3d.query_prox only (k_fields_prox): the proximity lanes follow the base table. */
    RQ(SPACE3D_F_PROX_MARGIN, "prox_margin"),
    W32(SPACE3D_F_PROX_SLOT, "prox_slot"),
    W32(SPACE3D_F_PROX_GEN, "prox_gen"),
    WQ(SPACE3D_F_PROX_DIST, "prox_dist"),
    /* space3d.query_shape only: the capsule-mover lanes follow the proximity lanes. */
    RQ(SPACE3D_F_Z0, "z0"),
    RQ(SPACE3D_F_Z1, "z1"),
    /* space3d.query_compound only: compound movers and the start-of-tick orientation. */
    R32(SPACE3D_F_COMPOUND, "compound"),
    W32(SPACE3D_F_C0_SUB, "c0_sub"),
    W32(SPACE3D_F_C1_SUB, "c1_sub"),
    RQ(SPACE3D_F_ROT0_W, "rot0_w"),
    RQ(SPACE3D_F_ROT0_X, "rot0_x"),
    RQ(SPACE3D_F_ROT0_Y, "rot0_y"),
    RQ(SPACE3D_F_ROT0_Z, "rot0_z"),
};

#define S3_NFIELDS_COMPOUND (sizeof k_fields / sizeof k_fields[0])
#define S3_NFIELDS_SHAPE (S3_NFIELDS_COMPOUND - 11u)
#define S3_NFIELDS_PROX (S3_NFIELDS_SHAPE - 4u)
#define S3_NFIELDS (S3_NFIELDS_PROX - 6u)

static Space3dScratch g_scratch;
static Space3dCtx g_ctx = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, &g_scratch, NULL};

static uint32_t query_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx);
static uint32_t query_run_prox(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx);
static uint32_t query_run_shape(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx);
static uint32_t query_run_compound(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx);

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

/* Literal: SHA-256 of space3d_prox_constants_text() (checked by test_space3d_ops). Shares the static context: one
 * world per worker process, as for space3d.query. */
const SuHostOpDesc g_space3d_query_prox_desc = {
    "space3d.query_prox",
    SPACE3D_SCHEMA_ID,
    SPACE3D_OPS_VERSION,
    {0x79, 0x20, 0x4f, 0xc0, 0x6c, 0x5d, 0x7d, 0x1e, 0x5a, 0x0c, 0xa2, 0xe8, 0x5a, 0x68, 0x7f, 0x23,
     0x4c, 0x73, 0x1f, 0x66, 0x98, 0x53, 0x80, 0x07, 0xb8, 0x1e, 0xec, 0xd9, 0x60, 0x2d, 0x7a, 0x84},
    (uint32_t)S3_NFIELDS_PROX,
    k_fields,
    query_run_prox,
    &g_ctx,
};

/* Literal: SHA-256 of space3d_shape_constants_text() (checked by test_space3d_ops). */
const SuHostOpDesc g_space3d_query_shape_desc = {
    "space3d.query_shape",
    SPACE3D_SCHEMA_ID,
    SPACE3D_OPS_VERSION,
    {0xfb, 0x82, 0x87, 0xe3, 0x59, 0xd7, 0x86, 0x62, 0xd0, 0xea, 0x1f, 0xd2, 0xb7, 0x78, 0xc9, 0xec,
     0x0d, 0x03, 0x49, 0xbc, 0x34, 0x32, 0x50, 0xed, 0xa6, 0x1b, 0x91, 0xa7, 0x4b, 0xa9, 0x7a, 0xb8},
    (uint32_t)S3_NFIELDS_SHAPE,
    k_fields,
    query_run_shape,
    &g_ctx,
};

/* Literal: SHA-256 of space3d_compound_constants_text() (checked by test_space3d_ops). */
const SuHostOpDesc g_space3d_query_compound_desc = {
    "space3d.query_compound",
    SPACE3D_SCHEMA_ID,
    SPACE3D_OPS_VERSION,
    {0x36, 0xad, 0xa4, 0x9c, 0x9d, 0xe5, 0xaf, 0x57, 0x80, 0xe4, 0xb1, 0xda, 0xe8, 0x88, 0xc4, 0x5a,
     0x14, 0xcd, 0x58, 0x26, 0x25, 0x97, 0x80, 0x99, 0x66, 0x84, 0x3a, 0x6f, 0xe3, 0xdd, 0x3e, 0x24},
    (uint32_t)S3_NFIELDS_COMPOUND,
    k_fields,
    query_run_compound,
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

const char *space3d_prox_constants_text(void) {
    return "space3d_ops_v1+proximity\n"
           "base=space3d.query\n"
           "prox_margin=per_mover_q32_lane,le_0_off\n"
           "prox_bounds=mover_bounds_widened_by_margin\n"
           "prox_set=kept_candidates,other_than_self\n"
           "prox_distance=end_positions,centre_to_shape_minus_radii,clamp_ge_0\n"
           "prox_keep=distance_le_margin,min_distance,ties_lowest_slot\n"
           "none=4294967295\n";
}

const char *space3d_shape_constants_text(void) {
    return "space3d_ops_v1+proximity+capsule_movers\n"
           "base=space3d.query_prox\n"
           "mover_shape=capsule_segment_local_z_z0_z1,rot_lanes,radius\n"
           "mover_shape_off=z0_eq_0_and_z1_eq_0,sphere_path_bit_exact\n"
           "rotation=end_of_tick_orientation,held_constant,translation_only_sweep\n"
           "capsule_sweep=rounded_parallelogram,face_then_edges,min_s_earlier_feature\n"
           "capsule_box=end_spheres_then_12_edges,start_crossing_toi_0_chord_mid_normal\n"
           "capsule_bounds=union_end_spheres_at_prev_and_pos\n"
           "gate=reference_point_segment\n"
           "none=4294967295\n";
}

const char *space3d_compound_constants_text(void) {
    return "space3d_ops_v1+proximity+capsule_movers+compound_movers\n"
           "base=space3d.query_shape\n"
           "compound=u32_lane_index_into_s3mesh02_compounds,none_4294967295\n"
           "compound_subshapes=1_to_8_capsules_local_frame,rot_lanes,radius_z0_z1_unused\n"
           "compound_contact=min_toi_then_mover_sub_then_other_sub\n"
           "contact_sub=low16_mover_sub,high16_other_mover_sub,none_65535,no_contact_4294967295\n"
           "turn=rot0_lanes_start_orientation,all_zero_off\n"
           "turn_pad=2_reach_sin_half_angle,sin_plus_2^-20,mul_plus_1_raw,sweep_and_bounds_only\n"
           "compound_bounds=union_subshape_end_spheres_at_prev_and_pos\n"
           "compound_prox=min_over_subshapes,unpadded\n"
           "compound_off=compound_none_and_rot0_zero,query_shape_bit_exact\n"
           "gate=reference_point_segment\n"
           "none=4294967295\n";
}

/* ---------- scratch (no allocation in the tick) ----------
 * The static context's scratch lives in these arrays (wired up on first use); an instance allocates the same
 * arrays for its own row capacity at creation. */

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

static Space3dScratch *static_scratch(void) {
    Space3dScratch *s = &g_scratch;
    if (s->max_rows == 0u) {
        uint32_t a;
        s->max_rows = SPACE3D_MAX_ROWS;
        s->alive = g_alive;
        s->gen = g_gen;
        for (a = 0; a < 3u; a++) {
            s->lo[a] = g_lo[a];
            s->hi[a] = g_hi[a];
        }
        s->coll = g_coll;
        s->tmp = g_tmp;
        s->cand = g_cand;
        s->ncand = g_ncand;
        s->flags = g_flags;
        s->gate_slot = g_gate_slot;
        s->prev_coll = g_prev_coll;
        s->prev_order = g_prev_order;
        s->prev_n = 0u;
        s->prev_axis = 3u;
    }
    return s;
}

/* One allocation per instance: the context, its scratch header and every array, each array 8-byte aligned. */
uint32_t space3d_query_instance_create(uint32_t max_rows, Space3dCtx **out) {
    size_t n, off, total;
    uint8_t *base;
    Space3dCtx *ctx;
    Space3dScratch *s;
    uint32_t a;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (max_rows == 0u || max_rows > SPACE3D_MAX_ROWS) {
        return SU_E_INVALID_ARGUMENT;
    }
    n = max_rows;
#define S3_ALIGN8(x) (((x) + 7u) & ~(size_t)7u)
    off = S3_ALIGN8(sizeof(Space3dCtx)) + S3_ALIGN8(sizeof(Space3dScratch));
    total = off + S3_ALIGN8(n)                     /* alive */
            + S3_ALIGN8(n * 4u)                    /* gen */
            + 6u * n * 8u                          /* lo, hi */
            + 2u * S3_ALIGN8(n * 4u)               /* coll, tmp */
            + n * 4u * SPACE3D_CAND_MAX            /* cand */
            + 2u * S3_ALIGN8(n)                    /* ncand, flags */
            + 3u * S3_ALIGN8(n * 4u);              /* gate_slot, prev_coll, prev_order */
    base = calloc(1u, total);
    if (base == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    ctx = (Space3dCtx *)(void *)base;
    s = (Space3dScratch *)(void *)(base + S3_ALIGN8(sizeof(Space3dCtx)));
    ctx->scratch = s;
    s->max_rows = max_rows;
    s->prev_axis = 3u;
    s->alive = base + off;
    off += S3_ALIGN8(n);
    s->gen = (uint32_t *)(void *)(base + off);
    off += S3_ALIGN8(n * 4u);
    for (a = 0; a < 3u; a++) {
        s->lo[a] = (int64_t *)(void *)(base + off);
        off += n * 8u;
        s->hi[a] = (int64_t *)(void *)(base + off);
        off += n * 8u;
    }
    s->coll = (uint32_t *)(void *)(base + off);
    off += S3_ALIGN8(n * 4u);
    s->tmp = (uint32_t *)(void *)(base + off);
    off += S3_ALIGN8(n * 4u);
    s->cand = (uint32_t(*)[SPACE3D_CAND_MAX])(void *)(base + off);
    off += n * 4u * SPACE3D_CAND_MAX;
    s->ncand = base + off;
    off += S3_ALIGN8(n);
    s->flags = base + off;
    off += S3_ALIGN8(n);
    s->gate_slot = (uint32_t *)(void *)(base + off);
    off += S3_ALIGN8(n * 4u);
    s->prev_coll = (uint32_t *)(void *)(base + off);
    off += S3_ALIGN8(n * 4u);
    s->prev_order = (uint32_t *)(void *)(base + off);
    off += S3_ALIGN8(n * 4u);
#undef S3_ALIGN8
    if (off != total) {
        free(base);
        return SU_E_INTERNAL;
    }
    *out = ctx;
    return SU_OK;
}

void space3d_query_instance_destroy(void *ctx) {
    if (ctx != NULL) {
        s3mesh_set_release(((Space3dCtx *)ctx)->meshes);
    }
    free(ctx);
}

uint32_t space3d_query_instance_set_meshes(Space3dCtx *ctx, struct S3MeshSet *set) {
    if (ctx == NULL || set == NULL || ctx == &g_ctx || ctx->meshes != NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    s3mesh_set_retain(set);
    ctx->meshes = set;
    return SU_OK;
}

/* ---------- column access ---------- */

typedef struct Cols {
    const uint32_t *kind, *next_gate, *gate_index;
    const uint32_t *f[14][2]; /* q32 inputs: pos xyz, prev xyz, rot wxyz, radius, half xyz; [lo, hi] */
    uint32_t *hit_count, *hit_flags, *gate_dir, *gate_slot;
    uint32_t *c_slot[SPACE3D_CONTACTS], *c_gen[SPACE3D_CONTACTS];
    uint32_t *c_q[SPACE3D_CONTACTS][4][2]; /* toi, nx, ny, nz; [lo, hi] */
    uint32_t *gate_toi[2];
    const uint32_t *prox_margin[2]; /* query_prox only */
    uint32_t *prox_slot, *prox_gen, *prox_dist[2];
    const uint32_t *z[2][2]; /* query_shape only: z0, z1; [lo, hi] */
    int shape;
    const uint32_t *compound; /* query_compound only */
    uint32_t *c_sub[SPACE3D_CONTACTS];
    const uint32_t *rot0[4][2];
    const S3MeshSet *meshes; /* static meshes (NULL: none) */
    S3MeshStats *mst;
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

static int bind_cols(const SuInvocation *in, SuOutputs *out, Cols *c, int prox, int shape, int compound) {
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
    if (prox) {
        c->prox_margin[0] = in_col(in, SPACE3D_F_PROX_MARGIN);
        c->prox_margin[1] = in_col(in, SPACE3D_F_PROX_MARGIN + 1u);
        c->prox_slot = out_col(out, SPACE3D_F_PROX_SLOT);
        c->prox_gen = out_col(out, SPACE3D_F_PROX_GEN);
        c->prox_dist[0] = out_col(out, SPACE3D_F_PROX_DIST);
        c->prox_dist[1] = out_col(out, SPACE3D_F_PROX_DIST + 1u);
        ok &= c->prox_margin[0] != NULL && c->prox_margin[1] != NULL && c->prox_slot != NULL && c->prox_gen != NULL &&
              c->prox_dist[0] != NULL && c->prox_dist[1] != NULL;
    }
    c->shape = shape;
    if (shape) {
        for (j = 0; j < 2u; j++) {
            c->z[j][0] = in_col(in, SPACE3D_F_Z0 + 2u * j);
            c->z[j][1] = in_col(in, SPACE3D_F_Z0 + 2u * j + 1u);
            ok &= c->z[j][0] != NULL && c->z[j][1] != NULL;
        }
    }
    c->compound = NULL;
    if (compound) {
        c->compound = in_col(in, SPACE3D_F_COMPOUND);
        c->c_sub[0] = out_col(out, SPACE3D_F_C0_SUB);
        c->c_sub[1] = out_col(out, SPACE3D_F_C1_SUB);
        ok &= c->compound != NULL && c->c_sub[0] != NULL && c->c_sub[1] != NULL;
        for (j = 0; j < 4u; j++) {
            c->rot0[j][0] = in_col(in, SPACE3D_F_ROT0_W + 2u * j);
            c->rot0[j][1] = in_col(in, SPACE3D_F_ROT0_W + 2u * j + 1u);
            ok &= c->rot0[j][0] != NULL && c->rot0[j][1] != NULL;
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

/* A kind 8 row collides when the instance holds a mesh set with a nonempty mesh gate_index. */
static int is_mesh(const Cols *c, uint32_t r) {
    return c->kind[r] == SPACE3D_KIND_MESH && s3mesh_valid(c->meshes, c->gate_index[r]);
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

/* Capsule mover (query_shape): world offsets e0 = rot * (0, 0, z0), e1 = rot * (0, 0, z1) of mover r. Returns 0 (a
 * sphere mover, offsets zero) unless the operator is query_shape, r is a mover and z0 or z1 is nonzero. */
static int mover_seg(const Cols *c, uint32_t r, SuV3 *e0, SuV3 *e1) {
    SuV3 l = {0, 0, 0};
    int64_t z0, z1;
    SuQuat q;
    e0->x = e0->y = e0->z = 0;
    *e1 = *e0;
    if (!c->shape || c->kind[r] != SPACE3D_KIND_MOVER) {
        return 0;
    }
    z0 = su_i64_from_bits(((uint64_t)c->z[0][1][r] << 32) | (uint64_t)c->z[0][0][r]);
    z1 = su_i64_from_bits(((uint64_t)c->z[1][1][r] << 32) | (uint64_t)c->z[1][0][r]);
    if (z0 == 0 && z1 == 0) {
        return 0;
    }
    q = rdrot(c, r);
    l.z = z0;
    *e0 = su_quat_rotatef(q, l);
    l.z = z1;
    *e1 = su_quat_rotatef(q, l);
    return 1;
}

/* ---------- compound movers and turning (query_compound) ----------
 * A mover is a list of pieces: world offsets of a core segment from the reference point at the end-of-tick
 * orientation, a swept radius (padded for the turn) and the unpadded radius (proximity). */
typedef struct Piece {
    SuV3 e0, e1;
    int64_t r, r0;
    int seg; /* 0: a sphere centred at the reference point + e0 */
} Piece;

typedef struct Geo {
    uint32_t n;
    int compound; /* a compound mover: contacts report its sub-shape index */
    int special;  /* compound or padded: the piece code path (else the query_shape code, bit for bit) */
    Piece p[S3MESH_COMPOUND_MAX];
} Geo;

static int64_t rd_lanes(const uint32_t *const lanes[2], uint32_t r) {
    return su_i64_from_bits(((uint64_t)lanes[1][r] << 32) | (uint64_t)lanes[0][r]);
}

/* sin(a / 2) for the angle a of the shortest rotation from rot0 to rot: |vector part| / |quaternion| of
 * conj(rot0) * rot, plus 2^-20 for rounding, at most ONE. 0 when rot0 is all zero or the operator is not
 * query_compound. */
static int64_t turn_sin(const Cols *c, uint32_t r) {
    SuQuat q0, qr;
    SuV3 vp;
    int64_t vl, w, nq, sn;
    if (c->compound == NULL) {
        return 0;
    }
    q0.w = rd_lanes(c->rot0[0], r);
    q0.x = rd_lanes(c->rot0[1], r);
    q0.y = rd_lanes(c->rot0[2], r);
    q0.z = rd_lanes(c->rot0[3], r);
    if (q0.w == 0 && q0.x == 0 && q0.y == 0 && q0.z == 0) {
        return 0;
    }
    qr = su_quat_mulf(su_quat_conjf(q0), rdrot(c, r));
    vp.x = qr.x;
    vp.y = qr.y;
    vp.z = qr.z;
    vl = su_v3_lenf(vp);
    w = qr.w < 0 ? su_q32_neg(qr.w) : qr.w;
    nq = su_q32_sqrtf(su_q32_addf(su_q32_mulf(vl, vl), su_q32_mulf(w, w)));
    sn = nq > 0 ? su_q32_divf(vl, nq) : ONE;
    sn = su_q32_addf(sn, ONE >> 20);
    return sn > ONE ? ONE : sn;
}

/* Turn pad of a core point within reach of the reference point: 2 reach sin(a / 2), plus one raw unit. */
static int64_t turn_pad(int64_t reach, int64_t sn) {
    return su_q32_addf(su_q32_mulf(su_q32_addf(reach, reach), sn), 1);
}

static void geo_of(const Cols *c, uint32_t r, Geo *g) {
    int64_t sn = turn_sin(c, r);
    const S3SubShape *sub = NULL;
    uint32_t n = 0, i;
    Piece *p = &g->p[0];
    g->n = 1;
    g->compound = 0;
    g->special = 0;
    if (c->compound != NULL && c->kind[r] == SPACE3D_KIND_MOVER) {
        sub = s3mesh_compound(c->meshes, c->compound[r], &n);
    }
    if (sub != NULL) {
        SuQuat q = rdrot(c, r);
        g->n = n;
        g->compound = 1;
        g->special = 1;
        for (i = 0; i < n; i++) {
            p = &g->p[i];
            p->seg = sub[i].a.x != sub[i].b.x || sub[i].a.y != sub[i].b.y || sub[i].a.z != sub[i].b.z;
            p->e0 = su_quat_rotatef(q, sub[i].a);
            p->e1 = p->seg ? su_quat_rotatef(q, sub[i].b) : p->e0;
            p->r0 = sub[i].r;
            p->r = sn > 0 ? su_q32_addf(p->r0, turn_pad(sub[i].reach, sn)) : p->r0;
        }
        return;
    }
    p->seg = mover_seg(c, r, &p->e0, &p->e1);
    p->r0 = rdq(c, F_RADIUS, r);
    p->r = p->r0;
    if (sn > 0 && p->seg) {
        int64_t z0 = su_i64_from_bits(((uint64_t)c->z[0][1][r] << 32) | (uint64_t)c->z[0][0][r]);
        int64_t z1 = su_i64_from_bits(((uint64_t)c->z[1][1][r] << 32) | (uint64_t)c->z[1][0][r]);
        int64_t reach;
        z0 = z0 < 0 ? su_q32_neg(z0) : z0;
        z1 = z1 < 0 ? su_q32_neg(z1) : z1;
        reach = z0 > z1 ? z0 : z1;
        p->r = su_q32_addf(p->r0, turn_pad(reach, sn));
        g->special = 1;
    }
}

static void bounds_union(SuV3 *lo, SuV3 *hi, SuV3 lo2, SuV3 hi2) {
    lo->x = lo2.x < lo->x ? lo2.x : lo->x;
    lo->y = lo2.y < lo->y ? lo2.y : lo->y;
    lo->z = lo2.z < lo->z ? lo2.z : lo->z;
    hi->x = hi2.x > hi->x ? hi2.x : hi->x;
    hi->y = hi2.y > hi->y ? hi2.y : hi->y;
    hi->z = hi2.z > hi->z ? hi2.z : hi->z;
}

static int64_t prox_margin_of(const Cols *c, uint32_t r) {
    return su_i64_from_bits(((uint64_t)c->prox_margin[1][r] << 32) | (uint64_t)c->prox_margin[0][r]);
}

/* Surface distance (q32, >= 0) between mover m at its end position and collidable o (others at their end position
 * too): centre-to-shape distance minus the radii, clamped at 0. Every step is a profile operation in this order. */
static int64_t prox_distance(const Cols *c, uint32_t m, uint32_t o, int64_t margin) {
    SuV3 p = rd3(c, F_POSX, m);
    int64_t d;
    uint32_t ko = c->kind[o];
    SuV3 e0, e1, f0, f1;
    int mseg = mover_seg(c, m, &e0, &e1);
    int oseg = mover_seg(c, o, &f0, &f1);
    if (ko == SPACE3D_KIND_MESH) {
        /* Static mesh: core distance to its triangles, searched within margin + the mover's radius (a value
         * beyond it means "not within the margin", which the caller drops). */
        d = s3mesh_dist(c->meshes, c->gate_index[o], rd3(c, F_POSX, o), rdrot(c, o), p, e0, e1,
                        su_q32_addf(margin, rdq(c, F_RADIUS, m)), c->mst);
    } else if (mseg || oseg) {
        /* Capsule mover involved (query_shape): core segment distances. */
        if (ko == SPACE3D_KIND_BOX) {
            d = s3_dist_seg_obb(p, e0, e1, rd3(c, F_POSX, o), rdrot(c, o), rd3(c, F_HALFX, o));
        } else {
            SuV3 a, b;
            if (ko == SPACE3D_KIND_CAPSULE) {
                capsule_ends(c, o, &a, &b);
            } else {
                SuV3 po = rd3(c, F_POSX, o);
                a = su_v3_addf(po, f0);
                b = su_v3_addf(po, f1);
            }
            d = su_q32_subf(s3_dist_seg_seg(p, e0, e1, a, b), rdq(c, F_RADIUS, o));
        }
    } else if (ko == SPACE3D_KIND_MOVER || ko == SPACE3D_KIND_SPHERE) {
        d = su_q32_subf(su_v3_lenf(su_v3_subf(p, rd3(c, F_POSX, o))), rdq(c, F_RADIUS, o));
    } else if (ko == SPACE3D_KIND_CAPSULE) {
        SuV3 a, b, ab, cp;
        int64_t den, t = 0;
        capsule_ends(c, o, &a, &b);
        ab = su_v3_subf(b, a);
        den = su_v3_dotf(ab, ab);
        if (den > 0) {
            t = su_q32_divf(su_v3_dotf(su_v3_subf(p, a), ab), den);
            t = t < 0 ? 0 : t > ONE ? ONE : t;
        }
        cp = su_v3_addf(a, su_v3_scalef(ab, t));
        d = su_q32_subf(su_v3_lenf(su_v3_subf(p, cp)), rdq(c, F_RADIUS, o));
    } else {
        SuV3 l = su_quat_rotatef(su_quat_conjf(rdrot(c, o)), su_v3_subf(p, rd3(c, F_POSX, o)));
        SuV3 h = rd3(c, F_HALFX, o), q;
        q.x = l.x < -h.x ? -h.x : l.x > h.x ? h.x : l.x;
        q.y = l.y < -h.y ? -h.y : l.y > h.y ? h.y : l.y;
        q.z = l.z < -h.z ? -h.z : l.z > h.z ? h.z : l.z;
        d = su_v3_lenf(su_v3_subf(l, q));
    }
    d = su_q32_subf(d, rdq(c, F_RADIUS, m));
    return d < 0 ? 0 : d;
}

/* Proximity with pieces (query_compound, mover m or a mover o special): the smallest over the pieces of
 * (core distance minus the unpadded radii), clamped at 0. A mesh is searched within min(margin, best so far) plus
 * the piece radius, as prox_distance does. */
static int64_t prox_geo(const Cols *c, uint32_t m, const Geo *gm, uint32_t o, const Geo *go, int64_t margin) {
    SuV3 p = rd3(c, F_POSX, m);
    int64_t best = INT64_MAX;
    uint32_t ko = c->kind[o], i, j;
    for (i = 0; i < gm->n; i++) {
        const Piece *pc = &gm->p[i];
        int64_t d;
        if (ko == SPACE3D_KIND_MESH) {
            int64_t lim = best < margin ? best : margin;
            d = su_q32_subf(s3mesh_dist(c->meshes, c->gate_index[o], rd3(c, F_POSX, o), rdrot(c, o), p, pc->e0, pc->e1,
                                        su_q32_addf(lim, pc->r0), c->mst),
                            pc->r0);
        } else if (ko == SPACE3D_KIND_BOX) {
            d = su_q32_subf(s3_dist_seg_obb(p, pc->e0, pc->e1, rd3(c, F_POSX, o), rdrot(c, o), rd3(c, F_HALFX, o)),
                            pc->r0);
        } else if (ko == SPACE3D_KIND_MOVER) {
            SuV3 po = rd3(c, F_POSX, o);
            d = INT64_MAX;
            for (j = 0; j < go->n; j++) {
                const Piece *q = &go->p[j];
                int64_t dj = su_q32_subf(su_q32_subf(s3_dist_seg_seg(p, pc->e0, pc->e1, su_v3_addf(po, q->e0),
                                                                     su_v3_addf(po, q->e1)),
                                                     q->r0),
                                         pc->r0);
                d = dj < d ? dj : d;
            }
        } else {
            SuV3 a, b;
            if (ko == SPACE3D_KIND_CAPSULE) {
                capsule_ends(c, o, &a, &b);
            } else {
                a = b = rd3(c, F_POSX, o);
            }
            d = su_q32_subf(su_q32_subf(s3_dist_seg_seg(p, pc->e0, pc->e1, a, b), rdq(c, F_RADIUS, o)), pc->r0);
        }
        best = d < best ? d : best;
    }
    return best < 0 ? 0 : best;
}

static void row_bounds(Space3dScratch *s, const Cols *c, uint32_t r, uint32_t kind) {
    SuV3 lo, hi, lo2, hi2;
    int64_t rad = rdq(c, F_RADIUS, r);
    SuV3 e0, e1;
    Geo g;
    g.special = 0;
    if (c->compound != NULL && kind == SPACE3D_KIND_MOVER) {
        geo_of(c, r, &g);
    }
    if (g.special) {
        /* Pieces: the end spheres of every piece at the start and end positions (padded radius). */
        SuV3 p0 = rd3(c, F_PREVX, r), p1 = rd3(c, F_POSX, r);
        uint32_t i;
        s3_bounds_sphere(su_v3_addf(p0, g.p[0].e0), g.p[0].r, &lo, &hi);
        for (i = 0; i < g.n; i++) {
            const Piece *pc = &g.p[i];
            s3_bounds_sphere(su_v3_addf(p0, pc->e0), pc->r, &lo2, &hi2);
            bounds_union(&lo, &hi, lo2, hi2);
            s3_bounds_sphere(su_v3_addf(p0, pc->e1), pc->r, &lo2, &hi2);
            bounds_union(&lo, &hi, lo2, hi2);
            s3_bounds_sphere(su_v3_addf(p1, pc->e0), pc->r, &lo2, &hi2);
            bounds_union(&lo, &hi, lo2, hi2);
            s3_bounds_sphere(su_v3_addf(p1, pc->e1), pc->r, &lo2, &hi2);
            bounds_union(&lo, &hi, lo2, hi2);
        }
    } else if (kind == SPACE3D_KIND_MOVER && mover_seg(c, r, &e0, &e1)) {
        /* Capsule mover: the end spheres at the start and end positions (the swept shape is their convex hull). */
        SuV3 p0 = rd3(c, F_PREVX, r), p1 = rd3(c, F_POSX, r);
        s3_bounds_sphere(su_v3_addf(p0, e0), rad, &lo, &hi);
        s3_bounds_sphere(su_v3_addf(p0, e1), rad, &lo2, &hi2);
        bounds_union(&lo, &hi, lo2, hi2);
        s3_bounds_sphere(su_v3_addf(p1, e0), rad, &lo2, &hi2);
        bounds_union(&lo, &hi, lo2, hi2);
        s3_bounds_sphere(su_v3_addf(p1, e1), rad, &lo2, &hi2);
        bounds_union(&lo, &hi, lo2, hi2);
    } else if (kind == SPACE3D_KIND_MOVER) {
        s3_bounds_sphere(rd3(c, F_PREVX, r), rad, &lo, &hi);
        s3_bounds_sphere(rd3(c, F_POSX, r), rad, &lo2, &hi2);
        bounds_union(&lo, &hi, lo2, hi2);
    } else if (kind == SPACE3D_KIND_SPHERE) {
        s3_bounds_sphere(rd3(c, F_POSX, r), rad, &lo, &hi);
    } else if (kind == SPACE3D_KIND_BOX) {
        s3_bounds_obb(rd3(c, F_POSX, r), rdrot(c, r), rd3(c, F_HALFX, r), &lo, &hi);
    } else if (kind == SPACE3D_KIND_MESH) {
        /* The mesh-frame bounds as an oriented box (half extents rounded up) at the row's pose. */
        SuV3 mlo, mhi, h, ce;
        SuQuat q = rdrot(c, r);
        s3mesh_bounds(c->meshes, c->gate_index[r], &mlo, &mhi);
        h.x = (su_q32_subf(mhi.x, mlo.x) + 1) / 2;
        h.y = (su_q32_subf(mhi.y, mlo.y) + 1) / 2;
        h.z = (su_q32_subf(mhi.z, mlo.z) + 1) / 2;
        ce.x = su_q32_addf(mlo.x, h.x);
        ce.y = su_q32_addf(mlo.y, h.y);
        ce.z = su_q32_addf(mlo.z, h.z);
        s3_bounds_obb(su_v3_addf(rd3(c, F_POSX, r), su_quat_rotatef(q, ce)), q, h, &lo, &hi);
    } else {
        SuV3 a, b;
        capsule_ends(c, r, &a, &b);
        s3_bounds_sphere(a, rad, &lo, &hi);
        s3_bounds_sphere(b, rad, &lo2, &hi2);
        bounds_union(&lo, &hi, lo2, hi2);
    }
    s->lo[0][r] = su_q32_subf(lo.x, S3_BOUNDS_SLACK);
    s->lo[1][r] = su_q32_subf(lo.y, S3_BOUNDS_SLACK);
    s->lo[2][r] = su_q32_subf(lo.z, S3_BOUNDS_SLACK);
    s->hi[0][r] = su_q32_addf(hi.x, S3_BOUNDS_SLACK);
    s->hi[1][r] = su_q32_addf(hi.y, S3_BOUNDS_SLACK);
    s->hi[2][r] = su_q32_addf(hi.z, S3_BOUNDS_SLACK);
}

/* Stable bottom-up merge sort of idx[0..n) by (lo[axis][slot], slot). */
static void sort_by_min(Space3dScratch *s, uint32_t *idx, uint32_t n, uint32_t axis) {
    uint32_t width, i;
    uint32_t *src = idx, *dst = s->tmp;
    const int64_t *key = s->lo[axis];
    for (width = 1; width < n; width *= 2u) {
        for (i = 0; i < n; i += 2u * width) {
            uint32_t lo = i, mid = i + width < n ? i + width : n, hi = i + 2u * width < n ? i + 2u * width : n;
            uint32_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                int64_t ka = key[src[a]], kb = key[src[b]];
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

/* (lo[axis][a], a) < (lo[axis][b], b): the strict total order of the sweep. */
static int sweep_less(const int64_t *key, uint32_t a, uint32_t b) {
    int64_t ka = key[a], kb = key[b];
    return ka < kb || (ka == kb && a < b);
}

/* Sorts coll[0..n) by the sweep order. The order is a strict total order, so every correct sort gives the same
 * permutation: when the set of collidables and the axis equal the previous call's, insertion sort starts from the
 * previous order (bounded work; past 8n moves it falls back); otherwise, and on fallback, the merge sort runs on
 * the ascending-slot list. */
static void sort_sweep(Space3dScratch *s, uint32_t n, uint32_t axis) {
    uint32_t i;
    int done = 0;
    const int64_t *key = s->lo[axis];
    if (n == s->prev_n && axis == s->prev_axis && n > 0u &&
        memcmp(s->coll, s->prev_coll, (size_t)n * sizeof *s->coll) == 0) {
        uint64_t moves = 0, cap = 8u * (uint64_t)n;
        memcpy(s->tmp, s->prev_order, (size_t)n * sizeof *s->tmp);
        done = 1;
        for (i = 1; i < n && done; i++) {
            uint32_t k = s->tmp[i], j = i;
            while (j > 0 && sweep_less(key, k, s->tmp[j - 1u])) {
                s->tmp[j] = s->tmp[j - 1u];
                j--;
                if (++moves > cap) {
                    done = 0;
                    break;
                }
            }
            s->tmp[j] = k;
        }
    }
    memcpy(s->prev_coll, s->coll, (size_t)n * sizeof *s->coll);
    if (done) {
        memcpy(s->coll, s->tmp, (size_t)n * sizeof *s->coll);
    } else {
        sort_by_min(s, s->coll, n, axis);
    }
    memcpy(s->prev_order, s->coll, (size_t)n * sizeof *s->coll);
    s->prev_n = n;
    s->prev_axis = axis;
}

static void add_cand(Space3dScratch *s, uint32_t m, uint32_t o) {
    if (s->ncand[m] < SPACE3D_CAND_MAX) {
        s->cand[m][s->ncand[m]++] = o;
    } else {
        s->flags[m] |= SPACE3D_FLAG_CAND_TRUNCATED;
    }
}

typedef struct Contact {
    uint32_t slot;
    int64_t toi;
    SuV3 n;
    uint32_t sub;
} Contact;

#define S3_SUB_PAIR(a, b) ((uint32_t)(a) | ((uint32_t)(b) << 16))

/* Piece p of a mover (reference point c0, displacement v) against static row o; mesh hits are searched up to
 * tmax only (s3mesh_sweep_lim). A sphere piece sweeps its own centre. */
static S3Hit piece_static(const Cols *c, uint32_t o, SuV3 c0, SuV3 v, const Piece *p, int64_t tmax) {
    uint32_t ko = c->kind[o];
    SuV3 cc = p->seg ? c0 : su_v3_addf(c0, p->e0);
    if (ko == SPACE3D_KIND_MESH) {
        SuV3 z = {0, 0, 0};
        return s3mesh_sweep_lim(c->meshes, c->gate_index[o], rd3(c, F_POSX, o), rdrot(c, o), cc, v, p->r,
                                p->seg ? p->e0 : z, p->seg ? p->e1 : z, tmax, c->mst);
    }
    if (ko == SPACE3D_KIND_SPHERE) {
        return p->seg ? s3_sweep_capsule_sphere(c0, v, p->r, p->e0, p->e1, rd3(c, F_POSX, o), rdq(c, F_RADIUS, o))
                      : s3_sweep_sphere_sphere(cc, v, p->r, rd3(c, F_POSX, o), rdq(c, F_RADIUS, o));
    }
    if (ko == SPACE3D_KIND_BOX) {
        return p->seg ? s3_sweep_capsule_obb(c0, v, p->r, p->e0, p->e1, rd3(c, F_POSX, o), rdrot(c, o),
                                             rd3(c, F_HALFX, o))
                      : s3_sweep_sphere_obb(cc, v, p->r, rd3(c, F_POSX, o), rdrot(c, o), rd3(c, F_HALFX, o));
    } else {
        SuV3 a, b;
        capsule_ends(c, o, &a, &b);
        return p->seg ? s3_sweep_capsule_capsule(c0, v, p->r, p->e0, p->e1, a, b, rdq(c, F_RADIUS, o))
                      : s3_sweep_sphere_capsule(cc, v, p->r, a, b, rdq(c, F_RADIUS, o));
    }
}

/* Piece p (reference c0, relative displacement vr) against piece q of another mover held at its start o0. */
static S3Hit piece_piece(SuV3 c0, SuV3 vr, const Piece *p, SuV3 o0, const Piece *q) {
    SuV3 cc = p->seg ? c0 : su_v3_addf(c0, p->e0);
    if (q->seg) {
        SuV3 a = su_v3_addf(o0, q->e0), b = su_v3_addf(o0, q->e1);
        return p->seg ? s3_sweep_capsule_capsule(c0, vr, p->r, p->e0, p->e1, a, b, q->r)
                      : s3_sweep_sphere_capsule(cc, vr, p->r, a, b, q->r);
    } else {
        SuV3 sc = su_v3_addf(o0, q->e0);
        return p->seg ? s3_sweep_capsule_sphere(c0, vr, p->r, p->e0, p->e1, sc, q->r)
                      : s3_sweep_sphere_sphere(cc, vr, p->r, sc, q->r);
    }
}

/* Narrow phase with pieces: the earliest hit over (mover piece i, other piece j), ties to the lower (i, j); *sub
 * receives S3_SUB_PAIR(i or SPACE3D_SUB_NONE, j or SPACE3D_SUB_NONE). */
static S3Hit narrow_geo(const Cols *c, uint32_t m, const Geo *gm, uint32_t o, const Geo *go, uint32_t *sub) {
    SuV3 c0 = rd3(c, F_PREVX, m);
    SuV3 v = su_v3_subf(rd3(c, F_POSX, m), c0);
    S3Hit best;
    uint32_t i, j, bi = 0, bj = 0;
    best.hit = 0;
    best.toi = 0;
    best.n.x = best.n.y = best.n.z = 0;
    if (c->kind[o] == SPACE3D_KIND_MOVER) {
        SuV3 o0 = rd3(c, F_PREVX, o);
        SuV3 vr = su_v3_subf(v, su_v3_subf(rd3(c, F_POSX, o), o0));
        for (i = 0; i < gm->n; i++) {
            for (j = 0; j < go->n; j++) {
                S3Hit h = piece_piece(c0, vr, &gm->p[i], o0, &go->p[j]);
                if (h.hit && (!best.hit || h.toi < best.toi)) {
                    best = h;
                    bi = i;
                    bj = j;
                }
            }
        }
        *sub = S3_SUB_PAIR(gm->compound ? bi : SPACE3D_SUB_NONE, go->compound ? bj : SPACE3D_SUB_NONE);
        return best;
    }
    for (i = 0; i < gm->n; i++) {
        S3Hit h = piece_static(c, o, c0, v, &gm->p[i], best.hit ? best.toi : ONE);
        if (h.hit && (!best.hit || h.toi < best.toi)) {
            best = h;
            bi = i;
        }
    }
    *sub = S3_SUB_PAIR(gm->compound ? bi : SPACE3D_SUB_NONE, SPACE3D_SUB_NONE);
    return best;
}

/* Narrow phase of mover m against candidate o. */
static S3Hit narrow(const Cols *c, uint32_t m, uint32_t o) {
    SuV3 c0 = rd3(c, F_PREVX, m);
    SuV3 v = su_v3_subf(rd3(c, F_POSX, m), c0);
    int64_t r = rdq(c, F_RADIUS, m);
    uint32_t ko = c->kind[o];
    SuV3 e0, e1, f0, f1;
    int mseg = mover_seg(c, m, &e0, &e1);
    int oseg = mover_seg(c, o, &f0, &f1);
    if (ko == SPACE3D_KIND_MESH) {
        return s3mesh_sweep(c->meshes, c->gate_index[o], rd3(c, F_POSX, o), rdrot(c, o), c0, v, r, e0, e1, c->mst);
    }
    if (mseg || oseg) {
        /* Capsule mover involved (query_shape). Another mover is taken at its start position with the relative
         * displacement, as for spheres. */
        if (ko == SPACE3D_KIND_MOVER) {
            SuV3 o0 = rd3(c, F_PREVX, o);
            SuV3 vr = su_v3_subf(v, su_v3_subf(rd3(c, F_POSX, o), o0));
            SuV3 a = su_v3_addf(o0, f0), b = su_v3_addf(o0, f1);
            if (!mseg) {
                return s3_sweep_sphere_capsule(c0, vr, r, a, b, rdq(c, F_RADIUS, o));
            }
            return s3_sweep_capsule_capsule(c0, vr, r, e0, e1, a, b, rdq(c, F_RADIUS, o));
        }
        if (ko == SPACE3D_KIND_SPHERE) {
            return s3_sweep_capsule_sphere(c0, v, r, e0, e1, rd3(c, F_POSX, o), rdq(c, F_RADIUS, o));
        }
        if (ko == SPACE3D_KIND_BOX) {
            return s3_sweep_capsule_obb(c0, v, r, e0, e1, rd3(c, F_POSX, o), rdrot(c, o), rd3(c, F_HALFX, o));
        } else {
            SuV3 a, b;
            capsule_ends(c, o, &a, &b);
            return s3_sweep_capsule_capsule(c0, v, r, e0, e1, a, b, rdq(c, F_RADIUS, o));
        }
    }
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

/* Index of the first gate (lowest slot) whose gate_index equals key, in gate_slot[0..ng) sorted by
 * (gate_index, slot); S3_NONE when none. */
static uint32_t find_gate(const uint32_t *gate_slot, const Cols *c, uint32_t ng, uint32_t key) {
    uint32_t lo = 0, hi = ng;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (c->gate_index[gate_slot[mid]] < key) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < ng && c->gate_index[gate_slot[lo]] == key ? gate_slot[lo] : S3_NONE;
}

static void sort_gates(uint32_t *gate_slot, const Cols *c, uint32_t ng) {
    /* Insertion sort by (gate_index, slot): gates are few; the input is in ascending slot order. */
    uint32_t i, j;
    for (i = 1; i < ng; i++) {
        uint32_t key = gate_slot[i];
        j = i;
        while (j > 0 && c->gate_index[gate_slot[j - 1u]] > c->gate_index[key]) {
            gate_slot[j] = gate_slot[j - 1u];
            j--;
        }
        gate_slot[j] = key;
    }
}

static uint32_t query_core(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx, int prox,
                           int shape, int compound) {
    Space3dCtx *ctx = host_ctx != NULL ? (Space3dCtx *)host_ctx : &g_ctx;
    Space3dScratch *s = ctx->scratch == &g_scratch ? static_scratch() : ctx->scratch;
    Cols c;
    uint64_t rows64 = in->row_count;
    uint32_t rows, r, ncoll = 0, ngate = 0, axis = 0, i;
    uint64_t pairs = 0;
    int budget_hit = 0;
    Space3dStats st;
    S3MeshStats mst;

    if (rows64 > s->max_rows) {
        return SU_E_RESOURCE_LIMIT;
    }
    rows = (uint32_t)rows64;
    if (!bind_cols(in, out, &c, prox, shape, compound)) {
        return SU_E_SCHEMA_MISMATCH;
    }
    memset(&st, 0, sizeof st);
    {
        S3MeshStats ms0;
        memset(&ms0, 0, sizeof ms0);
        mst = ms0;
    }
    c.meshes = ctx->meshes;
    c.mst = &mst;
    st.rows = rows;
    if (rows > 0u && (su_world_alive_view(w, 0, rows, s->alive) != SU_OK ||
                      su_world_generation_view(w, 0, rows, s->gen) != SU_OK)) {
        return SU_E_INTERNAL;
    }

    /* Gather collidables (ascending slot) and gates; compute bounds. */
    for (r = 0; r < rows; r++) {
        uint32_t k = c.kind[r];
        s->ncand[r] = 0;
        s->flags[r] = 0;
        if (!s->alive[r]) {
            continue;
        }
        if (is_collidable(k) || is_mesh(&c, r)) {
            row_bounds(s, &c, r, k);
            if (prox && k == SPACE3D_KIND_MOVER) {
                /* Proximity: a mover's bounds grow by its margin, so every collidable within it is a candidate. */
                int64_t mg = prox_margin_of(&c, r);
                uint32_t a;
                if (mg > 0) {
                    for (a = 0; a < 3u; a++) {
                        s->lo[a][r] = su_q32_subf(s->lo[a][r], mg);
                        s->hi[a][r] = su_q32_addf(s->hi[a][r], mg);
                    }
                }
            }
            s->coll[ncoll++] = r;
            st.movers += k == SPACE3D_KIND_MOVER;
        } else if (k >= SPACE3D_KIND_GATE_RECT && k <= SPACE3D_KIND_GATE_ELLIPSE) {
            s->gate_slot[ngate++] = r;
        }
    }
    st.collidables = ncoll;
    st.gates = ngate;
    sort_gates(s->gate_slot, &c, ngate);

    if (ctx != NULL && ctx->brute_force) {
        /* Test seam: no broadphase; the narrow phase below takes every other collidable (s->coll stays in
         * ascending slot order) with no candidate cap and no budget. */
    } else if (ncoll > 1u) {
        /* Sweep axis: largest spread of bound centres (lo + hi, saturating), ties to the lower axis. */
        int64_t best = -1;
        uint32_t a;
        for (a = 0; a < 3u; a++) {
            int64_t mn = INT64_MAX, mx = INT64_MIN, spread;
            for (i = 0; i < ncoll; i++) {
                int64_t cen = su_q32_addf(s->lo[a][s->coll[i]], s->hi[a][s->coll[i]]);
                mn = cen < mn ? cen : mn;
                mx = cen > mx ? cen : mx;
            }
            spread = su_q32_subf(mx, mn);
            if (spread > best) {
                best = spread;
                axis = a;
            }
        }
        sort_sweep(s, ncoll, axis);
        for (i = 0; i < ncoll && !budget_hit; i++) {
            uint32_t p = s->coll[i], j;
            for (j = i + 1u; j < ncoll; j++) {
                uint32_t q = s->coll[j];
                uint32_t a1 = (axis + 1u) % 3u, a2 = (axis + 2u) % 3u;
                int pm, qm;
                if (s->lo[axis][q] > s->hi[axis][p]) {
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
                if (s->lo[a1][p] > s->hi[a1][q] || s->lo[a1][q] > s->hi[a1][p] || s->lo[a2][p] > s->hi[a2][q] ||
                    s->lo[a2][q] > s->hi[a2][p]) {
                    continue;
                }
                if (pm) {
                    add_cand(s, p, q);
                }
                if (qm) {
                    add_cand(s, q, p);
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
        uint32_t pslot = S3_NONE;
        int64_t pdist = 0;
        uint32_t flags = s->flags[r];
        uint32_t gdir = SPACE3D_GATE_NONE, gslot = S3_NONE;
        int64_t gtoi = 0;
        int mover = s->alive[r] && c.kind[r] == SPACE3D_KIND_MOVER;
        int brute = ctx != NULL && ctx->brute_force;
        Geo gm, go;
        gm.special = 0;
        go.special = 0;
        if (mover && compound) {
            geo_of(&c, r, &gm);
        }
        if (mover) {
            uint32_t n = brute ? ncoll : s->ncand[r];
            if (budget_hit) {
                flags |= SPACE3D_FLAG_SWEEP_BUDGET;
            }
            /* Candidates in ascending slot order (insertion sort, n <= SPACE3D_CAND_MAX). */
            for (k = 1; k < n && !brute; k++) {
                uint32_t key = s->cand[r][k];
                j = k;
                while (j > 0 && s->cand[r][j - 1u] > key) {
                    s->cand[r][j] = s->cand[r][j - 1u];
                    j--;
                }
                s->cand[r][j] = key;
            }
            st.candidates += n;
            for (k = 0; k < n; k++) {
                uint32_t o = brute ? s->coll[k] : s->cand[r][k];
                uint32_t sub = S3_NONE;
                S3Hit h;
                if (o == r) {
                    continue;
                }
                go.special = 0;
                if (compound && c.kind[o] == SPACE3D_KIND_MOVER) {
                    geo_of(&c, o, &go);
                }
                h = gm.special || go.special ? narrow_geo(&c, r, &gm, o, &go, &sub) : narrow(&c, r, o);
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
                    best[j].sub = sub;
                    if (nbest < SPACE3D_CONTACTS) {
                        nbest++;
                    }
                }
            }
            if (prox) {
                /* Closest collidable within the margin among the same candidates (ascending slot: a strict < keeps
                 * the lowest slot among equal distances). */
                int64_t mg = prox_margin_of(&c, r);
                for (k = 0; k < n && mg > 0; k++) {
                    uint32_t o = brute ? s->coll[k] : s->cand[r][k];
                    int64_t d;
                    if (o == r) {
                        continue;
                    }
                    /* a mesh need only be searched below the best distance so far (strict < keeps ties) */
                    go.special = 0;
                    if (compound && c.kind[o] == SPACE3D_KIND_MOVER) {
                        geo_of(&c, o, &go);
                    }
                    if (gm.special || go.special) {
                        d = prox_geo(&c, r, &gm, o, &go, pslot == S3_NONE || pdist > mg ? mg : pdist);
                    } else {
                        d = prox_distance(&c, r, o, pslot == S3_NONE || pdist > mg ? mg : pdist);
                    }
                    if (d <= mg && (pslot == S3_NONE || d < pdist)) {
                        pslot = o;
                        pdist = d;
                    }
                }
            }
            st.contacts += found;
            if (found > SPACE3D_CONTACTS) {
                flags |= SPACE3D_FLAG_CONTACT_TRUNCATED;
            }
            /* Gate crossing of the centre segment against the next gate. */
            {
                uint32_t g = find_gate(s->gate_slot, &c, ngate, c.next_gate[r]);
                if (g != S3_NONE) {
                    S3GateHit gh;
                    int gshape = c.kind[g] == SPACE3D_KIND_GATE_DISC     ? S3_GATE_DISC
                                : c.kind[g] == SPACE3D_KIND_GATE_ELLIPSE ? S3_GATE_ELLIPSE
                                                                         : S3_GATE_RECT;
                    if (s3_segment_gate(rd3(&c, F_PREVX, r), rd3(&c, F_POSX, r), rd3(&c, F_POSX, g), rdrot(&c, g),
                                        rdq(&c, F_HALFX, g), rdq(&c, F_HALFY, g), rdq(&c, F_RADIUS, g), gshape,
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
            c.c_gen[k][r] = have ? s->gen[best[k].slot] : 0u;
            wrq(c.c_q[k][0], r, have ? best[k].toi : 0);
            wrq(c.c_q[k][1], r, have ? best[k].n.x : 0);
            wrq(c.c_q[k][2], r, have ? best[k].n.y : 0);
            wrq(c.c_q[k][3], r, have ? best[k].n.z : 0);
            if (compound) {
                c.c_sub[k][r] = have ? best[k].sub : S3_NONE;
            }
        }
        c.gate_dir[r] = gdir;
        c.gate_slot[r] = gslot;
        wrq(c.gate_toi, r, gtoi);
        if (prox) {
            c.prox_slot[r] = pslot;
            c.prox_gen[r] = pslot != S3_NONE ? s->gen[pslot] : 0u;
            wrq(c.prox_dist, r, pdist);
        }
    }
    st.mesh_nodes = mst.nodes;
    st.mesh_tri_tests = mst.tri_tests;
    if (ctx != NULL) {
        ctx->last = st;
    }
    return SU_OK;
}

static uint32_t query_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    return query_core(w, in, out, host_ctx, 0, 0, 0);
}

static uint32_t query_run_prox(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    return query_core(w, in, out, host_ctx, 1, 0, 0);
}

static uint32_t query_run_shape(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    return query_core(w, in, out, host_ctx, 1, 1, 0);
}

static uint32_t query_run_compound(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    return query_core(w, in, out, host_ctx, 1, 1, 1);
}
