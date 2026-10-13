/* T-RTS-CHURN-1: 100,000 accepted spawn/despawn operations on the 39-field, 16k RTS schema. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdio.h>
#include <stdlib.h>

#include "rts_fixture.h"
#include "world/entities/su_ents.h"

#define CAPACITY 16384u
#define OPERATIONS 100000u
#define BATCH 257u

typedef struct Model {
    RtsRow rows[CAPACITY];
    uint32_t gen[CAPACITY];
    uint32_t external[CAPACITY];
    unsigned char alive[CAPACITY];
    uint32_t count;
} Model;

static unsigned g_checks;
static unsigned g_failures;
static uint64_t g_rng = UINT64_C(0x5a71e4c8362b9d0f);
#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t random_u32(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (uint32_t)(g_rng * UINT64_C(2685821657736338717));
}

static RtsRow row_for(uint32_t serial) {
    RtsRow r = rts_row_blank();
    r.v[F_KIND] = 1u;
    r.v[F_TEAM] = serial & 3u;
    r.v[F_HP] = 1u + serial % 65535u;
    r.v[F_POS_X] = serial * 173u % 32768u;
    r.v[F_POS_Y] = serial * 311u % 32768u;
    r.v[F_SPEED] = serial % 1025u;
    r.v[F_RANGE_SQ] = serial % 262145u;
    r.v[F_RELOAD] = 1u + serial % 1024u;
    r.v[F_DAMAGE] = 1u + serial % 65535u;
    r.v[F_FLIGHT] = 1u + serial % 64u;
    r.v[F_ORDER_MODE] = serial % 3u;
    r.v[F_ORDER_X] = serial * 13u % 32768u;
    r.v[F_ORDER_Y] = serial * 23u % 32768u;
    return r;
}

/* Model allocation scans its own liveness array; no engine allocator is used as an oracle. */
static uint32_t lowest_free(const Model *m) {
    uint32_t slot;
    for (slot = 0u; slot < CAPACITY; slot++) if (!m->alive[slot]) return slot;
    return CAPACITY;
}

static void spawn(SuWorld *w, Model *m, uint32_t serial) {
    uint32_t slot = lowest_free(m);
    RtsRow row = row_for(serial);
    SuWorldSpawnValue values[RTS_F_COUNT];
    SuEntity handle = 0u;
    uint32_t ext = serial * 2654435761u;
    uint32_t field;
    uint32_t st;
    for (field = 1u; field <= RTS_F_COUNT; field++) {
        values[field - 1u].field_id = field;
        values[field - 1u].value = row.v[field];
    }
    st = su_world_spawn(w, values, RTS_F_COUNT, ext, &handle);
    CHECK(st == SU_OK && slot < CAPACITY);
    if (st != SU_OK || slot >= CAPACITY) return;
    CHECK(handle == ((uint64_t)m->gen[slot] << 32 | slot));
    m->alive[slot] = 1u;
    m->rows[slot] = row;
    m->external[slot] = ext;
    m->count++;
}

static void compare(SuWorld *w, const Model *m, uint32_t operation) {
    const SuEnts *entities = su_world_entities_committed(w);
    uint32_t slot;
    uint32_t field;
    int equal = su_ents_capacity(entities) == CAPACITY &&
                su_ents_high_water(entities) == CAPACITY &&
                su_ents_alive_count(entities) == m->count &&
                su_ents_retired_count(entities) == 0u;
    for (slot = 0u; slot < CAPACITY && equal; slot++) {
        uint32_t gen = 0u;
        uint32_t ext = 0u;
        equal = su_ents_slot_generation(entities, slot, &gen) == SU_OK && gen == m->gen[slot] &&
                su_ents_slot_alive(entities, slot) == m->alive[slot];
        if (m->alive[slot]) {
            equal = equal && su_ents_get_external(entities, ((uint64_t)gen << 32) | slot, &ext) == SU_OK &&
                    ext == m->external[slot];
        }
    }
    for (field = 1u; field <= RTS_F_COUNT && equal; field++) {
        const uint32_t *data = NULL;
        uint64_t rows = 0u;
        equal = su_world_read_committed(w, field, &data, &rows) == SU_OK && rows == CAPACITY;
        for (slot = 0u; slot < CAPACITY && equal; slot++) {
            equal = data[slot] == (m->alive[slot] ? m->rows[slot].v[field] : 0u);
        }
    }
    if (!equal) fprintf(stderr, "RTS churn model mismatch after operation %u\n", operation);
    CHECK(equal);
}

int main(void) {
    SuWorld *w = rts_world_create(CAPACITY);
    Model *m = calloc(1u, sizeof *m);
    uint32_t serial = 0u;
    uint32_t operations = 0u;
    uint32_t slot;
    CHECK(w != NULL && m != NULL);
    if (w == NULL || m == NULL) { su_world_destroy(w); free(m); return 1; }
    for (slot = 0u; slot < CAPACITY; slot++) m->gen[slot] = 1u;
    for (slot = 0u; slot < CAPACITY; slot++) spawn(w, m, serial++);
    compare(w, m, operations);
    while (operations < OPERATIONS && g_failures == 0u) {
        uint32_t n = (OPERATIONS - operations) / 2u;
        uint32_t j;
        SuEntity stale[BATCH];
        if (n > BATCH) n = BATCH;
        /* Create many randomly distributed holes; refill them in lowest-slot order. */
        for (j = 0u; j < n; j++) {
            do { slot = random_u32() % CAPACITY; } while (!m->alive[slot]);
            stale[j] = ((uint64_t)m->gen[slot] << 32) | slot;
            CHECK(su_world_despawn(w, stale[j]) == SU_OK);
            m->alive[slot] = 0u;
            m->gen[slot]++;
            m->external[slot] = 0u;
            m->count--;
            operations++;
        }
        compare(w, m, operations);
        for (j = 0u; j < n; j++) { spawn(w, m, serial++); operations++; }
        for (j = 0u; j < n; j++) CHECK(su_world_despawn(w, stale[j]) == SU_E_STALE_HANDLE);
        compare(w, m, operations);
    }
    CHECK(operations == OPERATIONS && m->count == CAPACITY);
    su_world_destroy(w);
    free(m);
    printf("test_rts_churn: %u accepted churn operations, %u checks, %u failures\n",
           operations, g_checks, g_failures);
    return g_failures != 0u;
}
