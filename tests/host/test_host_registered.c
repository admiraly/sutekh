/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Registered host operators (include/sutekh/host.h, "registered host operators"; issue 137), built twice: against
 * the static library and against the DLL. Everything the operator needs lives in this translation unit, outside
 * the engine library: the operator (game.target: for every armed ship, the nearest live ship of another team, a
 * cross-row search IR cannot express), its descriptor, its host record, the IR capsules around it and the fixture.
 *
 * The world (schema game.fleet, capacity 16, 12 ships, slot 3 despawned) runs, in capsule order:
 *   game.move    IR, phase simulate: x += vx;
 *   game.target  registered operator, phase query: target slot, generation and signed i64 distance, a read-write
 *                counter, a write-only mark stored for even slots only, and the tick it ran in;
 *   game.fire    IR, phase simulate, after game.target: shots += (target != NONE), last_tgt = target (same tick).
 * An independent reference model computed here checks every lane of every row after every tick.
 *
 * In-process checks (no arguments):
 *   1. registration refusals (one per rule; names with quotes, control characters or non-ASCII bytes), unregistered
 *      record (SU_E_CAPABILITY_DENIED), duplicate, built-in or reserved IDs in any letter case and reserved operator
 *      names (SU_E_DUPLICATE_KEY), listing;
 *   2. schedule order and same-tick consumption against the reference model, the run contract (columns, prefill,
 *      tick, scratch zeroed, row range), write-only lanes leak no input;
 *   3. snapshots, digests (SHA-256 and fast), allocating and in-place restore + re-simulation, bit-identical;
 *   4. replay record, seek and play in a fresh world;
 *   5. a schema-relative binding of the operator: same lanes, different world identity;
 *   6. negative: mismatched records (name, version, constants hash, field access, name, count, numeric profile),
 *      an IR system disagreeing with a declared lane, undeclared writes into read lanes (detected:
 *      SU_E_UNDECLARED_WRITE, nothing committed; undetected only with SU_HOST_OPERATOR_TRUST_INPUTS), a failing
 *      run, a refused scratch size;
 *   7. operator data: refusals, identity, effect on results;
 *   8. no allocation in the tick (static Linux build: malloc/calloc/realloc wrapped and counted);
 *   9. two worlds stepped on two threads equal the serial run;
 *  10. descriptor versioning (v1 size, a larger descriptor with a zero or nonzero tail), concurrent registration,
 *      the registry limit; a failing run's reason buffer (in 6).
 *
 * Cross-process modes (ctest runs record, then verify in other processes, partly through the DLL):
 *   record <dir>             writes a snapshot at tick 20, a replay of 60 ticks and the digests;
 *   verify <dir> same        same registration: the snapshot restores, the replay verifies, digests equal;
 *   verify <dir> version|constants|name|flags|scratch   the operator registered with another operator_version,
 *                            constants hash, operator name, flags (TRUST_INPUTS) or scratch size (and a record
 *                            matching it): snapshot and replay refused (SU_E_SCHEMA_MISMATCH, world identity);
 *   verify <dir> none        nothing registered: the record is refused (SU_E_CAPABILITY_DENIED). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"
#include "sutekh/replay.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

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

/* ---------- allocation counting (static Linux build, -Wl,--wrap) ---------- */

#ifdef SU_TEST_WRAP_ALLOC
static volatile int g_count_alloc;
static unsigned long g_allocs;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t m);
void *__wrap_realloc(void *p, size_t n);
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

/* ---------- the schema ---------- */

#define CAP 16u
#define SHIPS 12u
#define DEAD_SLOT 3u
#define NONE UINT32_C(0xFFFFFFFF)

enum { F_X = 1, F_VX = 2, F_TEAM = 3, F_ARMED = 4, F_TGT = 10, F_TGEN = 11, F_DIST = 12, F_DIST_HI = 13, F_SEEN = 14,
       F_MARK = 15, F_OPTICK = 16, F_SHOTS = 20, F_LAST = 21 };

/* Operator lanes, descriptor order (indices into run->columns). */
enum { C_X, C_TEAM, C_ARMED, C_TGT, C_TGEN, C_DIST_LO, C_DIST_HI, C_SEEN, C_MARK, C_OPTICK, C_COUNT };

static const SuHostOperatorField k_fields[C_COUNT] = {
    {F_X, SU_TYPE_U32, SU_ACCESS_READ, 0, "x"},
    {F_TEAM, SU_TYPE_U32, SU_ACCESS_READ, 0, "team"},
    {F_ARMED, SU_TYPE_U32, SU_ACCESS_READ, 0, "armed"},
    {F_TGT, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "tgt_slot"},
    {F_TGEN, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "tgt_gen"},
    {F_DIST, SU_TYPE_I64, SU_ACCESS_WRITE, 0, "tgt_dist.lo"},
    {F_DIST_HI, SU_TYPE_I64, SU_ACCESS_WRITE, 0, "tgt_dist.hi"},
    {F_SEEN, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, 0, "seen"},
    {F_MARK, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "mark"},
    {F_OPTICK, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "op_tick"},
};

/* ---------- the operator (game code, outside the engine) ---------- */

typedef struct OpStats {
    unsigned calls;
    unsigned contract_bad;   /* a run saw a contract violation (counted, reported by the test) */
    unsigned scratch_dirty;  /* scratch was not zero at entry */
    uint64_t last_tick;
} OpStats;

static OpStats g_stats;

typedef struct OpData {
    uint32_t max_range;
} OpData;

static uint32_t target_scratch(uint64_t capacity, size_t *bytes, void *user) {
    (void)user;
    *bytes = (size_t)capacity * sizeof(uint32_t);
    return SU_OK;
}

/* The V_SCRATCH variant: same results, a larger scratch (the identity must still differ). */
static uint32_t target_scratch_big(uint64_t capacity, size_t *bytes, void *user) {
    (void)user;
    *bytes = (size_t)capacity * sizeof(uint32_t) + 64u;
    return SU_OK;
}

static int64_t abs64(int64_t v) {
    return v < 0 ? -v : v;
}

static uint32_t target_run(const SuHostOperatorRun *r, void *user) {
    const uint32_t *x, *team, *armed, *seen_in;
    uint32_t *tgt, *tgen, *dlo, *dhi, *seen, *mark, *optick;
    uint32_t *eligible = (uint32_t *)r->scratch;
    const OpData *data = (const OpData *)r->data;
    uint64_t i, j, n = 0;
    (void)user;
    g_stats.calls++;
    g_stats.last_tick = r->tick;
    if (r->struct_size != sizeof *r || r->column_count != C_COUNT || r->row_begin != 0u || r->row_end != r->rows ||
        r->chunk_index != 0u || r->chunk_count != 1u || r->scratch == NULL ||
        r->scratch_bytes < (size_t)r->rows * sizeof(uint32_t) || r->reason == NULL || r->reason[0] != '\0') {
        g_stats.contract_bad++;
        return SU_E_INTERNAL;
    }
    for (i = 0; i < C_COUNT; i++) {
        const SuHostOperatorColumn *c = &r->columns[i];
        int rd = (k_fields[i].access & SU_ACCESS_READ) != 0u, wr = (k_fields[i].access & SU_ACCESS_WRITE) != 0u;
        if (c->field_id != k_fields[i].id || c->access != k_fields[i].access || (c->in != NULL) != rd ||
            (c->out != NULL) != wr) {
            g_stats.contract_bad++;
            return SU_E_INTERNAL;
        }
        for (j = 0; j < r->rows; j++) {
            /* Prefill: write-only lanes are zero, read-write lanes start from their input. */
            if (wr && !rd && c->out[j] != 0u) {
                g_stats.contract_bad++;
            }
            if (wr && rd && c->out[j] != c->in[j]) {
                g_stats.contract_bad++;
            }
        }
    }
    for (i = 0; i < r->scratch_bytes / sizeof(uint32_t); i++) {
        if (eligible[i] != 0u) {
            g_stats.scratch_dirty++;
            break;
        }
    }
    x = r->columns[C_X].in;
    team = r->columns[C_TEAM].in;
    armed = r->columns[C_ARMED].in;
    seen_in = r->columns[C_SEEN].in;
    tgt = r->columns[C_TGT].out;
    tgen = r->columns[C_TGEN].out;
    dlo = r->columns[C_DIST_LO].out;
    dhi = r->columns[C_DIST_HI].out;
    seen = r->columns[C_SEEN].out;
    mark = r->columns[C_MARK].out;
    optick = r->columns[C_OPTICK].out;
    for (i = 0; i < r->rows; i++) {
        if (r->alive[i]) {
            eligible[n++] = (uint32_t)i;
        }
    }
    for (i = 0; i < r->rows; i++) {
        uint32_t best = NONE;
        int64_t best_d = 0;
        tgt[i] = NONE;
        tgen[i] = 0u;
        dlo[i] = 0u;
        dhi[i] = 0u;
        optick[i] = (uint32_t)r->tick;
        if (!r->alive[i]) {
            continue;
        }
        if ((i & 1u) == 0u) {
            mark[i] = 0xA500u + (uint32_t)i; /* odd rows are not stored: the engine's zero prefill stands */
        }
        if (armed[i] == 0u) {
            continue;
        }
        for (j = 0; j < n; j++) {
            uint32_t s = eligible[j];
            int64_t d = (int64_t)(int32_t)x[s] - (int64_t)(int32_t)x[i];
            if (s == i || team[s] == team[i] || (data != NULL && abs64(d) > (int64_t)data->max_range)) {
                continue;
            }
            if (best == NONE || abs64(d) < abs64(best_d)) { /* ties: the lower slot (scan order) */
                best = s;
                best_d = d;
            }
        }
        if (best != NONE) {
            tgt[i] = best;
            tgen[i] = r->generation[best];
            dlo[i] = (uint32_t)((uint64_t)best_d & 0xFFFFFFFFu);
            dhi[i] = (uint32_t)((uint64_t)best_d >> 32);
            seen[i] = seen_in[i] + 1u;
        }
    }
    return SU_OK;
}

static uint32_t target_parse(const uint8_t *bytes, size_t len, void **parsed, char *why, size_t why_cap, void *user) {
    OpData *d;
    (void)user;
    if (len != 8u || memcmp(bytes, "RNG1", 4) != 0) {
        snprintf(why, why_cap, "operator data must be \"RNG1\" and a u32 range (8 bytes), got %u bytes",
                 (unsigned)len);
        return SU_E_PARSE;
    }
    d = malloc(sizeof *d);
    if (d == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    d->max_range = (uint32_t)bytes[4] | (uint32_t)bytes[5] << 8 | (uint32_t)bytes[6] << 16 | (uint32_t)bytes[7] << 24;
    *parsed = d;
    return SU_OK;
}

static unsigned g_releases;

static void target_release(void *parsed, void *user) {
    (void)user;
    g_releases++;
    free(parsed);
}

/* Variants of the registration (cross-process identity checks). */
enum { V_SAME, V_VERSION, V_CONSTANTS, V_NAME, V_FLAGS, V_SCRATCH };

static void fleet_desc(SuHostOperatorDesc *d, int variant) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(d);
    d->system_id = "game.target";
    d->schema_id = "game.fleet";
    d->operator_name = variant == V_NAME ? "fleet_ops_v1b" : "fleet_ops_v1";
    d->operator_version = variant == V_VERSION ? 2u : 1u;
    d->field_count = C_COUNT;
    d->fields = k_fields;
    for (i = 0; i < 32u; i++) {
        d->constants_hash[i] = (uint8_t)(i * 7u + (variant == V_CONSTANTS ? 3u : 1u));
    }
    d->flags = variant == V_FLAGS ? SU_HOST_OPERATOR_TRUST_INPUTS : 0u;
    d->scratch_size = variant == V_SCRATCH ? target_scratch_big : target_scratch;
    d->run = target_run;
    d->parse_data = target_parse;
    d->release_data = target_release;
}

/* ---------- JSON builders ---------- */

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

static const char *access_name(uint32_t a) {
    return a == SU_ACCESS_READ ? "read" : a == SU_ACCESS_WRITE ? "write" : "read_write";
}

/* Record options: the operator's own table, or a binding to schema game.fleet2 (IDs + 100, names "g_" + name). */
typedef struct RecOpt {
    const SuHostOperatorDesc *d;
    int bound;
    const char *operator_name;   /* NULL: d's */
    int version_delta;
    int flip_hash;
    int access_flip_field;       /* -1 none */
    int rename_field;            /* -1 none */
    int drop_last;
    const char *profile;         /* NULL: fixed_q32_v1 (the table has an i64 lane) */
} RecOpt;

static void rec_default(RecOpt *o, const SuHostOperatorDesc *d) {
    memset(o, 0, sizeof *o);
    o->d = d;
    o->access_flip_field = -1;
    o->rename_field = -1;
}

static Buf make_record(const RecOpt *o) {
    Buf b = {NULL, 0, 0};
    uint32_t i, n = o->d->field_count - (o->drop_last ? 1u : 0u);
    put(&b, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"system_rev\":1,\"capabilities\":[],", o->d->system_id);
    put(&b, "\"schema\":{\"id\":\"%s\",\"version\":1,\"layout_generation\":1},",
        o->bound ? "game.fleet2" : "game.fleet");
    put(&b, "\"numeric_profile\":\"%s\",", o->profile != NULL ? o->profile : "fixed_q32_v1");
    put(&b, "\"schedule\":{\"phase\":\"query\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},");
    put(&b, "\"host\":{\"operator\":\"%s\",\"operator_version\":%u,\"constants_hash\":\"",
        o->operator_name != NULL ? o->operator_name : o->d->operator_name,
        (unsigned)((int)o->d->operator_version + o->version_delta));
    for (i = 0; i < 32u; i++) {
        put(&b, "%02x", (unsigned)(o->d->constants_hash[i] ^ (o->flip_hash && i == 5u ? 0x10u : 0u)));
    }
    put(&b, "\",\"fields\":[");
    for (i = 0; i < n; i++) {
        const SuHostOperatorField *f = &o->d->fields[i];
        uint32_t acc = f->access;
        if ((int)i == o->access_flip_field) {
            acc = acc == SU_ACCESS_READ ? SU_ACCESS_WRITE : SU_ACCESS_READ;
        }
        put(&b, "%s{\"id\":%u,\"name\":\"%s%s%s\",\"type\":\"%s\",\"access\":\"%s\"", i > 0u ? "," : "",
            (unsigned)(f->id + (o->bound ? 100u : 0u)), o->bound ? "g_" : "", f->name,
            (int)i == o->rename_field ? "_x" : "", type_name(f->type), access_name(acc));
        if (o->bound) {
            put(&b, ",\"binds\":\"%s\"", f->name);
        }
        put(&b, "}");
    }
    put(&b, "]}}");
    return b;
}

typedef struct IrField {
    uint32_t id;
    const char *name;
    int read, write;
} IrField;

static Buf make_capsule(const char *system_id, const char *schema, const char *phase, const char *after,
                        const IrField *f, uint32_t n) {
    Buf b = {NULL, 0, 0};
    uint32_t i;
    put(&b, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"source\":\"inline.lir.json\",", system_id);
    put(&b, "\"schema\":{\"id\":\"%s\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"u32_mod_v1\",",
        schema);
    put(&b, "\"fields\":[");
    for (i = 0; i < n; i++) {
        put(&b, "%s{\"id\":%u,\"name\":\"%s\",\"type\":\"u32\",\"read\":%s,\"write\":%s}", i > 0u ? "," : "",
            (unsigned)f[i].id, f[i].name, f[i].read ? "true" : "false", f[i].write ? "true" : "false");
    }
    put(&b, "],\"schedule\":{\"phase\":\"%s\",\"after\":[%s],\"before\":[],\"parallel\":\"serial\"},", phase, after);
    put(&b, "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},");
    put(&b, "\"tests\":[\"tests/host/test_host_registered.c\"]}");
    return b;
}

/* IR of game.move (x += vx) and game.fire (shots += target != NONE; last_tgt = target), for a field prefix. */
static Buf make_move_lir(const char *schema, const char *pfx) {
    Buf b = {NULL, 0, 0};
    put(&b, "{\"ir_version\":\"0.1\",\"system_id\":\"game.move\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"%s\","
            "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"p\",\"field\":\"%sx\"},"
            "{\"op\":\"load_u32\",\"dst\":\"v\",\"field\":\"%svx\"},"
            "{\"op\":\"add_u32\",\"dst\":\"n\",\"a\":\"p\",\"b\":\"v\"},"
            "{\"op\":\"store_u32\",\"field\":\"%sx\",\"src\":\"n\"}]}",
        schema, pfx, pfx, pfx);
    return b;
}

static Buf make_fire_lir(const char *schema, const char *pfx) {
    Buf b = {NULL, 0, 0};
    put(&b, "{\"ir_version\":\"0.1\",\"system_id\":\"game.fire\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"%s\","
            "\"instructions\":[{\"op\":\"const_u32\",\"dst\":\"none\",\"value\":4294967295},"
            "{\"op\":\"const_u32\",\"dst\":\"one\",\"value\":1},{\"op\":\"const_u32\",\"dst\":\"zero\",\"value\":0},"
            "{\"op\":\"load_u32\",\"dst\":\"t\",\"field\":\"%stgt_slot\"},"
            "{\"op\":\"eq_u32\",\"dst\":\"isnone\",\"a\":\"t\",\"b\":\"none\"},"
            "{\"op\":\"select_u32\",\"dst\":\"inc\",\"cond\":\"isnone\",\"on_true\":\"zero\",\"on_false\":\"one\"},"
            "{\"op\":\"load_u32\",\"dst\":\"s\",\"field\":\"%sshots\"},"
            "{\"op\":\"add_u32\",\"dst\":\"s2\",\"a\":\"s\",\"b\":\"inc\"},"
            "{\"op\":\"store_u32\",\"field\":\"%sshots\",\"src\":\"s2\"},"
            "{\"op\":\"store_u32\",\"field\":\"%slast_tgt\",\"src\":\"t\"}]}",
        schema, pfx, pfx, pfx, pfx);
    return b;
}

/* ---------- worlds ---------- */

typedef struct Game {
    Buf move_cap, move_lir, fire_cap, fire_lir, record, fixture;
    SuHostCapsule caps[3];
    SuHostWorldDesc desc;
} Game;

static uint32_t init_x(uint32_t i) {
    return (i * 37u) % 101u;
}
static uint32_t init_vx(uint32_t i) {
    static const int32_t v[7] = {-3, -2, -1, 0, 1, 2, 3};
    return (uint32_t)v[(i * 5u) % 7u];
}
static uint32_t init_team(uint32_t i) {
    return i % 3u;
}
static uint32_t init_armed(uint32_t i) {
    return i % 4u != 1u ? 1u : 0u;
}

/* bound: the game schema game.fleet2 (IDs + 100, names "g_" + name) with a binding record. record_override: NULL
 * for the record of d. */
static void game_build(Game *g, const SuHostOperatorDesc *d, int bound, const Buf *record_override) {
    const char *schema = bound ? "game.fleet2" : "game.fleet";
    const char *pfx = bound ? "g_" : "";
    const uint32_t off = bound ? 100u : 0u;
    char nm[3][4][32];
    IrField mf[2], ff[3];
    RecOpt ro;
    uint32_t i, k;
    static const char *const cols[] = {"x", "vx", "team", "armed", "tgt_slot", "tgt_gen", "tgt_dist.lo",
                                       "tgt_dist.hi", "seen", "mark", "op_tick", "shots", "last_tgt"};
    memset(g, 0, sizeof *g);
    snprintf(nm[0][0], sizeof nm[0][0], "%sx", pfx);
    snprintf(nm[0][1], sizeof nm[0][1], "%svx", pfx);
    snprintf(nm[1][0], sizeof nm[1][0], "%stgt_slot", pfx);
    snprintf(nm[1][1], sizeof nm[1][1], "%sshots", pfx);
    snprintf(nm[1][2], sizeof nm[1][2], "%slast_tgt", pfx);
    mf[0].id = F_X + off, mf[0].name = nm[0][0], mf[0].read = 1, mf[0].write = 1;
    mf[1].id = F_VX + off, mf[1].name = nm[0][1], mf[1].read = 1, mf[1].write = 0;
    ff[0].id = F_TGT + off, ff[0].name = nm[1][0], ff[0].read = 1, ff[0].write = 0;
    ff[1].id = F_SHOTS + off, ff[1].name = nm[1][1], ff[1].read = 1, ff[1].write = 1;
    ff[2].id = F_LAST + off, ff[2].name = nm[1][2], ff[2].read = 0, ff[2].write = 1;
    g->move_cap = make_capsule("game.move", schema, "simulate", "", mf, 2);
    g->move_lir = make_move_lir(schema, pfx);
    g->fire_cap = make_capsule("game.fire", schema, "simulate", "\"game.target\"", ff, 3);
    g->fire_lir = make_fire_lir(schema, pfx);
    if (record_override != NULL) {
        g->record.p = malloc(record_override->n + 1u);
        memcpy(g->record.p, record_override->p, record_override->n + 1u);
        g->record.n = record_override->n;
    } else {
        rec_default(&ro, d);
        ro.bound = bound;
        g->record = make_record(&ro);
    }
    put(&g->fixture,
        "{\"fixture_format\":\"0.1\",\"schema_id\":\"%s\",\"schema_version\":1,\"tick\":0,\"entity_ids\":[", schema);
    for (i = 0; i < SHIPS; i++) {
        put(&g->fixture, "%s%u", i > 0u ? "," : "", (unsigned)(i + 1u));
    }
    put(&g->fixture, "],\"columns\":{");
    for (k = 0; k < sizeof cols / sizeof cols[0]; k++) {
        put(&g->fixture, "%s\"%s%s\":[", k > 0u ? "," : "", pfx, cols[k]);
        for (i = 0; i < SHIPS; i++) {
            uint32_t v = k == 0u   ? init_x(i)
                         : k == 1u ? init_vx(i)
                         : k == 2u ? init_team(i)
                         : k == 3u ? init_armed(i)
                                   : 0u;
            put(&g->fixture, "%s%u", i > 0u ? "," : "", (unsigned)v);
        }
        put(&g->fixture, "]");
    }
    put(&g->fixture, "}}");
    g->caps[0].capsule_json = (const uint8_t *)g->move_cap.p;
    g->caps[0].capsule_len = g->move_cap.n;
    g->caps[0].lir_json = (const uint8_t *)g->move_lir.p;
    g->caps[0].lir_len = g->move_lir.n;
    g->caps[1].capsule_json = (const uint8_t *)g->record.p; /* host operator record: lir_json NULL, lir_len 0 */
    g->caps[1].capsule_len = g->record.n;
    g->caps[2].capsule_json = (const uint8_t *)g->fire_cap.p;
    g->caps[2].capsule_len = g->fire_cap.n;
    g->caps[2].lir_json = (const uint8_t *)g->fire_lir.p;
    g->caps[2].lir_len = g->fire_lir.n;
    g->desc.struct_size = sizeof g->desc;
    g->desc.capsule_count = 3;
    g->desc.capsules = g->caps;
    g->desc.fixture_json = (const uint8_t *)g->fixture.p;
    g->desc.fixture_len = g->fixture.n;
    g->desc.max_entities = CAP;
    g->desc.tick_rate_hz = 60;
    g->desc.seed = 137;
}

static void game_free(Game *g) {
    free(g->move_cap.p);
    free(g->move_lir.p);
    free(g->fire_cap.p);
    free(g->fire_lir.p);
    free(g->record.p);
    free(g->fixture.p);
}

static SuHostWorld *game_world(const Game *g, int despawn) {
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t st = su_host_world_create(&g->desc, &w, &err);
    check(st == SU_OK, "world create: %s (%s)", su_host_status_name(st), err.message);
    if (st != SU_OK) {
        return NULL;
    }
    if (despawn) {
        uint64_t e = 0;
        check(su_host_entity_at(w, DEAD_SLOT, &e) == SU_OK && su_host_despawn(w, e, &err) == SU_OK, "despawn slot 3");
    }
    return w;
}

/* ---------- reference model ---------- */

typedef struct Ref {
    uint32_t col[22][CAP]; /* indexed by field id (offset removed) */
    uint8_t alive[CAP];
    uint32_t gen[CAP];
    uint64_t tick;
    uint32_t range;        /* NONE: unlimited */
} Ref;

static void ref_init(Ref *r) {
    uint32_t i;
    memset(r, 0, sizeof *r);
    r->range = NONE;
    for (i = 0; i < SHIPS; i++) {
        r->col[F_X][i] = init_x(i);
        r->col[F_VX][i] = init_vx(i);
        r->col[F_TEAM][i] = init_team(i);
        r->col[F_ARMED][i] = init_armed(i);
        r->alive[i] = 1u;
        r->gen[i] = 1u;
    }
}

static void ref_despawn(Ref *r, uint32_t s) {
    uint32_t f;
    r->alive[s] = 0u;
    r->gen[s]++;
    for (f = 0; f < 22u; f++) {
        r->col[f][s] = 0u;
    }
}

static void ref_step(Ref *r) {
    uint32_t i, j, f;
    for (i = 0; i < CAP; i++) { /* game.move */
        r->col[F_X][i] += r->col[F_VX][i];
    }
    for (i = 0; i < CAP; i++) { /* game.target */
        uint32_t best = NONE;
        int64_t bd = 0;
        r->col[F_TGT][i] = NONE;
        r->col[F_TGEN][i] = 0u;
        r->col[F_DIST][i] = 0u;
        r->col[F_DIST_HI][i] = 0u;
        r->col[F_MARK][i] = (r->alive[i] && (i & 1u) == 0u) ? 0xA500u + i : 0u;
        r->col[F_OPTICK][i] = (uint32_t)r->tick;
        if (!r->alive[i] || r->col[F_ARMED][i] == 0u) {
            continue;
        }
        for (j = 0; j < CAP; j++) {
            int64_t d = (int64_t)(int32_t)r->col[F_X][j] - (int64_t)(int32_t)r->col[F_X][i];
            if (!r->alive[j] || j == i || r->col[F_TEAM][j] == r->col[F_TEAM][i] ||
                (r->range != NONE && abs64(d) > (int64_t)r->range)) {
                continue;
            }
            if (best == NONE || abs64(d) < abs64(bd)) {
                best = j;
                bd = d;
            }
        }
        if (best != NONE) {
            r->col[F_TGT][i] = best;
            r->col[F_TGEN][i] = r->gen[best];
            r->col[F_DIST][i] = (uint32_t)((uint64_t)bd & 0xFFFFFFFFu);
            r->col[F_DIST_HI][i] = (uint32_t)((uint64_t)bd >> 32);
            r->col[F_SEEN][i]++;
        }
    }
    for (i = 0; i < CAP; i++) { /* game.fire, same tick */
        r->col[F_SHOTS][i] += r->col[F_TGT][i] != NONE ? 1u : 0u;
        r->col[F_LAST][i] = r->col[F_TGT][i];
    }
    for (i = 0; i < CAP; i++) { /* the world zeroes dead rows */
        if (!r->alive[i]) {
            for (f = 0; f < 22u; f++) {
                r->col[f][i] = 0u;
            }
        }
    }
    r->tick++;
}

static const uint32_t k_all_fields[] = {F_X,    F_VX,      F_TEAM, F_ARMED, F_TGT,    F_TGEN, F_DIST,
                                        F_DIST_HI, F_SEEN, F_MARK, F_OPTICK, F_SHOTS, F_LAST};

/* Every lane of every row equals the model. Returns the number of differing cells. */
static unsigned compare_ref(const SuHostWorld *w, const Ref *r, uint32_t off) {
    unsigned bad = 0;
    uint32_t k, i;
    for (k = 0; k < sizeof k_all_fields / sizeof k_all_fields[0]; k++) {
        uint32_t v[CAP];
        if (su_host_read(w, k_all_fields[k] + off, 0, CAP, v) != SU_OK) {
            return 1000u;
        }
        for (i = 0; i < CAP; i++) {
            if (v[i] != r->col[k_all_fields[k]][i]) {
                if (bad < 5u) {
                    fprintf(stderr, "  tick %llu field %u row %u: engine %u, model %u\n", (unsigned long long)r->tick,
                            (unsigned)k_all_fields[k], (unsigned)i, (unsigned)v[i],
                            (unsigned)r->col[k_all_fields[k]][i]);
                }
                bad++;
            }
        }
    }
    return bad;
}

static void digest_hex(const SuHostWorld *w, char hex[65]) {
    uint8_t d[32];
    check(su_host_digest(w, d) == SU_OK, "digest");
    su_host_hex(d, hex);
}

static uint8_t *snapshot(const SuHostWorld *w, size_t *len) {
    size_t n = 0, got = 0;
    uint8_t *b;
    check(su_host_snapshot_size(w, &n) == SU_OK, "snapshot size");
    b = malloc(n);
    check(b != NULL && su_host_snapshot_write(w, b, n, &got) == SU_OK && got == n, "snapshot write");
    *len = n;
    return b;
}

static int step1(SuHostWorld *w) {
    uint64_t done = 0;
    SuHostError err;
    uint32_t st = su_host_step(w, 1, &done, &err);
    check(st == SU_OK && done == 1u, "step: %s (%s)", su_host_status_name(st), err.message);
    return st == SU_OK;
}

/* ---------- 1. registration ---------- */

static uint32_t try_register(const SuHostOperatorDesc *d, SuHostError *err) {
    memset(err, 0, sizeof *err);
    return su_host_register_operator(d, err);
}

static void test_registration(const Game *g) {
    SuHostOperatorDesc d, bad;
    SuHostOperatorField f[C_COUNT];
    SuHostError err;
    SuHostWorld *w = NULL;
    uint32_t base = su_host_operator_count(), st;
    const char *sid = NULL, *name = NULL;

    /* Unregistered operator: the record is refused as before registration exists. */
    st = su_host_world_create(&g->desc, &w, &err);
    check(st == SU_E_CAPABILITY_DENIED && w == NULL && err.index == 1u && strstr(err.message, "game.target") != NULL,
          "unregistered record: %s (%s)", su_host_status_name(st), err.message);

    fleet_desc(&d, V_SAME);
    check(try_register(NULL, &err) == SU_E_INVALID_ARGUMENT, "NULL descriptor");
#define REFUSE(code, mutate, what)                                                                    \
    do {                                                                                              \
        bad = d;                                                                                      \
        memcpy(f, k_fields, sizeof f);                                                                \
        bad.fields = f;                                                                               \
        mutate;                                                                                       \
        st = try_register(&bad, &err);                                                                \
        check(st == (code), "%s: got %s (%s)", what, su_host_status_name(st), err.message);           \
    } while (0)
    REFUSE(SU_E_INVALID_ARGUMENT, bad.struct_size = 8u, "struct_size");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.flags = 0x100u, "unknown flag");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.flags = 0x2u, "bit 2 (a scratch-keeping mode was never released)");
    /* Identifiers only: quotes, control characters and non-ASCII bytes would make the hashed text ambiguous. */
    REFUSE(SU_E_INVALID_ARGUMENT, bad.system_id = "game.\"t\"", "system_id with a quote");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.schema_id = "game.fleet\",\"x", "schema_id with a quote");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.schema_id = "game.fl\teet", "schema_id with a control character");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.schema_id = "game.fl\xC3\xA9" "et", "schema_id with non-ASCII bytes");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.operator_name = "fleet\"ops", "operator_name with a quote");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.operator_name = "fleet\x01ops", "operator_name with a control character");
    REFUSE(SU_E_INVALID_ARGUMENT, f[0].name = "x\",\"type\":\"u32", "field name with quotes");
    check(err.index == 0u, "field name error names field 0 (%u)", (unsigned)err.index);
    REFUSE(SU_E_INVALID_ARGUMENT, f[1].name = "te\nam", "field name with a newline");
    REFUSE(SU_E_INVALID_ARGUMENT, f[2].name = "arm\xE2\x82\xAC" "ed", "field name with non-ASCII bytes");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.run = NULL, "NULL run");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.parse_data = NULL, "release_data without parse_data");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.system_id = "game target", "system_id with a space");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.system_id = "", "empty system_id");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.system_id = ".", "system_id without a letter or digit (.)");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.system_id = "-", "system_id without a letter or digit (-)");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.system_id = "..", "system_id without a letter or digit (..)");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.operator_name = "_-_", "operator_name without a letter or digit");
    REFUSE(SU_E_INVALID_ARGUMENT, f[0].name = ".", "field name without a letter or digit");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.schema_id = NULL, "NULL schema_id");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.operator_name = "fleet ops", "operator_name with a space");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.operator_version = 0u, "operator_version 0");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.field_count = 0u, "no fields");
    REFUSE(SU_E_INVALID_ARGUMENT, f[2].id = 1u, "ids not ascending");
    check(err.index == 2u, "ids not ascending: err->index %u", (unsigned)err.index);
    REFUSE(SU_E_INVALID_ARGUMENT, f[0].type = SU_TYPE_BOOL, "bool lane");
    REFUSE(SU_E_INVALID_ARGUMENT, f[0].access = 0u, "access 0");
    REFUSE(SU_E_INVALID_ARGUMENT, f[0].reserved = 1u, "reserved field word");
    REFUSE(SU_E_INVALID_ARGUMENT, f[1].name = "x", "duplicate field name");
    REFUSE(SU_E_INVALID_ARGUMENT, f[6].name = "tgt_far.hi", "64-bit lanes with different stems");
    REFUSE(SU_E_INVALID_ARGUMENT, f[6].access = SU_ACCESS_READ, "64-bit lanes with different access");
    REFUSE(SU_E_INVALID_ARGUMENT, bad.field_count = 6u, "64-bit field without its high lane");
    REFUSE(SU_E_DUPLICATE_KEY, bad.system_id = "space3d.query", "built-in operator ID");
    check(strstr(err.message, "built into") != NULL, "built-in message: %s", err.message);
    REFUSE(SU_E_DUPLICATE_KEY, bad.system_id = "rts.spatial", "reserved rts prefix (engine operator of the worker)");
    REFUSE(SU_E_DUPLICATE_KEY, bad.system_id = "space3d.future", "reserved space3d prefix");
    REFUSE(SU_E_DUPLICATE_KEY, bad.system_id = "sutekh.x", "reserved sutekh prefix");
    REFUSE(SU_E_DUPLICATE_KEY, bad.system_id = "RTS.spatial", "reserved prefix in upper case");
    REFUSE(SU_E_DUPLICATE_KEY, bad.system_id = "Space3D.query", "reserved prefix in mixed case");
    REFUSE(SU_E_DUPLICATE_KEY, bad.operator_name = "space3d_ops_v1", "a built-in operator name");
    REFUSE(SU_E_DUPLICATE_KEY, bad.operator_name = "RTS_OPS_v1", "a reserved operator family name");
    REFUSE(SU_E_DUPLICATE_KEY, bad.operator_name = "space3d-ops_v1", "a reserved name with '-' for '_'");
    REFUSE(SU_E_DUPLICATE_KEY, bad.operator_name = "Rts-Ops-v7", "a reserved name, mixed case and '-'");
#undef REFUSE
    check(su_host_operator_count() == base, "refusals register nothing");

    st = try_register(&d, &err);
    check(st == SU_OK, "register game.target: %s (%s)", su_host_status_name(st), err.message);
    check(su_host_operator_count() == base + 1u, "listing grows by one");
    check(su_host_operator_info(base, &sid, &name) == SU_OK && strcmp(sid, "game.target") == 0 &&
              strcmp(name, "fleet_ops_v1") == 0,
          "listing shows the registered operator after the built-in ones");
    check(su_host_operator_info(base + 1u, &sid, &name) == SU_E_INVALID_ARGUMENT, "listing ends");
    /* Same ID again (even with another table): refused. */
    bad = d;
    bad.operator_version = 9u;
    st = try_register(&bad, &err);
    check(st == SU_E_DUPLICATE_KEY && strstr(err.message, "already registered") != NULL, "duplicate ID: %s (%s)",
          su_host_status_name(st), err.message);
    bad = d;
    bad.system_id = "GAME.Target";
    st = try_register(&bad, &err);
    check(st == SU_E_DUPLICATE_KEY && strstr(err.message, "game.target") != NULL,
          "duplicate ID in another letter case: %s (%s)", su_host_status_name(st), err.message);
    check(su_host_operator_count() == base + 1u, "duplicate registers nothing");
}

/* ---------- 2. order, same-tick consumption, contract ---------- */

static void test_run_against_model(const Game *g) {
    SuHostWorld *w = game_world(g, 1);
    Ref r;
    SuHostInput in;
    SuHostError err;
    uint64_t e = 0;
    unsigned t, bad = 0, calls0 = g_stats.calls;
    const char *sid = NULL, *sem = NULL;
    uint32_t tier = 0;
    int64_t dist[CAP];
    if (w == NULL) {
        return;
    }
    ref_init(&r);
    ref_despawn(&r, DEAD_SLOT);
    check(su_host_system_count(w) == 3u && su_host_system_info(w, 1, &sid, &sem) == SU_OK &&
              strcmp(sid, "game.target") == 0 && strlen(sem) == 64u,
          "the operator is schedule entry 1");
    check(su_host_system_tier(w, 1, &tier) == SU_OK && tier == SU_HOST_TIER_OPERATOR, "tier OPERATOR");
    check(su_host_system_set_tier(w, 1, SU_HOST_TIER_REFERENCE, &err) == SU_E_INVALID_ARGUMENT, "tier fixed");
    for (t = 0; t < 40u; t++) {
        if (t == 5u) {
            /* An input into the write-only lane `mark` of odd slot 1: the operator must not see it (zero prefill)
             * and the committed result is 0. An input into the read-write lane `seen` is visible to it. */
            check(su_host_entity_at(w, 1, &e) == SU_OK, "entity 1");
            in.entity = e, in.field_id = F_MARK, in.value = 77u;
            check(su_host_input_push(w, 5, &in, 1, &err) == SU_OK, "push mark");
            check(su_host_entity_at(w, 0, &e) == SU_OK, "entity 0");
            in.entity = e, in.field_id = F_SEEN, in.value = 1000u;
            check(su_host_input_push(w, 5, &in, 1, &err) == SU_OK, "push seen");
            r.col[F_MARK][1] = 77u;
            r.col[F_SEEN][0] = 1000u;
        }
        if (!step1(w)) {
            break;
        }
        ref_step(&r);
        bad += compare_ref(w, &r, 0u);
    }
    check(bad == 0u, "40 ticks: every lane equals the reference model (%u cells differ)", bad);
    check(g_stats.calls - calls0 == 40u, "run once per tick (%u)", g_stats.calls - calls0);
    check(g_stats.last_tick == 39u, "run->tick is the committed tick the step advances from (%llu)",
          (unsigned long long)g_stats.last_tick);
    check(g_stats.contract_bad == 0u, "run contract (columns, prefill, row range) held (%u)", g_stats.contract_bad);
    check(g_stats.scratch_dirty == 0u, "scratch zeroed before every call");
    check(su_host_read_i64(w, F_DIST, 0, CAP, dist) == SU_OK, "read i64 distance");
    {
        uint32_t i, nonzero = 0, targets = 0;
        for (i = 0; i < CAP; i++) {
            nonzero += dist[i] != 0 ? 1u : 0u;
            targets += r.col[F_TGT][i] != NONE ? 1u : 0u;
            check(dist[i] == (int64_t)((uint64_t)r.col[F_DIST_HI][i] << 32 | r.col[F_DIST][i]), "i64 row %u", i);
        }
        check(targets >= 6u && nonzero >= 4u, "scenario exercises targeting (%u targets, %u nonzero distances)",
              targets, nonzero);
    }
    su_host_world_destroy(w);
}

/* ---------- 3. snapshots, digests, rollback ---------- */

static void test_rollback(const Game *g) {
    SuHostWorld *w = game_world(g, 1);
    SuHostWorld *w2;
    char hex[41][65], again[65];
    uint64_t fast[41], f2 = 0;
    uint8_t *snap;
    size_t len;
    SuHostError err;
    unsigned t, pass, diff = 0;
    if (w == NULL) {
        return;
    }
    for (t = 0; t < 10u; t++) {
        step1(w);
    }
    snap = snapshot(w, &len);
    digest_hex(w, hex[10]);
    check(su_host_digest_fast(w, &fast[10]) == SU_OK, "fast digest");
    for (t = 11; t <= 40u; t++) {
        step1(w);
        digest_hex(w, hex[t]);
        su_host_digest_fast(w, &fast[t]);
    }
    for (pass = 0; pass < 2u; pass++) {
        uint32_t st = pass == 0u ? su_host_snapshot_restore(w, snap, len, &err)
                                 : su_host_snapshot_restore_inplace(w, snap, len, &err);
        check(st == SU_OK && su_host_tick(w) == 10u, "restore %u: %s (%s)", pass, su_host_status_name(st),
              err.message);
        for (t = 11; t <= 40u; t++) {
            step1(w);
            digest_hex(w, again);
            su_host_digest_fast(w, &f2);
            diff += strcmp(again, hex[t]) != 0 || f2 != fast[t];
        }
    }
    check(diff == 0u, "re-simulation after both restores equals the first run every tick (%u differ)", diff);
    /* A fresh world restored from the snapshot reaches the same end state. */
    w2 = game_world(g, 0);
    if (w2 != NULL) {
        check(su_host_snapshot_restore(w2, snap, len, &err) == SU_OK, "restore into a fresh world: %s", err.message);
        for (t = 11; t <= 40u; t++) {
            step1(w2);
        }
        digest_hex(w2, again);
        check(strcmp(again, hex[40]) == 0, "fresh world reaches the same digest");
        su_host_world_destroy(w2);
    }
    free(snap);
    su_host_world_destroy(w);
}

/* ---------- 4. replay ---------- */

static uint32_t record_replay(SuHostWorld *w, uint64_t ticks, uint8_t **bytes, size_t *len) {
    SuReplayOptions o;
    SuReplayWriter *rw = NULL;
    SuHostError err;
    uint64_t done = 0;
    uint32_t st;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.keyframe_interval = 16;
    o.digest_interval = 1;
    st = su_replay_writer_begin(w, &o, &rw, &err);
    if (st == SU_OK) {
        st = su_replay_writer_step(rw, w, ticks, &done, &err);
    }
    if (st == SU_OK) {
        st = su_replay_writer_finish(rw, w, bytes, len);
    }
    check(st == SU_OK && done == ticks, "replay recording: %s (%s)", su_host_status_name(st), err.message);
    su_replay_writer_destroy(rw);
    return st;
}

static void test_replay(const Game *g) {
    SuHostWorld *w = game_world(g, 1), *w2;
    uint8_t *bytes = NULL;
    size_t len = 0;
    SuReplay *rp = NULL;
    SuHostError err;
    SuHostInput in;
    uint64_t e = 0, done = 0;
    char a[65], b[65];
    uint32_t st;
    if (w == NULL) {
        return;
    }
    check(su_host_entity_at(w, 2, &e) == SU_OK, "entity 2");
    in.entity = e, in.field_id = F_VX, in.value = 9u; /* an input mid-run, recorded in the replay */
    check(su_host_input_push(w, 20, &in, 1, &err) == SU_OK, "push vx");
    if (record_replay(w, 60, &bytes, &len) != SU_OK) {
        su_host_world_destroy(w);
        return;
    }
    digest_hex(w, a);
    check(su_replay_open(bytes, len, &rp, &err) == SU_OK, "replay open: %s", err.message);
    w2 = game_world(g, 0);
    if (rp != NULL && w2 != NULL) {
        st = su_replay_seek(rp, w2, 33, &err);
        check(st == SU_OK && su_host_tick(w2) == 33u, "seek 33: %s (%s)", su_host_status_name(st), err.message);
        st = su_replay_play(rp, w2, 100, &done, &err);
        check(st == SU_OK && done == 27u, "play to the end: %s (%s)", su_host_status_name(st), err.message);
        digest_hex(w2, b);
        check(strcmp(a, b) == 0, "replayed end state equals the recorded run");
    }
    su_replay_close(rp);
    su_replay_free(bytes);
    su_host_world_destroy(w2);
    su_host_world_destroy(w);
}

/* ---------- 5. schema-relative binding ---------- */

static void test_bound(const Game *g, const SuHostOperatorDesc *d) {
    Game gb;
    SuHostWorld *w, *wb;
    uint8_t ia[32], ib[32];
    unsigned t, diff = 0;
    uint32_t k;
    game_build(&gb, d, 1, NULL);
    w = game_world(g, 1);
    wb = game_world(&gb, 1);
    if (w != NULL && wb != NULL) {
        for (t = 0; t < 25u; t++) {
            step1(w);
            step1(wb);
        }
        for (k = 0; k < sizeof k_all_fields / sizeof k_all_fields[0]; k++) {
            uint32_t va[CAP], vb[CAP];
            check(su_host_read(w, k_all_fields[k], 0, CAP, va) == SU_OK &&
                      su_host_read(wb, k_all_fields[k] + 100u, 0, CAP, vb) == SU_OK,
                  "read lane %u", (unsigned)k_all_fields[k]);
            diff += memcmp(va, vb, sizeof va) != 0;
        }
        check(diff == 0u, "bound record (game IDs + 100): every lane equals the unbound world (%u differ)", diff);
        su_host_world_identity(w, ia);
        su_host_world_identity(wb, ib);
        check(memcmp(ia, ib, 32) != 0, "the binding is part of the world identity");
    }
    su_host_world_destroy(w);
    su_host_world_destroy(wb);
    game_free(&gb);
}

/* ---------- 6. negative cases ---------- */

static void expect_create(const SuHostOperatorDesc *d, const RecOpt *ro, uint32_t code, const char *what) {
    Game g;
    Buf rec = make_record(ro);
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t st;
    game_build(&g, d, 0, &rec);
    st = su_host_world_create(&g.desc, &w, &err);
    check(st == code && w == NULL, "%s: got %s (%s)", what, su_host_status_name(st), err.message);
    su_host_world_destroy(w);
    game_free(&g);
    free(rec.p);
}

/* Bad operators: a second, small registration on schema game.fleet that writes into lanes it may only read. */
static int g_bad_mode;
static uint32_t bad_run(const SuHostOperatorRun *r, void *user) {
    (void)user;
    if (g_bad_mode == 1) {
        ((uint32_t *)r->columns[0].in)[2] ^= 1u; /* read-only lane x, through a cast */
    } else if (g_bad_mode == 2) {
        ((uint32_t *)r->columns[1].in)[0] += 5u; /* the input view of read-write lane seen */
    } else if (g_bad_mode == 3) {
        if (r->reason == NULL || r->reason_cap < 16u || r->reason[0] != '\0') {
            return SU_E_INTERNAL;
        }
        snprintf(r->reason, r->reason_cap, "magazine empty in row %u", 2u);
        return SU_E_RESOURCE_LIMIT; /* the operator's own failure, with a reason */
    }
    return SU_OK;
}

static const SuHostOperatorField k_bad_fields[2] = {
    {F_X, SU_TYPE_U32, SU_ACCESS_READ, 0, "x"},
    {F_SEEN, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, 0, "seen"},
};

static uint32_t refuse_scratch(uint64_t capacity, size_t *bytes, void *user) {
    (void)user;
    *bytes = 0;
    return capacity > 8u ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static void bad_desc(SuHostOperatorDesc *b, const char *sid, uint32_t flags) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(b);
    b->flags = flags;
    b->system_id = sid;
    b->schema_id = "game.fleet";
    b->operator_name = "fleet_bad_v1";
    b->operator_version = 1;
    b->field_count = 2;
    b->fields = k_bad_fields;
    for (i = 0; i < 32u; i++) {
        b->constants_hash[i] = (uint8_t)(0xB0u + i);
    }
    b->run = bad_run;
}

/* World game.move + <sid> (the bad operator in place of game.target) + nothing else. */
static SuHostWorld *bad_world(const Game *g, const SuHostOperatorDesc *b) {
    SuHostWorldDesc d = g->desc;
    SuHostCapsule caps[2];
    RecOpt ro;
    Buf rec, fx = {NULL, 0, 0};
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t st, i;
    rec_default(&ro, b);
    ro.profile = "u32_mod_v1";
    rec = make_record(&ro);
    put(&fx, "{\"fixture_format\":\"0.1\",\"schema_id\":\"game.fleet\",\"schema_version\":1,\"tick\":0,"
             "\"entity_ids\":[1,2,3,4],\"columns\":{\"x\":[1,2,3,4],\"vx\":[1,1,1,1],\"seen\":[0,0,0,0]}}");
    caps[0] = g->caps[0];
    caps[1].capsule_json = (const uint8_t *)rec.p;
    caps[1].capsule_len = rec.n;
    caps[1].lir_json = NULL;
    caps[1].lir_len = 0;
    d.capsules = caps;
    d.capsule_count = 2;
    d.fixture_json = (const uint8_t *)fx.p;
    d.fixture_len = fx.n;
    d.max_entities = 0;
    st = su_host_world_create(&d, &w, &err);
    check(st == SU_OK, "bad-operator world: %s (%s)", su_host_status_name(st), err.message);
    (void)i;
    free(rec.p);
    free(fx.p);
    return w;
}

static void test_negative(const Game *g, const SuHostOperatorDesc *d) {
    RecOpt ro;
    SuHostOperatorDesc b, t, s;
    SuHostWorld *w;
    SuHostError err;
    uint64_t done = 9;
    uint32_t st;
    char before[65], after[65];

    /* Mismatched records. */
    rec_default(&ro, d), ro.operator_name = "fleet_ops_v2";
    expect_create(d, &ro, SU_E_SCHEMA_MISMATCH, "record with another operator name");
    rec_default(&ro, d), ro.version_delta = 1;
    expect_create(d, &ro, SU_E_SCHEMA_MISMATCH, "record with another operator_version");
    rec_default(&ro, d), ro.flip_hash = 1;
    expect_create(d, &ro, SU_E_SCHEMA_MISMATCH, "record with another constants_hash");
    rec_default(&ro, d), ro.access_flip_field = C_TEAM;
    expect_create(d, &ro, SU_E_SCHEMA_MISMATCH, "record declaring a read lane as a write");
    rec_default(&ro, d), ro.access_flip_field = C_TGT;
    expect_create(d, &ro, SU_E_SCHEMA_MISMATCH, "record declaring a write lane as a read");
    rec_default(&ro, d), ro.rename_field = C_SEEN;
    expect_create(d, &ro, SU_E_SCHEMA_MISMATCH, "record renaming a lane");
    rec_default(&ro, d), ro.drop_last = 1;
    expect_create(d, &ro, SU_E_SCHEMA_MISMATCH, "record missing a lane");
    rec_default(&ro, d), ro.profile = "u32_mod_v1";
    expect_create(d, &ro, SU_E_TYPE, "record with the wrong numeric profile");

    /* An IR system that disagrees with a declared lane (same ID, other name): the union table refuses it. */
    {
        Game g2;
        IrField ff[3] = {{F_TGT, "target", 1, 0}, {F_SHOTS, "shots", 1, 1}, {F_LAST, "last_tgt", 0, 1}};
        SuHostWorld *w2 = NULL;
        Buf lir = {NULL, 0, 0};
        game_build(&g2, d, 0, NULL);
        free(g2.fire_cap.p);
        free(g2.fire_lir.p);
        g2.fire_cap = make_capsule("game.fire", "game.fleet", "simulate", "\"game.target\"", ff, 3);
        put(&lir, "{\"ir_version\":\"0.1\",\"system_id\":\"game.fire\",\"numeric_profile\":\"u32_mod_v1\","
                  "\"query\":\"game.fleet\",\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"t\",\"field\":\"target\"},"
                  "{\"op\":\"load_u32\",\"dst\":\"s\",\"field\":\"shots\"},"
                  "{\"op\":\"add_u32\",\"dst\":\"s2\",\"a\":\"s\","
                  "\"b\":\"t\"},{\"op\":\"store_u32\",\"field\":\"shots\",\"src\":\"s2\"},{\"op\":\"store_u32\","
                  "\"field\":\"last_tgt\",\"src\":\"t\"}]}");
        g2.fire_lir = lir;
        g2.caps[2].capsule_json = (const uint8_t *)g2.fire_cap.p;
        g2.caps[2].capsule_len = g2.fire_cap.n;
        g2.caps[2].lir_json = (const uint8_t *)g2.fire_lir.p;
        g2.caps[2].lir_len = g2.fire_lir.n;
        st = su_host_world_create(&g2.desc, &w2, &err);
        check(st == SU_E_SCHEMA_MISMATCH && strstr(err.message, "game.target") != NULL,
              "IR lane disagreeing with the operator's declaration: %s (%s)", su_host_status_name(st), err.message);
        su_host_world_destroy(w2);
        game_free(&g2);
    }

    /* Undeclared writes, detected after run: the tick fails, nothing is committed. */
    bad_desc(&b, "game.bad", 0u);
    st = su_host_register_operator(&b, &err);
    check(st == SU_OK, "register game.bad: %s (%s)", su_host_status_name(st), err.message);
    w = bad_world(g, &b);
    if (w != NULL) {
        int mode;
        for (mode = 1; mode <= 3; mode++) {
            g_bad_mode = 0;
            step1(w);
            digest_hex(w, before);
            g_bad_mode = mode;
            st = su_host_step(w, 1, &done, &err);
            digest_hex(w, after);
            if (mode < 3) {
                check(st == SU_E_UNDECLARED_WRITE && err.index == 1u && done == 0u &&
                          strstr(err.message, "may only read") != NULL &&
                          strstr(err.message, mode == 1 ? "lane x" : "lane seen") != NULL,
                      "undeclared write (mode %d) detected: %s (%s)", mode, su_host_status_name(st), err.message);
            } else {
                check(st == SU_E_RESOURCE_LIMIT && err.index == 1u && strstr(err.message, "SU_E_RESOURCE_LIMIT") != NULL &&
                          strstr(err.message, "magazine empty in row 2") != NULL,
                      "failing run: %s (%s)", su_host_status_name(st), err.message);
            }
            check(strcmp(before, after) == 0 && su_host_tick(w) == (uint64_t)mode, "failed tick commits nothing");
        }
        g_bad_mode = 0;
        check(step1(w), "the world steps again after the failures");
        su_host_world_destroy(w);
    }
    /* The same write under SU_HOST_OPERATOR_TRUST_INPUTS is not detected (the flag removes the check). */
    bad_desc(&t, "game.bad_trusted", SU_HOST_OPERATOR_TRUST_INPUTS);
    check(su_host_register_operator(&t, &err) == SU_OK, "register game.bad_trusted");
    w = bad_world(g, &t);
    if (w != NULL) {
        g_bad_mode = 1;
        st = su_host_step(w, 1, &done, &err);
        check(st == SU_OK, "trusted inputs: the corrupting write is not detected (%s)", su_host_status_name(st));
        g_bad_mode = 0;
        su_host_world_destroy(w);
    }
    /* A scratch size the operator refuses refuses the world. */
    bad_desc(&s, "game.bad_scratch", 0u);
    s.scratch_size = refuse_scratch;
    check(su_host_register_operator(&s, &err) == SU_OK, "register game.bad_scratch");
    {
        SuHostWorldDesc dd;
        SuHostCapsule caps[2];
        RecOpt r2;
        Buf rec;
        SuHostWorld *w2 = NULL;
        rec_default(&r2, &s);
        r2.profile = "u32_mod_v1";
        rec = make_record(&r2);
        caps[0] = g->caps[0];
        caps[1].capsule_json = (const uint8_t *)rec.p;
        caps[1].capsule_len = rec.n;
        caps[1].lir_json = NULL;
        caps[1].lir_len = 0;
        dd = g->desc;
        dd.capsules = caps;
        dd.capsule_count = 2;
        dd.fixture_json = NULL;
        dd.fixture_len = 0;
        dd.max_entities = 64;
        st = su_host_world_create(&dd, &w2, &err);
        check(st == SU_E_RESOURCE_LIMIT && w2 == NULL && err.index == 1u, "refused scratch size: %s (%s)",
              su_host_status_name(st), err.message);
        su_host_world_destroy(w2);
        dd.max_entities = 8;
        st = su_host_world_create(&dd, &w2, &err);
        check(st == SU_OK, "accepted scratch size: %s (%s)", su_host_status_name(st), err.message);
        su_host_world_destroy(w2);
        free(rec.p);
    }
}

/* ---------- 7. operator data ---------- */

static void test_data(const Game *g) {
    SuHostWorld *w = game_world(g, 1), *w0 = game_world(g, 1);
    static const uint8_t good[8] = {'R', 'N', 'G', '1', 20, 0, 0, 0};
    static const uint8_t junk[5] = {'n', 'o', 'p', 'e', 0};
    uint8_t i0[32], i1[32];
    SuHostError err;
    Ref r;
    unsigned t, bad = 0, rel0 = g_releases;
    uint32_t st;
    if (w == NULL || w0 == NULL) {
        su_host_world_destroy(w);
        su_host_world_destroy(w0);
        return;
    }
    su_host_world_identity(w, i0);
    st = su_host_set_operator_data(w, "game.target", junk, sizeof junk, &err);
    check(st == SU_E_PARSE && strstr(err.message, "RNG1") != NULL, "malformed data refused: %s (%s)",
          su_host_status_name(st), err.message);
    su_host_world_identity(w, i1);
    check(memcmp(i0, i1, 32) == 0, "a refused data set leaves the identity");
    check(su_host_set_operator_data(w, "game.move", good, sizeof good, &err) == SU_E_INVALID_ARGUMENT,
          "an IR system takes no data");
    st = su_host_set_operator_data(w, NULL, good, sizeof good, &err);
    check(st == SU_OK, "data for every operator that takes it: %s (%s)", su_host_status_name(st), err.message);
    su_host_world_identity(w, i1);
    check(memcmp(i0, i1, 32) != 0, "data joins the world identity");
    check(su_host_set_operator_data(w, "game.target", good, sizeof good, &err) == SU_E_INVALID_ARGUMENT,
          "data only once");
    ref_init(&r);
    ref_despawn(&r, DEAD_SLOT);
    r.range = 20u;
    for (t = 0; t < 30u; t++) {
        step1(w);
        step1(w0);
        ref_step(&r);
        bad += compare_ref(w, &r, 0u);
    }
    check(bad == 0u, "range-limited targeting equals the model (%u cells differ)", bad);
    {
        uint32_t a[CAP], b[CAP];
        su_host_read(w, F_TGT, 0, CAP, a);
        su_host_read(w0, F_TGT, 0, CAP, b);
        check(memcmp(a, b, sizeof a) != 0, "the data changes the results");
    }
    check(su_host_set_operator_data(w0, NULL, good, sizeof good, &err) == SU_E_INVALID_ARGUMENT,
          "no data after the first step");
    check(g_releases == rel0, "parsed data is held while the world lives");
    su_host_world_destroy(w);
    check(g_releases == rel0 + 1u, "parsed data released with the world");
    su_host_world_destroy(w0);
}

/* ---------- 8. no allocation in the tick ---------- */

static void test_no_alloc(const Game *g) {
#ifdef SU_TEST_WRAP_ALLOC
    SuHostWorld *w = game_world(g, 1);
    uint64_t done = 0;
    SuHostError err;
    if (w == NULL) {
        return;
    }
    step1(w); /* any lazy first-tick setup outside the measurement */
    g_allocs = 0;
    g_count_alloc = 1;
    su_host_step(w, 50, &done, &err);
    g_count_alloc = 0;
    check(done == 50u && g_allocs == 0u, "50 ticks with the registered operator allocate nothing (%lu allocations)",
          g_allocs);
    su_host_world_destroy(w);
#else
    (void)g;
    printf("note: allocation counting not built in (not the static Linux variant)\n");
#endif
}

/* ---------- 9. two threads ---------- */

typedef struct ThreadArg {
    const Game *g;
    char hex[65];
} ThreadArg;

static void thread_body(ThreadArg *a) {
    SuHostWorld *w = NULL;
    SuHostError err;
    uint64_t done = 0;
    a->hex[0] = '\0';
    if (su_host_world_create(&a->g->desc, &w, &err) != SU_OK) {
        return;
    }
    if (su_host_step(w, 200, &done, &err) == SU_OK && done == 200u) {
        uint8_t d[32];
        su_host_digest(w, d);
        su_host_hex(d, a->hex);
    }
    su_host_world_destroy(w);
}

#if defined(_WIN32)
static unsigned __stdcall thread_main(void *p) {
    thread_body((ThreadArg *)p);
    return 0;
}
#else
static void *thread_main(void *p) {
    thread_body((ThreadArg *)p);
    return NULL;
}
#endif

static void test_threads(const Game *g) {
    ThreadArg serial, a, b;
    serial.g = a.g = b.g = g;
    thread_body(&serial);
#if defined(_WIN32)
    {
        HANDLE ha = (HANDLE)_beginthreadex(NULL, 0, thread_main, &a, 0, NULL);
        HANDLE hb = (HANDLE)_beginthreadex(NULL, 0, thread_main, &b, 0, NULL);
        WaitForSingleObject(ha, INFINITE);
        WaitForSingleObject(hb, INFINITE);
        CloseHandle(ha);
        CloseHandle(hb);
    }
#else
    {
        pthread_t ta, tb;
        int ca = pthread_create(&ta, NULL, thread_main, &a) == 0;
        int cb = pthread_create(&tb, NULL, thread_main, &b) == 0;
        check(ca && cb, "threads");
        if (ca) {
            pthread_join(ta, NULL);
        }
        if (cb) {
            pthread_join(tb, NULL);
        }
    }
#endif
    check(serial.hex[0] != '\0' && strcmp(serial.hex, a.hex) == 0 && strcmp(serial.hex, b.hex) == 0,
          "two worlds on two threads equal the serial run");
}

/* ---------- 10. descriptor versioning, concurrent registration, registry limit ---------- */

/* struct_size: the v1 size; a larger descriptor whose unknown tail is zero; a nonzero tail is refused. */
static void test_desc_versions(void) {
    struct {
        SuHostOperatorDesc d;
        uint8_t tail[40];
    } big;
    SuHostOperatorDesc d;
    SuHostError err;
    uint32_t st;
    check(SU_HOST_OPERATOR_DESC_V1_SIZE <= sizeof(SuHostOperatorDesc), "v1 size within the descriptor");
    check(SU_HOST_OPERATOR_DESC_V1_SIZE == (sizeof(void *) == 8u ? 120u : 84u), "v1 size pinned (%u)",
          (unsigned)SU_HOST_OPERATOR_DESC_V1_SIZE);
    bad_desc(&d, "game.v1size", 0u);
    d.struct_size = (uint32_t)SU_HOST_OPERATOR_DESC_V1_SIZE;
    st = su_host_register_operator(&d, &err);
    check(st == SU_OK, "v1-sized descriptor accepted: %s (%s)", su_host_status_name(st), err.message);
    memset(&big, 0, sizeof big);
    bad_desc(&big.d, "game.bigdesc", 0u);
    big.d.struct_size = (uint32_t)sizeof big;
    st = su_host_register_operator(&big.d, &err);
    check(st == SU_OK, "larger descriptor with a zero tail accepted: %s (%s)", su_host_status_name(st), err.message);
    bad_desc(&big.d, "game.bigdesc_nonzero", 0u);
    big.d.struct_size = (uint32_t)sizeof big;
    big.tail[17] = 1u;
    st = su_host_register_operator(&big.d, &err);
    check(st == SU_E_INVALID_ARGUMENT && strstr(err.message, "does not know") != NULL,
          "larger descriptor with a nonzero unknown field refused: %s (%s)", su_host_status_name(st), err.message);
    bad_desc(&d, "game.small", 0u);
    d.struct_size = (uint32_t)SU_HOST_OPERATOR_DESC_V1_SIZE - 8u;
    st = su_host_register_operator(&d, &err);
    check(st == SU_E_INVALID_ARGUMENT, "descriptor below the v1 size refused: %s", su_host_status_name(st));
}

#define REG_THREAD_OPS 8u

typedef struct RegThread {
    unsigned id;
    unsigned ok;
} RegThread;

static void reg_thread_body(RegThread *a) {
    unsigned i;
    a->ok = 0;
    for (i = 0; i < REG_THREAD_OPS; i++) {
        SuHostOperatorDesc d;
        SuHostError err;
        char sid[64];
        snprintf(sid, sizeof sid, "game.concurrent.%u.%u", a->id, i);
        bad_desc(&d, sid, 0u);
        if (su_host_register_operator(&d, &err) == SU_OK) {
            a->ok++;
        }
        (void)su_host_operator_count(); /* lookups interleaved with registrations */
    }
}

#if defined(_WIN32)
static unsigned __stdcall reg_thread_main(void *p) {
    reg_thread_body((RegThread *)p);
    return 0;
}
#else
static void *reg_thread_main(void *p) {
    reg_thread_body((RegThread *)p);
    return NULL;
}
#endif

static void test_registry_limits(void) {
    RegThread a, b;
    SuHostOperatorDesc d;
    SuHostError err;
    uint32_t before = su_host_operator_count(), i, found = 0, st = SU_OK, registered = 0;
    a.id = 1u, b.id = 2u;
#if defined(_WIN32)
    {
        HANDLE ha = (HANDLE)_beginthreadex(NULL, 0, reg_thread_main, &a, 0, NULL);
        HANDLE hb = (HANDLE)_beginthreadex(NULL, 0, reg_thread_main, &b, 0, NULL);
        WaitForSingleObject(ha, INFINITE);
        WaitForSingleObject(hb, INFINITE);
        CloseHandle(ha);
        CloseHandle(hb);
    }
#else
    {
        pthread_t ta, tb;
        int ca = pthread_create(&ta, NULL, reg_thread_main, &a) == 0;
        int cb = pthread_create(&tb, NULL, reg_thread_main, &b) == 0;
        check(ca && cb, "registration threads");
        if (ca) {
            pthread_join(ta, NULL);
        }
        if (cb) {
            pthread_join(tb, NULL);
        }
    }
#endif
    check(a.ok == REG_THREAD_OPS && b.ok == REG_THREAD_OPS && su_host_operator_count() == before + 2u * REG_THREAD_OPS,
          "concurrent registrations from two threads all land (%u + %u)", a.ok, b.ok);
    for (i = before; i < su_host_operator_count(); i++) {
        const char *sid = NULL;
        if (su_host_operator_info(i, &sid, NULL) == SU_OK && strncmp(sid, "game.concurrent.", 16) == 0) {
            found++;
        }
    }
    check(found == 2u * REG_THREAD_OPS, "each concurrent registration is listed once (%u)", found);
    /* Fill the registry: the entry after the last is SU_E_RESOURCE_LIMIT and changes nothing. */
    for (i = 0; i < 2u * SU_HOST_MAX_REGISTERED_OPERATORS && st == SU_OK; i++) {
        char sid[64];
        snprintf(sid, sizeof sid, "game.fill.%u", (unsigned)i);
        bad_desc(&d, sid, 0u);
        st = su_host_register_operator(&d, &err);
        registered += st == SU_OK ? 1u : 0u;
    }
    check(st == SU_E_RESOURCE_LIMIT && strstr(err.message, "full") != NULL, "a full registry: %s (%s)",
          su_host_status_name(st), err.message);
    check(su_host_operator_count() - before == 2u * REG_THREAD_OPS + registered &&
              registered < SU_HOST_MAX_REGISTERED_OPERATORS,
          "the registry holds at most SU_HOST_MAX_REGISTERED_OPERATORS operators (%u more fitted)", registered);
}

/* ---------- cross-process modes ---------- */

static uint8_t *read_file(const char *dir, const char *name, size_t *len) {
    char path[1024];
    FILE *f;
    long n;
    uint8_t *b;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    *len = 0;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        if (f != NULL) {
            fclose(f);
        }
        return NULL;
    }
    b = malloc((size_t)n + 1u);
    if (b != NULL && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int write_file(const char *dir, const char *name, const void *p, size_t n) {
    char path[1024];
    FILE *f;
    int ok;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static int mode_record(const char *dir) {
    SuHostOperatorDesc d;
    SuHostError err;
    Game g;
    SuHostWorld *w;
    uint8_t *snap, *rep = NULL;
    size_t sn, rn = 0;
    char hex[65], line[200];
    unsigned t;
    fleet_desc(&d, V_SAME);
    check(su_host_register_operator(&d, &err) == SU_OK, "register: %s", err.message);
    game_build(&g, &d, 0, NULL);
    w = game_world(&g, 1);
    if (w != NULL) {
        for (t = 0; t < 20u; t++) {
            step1(w);
        }
        snap = snapshot(w, &sn);
        digest_hex(w, hex);
        check(write_file(dir, "reg_snapshot.bin", snap, sn), "write snapshot");
        free(snap);
        snprintf(line, sizeof line, "%s\n", hex);
        if (record_replay(w, 60, &rep, &rn) == SU_OK) {
            check(write_file(dir, "reg_replay.bin", rep, rn), "write replay");
        }
        su_replay_free(rep);
        digest_hex(w, hex);
        snprintf(line + strlen(line), sizeof line - strlen(line), "%s\n", hex);
        check(write_file(dir, "reg_digests.txt", line, strlen(line)), "write digests");
        printf("recorded: tick 20 %.16s..., tick 80 %.16s...\n", line, hex);
        su_host_world_destroy(w);
    }
    game_free(&g);
    return 0;
}

static int mode_verify(const char *dir, const char *variant) {
    SuHostOperatorDesc d;
    SuHostError err;
    Game g;
    SuHostWorld *w = NULL;
    SuReplay *rp = NULL;
    uint8_t *snap, *rep, *dig;
    size_t sn, rn, dn;
    int v = strcmp(variant, "version") == 0     ? V_VERSION
            : strcmp(variant, "constants") == 0 ? V_CONSTANTS
            : strcmp(variant, "name") == 0      ? V_NAME
            : strcmp(variant, "flags") == 0     ? V_FLAGS
            : strcmp(variant, "scratch") == 0   ? V_SCRATCH
                                                : V_SAME;
    int none = strcmp(variant, "none") == 0;
    uint32_t st;
    char hex[65];
    snap = read_file(dir, "reg_snapshot.bin", &sn);
    rep = read_file(dir, "reg_replay.bin", &rn);
    dig = read_file(dir, "reg_digests.txt", &dn);
    if (snap == NULL || rep == NULL || dig == NULL || dn < 130u) {
        fprintf(stderr, "verify: run the record mode first (files in %s)\n", dir);
        return 2;
    }
    fleet_desc(&d, v);
    if (!none) {
        check(su_host_register_operator(&d, &err) == SU_OK, "register variant %s: %s", variant, err.message);
    }
    game_build(&g, &d, 0, NULL);
    st = su_host_world_create(&g.desc, &w, &err);
    if (none) {
        check(st == SU_E_CAPABILITY_DENIED && w == NULL, "unregistered: record refused (%s: %s)",
              su_host_status_name(st), err.message);
    } else {
        check(st == SU_OK, "world create (%s): %s", variant, err.message);
    }
    if (w != NULL) {
        int same = strcmp(variant, "same") == 0;
        uint64_t done = 0;
        st = su_host_snapshot_restore(w, snap, sn, &err);
        check(same ? st == SU_OK : st == SU_E_SCHEMA_MISMATCH, "snapshot restore (%s): %s (%s)", variant,
              su_host_status_name(st), err.message);
        if (same && st == SU_OK) {
            digest_hex(w, hex);
            check(memcmp(hex, dig, 64) == 0, "restored digest equals the recording process's");
        }
        check(su_replay_open(rep, rn, &rp, &err) == SU_OK, "replay open");
        st = su_replay_seek(rp, w, 20, &err);
        check(same ? st == SU_OK : st == SU_E_SCHEMA_MISMATCH, "replay seek (%s): %s (%s)", variant,
              su_host_status_name(st), err.message);
        if (same && st == SU_OK) {
            st = su_replay_play(rp, w, 1000, &done, &err);
            digest_hex(w, hex);
            check(st == SU_OK && done == 60u && memcmp(hex, dig + 65, 64) == 0,
                  "replay verifies to the recorded end digest: %s (%s)", su_host_status_name(st), err.message);
        }
        su_replay_close(rp);
        su_host_world_destroy(w);
    }
    game_free(&g);
    free(snap);
    free(rep);
    free(dig);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "record") == 0) {
        mode_record(argv[2]);
    } else if (argc >= 4 && strcmp(argv[1], "verify") == 0) {
        if (mode_verify(argv[2], argv[3]) != 0) {
            return 2;
        }
    } else {
        SuHostOperatorDesc d;
        Game g;
        fleet_desc(&d, V_SAME);
        game_build(&g, &d, 0, NULL);
        test_registration(&g);
        test_run_against_model(&g);
        test_rollback(&g);
        test_replay(&g);
        test_bound(&g, &d);
        test_negative(&g, &d);
        test_data(&g);
        test_no_alloc(&g);
        test_threads(&g);
        test_desc_versions();
        test_registry_limits(); /* last: it fills the registry */
        game_free(&g);
    }
    printf("test_host_registered: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
