/* Observation bench for multi-table worlds (include/sutekh/host.h, "tables"; issue 140): the per-tick cost of a
 * ship-only system with and without a large second table.
 *
 * The motivating shape: a small table of ships stepped by one system, next to a large table of projectiles that
 * the ship system does not iterate. With per-table begin/commit/publish the large table is not touched by a tick
 * whose systems are all in the small one; with one union table every system pays for every row of the union.
 *
 * Configurations (all at 2 lanes per kind, one system per kind):
 *   ships_only      one table (ships), one system
 *   ships_plus_idle two tables, the ship system only (the pool table has no system)
 *   ships_plus_pool two tables, the ship system and one pool system
 *   union_one_table one table of the union schema at the pool's capacity, the ship system and the pool system
 *
 * Output is one JSON object on stdout with per-configuration ns/tick (median and p95 of per-tick samples). These
 * are observations on the machine that ran it, not qualification against any target (constitution C10).
 *
 * Usage: bench_host_tables [ships] [pool_rows] [ticks]   (run from the repository root) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/su_platform.h"
#include "sutekh/entities.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b = malloc((size_t)n + 1u);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static uint64_t now_ns(void) {
    uint64_t t = 0;
    (void)su_platform_monotonic_ns(&t);
    return t;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void stats(uint64_t *s, uint32_t n, double *median_ns, double *p95_ns) {
    qsort(s, n, sizeof *s, cmp_u64);
    *median_ns = (double)s[n / 2u];
    *p95_ns = (double)s[(uint32_t)((uint64_t)n * 95u / 100u)];
}

/* The second kind: schema demo.pool, one read lane and one read-write lane. */
static const char k_pool_capsule[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"bench.pool_tick\",\"source\":\"pool.lir.json\","
    "\"schema\":{\"id\":\"bench.pool\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"u32_mod_v1\","
    "\"fields\":[{\"id\":1,\"name\":\"remaining\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":2,\"name\":\"damage\",\"type\":\"u32\",\"read\":true,\"write\":false}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":64,\"max_rows\":1000000,\"allocation\":\"forbidden\"},\"tests\":[\"pool.test.json\"]}";
static const char k_pool_lir[] =
    "{\"ir_version\":\"0.1\",\"system_id\":\"bench.pool_tick\",\"numeric_profile\":\"u32_mod_v1\","
    "\"query\":\"bench.pool\",\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"d\",\"field\":\"damage\"},"
    "{\"op\":\"store_u32\",\"field\":\"remaining\",\"src\":\"d\"}]}";
/* The union of both kinds, for the single-table baseline. */
static const char k_union_capsule[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"bench.union_tick\",\"source\":\"union.lir.json\","
    "\"schema\":{\"id\":\"bench.union\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"u32_mod_v1\","
    "\"fields\":[{\"id\":1,\"name\":\"position_x\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":2,\"name\":\"velocity_x\",\"type\":\"u32\",\"read\":true,\"write\":false}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":64,\"max_rows\":1000000,\"allocation\":\"forbidden\"},\"tests\":[\"pool.test.json\"]}";
static const char k_union_lir[] =
    "{\"ir_version\":\"0.1\",\"system_id\":\"bench.union_tick\",\"numeric_profile\":\"u32_mod_v1\","
    "\"query\":\"bench.union\",\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"v\",\"field\":\"velocity_x\"},"
    "{\"op\":\"store_u32\",\"field\":\"position_x\",\"src\":\"v\"}]}";
/* A pool system over the union schema (its lanes are 3 and 4). */
static const char k_union_pool_capsule[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"bench.union_pool\",\"source\":\"union_pool.lir.json\","
    "\"schema\":{\"id\":\"bench.union\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"u32_mod_v1\","
    "\"fields\":[{\"id\":3,\"name\":\"remaining\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":4,\"name\":\"damage\",\"type\":\"u32\",\"read\":true,\"write\":false}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":64,\"max_rows\":1000000,\"allocation\":\"forbidden\"},\"tests\":[\"pool.test.json\"]}";
static const char k_union_pool_lir[] =
    "{\"ir_version\":\"0.1\",\"system_id\":\"bench.union_pool\",\"numeric_profile\":\"u32_mod_v1\","
    "\"query\":\"bench.union\",\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"d\",\"field\":\"damage\"},"
    "{\"op\":\"store_u32\",\"field\":\"remaining\",\"src\":\"d\"}]}";

typedef struct Schema {
    SuHostSchema s;
    SuHostSchemaField f[4];
} Schema;

static void ship_schema(Schema *t, const char *id) {
    memset(t, 0, sizeof *t);
    t->s.struct_size = (uint32_t)sizeof t->s;
    t->s.field_count = 2;
    t->s.fields = t->f;
    t->s.schema_id = id;
    t->s.schema_version = 1;
    t->s.layout_generation = 1;
    t->f[0].id = 1;
    t->f[0].type = SU_TYPE_U32;
    t->f[0].name = "position_x";
    t->f[1].id = 2;
    t->f[1].type = SU_TYPE_U32;
    t->f[1].name = "velocity_x";
}

static void pool_schema(Schema *t, const char *id) {
    memset(t, 0, sizeof *t);
    t->s.struct_size = (uint32_t)sizeof t->s;
    t->s.field_count = 2;
    t->s.fields = t->f;
    t->s.schema_id = id;
    t->s.schema_version = 1;
    t->s.layout_generation = 1;
    t->f[0].id = 1;
    t->f[0].type = SU_TYPE_U32;
    t->f[0].name = "remaining";
    t->f[1].id = 2;
    t->f[1].type = SU_TYPE_U32;
    t->f[1].name = "damage";
}

static void union_schema(Schema *t) {
    memset(t, 0, sizeof *t);
    t->s.struct_size = (uint32_t)sizeof t->s;
    t->s.field_count = 4;
    t->s.fields = t->f;
    t->s.schema_id = "bench.union";
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

static SuHostWorld *make(Schema *ship, Schema *pool, Schema *uni, uint32_t mode, uint64_t ships, uint64_t pool_rows,
                         SuHostCapsule *ship_cap, SuHostCapsule *pool_cap, SuHostCapsule *uni_cap,
                         SuHostCapsule *uni_pool_cap, SuHostError *e) {
    static SuHostCapsule two_caps[2];
    static SuHostCapsule union_caps[2];
    SuHostTableDesc td[2];
    uint32_t assign[2] = {0u, 1u};
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;

    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.tick_rate_hz = 120;
    d.event_log_max = 64;
    switch (mode) {
    case 0: /* one table: ships only */
        ship_schema(ship, "demo.actor");
        d.capsule_count = 1;
        d.capsules = ship_cap;
        d.schema = &ship->s;
        d.max_entities = ships;
        break;
    case 1: /* two tables, the ship system only */
    case 2: /* two tables, the ship system and one pool system */
        ship_schema(ship, "demo.actor");
        pool_schema(pool, "bench.pool");
        memset(td, 0, sizeof td);
        td[0].struct_size = (uint32_t)sizeof td[0];
        td[0].schema = &ship->s;
        td[0].max_entities = ships;
        td[1].struct_size = (uint32_t)sizeof td[1];
        td[1].schema = &pool->s;
        td[1].max_entities = pool_rows;
        two_caps[0] = *ship_cap;
        two_caps[1] = *pool_cap;
        d.capsule_count = mode == 2u ? 2u : 1u;
        d.capsules = two_caps;
        d.table_count = 2;
        d.tables = td;
        d.capsule_table = assign;
        break;
    default: /* one union table with both systems */
        union_schema(uni);
        union_caps[0] = *uni_cap;
        union_caps[1] = *uni_pool_cap;
        d.capsule_count = 2;
        d.capsules = union_caps;
        d.schema = &uni->s;
        d.max_entities = ships + pool_rows; /* one table holds both kinds */
        break;
    }
    if (su_host_world_create(&d, &w, e) != SU_OK) {
        fprintf(stderr, "create failed (mode %u): %s\n", (unsigned)mode, e->message);
        exit(1);
    }
    return w;
}

int main(int argc, char **argv) {
    uint64_t ships = argc > 1 ? (uint64_t)strtoull(argv[1], NULL, 10) : 2000u;
    uint64_t pool_rows = argc > 2 ? (uint64_t)strtoull(argv[2], NULL, 10) : 27500u;
    uint32_t ticks = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 200u;
    size_t cap_len = 0, lir_len = 0;
    SuHostCapsule ship_cap, pool_cap, uni_cap, uni_pool_cap;
    Schema ship, pool, uni;
    uint64_t *samples;
    uint32_t mode;
    uint32_t first = 1;

    if (ticks < 20u) {
        ticks = 20u;
    }
    ship_cap.capsule_json = slurp("examples/movement.capsule.json", &cap_len);
    ship_cap.capsule_len = cap_len;
    ship_cap.lir_json = slurp("examples/movement.lir.json", &lir_len);
    ship_cap.lir_len = lir_len;
    pool_cap.capsule_json = (const uint8_t *)k_pool_capsule;
    pool_cap.capsule_len = sizeof k_pool_capsule - 1u;
    pool_cap.lir_json = (const uint8_t *)k_pool_lir;
    pool_cap.lir_len = sizeof k_pool_lir - 1u;
    uni_cap.capsule_json = (const uint8_t *)k_union_capsule;
    uni_cap.capsule_len = sizeof k_union_capsule - 1u;
    uni_cap.lir_json = (const uint8_t *)k_union_lir;
    uni_cap.lir_len = sizeof k_union_lir - 1u;
    uni_pool_cap.capsule_json = (const uint8_t *)k_union_pool_capsule;
    uni_pool_cap.capsule_len = sizeof k_union_pool_capsule - 1u;
    uni_pool_cap.lir_json = (const uint8_t *)k_union_pool_lir;
    uni_pool_cap.lir_len = sizeof k_union_pool_lir - 1u;
    samples = malloc((size_t)ticks * sizeof *samples);
    if (samples == NULL) {
        return 1;
    }
    printf("{\"bench\":\"host_tables\",\"kind\":\"observation\",\"ships\":%llu,\"pool_rows\":%llu,\"ticks\":%u,"
           "\"cpus\":%u,\"results\":[",
           (unsigned long long)ships, (unsigned long long)pool_rows, (unsigned)ticks,
           (unsigned)su_platform_cpu_count());
    for (mode = 0; mode < 4u; mode++) {
        static const char *const names[4] = {"ships_only", "ships_plus_idle_table", "ships_plus_pool_table",
                                             "union_one_table"};
        SuHostWorld *w;
        SuHostError e;
        SuHostMemory m;
        uint64_t k;
        uint32_t i;
        double med = 0.0, p95 = 0.0;
        uint64_t warm;

        w = make(&ship, &pool, &uni, mode, ships, pool_rows, &ship_cap, &pool_cap, &uni_cap, &uni_pool_cap, &e);
        for (k = 0; k < ships; k++) {
            SuHostValue v[2] = {{1u, (uint32_t)k}, {2u, 1u + (uint32_t)(k % 7u)}};
            uint64_t h = 0;
            if (su_host_spawn_in(w, 0u, v, 2u, 0u, &h, &e) != SU_OK) {
                fprintf(stderr, "spawn failed: %s\n", e.message);
                return 1;
            }
        }
        if (mode == 2u || mode == 3u) {
            for (k = 0; k < pool_rows; k++) {
                SuHostValue v[1] = {{2u, 1u + (uint32_t)(k % 5u)}};
                uint64_t h = 0;
                if (su_host_spawn_in(w, mode == 3u ? 0u : 1u, v, 1u, 0u, &h, &e) != SU_OK) {
                    fprintf(stderr, "pool spawn failed: %s\n", e.message);
                    return 1;
                }
            }
        }
        for (warm = 0; warm < 10u; warm++) {
            if (su_host_step(w, 1u, NULL, &e) != SU_OK) {
                fprintf(stderr, "warm-up step failed: %s\n", e.message);
                return 1;
            }
        }
        for (i = 0; i < ticks; i++) {
            uint64_t t0 = now_ns();
            if (su_host_step(w, 1u, NULL, &e) != SU_OK) {
                fprintf(stderr, "step failed: %s\n", e.message);
                return 1;
            }
            samples[i] = now_ns() - t0;
        }
        stats(samples, ticks, &med, &p95);
        memset(&m, 0, sizeof m);
        m.struct_size = (uint32_t)sizeof m;
        (void)su_host_memory(w, &m);
        printf("%s{\"name\":\"%s\",\"median_ns_per_tick\":%.0f,\"p95_ns_per_tick\":%.0f,\"tables\":%u,"
               "\"world_bytes\":%llu}",
               first ? "" : ",", names[mode], med, p95, (unsigned)su_host_table_count(w),
               (unsigned long long)m.total);
        first = 0;
        su_host_world_destroy(w);
    }
    printf("]}\n");
    free(samples);
    return 0;
}
