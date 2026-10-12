/* Regression: su_world_create_ext must reject a retired field ID that is a live field, even when
 * ext->defaults is NULL. The header says NULL defaults mean "all 0", so it is a valid input, and
 * the retired-ID checks must still run. Before the fix, su_check_ext returned SU_OK early.
 *
 * Run from the repository root (or any directory: the test uses no fixtures).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK_ST(expr, want)                                                                  \
    do {                                                                                      \
        uint32_t got__ = (expr);                                                              \
        g_checks++;                                                                           \
        if (got__ != (want)) {                                                                \
            fprintf(stderr, "%s:%d: %s returned %u, want %u\n", __FILE__, __LINE__, #expr,    \
                    (unsigned)got__, (unsigned)(want));                                       \
            g_failures++;                                                                     \
        }                                                                                     \
    } while (0)

int main(void) {
    SuWorldFieldDesc fields[1];
    SuWorldDesc desc;
    SuWorldSchemaExt ext;
    const uint32_t retired[1] = {7u};
    SuWorld *w = NULL;

    fields[0].id = 7u;
    fields[0].type = SU_TYPE_U32;
    fields[0].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
    fields[0].name = "hp";

    memset(&desc, 0, sizeof desc);
    desc.schema_id = "test_schema";
    desc.schema_version = 1u;
    desc.layout_generation = 0u;
    desc.row_count = 0u;
    desc.entity_ids = NULL;
    desc.fields = fields;
    desc.field_count = 1u;

    /* Live field 7 is also retired; defaults NULL. Must be refused. */
    memset(&ext, 0, sizeof ext);
    ext.defaults = NULL;
    ext.retired_ids = retired;
    ext.retired_count = 1u;
    ext.lineage = NULL;
    ext.lineage_count = 0u;
    w = NULL;
    CHECK_ST(su_world_create_ext(&desc, 0u, &ext, &w), SU_E_SCHEMA_MISMATCH);
    if (w != NULL) {
        su_world_destroy(w);
    }

    /* Control: the same retired ID list with no collision is accepted, so the test is not vacuous. */
    {
        const uint32_t retired_ok[1] = {9u};
        SuWorldSchemaExt ok_ext;
        memset(&ok_ext, 0, sizeof ok_ext);
        ok_ext.retired_ids = retired_ok;
        ok_ext.retired_count = 1u;
        w = NULL;
        CHECK_ST(su_world_create_ext(&desc, 0u, &ok_ext, &w), SU_OK);
        if (w != NULL) {
            su_world_destroy(w);
        }
    }

    if (g_failures != 0) {
        fprintf(stderr, "test_world_ext_retired: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_world_ext_retired: %d checks passed\n", g_checks);
    return 0;
}
