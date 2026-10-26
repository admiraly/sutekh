/* Host operators built into the in-process host API (include/sutekh/host.h, "host operators"). Only operators
 * that can give every world its own scratch are listed: the host API promises that distinct worlds are
 * independent, which a process-wide scratch (the RTS operators of the worker) cannot keep. Operators a game
 * registers at run time live in su_host_registered.c; su_host_operator_find looks here first, then there. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "host/su_host_internal.h"

#include <string.h>

#include "space3d_mesh.h"
#include "space3d_ops.h"
#include "sutekh/status.h"

static uint32_t space3d_create(const HostOperator *op, uint64_t rows, void **inst) {
    Space3dCtx *ctx = NULL;
    uint32_t st;
    (void)op;
    if (rows == 0u || rows > SPACE3D_MAX_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = space3d_query_instance_create((uint32_t)rows, &ctx);
    *inst = ctx;
    return st;
}

/* Static mesh set (space3d_mesh.h): parsed once per su_host_set_operator_data call, shared by its instances. */
static uint32_t space3d_data(const HostOperator *op, void *inst, const uint8_t *data, size_t len, void **shared,
                             char *why, size_t why_cap) {
    S3MeshSet *set = (S3MeshSet *)*shared;
    (void)op;
    if (set == NULL) {
        uint32_t st = s3mesh_set_create(data, len, &set, why, why_cap);
        if (st != SU_OK) {
            return st;
        }
        *shared = set;
    }
    return space3d_query_instance_set_meshes((Space3dCtx *)inst, set);
}

static void space3d_data_release(void *shared) {
    s3mesh_set_release((S3MeshSet *)shared);
}

static void space3d_counters(const void *inst, SuHostOperatorCounters *out) {
    const Space3dCtx *ctx = (const Space3dCtx *)inst;
    const Space3dStats *l = &ctx->last;
    out->runs = ctx->total.runs;
    out->rows_truncated = l->rows_truncated;
    out->rows_cand_truncated = l->rows_cand_truncated;
    out->rows_contact_truncated = l->rows_contact_truncated;
    out->rows_budget = l->rows_budget;
    out->pairs_examined = l->sweep_pairs;
    out->pair_budget = l->pair_budget;
    out->pairs_skipped = l->pairs_skipped;
    out->budget_hit = l->budget_hit;
    out->runs_truncated = ctx->total.runs_truncated;
    out->runs_budget = ctx->total.runs_budget;
    out->total_rows_truncated = ctx->total.rows_truncated;
    out->total_rows_budget = ctx->total.rows_budget;
    out->total_pairs_skipped = ctx->total.pairs_skipped;
}

static const HostOperator k_operators[] = {
    {&g_space3d_query_desc, "space3d_ops_v1", space3d_create, space3d_query_instance_destroy, space3d_data,
     space3d_data_release, NULL, NULL, NULL, NULL, NULL, NULL, space3d_counters},
    {&g_space3d_query_prox_desc, "space3d_ops_v1", space3d_create, space3d_query_instance_destroy, space3d_data,
     space3d_data_release, NULL, NULL, NULL, NULL, NULL, NULL, space3d_counters},
    {&g_space3d_query_shape_desc, "space3d_ops_v1", space3d_create, space3d_query_instance_destroy, space3d_data,
     space3d_data_release, NULL, NULL, NULL, NULL, NULL, NULL, space3d_counters},
    {&g_space3d_query_compound_desc, "space3d_ops_v1", space3d_create, space3d_query_instance_destroy, space3d_data,
     space3d_data_release, NULL, NULL, NULL, NULL, NULL, NULL, space3d_counters},
    /* space3d_ops_v2 (issue 163): the same variants with the grid broadphase and the local pair budget. */
    {&g_space3d_query2_desc, "space3d_ops_v2", space3d_create, space3d_query_instance_destroy, space3d_data,
     space3d_data_release, NULL, NULL, NULL, NULL, NULL, NULL, space3d_counters},
    {&g_space3d_query2_prox_desc, "space3d_ops_v2", space3d_create, space3d_query_instance_destroy, space3d_data,
     space3d_data_release, NULL, NULL, NULL, NULL, NULL, NULL, space3d_counters},
    {&g_space3d_query2_shape_desc, "space3d_ops_v2", space3d_create, space3d_query_instance_destroy, space3d_data,
     space3d_data_release, NULL, NULL, NULL, NULL, NULL, NULL, space3d_counters},
    {&g_space3d_query2_compound_desc, "space3d_ops_v2", space3d_create, space3d_query_instance_destroy,
     space3d_data, space3d_data_release, NULL, NULL, NULL, NULL, NULL, NULL, space3d_counters},
};

#define HOST_OPERATOR_COUNT (sizeof k_operators / sizeof k_operators[0])

const HostOperator *su_host_builtin_find(const char *system_id) {
    size_t i;
    if (system_id == NULL) {
        return NULL;
    }
    for (i = 0; i < HOST_OPERATOR_COUNT; i++) {
        if (strcmp(k_operators[i].desc->system_id, system_id) == 0) {
            return &k_operators[i];
        }
    }
    return NULL;
}

uint32_t su_host_builtin_count(void) {
    return (uint32_t)HOST_OPERATOR_COUNT;
}

const HostOperator *su_host_operator_find(const char *system_id) {
    const HostOperator *op = su_host_builtin_find(system_id);
    return op != NULL ? op : su_host_registered_find(system_id);
}

/* Built-in operators at 0..HOST_OPERATOR_COUNT-1, then registered ones in registration order (append-only). */
uint32_t su_host_operator_count(void) {
    return (uint32_t)HOST_OPERATOR_COUNT + su_host_registered_count();
}

uint32_t su_host_operator_info(uint32_t index, const char **system_id, const char **operator_name) {
    const HostOperator *op;
    if (index < HOST_OPERATOR_COUNT) {
        op = &k_operators[index];
    } else {
        op = su_host_registered_at(index - (uint32_t)HOST_OPERATOR_COUNT);
        if (op == NULL) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (system_id != NULL) {
        *system_id = op->desc->system_id;
    }
    if (operator_name != NULL) {
        *operator_name = op->operator_name;
    }
    return SU_OK;
}
