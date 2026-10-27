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
    U(201, "active"),   Q(202, "vel_x"),    U(208, "fire"),     Q(210, "fire_x"),   Q(212, "fire_v"),
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

/* game.proj_fire (schema space3d): a pool row the host asked to fire (fire == 1, set through an input record)
 * becomes a live projectile: kind 1 (a mover), active, ttl 30, and the position and velocity of the request. Rows
 * that are not asked keep their entry values (the row filter copies every writable lane). */
static const IrField k_proj_fire_fields[] = {
    {1, "kind", "u32", 1, 1},
    {10, "pos_x", "q32", 1, 1},
    {16, "prev_x", "q32", 1, 1},
    {40, "hit_count", "u32", 1, 1},
    {200, "ttl", "u32", 1, 1},
    {201, "active", "u32", 1, 1},
    {202, "vel_x", "q32", 1, 1},
    {208, "fire", "u32", 1, 1},
    {210, "fire_x", "q32", 1, 0},
    {212, "fire_v", "q32", 1, 0},
};
static const char k_proj_fire_lir[] =
    "{\"ir_version\":\"0.4\",\"system_id\":\"game.proj_fire\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"space3d\",\"row_filter\":{\"field\":\"fire\",\"equals\":1},\"instructions\":["
    "{\"op\":\"load_q32\",\"dst\":\"x\",\"field\":\"fire_x\"},"
    "{\"op\":\"store_q32\",\"field\":\"pos_x\",\"src\":\"x\"},"
    "{\"op\":\"store_q32\",\"field\":\"prev_x\",\"src\":\"x\"},"
    "{\"op\":\"load_q32\",\"dst\":\"v\",\"field\":\"fire_v\"},"
    "{\"op\":\"store_q32\",\"field\":\"vel_x\",\"src\":\"v\"},"
    "{\"op\":\"const_u32\",\"dst\":\"one\",\"value\":1},"
    "{\"op\":\"store_u32\",\"field\":\"active\",\"src\":\"one\"},"
    "{\"op\":\"store_u32\",\"field\":\"kind\",\"src\":\"one\"},"
    "{\"op\":\"const_u32\",\"dst\":\"life\",\"value\":30},"
    "{\"op\":\"store_u32\",\"field\":\"ttl\",\"src\":\"life\"},"
    "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},"
    "{\"op\":\"store_u32\",\"field\":\"hit_count\",\"src\":\"z\"},"
    "{\"op\":\"store_u32\",\"field\":\"fire\",\"src\":\"z\"}]}";

/* game.proj_move (schema space3d): an active projectile's prev = pos; pos_x += vel_x. */
static const IrField k_proj_move_fields[] = {
    {10, "pos_x", "q32", 1, 1},
    {16, "prev_x", "q32", 1, 1},
    {201, "active", "u32", 1, 0},
    {202, "vel_x", "q32", 1, 0},
};
static const char k_proj_move_lir[] =
    "{\"ir_version\":\"0.4\",\"system_id\":\"game.proj_move\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"space3d\",\"row_filter\":{\"field\":\"active\",\"equals\":1},\"instructions\":["
    "{\"op\":\"load_q32\",\"dst\":\"p\",\"field\":\"pos_x\"},"
    "{\"op\":\"store_q32\",\"field\":\"prev_x\",\"src\":\"p\"},"
    "{\"op\":\"load_q32\",\"dst\":\"v\",\"field\":\"vel_x\"},"
    "{\"op\":\"add_q32\",\"dst\":\"n\",\"a\":\"p\",\"b\":\"v\"},"
    "{\"op\":\"store_q32\",\"field\":\"pos_x\",\"src\":\"n\"}]}";

/* game.proj_hit (schema space3d): an active projectile that space3d reported a contact for emits one event about
 * the row it hit (the contact slot, a world slot of the target table) and returns to the pool: active and kind 0,
 * ttl 0, so the next fire request can reuse the row. */
static const IrField k_proj_hit_fields[] = {
    {1, "kind", "u32", 1, 1},
    {40, "hit_count", "u32", 1, 0},
    {42, "c0_slot", "u32", 1, 0},
    {43, "c0_gen", "u32", 1, 0},
    {200, "ttl", "u32", 1, 1},
    {201, "active", "u32", 1, 1},
};
static const char k_proj_hit_lir[] =
    "{\"ir_version\":\"0.4\",\"system_id\":\"game.proj_hit\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"space3d\",\"row_filter\":{\"field\":\"active\",\"equals\":1},\"instructions\":["
    "{\"op\":\"load_u32\",\"dst\":\"n\",\"field\":\"hit_count\"},"
    "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},"
    "{\"op\":\"eq_u32\",\"dst\":\"none\",\"a\":\"n\",\"b\":\"z\"},"
    "{\"op\":\"not_bool\",\"dst\":\"hit\",\"a\":\"none\"},"
    "{\"op\":\"load_u32\",\"dst\":\"s\",\"field\":\"c0_slot\"},"
    "{\"op\":\"load_u32\",\"dst\":\"g\",\"field\":\"c0_gen\"},"
    "{\"op\":\"emit_event\",\"cond\":\"hit\",\"kind\":10,\"a\":\"s\",\"b\":\"g\"},"
    "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"active\"},"
    "{\"op\":\"select_u32\",\"dst\":\"na\",\"cond\":\"hit\",\"on_true\":\"z\",\"on_false\":\"a\"},"
    "{\"op\":\"store_u32\",\"field\":\"active\",\"src\":\"na\"},"
    "{\"op\":\"load_u32\",\"dst\":\"k\",\"field\":\"kind\"},"
    "{\"op\":\"select_u32\",\"dst\":\"nk\",\"cond\":\"hit\",\"on_true\":\"z\",\"on_false\":\"k\"},"
    "{\"op\":\"store_u32\",\"field\":\"kind\",\"src\":\"nk\"},"
    "{\"op\":\"load_u32\",\"dst\":\"t\",\"field\":\"ttl\"},"
    "{\"op\":\"select_u32\",\"dst\":\"nt\",\"cond\":\"hit\",\"on_true\":\"z\",\"on_false\":\"t\"},"
    "{\"op\":\"store_u32\",\"field\":\"ttl\",\"src\":\"nt\"}]}";

/* game.proj_expire (schema space3d): an active projectile's ttl -= 1; at 0 it emits one event about its own row
 * and returns to the pool. */
static const IrField k_proj_expire_fields[] = {
    {1, "kind", "u32", 1, 1},
    {200, "ttl", "u32", 1, 1},
    {201, "active", "u32", 1, 1},
};
static const char k_proj_expire_lir[] =
    "{\"ir_version\":\"0.4\",\"system_id\":\"game.proj_expire\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"space3d\",\"row_filter\":{\"field\":\"active\",\"equals\":1},\"instructions\":["
    "{\"op\":\"load_u32\",\"dst\":\"t\",\"field\":\"ttl\"},"
    "{\"op\":\"const_u32\",\"dst\":\"one\",\"value\":1},"
    "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},"
    "{\"op\":\"sub_u32\",\"dst\":\"n\",\"a\":\"t\",\"b\":\"one\"},"
    "{\"op\":\"store_u32\",\"field\":\"ttl\",\"src\":\"n\"},"
    "{\"op\":\"eq_u32\",\"dst\":\"out\",\"a\":\"n\",\"b\":\"z\"},"
    "{\"op\":\"slot_u32\",\"dst\":\"s\"},"
    "{\"op\":\"emit_event\",\"cond\":\"out\",\"kind\":9,\"a\":\"s\",\"b\":\"n\"},"
    "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"active\"},"
    "{\"op\":\"select_u32\",\"dst\":\"na\",\"cond\":\"out\",\"on_true\":\"z\",\"on_false\":\"a\"},"
    "{\"op\":\"store_u32\",\"field\":\"active\",\"src\":\"na\"},"
    "{\"op\":\"load_u32\",\"dst\":\"k\",\"field\":\"kind\"},"
    "{\"op\":\"select_u32\",\"dst\":\"nk\",\"cond\":\"out\",\"on_true\":\"z\",\"on_false\":\"k\"},"
    "{\"op\":\"store_u32\",\"field\":\"kind\",\"src\":\"nk\"}]}";

/* game.proj_life (schema space3d): the same expire rule for a projectile that is not pooled: ttl -= 1 and at 0 it
 * emits one event about its own row and despawns itself (the B1 ordering test's system). */
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
#define PROJ_ROWS 24u /* the fixed projectile pool: one table-1 row per pool slot */
#define SHIP_BASE 0u
#define PROJ_BASE SHIP_ROWS
#define NSYS 7u
#define TICKS 60u        /* the end-to-end run */
#define RESTORE_TICK 30u /* the snapshot the restore tests replay from */
#define SHIP_HP 50u
#define HIT_DAMAGE 10u
#define EV_PER_TICK 128u /* a tick's event list in a trace; the tests assert it is never truncated */

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
    static const char *const ids[NSYS] = {"game.ship_move", "game.ship_die", "game.proj_fire", "game.proj_move",
                                          "space3d.query", "game.proj_hit", "game.proj_expire"};
    static const char *const lirs[NSYS] = {k_ship_move_lir,   k_ship_die_lir,   k_proj_fire_lir, k_proj_move_lir,
                                           NULL,              k_proj_hit_lir,   k_proj_expire_lir};
    static const IrField *const flds[NSYS] = {k_ship_move_fields, k_ship_die_fields, k_proj_fire_fields,
                                              k_proj_move_fields, NULL, k_proj_hit_fields, k_proj_expire_fields};
    static const uint32_t nf[NSYS] = {
        (uint32_t)(sizeof k_ship_move_fields / sizeof k_ship_move_fields[0]),
        (uint32_t)(sizeof k_ship_die_fields / sizeof k_ship_die_fields[0]),
        (uint32_t)(sizeof k_proj_fire_fields / sizeof k_proj_fire_fields[0]),
        (uint32_t)(sizeof k_proj_move_fields / sizeof k_proj_move_fields[0]), 0u,
        (uint32_t)(sizeof k_proj_hit_fields / sizeof k_proj_hit_fields[0]),
        (uint32_t)(sizeof k_proj_expire_fields / sizeof k_proj_expire_fields[0])};
    static const char *const after[NSYS] = {"",
                                            "",
                                            "",
                                            "\"game.proj_fire\"",
                                            "\"game.proj_fire\",\"game.proj_move\"",
                                            "\"game.proj_fire\",\"game.proj_move\",\"space3d.query\"",
                                            "\"game.proj_fire\",\"game.proj_move\",\"game.proj_hit\""};
    static const char *const vers[NSYS] = {"0.3", "0.3", "0.4", "0.4", "0.3", "0.4", "0.4"};

    memset(x, 0, sizeof *x);
    x->query = slurp("profiles/space3d/space3d_query.capsule.json");
    for (i = 0; i < NSYS; i++) {
        if (i == 4u) {
            x->caps[i].capsule_json = x->query.data; /* the space3d.query record, unmodified */
            x->caps[i].capsule_len = x->query.len;
            continue;
        }
        {
            /* ship_die, proj_hit and proj_expire emit events; ship_die also despawns itself. */
            int entity_ops = i == 1u || i == 5u || i == 6u;
            x->cap[i] = ir_capsule(ids[i], i < 2u ? "game.ship" : "space3d", "simulate", after[i], flds[i], nf[i],
                                   vers[i], "fixed_q32_v1",
                                   entity_ops ? "\"despawn_self\",\"emit_event\"" : "", entity_ops ? 1u : 0u,
                                   entity_ops ? 1u : 0u);
        }
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
    x->targets[4] = 0u; /* space3d.query reads table 0 (the ships) as its target set */
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

/* Spawns one live projectile: kind 1 (a mover), active, at x moving by vx, radius 1/8, ttl. */
static uint64_t spawn_proj(SuHostWorld *w, int32_t x, int32_t vx, uint32_t ttl, SuHostError *err) {
    SuHostValue v[14];
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
    v[n].field_id = 201u; /* active */
    v[n++].value = 1u;
    if (su_host_spawn_in(w, 1u, v, n, 0u, &h, err) != SU_OK) {
        check(0, "projectile spawn: %s", err->message);
    }
    return h;
}

/* Spawns one pool row: an inactive projectile (kind 0, active 0, ttl 0, no fire request) at the origin. */
static uint64_t spawn_pool_row(SuHostWorld *w, SuHostError *err) {
    SuHostValue v[12];
    uint64_t h = 0;
    uint32_t n = 0;
    v[n].field_id = 1u; /* kind: not a collidable while the row is idle */
    v[n++].value = 0u;
    n = put_q32(v, n, 10u, 0);              /* pos_x */
    n = put_q32(v, n, 16u, 0);              /* prev_x */
    n = put_q32(v, n, 30u, q32(1) / 8);     /* radius */
    n = put_q32(v, n, 202u, 0);             /* vel_x */
    v[n].field_id = 200u; /* ttl */
    v[n++].value = 0u;
    v[n].field_id = 201u; /* active */
    v[n++].value = 0u;
    v[n].field_id = 208u; /* fire */
    v[n++].value = 0u;
    if (su_host_spawn_in(w, 1u, v, n, 0u, &h, err) != SU_OK) {
        check(0, "pool spawn: %s", err->message);
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

/* A tick's digest and its complete event list. */
typedef struct TickTrace {
    uint8_t digest[32];
    uint32_t n;
    SuHostEvent ev[EV_PER_TICK];
} TickTrace;

/* Pushes this tick's fire request: every other tick, the lowest free pool row (active 0 and no pending fire) becomes
 * a live projectile on the next tick. The choice is a function of the committed state only, so a restored world makes
 * the same ones. fired[] counts the requests per pool row. */
static void fire_one(SuHostWorld *w, uint64_t t, const uint64_t *pool, uint32_t *fired, SuHostError *e) {
    uint32_t active[PROJ_ROWS], fire[PROJ_ROWS];
    uint32_t r;
    if ((t % 2u) != 0u) {
        return;
    }
    if (su_host_read_in(w, 1u, 201u, 0u, PROJ_ROWS, active) != SU_OK ||
        su_host_read_in(w, 1u, 208u, 0u, PROJ_ROWS, fire) != SU_OK) {
        check(0, "reading the pool: %s", e->message);
        return;
    }
    for (r = 0; r < PROJ_ROWS; r++) {
        if (active[r] == 0u && fire[r] == 0u) {
            break;
        }
    }
    if (r == PROJ_ROWS) {
        return; /* the pool is full this tick */
    }
    {
        SuHostInput req;
        int64_t vx = q32((int64_t)(2u + (t % 3u)));
        /* Every fourth shot starts beyond the ships, so it flies away and expires instead of hitting. */
        int64_t fx = ((t % 4u) == 0u) ? q32(100) : q32(-4);
        req.entity = pool[r];
        req.field_id = 208u; /* fire */
        req.value = 1u;
        check(su_host_input_push(w, t, &req, 1u, e) == SU_OK, "fire request: %s", e->message);
        check(su_host_input_push_i64(w, t, pool[r], 210u, fx, e) == SU_OK, "fire position: %s", e->message);
        check(su_host_input_push_i64(w, t, pool[r], 212u, vx, e) == SU_OK, "fire velocity: %s", e->message);
        if (fired != NULL) {
            fired[r]++;
        }
    }
}

/* The ship damage the next tick applies, computed from this tick's hit events: a ship loses HIT_DAMAGE per hit and
 * despawns itself in the next tick's ship_die when its hp reaches 0. Ships that are already gone are skipped, and one
 * ship takes at most one record per tick. A queued input record is not world state (host.h, "snapshots"), so the
 * list is the test's own pending record: it is pushed at the start of the tick it belongs to, and re-pushed after a
 * restore, exactly as the rollback protocol in host.h describes. */
typedef struct Pending {
    uint32_t n;
    SuHostInput wr[SHIP_ROWS];
} Pending;

static void damage_from_hits(SuHostWorld *w, const TickTrace *tr, Pending *pend, SuHostError *e) {
    uint32_t hp[SHIP_ROWS], hits[SHIP_ROWS];
    uint32_t i;
    pend->n = 0u;
    if (su_host_read_in(w, 0u, 210u, 0u, SHIP_ROWS, hp) != SU_OK) {
        check(0, "reading ship hp: %s", e->message);
        return;
    }
    memset(hits, 0, sizeof hits);
    for (i = 0; i < tr->n; i++) {
        if (tr->ev[i].kind == 10u && tr->ev[i].a < SHIP_ROWS) {
            hits[tr->ev[i].a]++;
        }
    }
    for (i = 0; i < SHIP_ROWS; i++) {
        uint64_t h = 0;
        uint32_t dmg = hits[i] * HIT_DAMAGE;
        if (dmg == 0u || su_host_entity_at_in(w, 0u, i, &h) != SU_OK) {
            continue;
        }
        if (dmg > hp[i]) {
            dmg = hp[i];
        }
        hp[i] -= dmg;
        pend->wr[pend->n].entity = h;
        pend->wr[pend->n].field_id = 210u; /* hp */
        pend->wr[pend->n].value = hp[i];
        pend->n++;
    }
}

/* Pushes the pending damage record for tick (empty when the previous tick hit nothing). */
static void push_pending(SuHostWorld *w, uint64_t tick, const Pending *pend, SuHostError *e) {
    if (pend->n > 0u) {
        check(su_host_input_push(w, tick, pend->wr, pend->n, e) == SU_OK, "damage record of tick %llu: %s",
              (unsigned long long)tick, e->message);
    }
}

/* One tick of the example world: fire, push the damage the previous tick computed, step (through the replay writer
 * when one is attached), read the tick's events, compute the next tick's damage, and record the digest and the
 * events. */
static void run_one(SuHostWorld *w, uint64_t t, const uint64_t *pool, uint32_t *fired, TickTrace *tr,
                    SuReplayWriter *rw, Pending *pend, SuHostError *e) {
    uint64_t done = 0;
    uint32_t n = 0, total = 0, complete = 0;
    uint32_t st;
    fire_one(w, t, pool, fired, e);
    push_pending(w, t, pend, e);
    st = rw != NULL ? su_replay_writer_step(rw, w, 1u, &done, e) : su_host_step(w, 1u, &done, e);
    check(st == SU_OK && done == 1u, "tick %llu: %s", (unsigned long long)t, e->message);
    memset(tr->ev, 0, sizeof tr->ev);
    check(su_host_events_for_tick(w, t, tr->ev, EV_PER_TICK, &n, &total, &complete) == SU_OK,
          "events of tick %llu: %s", (unsigned long long)t, e->message);
    check(complete == 1u && n == total && n < EV_PER_TICK, "tick %llu has a complete event list (%u of %u)",
          (unsigned long long)t, (unsigned)n, (unsigned)total);
    tr->n = n;
    check(su_host_digest(w, tr->digest) == SU_OK, "digest of tick %llu", (unsigned long long)t);
    damage_from_hits(w, tr, pend, e);
}

/* Two runs of the same world from RESTORE_TICK to TICKS agree tick by tick: state digest and event list. */
static void compare_traces(const TickTrace *a, const TickTrace *b, const char *label) {
    uint64_t t;
    uint32_t i;
    for (t = RESTORE_TICK; t < TICKS; t++) {
        check(memcmp(a[t].digest, b[t].digest, 32u) == 0, "%s: tick %llu has the same digest", label,
              (unsigned long long)t);
        check(a[t].n == b[t].n, "%s: tick %llu has %u events, the other run %u", label, (unsigned long long)t,
              (unsigned)a[t].n, (unsigned)b[t].n);
        for (i = 0; i < a[t].n && i < b[t].n; i++) {
            check(memcmp(&a[t].ev[i], &b[t].ev[i], sizeof(SuHostEvent)) == 0, "%s: tick %llu event %u matches",
                  label, (unsigned long long)t, (unsigned)i);
        }
    }
}

/* Spawns the world's fixed rows: 8 ships (one at 0 hp, which despawns on the first tick) and the 24 pool rows. */
static void spawn_example(SuHostWorld *w, uint64_t *ships, uint64_t *pool, SuHostError *e) {
    uint32_t i;
    for (i = 0; i < SHIP_ROWS; i++) {
        ships[i] = spawn_ship(w, (int32_t)i * 4, 0, i == 3u ? 0u : SHIP_HP, e);
        check(SU_ENTITY_SLOT(ships[i]) == i, "ship %u is table 0's local slot %u", (unsigned)i, (unsigned)i);
    }
    for (i = 0; i < PROJ_ROWS; i++) {
        pool[i] = spawn_pool_row(w, e);
        check(SU_ENTITY_SLOT(pool[i]) == PROJ_BASE + i, "pool row %u is table 1's local slot %u (world slot %u)",
              (unsigned)i, (unsigned)i, (unsigned)(PROJ_BASE + i));
    }
}

/* The end-to-end world of the issue: two kinds, a fixed projectile pool with an active flag, IR fire/move/hit/
 * expire and ship death, cross-table queries, ship damage through input records, 60 ticks, a snapshot restored into
 * a fresh world and in place (both continued to the end and compared tick by tick), and a replay with a seek. */
static void test_end_to_end(void) {
    Example x, xf, xp;
    SuHostWorld *w = NULL, *fresh = NULL, *rw = NULL;
    SuHostError e;
    uint64_t ships[SHIP_ROWS], pool[PROJ_ROWS];
    TickTrace *run_tr = NULL, *fresh_tr = NULL, *inplace_tr = NULL, *rep_tr = NULL;
    uint32_t fired[PROJ_ROWS];
    Pending pend, pend30;
    uint8_t *snap = NULL;
    size_t snap_size = 0;
    uint64_t t, done;
    uint32_t i, n3 = 0, n9 = 0, n10 = 0, max_fired = 0;

    memset(&e, 0, sizeof e);
    memset(fired, 0, sizeof fired);
    memset(&pend, 0, sizeof pend);
    run_tr = calloc(TICKS + 1u, sizeof *run_tr);
    fresh_tr = calloc(TICKS + 1u, sizeof *fresh_tr);
    inplace_tr = calloc(TICKS + 1u, sizeof *inplace_tr);
    rep_tr = calloc(TICKS + 1u, sizeof *rep_tr);
    check(run_tr != NULL && fresh_tr != NULL && inplace_tr != NULL && rep_tr != NULL, "the traces allocate");
    if (run_tr == NULL || fresh_tr == NULL || inplace_tr == NULL || rep_tr == NULL) {
        goto out;
    }
    w = example_world(&x, &e);
    check(w != NULL, "the two-kind world creates: %s", e.message);
    if (w == NULL) {
        goto out;
    }
    check(su_host_table_count(w) == 2u, "two tables");
    spawn_example(w, ships, pool, &e);
    /* The pool is a fixed set of rows: 24 table-1 entities, none of them active. */
    for (i = 0; i < PROJ_ROWS; i++) {
        uint64_t h = 0;
        check(su_host_entity_at_in(w, 1u, i, &h) == SU_OK && h == pool[i], "pool row %u is a live entity", (unsigned)i);
    }
    /* 0..RESTORE_TICK, then a snapshot of the world at RESTORE_TICK. pend holds the damage tick 30 applies, which a
     * restored world has to be given again: a queued input record is not part of the snapshot. */
    for (t = 0; t < RESTORE_TICK; t++) {
        run_one(w, t, pool, fired, &run_tr[t], NULL, &pend, &e);
    }
    pend30 = pend;
    check(su_host_tick(w) == RESTORE_TICK, "the world is at tick %u (%llu)", (unsigned)RESTORE_TICK,
          (unsigned long long)su_host_tick(w));
    check(su_host_snapshot_size(w, &snap_size) == SU_OK && snap_size > 0u, "the snapshot has a size");
    snap = malloc(snap_size);
    check(snap != NULL && su_host_snapshot_write(w, snap, snap_size, &snap_size) == SU_OK, "the snapshot writes");
    if (snap != NULL) {
        /* A multi-table world writes snapshot version 3 (host.h, "tables"). */
        check(memcmp(snap, "SUHSNAP", 7u) == 0 && snap[7] == 0u, "the snapshot magic");
        check(snap[8] == 3u && snap[9] == 0u && snap[10] == 0u && snap[11] == 0u,
              "the multi-table snapshot is version 3");
    }
    /* The rest of the run, in the original world. */
    for (t = RESTORE_TICK; t < TICKS; t++) {
        run_one(w, t, pool, fired, &run_tr[t], NULL, &pend, &e);
    }
    /* The same ticks from a fresh world restored from the snapshot: same digests and event lists. */
    fresh = example_world(&xf, &e);
    check(fresh != NULL, "the restore world creates: %s", e.message);
    if (fresh != NULL && snap != NULL) {
        check(su_host_snapshot_restore(fresh, snap, snap_size, &e) == SU_OK, "restore into a fresh world: %s",
              e.message);
        check(su_host_tick(fresh) == RESTORE_TICK, "the restored world is at tick %u",
              (unsigned)RESTORE_TICK);
        pend = pend30; /* the record tick 30 applies is the test's, not the snapshot's */
        for (t = RESTORE_TICK; t < TICKS; t++) {
            run_one(fresh, t, pool, NULL, &fresh_tr[t], NULL, &pend, &e);
        }
        compare_traces(run_tr, fresh_tr, "fresh restore");
    }
    /* The same ticks from the snapshot restored in place, in the world that already ran to TICKS. */
    if (snap != NULL) {
        check(su_host_snapshot_restore_inplace(w, snap, snap_size, &e) == SU_OK, "in-place restore: %s", e.message);
        check(su_host_tick(w) == RESTORE_TICK, "the in-place restored world is at tick %u", (unsigned)RESTORE_TICK);
        /* Rollback protocol (host.h, "snapshots and digest"): drop the records queued for the ticks the restore puts
         * back in the future, then push the one tick 30 applies. */
        su_host_input_clear(w, RESTORE_TICK);
        pend = pend30;
        for (t = RESTORE_TICK; t < TICKS; t++) {
            run_one(w, t, pool, NULL, &inplace_tr[t], NULL, &pend, &e);
        }
        compare_traces(run_tr, inplace_tr, "in-place restore");
    }
    /* Content: the run exercised every path of the world. */
    for (t = 0; t < TICKS; t++) {
        for (i = 0; i < run_tr[t].n; i++) {
            const SuHostEvent *v = &run_tr[t].ev[i];
            if (v->kind == 3u) {
                n3++;
                check(v->slot == v->a && v->a < SHIP_ROWS, "the dead ship's event names its world slot (%u, %u)",
                      (unsigned)v->slot, (unsigned)v->a);
                check(v->system_index == 1u, "the death event comes from game.ship_die (%u)", (unsigned)v->system_index);
            } else if (v->kind == 9u) {
                n9++;
                check(v->a == v->slot && v->slot >= PROJ_BASE && v->slot < PROJ_BASE + PROJ_ROWS,
                      "an expiry names the projectile's world slot (%u, %u)", (unsigned)v->a, (unsigned)v->slot);
                check(v->system_index == 6u, "the expiry comes from game.proj_expire (%u)", (unsigned)v->system_index);
            } else if (v->kind == 10u) {
                n10++;
                check(v->a < SHIP_ROWS, "a hit names a ship's world slot (%u)", (unsigned)v->a);
                check(v->b == 1u, "the contact's generation is the ship's (%u)", (unsigned)v->b);
                check(v->slot >= PROJ_BASE && v->slot < PROJ_BASE + PROJ_ROWS,
                      "the hit's slot is the projectile's world slot (%u)", (unsigned)v->slot);
                check(v->system_index == 5u, "the hit comes from game.proj_hit (%u)", (unsigned)v->system_index);
            }
        }
    }
    for (i = 0; i < PROJ_ROWS; i++) {
        max_fired = fired[i] > max_fired ? fired[i] : max_fired;
    }
    check(n3 >= 1u, "at least one ship died through despawn_self (%u)", (unsigned)n3);
    check(n9 >= 1u, "at least one projectile expired (%u)", (unsigned)n9);
    check(n10 >= 1u, "the cross-table query reported hits (%u)", (unsigned)n10);
    check(max_fired >= 2u, "the pool is reused: one row was fired %u times", (unsigned)max_fired);
    /* Replay: record the same 60 ticks of a fresh world, then seek to tick 45 and play to the end. */
    {
        SuReplayOptions opt;
        SuReplayWriter *wr = NULL;
        SuReplay *r = NULL;
        SuReplayInfo info;
        uint8_t *bytes = NULL;
        size_t len = 0;
        uint8_t end_digest[32];

        rw = example_world(&xp, &e);
        check(rw != NULL, "the recording world creates: %s", e.message);
        if (rw != NULL) {
            spawn_example(rw, ships, pool, &e);
            memset(&opt, 0, sizeof opt);
            opt.struct_size = (uint32_t)sizeof opt;
            opt.keyframe_interval = 5u;
            opt.digest_interval = 1u;
            check(su_replay_writer_begin(rw, &opt, &wr, &e) == SU_OK, "record: %s", e.message);
            memset(&pend, 0, sizeof pend);
            for (t = 0; wr != NULL && t < TICKS; t++) {
                run_one(rw, t, pool, NULL, &rep_tr[t], wr, &pend, &e);
            }
            check(wr != NULL && su_replay_writer_finish(wr, rw, &bytes, &len) == SU_OK, "the recording finishes");
            if (wr != NULL) {
                su_replay_writer_destroy(wr);
            }
            /* The recording is the same run: every tick's digest and event list matches the original. */
            for (t = 0; t < TICKS; t++) {
                check(memcmp(rep_tr[t].digest, run_tr[t].digest, 32u) == 0,
                      "the recorded run matches at tick %llu", (unsigned long long)t);
            }
            check(bytes != NULL && len > 0u, "the recording has bytes");
            check(bytes != NULL && su_replay_open(bytes, len, &r, &e) == SU_OK, "open: %s", e.message);
            if (r != NULL) {
                check(su_replay_info(r, &info) == SU_OK && info.end_tick == TICKS && info.keyframe_count > 1u,
                      "the replay covers %u ticks with keyframes", (unsigned)TICKS);
                check(su_replay_seek(r, rw, 45u, &e) == SU_OK, "seek to tick 45: %s", e.message);
                check(su_host_tick(rw) == 45u, "the seeked world is at tick 45 (%llu)",
                      (unsigned long long)su_host_tick(rw));
                check(su_replay_play(r, rw, TICKS - 45u, &done, &e) == SU_OK && done == TICKS - 45u,
                      "play to the end: %s", e.message);
                check(su_host_digest(rw, end_digest) == SU_OK && memcmp(end_digest, run_tr[TICKS - 1u].digest, 32u) == 0,
                      "the replay's end state is the recorded one");
                su_replay_close(r);
            }
            su_replay_free(bytes);
            su_host_world_destroy(rw);
        }
        example_free(&xp);
    }
    su_host_world_destroy(fresh);
    example_free(&xf);
    su_host_world_destroy(w);
    example_free(&x);
out:
    free(snap);
    free(run_tr);
    free(fresh_tr);
    free(inplace_tr);
    free(rep_tr);
}

/* ---------- B1: log order inside a system in table 1 or higher ---------- */

/* A table-1 system whose two live rows each emit an event (seq 0) and despawn themselves (seq 1) in the same tick.
 * The tick's records must stay ordered by (system_index, world slot, seq): row 8's pair before row 9's. Adding the
 * table base before the merge instead sorted both events (local slots 0, 1) ahead of both despawn records (world
 * slots 8, 9), which is the bug this test pins. */
static void test_table1_event_order(void) {
    SuHostSchemaField zf[1], pf[N_PROJ];
    SuHostSchema zs, ps;
    SuHostTableDesc td[2];
    SuHostWorldDesc d;
    SuHostCapsule cap;
    SuHostWorld *w = NULL;
    SuHostError e;
    Buf c = ir_capsule("game.proj_life", "space3d", "simulate", "", k_proj_life_fields,
                       (uint32_t)(sizeof k_proj_life_fields / sizeof k_proj_life_fields[0]), "0.3", "fixed_q32_v1",
                       "\"despawn_self\",\"emit_event\"", 1u, 1u);
    uint32_t tables[1] = {1u};
    uint64_t done = 0;
    EvList ev;
    uint32_t i, n = 0;
    static const uint32_t want_slot[4] = {8u, 8u, 9u, 9u};
    static const uint32_t want_seq[4] = {0u, 1u, 0u, 1u};
    static const uint32_t want_kind[4] = {9u, SU_HOST_EVENT_DESPAWNED, 9u, SU_HOST_EVENT_DESPAWNED};

    fill_schema(&zs, zf, (const SField[]){U(1, "unused")}, 1u, "demo.zero");
    fill_schema(&ps, pf, k_proj_schema, (uint32_t)N_PROJ, "space3d");
    memset(td, 0, sizeof td);
    td[0].struct_size = (uint32_t)sizeof td[0];
    td[0].schema = &zs;
    td[0].max_entities = 8u; /* table 0's capacity is table 1's base: world slots 8.. */
    td[1].struct_size = (uint32_t)sizeof td[1];
    td[1].schema = &ps;
    td[1].max_entities = 4u;
    memset(&cap, 0, sizeof cap);
    cap.capsule_json = (const uint8_t *)c.p;
    cap.capsule_len = c.n;
    cap.lir_json = (const uint8_t *)k_proj_life_lir;
    cap.lir_len = sizeof k_proj_life_lir - 1u;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.tick_rate_hz = 120;
    d.max_events_per_tick = 16;
    d.table_count = 2;
    d.tables = td;
    d.capsule_table = tables;
    check(su_host_world_create(&d, &w, &e) == SU_OK, "event-order world: %s", e.message);
    free(c.p);
    if (w == NULL) {
        return;
    }
    (void)spawn_proj(w, 0, 0, 1u, &e); /* both expire on the first tick */
    (void)spawn_proj(w, 0, 0, 1u, &e);
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "event-order tick: %s", e.message);
    collect_events(w, &ev);
    for (i = 0; i < ev.n; i++) {
        const SuHostEvent *v = &ev.ev[i];
        if (v->system_index == SU_HOST_EVENT_SYSTEM_HOST) {
            continue;
        }
        check(n < 4u, "at most four records from the system (%u)", (unsigned)n);
        if (n >= 4u) {
            break;
        }
        check(v->slot == want_slot[n], "record %u: slot %u, want %u", (unsigned)n, (unsigned)v->slot,
              (unsigned)want_slot[n]);
        check(v->seq == want_seq[n], "record %u: seq %u, want %u", (unsigned)n, (unsigned)v->seq,
              (unsigned)want_seq[n]);
        check(v->kind == want_kind[n], "record %u: kind %u, want %u", (unsigned)n, (unsigned)v->kind,
              (unsigned)want_kind[n]);
        check(v->a == want_slot[n], "record %u: a is the row's world slot (%u)", (unsigned)n, (unsigned)v->a);
        n++;
    }
    check(n == 4u, "the tick logged four records, two per row (%u)", (unsigned)n);
    su_host_world_destroy(w);
}

/* ---------- B2: gather_* and find_eq_u32 in table 1 use world slots ---------- */

/* Table 0 has 4 rows and no system, so table 1's base is 4. The claim pattern of LIVE_IR_SPEC.md runs in table 1:
 * every row gathers its own `want` through slot_u32, finds the lowest row that wants its own slot, and reads a
 * table-0 slot (out of range) as 0. */
static const SField k_claim_lanes[] = {U(1, "want"), U(2, "got"), U(3, "owner"), U(4, "outside"), U(5, "first")};

static const char k_claim_capsule[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"game.claim\",\"source\":\"inline.lir.json\","
    "\"schema\":{\"id\":\"demo.claim\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"fixed_q32_v1\",\"ir_version\":\"0.4\",\"capabilities\":[],\"fields\":["
    "{\"id\":1,\"name\":\"want\",\"type\":\"u32\",\"read\":true,\"write\":false},"
    "{\"id\":2,\"name\":\"got\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":3,\"name\":\"owner\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":4,\"name\":\"outside\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":5,\"name\":\"first\",\"type\":\"u32\",\"read\":false,\"write\":true}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},"
    "\"tests\":[\"tests/host/test_host_tables_entities.c\"]}";

static const char k_claim_lir[] =
    "{\"ir_version\":\"0.4\",\"system_id\":\"game.claim\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"demo.claim\",\"instructions\":["
    "{\"op\":\"load_u32\",\"dst\":\"w\",\"field\":\"want\"},"
    "{\"op\":\"slot_u32\",\"dst\":\"s\"},"
    "{\"op\":\"gather_u32\",\"dst\":\"g\",\"field\":\"want\",\"slot\":\"s\"},"
    "{\"op\":\"store_u32\",\"field\":\"got\",\"src\":\"g\"},"
    "{\"op\":\"find_eq_u32\",\"dst\":\"o\",\"field\":\"want\",\"key\":\"s\",\"span\":4},"
    "{\"op\":\"store_u32\",\"field\":\"owner\",\"src\":\"o\"},"
    "{\"op\":\"const_u32\",\"dst\":\"t0\",\"value\":1},"
    "{\"op\":\"gather_u32\",\"dst\":\"x\",\"field\":\"want\",\"slot\":\"t0\"},"
    "{\"op\":\"store_u32\",\"field\":\"outside\",\"src\":\"x\"},"
    "{\"op\":\"const_u32\",\"dst\":\"k4\",\"value\":4},"
    "{\"op\":\"find_eq_u32\",\"dst\":\"f\",\"field\":\"want\",\"key\":\"k4\",\"span\":4},"
    "{\"op\":\"store_u32\",\"field\":\"first\",\"src\":\"f\"}]}";

/* want = [4, 5, 4, 7]: rows 0, 1 and 3 want their own world slot (base 4 plus their local row), row 2 wants row
 * 0's slot, so row 2's own slot (6) has no claimant. */
static const char k_claim_fixture[] =
    "{\"fixture_format\":\"0.1\",\"schema_id\":\"demo.claim\",\"schema_version\":1,\"tick\":0,"
    "\"entity_ids\":[1,2,3,4],\"columns\":{\"want\":[4,5,4,7],\"got\":[0,0,0,0],\"owner\":[0,0,0,0],"
    "\"outside\":[0,0,0,0],\"first\":[0,0,0,0]}}";

static void test_claim_table1(void) {
    SuHostSchemaField zf[1];
    SuHostSchema zs, cs;
    SuHostSchemaField cf[5];
    SuHostTableDesc td[2];
    SuHostWorldDesc d;
    SuHostCapsule cap;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t tables[1] = {1u};
    uint64_t done = 0;
    uint32_t got[4], owner[4], outside[4], first[4];
    static const uint32_t want_got[4] = {4u, 5u, 4u, 7u};
    static const uint32_t want_owner[4] = {4u, 5u, 0xFFFFFFFFu, 7u};
    uint32_t i;

    fill_schema(&zs, zf, (const SField[]){U(1, "unused")}, 1u, "demo.zero");
    fill_schema(&cs, cf, k_claim_lanes, 5u, "demo.claim");
    memset(td, 0, sizeof td);
    td[0].struct_size = (uint32_t)sizeof td[0];
    td[0].schema = &zs;
    td[0].max_entities = 4u; /* table 1's base */
    td[1].struct_size = (uint32_t)sizeof td[1];
    td[1].schema = &cs;
    td[1].max_entities = 4u;
    td[1].fixture_json = (const uint8_t *)k_claim_fixture;
    td[1].fixture_len = sizeof k_claim_fixture - 1u;
    memset(&cap, 0, sizeof cap);
    cap.capsule_json = (const uint8_t *)k_claim_capsule;
    cap.capsule_len = sizeof k_claim_capsule - 1u;
    cap.lir_json = (const uint8_t *)k_claim_lir;
    cap.lir_len = sizeof k_claim_lir - 1u;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.tick_rate_hz = 120;
    d.table_count = 2;
    d.tables = td;
    d.capsule_table = tables;
    check(su_host_world_create(&d, &w, &e) == SU_OK, "claim world: %s", e.message);
    if (w == NULL) {
        return;
    }
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "claim tick: %s", e.message);
    check(su_host_read_in(w, 1u, 2u, 0u, 4u, got) == SU_OK, "check failed at line %d", __LINE__);
    check(su_host_read_in(w, 1u, 3u, 0u, 4u, owner) == SU_OK, "check failed at line %d", __LINE__);
    check(su_host_read_in(w, 1u, 4u, 0u, 4u, outside) == SU_OK, "check failed at line %d", __LINE__);
    check(su_host_read_in(w, 1u, 5u, 0u, 4u, first) == SU_OK, "check failed at line %d", __LINE__);
    for (i = 0; i < 4u; i++) {
        check(got[i] == want_got[i], "row %u: gather_u32(slot_u32) = %u, want %u", (unsigned)i, (unsigned)got[i],
              (unsigned)want_got[i]);
        check(owner[i] == want_owner[i], "row %u: find_eq_u32 returns world slot %u, want %u", (unsigned)i,
              (unsigned)owner[i], (unsigned)want_owner[i]);
        check(outside[i] == 0u, "row %u: a table-0 world slot reads 0 through gather_u32 (%u)", (unsigned)i,
              (unsigned)outside[i]);
        check(first[i] == 4u, "row %u: the lowest claimant of world slot 4 is row 0's slot (%u)", (unsigned)i,
              (unsigned)first[i]);
    }
    su_host_world_destroy(w);
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
 * handle survive, entries whose handle died in either table are erased, a table-0 handle and a table-1 entity at the
 * same LOCAL slot are told apart, and the state survives a snapshot round trip. */
static void test_stateful_operator_table1(void) {
    SuHostOperatorDesc kd;
    OpWorld o, of;
    SuHostWorld *w = NULL;
    SuHostWorld *fresh = NULL;
    SuHostError e;
    Bytes ac, al;
    uint64_t done = 0;
    const SuHostState *st = NULL;
    uint8_t data[8 + 16];
    uint32_t count = 0, slot0 = 1u, gen0 = 1u, slot1 = 2u, gen1 = 1u;
    uint64_t ship1 = 0;
    const void *p = NULL;
    uint32_t len = 0;
    uint8_t *snap = NULL;
    size_t snap_size = 0;

    keep_desc(&kd);
    check(su_host_register_operator(&kd, &e) == SU_OK || e.code == SU_E_DUPLICATE_KEY, "register game.keep: %s",
          e.message);
    ac = slurp("tests/fixtures/entities/despawn_move.capsule.json");
    al = slurp("tests/fixtures/entities/despawn_move.lir.json");
    memset(&o, 0, sizeof o);
    /* Table 0 has 3 rows (base 0), table 1 has 2 rows (base 3): table 0's local slot 1 is world slot 1, table 1's
     * local slot 1 is world slot 4. */
    w = op_world(&o, &kd, &ac, &al, 3u, 2u, (uint32_t)sizeof(SuHostWorldDesc), SU_HOST_TABLE_NONE, &e);
    check(w != NULL, "state world: %s", e.message);
    if (w == NULL) {
        free(ac.data);
        free(al.data);
        free(o.record.p);
        return;
    }
    spawn_pool(w, 2u, &e);
    memcpy(data, "HKEY", 4u);
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
        free(ac.data);
        free(al.data);
        su_host_world_destroy(w);
        free(o.record.p);
        return;
    }
    /* Two own entries (table 1's world slots 3 and 4) and two of table 0's (slots 1 and 2), keyed by world handle. */
    check(su_host_state_count(st) == 4u, "four entries (%u)", (unsigned)su_host_state_count(st));
    check(su_host_state_get(st, H(1u, 1u), 1u, &p, &len) == SU_OK && p != NULL, "the table-0 handle 1 is kept");
    check(su_host_state_get(st, H(2u, 1u), 1u, &p, &len) == SU_OK && p != NULL, "the table-0 handle 2 is kept");
    check(su_host_state_get(st, H(3u, 1u), 0u, &p, &len) == SU_OK && p != NULL, "the operator's own row 0 is kept");
    check(su_host_state_get(st, H(4u, 1u), 0u, &p, &len) == SU_OK && p != NULL, "the operator's own row 1 is kept");
    /* Despawn table 0's row 1 (world slot 1). Table 1's row 1 is the same LOCAL slot and stays alive, so its entry
     * (world slot 4) must survive: the purge validates a key against the table its slot belongs to. */
    check(su_host_entity_at_in(w, 0u, 1u, &ship1) == SU_OK, "table 0 slot 1 is alive");
    check(su_host_despawn(w, ship1, &e) == SU_OK, "despawn: %s", e.message);
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "state tick 2: %s", e.message);
    check(su_host_state_get(st, H(1u, 1u), 1u, &p, &len) == SU_OK && p == NULL, "the dead table-0 handle is purged");
    check(su_host_state_get(st, H(4u, 1u), 0u, &p, &len) == SU_OK && p != NULL,
          "table 1's row at the same local slot survives");
    check(su_host_state_get(st, H(2u, 1u), 1u, &p, &len) == SU_OK && p != NULL, "the live table-0 handle survives");
    check(su_host_state_get(st, H(3u, 1u), 0u, &p, &len) == SU_OK && p != NULL, "the operator's own row 0 survives");
    /* The state is part of the snapshot: a fresh world restored from it has the same entries and values. */
    check(su_host_snapshot_size(w, &snap_size) == SU_OK && snap_size > 0u, "the state world has a snapshot");
    snap = malloc(snap_size);
    check(snap != NULL && su_host_snapshot_write(w, snap, snap_size, &snap_size) == SU_OK, "the snapshot writes");
    memset(&of, 0, sizeof of);
    fresh = op_world(&of, &kd, &ac, &al, 3u, 2u, (uint32_t)sizeof(SuHostWorldDesc), SU_HOST_TABLE_NONE, &e);
    check(fresh != NULL, "the state restore world creates: %s", e.message);
    if (fresh != NULL && snap != NULL) {
        const SuHostState *fs = NULL;
        /* The operator's data joins the world identity (host.h, "operator state"), so the fresh world takes the same
         * data before the snapshot can load into it. */
        check(su_host_set_operator_data(fresh, "game.keep", data, sizeof data, &e) == SU_OK,
              "operator data on the restore world: %s", e.message);
        check(su_host_snapshot_restore(fresh, snap, snap_size, &e) == SU_OK, "restore the state world: %s",
              e.message);
        check(su_host_operator_state(fresh, 1u, &fs) == SU_OK && fs != NULL, "the restored operator has state");
        check(fs != NULL && su_host_state_count(fs) == 3u, "three entries survive the snapshot (%u)",
              fs != NULL ? (unsigned)su_host_state_count(fs) : 0u);
        check(fs != NULL && su_host_state_get(fs, H(1u, 1u), 1u, &p, &len) == SU_OK && p == NULL,
              "the purged entry stays purged");
        check(fs != NULL && su_host_state_get(fs, H(4u, 1u), 0u, &p, &len) == SU_OK && p != NULL,
              "table 1's entry survives the snapshot");
        check(fs != NULL && su_host_state_get(fs, H(2u, 1u), 1u, &p, &len) == SU_OK && p != NULL,
              "table 0's live entry survives the snapshot");
        /* And it keeps working: despawn table 1's row 0 (world slot 3) and only its own entry goes. */
        {
            uint64_t h = 0;
            check(su_host_entity_at_in(fresh, 1u, 0u, &h) == SU_OK, "table 1 slot 0 is alive in the restored world");
            check(su_host_despawn(fresh, h, &e) == SU_OK, "despawn in the restored world: %s", e.message);
        }
        check(su_host_step(fresh, 1u, &done, &e) == SU_OK && done == 1u, "restored state tick: %s", e.message);
        check(fs != NULL && su_host_state_get(fs, H(3u, 1u), 0u, &p, &len) == SU_OK && p == NULL,
              "the operator's dead row is purged");
        check(fs != NULL && su_host_state_get(fs, H(4u, 1u), 0u, &p, &len) == SU_OK && p != NULL,
              "its live row survives");
        check(fs != NULL && su_host_state_get(fs, H(2u, 1u), 1u, &p, &len) == SU_OK && p != NULL,
              "the table-0 entry survives");
    }
    su_host_world_destroy(fresh);
    free(of.record.p);
    free(snap);
    /* The same despawn in the original world: table 1's row 0 (world slot 3) loses its own entry only. */
    {
        uint64_t h = 0;
        check(su_host_entity_at_in(w, 1u, 0u, &h) == SU_OK, "table 1 slot 0 is alive");
        check(su_host_despawn(w, h, &e) == SU_OK, "check failed at line %d", __LINE__);
    }
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "state tick 3: %s", e.message);
    check(su_host_state_get(st, H(3u, 1u), 0u, &p, &len) == SU_OK && p == NULL, "the operator's dead row is purged");
    check(su_host_state_get(st, H(4u, 1u), 0u, &p, &len) == SU_OK && p != NULL, "its live row survives");
    su_host_world_destroy(w);
    free(ac.data);
    free(al.data);
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

/* ---------- the v2 pair budget counts the target table's rows too ---------- */

/* The v2 pair budget is max(2^22, 1024 x rows indexed). A cross-table query indexes the mover table's rows and its
 * target table's, so with 5000 movers and 3000 targets the budget is 1024 x 8000, not 1024 x 5000 (5000 rows put the
 * mover-only budget above the 2^22 floor, so the two differ). No row is spawned: the budget follows the capacities
 * the query iterates. */
static void test_pair_budget_cross_table(void) {
    SuHostSchemaField ship_fields[N_SHIP], proj_fields[N_PROJ];
    SuHostSchema ship_s, proj_s;
    SuHostTableDesc td[2];
    SuHostWorldDesc d;
    SuHostCapsule cap;
    SuHostWorld *w = NULL;
    SuHostError e;
    Bytes q = slurp("profiles/space3d/space3d_query2.capsule.json");
    uint32_t tables[1] = {1u};
    uint32_t targets[1] = {0u};
    uint64_t done = 0;
    SuHostOperatorCounters c;

    fill_schema(&ship_s, ship_fields, k_ship_schema, (uint32_t)N_SHIP, "game.ship");
    fill_schema(&proj_s, proj_fields, k_proj_schema, (uint32_t)N_PROJ, "space3d");
    memset(td, 0, sizeof td);
    td[0].struct_size = (uint32_t)sizeof td[0];
    td[0].schema = &ship_s;
    td[0].max_entities = 3000u;
    td[1].struct_size = (uint32_t)sizeof td[1];
    td[1].schema = &proj_s;
    td[1].max_entities = 5000u;
    memset(&cap, 0, sizeof cap);
    cap.capsule_json = q.data;
    cap.capsule_len = q.len;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.tick_rate_hz = 120;
    d.seed = 11;
    d.table_count = 2;
    d.tables = td;
    d.capsule_table = tables;
    d.capsule_target_table = targets;
    check(su_host_world_create(&d, &w, &e) == SU_OK, "pair-budget world: %s", e.message);
    free(q.data);
    if (w == NULL) {
        return;
    }
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "pair-budget tick: %s", e.message);
    memset(&c, 0, sizeof c);
    c.struct_size = (uint32_t)sizeof c;
    check(su_host_operator_counters(w, 0u, &c) == SU_OK, "the operator reports counters");
    check(c.runs == 1u, "the operator ran once (%llu)", (unsigned long long)c.runs);
    check(c.pair_budget == 1024ull * 8000ull, "the budget counts movers and targets (%llu)",
          (unsigned long long)c.pair_budget);
    su_host_world_destroy(w);
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

/* ---------- a 64-bit lane in the target set (issue 182) ---------- */

#define I64(id, name) {id, SU_TYPE_I64, name}

/* The target table's schema: a q32 position (lanes 10/11), an i64 clock (lanes 20/21), a 32-bit team and the lane
 * demo.wide_src writes. */
static const SField k_wide_schema[] = {Q(10, "pos_x"), I64(20, "clock64"), U(30, "team"), U(31, "seen")};
#define N_WIDE (sizeof k_wide_schema / sizeof k_wide_schema[0])

/* demo.wide_src: reads the target lanes (a lane some system reads is readable, so it can be a target lane) and
 * copies team into seen, so the table's systems are an ordinary read/write pair. */
static const IrField k_wide_src_fields[] = {
    {10, "pos_x", "q32", 1, 0},
    {20, "clock64", "i64", 1, 0},
    {30, "team", "u32", 1, 0},
    {31, "seen", "u32", 0, 1},
};
static const char k_wide_src_lir[] =
    "{\"ir_version\":\"0.3\",\"system_id\":\"demo.wide_src\",\"numeric_profile\":\"fixed_q32_v1\","
    "\"query\":\"demo.wide\",\"instructions\":["
    "{\"op\":\"load_q32\",\"dst\":\"p\",\"field\":\"pos_x\"},"
    "{\"op\":\"load_i64\",\"dst\":\"c\",\"field\":\"clock64\"},"
    "{\"op\":\"load_u32\",\"dst\":\"t\",\"field\":\"team\"},"
    "{\"op\":\"store_u32\",\"field\":\"seen\",\"src\":\"t\"}]}";

enum { WR_MARK = 1 };
static const SuHostOperatorField k_wide_own_fields[] = {{WR_MARK, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "mark"}};

/* game.wide_reach: reads a target set that mixes 64-bit lanes with a 32-bit one. A 64-bit value is two storage
 * lanes, so each q32/i64 target field is the pair ".lo" (id I) and ".hi" (id I + 1), as for the operator's own
 * fields. The run reassembles both pairs, keeps what it read for the test, and writes the q32's low word into its
 * own table's mark lane. */
static const SuHostOperatorField k_wide_target_fields[] = {
    {10u, SU_TYPE_Q32, SU_ACCESS_READ, 0, "pos_x.lo"},
    {11u, SU_TYPE_Q32, SU_ACCESS_READ, 0, "pos_x.hi"},
    {20u, SU_TYPE_I64, SU_ACCESS_READ, 0, "clock64.lo"},
    {21u, SU_TYPE_I64, SU_ACCESS_READ, 0, "clock64.hi"},
    {30u, SU_TYPE_U32, SU_ACCESS_READ, 0, "team"},
};
#define N_WIDE_TARGET (uint32_t)(sizeof k_wide_target_fields / sizeof k_wide_target_fields[0])

static uint32_t g_wr_runs, g_wr_cols, g_wr_alive, g_wr_team;
static int64_t g_wr_pos, g_wr_clock;

static int64_t lanes64(uint32_t lo, uint32_t hi) {
    return (int64_t)(((uint64_t)hi << 32) | (uint64_t)lo);
}

static uint32_t wide_reach_run(const SuHostOperatorRun *r, void *user) {
    uint64_t i;
    uint32_t alive = 0;
    (void)user;
    g_wr_runs++;
    if (r->target_column_count != N_WIDE_TARGET || r->target_columns == NULL || r->target_alive == NULL ||
        r->target_generation == NULL || r->target_table != 0u || r->target_rows < 1u) {
        return SU_E_INTERNAL;
    }
    for (i = 0; i < N_WIDE_TARGET; i++) {
        if (r->target_columns[i].in == NULL || r->target_columns[i].out != NULL) {
            return SU_E_INTERNAL;
        }
    }
    for (i = 0; i < r->target_rows; i++) {
        alive += r->target_alive[i] != 0u;
    }
    g_wr_cols = r->target_column_count;
    g_wr_alive = alive;
    g_wr_pos = lanes64(r->target_columns[0].in[0], r->target_columns[1].in[0]);
    g_wr_clock = lanes64(r->target_columns[2].in[0], r->target_columns[3].in[0]);
    g_wr_team = r->target_columns[4].in[0];
    for (i = 0; i < r->rows; i++) {
        r->columns[0].out[i] = (uint32_t)(uint64_t)g_wr_pos;
    }
    return SU_OK;
}

static void wide_desc(SuHostOperatorDesc *d) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(d);
    d->system_id = "game.wide_reach";
    d->schema_id = "demo.pool";
    d->operator_name = "wide_ops_v1";
    d->operator_version = 1u;
    d->field_count = 1u;
    d->fields = k_wide_own_fields;
    for (i = 0; i < 32u; i++) {
        d->constants_hash[i] = (uint8_t)(0xE0u + i);
    }
    d->scratch_size = pulse_scratch;
    d->run = wide_reach_run;
    d->target_field_count = N_WIDE_TARGET;
    d->target_fields = k_wide_target_fields;
}

typedef struct WideWorld {
    Buf src_cap, record;
    SuHostCapsule caps[2];
    SuHostTableDesc td[2];
    SuHostSchemaField wf[N_WIDE], pf[2];
    SuHostSchema ws, ps;
    SuHostWorldDesc desc;
    uint32_t tables[2], targets[2];
} WideWorld;

static void wide_free(WideWorld *x) {
    free(x->src_cap.p);
    free(x->record.p);
}

/* Table 0 is the target table (demo.wide), table 1 the operator's own (demo.pool). */
static SuHostWorld *wide_world(WideWorld *x, const SuHostOperatorDesc *op, SuHostError *err) {
    static const SField pool_schema[] = {U(1, "mark"), U(2, "n")};
    SuHostWorld *w = NULL;
    memset(x, 0, sizeof *x);
    x->src_cap = ir_capsule("demo.wide_src", "demo.wide", "simulate", "", k_wide_src_fields,
                            (uint32_t)(sizeof k_wide_src_fields / sizeof k_wide_src_fields[0]), "0.3",
                            "fixed_q32_v1", "", 0u, 0u);
    x->record = host_record(op->system_id, op->schema_id, op->operator_name, op->operator_version,
                            op->constants_hash, op->fields, op->field_count, "simulate", "u32_mod_v1");
    x->caps[0].capsule_json = (const uint8_t *)x->src_cap.p;
    x->caps[0].capsule_len = x->src_cap.n;
    x->caps[0].lir_json = (const uint8_t *)k_wide_src_lir;
    x->caps[0].lir_len = strlen(k_wide_src_lir);
    x->caps[1].capsule_json = (const uint8_t *)x->record.p;
    x->caps[1].capsule_len = x->record.n;
    fill_schema(&x->ws, x->wf, k_wide_schema, (uint32_t)N_WIDE, "demo.wide");
    fill_schema(&x->ps, x->pf, pool_schema, 2u, "demo.pool");
    memset(x->td, 0, sizeof x->td);
    x->td[0].struct_size = (uint32_t)sizeof x->td[0];
    x->td[0].schema = &x->ws;
    x->td[0].max_entities = 4u;
    x->td[1].struct_size = (uint32_t)sizeof x->td[1];
    x->td[1].schema = &x->ps;
    x->td[1].max_entities = 4u;
    x->tables[0] = 0u;
    x->tables[1] = 1u;
    x->targets[0] = SU_HOST_TABLE_NONE;
    x->targets[1] = 0u;
    memset(&x->desc, 0, sizeof x->desc);
    x->desc.struct_size = (uint32_t)sizeof x->desc;
    x->desc.capsule_count = 2;
    x->desc.capsules = x->caps;
    x->desc.tick_rate_hz = 120;
    x->desc.max_events_per_tick = 8;
    x->desc.table_count = 2;
    x->desc.tables = x->td;
    x->desc.capsule_table = x->tables;
    x->desc.capsule_target_table = x->targets;
    if (su_host_world_create(&x->desc, &w, err) != SU_OK) {
        return NULL;
    }
    return w;
}

/* Spawns one target row: pos_x and clock64 as 64-bit values (two lanes each), team, and seen 0. */
static void spawn_wide(SuHostWorld *w, int64_t pos, int64_t clock, uint32_t team, SuHostError *err) {
    SuHostValue v[8];
    uint64_t h = 0;
    uint32_t n = 0;
    n = put_q32(v, n, 10u, pos);
    n = put_q32(v, n, 20u, clock);
    v[n].field_id = 30u;
    v[n++].value = team;
    v[n].field_id = 31u;
    v[n++].value = 0u;
    check(su_host_spawn_in(w, 0u, v, n, 0u, &h, err) == SU_OK, "wide target spawn: %s", err->message);
}

static void test_target_64bit_lanes(void) {
    SuHostOperatorDesc wd;
    WideWorld x;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint64_t done = 0;
    uint32_t mark[2] = {0, 0};
    const int64_t pos = q32(3) + INT64_C(7); /* 3 + 7/2^32: both lanes carry a nonzero word */
    const int64_t clock = -INT64_C(5000000000);
    uint32_t i;

    wide_desc(&wd);
    check(su_host_register_operator(&wd, &e) == SU_OK, "register game.wide_reach: %s", e.message);
    w = wide_world(&x, &wd, &e);
    check(w != NULL, "64-bit target world: %s", e.message);
    if (w == NULL) {
        wide_free(&x);
        return;
    }
    spawn_wide(w, pos, clock, 7u, &e);
    spawn_wide(w, q32(1), INT64_C(1), 9u, &e);
    spawn_pool(w, 2u, &e);
    check(su_host_step(w, 1u, &done, &e) == SU_OK && done == 1u, "the 64-bit target tick: %s", e.message);
    check(g_wr_runs == 1u, "the operator ran once (%u)", (unsigned)g_wr_runs);
    check(g_wr_cols == N_WIDE_TARGET, "the target set has %u columns (%u)", (unsigned)N_WIDE_TARGET,
          (unsigned)g_wr_cols);
    check(g_wr_alive == 2u, "both target rows are alive (%u)", (unsigned)g_wr_alive);
    check(g_wr_pos == pos, "the q32 target pair reads as one 64-bit value (%lld)", (long long)g_wr_pos);
    check(g_wr_clock == clock, "the i64 target pair reads as one 64-bit value (%lld)", (long long)g_wr_clock);
    check(g_wr_team == 7u, "the 32-bit target lane next to the pairs reads its row's value (%u)",
          (unsigned)g_wr_team);
    check(su_host_read_in(w, 1u, WR_MARK, 0u, 2u, mark) == SU_OK, "check failed at line %d", __LINE__);
    for (i = 0; i < 2u; i++) {
        check(mark[i] == (uint32_t)(uint64_t)pos, "row %u's mark lane is the q32's low word (%u)", (unsigned)i,
              (unsigned)mark[i]);
    }
    /* The world carries the target table's lanes, both halves of each 64-bit field. */
    {
        uint32_t id = 0;
        check(su_host_field_id_in(w, 0u, "pos_x.hi", &id) == SU_OK && id == 11u,
              "the target table names the q32's high lane (id %u)", (unsigned)id);
        check(su_host_field_id_in(w, 0u, "clock64.hi", &id) == SU_OK && id == 21u,
              "the target table names the i64's high lane (id %u)", (unsigned)id);
    }
    su_host_world_destroy(w);
    wide_free(&x);
}

/* The 64-bit pair rules are unchanged for a target set: an unpaired, wrong-id or wrong-type .hi entry, and a
 * 64-bit lane with no partner after it, are refused with the existing message. */
static void test_target_64bit_refusals(void) {
    /* A q32 lane at 10 followed by a u32 lane at 30: the pair's .hi is missing. */
    static const SuHostOperatorField unpaired[] = {
        {10u, SU_TYPE_Q32, SU_ACCESS_READ, 0, "pos_x.lo"},
        {30u, SU_TYPE_U32, SU_ACCESS_READ, 0, "team"},
    };
    /* The .hi at 12 instead of 11. */
    static const SuHostOperatorField wrong_id[] = {
        {10u, SU_TYPE_Q32, SU_ACCESS_READ, 0, "pos_x.lo"},
        {12u, SU_TYPE_Q32, SU_ACCESS_READ, 0, "pos_x.hi"},
    };
    /* The .hi typed i64. */
    static const SuHostOperatorField wrong_type[] = {
        {10u, SU_TYPE_Q32, SU_ACCESS_READ, 0, "pos_x.lo"},
        {11u, SU_TYPE_I64, SU_ACCESS_READ, 0, "pos_x.hi"},
    };
    /* A u32 lane, then a q32 lane with no partner after it. */
    static const SuHostOperatorField trailing[] = {
        {30u, SU_TYPE_U32, SU_ACCESS_READ, 0, "team"},
        {40u, SU_TYPE_Q32, SU_ACCESS_READ, 0, "pos_x.lo"},
    };
    /* A valid pair whose .hi half is not read-only. */
    static const SuHostOperatorField writable_hi[] = {
        {10u, SU_TYPE_Q32, SU_ACCESS_READ, 0, "pos_x.lo"},
        {11u, SU_TYPE_Q32, SU_ACCESS_WRITE, 0, "pos_x.hi"},
    };
    static const char pair_rule[] = "is a 64-bit lane: the next entry must be id + 1 with the same type";
    SuHostOperatorDesc d, bad;
    SuHostError e;
    uint32_t st, base = su_host_operator_count();

#define REFUSE_TARGET(arr, what, at, msgpart)                                                         \
    do {                                                                                              \
        bad = d;                                                                                      \
        bad.target_fields = (arr);                                                                    \
        bad.target_field_count = (uint32_t)(sizeof(arr) / sizeof((arr)[0]));                           \
        st = su_host_register_operator(&bad, &e);                                                     \
        check(st == SU_E_INVALID_ARGUMENT, "%s: got %s (%s)", what, su_host_status_name(st), e.message); \
        check(e.index == (at), "%s: err->index %u", what, (unsigned)e.index);                          \
        check(strstr(e.message, (msgpart)) != NULL, "%s: the message (%s)", what, e.message);          \
    } while (0)
    wide_desc(&d);
    d.system_id = "game.wide_bad"; /* never registered: every variant below is refused */
    REFUSE_TARGET(unpaired, "an unpaired .hi (a u32 lane where the .hi belongs)", 0u, pair_rule);
    REFUSE_TARGET(wrong_id, "a .hi at id + 2", 0u, pair_rule);
    REFUSE_TARGET(wrong_type, "a .hi of another type", 0u, pair_rule);
    REFUSE_TARGET(trailing, "a trailing 64-bit lane with no partner", 1u, pair_rule);
    REFUSE_TARGET(writable_hi, "a writable .hi half", 1u, "must be a read-only lane");
#undef REFUSE_TARGET
    check(su_host_operator_count() == base, "refused target sets register nothing");
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
        x.targets[4] = 1u;
        check(su_host_world_create(&x.desc, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL,
              "the target table must differ from the capsule's table (%s)", e.message);
        x.targets[4] = 7u;
        check(su_host_world_create(&x.desc, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL,
              "the target table must exist (%s)", e.message);
        /* A capsule that takes no target set cannot name one. */
        x.targets[4] = SU_HOST_TABLE_NONE;
        x.targets[0] = 1u;
        check(su_host_world_create(&x.desc, &w, &e) == SU_E_INVALID_ARGUMENT && w == NULL,
              "an IR system takes no target set (%s)", e.message);
        x.targets[0] = SU_HOST_TABLE_NONE;
        /* The target table's declared schema must carry every target lane. */
        x.ship_fields[4].name = "pos_z_x";
        x.targets[4] = 0u;
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
    test_table1_event_order();
    test_claim_table1();
    test_world_slots();
    test_idle_target_table();
    test_operator_events();
    test_registered_target();
    test_target_64bit_lanes();
    test_target_64bit_refusals();
    test_pair_budget_cross_table();
    test_stateful_operator_table1();
    test_descriptor_sizes();
    test_refusals();
    test_no_allocation_in_tick();
    printf("test_host_tables_entities: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
