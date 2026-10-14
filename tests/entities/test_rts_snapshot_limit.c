/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
/* Runtime limits v2: preserve the historical lower-bound finding and verify larger captures.
 * Library-level snapshot evidence; this is not an accepted supervisor load at scale. */
#include <stdio.h>
#include <stdlib.h>
#include "rts_fixture.h"
#include "world/snapshot.h"

static unsigned checks, failures;
static int restore_large;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void capture(uint32_t rows, uint32_t expected) {
    SuWorldFieldDesc fields[RTS_F_COUNT];
    SuWorldDesc desc = {0};
    SuWorld *world = NULL;
    SuSnapshot snapshot = {0};
    uint32_t *ids = malloc((size_t)rows * sizeof *ids);
    uint32_t st;
    CHECK(ids != NULL);
    if (ids == NULL) return;
    for (uint32_t i = 0; i < rows; i++) ids[i] = i + 1u;
    for (uint32_t i = 0; i < RTS_F_COUNT; i++) {
        fields[i] = (SuWorldFieldDesc){i+1u, SU_TYPE_U32, SU_ACCESS_READ, k_rts_names[i+1u]};
    }
    desc.schema_id = "rts"; desc.schema_version = 1u; desc.layout_generation = 1u;
    desc.row_count = rows; desc.entity_ids = ids; desc.fields = fields; desc.field_count = RTS_F_COUNT;
    st = su_world_create(&desc, &world);
    CHECK(st == SU_OK);
    if (world != NULL) {
        CHECK(su_ents_high_water(su_world_entities_committed(world)) == rows);
        st = su_snapshot_capture(world, &desc, "world-1.r1", NULL, 0u, &snapshot);
        printf("rows=%u capture_status=%u bytes=%zu cap=%u\n", rows, st, snapshot.len, SU_SNAPSHOT_MAX_BYTES);
        CHECK(st == expected);
        if (st == SU_OK) CHECK(snapshot.bytes != NULL && snapshot.len <= SU_SNAPSHOT_MAX_BYTES);
        else CHECK(snapshot.bytes == NULL);
        if (restore_large && rows == 1000000u && st == SU_OK) {
            SuWorld *restored = NULL;
            SuSnapshot again = {0};
            SuSnapshotError error = {{0}};
            su_world_destroy(world);
            world = NULL; /* replacement workers start without the failed world's allocation */
            st = su_snapshot_restore_world(snapshot.bytes, snapshot.len, NULL, 1, &restored, NULL, &error);
            printf("large restore status=%u error=%s\n", st, error.message);
            fflush(stdout);
            CHECK(st == SU_OK && restored != NULL);
            if (restored != NULL) {
                CHECK(su_snapshot_capture(restored, &desc, "world-1.r1", NULL, 0u, &again) == SU_OK);
                CHECK(again.len == snapshot.len && memcmp(again.digest, snapshot.digest, 32u) == 0);
                su_snapshot_destroy(&again);
                su_world_destroy(restored);
            }
        }
        su_snapshot_destroy(&snapshot);
        su_world_destroy(world);
    }
    free(ids);
}

int main(int argc, char **argv) {
    restore_large = argc == 2 && strcmp(argv[1], "--restore-large") == 0;
    /* Even ignoring headers and events, minimum decimal JSON widths are:
     * 39 columns: 39*(2*N-1); entity IDs: 2*N-1; slots: 8*N-1.
     * [1,1,0] is seven bytes; commas add N-1. Larger numbers only grow it.
     * Thus 88*N-41 is a conservative lower bound, not a benchmark estimate. */
    uint64_t lower_bound = UINT64_C(88) * 838860u - 41u;
    CHECK(lower_bound > 64u * 1024u * 1024u); /* old policy could not capture it */
    CHECK(lower_bound < SU_SNAPSHOT_MAX_BYTES);
    printf("minimum_payload_838860=%llu\n", (unsigned long long)lower_bound);
    capture(65536u, SU_OK);
    capture(838860u, SU_OK);
    capture(1000000u, SU_OK);
    printf("snapshot limit: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
