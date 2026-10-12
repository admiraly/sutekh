/* rts.spatial host operator (M3-04, contract r4 section 4.1, 4.2; descriptor 3.1). See rts_ops.h.
 *
 * Declared reads (ascending id): kind 1, team 2, hp 3, pos_x 4, pos_y 5, range_sq 7, formation_slot 15,
 * formation_gen 16. Declared writes (write-only, so every row is stored here): anchor_x 21, anchor_y 22,
 * anchor_valid 23, target_slot 24, target_gen 25, target_dist_sq 26, target_valid 27, target_x 28,
 * target_y 29, truncated 30. Alive and generation come from the const world (4.1(a), (c)). */
#include <string.h>

#include "rts_ops.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

/* Declared read count (kind, team, hp, pos_x, pos_y, range_sq, formation_slot, formation_gen). */
#define SP_NREAD 8u

/* Declared write indices (ascending field id). */
#define SP_W_ANCHOR_X 0u
#define SP_W_ANCHOR_Y 1u
#define SP_W_ANCHOR_VALID 2u
#define SP_W_TARGET_SLOT 3u
#define SP_W_TARGET_GEN 4u
#define SP_W_TARGET_DIST 5u
#define SP_W_TARGET_VALID 6u
#define SP_W_TARGET_X 7u
#define SP_W_TARGET_Y 8u
#define SP_W_TRUNCATED 9u
#define SP_NWRITE 10u

static RtsOpsCtx g_spatial_ctx = {NULL, RTS_OPS_NO_FAULT, 0u};

/* Scratch (static, sized by RTS_OPS_MAX_ROWS; no allocation in the tick). */
static uint8_t g_alive[RTS_OPS_MAX_ROWS];
static uint32_t g_items[RTS_OPS_MAX_ROWS];
static uint32_t g_cell_start[RTS_OPS_CELLS + 1u];
static uint32_t g_cell_fill[RTS_OPS_CELLS];

static const SuHostField k_spatial_fields[SP_NREAD + SP_NWRITE] = {
    {1u, SU_TYPE_U32, SU_ACCESS_READ, "kind"},
    {2u, SU_TYPE_U32, SU_ACCESS_READ, "team"},
    {3u, SU_TYPE_U32, SU_ACCESS_READ, "hp"},
    {4u, SU_TYPE_U32, SU_ACCESS_READ, "pos_x"},
    {5u, SU_TYPE_U32, SU_ACCESS_READ, "pos_y"},
    {7u, SU_TYPE_U32, SU_ACCESS_READ, "range_sq"},
    {15u, SU_TYPE_U32, SU_ACCESS_READ, "formation_slot"},
    {16u, SU_TYPE_U32, SU_ACCESS_READ, "formation_gen"},
    {21u, SU_TYPE_U32, SU_ACCESS_WRITE, "anchor_x"},
    {22u, SU_TYPE_U32, SU_ACCESS_WRITE, "anchor_y"},
    {23u, SU_TYPE_U32, SU_ACCESS_WRITE, "anchor_valid"},
    {24u, SU_TYPE_U32, SU_ACCESS_WRITE, "target_slot"},
    {25u, SU_TYPE_U32, SU_ACCESS_WRITE, "target_gen"},
    {26u, SU_TYPE_U32, SU_ACCESS_WRITE, "target_dist_sq"},
    {27u, SU_TYPE_U32, SU_ACCESS_WRITE, "target_valid"},
    {28u, SU_TYPE_U32, SU_ACCESS_WRITE, "target_x"},
    {29u, SU_TYPE_U32, SU_ACCESS_WRITE, "target_y"},
    {30u, SU_TYPE_U32, SU_ACCESS_WRITE, "truncated"},
};

static uint32_t spatial_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx);

/* Literal: SHA-256 of rts_spatial_constants_text() (checked by the RTS native tests). */
const SuHostOpDesc g_rts_spatial_desc = {
    "rts.spatial",
    "rts",
    RTS_OPS_VERSION,
    {0xe5, 0xad, 0xe2, 0x6f, 0x1d, 0x2e, 0x1f, 0x4f, 0xf8, 0xd0, 0x08, 0xe3, 0xe1, 0x40, 0x69, 0xb6,
     0x44, 0x7f, 0xe6, 0x54, 0x83, 0x94, 0xc4, 0x34, 0x2d, 0xf9, 0x74, 0x01, 0xf7, 0xef, 0x15, 0xb2},
    SP_NREAD + SP_NWRITE,
    k_spatial_fields,
    spatial_run,
    &g_spatial_ctx,
};

RtsOpsCtx *rts_spatial_ctx(void) {
    return &g_spatial_ctx;
}

static const uint32_t *in_col(const SuInvocation *in, uint32_t id) {
    uint32_t i;
    for (i = 0; i < in->binding_count; i++) {
        if (in->bindings[i].field_id == id && in->bindings[i].row_count == in->row_count) {
            return (const uint32_t *)in->bindings[i].data;
        }
    }
    return NULL;
}

static uint32_t *out_col(SuOutputs *out, uint32_t id, uint64_t rows) {
    uint32_t i;
    for (i = 0; i < out->binding_count; i++) {
        if (out->bindings[i].field_id == id && out->bindings[i].row_count == rows) {
            return (uint32_t *)out->bindings[i].data;
        }
    }
    return NULL;
}

/* Fault seam (N9): the run returns SU_E_INTERNAL at the fault_at-th fault point it passes. */
static int seam(RtsOpsCtx *c) {
    if (c->fault_at == RTS_OPS_NO_FAULT) {
        return 0;
    }
    return c->fault_seen++ == c->fault_at;
}

typedef struct QueryState {
    uint32_t r;
    uint32_t x;
    uint32_t y;
    uint32_t team;
    uint32_t range_sq;
    uint32_t have;
    uint32_t best_slot;
    uint32_t best_dist;
    uint64_t box_total;
    uint32_t examined;
    uint32_t stopped;
} QueryState;

/* Visits one cell in ring order: counts its entries into the box and examines them while the budget lasts. */
static void visit_cell(QueryState *q, const uint32_t *px, const uint32_t *py, const uint32_t *team, uint32_t cell) {
    uint32_t i;
    uint32_t s0 = g_cell_start[cell];
    uint32_t s1 = g_cell_start[cell + 1u];
    q->box_total += (uint64_t)(s1 - s0);
    if (q->stopped) {
        return;
    }
    for (i = s0; i < s1; i++) {
        uint32_t e = g_items[i];
        uint32_t dx;
        uint32_t dy;
        uint32_t dd;
        if (q->examined == RTS_OPS_BUDGET_K) {
            q->stopped = 1u;
            break;
        }
        q->examined++;
        if (e == q->r || team[e] == q->team) {
            continue;
        }
        dx = px[e] > q->x ? px[e] - q->x : q->x - px[e];
        dy = py[e] > q->y ? py[e] - q->y : q->y - py[e];
        dd = dx * dx + dy * dy;
        if (dd <= q->range_sq) {
            if (q->have == 0u || dd < q->best_dist || (dd == q->best_dist && e < q->best_slot)) {
                q->have = 1u;
                q->best_slot = e;
                q->best_dist = dd;
            }
        }
    }
}

/* 4.1(c) for one querier. Returns SU_OK, or SU_E_INTERNAL for a range or radius outside the operator's domain. */
static uint32_t query_one(QueryState *q, const uint32_t *px, const uint32_t *py, const uint32_t *team) {
    uint32_t R;
    uint32_t rc;
    uint32_t cx;
    uint32_t cy;
    uint32_t d;
    uint32_t cyp;
    uint32_t y0;
    uint32_t y1;
    if (q->range_sq > RTS_OPS_RANGE_SQ_MAX) {
        return SU_E_INTERNAL;
    }
    R = rts_ops_isqrt(q->range_sq);
    rc = (R + 63u) >> RTS_OPS_CELL_SHIFT;
    if (rc > RTS_OPS_RC_MAX) {
        return SU_E_INTERNAL;
    }
    cx = q->x >> RTS_OPS_CELL_SHIFT;
    cy = q->y >> RTS_OPS_CELL_SHIFT;
    for (d = 0; d <= rc; d++) {
        y0 = cy >= d ? cy - d : 0u;
        y1 = (cy + d) < RTS_OPS_GRID ? cy + d : RTS_OPS_GRID - 1u;
        for (cyp = y0; cyp <= y1; cyp++) {
            if (cyp + d == cy || cy + d == cyp) {
                /* top or bottom edge of ring d: every cell of the row, ascending cx */
                uint32_t x0 = cx >= d ? cx - d : 0u;
                uint32_t x1 = (cx + d) < RTS_OPS_GRID ? cx + d : RTS_OPS_GRID - 1u;
                uint32_t cxp;
                for (cxp = x0; cxp <= x1; cxp++) {
                    visit_cell(q, px, py, team, cyp * RTS_OPS_GRID + cxp);
                }
            } else {
                /* middle rows of ring d: the left cell then the right cell */
                if (cx >= d) {
                    visit_cell(q, px, py, team, cyp * RTS_OPS_GRID + (cx - d));
                }
                if (d > 0u && (cx + d) < RTS_OPS_GRID) {
                    visit_cell(q, px, py, team, cyp * RTS_OPS_GRID + (cx + d));
                }
            }
        }
    }
    return SU_OK;
}

static uint32_t spatial_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    RtsOpsCtx *c = (RtsOpsCtx *)host_ctx;
    const uint64_t rows = in->row_count;
    uint32_t cap;
    uint32_t r;
    uint32_t s;
    const uint32_t *kind;
    const uint32_t *team;
    const uint32_t *hp;
    const uint32_t *px;
    const uint32_t *py;
    const uint32_t *range;
    const uint32_t *fslot;
    const uint32_t *fgen;
    uint32_t *o[SP_NWRITE];
    uint32_t st;

    if (c == NULL || w == NULL || out == NULL || rows == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (rows > RTS_OPS_MAX_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    cap = (uint32_t)rows;
    c->fault_seen = 0u;
    kind = in_col(in, 1u);
    team = in_col(in, 2u);
    hp = in_col(in, 3u);
    px = in_col(in, 4u);
    py = in_col(in, 5u);
    range = in_col(in, 7u);
    fslot = in_col(in, 15u);
    fgen = in_col(in, 16u);
    if (kind == NULL || team == NULL || hp == NULL || px == NULL || py == NULL || range == NULL ||
        fslot == NULL || fgen == NULL) {
        return SU_E_INTERNAL;
    }
    o[SP_W_ANCHOR_X] = out_col(out, 21u, rows);
    o[SP_W_ANCHOR_Y] = out_col(out, 22u, rows);
    o[SP_W_ANCHOR_VALID] = out_col(out, 23u, rows);
    o[SP_W_TARGET_SLOT] = out_col(out, 24u, rows);
    o[SP_W_TARGET_GEN] = out_col(out, 25u, rows);
    o[SP_W_TARGET_DIST] = out_col(out, 26u, rows);
    o[SP_W_TARGET_VALID] = out_col(out, 27u, rows);
    o[SP_W_TARGET_X] = out_col(out, 28u, rows);
    o[SP_W_TARGET_Y] = out_col(out, 29u, rows);
    o[SP_W_TRUNCATED] = out_col(out, 30u, rows);
    for (s = 0; s < SP_NWRITE; s++) {
        if (o[s] == NULL) {
            return SU_E_INTERNAL;
        }
    }

    st = su_world_alive_view(w, 0u, cap, g_alive);
    if (st != SU_OK) {
        return SU_E_INTERNAL;
    }
    for (r = 0; r < cap; r++) {
        if (g_alive[r] != 0u && (px[r] > RTS_OPS_DOMAIN_MAX || py[r] > RTS_OPS_DOMAIN_MAX)) {
            return SU_E_INTERNAL;
        }
    }

    /* (a) anchor gather. Dead rows store zeros. */
    if (seam(c)) {
        return SU_E_INTERNAL;
    }
    for (r = 0; r < cap; r++) {
        uint32_t valid = 0u;
        uint32_t ax = 0u;
        uint32_t ay = 0u;
        if (g_alive[r] != 0u && fslot[r] != RTS_OPS_NONE && fslot[r] < cap && g_alive[fslot[r]] != 0u) {
            uint32_t g = 0u;
            if (su_world_generation_view(w, fslot[r], 1u, &g) != SU_OK) {
                return SU_E_INTERNAL;
            }
            if (g == fgen[r]) {
                valid = 1u;
                ax = px[fslot[r]];
                ay = py[fslot[r]];
            }
        }
        o[SP_W_ANCHOR_X][r] = ax;
        rts_ops_note(c, SP_W_ANCHOR_X, r);
        o[SP_W_ANCHOR_Y][r] = ay;
        rts_ops_note(c, SP_W_ANCHOR_Y, r);
        o[SP_W_ANCHOR_VALID][r] = valid;
        rts_ops_note(c, SP_W_ANCHOR_VALID, r);
    }

    /* (b) grid build: counting sort, ascending slot within a cell. */
    if (seam(c)) {
        return SU_E_INTERNAL;
    }
    memset(g_cell_start, 0, sizeof(g_cell_start));
    for (r = 0; r < cap; r++) {
        if (g_alive[r] != 0u && kind[r] == RTS_OPS_KIND_UNIT && hp[r] > 0u) {
            uint32_t cell = (py[r] >> RTS_OPS_CELL_SHIFT) * RTS_OPS_GRID + (px[r] >> RTS_OPS_CELL_SHIFT);
            g_cell_start[cell + 1u]++;
        }
    }
    {
        uint32_t i;
        for (i = 1u; i <= RTS_OPS_CELLS; i++) {
            g_cell_start[i] += g_cell_start[i - 1u];
        }
    }
    memcpy(g_cell_fill, g_cell_start, sizeof(g_cell_fill));
    for (r = 0; r < cap; r++) {
        if (g_alive[r] != 0u && kind[r] == RTS_OPS_KIND_UNIT && hp[r] > 0u) {
            uint32_t cell = (py[r] >> RTS_OPS_CELL_SHIFT) * RTS_OPS_GRID + (px[r] >> RTS_OPS_CELL_SHIFT);
            g_items[g_cell_fill[cell]++] = r;
        }
    }

    /* (c) target query and (d) target gather. */
    for (r = 0; r < cap; r++) {
        uint32_t tslot = RTS_OPS_NONE;
        uint32_t tgen = 0u;
        uint32_t tdist = 0u;
        uint32_t tvalid = 0u;
        uint32_t trunc = 0u;
        uint32_t tx = 0u;
        uint32_t ty = 0u;
        if (g_alive[r] != 0u && kind[r] == RTS_OPS_KIND_UNIT && range[r] > 0u) {
            QueryState q;
            if (seam(c)) {
                return SU_E_INTERNAL;
            }
            memset(&q, 0, sizeof(q));
            q.r = r;
            q.x = px[r];
            q.y = py[r];
            q.team = team[r];
            q.range_sq = range[r];
            st = query_one(&q, px, py, team);
            if (st != SU_OK) {
                return st;
            }
            trunc = q.box_total > RTS_OPS_BUDGET_K ? 1u : 0u;
            if (q.have != 0u) {
                uint32_t g = 0u;
                if (su_world_generation_view(w, q.best_slot, 1u, &g) != SU_OK) {
                    return SU_E_INTERNAL;
                }
                tslot = q.best_slot;
                tgen = g;
                tdist = q.best_dist;
                tvalid = 1u;
                tx = px[q.best_slot];
                ty = py[q.best_slot];
            }
        } else {
            trunc = 0u;
        }
        o[SP_W_TARGET_SLOT][r] = tslot;
        rts_ops_note(c, SP_W_TARGET_SLOT, r);
        o[SP_W_TARGET_GEN][r] = tgen;
        rts_ops_note(c, SP_W_TARGET_GEN, r);
        o[SP_W_TARGET_DIST][r] = tdist;
        rts_ops_note(c, SP_W_TARGET_DIST, r);
        o[SP_W_TARGET_VALID][r] = tvalid;
        rts_ops_note(c, SP_W_TARGET_VALID, r);
        o[SP_W_TARGET_X][r] = tx;
        rts_ops_note(c, SP_W_TARGET_X, r);
        o[SP_W_TARGET_Y][r] = ty;
        rts_ops_note(c, SP_W_TARGET_Y, r);
        o[SP_W_TRUNCATED][r] = trunc;
        rts_ops_note(c, SP_W_TRUNCATED, r);
    }
    return SU_OK;
}
