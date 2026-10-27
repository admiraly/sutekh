/* Worlds with several entity kinds (include/sutekh/host.h, "tables"; issue 140): creation, per-table schemas and
 * capacities, per-table spawn/despawn and cross-table handle validation, per-table inputs, snapshots, replays,
 * memory accounting against the single-table baseline, and the refusals of the first slice.
 *
 * Usage: test_host_tables   (run from the repository root: it reads examples/) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/entities.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"
#include "sutekh/replay.h"

static long g_checks = 0;
static long g_fail = 0;

/* Allocation counting for the "no allocation in a tick" check (static Linux build: -Wl,--wrap=malloc,...). */
#ifdef SU_TEST_WRAP_ALLOC
static volatile int g_count_alloc;
static unsigned long g_allocs;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t m) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __real_calloc(n, m);
}
void *__wrap_realloc(void *p, size_t n) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __real_realloc(p, n);
}
#endif

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
    FILE *f = fopen(path, "rb");
    Bytes b;
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b.data = malloc((size_t)n + 1u);
    b.len = (size_t)n;
    if (b.data == NULL || fread(b.data, 1, b.len, f) != b.len) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    return b;
}

/* The second kind: a projectile pool, schema demo.pool, one read lane and one read-write lane. */
static const char k_pool_capsule[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"demo.pool_tick\",\"source\":\"pool.lir.json\","
    "\"schema\":{\"id\":\"demo.pool\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"u32_mod_v1\","
    "\"fields\":[{\"id\":1,\"name\":\"remaining\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":2,\"name\":\"damage\",\"type\":\"u32\",\"read\":true,\"write\":false}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":64,\"max_rows\":1000000,\"allocation\":\"forbidden\"},\"tests\":[\"pool.test.json\"]}";
static const char k_pool_lir[] =
    "{\"ir_version\":\"0.1\",\"system_id\":\"demo.pool_tick\",\"numeric_profile\":\"u32_mod_v1\","
    "\"query\":\"demo.pool\",\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"d\",\"field\":\"damage\"},"
    "{\"op\":\"store_u32\",\"field\":\"remaining\",\"src\":\"d\"}]}";

/* The single-table baseline for the memory test: one schema with the union of both kinds' lanes. */
static const char k_union_capsule[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"demo.union_tick\",\"source\":\"union.lir.json\","
    "\"schema\":{\"id\":\"demo.union\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"u32_mod_v1\","
    "\"fields\":[{\"id\":1,\"name\":\"position_x\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":2,\"name\":\"velocity_x\",\"type\":\"u32\",\"read\":true,\"write\":false}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":64,\"max_rows\":1000000,\"allocation\":\"forbidden\"},\"tests\":[\"pool.test.json\"]}";
static const char k_union_lir[] =
    "{\"ir_version\":\"0.1\",\"system_id\":\"demo.union_tick\",\"numeric_profile\":\"u32_mod_v1\","
    "\"query\":\"demo.union\",\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"v\",\"field\":\"velocity_x\"},"
    "{\"op\":\"store_u32\",\"field\":\"position_x\",\"src\":\"v\"}]}";

static Bytes g_cap;
static Bytes g_lir;
static SuHostCapsule g_ship;
static SuHostCapsule g_pool;
static SuHostCapsule g_union_cap;

/* The declared schema of a table, in the shape the world descriptor wants. */
typedef struct TableSchema {
    SuHostSchema s;
    SuHostSchemaField f[4];
} TableSchema;

static void ship_schema(TableSchema *t) {
    memset(t, 0, sizeof *t);
    t->s.struct_size = (uint32_t)sizeof t->s;
    t->s.field_count = 2;
    t->s.fields = t->f;
    t->s.schema_id = "demo.actor";
    t->s.schema_version = 1;
    t->s.layout_generation = 1;
    t->f[0].id = 1;
    t->f[0].type = SU_TYPE_U32;
    t->f[0].name = "position_x";
    t->f[1].id = 2;
    t->f[1].type = SU_TYPE_U32;
    t->f[1].name = "velocity_x";
}

static void pool_schema(TableSchema *t) {
    memset(t, 0, sizeof *t);
    t->s.struct_size = (uint32_t)sizeof t->s;
    t->s.field_count = 2;
    t->s.fields = t->f;
    t->s.schema_id = "demo.pool";
    t->s.schema_version = 1;
    t->s.layout_generation = 1;
    t->f[0].id = 1;
    t->f[0].type = SU_TYPE_U32;
    t->f[0].name = "remaining";
    t->f[1].id = 2;
    t->f[1].type = SU_TYPE_U32;
    t->f[1].name = "damage";
}

static void union_schema(TableSchema *t) {
    memset(t, 0, sizeof *t);
    t->s.struct_size = (uint32_t)sizeof t->s;
    t->s.field_count = 4;
    t->s.fields = t->f;
    t->s.schema_id = "demo.union";
    t->s.schema_version = 1;
    t->s.layout_generation = 1;
    t->f[0].id = 1;
    t->f[0].type = SU_TYPE_U32;
    t->f[0].name = "position_x";
    t->f[1].id = 2;
    t->f[1].type = SU_TYPE_U32;
    t->f[1].name = "velocity_x";
    t->f[2].id = 3;
    t->f[2].type = SU_TYPE_U32;
    t->f[2].name = "remaining";
    t->f[3].id = 4;
    t->f[3].type = SU_TYPE_U32;
    t->f[3].name = "damage";
}

/* Two tables: ships (demo.actor, the movement capsule) and the pool (demo.pool). */
static SuHostWorld *make_two(TableSchema *ship, TableSchema *pool, uint64_t ship_rows, uint64_t pool_rows,
                             const uint8_t *ship_fixture, size_t ship_fixture_len, const uint8_t *pool_fixture,
                             size_t pool_fixture_len, SuHostError *err) {
    SuHostTableDesc td[2];
    uint32_t tables[2] = {0u, 1u};
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;

    memset(td, 0, sizeof td);
    td[0].struct_size = (uint32_t)sizeof td[0];
    td[0].schema = &ship->s;
    td[0].max_entities = ship_rows;
    td[0].fixture_json = ship_fixture;
    td[0].fixture_len = ship_fixture_len;
    td[1].struct_size = (uint32_t)sizeof td[1];
    td[1].schema = &pool->s;
    td[1].max_entities = pool_rows;
    td[1].fixture_json = pool_fixture;
    td[1].fixture_len = pool_fixture_len;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 2;
    d.capsules = (SuHostCapsule[]){g_ship, g_pool};
    d.tick_rate_hz = 120;
    d.seed = 11;
    d.table_count = 2;
    d.tables = td;
    d.capsule_table = tables;
    if (su_host_world_create(&d, &w, err) != SU_OK) {
        fprintf(stderr, "two-table create failed: %s\n", err->message);
        exit(1);
    }
    return w;
}

static void test_create_and_tables(void) {
    TableSchema ship, pool;
    SuHostWorld *w;
    SuHostError e;
    const char *id = NULL;
    uint64_t rows = 0;
    uint64_t base = 0;
    uint32_t n = 0;
    uint32_t field = 0;

    ship_schema(&ship);
    pool_schema(&pool);
    w = make_two(&ship, &pool, 4u, 6u, NULL, 0u, NULL, 0u, &e);
    CHECK(su_host_table_count(w) == 2u);
    CHECK(su_host_table_capacity(w, 0, &rows) == SU_OK && rows == 4u);
    CHECK(su_host_table_capacity(w, 1, &rows) == SU_OK && rows == 6u);
    CHECK(su_host_table_capacity(w, 2, &rows) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_table_schema_id(w, 0, &id) == SU_OK && strcmp(id, "demo.actor") == 0);
    CHECK(su_host_table_schema_id(w, 1, &id) == SU_OK && strcmp(id, "demo.pool") == 0);
    CHECK(su_host_table_base(w, 0, &base) == SU_OK && base == 0u);
    CHECK(su_host_table_base(w, 1, &base) == SU_OK && base == 4u);
    CHECK(su_host_capacity(w) == 4u); /* table 0, as before */
    CHECK(su_host_schema_id(w) != NULL && strcmp(su_host_schema_id(w), "demo.actor") == 0);
    CHECK(su_host_field_count_in(w, 0, &n) == SU_OK && n == 2u);
    CHECK(su_host_field_count_in(w, 1, &n) == SU_OK && n == 2u);
    CHECK(su_host_field_id_in(w, 1, "damage", &field) == SU_OK && field == 2u);
    CHECK(su_host_field_id_in(w, 1, "velocity_x", &field) == SU_E_SCHEMA_MISMATCH);
    CHECK(su_host_field_id_in(w, 0, "remaining", &field) == SU_E_SCHEMA_MISMATCH);
    CHECK(su_host_system_count(w) == 2u);
    su_host_world_destroy(w);
}

static void test_spawn_despawn_both_tables(void) {
    TableSchema ship, pool;
    SuHostWorld *w;
    SuHostError e;
    uint64_t ships[3] = {0, 0, 0};
    uint64_t proj[3] = {0, 0, 0};
    uint64_t t = 0;
    uint32_t table = 0;
    uint32_t val = 0;
    uint32_t i;
    SuHostValue sv[2];
    uint64_t done = 0;

    ship_schema(&ship);
    pool_schema(&pool);
    w = make_two(&ship, &pool, 4u, 4u, NULL, 0u, NULL, 0u, &e);
    /* Two ships and two projectiles, each with its own field IDs. */
    for (i = 0; i < 2u; i++) {
        sv[0].field_id = 1u;
        sv[0].value = 10u + i;
        sv[1].field_id = 2u;
        sv[1].value = 1u;
        CHECK(su_host_spawn_in(w, 0u, sv, 2u, 100u + i, &ships[i], &e) == SU_OK);
        sv[0].field_id = 2u; /* damage */
        sv[0].value = 7u + i;
        CHECK(su_host_spawn_in(w, 1u, sv, 1u, 200u + i, &proj[i], &e) == SU_OK);
    }
    /* Handles carry the table through their slot range. */
    CHECK(su_host_table_of_handle(w, ships[0], &table) == SU_OK && table == 0u);
    CHECK(su_host_table_of_handle(w, proj[0], &table) == SU_OK && table == 1u);
    CHECK(SU_ENTITY_SLOT(proj[0]) == 4u && SU_ENTITY_SLOT(ships[0]) == 0u);
    CHECK(su_host_entity_at_in(w, 1u, 0u, &t) == SU_OK && t == proj[0]);
    CHECK(su_host_entity_at_in(w, 0u, 0u, &t) == SU_OK && t == ships[0]);
    CHECK(su_host_entity_at_in(w, 0u, 4u, &t) == SU_E_STALE_HANDLE);
    /* Table-qualified reads. */
    {
        uint32_t vals[2] = {0u, 0u};
        CHECK(su_host_read_in(w, 0u, 1u, 0u, 2u, vals) == SU_OK && vals[0] == 10u && vals[1] == 11u);
    }
    CHECK(su_host_read_in(w, 1u, 2u, 0u, 1u, &val) == SU_OK && val == 7u);
    /* A tick runs both tables' systems (position += velocity; remaining = damage). */
    CHECK(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u);
    CHECK(su_host_read_in(w, 0u, 1u, 0u, 1u, &val) == SU_OK && val == 11u);
    CHECK(su_host_read_in(w, 1u, 1u, 0u, 1u, &val) == SU_OK && val == 7u);

    /* Cross-table validation: a handle of the other table is stale for a table-qualified call. */
    CHECK(su_host_despawn_in(w, 0u, proj[0], &e) == SU_E_STALE_HANDLE);
    CHECK(su_host_despawn_in(w, 1u, ships[0], &e) == SU_E_STALE_HANDLE);
    /* su_host_despawn routes by the handle's table. */
    CHECK(su_host_despawn(w, proj[1], &e) == SU_OK);
    CHECK(su_host_despawn(w, ships[1], &e) == SU_OK);
    /* A despawned handle is stale, in its own table and in the other. */
    CHECK(su_host_despawn(w, proj[1], &e) == SU_E_STALE_HANDLE);
    CHECK(su_host_despawn_in(w, 1u, proj[1], &e) == SU_E_STALE_HANDLE);
    CHECK(su_host_table_of_handle(w, proj[1], &table) == SU_OK && table == 1u); /* the slot still names table 1 */
    /* The lowest free slot is reused, and the generation moves: the old handle stays stale. */
    sv[0].field_id = 2u;
    sv[0].value = 9u;
    CHECK(su_host_spawn_in(w, 1u, sv, 1u, 300u, &t, &e) == SU_OK);
    CHECK(t != proj[1]);
    CHECK(SU_ENTITY_SLOT(t) == SU_ENTITY_SLOT(proj[1]));
    CHECK(SU_ENTITY_GEN(t) == SU_ENTITY_GEN(proj[1]) + 1u);
    CHECK(su_host_despawn(w, proj[1], &e) == SU_E_STALE_HANDLE);
    CHECK(su_host_despawn(w, t, &e) == SU_OK);
    /* Spawning into a table that does not exist is an invalid argument, not a stale handle. */
    CHECK(su_host_spawn_in(w, 2u, sv, 1u, 0u, &t, &e) == SU_E_INVALID_ARGUMENT);
    su_host_world_destroy(w);
}

static void test_inputs_across_tables(void) {
    TableSchema ship, pool;
    SuHostWorld *w;
    SuHostError e;
    uint64_t ship_h = 0;
    uint64_t proj_h = 0;
    uint64_t done = 0;
    SuHostValue sv;
    SuHostInput in[2];
    uint8_t rec[128];
    size_t n = 0;
    uint32_t val = 0;

    ship_schema(&ship);
    pool_schema(&pool);
    w = make_two(&ship, &pool, 2u, 2u, NULL, 0u, NULL, 0u, &e);
    sv.field_id = 2u;
    sv.value = 0u;
    CHECK(su_host_spawn_in(w, 0u, &sv, 1u, 1u, &ship_h, &e) == SU_OK);
    CHECK(su_host_spawn_in(w, 1u, &sv, 1u, 2u, &proj_h, &e) == SU_OK);
    /* One record writes a cell in each table; the canonical order is by world slot. */
    in[0].entity = proj_h;
    in[0].field_id = 2u;
    in[0].value = 42u;
    in[1].entity = ship_h;
    in[1].field_id = 2u;
    in[1].value = 5u;
    /* A record is applied to the tick that is current now (tick 0 advances the world to tick 1). */
    CHECK(su_host_input_push(w, 0u, in, 2u, &e) == SU_OK);
    CHECK(su_host_input_encode(w, 0u, rec, sizeof rec, &n) == SU_OK && n > 0u);
    CHECK(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u);
    CHECK(su_host_read_in(w, 0u, 1u, 0u, 1u, &val) == SU_OK && val == 5u);
    CHECK(su_host_read_in(w, 1u, 1u, 0u, 1u, &val) == SU_OK && val == 42u);
    /* A write naming a field of the other table is refused. */
    in[0].entity = ship_h;
    in[0].field_id = 2u; /* velocity_x exists in table 0 */
    in[1].entity = ship_h;
    in[1].field_id = 2u;
    CHECK(su_host_input_push(w, 1u, in, 2u, &e) == SU_E_INVALID_ARGUMENT); /* the same cell twice */
    in[1].entity = ship_h;
    in[1].field_id = 4u; /* no field 4 in demo.actor */
    CHECK(su_host_input_push(w, 1u, in, 2u, &e) == SU_E_SCHEMA_MISMATCH);
    /* A handle whose slot is in no table is stale. */
    in[0].entity = SU_ENTITY_MAKE(99u, 1u);
    CHECK(su_host_input_push(w, 1u, in, 1u, &e) == SU_E_STALE_HANDLE);
    /* A queued write for a handle that dies before the tick fails the tick and changes nothing. */
    in[0].entity = proj_h;
    in[0].field_id = 1u;
    in[0].value = 1u;
    CHECK(su_host_input_push(w, 1u, in, 1u, &e) == SU_OK);
    CHECK(su_host_despawn(w, proj_h, &e) == SU_OK);
    CHECK(su_host_step(w, 1u, &done, &e) == SU_E_STALE_HANDLE && done == 0u);
    CHECK(su_host_tick(w) == 1u);
    su_host_world_destroy(w);
}

static void test_snapshot_round_trip(void) {
    TableSchema ship, pool;
    SuHostWorld *w;
    SuHostError e;
    uint64_t ship_h = 0;
    uint64_t proj_h = 0;
    uint64_t done = 0;
    SuHostValue sv;
    SuHostInput in;
    uint8_t d3[32], d6[32], now[32];
    uint8_t *snap;
    size_t size = 0;
    size_t got = 0;
    uint32_t val = 0;
    uint32_t version = 0;

    ship_schema(&ship);
    pool_schema(&pool);
    w = make_two(&ship, &pool, 2u, 3u, NULL, 0u, NULL, 0u, &e);
    sv.field_id = 2u;
    sv.value = 0u;
    CHECK(su_host_spawn_in(w, 0u, &sv, 1u, 1u, &ship_h, &e) == SU_OK);
    CHECK(su_host_spawn_in(w, 1u, &sv, 1u, 2u, &proj_h, &e) == SU_OK);
    CHECK(su_host_step(w, 3u, &done, &e) == SU_OK && done == 3u);
    CHECK(su_host_snapshot_size(w, &size) == SU_OK && size > 0u);
    snap = malloc(size);
    CHECK(snap != NULL);
    if (snap == NULL) {
        su_host_world_destroy(w);
        return;
    }
    CHECK(su_host_snapshot_write(w, snap, size, &got) == SU_OK && got == size);
    CHECK(memcmp(snap, "SUHSNAP", 8) == 0);
    version = (uint32_t)snap[8] | ((uint32_t)snap[9] << 8) | ((uint32_t)snap[10] << 16) | ((uint32_t)snap[11] << 24);
    CHECK(version == 3u); /* a multi-table world writes version 3 */
    CHECK(su_host_digest(w, d3) == SU_OK);
    /* Inputs in both tables, then three more ticks. */
    in.entity = ship_h;
    in.field_id = 2u;
    in.value = 3u;
    CHECK(su_host_input_push(w, 4u, &in, 1u, &e) == SU_OK);
    in.entity = proj_h;
    in.field_id = 2u;
    in.value = 6u;
    CHECK(su_host_input_push(w, 4u, &in, 1u, &e) == SU_OK);
    CHECK(su_host_step(w, 3u, &done, &e) == SU_OK && done == 3u);
    CHECK(su_host_digest(w, d6) == SU_OK);
    CHECK(su_host_snapshot_restore(w, snap, size, &e) == SU_OK);
    CHECK(su_host_tick(w) == 3u);
    CHECK(su_host_digest(w, now) == SU_OK && memcmp(now, d3, 32) == 0);
    /* The queued inputs for tick 4 are kept, as documented: replaying them reproduces the later state. */
    CHECK(su_host_input_push(w, 4u, &in, 1u, &e) == SU_OK); /* projectile velocity */
    in.entity = ship_h;
    in.field_id = 2u;
    in.value = 3u;
    CHECK(su_host_input_push(w, 4u, &in, 1u, &e) == SU_OK);
    CHECK(su_host_step(w, 3u, &done, &e) == SU_OK && done == 3u);
    CHECK(su_host_digest(w, now) == SU_OK && memcmp(now, d6, 32) == 0);
    /* In-place restore of the same bytes (no allocation after the first call) reaches the same state. */
    CHECK(su_host_snapshot_restore_inplace(w, snap, size, &e) == SU_OK);
    CHECK(su_host_digest(w, now) == SU_OK && memcmp(now, d3, 32) == 0);
    CHECK(su_host_read_in(w, 1u, 2u, 0u, 1u, &val) == SU_OK);
    free(snap);
    su_host_world_destroy(w);
}

static void test_replay_round_trip(void) {
    TableSchema ship, pool;
    SuHostWorld *w;
    SuHostWorld *replay_world;
    SuHostError e;
    SuReplayOptions opt;
    SuReplayWriter *rw = NULL;
    SuReplay *r = NULL;
    SuReplayInfo info;
    uint8_t *bytes = NULL;
    size_t len = 0;
    uint64_t ship_h = 0;
    uint64_t proj_h = 0;
    uint64_t done = 0;
    uint64_t i;
    SuHostValue sv;
    SuHostInput in;
    uint8_t before[32];
    uint8_t after[32];

    ship_schema(&ship);
    pool_schema(&pool);
    w = make_two(&ship, &pool, 2u, 4u, NULL, 0u, NULL, 0u, &e);
    sv.field_id = 2u;
    sv.value = 0u;
    CHECK(su_host_spawn_in(w, 0u, &sv, 1u, 1u, &ship_h, &e) == SU_OK);
    CHECK(su_host_spawn_in(w, 1u, &sv, 1u, 2u, &proj_h, &e) == SU_OK);
    memset(&opt, 0, sizeof opt);
    opt.struct_size = (uint32_t)sizeof opt;
    opt.keyframe_interval = 2u;
    opt.digest_interval = 1u;
    CHECK(su_replay_writer_begin(w, &opt, &rw, &e) == SU_OK);
    for (i = 0; i < 6u; i++) {
        in.entity = (i % 2u == 0u) ? ship_h : proj_h;
        in.field_id = 2u;
        in.value = (uint32_t)i + 1u;
        CHECK(su_host_input_push(w, su_host_tick(w), &in, 1u, &e) == SU_OK);
        CHECK(su_replay_writer_step(rw, w, 1u, &done, &e) == SU_OK && done == 1u);
    }
    CHECK(su_host_digest(w, before) == SU_OK);
    CHECK(su_replay_writer_finish(rw, w, &bytes, &len) == SU_OK);
    su_replay_writer_destroy(rw);
    CHECK(bytes != NULL && len > 0u);
    CHECK(su_replay_open(bytes, len, &r, &e) == SU_OK);
    CHECK(su_replay_info(r, &info) == SU_OK);
    CHECK(info.end_tick == 6u && info.start_tick == 0u);
    CHECK(memcmp(info.final_digest, before, 32) == 0);
    /* A fresh world of the same tables re-simulates to the recorded state. */
    replay_world = make_two(&ship, &pool, 2u, 4u, NULL, 0u, NULL, 0u, &e);
    sv.field_id = 2u;
    sv.value = 0u;
    CHECK(su_host_spawn_in(replay_world, 0u, &sv, 1u, 1u, &ship_h, &e) == SU_OK);
    CHECK(su_host_spawn_in(replay_world, 1u, &sv, 1u, 2u, &proj_h, &e) == SU_OK);
    /* The spawns happened before the recording started, so the recording's start state is tick 0 with them. */
    CHECK(su_replay_play(r, replay_world, 6u, &done, &e) == SU_OK && done == 6u);
    CHECK(su_host_digest(replay_world, after) == SU_OK && memcmp(after, before, 32) == 0);
    CHECK(su_replay_seek(r, replay_world, 3u, &e) == SU_OK);
    CHECK(su_host_tick(replay_world) == 3u);
    su_replay_close(r);
    su_replay_free(bytes);
    su_host_world_destroy(replay_world);
    su_host_world_destroy(w);
}

static void test_memory_accounting(void) {
    TableSchema ship, pool, uni;
    SuHostWorld *two;
    SuHostWorld *one;
    SuHostError e;
    SuHostMemory m;
    SuHostTableDesc td[2];
    uint32_t tables[2] = {0u, 1u};
    SuHostWorldDesc d;
    SuHostCapsule cap;
    const uint64_t ships = 10000u;
    const uint64_t pool_rows = 200000u;

    ship_schema(&ship);
    pool_schema(&pool);
    union_schema(&uni);
    /* Two tables: 10,000 ships (2 lanes) and 200,000 pool rows (2 lanes). */
    memset(td, 0, sizeof td);
    td[0].struct_size = (uint32_t)sizeof td[0];
    td[0].schema = &ship.s;
    td[0].max_entities = ships;
    td[1].struct_size = (uint32_t)sizeof td[1];
    td[1].schema = &pool.s;
    td[1].max_entities = pool_rows;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 2;
    d.capsules = (SuHostCapsule[]){g_ship, g_pool};
    d.tick_rate_hz = 120;
    d.table_count = 2;
    d.tables = td;
    d.capsule_table = tables;
    CHECK(su_host_world_create(&d, &two, &e) == SU_OK);
    /* The single-table baseline: one union schema of all four lanes at the largest capacity. */
    cap = g_union_cap;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.tick_rate_hz = 120;
    d.max_entities = ships + pool_rows; /* one union table holds both kinds */
    d.schema = &uni.s;
    CHECK(su_host_world_create(&d, &one, &e) == SU_OK);
    memset(&m, 0, sizeof m);
    m.struct_size = (uint32_t)sizeof m;
    CHECK(su_host_memory(two, &m) == SU_OK);
    CHECK(m.table_count == 2u);
    CHECK(m.table_total[0] > 0u && m.table_total[1] > 0u);
    CHECK(m.tables_total == m.table_total[0] + m.table_total[1]);
    CHECK(m.total == m.tables_total + m.input_storage + m.queued_inputs + m.restore_scratch + m.metadata);
    {
        uint64_t two_total = m.total;
        uint64_t ship_bytes = m.table_total[0];
        uint64_t pool_bytes = m.table_total[1];
        uint64_t input = m.input_storage;
        uint64_t queued = m.queued_inputs;
        uint64_t restore = m.restore_scratch;
        uint64_t meta = m.metadata;
        uint64_t one_total;
        CHECK(su_host_memory(one, &m) == SU_OK);
        CHECK(m.table_count == 1u);
        one_total = m.total;
        printf("memory: two tables %llu bytes (ship %llu + pool %llu + input %llu + queued %llu + restore %llu + "
               "metadata %llu); one union table %llu bytes; two tables save %llu bytes (%.2fx)\n",
               (unsigned long long)two_total, (unsigned long long)ship_bytes, (unsigned long long)pool_bytes,
               (unsigned long long)input, (unsigned long long)queued, (unsigned long long)restore,
               (unsigned long long)meta, (unsigned long long)one_total,
               (unsigned long long)(one_total - two_total), (double)one_total / (double)two_total);
        CHECK(two_total < one_total);
        /* The two-table world holds only the cells its kinds need: 10,000 x 2 + 200,000 x 2 against
         * 200,000 x 4 in the union table, so its columns are about half. Entity tables (one per table, 210,000
         * slots against 200,000) and the input buffers are part of the accounting too, so the whole world is
         * about 1.4x smaller at these capacities; a quarter is the bound this test asserts. */
        CHECK(two_total + two_total / 4u < one_total);
    }
    su_host_world_destroy(two);
    su_host_world_destroy(one);
}

static void test_refusals(void) {
    TableSchema ship, pool;
    SuHostWorld *w = NULL;
    SuHostError e;
    SuHostTableDesc td[2];
    SuHostWorldDesc d;
    uint32_t tables[2] = {0u, 1u};
    Bytes ent_cap;
    Bytes ent_lir;
    SuHostCapsule ent;

    ship_schema(&ship);
    pool_schema(&pool);
    memset(td, 0, sizeof td);
    td[0].struct_size = (uint32_t)sizeof td[0];
    td[0].schema = &ship.s;
    td[0].max_entities = 2u;
    td[1].struct_size = (uint32_t)sizeof td[1];
    td[1].schema = &pool.s;
    td[1].max_entities = 2u;
    /* A single-table descriptor may not carry tables. */
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &g_ship;
    d.tick_rate_hz = 120;
    d.max_entities = 2u;
    d.tables = td;
    CHECK(su_host_world_create(&d, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL);
    /* A multi-table world needs a declared schema per table. */
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 2;
    d.capsules = (SuHostCapsule[]){g_ship, g_pool};
    d.tick_rate_hz = 120;
    d.table_count = 2;
    d.tables = td;
    d.capsule_table = tables;
    td[1].schema = NULL;
    CHECK(su_host_world_create(&d, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL);
    td[1].schema = &pool.s;
    /* capsule_table must name an existing table. */
    {
        uint32_t bad[2] = {0u, 2u};
        d.capsule_table = bad;
        CHECK(su_host_world_create(&d, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL);
    }
    /* A capsule whose schema is not its table's schema is refused. */
    {
        uint32_t swapped[2] = {1u, 0u};
        d.capsule_table = swapped;
        CHECK(su_host_world_create(&d, &w, &e) == SU_E_SCHEMA_MISMATCH && w == NULL);
    }
    /* IR entity operations are supported in a multi-table world (slice 2): the entity tick is table-aware, so a
     * table-0 system that despawns rows runs while table 1 sits idle. */
    ent_cap = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    ent_lir = slurp("tests/fixtures/entities/despawn_move.lir.json");
    memset(&ent, 0, sizeof ent);
    ent.capsule_json = ent_cap.data;
    ent.capsule_len = ent_cap.len;
    ent.lir_json = ent_lir.data;
    ent.lir_len = ent_lir.len;
    {
        TableSchema ent_schema;
        SuHostValue sv = {2u, 1u};
        uint64_t h = 0;
        uint64_t done = 0;
        uint8_t dg[32];
        memset(&ent_schema, 0, sizeof ent_schema);
        ent_schema.s.struct_size = (uint32_t)sizeof ent_schema.s;
        ent_schema.s.field_count = 2;
        ent_schema.s.fields = ent_schema.f;
        ent_schema.s.schema_id = "demo.actor";
        ent_schema.s.schema_version = 1;
        ent_schema.s.layout_generation = 1;
        ent_schema.f[0].id = 1;
        ent_schema.f[0].type = SU_TYPE_U32;
        ent_schema.f[0].name = "position_x";
        ent_schema.f[1].id = 2;
        ent_schema.f[1].type = SU_TYPE_U32;
        ent_schema.f[1].name = "velocity_x";
        td[0].schema = &ent_schema.s;
        td[0].max_entities = 2u;
        td[1].schema = &pool.s;
        d.capsule_count = 2;
        d.capsules = (SuHostCapsule[]){ent, g_pool};
        d.capsule_table = tables;
        w = NULL;
        CHECK(su_host_world_create(&d, &w, &e) == SU_OK);
        if (w != NULL) {
            CHECK(su_host_spawn_in(w, 0u, &sv, 1u, 0u, &h, &e) == SU_OK);
            CHECK(su_host_spawn_in(w, 0u, &sv, 1u, 1u, &h, &e) == SU_OK);
            CHECK(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u);
            /* despawn_move despawns the rows its fixture marks dead; the world stepped, so table 0's entity
             * operations ran and table 1 (no system) stayed idle. */
            CHECK(su_host_digest(w, dg) == SU_OK);
            CHECK(su_host_tick(w) == 1u);
            su_host_world_destroy(w);
        }
    }
    free(ent_cap.data);
    free(ent_lir.data);
    /* A table index beyond SU_HOST_MAX_TABLES is a resource limit. */
    {
        SuHostTableDesc many[SU_HOST_MAX_TABLES + 1u];
        uint32_t assign[1] = {0u};
        memset(many, 0, sizeof many);
        d.tables = many;
        d.table_count = SU_HOST_MAX_TABLES + 1u;
        d.capsule_table = assign;
        d.capsule_count = 1;
        d.capsules = &g_ship;
        CHECK(su_host_world_create(&d, &w, &e) == SU_E_RESOURCE_LIMIT && w == NULL);
    }
}

/* The motivating case, functionally: a system over a small table does not pay for a large table with no systems.
 * The digest of the small table's world is the same with and without the second table (its content is not part of
 * the small table's state), and both step. Timing is the bench's job (bench/host/bench_host_tables.c). */
static void test_large_idle_table(void) {
    TableSchema ship, pool;
    SuHostWorld *two;
    SuHostWorld *one;
    SuHostError e;
    uint64_t done = 0;
    uint64_t rows = 0;
    uint32_t table = 0;
    uint8_t d2[32], d1[32];

    ship_schema(&ship);
    pool_schema(&pool);
    two = make_two(&ship, &pool, 8u, 1000u, NULL, 0u, NULL, 0u, &e);
    {
        SuHostWorldDesc d;
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.capsule_count = 1;
        d.capsules = &g_ship;
        d.tick_rate_hz = 120;
        d.seed = 11;
        d.max_entities = 8u;
        CHECK(su_host_world_create(&d, &one, &e) == SU_OK);
    }
    CHECK(su_host_step(two, 4u, &done, &e) == SU_OK && done == 4u);
    CHECK(su_host_step(one, 4u, &done, &e) == SU_OK && done == 4u);
    /* Different identities (the tables are part of it), but the same small-table columns. */
    CHECK(su_host_digest(two, d2) == SU_OK);
    CHECK(su_host_digest(one, d1) == SU_OK);
    CHECK(memcmp(d1, d2, 32) != 0);
    CHECK(su_host_tick(two) == su_host_tick(one));
    /* The small table's columns are the same in both worlds: the idle table costs nothing and changes nothing. */
    {
        uint32_t f;
        for (f = 1u; f <= 2u; f++) {
            const uint32_t *c2 = NULL;
            const uint32_t *c1 = NULL;
            uint64_t r2 = 0, r1 = 0;
            CHECK(su_host_column_in(two, 0u, f, &c2, &r2) == SU_OK);
            CHECK(su_host_column(one, f, &c1, &r1) == SU_OK);
            CHECK(r1 == r2 && r1 == 8u && memcmp(c1, c2, (size_t)r1 * sizeof *c1) == 0);
        }
    }
    /* The idle table keeps the world's tick (it is never begun or published), so its canonical state restores. */
    CHECK(su_host_table_capacity(two, 1u, &rows) == SU_OK && rows == 1000u);
    {
        uint8_t *snap;
        size_t size = 0;
        uint8_t d2b[32];
        uint64_t h = 0;
        SuHostValue sv = {2u, 3u};
        CHECK(su_host_spawn_in(two, 1u, &sv, 1u, 9u, &h, &e) == SU_OK);
        CHECK(su_host_step(two, 2u, &done, &e) == SU_OK && done == 2u);
        CHECK(su_host_digest(two, d2b) == SU_OK);
        CHECK(su_host_snapshot_size(two, &size) == SU_OK);
        snap = malloc(size);
        CHECK(snap != NULL && su_host_snapshot_write(two, snap, size, &size) == SU_OK);
        CHECK(su_host_step(two, 2u, &done, &e) == SU_OK);
        CHECK(su_host_snapshot_restore(two, snap, size, &e) == SU_OK);
        CHECK(su_host_digest(two, d2) == SU_OK && memcmp(d2, d2b, 32) == 0);
        CHECK(su_host_table_of_handle(two, h, &table) == SU_OK && table == 1u);
        free(snap);
    }
    su_host_world_destroy(two);
    su_host_world_destroy(one);
}

/* The tick allocates nothing: applying a record that touches both tables and running both tables' systems
 * allocates no memory (static Linux build only; the wrapper counts malloc, calloc and realloc). */
static void test_no_allocation_in_tick(void) {
#ifdef SU_TEST_WRAP_ALLOC
    TableSchema ship, pool;
    SuHostWorld *w;
    SuHostError e;
    SuHostValue sv;
    SuHostInput in[2];
    uint64_t ships[2] = {0, 0};
    uint64_t proj[2] = {0, 0};
    uint64_t done = 0;
    uint32_t i;
    ship_schema(&ship);
    pool_schema(&pool);
    w = make_two(&ship, &pool, 4u, 4u, NULL, 0u, NULL, 0u, &e);
    sv.field_id = 2u;
    sv.value = 1u;
    for (i = 0; i < 2u; i++) {
        CHECK(su_host_spawn_in(w, 0u, &sv, 1u, i, &ships[i], &e) == SU_OK);
        CHECK(su_host_spawn_in(w, 1u, &sv, 1u, i, &proj[i], &e) == SU_OK);
    }
    /* The first tick builds the entity runtime's cache (allocates), as documented. */
    CHECK(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u);
    in[0].entity = ships[0];
    in[0].field_id = 2u;
    in[0].value = 2u;
    in[1].entity = proj[0];
    in[1].field_id = 2u;
    in[1].value = 3u;
    CHECK(su_host_input_push(w, su_host_tick(w), in, 2u, &e) == SU_OK);
    g_allocs = 0u;
    g_count_alloc = 1;
    CHECK(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u);
    g_count_alloc = 0;
    CHECK(g_allocs == 0u);
    /* A failing tick (a write to an entity that dies before it) allocates nothing either. */
    in[0].entity = ships[1];
    in[0].field_id = 2u;
    in[0].value = 4u;
    CHECK(su_host_input_push(w, su_host_tick(w), in, 1u, &e) == SU_OK);
    CHECK(su_host_despawn(w, ships[1], &e) == SU_OK);
    g_allocs = 0u;
    g_count_alloc = 1;
    CHECK(su_host_step(w, 1u, &done, &e) == SU_E_STALE_HANDLE && done == 0u);
    g_count_alloc = 0;
    CHECK(g_allocs == 0u);
    su_host_world_destroy(w);
    /* A failure inside a system run (the second emit_event of a tick over the world's event cap) allocates
     * nothing either, and leaves the committed root as it was. */
    {
        Bytes ec = slurp("tests/fixtures/entities/emit_then_despawn.capsule.json");
        Bytes el = slurp("tests/fixtures/entities/emit_then_despawn.lir.json");
        SuHostCapsule cap;
        SuHostWorldDesc d;
        SuHostWorld *ew = NULL;
        uint8_t before[32], after[32];
        memset(&cap, 0, sizeof cap);
        cap.capsule_json = ec.data;
        cap.capsule_len = ec.len;
        cap.lir_json = el.data;
        cap.lir_len = el.len;
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.capsule_count = 1;
        d.capsules = &cap;
        d.tick_rate_hz = 120;
        d.max_entities = 4u;
        d.max_events_per_tick = 1u; /* the capsule declares one event per row; two rows emit in one chunk */
        CHECK(su_host_world_create(&d, &ew, &e) == SU_OK);
        free(ec.data);
        free(el.data);
        if (ew != NULL) {
            uint64_t h = 0;
            /* Warm up the entity cache with an empty tick: the first entity tick builds the cache (allocates). */
            CHECK(su_host_step(ew, 1u, &done, &e) == SU_OK && done == 1u);
            sv.field_id = 2u; /* velocity_x = 1 */
            sv.value = 1u;
            CHECK(su_host_spawn_in(ew, 0u, &sv, 1u, 0u, &h, &e) == SU_OK);
            CHECK(su_host_spawn_in(ew, 0u, &sv, 1u, 1u, &h, &e) == SU_OK);
            CHECK(su_host_digest(ew, before) == SU_OK);
            g_allocs = 0u;
            g_count_alloc = 1;
            CHECK(su_host_step(ew, 1u, &done, &e) == SU_E_RESOURCE_LIMIT && done == 0u);
            g_count_alloc = 0;
            CHECK(g_allocs == 0u);
            CHECK(su_host_digest(ew, after) == SU_OK && memcmp(before, after, 32u) == 0);
            su_host_world_destroy(ew);
        }
    }
#endif
}

/* Descriptor versioning: the 96-byte descriptor of the previous release is still a valid single-table world
 * (fields beyond struct_size read as zero, so no tables), and a size that ends inside a field is refused. */
static void test_descriptor_versions(void) {
    SuHostWorld *w3 = NULL;
    SuHostWorld *w4 = NULL;
    SuHostWorldDesc d;
    SuHostError e;
    uint8_t id3[32], id4[32];
    uint32_t st;

    memset(&d, 0, sizeof d);
    d.struct_size = SU_HOST_WORLD_DESC_V3_SIZE;
    d.capsule_count = 1;
    d.capsules = &g_ship;
    d.tick_rate_hz = 120;
    d.max_entities = 2u;
    CHECK(su_host_world_create(&d, &w3, &e) == SU_OK);
    CHECK(su_host_table_count(w3) == 1u);
    d.struct_size = (uint32_t)sizeof d;
    CHECK(su_host_world_create(&d, &w4, &e) == SU_OK);
    CHECK(su_host_table_count(w4) == 1u);
    CHECK(su_host_world_identity(w3, id3) == SU_OK && su_host_world_identity(w4, id4) == SU_OK &&
          memcmp(id3, id4, 32) == 0);
    {
        SuHostWorld *bad = NULL;
        d.struct_size = SU_HOST_WORLD_DESC_V3_SIZE + 4u; /* ends inside the tables pointer */
        st = su_host_world_create(&d, &bad, &e);
        CHECK(st == SU_E_INVALID_ARGUMENT && bad == NULL);
    }
    su_host_world_destroy(w3);
    su_host_world_destroy(w4);
}

/* The categories su_host_memory appends (issue 140): queued input records and the in-place restore scratch are
 * counted, so the "exact" claim covers them too. */
static void test_memory_categories(void) {
    TableSchema ship, pool;
    SuHostWorld *w;
    SuHostError e;
    SuHostMemory m;
    SuHostValue sv = {2u, 1u};
    SuHostInput in;
    uint64_t h = 0;
    uint64_t done = 0;
    uint8_t *snap = NULL;
    size_t size = 0;

    ship_schema(&ship);
    pool_schema(&pool);
    w = make_two(&ship, &pool, 4u, 4u, NULL, 0u, NULL, 0u, &e);
    memset(&m, 0, sizeof m);
    m.struct_size = (uint32_t)sizeof m;
    CHECK(su_host_memory(w, &m) == SU_OK);
    CHECK(m.queued_inputs == 0u && m.restore_scratch == 0u);
    CHECK(su_host_spawn_in(w, 0u, &sv, 1u, 0u, &h, &e) == SU_OK);
    CHECK(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u);
    in.entity = h;
    in.field_id = 2u;
    in.value = 3u;
    CHECK(su_host_input_push(w, su_host_tick(w), &in, 1u, &e) == SU_OK);
    memset(&m, 0, sizeof m);
    m.struct_size = (uint32_t)sizeof m;
    CHECK(su_host_memory(w, &m) == SU_OK);
    CHECK(m.queued_inputs > 0u);
    CHECK(m.total == m.tables_total + m.input_storage + m.queued_inputs + m.restore_scratch + m.metadata);
    /* The first in-place restore allocates its scratch; the next su_host_memory reports it. */
    CHECK(su_host_snapshot_size(w, &size) == SU_OK && size > 0u);
    snap = malloc(size);
    CHECK(snap != NULL && su_host_snapshot_write(w, snap, size, &size) == SU_OK);
    CHECK(su_host_snapshot_restore_inplace(w, snap, size, &e) == SU_OK);
    free(snap);
    memset(&m, 0, sizeof m);
    m.struct_size = (uint32_t)sizeof m;
    CHECK(su_host_memory(w, &m) == SU_OK);
    CHECK(m.restore_scratch > 0u);
    CHECK(m.total == m.tables_total + m.input_storage + m.queued_inputs + m.restore_scratch + m.metadata);
    /* A caller that passes the first released size gets the fields it covers, and the full total. */
    {
        SuHostMemory v1;
        memset(&v1, 0, sizeof v1);
        v1.struct_size = SU_HOST_MEMORY_V1_SIZE;
        CHECK(su_host_memory(w, &v1) == SU_OK);
        CHECK(v1.total == m.total && v1.tables_total == m.tables_total);
    }
    su_host_world_destroy(w);
}

int main(void) {
    g_cap = slurp("examples/movement.capsule.json");
    g_lir = slurp("examples/movement.lir.json");
    g_ship.capsule_json = g_cap.data;
    g_ship.capsule_len = g_cap.len;
    g_ship.lir_json = g_lir.data;
    g_ship.lir_len = g_lir.len;
    g_pool.capsule_json = (const uint8_t *)k_pool_capsule;
    g_pool.capsule_len = sizeof k_pool_capsule - 1u;
    g_pool.lir_json = (const uint8_t *)k_pool_lir;
    g_pool.lir_len = sizeof k_pool_lir - 1u;
    g_union_cap.capsule_json = (const uint8_t *)k_union_capsule;
    g_union_cap.capsule_len = sizeof k_union_capsule - 1u;
    g_union_cap.lir_json = (const uint8_t *)k_union_lir;
    g_union_cap.lir_len = sizeof k_union_lir - 1u;

    test_create_and_tables();
    test_spawn_despawn_both_tables();
    test_inputs_across_tables();
    test_snapshot_round_trip();
    test_replay_round_trip();
    test_memory_accounting();
    test_memory_categories();
    test_refusals();
    test_large_idle_table();
    test_descriptor_versions();
    test_no_allocation_in_tick();
    printf("test_host_tables: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
