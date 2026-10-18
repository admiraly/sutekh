/* Per-system structural and event caps (OD-20, M3 contract 9.5). Build and run:
 *   gcc -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -Iinclude -Isrc \
 *       tests/entities/test_cmdq_percap.c src/world/entities/su_cmdq.c -o test_cmdq_percap
 * Checks: a system with cap 0 gets no buffer and any append to it overflows; a system with a cap
 * gets its own capacity; uniform su_cmdq_create equals su_cmdq_create_ex with equal arrays; the
 * memory report counts the per-system cap arrays. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/entities.h"
#include "sutekh/status.h"
#include "world/entities/su_cmdq.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                        \
    } while (0)

static void test_zero_cap_system(void) {
    /* 3 systems, 2500 slots, chunks of 1024. System 1 has no structural cap (OD-20: not an entity-op system). */
    uint32_t ps[3] = {1u, 0u, 1u};
    uint32_t pe[3] = {0u, 0u, 0u};
    SuCmdQ *q = NULL;
    SuCmdQMemory m;
    uint32_t st;

    st = su_cmdq_create_ex(3u, 2500u, 1024u, ps, pe, 2500u, 5000u, &q);
    CHECK(st == SU_OK && q != NULL);
    if (q == NULL) {
        return;
    }
    /* Only systems 0 and 2 get structural capacity: 2500 each, summed over chunks. */
    CHECK(su_cmdq_memory(q, &m) == SU_OK);
    CHECK(m.structural == 2u * 2500u * sizeof(SuStructCmd));
    /* No event capacity at all: the report keeps its one-element minimum. */
    CHECK(m.events == sizeof(SuEvent));
    /* An append to the zero-cap system overflows and is refused. */
    CHECK(su_cmdq_append_struct(q, 1u, 0u, 0u, 0u, SU_EVENT_DESPAWNED, 0u, 0u, 0u) == SU_E_RESOURCE_LIMIT);
    CHECK(su_cmdq_overflowed(q) != 0);
    su_cmdq_destroy(q);
}

static void test_uniform_equals_ex(void) {
    uint32_t ps[2] = {2u, 2u};
    uint32_t pe[2] = {1u, 1u};
    SuCmdQ *a = NULL;
    SuCmdQ *b = NULL;
    SuCmdQMemory ma = {0};
    SuCmdQMemory mb = {0};

    CHECK(su_cmdq_create(2u, 1500u, 1024u, 2u, 1u, 3000u, 3000u, &a) == SU_OK);
    CHECK(su_cmdq_create_ex(2u, 1500u, 1024u, ps, pe, 3000u, 3000u, &b) == SU_OK);
    CHECK(a != NULL && b != NULL);
    if (a != NULL && b != NULL) {
        CHECK(su_cmdq_memory(a, &ma) == SU_OK && su_cmdq_memory(b, &mb) == SU_OK);
        CHECK(ma.structural == mb.structural && ma.events == mb.events && ma.total == mb.total);
    }
    su_cmdq_destroy(a);
    su_cmdq_destroy(b);
}

static void test_arguments(void) {
    uint32_t ps[1] = {65u};
    uint32_t pe[1] = {0u};
    SuCmdQ *q = NULL;

    CHECK(su_cmdq_create_ex(1u, 10u, 10u, ps, pe, 10u, 10u, &q) == SU_E_INVALID_ARGUMENT);
    CHECK(q == NULL);
    CHECK(su_cmdq_create_ex(1u, 10u, 10u, NULL, pe, 10u, 10u, &q) == SU_E_INVALID_ARGUMENT);
    CHECK(su_cmdq_create(0u, 10u, 10u, 1u, 1u, 10u, 10u, &q) == SU_E_INVALID_ARGUMENT);
}

int main(void) {
    test_zero_cap_system();
    test_uniform_equals_ex();
    test_arguments();
    printf("test_cmdq_percap: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
