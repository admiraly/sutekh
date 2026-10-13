/* Reject impossible queue dimensions before any allocation or per-system cap-array read. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "world/entities/su_cmdq.h"

static unsigned g_calls;
static unsigned g_checks;
static unsigned g_failed;

#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Refuse large requests rather than paging the host if the admission guard regresses. */
void *su_admission_malloc(size_t n) {
    g_calls++;
    return n > 1024u * 1024u ? NULL : malloc(n);
}

void *su_admission_calloc(size_t n, size_t size) {
    g_calls++;
    if (size != 0u && n > (1024u * 1024u) / size) {
        return NULL;
    }
    return calloc(n, size);
}

static void refused(uint32_t systems, uint64_t slots, uint64_t chunk) {
    SuCmdQ *q = NULL;
    uint32_t cap = 1u;
    g_calls = 0u;
    CHECK(su_cmdq_create(systems, slots, chunk, 1u, 1u, 1u, 1u, &q) == SU_E_RESOURCE_LIMIT);
    CHECK(q == NULL);
    CHECK(g_calls == 0u);
    su_cmdq_destroy(q);

    /* Exactly one cap element is supplied: impossible geometry must be rejected before
     * consulting arrays whose declared system count cannot be admitted. */
    g_calls = 0u;
    CHECK(su_cmdq_create_ex(systems, slots, chunk, &cap, &cap, 1u, 1u, &q) == SU_E_RESOURCE_LIMIT);
    CHECK(q == NULL);
    CHECK(g_calls == 0u);
    su_cmdq_destroy(q);
}

int main(void) {
    SuCmdQ *q = NULL;
    SuCmdView v;
    refused(1u << 30, 1u << 30, 1u);
    refused((uint32_t)SU_CMDQ_MAX_BUFFERS + 1u, 1u, 1u);
    refused(1u, SU_CMDQ_MAX_BUFFERS + 1u, 1u);
    refused(2u, SU_CMDQ_MAX_BUFFERS, 1u);
    {
        uint32_t cap = 0u;
        g_calls = 0u;
        CHECK(su_cmdq_create_ex_checked(2u, 4u, 1u, &cap, 1u, &cap, 1u, 4u, 4u, &q)
              == SU_E_INVALID_ARGUMENT);
        CHECK(q == NULL && g_calls == 0u);
    }
    g_calls = 0u;
    CHECK(su_cmdq_create(1u, 0u, 1u, 1u, 1u, 1u, 1u, &q) == SU_E_INVALID_ARGUMENT);
    CHECK(g_calls == 0u && q == NULL);
    CHECK(su_cmdq_create(1u, 4u, 1u, 1u, 1u, 4u, 4u, &q) == SU_OK);
    CHECK(g_calls > 0u);
    CHECK(su_cmdq_append_struct(q, 0u, 0u, 0u, 0u, SU_STRUCT_DESPAWN, 1u, 0u, 0u) == SU_OK);
    CHECK(su_cmdq_merge(q, &v) == SU_OK && v.n_cmds == 1u);
    su_cmdq_destroy(q);
    printf("test_cmdq_admission: %u checks, %u failures\n", g_checks, g_failed);
    return g_failed != 0u;
}
