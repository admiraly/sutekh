/* In-process host API (include/sutekh/host.h): creation, schema, inputs, stepping, reads, snapshots,
 * digests and errors, against an independent reference computed here (u32 wrapping arithmetic).
 *
 * Usage: test_host   (run from the repository root: it reads examples/ and tests/fixtures/entities/) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/host.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

typedef struct Bytes {
    uint8_t *data;
    size_t len;
} Bytes;

static Bytes slurp(const char *path) {
    Bytes b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b.data = malloc((size_t)n + 1u);
    if (b.data == NULL || fread(b.data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    b.len = (size_t)n;
    return b;
}

static Bytes g_cap;
static Bytes g_lir;
static Bytes g_fixture;
static SuHostCapsule g_movement;

static SuHostWorldDesc movement_desc(int with_fixture, uint64_t max_entities) {
    SuHostWorldDesc d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 1;
    d.capsules = &g_movement;
    if (with_fixture) {
        d.fixture_json = g_fixture.data;
        d.fixture_len = g_fixture.len;
    }
    d.max_entities = max_entities;
    d.tick_rate_hz = 120;
    d.seed = 7;
    return d;
}

static SuHostWorld *make_movement(void) {
    SuHostWorldDesc d = movement_desc(1, 0);
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t st = su_host_world_create(&d, &w, &e);
    if (st != SU_OK) {
        fprintf(stderr, "create failed: %s %s\n", su_host_status_name(st), e.message);
        exit(1);
    }
    return w;
}

static void digest_of(const SuHostWorld *w, uint8_t out[32]) {
    CHECK(su_host_digest(w, out) == SU_OK);
}

/* Reference movement: position += velocity (mod 2^32) per tick. */
static const uint32_t k_pos0[4] = {0u, 1u, 4294967295u, 7u};
static const uint32_t k_vel0[4] = {1u, 2u, 1u, 0u};

static void test_create_and_schema(void) {
    SuHostWorld *w = make_movement();
    SuHostFieldInfo fi;
    uint32_t id = 0;
    const char *sys = NULL;
    const char *sem = NULL;
    uint8_t ident[32];
    uint8_t zero[32];

    CHECK(su_host_api_version() == SU_HOST_API_VERSION);
    CHECK(strcmp(su_host_schema_id(w), "demo.actor") == 0);
    CHECK(su_host_tick(w) == 0u);
    CHECK(su_host_capacity(w) == 4u);
    CHECK(su_host_tick_rate(w) == 120u);
    CHECK(su_host_seed(w) == 7u);
    CHECK(su_host_field_count(w) == 2u);
    CHECK(su_host_field_info(w, 0, &fi) == SU_OK && fi.id == 1u && strcmp(fi.name, "position_x") == 0);
    CHECK(su_host_field_info(w, 1, &fi) == SU_OK && fi.id == 2u && strcmp(fi.name, "velocity_x") == 0);
    CHECK(su_host_field_info(w, 2, &fi) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_field_id(w, "velocity_x", &id) == SU_OK && id == 2u);
    CHECK(su_host_field_id(w, "nope", &id) == SU_E_SCHEMA_MISMATCH);
    CHECK(su_host_system_count(w) == 1u);
    CHECK(su_host_system_info(w, 0, &sys, &sem) == SU_OK && strcmp(sys, "demo.movement") == 0 && strlen(sem) == 64u);
    memset(zero, 0, sizeof zero);
    CHECK(su_host_world_identity(w, ident) == SU_OK && memcmp(ident, zero, 32) != 0);
    CHECK(su_host_set_tick_rate(w, 0) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_set_tick_rate(w, 60) == SU_OK && su_host_tick_rate(w) == 60u);
    su_host_world_destroy(w);
    su_host_world_destroy(NULL);
}

static void test_step_and_read(void) {
    SuHostWorld *w = make_movement();
    const uint32_t *col = NULL;
    uint64_t rows = 0;
    uint32_t copy[4];
    uint64_t done = 0;
    SuHostError e;
    int i;

    CHECK(su_host_step(w, 3, &done, &e) == SU_OK && done == 3u);
    CHECK(su_host_tick(w) == 3u);
    CHECK(su_host_column(w, 1, &col, &rows) == SU_OK && rows == 4u);
    CHECK(su_host_read(w, 1, 0, 4, copy) == SU_OK);
    for (i = 0; i < 4; i++) {
        uint32_t want = k_pos0[i] + 3u * k_vel0[i];
        CHECK(col[i] == want);
        CHECK(copy[i] == want);
    }
    CHECK(su_host_read(w, 1, 2, 3, copy) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_read(w, 99, 0, 1, copy) == SU_E_SCHEMA_MISMATCH);
    CHECK(su_host_step(w, 0, &done, &e) == SU_OK && done == 0u);
    su_host_world_destroy(w);
}

static void test_inputs(void) {
    SuHostWorld *w = make_movement();
    SuHostWorld *twin = make_movement();
    SuHostInput in[2];
    SuHostError e;
    uint64_t ent0 = 0;
    uint64_t ent3 = 0;
    uint64_t done = 0;
    uint32_t pos[4];
    uint8_t d1[32];
    uint8_t d2[32];
    uint8_t enc1[256];
    uint8_t enc2[256];
    size_t n1 = 0;
    size_t n2 = 0;

    CHECK(su_host_entity_at(w, 0, &ent0) == SU_OK && ent0 != SU_HOST_ENTITY_NONE);
    CHECK(su_host_entity_at(w, 3, &ent3) == SU_OK);
    CHECK(su_host_entity_at(w, 9, &ent3) == SU_E_STALE_HANDLE);
    CHECK(su_host_entity_at(w, 3, &ent3) == SU_OK);

    /* Tick 0 input: entity 0 velocity 10, entity 3 velocity 5. Applied before tick 0 -> 1. */
    in[0].entity = ent3;
    in[0].field_id = 2;
    in[0].value = 5;
    in[1].entity = ent0;
    in[1].field_id = 2;
    in[1].value = 10;
    CHECK(su_host_input_push(w, 0, in, 2, &e) == SU_OK);
    CHECK(su_host_input_push(twin, 0, &in[1], 1, &e) == SU_OK); /* same writes, pushed in two parts */
    CHECK(su_host_input_push(twin, 0, &in[0], 1, &e) == SU_OK);
    CHECK(su_host_input_encode(w, 0, enc1, sizeof enc1, &n1) == SU_OK);
    CHECK(su_host_input_encode(twin, 0, enc2, sizeof enc2, &n2) == SU_OK);
    CHECK(n1 == 24u + 2u * 16u && n1 == n2 && memcmp(enc1, enc2, n1) == 0); /* canonical: order-independent */

    /* Refusals leave the queue as it was. */
    CHECK(su_host_input_push(w, 0, &in[0], 1, &e) == SU_E_INVALID_ARGUMENT); /* duplicate cell */
    in[0].field_id = 77;
    CHECK(su_host_input_push(w, 0, &in[0], 1, &e) == SU_E_SCHEMA_MISMATCH && e.index == 0u);
    in[0].field_id = 2;
    CHECK(su_host_input_push(w, SU_HOST_INPUT_WINDOW, &in[0], 1, &e) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_input_encode(w, 0, enc2, sizeof enc2, &n2) == SU_OK && n2 == n1 && memcmp(enc1, enc2, n1) == 0);

    CHECK(su_host_step(w, 2, &done, &e) == SU_OK && done == 2u);
    CHECK(su_host_step(twin, 2, &done, &e) == SU_OK && done == 2u);
    CHECK(su_host_read(w, 1, 0, 4, pos) == SU_OK);
    CHECK(pos[0] == 0u + 2u * 10u);
    CHECK(pos[1] == 1u + 2u * 2u);
    CHECK(pos[2] == 4294967295u + 2u * 1u);
    CHECK(pos[3] == 7u + 2u * 5u);
    digest_of(w, d1);
    digest_of(twin, d2);
    CHECK(memcmp(d1, d2, 32) == 0);
    /* Records are consumed: nothing is left for tick 0. */
    CHECK(su_host_input_encode(w, 0, NULL, 0, &n2) == SU_OK && n2 == 24u);
    /* A tick in the past can no longer take inputs. */
    CHECK(su_host_input_push(w, 1, &in[0], 1, &e) == SU_E_INVALID_ARGUMENT);

    /* Encoded records round-trip; a non-canonical record is refused. */
    in[0].entity = ent0;
    in[0].field_id = 1;
    in[0].value = 1000;
    CHECK(su_host_input_push(w, 2, &in[0], 1, &e) == SU_OK);
    CHECK(su_host_input_encode(w, 2, enc1, sizeof enc1, &n1) == SU_OK);
    su_host_input_clear(w, 2);
    CHECK(su_host_input_encode(w, 2, NULL, 0, &n2) == SU_OK && n2 == 24u);
    CHECK(su_host_input_push_encoded(w, enc1, n1, &e) == SU_OK);
    CHECK(su_host_input_encode(w, 2, enc2, sizeof enc2, &n2) == SU_OK && n2 == n1 && memcmp(enc1, enc2, n1) == 0);
    CHECK(su_host_input_push_encoded(w, enc1, n1 - 1u, &e) == SU_E_PARSE);
    enc1[0] ^= 1u;
    CHECK(su_host_input_push_encoded(w, enc1, n1, &e) == SU_E_PARSE);

    /* Different inputs, different state. */
    CHECK(su_host_step(w, 1, &done, &e) == SU_OK);
    CHECK(su_host_step(twin, 1, &done, &e) == SU_OK);
    digest_of(w, d1);
    digest_of(twin, d2);
    CHECK(memcmp(d1, d2, 32) != 0);
    su_host_world_destroy(w);
    su_host_world_destroy(twin);
}

static void test_failed_tick_is_atomic(void) {
    SuHostWorld *w = make_movement();
    SuHostInput in[2];
    SuHostError e;
    uint64_t ent0 = 0;
    uint64_t ent1 = 0;
    uint64_t done = 99;
    uint8_t before[32];
    uint8_t after[32];

    CHECK(su_host_entity_at(w, 0, &ent0) == SU_OK);
    CHECK(su_host_entity_at(w, 1, &ent1) == SU_OK);
    CHECK(su_host_despawn(w, ent1, &e) == SU_OK);
    CHECK(su_host_despawn(w, ent1, &e) == SU_E_STALE_HANDLE);
    in[0].entity = ent0;
    in[0].field_id = 1;
    in[0].value = 123;
    in[1].entity = ent1; /* dead by the time the record applies */
    in[1].field_id = 1;
    in[1].value = 5;
    CHECK(su_host_input_push(w, 0, in, 2, &e) == SU_OK);
    digest_of(w, before);
    CHECK(su_host_step(w, 1, &done, &e) == SU_E_STALE_HANDLE && done == 0u && e.tick == 0u);
    digest_of(w, after);
    CHECK(memcmp(before, after, 32) == 0); /* entity 0's write was not applied either */
    CHECK(su_host_tick(w) == 0u);
    su_host_input_clear(w, 0);
    CHECK(su_host_step(w, 1, &done, &e) == SU_OK && done == 1u);
    su_host_world_destroy(w);
}

static void test_snapshots(void) {
    SuHostWorld *w = make_movement();
    SuHostWorld *other;
    SuHostWorldDesc d;
    SuHostError e;
    SuHostInput in;
    uint64_t ent2 = 0;
    uint64_t done = 0;
    uint8_t *snap;
    uint8_t *bad;
    size_t size = 0;
    size_t got = 0;
    uint8_t d_at3[32];
    uint8_t d_at6[32];
    uint8_t d_now[32];

    CHECK(su_host_step(w, 3, &done, &e) == SU_OK);
    CHECK(su_host_snapshot_size(w, &size) == SU_OK && size > 0u);
    snap = malloc(size);
    bad = malloc(size);
    CHECK(snap != NULL && bad != NULL);
    CHECK(su_host_snapshot_write(w, snap, size - 1u, &got) == SU_E_RESOURCE_LIMIT && got == size);
    CHECK(su_host_snapshot_write(w, snap, size, &got) == SU_OK && got == size);
    CHECK(memcmp(snap, "SUHSNAP", 8) == 0);
    digest_of(w, d_at3);

    /* Re-simulation from the snapshot reproduces the same states, inputs included. */
    CHECK(su_host_entity_at(w, 2, &ent2) == SU_OK);
    in.entity = ent2;
    in.field_id = 2;
    in.value = 9;
    CHECK(su_host_input_push(w, 4, &in, 1, &e) == SU_OK);
    CHECK(su_host_step(w, 3, &done, &e) == SU_OK && done == 3u);
    digest_of(w, d_at6);
    CHECK(su_host_snapshot_restore(w, snap, size, &e) == SU_OK);
    CHECK(su_host_tick(w) == 3u);
    digest_of(w, d_now);
    CHECK(memcmp(d_now, d_at3, 32) == 0);
    CHECK(su_host_input_push(w, 4, &in, 1, &e) == SU_OK);
    CHECK(su_host_step(w, 3, &done, &e) == SU_OK && done == 3u);
    digest_of(w, d_now);
    CHECK(memcmp(d_now, d_at6, 32) == 0);

    /* Malformed or foreign snapshots change nothing. */
    memcpy(bad, snap, size);
    bad[0] ^= 1u;
    CHECK(su_host_snapshot_restore(w, bad, size, &e) == SU_E_PARSE);
    CHECK(su_host_snapshot_restore(w, snap, size - 1u, &e) == SU_E_PARSE);
    memcpy(bad, snap, size);
    bad[20] ^= 1u; /* inside the world identity */
    CHECK(su_host_snapshot_restore(w, bad, size, &e) == SU_E_SCHEMA_MISMATCH);
    memcpy(bad, snap, size);
    bad[44] ^= 1u; /* header tick disagrees with the state */
    CHECK(su_host_snapshot_restore(w, bad, size, &e) != SU_OK);
    digest_of(w, d_now);
    CHECK(memcmp(d_now, d_at6, 32) == 0);
    CHECK(su_host_tick(w) == 6u);

    d = movement_desc(1, 16); /* same capsules, other capacity: other identity */
    CHECK(su_host_world_create(&d, &other, &e) == SU_OK);
    CHECK(su_host_snapshot_restore(other, snap, size, &e) == SU_E_SCHEMA_MISMATCH);
    su_host_world_destroy(other);

    free(snap);
    free(bad);
    su_host_world_destroy(w);
}

static void test_empty_world_and_entities(void) {
    SuHostWorldDesc d = movement_desc(0, 8);
    SuHostWorld *w = NULL;
    SuHostError e;
    SuHostValue v[2];
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t done = 0;
    uint8_t alive[8];
    uint32_t pos[8];
    uint8_t d1[32];
    uint8_t d2[32];

    CHECK(su_host_world_create(&d, &w, &e) == SU_OK);
    CHECK(su_host_capacity(w) == 8u);
    CHECK(su_host_alive(w, 0, 8, alive) == SU_OK && alive[0] == 0u);
    v[0].field_id = 1;
    v[0].value = 100;
    v[1].field_id = 2;
    v[1].value = 3;
    CHECK(su_host_spawn(w, v, 2, 42, &a, &e) == SU_OK);
    CHECK(su_host_spawn(w, v, 1, 43, &b, &e) == SU_OK);
    CHECK(su_host_alive(w, 0, 8, alive) == SU_OK && alive[0] == 1u && alive[1] == 1u && alive[2] == 0u);
    digest_of(w, d1);
    CHECK(su_host_step(w, 2, &done, &e) == SU_OK && done == 2u);
    CHECK(su_host_read(w, 1, 0, 8, pos) == SU_OK);
    CHECK(pos[0] == 106u && pos[1] == 100u && pos[2] == 0u);
    CHECK(su_host_despawn(w, a, &e) == SU_OK);
    CHECK(su_host_read(w, 1, 0, 1, pos) == SU_OK && pos[0] == 0u); /* dead rows are zero */
    digest_of(w, d2);
    CHECK(memcmp(d1, d2, 32) != 0);
    v[0].field_id = 99;
    CHECK(su_host_spawn(w, v, 1, 0, &a, &e) == SU_E_SCHEMA_MISMATCH);
    su_host_world_destroy(w);

    d = movement_desc(0, 0);
    CHECK(su_host_world_create(&d, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL);
}

static void test_entity_ops_capsule(void) {
    Bytes cap = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    Bytes lir = slurp("tests/fixtures/entities/despawn_move.lir.json");
    Bytes fx = slurp("tests/fixtures/entities/actors_3.json");
    SuHostCapsule c;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostWorld *w2 = NULL;
    SuHostError e;
    uint64_t done = 0;
    uint8_t d1[32];
    uint8_t d2[32];
    uint32_t st;

    c.capsule_json = cap.data;
    c.capsule_len = cap.len;
    c.lir_json = lir.data;
    c.lir_len = lir.len;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 1;
    d.capsules = &c;
    d.fixture_json = fx.data;
    d.fixture_len = fx.len;
    d.tick_rate_hz = 120;
    st = su_host_world_create(&d, &w, &e);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "entity capsule: %s %s\n", su_host_status_name(st), e.message);
    } else {
        CHECK(su_host_world_create(&d, &w2, &e) == SU_OK);
        CHECK(su_host_step(w, 4, &done, &e) == SU_OK && done == 4u);
        CHECK(su_host_step(w2, 4, &done, &e) == SU_OK && done == 4u);
        digest_of(w, d1);
        digest_of(w2, d2);
        CHECK(memcmp(d1, d2, 32) == 0);
    }
    su_host_world_destroy(w);
    su_host_world_destroy(w2);
    free(cap.data);
    free(lir.data);
    free(fx.data);
}

static void test_create_errors(void) {
    SuHostWorldDesc d = movement_desc(1, 0);
    SuHostCapsule two[2];
    SuHostCapsule broken = g_movement;
    SuHostWorld *w = (SuHostWorld *)&d;
    SuHostError e;
    uint8_t *lir = malloc(g_lir.len + 1u);

    d.struct_size = 4;
    CHECK(su_host_world_create(&d, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL);
    d = movement_desc(1, 0);
    d.tick_rate_hz = 0;
    CHECK(su_host_world_create(&d, &w, &e) == SU_E_INVALID_ARGUMENT);
    d = movement_desc(1, 0);
    two[0] = g_movement;
    two[1] = g_movement;
    d.capsules = two;
    d.capsule_count = 2;
    CHECK(su_host_world_create(&d, &w, &e) != SU_OK && w == NULL); /* duplicate system id */
    CHECK(e.index == 1u);
    memcpy(lir, g_lir.data, g_lir.len);
    lir[g_lir.len] = 0u;
    {
        /* "add_u32" -> "add_u33": an unknown opcode in capsule 0. */
        uint8_t *p = (uint8_t *)strstr((char *)lir, "add_u32");
        CHECK(p != NULL);
        if (p != NULL) {
            p[6] = '3';
        }
    }
    broken.lir_json = lir;
    d = movement_desc(1, 0);
    d.capsules = &broken;
    CHECK(su_host_world_create(&d, &w, &e) == SU_E_UNKNOWN_OPCODE && e.index == 0u && w == NULL);
    d = movement_desc(1, 0);
    d.fixture_len = 10; /* truncated fixture */
    CHECK(su_host_world_create(&d, &w, &e) == SU_E_PARSE && w == NULL);
    free(lir);
}

int main(void) {
    g_cap = slurp("examples/movement.capsule.json");
    g_lir = slurp("examples/movement.lir.json");
    g_fixture = slurp("examples/world.json");
    g_movement.capsule_json = g_cap.data;
    g_movement.capsule_len = g_cap.len;
    g_movement.lir_json = g_lir.data;
    g_movement.lir_len = g_lir.len;

    test_create_and_schema();
    test_step_and_read();
    test_inputs();
    test_failed_tick_is_atomic();
    test_snapshots();
    test_empty_world_and_entities();
    test_entity_ops_capsule();
    test_create_errors();

    free(g_cap.data);
    free(g_lir.data);
    free(g_fixture.data);
    printf("test_host: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
