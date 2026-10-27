/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Events from host operators (include/sutekh/host.h, "events from host operators"; issue 172), built twice: against
 * the static library and against the DLL. Everything the operator needs lives in this translation unit, outside the
 * engine library: the operators, their descriptors, their host records, the IR capsules around them and the fixture.
 *
 * The world (schema game.fx, capacity 8, 6 entities) runs, in capsule order:
 *   fx.before    IR 0.2, phase simulate: one event (kind 200, a = slot, b = x) per live row with x != 0;
 *   game.hits    registered operator, phase query: `hits` events per row (kind 300, a = slot, b = k), row by row, so
 *                the log shows the run's emission order; it also stores a column;
 *   fx.after     IR 0.2, phase simulate, after game.hits: one event (kind 400, a = slot, b = x) per live row.
 * The fixture's hits are {0, 1, 2, 0, 1, 2}, so the operator appends 6 events per tick, interleaved between the two
 * IR systems' events.
 *
 * In-process checks (no arguments):
 *   1. registration: the version 3 fields (max_events_per_tick, event_reserved), a version 2 descriptor (no events:
 *      emit_event NULL, unchanged behaviour), listing;
 *   2. the event log of every tick: the operator's records in emission order, between the earlier and the later IR
 *      system's records, with the IR record fields (kind, system_index, slot, seq, a, b) and one sequence number per
 *      event; the columns still commit;
 *   3. the declared per-tick cap: cap + 1 emissions refuse the tick with SU_E_RESOURCE_LIMIT and commit nothing,
 *      whether run returns the code or swallows it; exactly cap emissions pass; the world's per-tick event cap
 *      counts host records too;
 *   4. the log is in the canonical snapshot and both digests (same columns, other payloads: other digest);
 *   5. snapshot mid-run, restore (allocating and in-place), re-simulate: identical subsequent log and digests;
 *   6. replay record, seek and play in a fresh world: identical log and digests;
 *   7. an operator built against the shorter SuHostOperatorRun (through `state`) still runs unchanged;
 *   8. no allocation in the tick (static Linux build: malloc/calloc/realloc wrapped and counted);
 *   9. the declared cap is in the world identity: a snapshot and a replay recorded with one cap are refused in a
 *      process that registers the same operator with another (cross-process modes below).
 *
 * Cross-process modes (ctest runs record, then verify in other processes):
 *   record <dir>          writes a snapshot at tick 12, a replay of 30 ticks and the digests;
 *   verify <dir> same     same registration: the snapshot restores, the replay verifies, digests equal;
 *   verify <dir> cap      the operator registered with another max_events_per_tick: both are refused
 *                         (SU_E_SCHEMA_MISMATCH, world identity). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"
#include "sutekh/replay.h"

static unsigned g_checks, g_failures;

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

/* ---------- the schema ---------- */

#define CAP 8u
#define ENT 6u

enum { F_X = 1, F_HITS = 2, F_MARK = 3 };
enum { K_BEFORE = 200, K_HIT = 300, K_AFTER = 400 };

/* Operator lanes, descriptor order (indices into run->columns). */
enum { C_X, C_HITS, C_MARK, C_COUNT };

static const SuHostOperatorField k_fields[C_COUNT] = {
    {F_X, SU_TYPE_U32, SU_ACCESS_READ, 0, "x"},
    {F_HITS, SU_TYPE_U32, SU_ACCESS_READ, 0, "hits"},
    {F_MARK, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "mark"},
};

static const uint32_t k_x[ENT] = {1u, 2u, 3u, 4u, 5u, 6u};
static const uint32_t k_hits[ENT] = {0u, 1u, 2u, 0u, 1u, 2u};
/* Records one tick appends: both IR systems emit one per row, the operator emits one per hit (6). */
#define RECS_PER_TICK (2u * ENT + 6u)

/* ---------- the operator (game code, outside the engine) ---------- */

enum { MODE_NORMAL, MODE_RETURN, MODE_SWALLOW };

typedef struct OpStats {
    unsigned calls;
    unsigned refusals;      /* emit_event calls that returned a code other than SU_OK */
    unsigned missing_emit;  /* run->emit_event was NULL although the operator declares events */
    unsigned contract_bad;
    unsigned payload;       /* high half of the payload word a (check 4: only the log changes) */
    unsigned mode;
    unsigned limit;         /* 0: emit every hit; else stop after this many emissions (no refusal) */
} OpStats;

static OpStats g_op;

/* One event per hit: (a = slot | payload << 16, b = k), rows in ascending order, so the run's emission order is
 * (slot, k) ascending too. mark[i] counts the events emitted up to and including row i. */
static uint32_t hits_run(const SuHostOperatorRun *r, void *user) {
    const uint32_t *hits = r->columns[C_HITS].in;
    uint32_t *mark = r->columns[C_MARK].out;
    uint64_t i, k, n = 0;
    int stop = 0;
    (void)user;
    g_op.calls++;
    if (r->column_count != C_COUNT || r->row_begin != 0u || r->row_end != r->rows || r->chunk_index != 0u ||
        r->chunk_count != 1u || r->reason == NULL || r->reason[0] != '\0' || r->alive == NULL ||
        r->generation == NULL) {
        g_op.contract_bad++;
        return SU_E_INTERNAL;
    }
    if (r->emit_event == NULL) {
        /* An operator that declares no events gets no emitter; it runs exactly as before issue 172. */
        g_op.missing_emit++;
        for (i = 0; i < r->rows; i++) {
            mark[i] = 0u;
        }
        return SU_OK;
    }
    for (i = 0; i < r->rows; i++) {
        if (!r->alive[i] || stop) {
            mark[i] = (uint32_t)n;
            continue;
        }
        for (k = 0; k < hits[i]; k++) {
            uint32_t st;
            if (g_op.limit != 0u && n >= g_op.limit) {
                stop = 1;
                break;
            }
            st = r->emit_event(r->emit_ctx, (uint32_t)i, K_HIT, (uint32_t)i | (g_op.payload << 16), (uint32_t)k);
            if (st != SU_OK) {
                g_op.refusals++;
                if (g_op.mode == MODE_RETURN) {
                    return st;
                }
                stop = 1;
                break;
            }
            n++;
        }
        mark[i] = (uint32_t)n; /* the events emitted up to and including this row */
    }
    return SU_OK;
}

/* The layout of SuHostOperatorRun through `state`: what an operator built against the 2026-10-26 header sees. It
 * reads struct_size, the fields it knows and nothing else. */
typedef struct OldRun {
    uint32_t struct_size;
    uint32_t column_count;
    const SuHostOperatorColumn *columns;
    uint64_t rows;
    uint64_t row_begin;
    uint64_t row_end;
    uint32_t chunk_index;
    uint32_t chunk_count;
    uint64_t tick;
    const uint8_t *alive;
    const uint32_t *generation;
    void *scratch;
    size_t scratch_bytes;
    const void *data;
    char *reason;
    size_t reason_cap;
    SuHostState *state;
} OldRun;

_Static_assert(sizeof(OldRun) == SU_HOST_OPERATOR_RUN_STATE_SIZE,
               "the pre-2026-10-27 run layout is SU_HOST_OPERATOR_RUN_STATE_SIZE");

static unsigned g_old_calls;

/* An operator that predates the event fields: it reads the struct it was compiled against and never touches
 * emit_event. It runs unchanged, which is what struct_size versioning promises. */
static uint32_t old_run(const SuHostOperatorRun *r, void *user) {
    const OldRun *o = (const OldRun *)r;
    const uint32_t *hits;
    uint32_t *mark;
    uint64_t i;
    (void)user;
    g_old_calls++;
    if (o->struct_size < sizeof *o || o->column_count != C_COUNT || o->rows != CAP || o->alive == NULL) {
        return SU_E_INTERNAL;
    }
    hits = o->columns[C_HITS].in;
    mark = o->columns[C_MARK].out;
    for (i = 0; i < o->rows; i++) {
        mark[i] = o->alive[i] ? hits[i] + 100u + (uint32_t)o->tick : 0u;
    }
    return SU_OK;
}

/* Variants of the registration (cross-process identity check). */
enum { V_SAME, V_CAP };

static void hits_desc(SuHostOperatorDesc *d, const char *system_id, uint32_t cap, int variant) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(d);
    d->system_id = system_id;
    d->schema_id = "game.fx";
    d->operator_name = "fx_ops_v1";
    d->operator_version = 1u;
    d->field_count = C_COUNT;
    d->fields = k_fields;
    for (i = 0; i < 32u; i++) {
        d->constants_hash[i] = (uint8_t)(i * 11u + 5u);
    }
    d->max_events_per_tick = variant == V_CAP ? cap + 4u : cap;
    d->run = hits_run;
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

static void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static const char *access_name(uint32_t a) {
    return a == SU_ACCESS_READ ? "read" : a == SU_ACCESS_WRITE ? "write" : "read_write";
}

/* The host record of d: its own field table under the game schema's IDs. */
static void put_record(Buf *b, const SuHostOperatorDesc *d) {
    uint32_t i;
    put(b, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"system_rev\":1,\"capabilities\":[],", d->system_id);
    put(b, "\"schema\":{\"id\":\"%s\",\"version\":1,\"layout_generation\":1},", d->schema_id);
    put(b, "\"numeric_profile\":\"u32_mod_v1\",");
    put(b, "\"schedule\":{\"phase\":\"query\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},");
    put(b, "\"host\":{\"operator\":\"%s\",\"operator_version\":%u,\"constants_hash\":\"", d->operator_name,
        (unsigned)d->operator_version);
    for (i = 0; i < 32u; i++) {
        put(b, "%02x", (unsigned)d->constants_hash[i]);
    }
    put(b, "\",\"fields\":[");
    for (i = 0; i < d->field_count; i++) {
        const SuHostOperatorField *f = &d->fields[i];
        put(b, "%s{\"id\":%u,\"name\":\"%s\",\"type\":\"u32\",\"access\":\"%s\"}", i > 0u ? "," : "",
            (unsigned)f->id, f->name, access_name(f->access));
    }
    put(b, "]}}");
}

/* An IR 0.2 capsule that reads x and emits one event (kind k, a = slot, b = x) for every live row with x != 0. */
static void put_emit_capsule(Buf *b, const char *system_id, const char *after) {
    put(b, "{\"capsule_format\":\"0.1\",\"ir_version\":\"0.2\",\"system_id\":\"%s\",\"source\":\"inline.lir.json\",",
        system_id);
    put(b, "\"schema\":{\"id\":\"game.fx\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"u32_mod_v1\",");
    put(b, "\"fields\":[{\"id\":%u,\"name\":\"x\",\"type\":\"u32\",\"read\":true,\"write\":false}],", (unsigned)F_X);
    put(b, "\"schedule\":{\"phase\":\"simulate\",\"after\":[%s],\"before\":[],\"parallel\":\"serial\"},", after);
    put(b, "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\","
            "\"max_struct_per_row\":0,\"max_events_per_row\":1},");
    put(b, "\"tests\":[\"tests/host/test_host_events.c\"],\"capabilities\":[\"emit_event\"]}");
}

static void put_emit_lir(Buf *b, const char *system_id, uint32_t kind) {
    put(b, "{\"ir_version\":\"0.2\",\"system_id\":\"%s\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"game.fx\",",
        system_id);
    put(b, "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"x\"},"
            "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},{\"op\":\"lt_u32\",\"dst\":\"c\",\"a\":\"z\",\"b\":\"x\"},"
            "{\"op\":\"slot_u32\",\"dst\":\"s\"},{\"op\":\"emit_event\",\"cond\":\"c\",\"kind\":%u,\"a\":\"s\","
            "\"b\":\"x\"}]}",
        (unsigned)kind);
}

/* ---------- worlds ---------- */

typedef struct Scene {
    Buf before_cap, before_lir, after_cap, after_lir, record, fixture;
    SuHostCapsule caps[3];
    SuHostWorldDesc desc;
} Scene;

/* x_only_row0: only slot 0 has a nonzero x, so each IR system emits one event and a small world per-tick cap can
 * still hold them (the operator keeps its 6 hits). */
static void scene_build(Scene *sc, const SuHostOperatorDesc *d, uint64_t max_events_per_tick, int x_only_row0) {
    char after[300];
    uint32_t i;
    memset(sc, 0, sizeof *sc);
    snprintf(after, sizeof after, "\"%s\"", d->system_id);
    put_emit_capsule(&sc->before_cap, "fx.before", "");
    put_emit_lir(&sc->before_lir, "fx.before", K_BEFORE);
    put_emit_capsule(&sc->after_cap, "fx.after", after);
    put_emit_lir(&sc->after_lir, "fx.after", K_AFTER);
    put_record(&sc->record, d);
    put(&sc->fixture, "{\"fixture_format\":\"0.1\",\"schema_id\":\"game.fx\",\"schema_version\":1,\"tick\":0,"
                      "\"entity_ids\":[");
    for (i = 0; i < ENT; i++) {
        put(&sc->fixture, "%s%u", i > 0u ? "," : "", (unsigned)(i + 1u));
    }
    put(&sc->fixture, "],\"columns\":{\"x\":[");
    for (i = 0; i < ENT; i++) {
        put(&sc->fixture, "%s%u", i > 0u ? "," : "", (unsigned)(x_only_row0 && i != 0u ? 0u : k_x[i]));
    }
    put(&sc->fixture, "],\"hits\":[");
    for (i = 0; i < ENT; i++) {
        put(&sc->fixture, "%s%u", i > 0u ? "," : "", (unsigned)k_hits[i]);
    }
    put(&sc->fixture, "],\"mark\":[");
    for (i = 0; i < ENT; i++) {
        put(&sc->fixture, "%s0", i > 0u ? "," : "");
    }
    put(&sc->fixture, "]}}");
    sc->caps[0].capsule_json = (const uint8_t *)sc->before_cap.p;
    sc->caps[0].capsule_len = sc->before_cap.n;
    sc->caps[0].lir_json = (const uint8_t *)sc->before_lir.p;
    sc->caps[0].lir_len = sc->before_lir.n;
    sc->caps[1].capsule_json = (const uint8_t *)sc->record.p; /* host operator record: no lir_json */
    sc->caps[1].capsule_len = sc->record.n;
    sc->caps[2].capsule_json = (const uint8_t *)sc->after_cap.p;
    sc->caps[2].capsule_len = sc->after_cap.n;
    sc->caps[2].lir_json = (const uint8_t *)sc->after_lir.p;
    sc->caps[2].lir_len = sc->after_lir.n;
    sc->desc.struct_size = sizeof sc->desc;
    sc->desc.capsule_count = 3;
    sc->desc.capsules = sc->caps;
    sc->desc.fixture_json = (const uint8_t *)sc->fixture.p;
    sc->desc.fixture_len = sc->fixture.n;
    sc->desc.max_entities = CAP;
    sc->desc.tick_rate_hz = 60;
    sc->desc.max_events_per_tick = max_events_per_tick;
    sc->desc.event_log_max = 4096;
    sc->desc.seed = 172;
}

static void scene_free(Scene *sc) {
    buf_free(&sc->before_cap);
    buf_free(&sc->before_lir);
    buf_free(&sc->after_cap);
    buf_free(&sc->after_lir);
    buf_free(&sc->record);
    buf_free(&sc->fixture);
}

static SuHostWorld *scene_world(const Scene *sc) {
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t st = su_host_world_create(&sc->desc, &w, &err);
    check(st == SU_OK, "world create: %s (%s)", su_host_status_name(st), err.message);
    return st == SU_OK ? w : NULL;
}

static int step1(SuHostWorld *w) {
    uint64_t done = 0;
    SuHostError err;
    uint32_t st = su_host_step(w, 1, &done, &err);
    check(st == SU_OK && done == 1u, "step: %s (%s)", su_host_status_name(st), err.message);
    return st == SU_OK;
}

/* ---------- the log ---------- */

#define LOG_MAX 1024u

static uint32_t log_copy(const SuHostWorld *w, SuHostEvent *out) {
    uint32_t n = 0;
    uint64_t next = 0;
    check(su_host_events_read(w, 0u, out, LOG_MAX, &n, &next, NULL) == SU_OK, "events read");
    check(next == su_host_event_next_seq(w), "cursor is caught up");
    return n;
}

/* 1 when the two worlds' logs are equal event for event (sequence number included). */
static int log_equal(const SuHostWorld *a, const SuHostWorld *b, unsigned *diff_at) {
    SuHostEvent ea[LOG_MAX], eb[LOG_MAX];
    uint32_t na = log_copy(a, ea), nb = log_copy(b, eb), i;
    if (su_host_event_next_seq(a) != su_host_event_next_seq(b) || na != nb) {
        *diff_at = na < nb ? na : nb;
        return 0;
    }
    for (i = 0; i < na; i++) {
        if (ea[i].event_seq != eb[i].event_seq || ea[i].tick != eb[i].tick || ea[i].kind != eb[i].kind ||
            ea[i].system_index != eb[i].system_index || ea[i].slot != eb[i].slot || ea[i].seq != eb[i].seq ||
            ea[i].a != eb[i].a || ea[i].b != eb[i].b) {
            *diff_at = i;
            return 0;
        }
    }
    return 1;
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

/* The records of one tick, in log order. */
static uint32_t tick_events(const SuHostWorld *w, uint64_t tick, SuHostEvent *out) {
    uint32_t n = 0, total = 0, complete = 0;
    check(su_host_events_for_tick(w, tick, out, LOG_MAX, &n, &total, &complete) == SU_OK, "events for tick");
    check(n == total && complete == 1u, "tick %llu: %u of %u records, complete %u", (unsigned long long)tick,
          (unsigned)n, (unsigned)total, (unsigned)complete);
    return n;
}

/* ---------- 1. registration ---------- */

static void test_registration(void) {
    SuHostOperatorDesc d, bad, v2;
    SuHostError err;
    Scene sc;
    SuHostWorld *w;
    SuHostEvent ev[LOG_MAX];
    uint32_t base = su_host_operator_count(), st;
    const char *sid = NULL, *name = NULL;
    unsigned miss0;

    hits_desc(&d, "game.hits", 16u, V_SAME);
    check(su_host_register_operator(NULL, &err) == SU_E_INVALID_ARGUMENT, "NULL descriptor");
    bad = d;
    bad.max_events_per_tick = 1048577u;
    st = su_host_register_operator(&bad, &err);
    check(st == SU_E_INVALID_ARGUMENT && strstr(err.message, "max_events_per_tick") != NULL,
          "cap above 1048576: %s (%s)", su_host_status_name(st), err.message);
    bad = d;
    bad.event_reserved = 1u;
    st = su_host_register_operator(&bad, &err);
    check(st == SU_E_INVALID_ARGUMENT && strstr(err.message, "event_reserved") != NULL,
          "event_reserved nonzero: %s (%s)", su_host_status_name(st), err.message);
    check(su_host_operator_count() == base, "refusals register nothing");

    st = su_host_register_operator(&d, &err);
    check(st == SU_OK, "register game.hits: %s (%s)", su_host_status_name(st), err.message);
    check(su_host_operator_count() == base + 1u && su_host_operator_info(base, &sid, &name) == SU_OK &&
              strcmp(sid, "game.hits") == 0 && strcmp(name, "fx_ops_v1") == 0,
          "listing shows the registered operator");

    /* A version 2 descriptor (through state_reserved) still registers; it declares no events, so run gets no
     * emit_event and the world behaves exactly as before this change. */
    SU_HOST_OPERATOR_DESC_INIT(&v2);
    v2.struct_size = SU_HOST_OPERATOR_DESC_V2_SIZE;
    v2.system_id = "game.v2";
    v2.schema_id = d.schema_id;
    v2.operator_name = "fx_v2_ops";
    v2.operator_version = 1u;
    v2.field_count = C_COUNT;
    v2.fields = k_fields;
    memcpy(v2.constants_hash, d.constants_hash, 32u);
    v2.run = hits_run;
    st = su_host_register_operator(&v2, &err);
    check(st == SU_OK, "a version 2 descriptor registers: %s (%s)", su_host_status_name(st), err.message);
    memset(&g_op, 0, sizeof g_op);
    scene_build(&sc, &v2, 0u, 0);
    w = scene_world(&sc);
    if (w != NULL) {
        miss0 = g_op.missing_emit;
        if (step1(w)) {
            check(g_op.missing_emit == miss0 + 1u, "run->emit_event is NULL without a declared cap");
            check(tick_events(w, 1u, ev) == 2u * ENT, "only the two IR systems' records (%u)",
                  (unsigned)tick_events(w, 1u, ev));
            check(su_host_event_next_seq(w) == 2u * ENT, "12 events in the log");
        }
        su_host_world_destroy(w);
    }
    scene_free(&sc);
}

/* ---------- 2. order, interleaving and record format ---------- */

/* One tick's expected records: the IR system before, the operator's emissions in order, the IR system after. */
typedef struct Want {
    uint32_t kind;
    uint32_t system_index;
    uint32_t slot;
    uint32_t seq;
    uint32_t a;
    uint32_t b;
} Want;

static unsigned want_tick(Want *out) {
    unsigned n = 0, i, k;
    for (i = 0; i < ENT; i++) {
        out[n].kind = K_BEFORE;
        out[n].system_index = 0u;
        out[n].slot = i;
        out[n].seq = 0u;
        out[n].a = i;
        out[n].b = k_x[i];
        n++;
    }
    for (i = 0; i < ENT; i++) {
        for (k = 0; k < k_hits[i]; k++) {
            out[n].kind = K_HIT;
            out[n].system_index = 1u;
            out[n].slot = i;
            out[n].seq = n - ENT; /* the run's emission index */
            out[n].a = i;
            out[n].b = k;
            n++;
        }
    }
    for (i = 0; i < ENT; i++) {
        out[n].kind = K_AFTER;
        out[n].system_index = 2u;
        out[n].slot = i;
        out[n].seq = 0u;
        out[n].a = i;
        out[n].b = k_x[i];
        n++;
    }
    return n;
}

static void test_order(const SuHostOperatorDesc *d) {
    Scene sc;
    SuHostWorld *w;
    SuHostEvent ev[LOG_MAX];
    Want want[64];
    unsigned nw = want_tick(want), t, i, bad = 0;
    uint32_t n, mark[ENT];
    uint64_t seq = 0u;

    memset(&g_op, 0, sizeof g_op);
    scene_build(&sc, d, 0u, 0);
    w = scene_world(&sc);
    if (w == NULL) {
        scene_free(&sc);
        return;
    }
    check(su_host_system_count(w) == 3u, "three systems");
    check(g_op.calls == 0u, "no run before the first step");
    for (t = 0; t < 5u; t++) {
        if (!step1(w)) {
            break;
        }
        n = tick_events(w, t + 1u, ev);
        if (n != nw) {
            check(0, "tick %u: %u records, expected %u", t + 1u, (unsigned)n, nw);
            continue;
        }
        for (i = 0; i < nw; i++) {
            if (ev[i].kind != want[i].kind || ev[i].system_index != want[i].system_index ||
                ev[i].slot != want[i].slot || ev[i].seq != want[i].seq || ev[i].a != want[i].a ||
                ev[i].b != want[i].b || ev[i].tick != (uint64_t)t + 1u || ev[i].event_seq != seq + i) {
                if (bad < 5u) {
                    fprintf(stderr,
                            "  tick %u record %u: kind %u system %u slot %u seq %u a %u b %u tick %llu seq %llu\n",
                            t + 1u, i, (unsigned)ev[i].kind, (unsigned)ev[i].system_index, (unsigned)ev[i].slot,
                            (unsigned)ev[i].seq, (unsigned)ev[i].a, (unsigned)ev[i].b,
                            (unsigned long long)ev[i].tick, (unsigned long long)ev[i].event_seq);
                }
                bad++;
            }
        }
        seq += nw;
    }
    check(bad == 0u, "every tick's log is the expected sequence (%u records differ)", bad);
    check(su_host_event_next_seq(w) == 5u * nw, "one sequence number per event (%llu)",
          (unsigned long long)su_host_event_next_seq(w));
    check(g_op.calls == 5u, "run once per tick (%u)", g_op.calls);
    check(g_op.missing_emit == 0u, "a declared operator gets emit_event");
    check(g_op.refusals == 0u, "no emission was refused (%u)", g_op.refusals);
    check(g_op.contract_bad == 0u, "the run contract held");
    /* The operator's column still commits: mark[i] counts the events emitted up to and including row i. */
    check(su_host_read(w, F_MARK, 0u, ENT, mark) == SU_OK, "read mark");
    for (i = 0; i < ENT; i++) {
        uint32_t want_mark = 0u, k;
        for (k = 0; k <= i; k++) {
            want_mark += k_hits[k];
        }
        check(mark[i] == want_mark, "mark[%u] = %u (got %u)", i, (unsigned)want_mark, (unsigned)mark[i]);
    }
    su_host_world_destroy(w);
    scene_free(&sc);
}

/* ---------- 3. the declared cap ---------- */

/* The world is refused no further than its last committed tick: same tick, same digest, same log, same columns. */
static void check_unchanged(SuHostWorld *w, uint64_t tick, const char *digest, uint64_t next_seq) {
    char now[65];
    digest_hex(w, now);
    check(su_host_tick(w) == tick, "tick is still %llu (%llu)", (unsigned long long)tick,
          (unsigned long long)su_host_tick(w));
    check(strcmp(now, digest) == 0, "digest unchanged");
    check(su_host_event_next_seq(w) == next_seq, "log unchanged (%llu events)",
          (unsigned long long)su_host_event_next_seq(w));
}

static void test_cap(const SuHostOperatorDesc *cap4, const SuHostOperatorDesc *swallow) {
    Scene sc;
    SuHostWorld *w;
    SuHostEvent ev[LOG_MAX];
    SuHostError err;
    uint64_t done = 0u, tick;
    uint32_t st, n;
    char digest[65];
    unsigned pass;

    for (pass = 0; pass < 2u; pass++) {
        const SuHostOperatorDesc *d = pass == 0u ? cap4 : swallow;
        memset(&g_op, 0, sizeof g_op);
        g_op.limit = 4u; /* exactly the declared cap: 4 of the fixture's 6 hits */
        scene_build(&sc, d, 0u, 0);
        w = scene_world(&sc);
        if (w == NULL) {
            scene_free(&sc);
            continue;
        }
        check(step1(w), "%s: exactly cap emissions pass", d->system_id);
        n = tick_events(w, 1u, ev);
        check(n == 12u + 4u, "%s: 12 IR records and 4 operator records (%u)", d->system_id, (unsigned)n);
        check(su_host_event_next_seq(w) == 2u * ENT + 4u, "16 events after one tick");

        /* cap + 1 attempts against a cap of 4: the fifth emission is refused. */
        g_op.limit = 5u;
        g_op.mode = pass == 0u ? MODE_RETURN : MODE_SWALLOW;
        tick = su_host_tick(w);
        digest_hex(w, digest);
        st = su_host_step(w, 1u, &done, &err);
        check(st == SU_E_RESOURCE_LIMIT && done == 0u, "%s: over-cap tick refused (%s, done %llu)", d->system_id,
              su_host_status_name(st), (unsigned long long)done);
        check(err.index == 1u, "%s: the failing system is the operator (index %u)", d->system_id, (unsigned)err.index);
        check(strstr(err.message, "event") != NULL, "%s: the message names the refusal: %s", d->system_id,
              err.message);
        check_unchanged(w, tick, digest, 2u * ENT + 4u);
        check(g_op.refusals >= 1u, "%s: emit_event returned a code (%u)", d->system_id, g_op.refusals);
        su_host_world_destroy(w);
        scene_free(&sc);
    }
}

/* The world's own per-tick event cap counts the operator's records too: 2 IR records + 6 operator records against
 * a cap of 4 is refused at the boundary, before anything is appended. */
static void test_world_cap(const SuHostOperatorDesc *d) {
    Scene sc;
    SuHostWorld *w;
    SuHostError err;
    uint64_t done = 0u;
    uint32_t st;
    char digest[65];

    memset(&g_op, 0, sizeof g_op);
    scene_build(&sc, d, 4u, 1);
    w = scene_world(&sc);
    if (w == NULL) {
        scene_free(&sc);
        return;
    }
    digest_hex(w, digest);
    st = su_host_step(w, 1u, &done, &err);
    check(st == SU_E_RESOURCE_LIMIT && done == 0u, "the world's per-tick event cap counts host records (%s)",
          su_host_status_name(st));
    check(err.index == UINT32_MAX, "the boundary refused the tick, not a system (index %u)", (unsigned)err.index);
    check_unchanged(w, 0u, digest, 0u);
    check(su_host_event_next_seq(w) == 0u, "nothing was appended");
    su_host_world_destroy(w);
    scene_free(&sc);
}

/* ---------- 4. the log is in the snapshot and both digests ---------- */

static void test_coverage(const SuHostOperatorDesc *d) {
    Scene sc;
    SuHostWorld *wa, *wb;
    char ha[65], hb[65];
    uint8_t *sa, *sb;
    size_t la = 0u, lb = 0u;
    uint32_t ma[ENT], mb[ENT];

    memset(&g_op, 0, sizeof g_op);
    scene_build(&sc, d, 0u, 0);
    wa = scene_world(&sc);
    wb = scene_world(&sc);
    if (wa == NULL || wb == NULL) {
        su_host_world_destroy(wa);
        su_host_world_destroy(wb);
        scene_free(&sc);
        return;
    }
    g_op.payload = 0u;
    check(step1(wa), "world A");
    g_op.payload = 1u; /* only the payload word of the operator's records changes, never a column */
    check(step1(wb), "world B");
    check(su_host_read(wa, F_MARK, 0u, ENT, ma) == SU_OK && su_host_read(wb, F_MARK, 0u, ENT, mb) == SU_OK &&
              memcmp(ma, mb, sizeof ma) == 0,
          "both worlds have the same columns");
    digest_hex(wa, ha);
    digest_hex(wb, hb);
    check(strcmp(ha, hb) != 0, "different events, different digest");
    sa = snapshot(wa, &la);
    sb = snapshot(wb, &lb);
    check(la == lb && memcmp(sa, sb, la) != 0, "different events, different snapshot bytes (%zu, %zu)", la, lb);
    free(sa);
    free(sb);
    su_host_world_destroy(wa);
    su_host_world_destroy(wb);
    scene_free(&sc);
}

/* ---------- 5. snapshot mid-run, restore, re-simulate ---------- */

static void test_snapshot(const SuHostOperatorDesc *d) {
    Scene sc;
    SuHostWorld *w, *fresh;
    uint8_t *snap;
    size_t len = 0u;
    char before[65], again[65], per_tick[13][65];
    uint64_t t;
    unsigned diff = 0u, at = 0u, pass;
    SuHostError err;

    memset(&g_op, 0, sizeof g_op);
    scene_build(&sc, d, 0u, 0);
    w = scene_world(&sc);
    if (w == NULL) {
        scene_free(&sc);
        return;
    }
    for (t = 0; t < 5u; t++) {
        if (!step1(w)) {
            break;
        }
    }
    snap = snapshot(w, &len);
    digest_hex(w, before);
    for (t = 6u; t <= 12u; t++) {
        if (!step1(w)) {
            break;
        }
        digest_hex(w, per_tick[t]);
    }
    for (pass = 0; pass < 2u; pass++) {
        uint32_t st = pass == 0u ? su_host_snapshot_restore(w, snap, len, &err)
                                 : su_host_snapshot_restore_inplace(w, snap, len, &err);
        check(st == SU_OK && su_host_tick(w) == 5u, "restore %u: %s (%s)", pass, su_host_status_name(st),
              err.message);
        digest_hex(w, again);
        check(strcmp(again, before) == 0, "restore %u: digest is the snapshot's", pass);
        check(su_host_event_next_seq(w) == 5u * RECS_PER_TICK, "restore %u: the log is the snapshot's (%llu events)",
              pass,
              (unsigned long long)su_host_event_next_seq(w));
        for (t = 6u; t <= 12u; t++) {
            if (!step1(w)) {
                break;
            }
            digest_hex(w, again);
            diff += strcmp(again, per_tick[t]) != 0;
        }
    }
    check(diff == 0u, "re-simulation after both restores equals the first run every tick (%u differ)", diff);
    /* A fresh world restored from the snapshot reaches the same log and digest. */
    fresh = scene_world(&sc);
    if (fresh != NULL) {
        check(su_host_snapshot_restore(fresh, snap, len, &err) == SU_OK, "restore into a fresh world: %s",
              err.message);
        for (t = 6u; t <= 12u; t++) {
            step1(fresh);
        }
        digest_hex(fresh, again);
        check(strcmp(again, per_tick[12]) == 0, "fresh world reaches the same digest");
        check(log_equal(w, fresh, &at), "fresh world reaches the same log (first difference at %u)", at);
        su_host_world_destroy(fresh);
    }
    free(snap);
    su_host_world_destroy(w);
    scene_free(&sc);
}

/* ---------- 6. replay ---------- */

static uint32_t record_replay(SuHostWorld *w, uint64_t ticks, uint8_t **bytes, size_t *len) {
    SuReplayOptions o;
    SuReplayWriter *rw = NULL;
    SuHostError err;
    uint64_t done = 0u;
    uint32_t st;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.keyframe_interval = 8u;
    o.digest_interval = 1u;
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

static void test_replay(const SuHostOperatorDesc *d) {
    Scene sc;
    SuHostWorld *w, *w2;
    uint8_t *bytes = NULL;
    size_t len = 0u;
    SuReplay *rp = NULL;
    SuHostError err;
    uint64_t done = 0u;
    char a[65], b[65];
    unsigned at = 0u;
    uint32_t st;

    memset(&g_op, 0, sizeof g_op);
    scene_build(&sc, d, 0u, 0);
    w = scene_world(&sc);
    if (w == NULL) {
        scene_free(&sc);
        return;
    }
    if (record_replay(w, 30u, &bytes, &len) != SU_OK) {
        su_host_world_destroy(w);
        scene_free(&sc);
        return;
    }
    digest_hex(w, a);
    check(su_replay_open(bytes, len, &rp, &err) == SU_OK, "replay open: %s", err.message);
    w2 = scene_world(&sc);
    if (rp != NULL && w2 != NULL) {
        st = su_replay_seek(rp, w2, 12u, &err);
        check(st == SU_OK && su_host_tick(w2) == 12u, "seek 12: %s (%s)", su_host_status_name(st), err.message);
        check(su_host_event_next_seq(w2) == 12u * RECS_PER_TICK, "the keyframe carries the log (%llu events)",
              (unsigned long long)su_host_event_next_seq(w2));
        st = su_replay_play(rp, w2, 100u, &done, &err);
        check(st == SU_OK && done == 18u, "play to the end: %s (%s)", su_host_status_name(st), err.message);
        digest_hex(w2, b);
        check(strcmp(a, b) == 0, "replayed end state equals the recorded run");
        check(log_equal(w, w2, &at), "replayed log equals the recorded log (first difference at %u)", at);
    }
    su_replay_close(rp);
    su_replay_free(bytes);
    su_host_world_destroy(w2);
    su_host_world_destroy(w);
    scene_free(&sc);
}

/* ---------- 7. an operator built against the shorter struct ---------- */

static void test_old_operator(void) {
    SuHostOperatorDesc d;
    SuHostError err;
    Scene sc;
    SuHostWorld *w;
    SuHostEvent ev[LOG_MAX];
    uint32_t mark[ENT], i;
    unsigned calls0 = g_old_calls;

    SU_HOST_OPERATOR_DESC_INIT(&d);
    d.system_id = "game.old";
    d.schema_id = "game.fx";
    d.operator_name = "fx_old_ops";
    d.operator_version = 1u;
    d.field_count = C_COUNT;
    d.fields = k_fields;
    for (i = 0; i < 32u; i++) {
        d.constants_hash[i] = (uint8_t)(i * 3u + 9u);
    }
    d.run = old_run; /* max_events_per_tick stays 0: no events, as before issue 172 */
    {
        uint32_t st = su_host_register_operator(&d, &err);
        check(st == SU_OK, "register game.old: %s (%s)", su_host_status_name(st), err.message);
    }
    memset(&g_op, 0, sizeof g_op);
    scene_build(&sc, &d, 0u, 0);
    w = scene_world(&sc);
    if (w == NULL) {
        scene_free(&sc);
        return;
    }
    check(step1(w) && step1(w), "two ticks");
    check(g_old_calls == calls0 + 2u, "the old operator ran once per tick (%u)", g_old_calls - calls0);
    check(su_host_read(w, F_MARK, 0u, ENT, mark) == SU_OK, "read mark");
    for (i = 0; i < ENT; i++) {
        check(mark[i] == k_hits[i] + 100u + 1u, "old operator's column row %u = %u (got %u)", i,
              (unsigned)(k_hits[i] + 101u), (unsigned)mark[i]);
    }
    /* It appends nothing: the log is the two IR systems' records. */
    check(tick_events(w, 2u, ev) == 2u * ENT, "the old operator appends no events");
    check(su_host_event_next_seq(w) == 2u * 2u * ENT, "24 events after two ticks");
    su_host_world_destroy(w);
    scene_free(&sc);
}

/* ---------- 8. no allocation in the tick ---------- */

static void test_no_alloc(const SuHostOperatorDesc *d) {
#ifdef SU_TEST_WRAP_ALLOC
    Scene sc;
    SuHostWorld *w;
    SuHostError err;
    uint64_t done = 0u;

    memset(&g_op, 0, sizeof g_op);
    scene_build(&sc, d, 0u, 0);
    w = scene_world(&sc);
    if (w == NULL) {
        scene_free(&sc);
        return;
    }
    step1(w); /* any lazy first-tick setup outside the measurement */
    g_allocs = 0u;
    g_count_alloc = 1;
    su_host_step(w, 20u, &done, &err);
    g_count_alloc = 0;
    check(done == 20u && g_allocs == 0u, "20 ticks with a host operator's events allocate nothing (%lu allocations)",
          g_allocs);
    su_host_world_destroy(w);
    scene_free(&sc);
#else
    (void)d;
    printf("note: allocation counting not built in (not the static Linux variant)\n");
#endif
}

/* ---------- 9. the declared cap is in the world identity (cross-process) ---------- */

static int write_file(const char *dir, const char *name, const void *p, size_t n) {
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    if (fwrite(p, 1u, n, f) != n) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static uint8_t *read_file(const char *dir, const char *name, size_t *n) {
    char path[1024];
    FILE *f;
    uint8_t *b;
    long sz;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        if (f != NULL) {
            fclose(f);
        }
        return NULL;
    }
    b = malloc((size_t)sz + 1u);
    if (b == NULL || fread(b, 1u, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int run_record(const char *dir) {
    SuHostOperatorDesc d;
    SuHostError err;
    Scene sc;
    SuHostWorld *w;
    SuReplayWriter *rw = NULL;
    SuReplayOptions opt;
    uint8_t *bytes = NULL, *snap, dg[32];
    size_t len = 0u, slen = 0u;
    uint64_t t, done = 0u;

    memset(&g_op, 0, sizeof g_op);
    hits_desc(&d, "game.hits", 16u, V_SAME);
    check(su_host_register_operator(&d, &err) == SU_OK, "register");
    scene_build(&sc, &d, 0u, 0);
    w = scene_world(&sc);
    if (w == NULL) {
        return 1;
    }
    memset(&opt, 0, sizeof opt);
    opt.struct_size = sizeof opt;
    opt.keyframe_interval = 8u;
    opt.digest_interval = 1u;
    check(su_replay_writer_begin(w, &opt, &rw, &err) == SU_OK, "writer begin");
    for (t = 0; t < 30u; t++) {
        check(su_replay_writer_step(rw, w, 1u, &done, &err) == SU_OK, "writer step");
    }
    check(su_replay_writer_finish(rw, w, &bytes, &len) == SU_OK, "finish");
    su_replay_writer_destroy(rw);
    snap = snapshot(w, &slen);
    check(su_host_digest(w, dg) == SU_OK, "digest");
    check(write_file(dir, "events_replay.bin", bytes, len) && write_file(dir, "events_snap.bin", snap, slen) &&
              write_file(dir, "events_digest.bin", dg, 32u),
          "write files");
    su_replay_free(bytes);
    free(snap);
    su_host_world_destroy(w);
    scene_free(&sc);
    return 0;
}

static int run_verify(const char *dir, const char *what) {
    SuHostOperatorDesc d;
    SuHostError err;
    Scene sc;
    SuHostWorld *w;
    SuReplay *rp = NULL;
    uint8_t *bytes, *snap, *dg, now[32];
    size_t len = 0u, slen = 0u, dlen = 0u;
    int variant = strcmp(what, "same") == 0 ? V_SAME : strcmp(what, "cap") == 0 ? V_CAP : -1;
    uint32_t want = variant == V_SAME ? SU_OK : SU_E_SCHEMA_MISMATCH, st;

    if (variant < 0) {
        fprintf(stderr, "unknown verify mode %s\n", what);
        return 2;
    }
    memset(&g_op, 0, sizeof g_op);
    hits_desc(&d, "game.hits", 16u, variant);
    check(su_host_register_operator(&d, &err) == SU_OK, "register %s", what);
    scene_build(&sc, &d, 0u, 0);
    w = scene_world(&sc);
    bytes = read_file(dir, "events_replay.bin", &len);
    snap = read_file(dir, "events_snap.bin", &slen);
    dg = read_file(dir, "events_digest.bin", &dlen);
    if (w == NULL || bytes == NULL || snap == NULL || dg == NULL || dlen != 32u) {
        fprintf(stderr, "missing world or files\n");
        return 1;
    }
    st = su_host_snapshot_restore(w, snap, slen, &err);
    check(st == want, "%s: snapshot restore %s (want %s)", what, su_host_status_name(st), su_host_status_name(want));
    st = su_host_snapshot_restore_inplace(w, snap, slen, &err);
    check(st == want, "%s: in-place restore %s", what, su_host_status_name(st));
    if (want == SU_OK) {
        check(su_host_digest(w, now) == SU_OK && memcmp(now, dg, 32u) == 0,
              "restored digest equals the recording process's");
    }
    check(su_replay_open(bytes, len, &rp, &err) == SU_OK, "replay open");
    st = su_replay_seek(rp, w, 30u, &err);
    check(st == want, "%s: replay seek to the end %s (%s)", what, su_host_status_name(st), err.message);
    if (want == SU_OK) {
        uint64_t done = 0u;
        check(su_replay_seek(rp, w, 3u, &err) == SU_OK && su_replay_play(rp, w, 100u, &done, &err) == SU_OK &&
                  done == 27u,
              "replay seek and play verify every digest");
        check(su_host_digest(w, now) == SU_OK && memcmp(now, dg, 32u) == 0,
              "replayed end digest equals the recording process's");
    }
    su_replay_close(rp);
    free(bytes);
    free(snap);
    free(dg);
    su_host_world_destroy(w);
    scene_free(&sc);
    return 0;
}

int main(int argc, char **argv) {
    SuHostOperatorDesc d, d4, d4s;
    int rc = 0;

    if (argc >= 3 && strcmp(argv[1], "record") == 0) {
        rc = run_record(argv[2]);
    } else if (argc >= 4 && strcmp(argv[1], "verify") == 0) {
        rc = run_verify(argv[2], argv[3]);
    } else {
        test_registration();
        hits_desc(&d, "game.hits", 16u, V_SAME);
        test_order(&d);
        {
            SuHostError err;
            hits_desc(&d4, "game.cap", 4u, V_SAME);
            check(su_host_register_operator(&d4, &err) == SU_OK, "register game.cap: %s", err.message);
            hits_desc(&d4s, "game.capswallow", 4u, V_SAME);
            check(su_host_register_operator(&d4s, &err) == SU_OK, "register game.capswallow: %s", err.message);
        }
        test_cap(&d4, &d4s);
        test_world_cap(&d);
        test_coverage(&d);
        test_snapshot(&d);
        test_replay(&d);
        test_old_operator();
        test_no_alloc(&d);
    }
    if (g_failures != 0u) {
        fprintf(stderr, "%u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("%u checks passed\n", g_checks);
    return rc;
}
