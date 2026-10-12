/* Regression: su_world_restore_canonical (V1 path) must not dereference a NULL desc->entity_ids
 * when desc->row_count is nonzero. The header of su_world_create allows NULL entity_ids only
 * when row_count == 0, so a NULL array with rows > 0 is an invalid argument. Before the fix, the
 * ID comparison loop dereferenced NULL and crashed.
 *
 * Run from the repository root (or any directory: the test uses no fixtures).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

int main(void) {
    /* V1 layout: "SUWSTATE\0" (9) + u32 version (4) + u64 tick (8) + u64 rows (8) + u32 fc (4),
     * then rows u32 entity IDs. One row and no fields: 33 + 4 = 37 bytes. */
    uint8_t buf[37];
    SuWorldDesc desc;
    SuWorld *w = (SuWorld *)(uintptr_t)1;
    uint32_t st;

    memset(buf, 0, sizeof buf);
    memcpy(buf, "SUWSTATE", 8);
    put_u32(buf + 9, 1u);
    put_u64(buf + 13, 0u);
    put_u64(buf + 21, 1u);
    put_u32(buf + 29, 0u);
    put_u32(buf + 33, 1u);

    memset(&desc, 0, sizeof desc);
    desc.schema_id = "test_schema";
    desc.schema_version = 1u;
    desc.layout_generation = 0u;
    desc.row_count = 1u;
    desc.entity_ids = NULL;
    desc.fields = NULL;
    desc.field_count = 0u;

    st = su_world_restore_canonical(&desc, buf, sizeof buf, &w);
    CHECK(st == SU_E_INVALID_ARGUMENT);
    CHECK(w == NULL);

    if (g_failures != 0) {
        fprintf(stderr, "test_world_restore_null: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_world_restore_null: %d checks passed\n", g_checks);
    return 0;
}
