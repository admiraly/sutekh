/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.4 through the in-process host API (include/sutekh/host.h), where systems run in chunks of SU_CHUNK_SLOTS
 * slots: tests/fixtures/ir04/claim runs only on rows with kind == 2 (row filter), sets owner to the lowest slot
 * whose "want" names the row's own slot (find_eq_u32: the first-claimant-by-slot-order pattern) and far to the
 * "want" of slot 2099, which lies in the third chunk (gather_u32). 2100 entities span three chunks, so a gather that
 * saw only its own chunk would differ. Expected values are recomputed here from the committed columns; filtered-out
 * rows must keep their spawn values. Two worlds stepped identically must have equal digests. Run from the
 * repository root. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/entities.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CAP 2500u
#define LIVE 2100u
_Static_assert(CAP > 2u * SU_CHUNK_SLOTS, "the table must span three entity chunks");

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *b;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    b = malloc((size_t)sz + 1u);
    *len = b != NULL ? fread(b, 1, (size_t)sz, f) : 0u;
    fclose(f);
    return b;
}

static SuHostWorld *make_world(SuHostCapsule *cap) {
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t i;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = cap;
    d.max_entities = CAP;
    d.tick_rate_hz = 60;
    if (su_host_world_create(&d, &w, &e) != SU_OK) {
        fprintf(stderr, "create: %s\n", e.message);
        return NULL;
    }
    for (i = 0; i < LIVE; i++) {
        /* Every fifth entity is a resource (kind 2, wants nothing); the others claim a resource slot. */
        uint64_t ent = 0;
        SuHostValue v[4];
        int res = i % 5u == 0u;
        v[0].field_id = 1u;
        v[0].value = res ? 2u : 1u;
        v[1].field_id = 2u;
        v[1].value = res ? 0xFFFFFFFEu : ((i * 7u) % 420u) * 5u;
        v[2].field_id = 3u;
        v[2].value = 0x55u + i;
        v[3].field_id = 4u;
        v[3].value = 0x66u;
        if (su_host_spawn(w, v, 4u, i + 1u, &ent, &e) != SU_OK) {
            fprintf(stderr, "spawn: %s\n", e.message);
            su_host_world_destroy(w);
            return NULL;
        }
    }
    return w;
}

int main(void) {
    SuHostCapsule cap;
    SuHostWorld *w, *w2;
    SuHostError e;
    uint64_t done = 0;
    static uint32_t kind[CAP], want[CAP], owner[CAP], far[CAP];
    uint8_t d1[32], d2[32];
    uint32_t r, s, resources = 0, claimed = 0;

    memset(&cap, 0, sizeof cap);
    cap.capsule_json = slurp("tests/fixtures/ir04/claim.capsule.json", &cap.capsule_len);
    cap.lir_json = slurp("tests/fixtures/ir04/claim.lir.json", &cap.lir_len);
    if (cap.capsule_json == NULL || cap.lir_json == NULL) {
        fprintf(stderr, "run from the repository root\n");
        return 1;
    }
    /* CAP > 2 * SU_CHUNK_SLOTS: three chunks (static assertion above). */
    w = make_world(&cap);
    w2 = make_world(&cap);
    CHECK(w != NULL && w2 != NULL);
    if (w == NULL || w2 == NULL) {
        return 1;
    }
    CHECK(su_host_step(w, 2, &done, &e) == SU_OK && done == 2u);
    CHECK(su_host_step(w2, 2, &done, &e) == SU_OK && done == 2u);
    CHECK(su_host_read(w, 1u, 0, CAP, kind) == SU_OK);
    CHECK(su_host_read(w, 2u, 0, CAP, want) == SU_OK);
    CHECK(su_host_read(w, 3u, 0, CAP, owner) == SU_OK);
    CHECK(su_host_read(w, 4u, 0, CAP, far) == SU_OK);
    for (r = 0; r < LIVE; r++) {
        if (kind[r] == 2u) {
            uint32_t first = SU_FIND_NONE;
            for (s = 0; s < CAP && s < SU_FIND_SPAN_MAX; s++) {
                if (want[s] == r) {
                    first = s;
                    break;
                }
            }
            resources++;
            claimed += first != SU_FIND_NONE;
            CHECK(owner[r] == first);
            CHECK(far[r] == want[2099]);
        } else {
            CHECK(owner[r] == 0x55u + r); /* filtered out: spawn values kept */
            CHECK(far[r] == 0x66u);
        }
    }
    CHECK(resources == LIVE / 5u);
    CHECK(claimed > 0u && claimed < resources);
    CHECK(su_host_digest(w, d1) == SU_OK && su_host_digest(w2, d2) == SU_OK && memcmp(d1, d2, sizeof d1) == 0);
    su_host_world_destroy(w);
    su_host_world_destroy(w2);
    free((void *)cap.capsule_json);
    free((void *)cap.lir_json);
    printf("test_host04: %u resources, %u claimed; %d checks, %d failures\n", (unsigned)resources, (unsigned)claimed,
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
