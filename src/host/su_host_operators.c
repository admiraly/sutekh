/* Host operators available to the in-process host API (include/sutekh/host.h, "host operators"). Only operators
 * that can give every world its own scratch are listed: the host API promises that distinct worlds are
 * independent, which a process-wide scratch (the RTS operators of the worker) cannot keep. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "host/su_host_internal.h"

#include <string.h>

#include "space3d_ops.h"
#include "sutekh/status.h"

static uint32_t space3d_create(uint64_t rows, void **inst) {
    Space3dCtx *ctx = NULL;
    uint32_t st;
    if (rows == 0u || rows > SPACE3D_MAX_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = space3d_query_instance_create((uint32_t)rows, &ctx);
    *inst = ctx;
    return st;
}

static const HostOperator k_operators[] = {
    {&g_space3d_query_desc, "space3d_ops_v1", space3d_create, space3d_query_instance_destroy},
    {&g_space3d_query_prox_desc, "space3d_ops_v1", space3d_create, space3d_query_instance_destroy},
};

#define HOST_OPERATOR_COUNT (sizeof k_operators / sizeof k_operators[0])

const HostOperator *su_host_operator_find(const char *system_id) {
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

uint32_t su_host_operator_count(void) {
    return (uint32_t)HOST_OPERATOR_COUNT;
}

uint32_t su_host_operator_info(uint32_t index, const char **system_id, const char **operator_name) {
    if (index >= HOST_OPERATOR_COUNT) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (system_id != NULL) {
        *system_id = k_operators[index].desc->system_id;
    }
    if (operator_name != NULL) {
        *operator_name = k_operators[index].operator_name;
    }
    return SU_OK;
}
