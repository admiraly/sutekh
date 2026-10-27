/* rts.scatter host operator (M3-04, contract r4 section 4.3; descriptor 3.1). See rts_ops.h.
 *
 * Declared reads (ascending id): kind 1, dmg_out 36, dmg_target 37, dmg_target_gen 38, and incoming 39
 * (read_write: the working value is seeded into staging by run_host, so the operator adds to staging).
 * Declared write: incoming 39 (only the target rows change; the other rows keep their seeded value).
 * Alive and generation come from the const world. */
#include <string.h>

#include "rts_ops.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

#define SC_NREAD 4u
#define SC_NWRITE 1u
#define SC_W_INCOMING 0u

static RtsOpsCtx g_scatter_ctx = {NULL, RTS_OPS_NO_FAULT, 0u};

static uint8_t g_alive[RTS_OPS_MAX_ROWS];

static const SuHostField k_scatter_fields[SC_NREAD + 1u] = {
    {1u, SU_TYPE_U32, SU_ACCESS_READ, "kind"},
    {36u, SU_TYPE_U32, SU_ACCESS_READ, "dmg_out"},
    {37u, SU_TYPE_U32, SU_ACCESS_READ, "dmg_target"},
    {38u, SU_TYPE_U32, SU_ACCESS_READ, "dmg_target_gen"},
    {39u, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "incoming"},
};

static uint32_t scatter_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx);

/* Literal: SHA-256 of rts_scatter_constants_text() (checked by the RTS native tests). */
const SuHostOpDesc g_rts_scatter_desc = {
    "rts.scatter",
    "rts",
    RTS_OPS_VERSION,
    {0xbd, 0xd4, 0xa9, 0x7f, 0x87, 0xd3, 0xac, 0x5d, 0xd3, 0x32, 0x4f, 0x3c, 0x8d, 0xb2, 0x01, 0x34,
     0xa7, 0xe9, 0xf1, 0x2f, 0x5e, 0x11, 0x6f, 0xba, 0xae, 0xd0, 0x6f, 0x55, 0x5b, 0x9d, 0x18, 0xca},
    SC_NREAD + 1u,
    k_scatter_fields,
    scatter_run,
    &g_scatter_ctx,
    0u,       /* no target set (issue 140) */
    NULL,
    NULL,
};

RtsOpsCtx *rts_scatter_ctx(void) {
    return &g_scatter_ctx;
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

static int seam(RtsOpsCtx *c) {
    if (c->fault_at == RTS_OPS_NO_FAULT) {
        return 0;
    }
    return c->fault_seen++ == c->fault_at;
}

static uint32_t scatter_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    RtsOpsCtx *c = (RtsOpsCtx *)host_ctx;
    const uint64_t rows = in->row_count;
    uint32_t cap;
    uint32_t r;
    const uint32_t *kind;
    const uint32_t *dout;
    const uint32_t *dtgt;
    const uint32_t *dgen;
    uint32_t *inc;

    if (c == NULL || w == NULL || out == NULL || rows == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (rows > RTS_OPS_MAX_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    cap = (uint32_t)rows;
    c->fault_seen = 0u;
    kind = in_col(in, 1u);
    dout = in_col(in, 36u);
    dtgt = in_col(in, 37u);
    dgen = in_col(in, 38u);
    inc = out_col(out, 39u, rows);
    if (kind == NULL || dout == NULL || dtgt == NULL || dgen == NULL || inc == NULL) {
        return SU_E_INTERNAL;
    }
    if (su_world_alive_view(w, 0u, cap, g_alive) != SU_OK) {
        return SU_E_INTERNAL;
    }
    /* Shooters in ascending slot order (4.3). Only the target rows are stored. */
    for (r = 0; r < cap; r++) {
        uint32_t t;
        uint32_t amount;
        if (g_alive[r] == 0u) {
            continue;
        }
        t = dtgt[r];
        amount = dout[r];
        if (t == RTS_OPS_NONE || amount == 0u) {
            continue;
        }
        if (seam(c)) {
            return SU_E_INTERNAL;
        }
        if (t < cap && g_alive[t] != 0u && kind[t] == RTS_OPS_KIND_UNIT) {
            uint32_t g = 0u;
            if (su_world_generation_view(w, t, 1u, &g) != SU_OK) {
                return SU_E_INTERNAL;
            }
            if (g == dgen[r]) {
                inc[t] = rts_ops_sat_add(inc[t], amount);
                rts_ops_note(c, SC_W_INCOMING, t);
            }
        }
    }
    return SU_OK;
}
