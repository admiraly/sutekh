/* Worlds with several entity kinds, slice 2 (include/sutekh/host.h, "tables"; issue 140): a table-aware entity
 * tick (IR despawn_self/emit_event in every table), operator events in a multi-table world, world-slot semantics
 * for slot_u32/gen_u32/handle_live, cross-table space3d (movers in one table, targets in another), a stateful
 * registered operator in a table other than 0, and the end-to-end two-kind world of the issue.
 *
 * The example world: ships in table 0 (IR systems that move, emit events and despawn themselves) and projectiles
 * in table 1 (spawned by the host, moved and despawned by IR systems in table 1, collided against the ships by
 * space3d.query bound to table 0 as its target set). It runs ticks with spawns, events, despawns and cross-table
 * queries, and passes a snapshot and a replay round trip.
 *
 * Usage: test_host_tables_entities   (run from the repository root: it reads profiles/) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/entities.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"
#include "sutekh/replay.h"

static unsigned g_checks, g_failures;

static void check(int cond, const char *fmt, ...) {
    g_checks++;
    if (!cond) {
        va_list ap;
        g_failures++;
        fprintf(stderr, "FAIL: ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

/* Allocation counting for "no allocation in a tick" (static Linux build: -Wl,--wrap=malloc,...). */
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
    b.data[b.len] = '\0';
    fclose(f);
    return b;
}

/* ---------- JSON builders (the worlds of this test are built in memory) ---------- */

typedef struct Buf {
    char *p;
    size_t n, cap;
} Buf;

static void put(Buf *b, const char *fmt, ...) {
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->p != NULL ? b->p + b->n : NULL, b->p != NULL ? b->cap - b->n : 0u, fmt, ap);
        va_end(ap);
        if (k < 0) {
            fprintf(stderr, "vsnprintf failed\n");
            exit(2);
        }
        if (b->p != NULL && b->n + (size_t)k < b->cap) {
            b->n += (size_t)k;
            return;
        }
        b->cap = (b->cap + (size_t)k + 256u) * 2u;
        b->p = realloc(b->p, b->cap);
        if (b->p == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
    }
}

static const char *type_name(uint32_t t) {
    return t == SU_TYPE_I64 ? "i64" : t == SU_TYPE_Q32 ? "q32" : t == SU_TYPE_I32 ? "i32" : "u32";
}

typedef struct IrField {
    uint32_t id;
    const char *name;
    const char *type;   /* "u32" or "q32" */
    int read, write;
} IrField;

/* An IR capsule record for a system of `schema`. after: a JSON list body ("" for none). caps: capability names,
 * a JSON list body ("\"despawn_self\",\"emit_event\""), empty for none. */
static Buf ir_capsule(const char *system_id, const char *schema, const char *phase, const char *after,
                      const IrField *f, uint32_t n, const char *ir_version, const char *profile,
                      const char *caps, uint32_t max_struct, uint32_t max_events) {
    Buf b = {NULL, 0, 0};
    uint32_t i;
    put(&b, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"source\":\"inline.lir.json\",", system_id);
    put(&b, "\"schema\":{\"id\":\"%s\",\"version\":1,\"layout_generation\":1},", schema);
    put(&b, "\"numeric_profile\":\"%s\",\"ir_version\":\"%s\",\"capabilities\":[%s],", profile, ir_version, caps);
    put(&b, "\"fields\":[");
    for (i = 0; i < n; i++) {
        put(&b, "%s{\"id\":%u,\"name\":\"%s\",\"type\":\"%s\",\"read\":%s,\"write\":%s}", i > 0u ? "," : "",
            (unsigned)f[i].id, f[i].name, f[i].type, f[i].read ? "true" : "false", f[i].write ? "true" : "false");
    }
    put(&b, "],\"schedule\":{\"phase\":\"%s\",\"after\":[%s],\"before\":[],\"parallel\":\"serial\"},", phase, after);
    put(&b, "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"");
    put(&b, ",\"max_struct_per_row\":%u,\"max_events_per_row\":%u},", (unsigned)max_struct, (unsigned)max_events);
    put(&b, "\"tests\":[\"tests/host/test_host_tables_entities.c\"]}");
    return b;
}

/* A host operator record (exact binding: the record's lanes are the descriptor's). */
static Buf host_record(const char *system_id, const char *schema, const char *operator_name, uint32_t version,
                       const uint8_t hash[32], const SuHostOperatorField *f, uint32_t n, const char *phase,
                       const char *profile) {
    Buf b = {NULL, 0, 0};
    uint32_t i;
    put(&b, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"system_rev\":1,\"capabilities\":[],", system_id);
    put(&b, "\"schema\":{\"id\":\"%s\",\"version\":1,\"layout_generation\":1},", schema);
    put(&b, "\"numeric_profile\":\"%s\",", profile);
    put(&b, "\"schedule\":{\"phase\":\"%s\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},", phase);
    put(&b, "\"host\":{\"operator\":\"%s\",\"operator_version\":%u,\"constants_hash\":\"", operator_name,
        (unsigned)version);
    for (i = 0; i < 32u; i++) {
        put(&b, "%02x", (unsigned)hash[i]);
    }
    put(&b, "\",\"fields\":[");
    for (i = 0; i < n; i++) {
        const char *acc = f[i].access == SU_ACCESS_READ ? "read"
                          : f[i].access == SU_ACCESS_WRITE ? "write"
                                                          : "read_write";
        put(&b, "%s{\"id\":%u,\"name\":\"%s\",\"type\":\"%s\",\"access\":\"%s\"}", i > 0u ? "," : "",
            (unsigned)f[i].id, f[i].name, type_name(f[i].type), acc);
    }
    put(&b, "]}}");
    return b;
}

/* ---------- schemas ---------- */

typedef struct SField {
    uint32_t id;
    uint32_t type;
    const char *name;
} SField;

#define Q(id, name) {id, SU_TYPE_Q32, name}
#define U(id, name) {id, SU_TYPE_U32, name}

/* The space3d.query schema of the projectile table: exactly the lanes its record binds (the base query variant),
 * plus the two lanes the projectile IR systems use. */
static const SField k_proj_schema[] = {
    U(1, "kind"),       U(2, "next_gate"),  U(3, "gate_index"), Q(10, "pos_x"),     Q(12, "pos_y"),
    Q(14, "pos_z"),     Q(16, "prev_x"),    Q(18, "prev_y"),    Q(20, "prev_z"),    Q(22, "rot_w"),
    Q(24, "rot_x"),     Q(26, "rot_y"),     Q(28, "rot_z"),     Q(30, "radius"),    Q(32, "half_x"),
    Q(34, "half_y"),    Q(36, "half_z"),    U(40, "hit_count"), U(41, "hit_flags"), U(42, "c0_slot"),
    U(43, "c0_gen"),    Q(44, "c0_toi"),    Q(46, "c0_nx"),     Q(48, "c0_ny"),     Q(50, "c0_nz"),
    U(52, "c1_slot"),   U(53, "c1_gen"),    Q(54, "c1_toi"),    Q(56, "c1_nx"),     Q(58, "c1_ny"),
    Q(60, "c1_nz"),     U(62, "gate_dir"),  U(63, "gate_slot"), Q(64, "gate_toi"),  U(200, "ttl"),
    Q(202, "vel_x"),
};

/* The ship table: the space3d target set's lanes (kind, gate_index, pose, radius, half extents) plus the lanes the
 * ship IR systems use. */
static const SField k_ship_schema[] = {
    U(1, "kind"),    U(3, "gate_index"), Q(10, "pos_x"),  Q(12, "pos_y"), Q(14, "pos_z"),
    Q(22, "rot_w"),  Q(24, "rot_x"),     Q(26, "rot_y"),  Q(28, "rot_z"), Q(30, "radius"),
    Q(32, "half_x"), Q(34, "half_y"),    Q(36, "half_z"), U(210, "hp"),   Q(212, "vx"),
};

/* Builds an SuHostSchema over a static field table (dst holds n entries). */
static void fill_schema(SuHostSchema *s, SuHostSchemaField *dst, const SField *src, uint32_t n, const char *id) {
    uint32_t i;
    memset(s, 0, sizeof *s);
    s->struct_size = (uint32_t)sizeof *s;
    s->field_count = n;
    s->fields = dst;
    s->schema_id = id;
    s->schema_version = 1;
    s->layout_generation = 1;
    for (i = 0; i < n; i++) {
        dst[i].id = src[i].id;
        dst[i].type = src[i].type;
        dst[i].name = src[i].name;
    }
}

#define N_PROJ (sizeof k_proj_schema / sizeof k_proj_schema[0])
#define N_SHIP (sizeof k_ship_schema / sizeof k_ship_schema[0])

/* ---------- the systems ---------- */

/* game.ship_move (schema game.ship): pos_x += vx (q32). */
static const IrField k_ship_move_fields[] = {
    {10, "pos_x", "q32", 1, 1},
    {212, "vx", "q32", 1, 0},
};
static const char k_ship_move_lir[] =
    "{\"ir_version\":\"0.3\",\"system_id\":\"game.ship_move\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"game.ship\",\"instructions\":["
    "{\"op\":\"load_q32\",\"dst\":\"p\",\"field\":\"pos_x\"},"
    "{\"op\":\"load_q32\",\"dst\":\"v\",\"field\":\"vx\"},"
    "{\"op\":\"add_q32\",\"dst\":\"n\",\"a\":\"p\",\"b\":\"v\"},"
    "{\"op\":\"store_q32\",\"field\":\"pos_x\",\"src\":\"n\"}]}";

/* game.ship_die (schema game.ship): a ship with hp 0 emits one event about its world slot and despawns itself. */
static const IrField k_ship_die_fields[] = {
    {210, "hp", "u32", 1, 0},
};
static const char k_ship_die_lir[] =
    "{\"ir_version\":\"0.3\",\"system_id\":\"game.ship_die\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"game.ship\",\"instructions\":["
    "{\"op\":\"load_u32\",\"dst\":\"h\",\"field\":\"hp\"},"
    "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},"
    "{\"op\":\"eq_u32\",\"dst\":\"dead\",\"a\":\"h\",\"b\":\"z\"},"
    "{\"op\":\"slot_u32\",\"dst\":\"s\"},"
    "{\"op\":\"emit_event\",\"cond\":\"dead\",\"kind\":3,\"a\":\"s\",\"b\":\"h\"},"
    "{\"op\":\"despawn_self\",\"cond\":\"dead\"}]}";

/* game.proj_move (schema space3d): prev = pos; pos_x += vel_x. */
static const IrField k_proj_move_fields[] = {
    {10, "pos_x", "q32", 1, 1},
    {16, "prev_x", "q32", 0, 1},
    {202, "vel_x", "q32", 1, 0},
};
static const char k_proj_move_lir[] =
    "{\"ir_version\":\"0.3\",\"system_id\":\"game.proj_move\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"space3d\",\"instructions\":["
    "{\"op\":\"load_q32\",\"dst\":\"p\",\"field\":\"pos_x\"},"
    "{\"op\":\"store_q32\",\"field\":\"prev_x\",\"src\":\"p\"},"
    "{\"op\":\"load_q32\",\"dst\":\"v\",\"field\":\"vel_x\"},"
    "{\"op\":\"add_q32\",\"dst\":\"n\",\"a\":\"p\",\"b\":\"v\"},"
    "{\"op\":\"store_q32\",\"field\":\"pos_x\",\"src\":\"n\"}]}";

/* game.proj_hit (schema space3d): a projectile that space3d reported a contact for emits one event about the row it
 * hit (the contact slot, a world slot of the target table) and despawns itself. */
static const IrField k_proj_hit_fields[] = {
    {40, "hit_count", "u32", 1, 0},
    {42, "c0_slot", "u32", 1, 0},
    {43, "c0_gen", "u32", 1, 0},
};
static const char k_proj_hit_lir[] =
    "{\"ir_version\":\"0.3\",\"system_id\":\"game.proj_hit\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"space3d\",\"instructions\":["
    "{\"op\":\"load_u32\",\"dst\":\"n\",\"field\":\"hit_count\"},"
    "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},"
    "{\"op\":\"eq_u32\",\"dst\":\"none\",\"a\":\"n\",\"b\":\"z\"},"
    "{\"op\":\"not_bool\",\"dst\":\"hit\",\"a\":\"none\"},"
    "{\"op\":\"load_u32\",\"dst\":\"s\",\"field\":\"c0_slot\"},"
    "{\"op\":\"load_u32\",\"dst\":\"g\",\"field\":\"c0_gen\"},"
    "{\"op\":\"emit_event\",\"cond\":\"hit\",\"kind\":10,\"a\":\"s\",\"b\":\"g\"},"
    "{\"op\":\"despawn_self\",\"cond\":\"hit\"}]}";

/* game.proj_life (schema space3d): ttl -= 1; at 0 the projectile emits one event about its own row and despawns. */
static const IrField k_proj_life_fields[] = {
    {200, "ttl", "u32", 1, 1},
};
static const char k_proj_life_lir[] =
    "{\"ir_version\":\"0.3\",\"system_id\":\"game.proj_life\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"space3d\",\"instructions\":["
    "{\"op\":\"load_u32\",\"dst\":\"t\",\"field\":\"ttl\"},"
    "{\"op\":\"const_u32\",\"dst\":\"one\",\"value\":1},"
    "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},"
    "{\"op\":\"sub_u32\",\"dst\":\"n\",\"a\":\"t\",\"b\":\"one\"},"
    "{\"op\":\"store_u32\",\"field\":\"ttl\",\"src\":\"n\"},"
    "{\"op\":\"eq_u32\",\"dst\":\"out\",\"a\":\"n\",\"b\":\"z\"},"
    "{\"op\":\"slot_u32\",\"dst\":\"s\"},"
    "{\"op\":\"emit_event\",\"cond\":\"out\",\"kind\":9,\"a\":\"s\",\"b\":\"n\"},"
    "{\"op\":\"despawn_self\",\"cond\":\"out\"}]}";

/* ---------- the example world ---------- */

#define SHIP_ROWS 8u
#define PROJ_ROWS 32u
#define SHIP_BASE 0u
#define PROJ_BASE SHIP_ROWS
#define NSYS 6u

/* Q32.32 raw value of an integer. */
#define Q32_ONE INT64_C(4294967296)
static int64_t q32(int64_t v) {
    return v * Q32_ONE;
}

/* Adds both storage lanes of a q32 field (lane 0 at id, lane 1 at id + 1) to a spawn value list. */
static uint32_t put_q32(SuHostValue *v, uint32_t n, uint32_t id, int64_t raw) {
    v[n].field_id = id;
    v[n].value = (uint32_t)((uint64_t)raw & 0xFFFFFFFFu);
    v[n + 1u].field_id = id + 1u;
    v[n + 1u].value = (uint32_t)((uint64_t)raw >> 32);
    return n + 2u;
}

typedef struct Example {
    Buf cap[NSYS];
    Bytes query;
    SuHostCapsule caps[NSYS];
    SuHostTableDesc td[2];
    SuHostSchemaField ship_fields[N_SHIP], proj_fields[N_PROJ];
    SuHostSchema ship_s, proj_s;
    SuHostWorldDesc desc;
    uint32_t tables[NSYS];
    uint32_t targets[NSYS];
} Example;

static void example_free(Example *x) {
    uint32_t i;
    for (i = 0; i < NSYS; i++) {
        free(x->cap[i].p);
    }
    free(x->query.data);
}

/* Creates the world of the issue's example, or returns NULL with the reason in why. */
static SuHostWorld *example_world(Example *x, SuHostError *err) {
    SuHostWorld *w = NULL;
    uint32_t i;
    static const char *const ids[NSYS] = {"game.ship_move", "game.ship_die", "game.proj_move", "space3d.query",
                                          "game.proj_hit", "game.proj_life"};
    static const char *const lirs[NSYS] = {k_ship_move_lir, k_ship_die_lir, k_proj_move_lir, NULL, k_proj_hit_lir,
                                           k_proj_life_lir};
    static const IrField *const flds[NSYS] = {k_ship_move_fields, k_ship_die_fields, k_proj_move_fields, NULL,
                                              k_proj_hit_fields, k_proj_life_fields};
    static const uint32_t nf[NSYS] = {
        (uint32_t)(sizeof k_ship_move_fields / sizeof k_ship_move_fields[0]),
        (uint32_t)(sizeof k_ship_die_fields / sizeof k_ship_die_fields[0]),
        (uint32_t)(sizeof k_proj_move_fields / sizeof k_proj_move_fields[0]), 0u,
        (uint32_t)(sizeof k_proj_hit_fields / sizeof k_proj_hit_fields[0]),
        (uint32_t)(sizeof k_proj_life_fields / sizeof k_proj_life_fields[0])};
    static const char *const after[NSYS] = {"", "", "", "\"game.proj_move\"", "\"space3d.query\"", ""};

    memset(x, 0, sizeof *x);
    x->query = slurp("profiles/space3d/space3d_query.capsule.json");
    for (i = 0; i < NSYS; i++) {
        if (i == 3u) {
            x->caps[i].capsule_json = x->query.data; /* the space3d.query record, unmodified */
            x->caps[i].capsule_len = x->query.len;
            continue;
        }
        x->cap[i] = ir_capsule(ids[i], i < 2u ? "game.ship" : "space3d", "simulate", after[i], flds[i], nf[i],
                               "0.3", "fixed_q32_v1",
                               i == 0u || i == 2u ? "" : "\"despawn_self\",\"emit_event\"", i == 0u || i == 2u ? 0u : 1u,
                               i == 0u || i == 2u ? 0u : 1u);
        x->caps[i].capsule_json = (const uint8_t *)x->cap[i].p;
        x->caps[i].capsule_len = x->cap[i].n;
        x->caps[i].lir_json = (const uint8_t *)lirs[i];
        x->caps[i].lir_len = strlen(lirs[i]);
    }
    fill_schema(&x->ship_s, x->ship_fields, k_ship_schema, (uint32_t)N_SHIP, "game.ship");
    fill_schema(&x->proj_s, x->proj_fields, k_proj_schema, (uint32_t)N_PROJ, "space3d");
    memset(x->td, 0, sizeof x->td);
    x->td[0].struct_size = (uint32_t)sizeof x->td[0];
    x->td[0].schema = &x->ship_s;
    x->td[0].max_entities = SHIP_ROWS;
    x->td[1].struct_size = (uint32_t)sizeof x->td[1];
    x->td[1].schema = &x->proj_s;
    x->td[1].max_entities = PROJ_ROWS;
    for (i = 0; i < NSYS; i++) {
        x->tables[i] = i < 2u ? 0u : 1u;
        x->targets[i] = SU_HOST_TABLE_NONE;
    }
    x->targets[3] = 0u; /* space3d.query reads table 0 (the ships) as its target set */
    memset(&x->desc, 0, sizeof x->desc);
    x->desc.struct_size = (uint32_t)sizeof x->desc;
    x->desc.capsule_count = NSYS;
    x->desc.capsules = x->caps;
    x->desc.tick_rate_hz = 120;
    x->desc.seed = 5;
    x->desc.max_events_per_tick = 64;
    x->desc.table_count = 2;
    x->desc.tables = x->td;
    x->desc.capsule_table = x->tables;
    x->desc.capsule_target_table = x->targets;
    if (su_host_world_create(&x->desc, &w, err) != SU_OK) {
        return NULL;
    }
    return w;
}

/* Spawns one ship: kind 2 (a static sphere) at x, radius 1, vx, hp. */
static uint64_t spawn_ship(SuHostWorld *w, int32_t x, int32_t vx, uint32_t hp, SuHostError *err) {
    SuHostValue v[9];
    uint64_t h = 0;
    uint32_t n = 0;
    v[n].field_id = 1u; /* kind: a static sphere */
    v[n++].value = 2u;
    n = put_q32(v, n, 10u, q32(x));    /* pos_x */
    n = put_q32(v, n, 30u, q32(1));    /* radius */
    n = put_q32(v, n, 212u, q32(vx));  /* vx */
    v[n].field_id = 210u; /* hp */
    v[n++].value = hp;
    if (su_host_spawn_in(w, 0u, v, n, 0u, &h, err) != SU_OK) {
        check(0, "ship spawn: %s", err->message);
    }
    return h;
}

/* Spawns one projectile: kind 1 (a mover) at x moving by vx, radius 1/8, ttl. */
static uint64_t spawn_proj(SuHostWorld *w, int32_t x, int32_t vx, uint32_t ttl, SuHostError *err) {
    SuHostValue v[12];
    uint64_t h = 0;
    uint32_t n = 0;
    v[n].field_id = 1u; /* kind: a mover */
    v[n++].value = 1u;
    n = put_q32(v, n, 10u, q32(x));          /* pos_x */
    n = put_q32(v, n, 16u, q32(x));          /* prev_x */
    n = put_q32(v, n, 30u, q32(1) / 8);      /* radius */
    n = put_q32(v, n, 202u, q32(vx));        /* vel_x */
    v[n].field_id = 200u; /* ttl */
    v[n++].value = ttl;
    if (su_host_spawn_in(w, 1u, v, n, 0u, &h, err) != SU_OK) {
        check(0, "projectile spawn: %s", err->message);
    }
    return h;
}

typedef struct EvList {
    SuHostEvent ev[256];
    uint32_t n;
} EvList;

static void collect_events(SuHostWorld *w, EvList *l) {
    uint64_t cur = 0;
    l->n = 0;
    for (;;) {
        uint32_t got = 0;
        uint64_t nc = 0, sk = 0;
        if (l->n >= 256u) {
            break;
        }
        if (su_host_events_read(w, cur, l->ev + l->n, 256u - l->n, &got, &nc, &sk) != SU_OK || got == 0u) {
            break;
        }
        l->n += got;
        cur = nc;
    }
}

/* The end-to-end world of the issue: two kinds, spawns, IR events and self-despawns in both tables, cross-table
 * queries, a snapshot and a replay round trip. */
static void test_end_to_end(void) {
    Example x, xr, xp;
    SuHostWorld *w;
    SuHostWorld *rw;
    SuHostError e;
    EvList ev;
    uint64_t ships[SHIP_ROWS];
    uint64_t projs[4];
    uint64_t done = 0;
    uint64_t t;
    uint32_t i, n10 = 0, n9 = 0, n3 = 0;
    uint8_t d1[32], d2[32];
    uint32_t hit_ships[SHIP_ROWS];

    memset(&ev, 0, sizeof ev);
    memset(hit_ships, 0, sizeof hit_ships);
    w = example_world(&x, &e);
    check(w != NULL, "the two-kind world creates: %s", e.message);
    if (w == NULL) {
        example_free(&x);
        return;
    }
    check(su_host_table_count(w) == 2u, "two tables");
    for (i = 0; i < SHIP_ROWS; i++) {
        ships[i] = spawn_ship(w, (int32_t)i * 4, 0, i == 3u ? 0u : 100u, &e);
        check(SU_ENTITY_SLOT(ships[i]) == i, "ship %u is table 0's local slot %u", (unsigned)i, (unsigned)i);
    }
    projs[0] = spawn_proj(w, -4, 2, 100, &e);
    projs[1] = spawn_proj(w, -4, 3, 100, &e);
    projs[2] = spawn_proj(w, -4, 100, 100, &e);
    projs[3] = spawn_proj(w, -100, 0, 3, &e);
    check(SU_ENTITY_SLOT(projs[0]) == PROJ_BASE, "the first projectile is table 1's local slot 0 (world slot %u)",
          (unsigned)SU_ENTITY_SLOT(projs[0]));
    check(su_host_step(w, 12u, &done, &e) == SU_OK && done == 12u, "twelve ticks: %s", e.message);
    /* Ship 3 (hp 0) despawned itself from its table-0 IR system. */
    {
        uint64_t h = 0;
        check(su_host_entity_at_in(w, 0u, 3u, &h) == SU_E_STALE_HANDLE, "the dead ship's slot is free");
    }
    collect_events(w, &ev);
    for (i = 0; i < ev.n; i++) {
        const SuHostEvent *v = &ev.ev[i];
        if (v->kind == 3u) {
            n3++;
            check(v->a == 3u && v->b == 0u && v->system_index == 1u,
                  "ship_die event: slot %u a %u b %u system %u (world slot of table 0 row 3)", (unsigned)v->slot,
                  (unsigned)v->a, (unsigned)v->b, (unsigned)v->system_index);
            check(v->slot == 3u, "the event's slot is the world slot of the emitting row (%u)", (unsigned)v->slot);
        } else if (v->kind == 10u) {
            n10++;
            check(v->a < SHIP_ROWS, "a hit names a ship's world slot (%u)", (unsigned)v->a);
            check(v->b == 1u, "the contact's generation is the ship's (%u)", (unsigned)v->b);
            check(v->slot >= PROJ_BASE && v->slot < PROJ_BASE + PROJ_ROWS,
                  "the hit event's slot is the projectile's world slot (%u)", (unsigned)v->slot);
            check(v->system_index == 4u, "the hit event comes from game.proj_hit (system %u)",
                  (unsigned)v->system_index);
            hit_ships[v->a % SHIP_ROWS]++;
        } else if (v->kind == 9u) {
            n9++;
            check(v->a >= PROJ_BASE && v->a < PROJ_BASE + PROJ_ROWS,
                  "the ttl event names a projectile world slot (%u)", (unsigned)v->a);
            check(v->a == v->slot, "slot_u32 in table 1 is the world slot (a %u, slot %u)", (unsigned)v->a,
                  (unsigned)v->slot);
            check(v->system_index == 5u, "the ttl event comes from game.proj_life (system %u)",
                  (unsigned)v->system_index);
        }
    }
    check(n3 == 1u, "one ship died (%u)", n3);
    check(n10 >= 2u, "the cross-table query found the ships (%u contacts)", n10);
    check(n9 == 1u, "one projectile expired (%u)", n9);
    check(hit_ships[0] > 0u, "ship 0 was hit");
    /* A tick's events are ordered by the schedule position of the system that produced them. */
    for (i = 1; i < ev.n; i++) {
        if (ev.ev[i].tick == ev.ev[i - 1u].tick) {
            check(ev.ev[i].system_index >= ev.ev[i - 1u].system_index, "events of tick %llu are in system order",
                  (unsigned long long)ev.ev[i].tick);
        }
    }
    /* Every hit projectile despawned itself; the expired one too. */
    for (i = 0; i < 3u; i++) {
        uint64_t h = 0;
        uint32_t local = (uint32_t)SU_ENTITY_SLOT(projs[i]) - PROJ_BASE;
        check(su_host_entity_at_in(w, 1u, local, &h) == SU_E_STALE_HANDLE, "projectile %u is gone", (unsigned)i);
    }
    /* Snapshot: the same bytes restore to the same digest, and a restore in place too. */
    {
        uint8_t *snap;
        size_t size = 0;
        check(su_host_digest(w, d1) == SU_OK, "check failed at line %d", __LINE__);
        check(su_host_snapshot_size(w, &size) == SU_OK && size > 0u, "check failed at line %d", __LINE__);
        snap = malloc(size);
        check(snap != NULL && su_host_snapshot_write(w, snap, size, &size) == SU_OK, "check failed at line %d", __LINE__);
        /* A multi-table world writes snapshot version 3 (host.h, "tables"). */
        check(snap != NULL && memcmp(snap, "SUHSNAP", 7u) == 0 && snap[7] == 0u, "the snapshot magic");
        check(snap != NULL && snap[8] == 3u && snap[9] == 0u && snap[10] == 0u && snap[11] == 0u,
              "the multi-table snapshot is version 3");
        check(su_host_step(w, 3u, &done, &e) == SU_OK && done == 3u, "check failed at line %d", __LINE__);
        check(su_host_snapshot_restore(w, snap, size, &e) == SU_OK, "restore: %s", e.message);
        check(su_host_digest(w, d2) == SU_OK && memcmp(d1, d2, 32u) == 0, "the restored world is the snapshot");
        check(su_host_step(w, 3u, &done, &e) == SU_OK && done == 3u, "check failed at line %d", __LINE__);
        check(su_host_snapshot_restore_inplace(w, snap, size, &e) == SU_OK, "in-place restore: %s", e.message);
        check(su_host_digest(w, d2) == SU_OK && memcmp(d1, d2, 32u) == 0, "in-place restore is the snapshot");
        free(snap);
    }
    /* Replay: record six ticks, then play them into a fresh world with the same spawns. */
    {
        SuReplayOptions opt;
        SuReplayWriter *wr = NULL;
        SuReplay *r = NULL;
        SuReplayInfo info;
        uint8_t *bytes = NULL;
        size_t len = 0;
        uint8_t before[32], after[32];

        memset(&opt, 0, sizeof opt);
        opt.struct_size = (uint32_t)sizeof opt;
        opt.keyframe_interval = 2u;
        opt.digest_interval = 1u;
        /* Record from tick 0 of a fresh world: the host's spawns are not part of the input record stream, so the
         * recording starts after them (as in the single-table host tests). */
        rw = example_world(&xr, &e);
        check(rw != NULL, "the recording world creates");
        if (rw == NULL) {
            example_free(&xr);
            return;
        }
        for (i = 0; i < SHIP_ROWS; i++) {
            spawn_ship(rw, (int32_t)i * 4, 0, i == 3u ? 0u : 100u, &e);
        }
        spawn_proj(rw, -4, 2, 100, &e);
        spawn_proj(rw, -4, 3, 100, &e);
        spawn_proj(rw, -4, 100, 100, &e);
        spawn_proj(rw, -100, 0, 3, &e);
        check(su_replay_writer_begin(rw, &opt, &wr, &e) == SU_OK, "record: %s", e.message);
        for (t = 0; t < 6u; t++) {
            check(su_replay_writer_step(wr, rw, 1u, &done, &e) == SU_OK && done == 1u, "record tick: %s", e.message);
        }
        check(su_host_digest(rw, before) == SU_OK, "check failed at line %d", __LINE__);
        check(su_replay_writer_finish(wr, rw, &bytes, &len) == SU_OK, "check failed at line %d", __LINE__);
        su_replay_writer_destroy(wr);
        check(bytes != NULL && len > 0u, "the recording has bytes");
        check(su_replay_open(bytes, len, &r, &e) == SU_OK, "open: %s", e.message);
        check(su_replay_info(r, &info) == SU_OK && info.end_tick == 6u, "six recorded ticks");
        su_host_world_destroy(rw);
        example_free(&xr);
        rw = example_world(&xp, &e);
        check(rw != NULL, "the replay world creates");
        if (rw != NULL) {
            for (i = 0; i < SHIP_ROWS; i++) {
                spawn_ship(rw, (int32_t)i * 4, 0, i == 3u ? 0u : 100u, &e);
            }
            spawn_proj(rw, -4, 2, 100, &e);
            spawn_proj(rw, -4, 3, 100, &e);
            spawn_proj(rw, -4, 100, 100, &e);
            spawn_proj(rw, -100, 0, 3, &e);
            check(su_replay_play(r, rw, 6u, &done, &e) == SU_OK && done == 6u, "play: %s", e.message);
            check(su_host_digest(rw, after) == SU_OK && memcmp(before, after, 32u) == 0,
                  "the replay reproduces the recorded state");
            su_host_world_destroy(rw);
        }
        example_free(&xp);
        su_replay_close(r);
        su_replay_free(bytes);
    }
    su_host_world_destroy(w);
    example_free(&x);
}

/* ---------- world-slot semantics: slot_u32, gen_u32, handle_live ---------- */

static const char k_slots_lir[] =
    "{\"ir_version\":\"0.4\",\"system_id\":\"game.slots\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"demo.slot\",\"instructions\":["
    "{\"op\":\"slot_u32\",\"dst\":\"s\"},"
    "{\"op\":\"store_u32\",\"field\":\"slotout\",\"src\":\"s\"},"
    "{\"op\":\"gen_u32\",\"dst\":\"g\"},"
    "{\"op\":\"store_u32\",\"field\":\"genout\",\"src\":\"g\"},"
    "{\"op\":\"load_u32\",\"dst\":\"hs\",\"field\":\"slot\"},"
    "{\"op\":\"load_u32\",\"dst\":\"hg\",\"field\":\"gen\"},"
    "{\"op\":\"handle_live\",\"dst\":\"lv\",\"slot\":\"hs\",\"gen\":\"hg\"},"
    "{\"op\":\"const_u32\",\"dst\":\"one\",\"value\":1},"
    "{\"op\":\"const_u32\",\"dst\":\"zero\",\"value\":0},"
    "{\"op\":\"select_u32\",\"dst\":\"lv32\",\"cond\":\"lv\",\"on_true\":\"one\",\"on_false\":\"zero\"},"
    "{\"op\":\"store_u32\",\"field\":\"live\",\"src\":\"lv32\"}]}";

static const char k_slots_capsule[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"game.slots\",\"source\":\"inline.lir.json\","
    "\"schema\":{\"id\":\"demo.slot\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"fixed_q32_v1\",\"ir_version\":\"0.4\",\"capabilities\":[],\"fields\":["
    "{\"id\":1,\"name\":\"slot\",\"type\":\"u32\",\"read\":true,\"write\":false},"
    "{\"id\":2,\"name\":\"gen\",\"type\":\"u32\",\"read\":true,\"write\":false},"
    "{\"id\":3,\"name\":\"slotout\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":4,\"name\":\"genout\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":5,\"name\":\"live\",\"type\":\"u32\",\"read\":false,\"write\":true}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},"
    "\"tests\":[\"tests/host/test_host_tables_entities.c\"]}";

/* Table 0 has 4 rows and no system; table 1 has 4 rows (base 4) and the 0.4 system. Row 1's `slot` names world
 * slot 1, which belongs to table 0: handle_live in table 1 must answer 0 for it. */
static const char k_slots_fixture[] =
    "{\"fixture_format\":\"0.1\",\"schema_id\":\"demo.slot\",\"schema_version\":1,\"tick\":0,"
    "\"entity_ids\":[1,2,3,4],\"columns\":{\"slot\":[4,1,4,10],\"gen\":[1,1,2,1],\"slotout\":[0,0,0,0],"
    "\"genout\":[0,0,0,0],\"live\":[0,0,0,0]}}";

static void test_world_slots(void) {
    SuHostSchemaField sf[1];
    SuHostSchema s0;
    SuHostSchemaField tf[5];
    SuHostSchema s1;
    static const SField zero_lane[] = {U(1, "unused")};
    static const SField slot_lanes[] = {U(1, "slot"), U(2, "gen"), U(3, "slotout"), U(4, "genout"), U(5, "live")};
    SuHostTableDesc td[2];
    SuHostWorldDesc d;
    SuHostCapsule cap;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t tables[1] = {1u};
    uint32_t got[4];
    uint64_t done = 0;
    uint32_t i;

    fill_schema(&s0, sf, zero_lane, 1u, "demo.zero");
    fill_schema(&s1, tf, slot_lanes, 5u, "demo.slot");
    memset(td, 0, sizeof td);
    td[0].struct_size = (uint32_t)sizeof td[0];
    td[0].schema = &s0;
    td[0].max_entities = 4u;
    td[1].struct_size = (uint32_t)sizeof td[1];
    td[1].schema = &s1;
    td[1].max_entities = 4u;
    td[1].fixture_json = (const uint8_t *)k_slots_fixture;
    td[1].fixture_len = sizeof k_slots_fixture - 1u;
    memset(&cap, 0, sizeof cap);
    cap.capsule_json = (const uint8_t *)k_slots_capsule;
    cap.capsule_len = sizeof k_slots_capsule - 1u;
    cap.lir_json = (const uint8_t *)k_slots_lir;
    cap.lir_len = sizeof k_slots_lir - 1u;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.tick_rate_hz = 120;
    d.table_count = 2;
    d.tables = td;
    d.capsule_table = tables;
    check(su_host_world_create(&d, &w, &e) == SU_OK, "slot world: %s", e.message);
    if (w == NULL) {
        return;
    }
    { uint32_t st = su_host_step(w, 1u, &done, &e); check(st == SU_OK && done == 1u, "slot tick: %s (%s)", su_host_status_name(st), e.message); }
    /* slot_u32 and gen_u32 name world slots and the row's generation; table 1's base is 4. */
    check(su_host_read_in(w, 1u, 3u, 0u, 4u, got) == SU_OK, "check failed at line %d", __LINE__);
    check(got[0] == 4u && got[1] == 5u && got[2] == 6u && got[3] == 7u,
          "slot_u32 is the world slot: %u %u %u %u", (unsigned)got[0], (unsigned)got[1], (unsigned)got[2],
          (unsigned)got[3]);
    check(su_host_read_in(w, 1u, 4u, 0u, 4u, got) == SU_OK, "check failed at line %d", __LINE__);
    for (i = 0; i < 4u; i++) {
        check(got[i] == 1u, "gen_u32 is the row's generation (%u)", (unsigned)got[i]);
    }
    /* handle_live: (4,1) is live; (1,1) is table 0's slot seen from table 1 (not this table's); (4,2) is a stale
     * generation; (10,1) is no slot of the world. */
    check(su_host_read_in(w, 1u, 5u, 0u, 4u, got) == SU_OK, "check failed at line %d", __LINE__);
    check(got[0] == 1u && got[1] == 0u && got[2] == 0u && got[3] == 0u,
          "handle_live takes world slots of its own table: %u %u %u %u", (unsigned)got[0], (unsigned)got[1],
          (unsigned)got[2], (unsigned)got[3]);
    su_host_world_destroy(w);
}

/* ---------- a registered operator with events and a registered stateful operator ---------- */

enum { PF_MARK = 1 };
static const SuHostOperatorField k_pulse_fields[] = {{PF_MARK, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "mark"}};
static uint64_t g_fail_tick = UINT64_MAX;
static uint32_t g_pulse_calls;

/* game.pulse: one event per live row, about that row's WORLD slot, and a mark; fails on g_fail_tick. */
static uint32_t pulse_run(const SuHostOperatorRun *r, void *user) {
    uint64_t i;
    (void)user;
    g_pulse_calls++;
    if (r->struct_size < SU_HOST_OPERATOR_RUN_TARGETS_SIZE) {
        return SU_E_INTERNAL;
    }
    if (r->tick == g_fail_tick) {
        snprintf(r->reason, r->reason_cap, "asked to fail");
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < r->rows; i++) {
        r->columns[0].out[i] = 1u;
        if (r->alive[i] && r->emit_event != NULL) {
            uint32_t slot = (uint32_t)(r->table_base + i);
            if (r->emit_event(r->emit_ctx, slot, 7u, slot, (uint32_t)r->tick) != SU_OK) {
                return SU_E_RESOURCE_LIMIT;
            }
        }
    }
    return SU_OK;
}

static uint32_t pulse_scratch(uint64_t capacity, size_t *bytes, void *user) {
    (void)capacity;
    (void)user;
    *bytes = 0u;
    return SU_OK;
}

static void pulse_desc(SuHostOperatorDesc *d) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(d);
    d->system_id = "game.pulse";
    d->schema_id = "demo.pool";
    d->operator_name = "pulse_ops_v1";
    d->operator_version = 1u;
    d->field_count = 1u;
    d->fields = k_pulse_fields;
    for (i = 0; i < 32u; i++) {
        d->constants_hash[i] = (uint8_t)(0x40u + i);
    }
    d->scratch_size = pulse_scratch;
    d->run = pulse_run;
    d->max_events_per_tick = 16u;
}

/* game.keep: SU_HOST_STATE_DROP_DEAD state keyed by world handles, its own rows plus the handles of operator
 * data (a "HKEY" blob of (slot, generation) pairs, which may be handles of another table). */
enum { KF_N = 2 };
static const SuHostOperatorField k_keep_fields[] = {{KF_N, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "n"}};

typedef struct KeepData {
    uint32_t count;
    uint32_t slot[8];
    uint32_t gen[8];
} KeepData;

static uint32_t keep_parse(const uint8_t *b, size_t len, void **parsed, char *why, size_t why_cap, void *user) {
    KeepData *k;
    uint32_t i;
    (void)user;
    if (len < 8u || memcmp(b, "HKEY", 4u) != 0 || (len - 8u) % 8u != 0u) {
        snprintf(why, why_cap, "operator data must be HKEY, a count and (slot, generation) pairs");
        return SU_E_PARSE;
    }
    k = calloc(1u, sizeof *k);
    if (k == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(&k->count, b + 4, 4u);
    if (k->count > 8u || 8u + (size_t)k->count * 8u != len) {
        uint32_t bad = k->count;
        free(k);
        snprintf(why, why_cap, "operator data names %u handles; 0..8 fit", (unsigned)bad);
        return SU_E_PARSE;
    }
    for (i = 0; i < k->count; i++) {
        memcpy(&k->slot[i], b + 8u + (size_t)i * 8u, 4u);
        memcpy(&k->gen[i], b + 12u + (size_t)i * 8u, 4u);
    }
    *parsed = k;
    return SU_OK;
}

static void keep_release(void *parsed, void *user) {
    (void)user;
    free(parsed);
}

static uint32_t keep_run(const SuHostOperatorRun *r, void *user) {
    const KeepData *k = (const KeepData *)r->data;
    uint64_t i;
    uint32_t v;
    (void)user;
    if (r->state == NULL) {
        return SU_E_INTERNAL;
    }
    for (i = 0; i < r->rows; i++) {
        uint64_t h;
        r->columns[0].out[i] = 1u;
        if (!r->alive[i]) {
            continue;
        }
        h = (uint64_t)(r->table_base + i) | ((uint64_t)r->generation[i] << 32);
        v = (uint32_t)i;
        if (su_host_state_put(r->state, h, 0u, &v, sizeof v) != SU_OK) {
            return SU_E_RESOURCE_LIMIT;
        }
    }
    for (i = 0; k != NULL && i < k->count; i++) {
        uint64_t h = (uint64_t)k->slot[i] | ((uint64_t)k->gen[i] << 32);
        v = 0xABCD0000u + (uint32_t)i;
        if (su_host_state_put(r->state, h, 1u, &v, sizeof v) != SU_OK) {
            return SU_E_STALE_HANDLE; /* the handle is not alive: the test sets only live ones */
        }
    }
    return SU_OK;
}

static void keep_desc(SuHostOperatorDesc *d) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(d);
    d->system_id = "game.keep";
    d->schema_id = "demo.pool";
    d->operator_name = "keep_ops_v1";
    d->operator_version = 1u;
    d->field_count = 1u;
    d->fields = k_keep_fields;
    for (i = 0; i < 32u; i++) {
        d->constants_hash[i] = (uint8_t)(0x80u + i);
    }
    d->scratch_size = pulse_scratch;
    d->run = keep_run;
    d->parse_data = keep_parse;
    d->release_data = keep_release;
    d->state_flags = SU_HOST_STATE_DROP_DEAD;
    d->state_max_entries = 64u;
    d->state_max_bytes = 256u;
}

/* ---------- a two-table world with one registered operator in table 1 ---------- */

static const SField k_actor_schema[] = {U(1, "position_x"), U(2, "velocity_x")};
static const SField k_pool_schema[] = {U(1, "mark"), U(2, "n")};
static const char k_actor_fixture[] =
    "{\"fixture_format\":\"0.1\",\"schema_id\":\"demo.actor\",\"schema_version\":1,\"tick\":0,"
    "\"entity_ids\":[1,2,3],\"columns\":{\"position_x\":[0,5,9],\"velocity_x\":[1,2,3]}}";

typedef struct OpWorld {
    Buf record;
    SuHostCapsule caps[2];
    SuHostTableDesc td[2];
    SuHostSchemaField af[2], pf[2];
    SuHostSchema as, ps;
    SuHostWorldDesc desc;
    uint32_t tables[2];
    uint32_t targets[2];
} OpWorld;

/* struct_size selects the descriptor version under test; target1 is the target table of the operator entry
 * (SU_HOST_TABLE_NONE for none), read only when struct_size covers capsule_target_table. */
static SuHostWorld *op_world(OpWorld *o, const SuHostOperatorDesc *op, const Bytes *actor_cap,
                             const Bytes *actor_lir, uint32_t actor_rows, uint32_t pool_rows, uint32_t struct_size,
                             uint32_t target1, SuHostError *err) {
    memset(o, 0, sizeof *o);
    o->record = host_record(op->system_id, op->schema_id, op->operator_name, op->operator_version, op->constants_hash,
                            op->fields, op->field_count, "simulate", "u32_mod_v1");
    fill_schema(&o->as, o->af, k_actor_schema, 2u, "demo.actor");
    fill_schema(&o->ps, o->pf, k_pool_schema, 2u, "demo.pool");
    memset(o->td, 0, sizeof o->td);
    o->td[0].struct_size = (uint32_t)sizeof o->td[0];
    o->td[0].schema = &o->as;
    o->td[0].max_entities = actor_rows;
    o->td[0].fixture_json = (const uint8_t *)k_actor_fixture;
    o->td[0].fixture_len = sizeof k_actor_fixture - 1u;
    o->td[1].struct_size = (uint32_t)sizeof o->td[1];
    o->td[1].schema = &o->ps;
    o->td[1].max_entities = pool_rows;
    o->caps[0].capsule_json = actor_cap->data;
    o->caps[0].capsule_len = actor_cap->len;
    o->caps[0].lir_json = actor_lir->data;
    o->caps[0].lir_len = actor_lir->len;
    o->caps[1].capsule_json = (const uint8_t *)o->record.p;
    o->caps[1].capsule_len = o->record.n;
    o->tables[0] = 0u;
    o->tables[1] = 1u;
    o->targets[0] = SU_HOST_TABLE_NONE;
    o->targets[1] = target1;
    memset(&o->desc, 0, sizeof o->desc);
    o->desc.struct_size = struct_size;
    o->desc.capsule_count = 2;
    o->desc.capsules = o->caps;
    o->desc.tick_rate_hz = 120;
    o->desc.max_events_per_tick = 32;
    o->desc.table_count = 2;
    o->desc.tables = o->td;
    o->desc.capsule_table = o->tables;
    o->desc.capsule_target_table = o->targets;
    {
        SuHostWorld *w = NULL;
        if (su_host_world_create(&o->desc, &w, err) != SU_OK) {
            return NULL;
        }
        return w;
    }
}

/* A world handle: slot | generation << 32. */
static uint64_t H(uint32_t slot, uint32_t gen) {
    return (uint64_t)slot | ((uint64_t)gen << 32);
}

/* Spawns n rows into table 1 (the pool). */
static void spawn_pool(SuHostWorld *w, uint32_t n, SuHostError *e) {
    SuHostValue v;
    uint32_t i;
    uint64_t h = 0;
    v.field_id = 1u; /* mark */
    v.value = 1u;
    for (i = 0; i < n; i++) {
        check(su_host_spawn_in(w, 1u, &v, 1u, 0u, &h, e) == SU_OK, "pool spawn: %s", e->message);
    }
}

/* Operator events in a multi-table world: a table-1 operator's records join the log with table 0's IR events in
 * schedule order, and carry world slots. */
static void test_operator_events(void) {
    SuHostOperatorDesc pd;
    OpWorld o;
    SuHostWorld *w = NULL;
    SuHostError e;
    Bytes ac, al;
    uint64_t done = 0;
    EvList ev;
    uint32_t i, n0 = 0, n1 = 0;

    pulse_desc(&pd);
    check(su_host_register_operator(&pd, &e) == SU_OK || e.code == SU_E_DUPLICATE_KEY, "register game.pulse: %s",
          e.message);
    ac = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    al = slurp("tests/fixtures/entities/despawn_move.lir.json");
    memset(&o, 0, sizeof o);
    w = op_world(&o, &pd, &ac, &al, 3u, 4u, (uint32_t)sizeof(SuHostWorldDesc), SU_HOST_TABLE_NONE, &e);
    check(w != NULL, "event world: %s", e.message);
    free(ac.data);
    free(al.data);
    if (w == NULL) {
        free(o.record.p);
        return;
    }
    spawn_pool(w, 4u, &e);
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "event tick: %s", e.message);
    collect_events(w, &ev);
    for (i = 0; i < ev.n; i++) {
        const SuHostEvent *v = &ev.ev[i];
        if (v->system_index == SU_HOST_EVENT_SYSTEM_HOST) {
            continue; /* the host's spawn records, outside the schedule */
        }
        if (v->system_index == 0u) {
            if (v->kind == 7u) {
                n0++;
                check(v->slot == 0u && v->a == 0u, "the table-0 IR event: slot %u a %u", (unsigned)v->slot,
                      (unsigned)v->a);
            } else {
                /* The structural command record of the despawn: the boundary appends one per command. */
                check(v->kind == SU_EVENT_DESPAWNED && v->slot == 0u, "the despawn record: kind %u slot %u",
                      (unsigned)v->kind, (unsigned)v->slot);
            }
        } else {
            n1++;
            check(v->system_index == 1u, "the operator is schedule position 1 (%u)", (unsigned)v->system_index);
            check(v->kind == 7u && v->a == v->slot && v->slot >= 3u && v->slot < 7u,
                  "the operator's event names a world slot of table 1 (%u)", (unsigned)v->slot);
        }
    }
    check(n0 == 1u, "one table-0 event (%u)", n0);
    check(n1 == 4u, "one event per live pool row (%u)", n1);
    for (i = 1; i < ev.n; i++) {
        if (ev.ev[i].system_index == SU_HOST_EVENT_SYSTEM_HOST || ev.ev[i - 1u].system_index == SU_HOST_EVENT_SYSTEM_HOST) {
            continue;
        }
        check(ev.ev[i].system_index >= ev.ev[i - 1u].system_index, "events are in schedule order");
    }
    check(g_pulse_calls >= 1u, "the operator ran");
    su_host_world_destroy(w);
    free(o.record.p);
}

/* A stateful registered operator in table 1 with SU_HOST_STATE_DROP_DEAD: entries keyed by another table's live
 * handle survive, entries whose handle died in either table are erased. */
static void test_stateful_operator_table1(void) {
    SuHostOperatorDesc kd;
    OpWorld o;
    SuHostWorld *w = NULL;
    SuHostError e;
    Bytes ac, al;
    uint64_t done = 0;
    const SuHostState *st = NULL;
    uint8_t data[8 + 16];
    uint32_t count = 0, slot0 = 1u, gen0 = 1u, slot1 = 2u, gen1 = 1u;
    uint64_t ship0 = 0, ship1 = 0;
    const void *p = NULL;
    uint32_t len = 0;

    keep_desc(&kd);
    check(su_host_register_operator(&kd, &e) == SU_OK || e.code == SU_E_DUPLICATE_KEY, "register game.keep: %s",
          e.message);
    ac = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    al = slurp("tests/fixtures/entities/despawn_move.lir.json");
    memset(&o, 0, sizeof o);
    w = op_world(&o, &kd, &ac, &al, 3u, 2u, (uint32_t)sizeof(SuHostWorldDesc), SU_HOST_TABLE_NONE, &e);
    check(w != NULL, "state world: %s", e.message);
    free(ac.data);
    free(al.data);
    if (w == NULL) {
        free(o.record.p);
        return;
    }
    spawn_pool(w, 2u, &e);
    memcpy(data, "HKEY", 4u);
    memcpy(data + 4, &count, 4u);
    /* The data names two handles of table 0 (slots 1 and 2, generation 1: the fixture's row 0 despawns itself on
     * tick 1, so its handle would be stale). */
    count = 2u;
    memcpy(data + 4, &count, 4u);
    memcpy(data + 8, &slot0, 4u);
    memcpy(data + 12, &gen0, 4u);
    memcpy(data + 16, &slot1, 4u);
    memcpy(data + 20, &gen1, 4u);
    check(su_host_set_operator_data(w, "game.keep", data, sizeof data, &e) == SU_OK, "operator data: %s", e.message);
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "state tick: %s", e.message);
    check(su_host_operator_state(w, 1u, &st) == SU_OK && st != NULL, "the operator has state");
    if (st == NULL) {
        su_host_world_destroy(w);
        free(o.record.p);
        return;
    }
    /* Two own entries (table 1's slots 3 and 4) and two of table 0's (slots 1 and 2), keyed by world handle. */
    check(su_host_state_count(st) == 4u, "four entries (%u)", (unsigned)su_host_state_count(st));
    check(su_host_state_get(st, H(1u, 1u), 1u, &p, &len) == SU_OK && p != NULL, "the table-0 handle 1 is kept");
    check(su_host_state_get(st, H(2u, 1u), 1u, &p, &len) == SU_OK && p != NULL, "the table-0 handle 2 is kept");
    check(su_host_state_get(st, H(3u, 1u), 0u, &p, &len) == SU_OK && p != NULL, "the operator's own row 0 is kept");
    /* Despawn table 0's slot 2: its entry is erased, the live table-0 handle 1 and table 1's entries stay. */
    check(su_host_entity_at_in(w, 0u, 2u, &ship1) == SU_OK, "table 0 slot 2 is alive");
    (void)ship0;
    check(su_host_despawn(w, ship1, &e) == SU_OK, "despawn: %s", e.message);
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "state tick 2: %s", e.message);
    check(su_host_state_get(st, H(2u, 1u), 1u, &p, &len) == SU_OK && p == NULL, "the dead table-0 handle is purged");
    check(su_host_state_get(st, H(1u, 1u), 1u, &p, &len) == SU_OK && p != NULL, "the live table-0 handle survives");
    check(su_host_state_get(st, H(3u, 1u), 0u, &p, &len) == SU_OK && p != NULL, "the operator's own row 0 survives");
    /* Despawn table 1's row 0 (world slot 3): its own entry is purged. */
    {
        uint64_t h = 0;
        check(su_host_entity_at_in(w, 1u, 0u, &h) == SU_OK, "table 1 slot 0 is alive");
        check(su_host_despawn(w, h, &e) == SU_OK, "check failed at line %d", __LINE__);
    }
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "state tick 3: %s", e.message);
    check(su_host_state_get(st, H(3u, 1u), 0u, &p, &len) == SU_OK && p == NULL, "the operator's dead row is purged");
    check(su_host_state_get(st, H(4u, 1u), 0u, &p, &len) == SU_OK && p != NULL, "its live row survives");
    su_host_world_destroy(w);
    free(o.record.p);
}

/* ---------- a target table with no system (the committed-read fallback) ---------- */

/* Ships in table 0 (no system at all) and projectiles in table 1 (the cross-table query). Table 0 is never begun or
 * published, so the operator's target set reads its committed columns; the tick must still succeed and the query
 * must find the ships. Run for the v1 sweep form and the v2 grid form. */
static void idle_target_case(const char *capsule_path, const char *label) {
    SuHostSchemaField ship_fields[N_SHIP], proj_fields[N_PROJ];
    SuHostSchema ship_s, proj_s;
    SuHostTableDesc td[2];
    SuHostWorldDesc d;
    SuHostCapsule cap;
    SuHostWorld *w = NULL;
    SuHostError e;
    Bytes q = slurp(capsule_path);
    uint32_t tables[1] = {1u};
    uint32_t targets[1] = {0u};
    uint64_t done = 0;
    uint32_t hits = 0, slot = 0xFFFFFFFFu, gen = 0;

    fill_schema(&ship_s, ship_fields, k_ship_schema, (uint32_t)N_SHIP, "game.ship");
    fill_schema(&proj_s, proj_fields, k_proj_schema, (uint32_t)N_PROJ, "space3d");
    memset(td, 0, sizeof td);
    td[0].struct_size = (uint32_t)sizeof td[0];
    td[0].schema = &ship_s;
    td[0].max_entities = SHIP_ROWS;
    td[1].struct_size = (uint32_t)sizeof td[1];
    td[1].schema = &proj_s;
    td[1].max_entities = PROJ_ROWS;
    memset(&cap, 0, sizeof cap);
    cap.capsule_json = q.data;
    cap.capsule_len = q.len;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.tick_rate_hz = 120;
    d.seed = 9;
    d.table_count = 2;
    d.tables = td;
    d.capsule_table = tables;
    d.capsule_target_table = targets;
    check(su_host_world_create(&d, &w, &e) == SU_OK, "%s: idle-target world: %s", label, e.message);
    free(q.data);
    if (w == NULL) {
        return;
    }
    (void)spawn_ship(w, 0, 0, 100u, &e);
    (void)spawn_proj(w, 0, 0, 100u, &e);
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "%s: the idle-target tick: %s", label, e.message);
    check(su_host_read_in(w, 1u, 40u, 0u, 1u, &hits) == SU_OK && hits > 0u,
          "%s: the cross-table query found the ship (%u contacts)", label, (unsigned)hits);
    check(su_host_read_in(w, 1u, 42u, 0u, 1u, &slot) == SU_OK && slot == 0u,
          "%s: the contact names the ship's world slot (%u)", label, (unsigned)slot);
    check(su_host_read_in(w, 1u, 43u, 0u, 1u, &gen) == SU_OK && gen == 1u,
          "%s: the contact carries the ship's generation (%u)", label, (unsigned)gen);
    /* The idle table keeps the world's tick without being stepped. */
    check(su_host_tick(w) == 1u, "%s: the world is at tick 1 (%llu)", label, (unsigned long long)su_host_tick(w));
#ifdef SU_TEST_WRAP_ALLOC
    /* A cross-table query allocates nothing: the instance scratch and the target views exist from creation. */
    g_allocs = 0u;
    g_count_alloc = 1;
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "%s: second tick: %s", label, e.message);
    g_count_alloc = 0;
    check(g_allocs == 0u, "%s: a cross-table tick allocates nothing (%lu allocations)", label, g_allocs);
#endif
    {
        uint64_t t0 = 0;
        check(su_host_table_capacity(w, 0u, &t0) == SU_OK && t0 == SHIP_ROWS, "check failed at line %d", __LINE__);
    }
    su_host_world_destroy(w);
}

static void test_idle_target_table(void) {
    idle_target_case("profiles/space3d/space3d_query.capsule.json", "query");
    idle_target_case("profiles/space3d/space3d_query2.capsule.json", "query2");
}

/* ---------- a registered operator with a target set (table 1 reads table 0) ---------- */

enum { RF_HIT = 1 };
static const SuHostOperatorField k_reach_fields[] = {{RF_HIT, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "mark"}};
static const SuHostOperatorField k_reach_target_fields[] = {
    {1u, SU_TYPE_U32, SU_ACCESS_READ, 0, "position_x"}};
static uint32_t g_reach_runs;
static uint64_t g_reach_rows, g_reach_target_rows, g_reach_own_base, g_reach_target_base;
static uint32_t g_reach_alive, g_reach_sum;

/* game.reach: reads table 0's position_x through the target set and writes the number of live targets into its own
 * table's hit lane. Fails the tick unless the target set has the shape the world promises. */
static uint32_t reach_run(const SuHostOperatorRun *r, void *user) {
    uint64_t i;
    uint32_t alive = 0, sum = 0;
    (void)user;
    g_reach_runs++;
    if (r->target_column_count != 1u || r->target_columns == NULL || r->target_alive == NULL ||
        r->target_generation == NULL || r->target_table != 0u) {
        return SU_E_INTERNAL;
    }
    for (i = 0; i < r->target_rows; i++) {
        alive += r->target_alive[i] != 0u;
        sum += r->target_columns[0].in[i];
    }
    g_reach_rows = r->rows;
    g_reach_target_rows = r->target_rows;
    g_reach_own_base = r->table_base;
    g_reach_target_base = r->target_base;
    g_reach_alive = alive;
    g_reach_sum = sum;
    for (i = 0; i < r->rows; i++) {
        r->columns[0].out[i] = alive;
    }
    return SU_OK;
}

static void reach_desc(SuHostOperatorDesc *d) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(d);
    d->system_id = "game.reach";
    d->schema_id = "demo.pool";
    d->operator_name = "reach_ops_v1";
    d->operator_version = 1u;
    d->field_count = 1u;
    d->fields = k_reach_fields;
    for (i = 0; i < 32u; i++) {
        d->constants_hash[i] = (uint8_t)(0xC0u + i);
    }
    d->scratch_size = pulse_scratch;
    d->run = reach_run;
    d->target_field_count = 1u;
    d->target_fields = k_reach_target_fields;
}

static void test_registered_target(void) {
    SuHostOperatorDesc rd;
    OpWorld o;
    SuHostWorld *w = NULL;
    SuHostError e;
    Bytes ac, al;
    uint64_t done = 0;
    uint32_t hit[2] = {0, 0};
    uint32_t i;

    reach_desc(&rd);
    check(su_host_register_operator(&rd, &e) == SU_OK || e.code == SU_E_DUPLICATE_KEY, "register game.reach: %s",
          e.message);
    ac = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    al = slurp("tests/fixtures/entities/despawn_move.lir.json");
    memset(&o, 0, sizeof o);
    w = op_world(&o, &rd, &ac, &al, 3u, 2u, (uint32_t)sizeof(SuHostWorldDesc), 0u, &e);
    check(w != NULL, "registered-target world: %s", e.message);
    free(ac.data);
    free(al.data);
    if (w == NULL) {
        free(o.record.p);
        return;
    }
    spawn_pool(w, 2u, &e);
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "registered-target tick: %s", e.message);
    /* The run iterates its own table (2 rows) and sees the target table (3 rows) under its own base. */
    check(g_reach_runs == 1u, "the operator ran once (%u)", (unsigned)g_reach_runs);
    check(g_reach_rows == 2u, "run.rows is the operator's own table (%llu)", (unsigned long long)g_reach_rows);
    check(g_reach_target_rows == 3u, "target_rows is the target table's (%llu)",
          (unsigned long long)g_reach_target_rows);
    check(g_reach_own_base == 3u, "the operator's base is table 1's base (%llu)", (unsigned long long)g_reach_own_base);
    check(g_reach_target_base == 0u, "the target base is table 0's (%llu)",
          (unsigned long long)g_reach_target_base);
    check(g_reach_alive == 3u, "all three targets are alive at the tick start (%u)", (unsigned)g_reach_alive);
    /* The actor system ran first in the tick, so the target column holds its working values: 0+1, 5+2, 9+3. */
    check(g_reach_sum == 1u + 7u + 12u, "the target column is the target table's working values (sum %u)",
          (unsigned)g_reach_sum);
    check(su_host_read_in(w, 1u, RF_HIT, 0u, 2u, hit) == SU_OK, "check failed at line %d", __LINE__);
    for (i = 0; i < 2u; i++) {
        check(hit[i] == 3u, "row %u's hit lane counts the live targets (%u)", (unsigned)i, (unsigned)hit[i]);
    }
    su_host_world_destroy(w);
    free(o.record.p);
}

/* ---------- descriptor sizes: v4 (no target set) and v5 describe the same world ---------- */

static void test_descriptor_sizes(void) {
    SuHostOperatorDesc pd;
    OpWorld o;
    SuHostWorld *w4 = NULL;
    SuHostWorld *w5 = NULL;
    SuHostWorld *w4t = NULL;
    SuHostError e;
    Bytes ac, al;
    uint8_t i4[32] = {0}, i5[32] = {0}, i4t[32] = {0};
    uint64_t done = 0;

    pulse_desc(&pd);
    check(su_host_register_operator(&pd, &e) == SU_OK || e.code == SU_E_DUPLICATE_KEY, "register game.pulse");
    ac = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    al = slurp("tests/fixtures/entities/despawn_move.lir.json");
    memset(&o, 0, sizeof o);
    w4 = op_world(&o, &pd, &ac, &al, 3u, 2u, SU_HOST_WORLD_DESC_V4_SIZE, SU_HOST_TABLE_NONE, &e);
    check(w4 != NULL, "v4 world: %s", e.message);
    if (w4 != NULL) {
        check(su_host_world_identity(w4, i4) == SU_OK, "check failed at line %d", __LINE__);
    }
    su_host_world_destroy(w4);
    free(o.record.p);
    memset(&o, 0, sizeof o);
    w5 = op_world(&o, &pd, &ac, &al, 3u, 2u, (uint32_t)sizeof(SuHostWorldDesc), SU_HOST_TABLE_NONE, &e);
    check(w5 != NULL, "v5 world: %s", e.message);
    if (w5 != NULL) {
        check(su_host_world_identity(w5, i5) == SU_OK, "check failed at line %d", __LINE__);
    }
    su_host_world_destroy(w5);
    free(o.record.p);
    /* A v4 descriptor's bytes end before capsule_target_table, so a pointer there is not read: the world is the
     * same as one that names no target. */
    memset(&o, 0, sizeof o);
    w4t = op_world(&o, &pd, &ac, &al, 3u, 2u, SU_HOST_WORLD_DESC_V4_SIZE, 0u, &e);
    check(w4t != NULL, "v4 world with a target pointer: %s", e.message);
    if (w4t != NULL) {
        check(su_host_world_identity(w4t, i4t) == SU_OK, "check failed at line %d", __LINE__);
        check(su_host_step(w4t, 1u, &done, &e) == SU_OK && done == 1u, "the v4 world steps: %s", e.message);
    }
    if (w4 != NULL && w5 != NULL && w4t != NULL) {
        check(memcmp(i4, i5, 32u) == 0, "v4 and v5 descriptors of the same world share an identity");
        check(memcmp(i4, i4t, 32u) == 0, "a v4 descriptor does not read capsule_target_table");
    }
    su_host_world_destroy(w4t);
    free(o.record.p);
    free(ac.data);
    free(al.data);
}

/* ---------- refusals ---------- */

static void test_refusals(void) {
    Example x;
    SuHostError e;
    SuHostWorld *w = NULL;

    /* A single-table world takes no target table. */
    {
        SuHostCapsule cap;
        SuHostWorldDesc d;
        uint32_t tgt = 0u;
        Bytes q = slurp("profiles/space3d/space3d_query.capsule.json");
        memset(&cap, 0, sizeof cap);
        cap.capsule_json = q.data;
        cap.capsule_len = q.len;
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.capsule_count = 1;
        d.capsules = &cap;
        d.tick_rate_hz = 120;
        d.max_entities = 4u;
        d.capsule_target_table = &tgt;
        check(su_host_world_create(&d, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL,
              "a target table needs a world with tables (%s)", e.message);
        free(q.data);
    }
    /* The target table must differ from the capsule's own table, and must exist. */
    {
        w = example_world(&x, &e);
        check(w != NULL, "the example world creates");
        su_host_world_destroy(w);
        w = NULL;
        x.targets[3] = 1u;
        check(su_host_world_create(&x.desc, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL,
              "the target table must differ from the capsule's table (%s)", e.message);
        x.targets[3] = 7u;
        check(su_host_world_create(&x.desc, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL,
              "the target table must exist (%s)", e.message);
        /* A capsule that takes no target set cannot name one. */
        x.targets[3] = SU_HOST_TABLE_NONE;
        x.targets[0] = 1u;
        check(su_host_world_create(&x.desc, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL,
              "an IR system takes no target set (%s)", e.message);
        x.targets[0] = SU_HOST_TABLE_NONE;
        /* The target table's declared schema must carry every target lane. */
        x.ship_fields[4].name = "pos_z_x";
        x.targets[3] = 0u;
        check(su_host_world_create(&x.desc, &w, &e) == SU_E_SCHEMA_MISMATCH && w == NULL,
              "a missing target lane is a schema mismatch (%s)", e.message);
        check(strstr(e.message, "target lane") != NULL, "the message names the target lane: %s", e.message);
        example_free(&x);
    }
}

/* ---------- no allocation in a tick (entity tick, two tables, and a failing system) ---------- */

static void test_no_allocation_in_tick(void) {
#ifdef SU_TEST_WRAP_ALLOC
    SuHostOperatorDesc pd;
    OpWorld o;
    SuHostWorld *w = NULL;
    SuHostError e;
    Bytes ac, al;
    uint64_t done = 0;

    pulse_desc(&pd);
    check(su_host_register_operator(&pd, &e) == SU_OK || e.code == SU_E_DUPLICATE_KEY, "register game.pulse");
    ac = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    al = slurp("tests/fixtures/entities/despawn_move.lir.json");
    memset(&o, 0, sizeof o);
    w = op_world(&o, &pd, &ac, &al, 3u, 4u, (uint32_t)sizeof(SuHostWorldDesc), SU_HOST_TABLE_NONE, &e);
    check(w != NULL, "alloc world: %s", e.message);
    free(ac.data);
    free(al.data);
    if (w == NULL) {
        free(o.record.p);
        return;
    }
    /* The first tick builds the entity runtime's caches (allocates), as documented. */
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "warm-up tick: %s", e.message);
    g_allocs = 0u;
    g_count_alloc = 1;
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "check failed at line %d", __LINE__);
    g_count_alloc = 0;
    check(g_allocs == 0u, "a two-table entity tick allocates nothing (%lu allocations)", g_allocs);
    /* A tick whose operator run fails allocates nothing either. */
    g_fail_tick = su_host_tick(w);
    g_allocs = 0u;
    g_count_alloc = 1;
    check(su_host_step(w, 1u, &done, &e) == SU_E_INVALID_ARGUMENT && done == 0u,
          "the failing system fails its tick (%s)", e.message);
    g_count_alloc = 0;
    g_fail_tick = UINT64_MAX;
    check(g_allocs == 0u, "a failing tick allocates nothing (%lu allocations)", g_allocs);
    check(strstr(e.message, "asked to fail") != NULL, "the operator's reason is reported: %s", e.message);
    su_host_world_destroy(w);
    free(o.record.p);
#endif
}

int main(void) {
    test_end_to_end();
    test_world_slots();
    test_idle_target_table();
    test_operator_events();
    test_registered_target();
    test_stateful_operator_table1();
    test_descriptor_sizes();
    test_refusals();
    test_no_allocation_in_tick();
    printf("test_host_tables_entities: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
