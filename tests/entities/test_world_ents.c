/* M2-00c1 world integration tests: entity table inside su_world (spawn, despawn, handles, dead-row
 * zeroing, alive/generation views, canonical v2 dumps and their strict restore, the v1 restore path),
 * with a churn run against a reference model. Self-contained assert harness.
 * Usage: test_world_ents [path/to/repo]  (default: current directory; reads tests/fixtures). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/entities.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "world/su_world.h"

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

#define CHECK_ST(expr, want)                                                        \
    do {                                                                            \
        uint32_t got_ = (expr);                                                     \
        g_checks++;                                                                 \
        if (got_ != (want)) {                                                       \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: %s -> %s, want %s\n", __FILE__, __LINE__, #expr, \
                    su_status_name(got_), su_status_name(want));                    \
        }                                                                           \
    } while (0)

#define ID_A 10u
#define ID_B 20u
#define ID_C 5u

static const SuWorldFieldDesc k_fields[] = {
    {ID_A, SU_TYPE_U32, SU_ACCESS_READ, "a"},
    {ID_B, SU_TYPE_U32, SU_ACCESS_WRITE, "b"},
    {ID_C, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "c"},
};
#define NFIELDS 3u

static const uint32_t k_ids4[4] = {101u, 102u, 103u, 104u};
static const uint32_t k_a4[4] = {1u, 2u, 3u, 4u};
static const uint32_t k_b4[4] = {5u, 6u, 7u, 8u};
static const uint32_t k_c4[4] = {9u, 10u, 11u, 12u};

static void desc4(SuWorldDesc *d, uint64_t rows) {
    memset(d, 0, sizeof *d);
    d->schema_id = "ents";
    d->schema_version = 1u;
    d->layout_generation = 1u;
    d->row_count = rows;
    d->entity_ids = k_ids4;
    d->fields = k_fields;
    d->field_count = NFIELDS;
}

/* World with capacity cap and the first four rows alive, values as in the fixture, tick 1. */
static SuWorld *make_world(uint64_t cap) {
    SuWorldDesc d;
    SuWorld *w = NULL;
    desc4(&d, 4u);
    if (su_world_create_ex(&d, cap, &w) != SU_OK) {
        return NULL;
    }
    if (su_world_init_committed(w, ID_A, k_a4, 4u) != SU_OK ||
        su_world_init_committed(w, ID_B, k_b4, 4u) != SU_OK ||
        su_world_init_committed(w, ID_C, k_c4, 4u) != SU_OK) {
        su_world_destroy(w);
        return NULL;
    }
    if (su_world_begin_tick(w) != SU_OK || su_world_publish(w) != SU_OK) {
        su_world_destroy(w);
        return NULL;
    }
    return w;
}

static uint32_t committed_value(const SuWorld *w, uint32_t id, uint64_t row) {
    const uint32_t *data = NULL;
    uint64_t rows = 0;
    if (su_world_read_committed(w, id, &data, &rows) != SU_OK || row >= rows) {
        return 0xFFFFFFFFu;
    }
    return data[row];
}

/* Working column values of a readable field, via a tick-open invocation view. */
static const uint32_t *working_column(SuWorld *w, uint32_t id, SuInvocation *inv) {
    uint32_t i;
    if (su_world_build_invocation(w, inv) != SU_OK) {
        return NULL;
    }
    for (i = 0; i < inv->binding_count; i++) {
        if (inv->bindings[i].field_id == id) {
            return (const uint32_t *)inv->bindings[i].data;
        }
    }
    return NULL;
}

static uint8_t *dump(const SuWorld *w, size_t *len) {
    size_t n = 0;
    uint8_t *buf;
    size_t written = 0;
    if (su_world_canonical_size(w, &n) != SU_OK) {
        return NULL;
    }
    buf = malloc(n);
    if (buf == NULL || su_world_canonical_write(w, buf, n, &written) != SU_OK) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static int same_dump(const SuWorld *w, const uint8_t *want, size_t want_len) {
    size_t n = 0;
    uint8_t *b = dump(w, &n);
    int eq = b != NULL && n == want_len && memcmp(b, want, n) == 0;
    free(b);
    return eq;
}

/* Test-only: the committed or working table with the fault seams of su_ents. Not for production. */
static SuEnts *committed_table_mut(SuWorld *w) {
    return (SuEnts *)(void *)su_world_entities_committed(w);
}

static SuEnts *working_table_mut(SuWorld *w) {
    return (SuEnts *)(void *)su_world_entities_working(w);
}

/* ---- spawn, despawn and handles -------------------------------------- */

static void test_spawn_despawn(void) {
    SuWorld *w = make_world(8u);
    SuWorldSpawnValue sv[2];
    SuEntity h = SU_ENTITY_INVALID;
    SuEntity h2 = SU_ENTITY_INVALID;
    uint32_t ext = 0;
    uint32_t gen = 0;
    int alive = 0;

    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    CHECK(su_world_row_count(w) == 8u);
    CHECK(su_ents_alive_count(su_world_entities_committed(w)) == 4u);

    /* Spawn takes the lowest free slot, writes the given values, zeroes the others. */
    sv[0].field_id = ID_A;
    sv[0].value = 100u;
    CHECK_ST(su_world_spawn(w, sv, 1u, 0u, &h), SU_OK);
    CHECK(h == SU_ENTITY_MAKE(4u, 1u));
    CHECK(committed_value(w, ID_A, 4u) == 100u);
    CHECK(committed_value(w, ID_B, 4u) == 0u);
    CHECK(committed_value(w, ID_C, 4u) == 0u);
    CHECK_ST(su_world_spawn(w, NULL, 0u, 77u, &h2), SU_OK);
    CHECK(h2 == SU_ENTITY_MAKE(5u, 1u));
    CHECK_ST(su_ents_get_external(su_world_entities_committed(w), h2, &ext), SU_OK);
    CHECK(ext == 77u);

    /* Despawn zeroes the columns of the slot and retires the handle. */
    sv[0].field_id = ID_C;
    sv[0].value = 42u;
    CHECK_ST(su_world_spawn(w, sv, 1u, 0u, &h), SU_OK); /* slot 6 */
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(6u, 1u)), SU_OK);
    CHECK(committed_value(w, ID_C, 6u) == 0u);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(6u, 1u)), SU_E_STALE_HANDLE);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(6u, 2u)), SU_E_STALE_HANDLE);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(100u, 1u)), SU_E_STALE_HANDLE);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_INVALID), SU_E_STALE_HANDLE);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(4u, 0u)), SU_E_STALE_HANDLE);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(4u, 1u)), SU_OK);
    CHECK(committed_value(w, ID_A, 4u) == 0u);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(4u, 1u)), SU_E_STALE_HANDLE);

    /* Lowest free slot is reused with the next generation. */
    CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_OK);
    CHECK(h == SU_ENTITY_MAKE(4u, 2u));
    CHECK_ST(su_ents_slot_generation(su_world_entities_committed(w), 4u, &gen), SU_OK);
    CHECK(gen == 2u);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(4u, 1u)), SU_E_STALE_HANDLE);

    /* Alive and generation views. */
    {
        uint8_t alive_bits[8];
        uint32_t gens[8];
        uint32_t k;
        CHECK_ST(su_world_alive_view(w, 0u, 8u, alive_bits), SU_OK);
        for (k = 0; k < 8u; k++) {
            uint8_t want = (k == 0u || k == 1u || k == 2u || k == 3u || k == 4u || k == 5u) ? 1u : 0u;
            CHECK(alive_bits[k] == want);
        }
        CHECK_ST(su_world_generation_view(w, 0u, 8u, gens), SU_OK);
        CHECK(gens[4] == 2u && gens[0] == 1u && gens[6] == 2u && gens[7] == 1u);
        CHECK_ST(su_world_alive_view(w, 6u, 3u, alive_bits), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_world_alive_view(w, 0u, 1u, NULL), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_world_generation_view(w, 0u, 1u, NULL), SU_E_INVALID_ARGUMENT);
    }

    /* Handles of a live entity resolve through the table; stale ones do not. */
    CHECK_ST(su_ents_handle_for_slot(su_world_entities_committed(w), 5u, &h), SU_OK);
    CHECK(h == SU_ENTITY_MAKE(5u, 1u));
    CHECK_ST(su_ents_validate(su_world_entities_committed(w), SU_ENTITY_MAKE(5u, 1u)), SU_OK);
    CHECK_ST(su_ents_validate(su_world_entities_committed(w), SU_ENTITY_MAKE(5u, 2u)), SU_E_STALE_HANDLE);
    CHECK(su_ents_slot_alive(su_world_entities_committed(w), 4u) == 1);
    alive = su_ents_slot_alive(su_world_entities_committed(w), 7u);
    CHECK(alive == 0);

    su_world_destroy(w);
}

static void test_mutation_phase_and_errors(void) {
    SuWorld *w = make_world(8u);
    SuWorldSpawnValue sv[4];
    SuEntity h = SU_ENTITY_INVALID;
    uint32_t i;
    size_t n = 0;
    uint8_t *before;

    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    /* Spawn and despawn are control-plane operations between ticks only. */
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_E_INVALID_ARGUMENT);
    CHECK(h == SU_ENTITY_INVALID);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(0u, 1u)), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_abort(w), SU_OK);

    /* Argument errors leave the world unchanged. */
    before = dump(w, &n);
    CHECK(before != NULL);
    sv[0].field_id = 99u;
    sv[0].value = 1u;
    CHECK_ST(su_world_spawn(w, sv, 1u, 0u, &h), SU_E_SCHEMA_MISMATCH);
    sv[0].field_id = ID_A;
    sv[1].field_id = ID_A;
    sv[1].value = 2u;
    CHECK_ST(su_world_spawn(w, sv, 2u, 0u, &h), SU_E_INVALID_ARGUMENT);
    for (i = 0; i < 4u; i++) {
        sv[i].field_id = ID_A;
        sv[i].value = i;
    }
    CHECK_ST(su_world_spawn(w, sv, 4u, 0u, &h), SU_E_INVALID_ARGUMENT); /* more values than fields */
    CHECK_ST(su_world_spawn(w, sv, 1u, 0u, NULL), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_spawn(w, NULL, 1u, 0u, &h), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_spawn(NULL, NULL, 0u, 0u, &h), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_despawn(NULL, SU_ENTITY_MAKE(0u, 1u)), SU_E_INVALID_ARGUMENT);
    if (before != NULL) {
        CHECK(same_dump(w, before, n));
        free(before);
    }
    su_world_destroy(w);
}

/* Exhausted capacity refuses spawn with nothing changed; the dump is identical before and after. */
static void test_capacity_exhaustion(void) {
    SuWorld *w = make_world(6u);
    SuEntity h = SU_ENTITY_INVALID;
    size_t n = 0;
    uint8_t *before;
    uint32_t k;

    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    for (k = 0; k < 2u; k++) {
        CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_OK);
    }
    CHECK(h == SU_ENTITY_MAKE(5u, 1u));
    before = dump(w, &n);
    CHECK(before != NULL);
    CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_E_RESOURCE_LIMIT);
    CHECK(h == SU_ENTITY_INVALID);
    if (before != NULL) {
        CHECK(same_dump(w, before, n));
        free(before);
    }
    CHECK(su_ents_alive_count(su_world_entities_committed(w)) == 6u);
    su_world_destroy(w);

    /* A world whose capacity equals its rows has no free slot. */
    w = make_world(4u);
    CHECK(w != NULL);
    if (w != NULL) {
        CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_E_RESOURCE_LIMIT);
        su_world_destroy(w);
    }
}

/* Generation exhaustion: the slot is retired and never reissued. */
static void test_retirement(void) {
    SuWorld *w = make_world(6u);
    SuEntity h = SU_ENTITY_INVALID;
    SuEntity hr = SU_ENTITY_INVALID;
    uint32_t k;

    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    /* Retire slot 4: its handle is about to hit the last generation. */
    CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_OK); /* slot 4 */
    CHECK(SU_ENTITY_SLOT(h) == 4u);
    CHECK_ST(su_ents_fault_set_generation(committed_table_mut(w), 4u, 0xFFFFFFFEu), SU_OK);
    CHECK_ST(su_ents_handle_for_slot(su_world_entities_committed(w), 4u, &hr), SU_OK);
    CHECK(SU_ENTITY_GEN(hr) == 0xFFFFFFFEu);
    CHECK_ST(su_world_despawn(w, hr), SU_OK);
    CHECK(su_ents_slot_retired(su_world_entities_committed(w), 4u) == 1);
    CHECK_ST(su_world_despawn(w, hr), SU_E_STALE_HANDLE);
    CHECK_ST(su_world_despawn(w, SU_ENTITY_MAKE(4u, 1u)), SU_E_STALE_HANDLE);

    /* Fill the rest: the retired slot is never handed out again. */
    for (k = 0; k < 5u; k++) {
        SuEntity got = SU_ENTITY_INVALID;
        if (su_world_spawn(w, NULL, 0u, 0u, &got) == SU_OK) {
            CHECK(SU_ENTITY_SLOT(got) != 4u);
        }
    }
    CHECK(su_ents_retired_count(su_world_entities_committed(w)) == 1u);
    CHECK(su_ents_alive_count(su_world_entities_committed(w)) == 6u - 1u);
    CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_E_RESOURCE_LIMIT);
    su_world_destroy(w);
}

/* ---- dead-row zeroing --------------------------------------------------- */

/* Native-like step: every row of every writable column, dead rows included, receives a nonzero value
 * through staging, then commit_staging copies it into the working root. */
static void stage_nonzero_everywhere(SuWorld *w, uint32_t salt) {
    SuOutputs out;
    uint32_t ids[2] = {ID_B, ID_C};
    uint64_t r;
    uint32_t k;
    CHECK_ST(su_world_build_outputs(w, &out), SU_OK);
    for (k = 0; k < out.binding_count; k++) {
        uint32_t *data = (uint32_t *)out.bindings[k].data;
        for (r = 0; r < out.bindings[k].row_count; r++) {
            data[r] = 0x5A5A0000u + (uint32_t)r + salt;
        }
    }
    CHECK_ST(su_world_commit_staging(w, ids, 2u), SU_OK);
}

static void test_zero_dead(void) {
    SuWorld *w = make_world(8u);
    SuInvocation inv;
    const uint32_t *wa;
    const uint32_t *wc;
    uint64_t r;

    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    /* Zeroing outside a tick is refused. */
    CHECK_ST(su_world_zero_dead(w), SU_E_INVALID_ARGUMENT);

    CHECK_ST(su_world_begin_tick(w), SU_OK);
    stage_nonzero_everywhere(w, 1u);
    /* Before zeroing: dead rows (4..7) hold the native-written values. */
    wc = working_column(w, ID_C, &inv);
    CHECK(wc != NULL);
    if (wc != NULL) {
        CHECK(wc[5] != 0u && wc[6] != 0u);
    }
    CHECK_ST(su_world_zero_dead(w), SU_OK);
    wa = working_column(w, ID_A, &inv);
    wc = working_column(w, ID_C, &inv);
    CHECK(wa != NULL && wc != NULL);
    if (wa != NULL && wc != NULL) {
        for (r = 4; r < 8u; r++) {
            CHECK(wa[r] == 0u && wc[r] == 0u);
        }
        /* Alive rows keep the staged values. */
        for (r = 0; r < 4u; r++) {
            CHECK(wc[r] == 0x5A5A0000u + (uint32_t)r + 1u);
        }
    }
    CHECK_ST(su_world_abort(w), SU_OK);
    su_world_destroy(w);

    /* Publish alone also leaves dead rows canonically zero in the committed root. */
    w = make_world(8u);
    CHECK(w != NULL);
    if (w != NULL) {
        CHECK_ST(su_world_begin_tick(w), SU_OK);
        stage_nonzero_everywhere(w, 2u);
        CHECK_ST(su_world_publish(w), SU_OK);
        for (r = 4; r < 8u; r++) {
            CHECK(committed_value(w, ID_B, r) == 0u && committed_value(w, ID_C, r) == 0u);
        }
        CHECK(committed_value(w, ID_C, 3u) == 0x5A5A0003u + 2u);
        su_world_destroy(w);
    }

    /* Despawned rows are zeroed in the committed root too, and the next native write cannot leak. */
    w = make_world(8u);
    CHECK(w != NULL);
    if (w != NULL) {
        SuWorldSpawnValue sv;
        SuEntity h = SU_ENTITY_INVALID;
        sv.field_id = ID_B;
        sv.value = 999u;
        CHECK_ST(su_world_spawn(w, &sv, 1u, 0u, &h), SU_OK); /* slot 4 */
        CHECK(committed_value(w, ID_B, 4u) == 999u);
        CHECK_ST(su_world_despawn(w, h), SU_OK);
        CHECK(committed_value(w, ID_B, 4u) == 0u);
        su_world_destroy(w);
    }
}

/* ---- abort and publish of the entity table (test seam: mutate the working table in a tick) ---- */

static void test_table_tick_discipline(void) {
    SuWorld *w = make_world(8u);
    size_t n = 0;
    uint8_t *before;
    SuEntity h = SU_ENTITY_INVALID;

    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    before = dump(w, &n);
    CHECK(before != NULL);

    /* Abort discards the working table: the committed table keeps its entities. */
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK_ST(su_ents_despawn(working_table_mut(w), SU_ENTITY_MAKE(0u, 1u)), SU_OK);
    CHECK(su_ents_slot_alive(su_world_entities_committed(w), 0u) == 1);
    CHECK_ST(su_world_abort(w), SU_OK);
    CHECK(su_ents_slot_alive(su_world_entities_committed(w), 0u) == 1);
    if (before != NULL) {
        CHECK(same_dump(w, before, n));
    }
    CHECK(su_world_tick(w) == 1u);

    /* A tick that changes the working table publishes it. */
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK_ST(su_ents_despawn(working_table_mut(w), SU_ENTITY_MAKE(0u, 1u)), SU_OK);
    CHECK_ST(su_world_publish(w), SU_OK);
    CHECK(su_ents_slot_alive(su_world_entities_committed(w), 0u) == 0);
    CHECK(su_ents_alive_count(su_world_entities_committed(w)) == 3u);
    CHECK(su_world_tick(w) == 2u);
    /* The next tick starts from the published table again. */
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK(su_ents_slot_alive(su_world_entities_working(w), 0u) == 0);
    CHECK_ST(su_world_abort(w), SU_OK);
    CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_OK);
    CHECK(h == SU_ENTITY_MAKE(0u, 2u));

    free(before);
    su_world_destroy(w);
}

/* ---- canonical v2 dumps ------------------------------------------------- */

static SuWorld *history_world(void) {
    SuWorld *w = make_world(8u);
    SuWorldSpawnValue sv;
    SuEntity h = SU_ENTITY_INVALID;
    if (w == NULL) {
        return NULL;
    }
    sv.field_id = ID_A;
    sv.value = 100u;
    if (su_world_spawn(w, &sv, 1u, 0u, &h) != SU_OK || su_world_despawn(w, SU_ENTITY_MAKE(1u, 1u)) != SU_OK ||
        su_world_spawn(w, NULL, 0u, 9u, &h) != SU_OK) {
        su_world_destroy(w);
        return NULL;
    }
    CHECK(h == SU_ENTITY_MAKE(1u, 2u));
    return w;
}

static void test_canonical_v2(void) {
    SuWorld *a = history_world();
    SuWorld *b = history_world();
    SuWorld *dw = NULL;
    SuWorld *r = NULL;
    SuWorldDesc d;
    size_t na = 0;
    size_t nb = 0;
    uint8_t *ba = NULL;
    uint8_t *bb = NULL;
    int eq = 0;
    const size_t hw = 5u; /* history: slots 0..4 used, slot 4 spawned last */

    CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        su_world_destroy(a);
        su_world_destroy(b);
        return;
    }
    /* Determinism: identical histories give identical bytes. */
    ba = dump(a, &na);
    bb = dump(b, &nb);
    CHECK(ba != NULL && bb != NULL && na == nb && bb != NULL && memcmp(ba, bb, na) == 0);
    CHECK(na == 25u + (60u + 9u * hw) + NFIELDS * (8u + 4u * hw));
    if (ba != NULL) {
        CHECK(memcmp(ba, "SUWSTATE\0", 9u) == 0 && ba[9] == 2u);
    }

    /* Round trip: the restored world has the same state, tick and entity table. */
    desc4(&d, 4u);
    if (ba != NULL) {
        CHECK_ST(su_world_restore_canonical(&d, ba, na, &r), SU_OK);
    }
    if (r != NULL) {
        CHECK_ST(su_world_state_equal(a, r, &eq), SU_OK);
        CHECK(eq == 1);
        CHECK(su_world_tick(r) == su_world_tick(a));
        CHECK(su_ents_alive_count(su_world_entities_committed(r)) == 5u);
        su_world_destroy(r);
        r = NULL;
    }

    /* A world with a dead row below high_water: spawn at slot 4, then despawn it. */
    dw = make_world(8u);
    CHECK(dw != NULL);
    if (dw != NULL) {
        SuWorldSpawnValue sv;
        SuEntity h = SU_ENTITY_INVALID;
        sv.field_id = ID_C;
        sv.value = 3u;
        CHECK_ST(su_world_spawn(dw, &sv, 1u, 0u, &h), SU_OK);
        CHECK_ST(su_world_despawn(dw, h), SU_OK);
    }
    if (dw != NULL && ba != NULL) {
        size_t nd = 0;
        uint8_t *bd = dump(dw, &nd);
        CHECK(bd != NULL && nd == 25u + (60u + 9u * 5u) + NFIELDS * (8u + 4u * 5u));
        if (bd != NULL) {
            /* Corruption: every mutation of the valid dump is refused and yields no world. */
            uint8_t *m = malloc(nd);
            size_t field0 = 25u + 60u + 9u * 5u; /* first field record: id 5 (field c) */
            if (m != NULL) {
                memcpy(m, bd, nd);
                CHECK_ST(su_world_restore_canonical(&d, m, nd - 1u, &r), SU_E_PARSE);
                CHECK(r == NULL);
                memcpy(m, bd, nd);
                m[0] = 'X';
                CHECK_ST(su_world_restore_canonical(&d, m, nd, &r), SU_E_PARSE);
                CHECK(r == NULL);
                memcpy(m, bd, nd);
                m[9] = 7u;
                CHECK_ST(su_world_restore_canonical(&d, m, nd, &r), SU_E_PARSE);
                CHECK(r == NULL);
                /* Generation 0 on an alive slot (slot 0 record at 25 + 60). */
                memcpy(m, bd, nd);
                m[25 + 60] = 0u;
                m[25 + 60 + 1] = 0u;
                m[25 + 60 + 2] = 0u;
                m[25 + 60 + 3] = 0u;
                CHECK_ST(su_world_restore_canonical(&d, m, nd, &r), SU_E_PARSE);
                CHECK(r == NULL);
                /* Dead slot 4 holds a value in field c: row 4 of the first field record. */
                memcpy(m, bd, nd);
                m[field0 + 8u + 4u * 4u] = 1u;
                CHECK_ST(su_world_restore_canonical(&d, m, nd, &r), SU_E_TYPE);
                CHECK(r == NULL);
                /* Field identity changed in the first record. */
                memcpy(m, bd, nd);
                m[field0] ^= 0x01u;
                CHECK_ST(su_world_restore_canonical(&d, m, nd, &r), SU_E_SCHEMA_MISMATCH);
                CHECK(r == NULL);
                /* high_water above capacity. */
                memcpy(m, bd, nd);
                m[25 + 36] = 9u;
                CHECK_ST(su_world_restore_canonical(&d, m, nd, &r), SU_E_PARSE);
                CHECK(r == NULL);
                /* Trailing byte. */
                memcpy(m, bd, nd);
                free(m);
                m = malloc(nd + 1u);
                if (m != NULL) {
                    memcpy(m, bd, nd);
                    m[nd] = 0u;
                    CHECK_ST(su_world_restore_canonical(&d, m, nd + 1u, &r), SU_E_PARSE);
                    CHECK(r == NULL);
                    free(m);
                }
            }
            /* The clean dump restores, and the dead slot comes back dead with its generation. */
            CHECK_ST(su_world_restore_canonical(&d, bd, nd, &r), SU_OK);
            if (r != NULL) {
                CHECK(su_ents_slot_alive(su_world_entities_committed(r), 4u) == 0);
                CHECK_ST(su_world_state_equal(dw, r, &eq), SU_OK);
                CHECK(eq == 1);
                su_world_destroy(r);
                r = NULL;
            }
            free(bd);
        }
    }

    free(ba);
    free(bb);
    su_world_destroy(dw);
    su_world_destroy(a);
    su_world_destroy(b);
}

/* The M0 v1 layout restores through the same entry point, to a world with every slot alive. */
static void test_canonical_v1(void) {
    size_t vlen = 0;
    uint8_t *v1 = NULL;
    FILE *f = fopen("tests/fixtures/world_v1/state_v1.bin", "rb");
    SuWorldDesc d;
    SuWorld *w = NULL;
    SuWorld *ref = make_world(4u);
    int eq = 0;
    long n;
    if (f == NULL) {
        CHECK(0);
        su_world_destroy(ref);
        return;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v1 = malloc((size_t)n);
    if (v1 == NULL || fread(v1, 1u, (size_t)n, f) != (size_t)n) {
        CHECK(0);
        free(v1);
        fclose(f);
        su_world_destroy(ref);
        return;
    }
    fclose(f);
    vlen = (size_t)n;
    CHECK(vlen == 121u);

    /* The v1 dump of the golden state (tick 1, four rows, ids 101..104) equals the current state. */
    desc4(&d, 4u);
    CHECK_ST(su_world_restore_canonical(&d, v1, vlen, &w), SU_OK);
    if (w != NULL && ref != NULL) {
        CHECK_ST(su_world_state_equal(w, ref, &eq), SU_OK);
        CHECK(eq == 1);
        CHECK(su_world_tick(w) == 1u);
        CHECK(su_ents_alive_count(su_world_entities_committed(w)) == 4u);
        su_world_destroy(w);
        w = NULL;
    }
    /* Row count must match the descriptor; a truncated v1 dump is refused. */
    desc4(&d, 3u);
    CHECK_ST(su_world_restore_canonical(&d, v1, vlen, &w), SU_E_SCHEMA_MISMATCH);
    CHECK(w == NULL);
    desc4(&d, 4u);
    CHECK_ST(su_world_restore_canonical(&d, v1, vlen - 1u, &w), SU_E_PARSE);
    CHECK(w == NULL);

    free(v1);
    su_world_destroy(ref);
}

/* ---- churn against a reference model ----------------------------------- */

#define CHURN_CAP 64u

static uint32_t churn_rng(uint64_t *s) {
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(*s >> 33);
}

static void test_churn(uint64_t ops) {
    SuWorld *w = NULL;
    SuWorldDesc d;
    uint32_t ref_alive[CHURN_CAP];
    uint32_t ref_gen[CHURN_CAP];
    SuEntity live[CHURN_CAP];
    uint32_t nlive = 0;
    uint64_t seed = 0x2545F4914F6CDD1Dull;
    uint64_t i;
    uint32_t k;
    uint32_t failures_before = (uint32_t)g_failures;

    desc4(&d, 0u);
    d.entity_ids = NULL;
    d.fields = k_fields;
    d.field_count = NFIELDS;
    if (su_world_create_ex(&d, CHURN_CAP, &w) != SU_OK) {
        CHECK(0);
        return;
    }
    for (k = 0; k < CHURN_CAP; k++) {
        ref_alive[k] = 0u;
        ref_gen[k] = 1u;
    }
    for (i = 0; i < ops; i++) {
        uint32_t r = churn_rng(&seed);
        if (nlive == 0u || (nlive < CHURN_CAP && (r % 100u) < 55u)) {
            /* Reference: lowest free slot takes the stored generation. */
            uint32_t slot = CHURN_CAP;
            SuEntity got = SU_ENTITY_INVALID;
            for (k = 0; k < CHURN_CAP; k++) {
                if (!ref_alive[k] && ref_gen[k] != 0xFFFFFFFFu) {
                    slot = k;
                    break;
                }
            }
            if (slot == CHURN_CAP) {
                CHECK_ST(su_world_spawn(w, NULL, 0u, (uint32_t)(i + 1u), &got), SU_E_RESOURCE_LIMIT);
                continue;
            }
            CHECK_ST(su_world_spawn(w, NULL, 0u, (uint32_t)(i + 1u), &got), SU_OK);
            CHECK(got == SU_ENTITY_MAKE(slot, ref_gen[slot]));
            ref_alive[slot] = 1u;
            live[nlive++] = got;
        } else {
            uint32_t pick = r % nlive;
            SuEntity h = live[pick];
            uint32_t slot = SU_ENTITY_SLOT(h);
            CHECK_ST(su_world_despawn(w, h), SU_OK);
            ref_alive[slot] = 0u;
            ref_gen[slot] = ref_gen[slot] + 1u;
            live[pick] = live[--nlive];
            /* A stale copy of the handle is refused. */
            CHECK_ST(su_world_despawn(w, h), SU_E_STALE_HANDLE);
        }
        if ((i & 1023u) == 0u) {
            uint32_t alive_now = 0;
            for (k = 0; k < CHURN_CAP; k++) {
                alive_now += ref_alive[k];
            }
            CHECK(alive_now == nlive);
        }
    }
    /* Final state agrees with the reference slot by slot. */
    for (k = 0; k < CHURN_CAP; k++) {
        uint32_t gen = 0;
        CHECK(su_ents_slot_alive(su_world_entities_committed(w), k) == (int)ref_alive[k]);
        CHECK_ST(su_ents_slot_generation(su_world_entities_committed(w), k, &gen), SU_OK);
        CHECK(gen == ref_gen[k]);
    }
    CHECK(su_ents_alive_count(su_world_entities_committed(w)) == nlive);
    if (g_failures == (int)failures_before) {
        printf("churn: ops=%llu alive=%u high_water=%u\n", (unsigned long long)ops, nlive,
               su_ents_high_water(su_world_entities_committed(w)));
    }
    su_world_destroy(w);
}

int main(void) {
    /* Reads tests/fixtures relative to the working directory (ctest runs in the repository root). */
    test_spawn_despawn();
    test_mutation_phase_and_errors();
    test_capacity_exhaustion();
    test_retirement();
    test_zero_dead();
    test_table_tick_discipline();
    test_canonical_v2();
    test_canonical_v1();
    test_churn(100000u);
    printf("test_world_ents: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
