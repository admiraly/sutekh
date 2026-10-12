/* M2-00c3 entity tick integration (runtime/entity_tick.c over runtime/schedule.c).
 *
 * The system is tests/fixtures/ir02/ok_valid (IR 0.2, movement with despawn_self and emit_event):
 * a row dies when position < velocity, emits kind 7 with (slot, next) and stores next. Checks:
 *   - an independent per-tick oracle (plain C, from the rule above and the contract) predicts the
 *     committed columns, the alive flags and every event record, tick by tick;
 *   - workers 1, 2 and 4 give byte-identical canonical state and event log dumps (5000 rows: five
 *     storage chunks with a short last chunk);
 *   - a per-tick cap overflow aborts the tick with SU_E_RESOURCE_LIMIT and leaves the committed table,
 *     columns and event log unchanged;
 *   - the event ring wraps: events.read reports skipped and dropped counts that match the oracle.
 *
 * Row sequence: despawn_self and emit_event on one row share a single per-row sequence (contract r2
 * section 2). The fixture's despawn precedes its event in instruction order, so the despawn record
 * takes seq 0 and the event seq 1. The oracle encodes that order.
 *
 * Build and run from the repository root:
 *   cmake --build <dir> --target test_entity_tick && <dir>/test_entity_tick
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "runtime/entity_tick.h"
#include "runtime/schedule.h"
#include "sutekh/abi.h"
#include "sutekh/entities.h"
#include "sutekh/status.h"
#include "world/entities/su_cmdq.h"
#include "world/entities/su_ents.h"
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

#define SYS_CAP "tests/fixtures/ir02/ok_valid.capsule.json"
#define SYS_LIR "tests/fixtures/ir02/ok_valid.lir.json"
#define N_ROWS 5000u
#define MAX_ENT 6000u
#define TICKS 8u

/* ---------- files and JSON ---------- */

typedef struct Buf {
    uint8_t *p;
    size_t n;
} Buf;

static int read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    long sz;
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return 0;
    }
    rewind(f);
    b->p = malloc((size_t)sz + 1u);
    if (b->p == NULL) {
        fclose(f);
        return 0;
    }
    b->n = fread(b->p, 1, (size_t)sz, f);
    fclose(f);
    return b->n == (size_t)sz;
}

static const SuJsonLimits k_limits = {16u * 1024u * 1024u, 32u};

static const char *json_str(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    return v != NULL && v->type == SU_JSON_STRING ? v->u.string.data : NULL;
}

/* Verifies the capsule and its IR (path relative to the repository root). */
static uint32_t load_module(SuLirModule *mod, uint32_t *gen, uint32_t *sv) {
    Buf cb = {0};
    Buf lb = {0};
    SuJsonDoc cdoc;
    SuJsonError jerr;
    SuDiag diag;
    const SuJsonValue *schema;
    const char *src;
    char lir_path[512];
    uint64_t g = 0;
    uint64_t v = 0;
    uint32_t st = SU_E_INVALID_ARGUMENT;

    if (!read_file(SYS_CAP, &cb) || su_json_parse(cb.p, cb.n, &k_limits, &cdoc, &jerr) != SU_OK) {
        free(cb.p);
        return st;
    }
    schema = su_json_member(cdoc.root, "schema");
    src = json_str(cdoc.root, "source");
    if (schema != NULL && src != NULL && su_json_get_uint(su_json_member(schema, "layout_generation"), &g) &&
        su_json_get_uint(su_json_member(schema, "version"), &v)) {
        snprintf(lir_path, sizeof lir_path, "%s", SYS_LIR);
        if (read_file(lir_path, &lb)) {
            st = su_lir_verify(cb.p, cb.n, lb.p, lb.n, mod, &diag);
            if (st != SU_OK) {
                fprintf(stderr, "verify: %s (%s)\n", su_status_name(st), diag.message);
            }
        }
    }
    *gen = (uint32_t)g;
    *sv = (uint32_t)v;
    su_json_doc_free(&cdoc);
    free(cb.p);
    free(lb.p);
    return st;
}

/* ---------- worlds and schedules ---------- */

static uint32_t build_sched(SuSchedule *s, uint64_t struct_cap, uint64_t event_cap, uint64_t log_max) {
    SuLirModule *m = malloc(sizeof *m);
    uint32_t gen = 0;
    uint32_t sv = 0;
    uint32_t st;
    if (m == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memset(m, 0, sizeof *m);
    st = load_module(m, &gen, &sv);
    if (st != SU_OK) {
        free(m);
        return st;
    }
    su_sched_init(s);
    st = su_sched_add(s, m, sv, gen, 0, 1, 0);
    if (st != SU_OK) {
        su_lir_module_free(m);
        free(m);
        return st;
    }
    st = su_sched_finalize(s);
    if (st != SU_OK) {
        return st;
    }
    return su_sched_enable_entities(s, struct_cap, event_cap, log_max);
}

static SuWorld *make_world(uint32_t rows, uint32_t cap) {
    SuWorldFieldDesc fd[2];
    SuWorldDesc d;
    SuWorld *w = NULL;
    uint32_t *ids = calloc(rows, sizeof *ids);
    uint32_t i;
    if (ids == NULL) {
        return NULL;
    }
    for (i = 0; i < rows; i++) {
        ids[i] = i + 1u;
    }
    fd[0].id = 1u;
    fd[0].type = SU_TYPE_U32;
    fd[0].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
    fd[0].name = "position_x";
    fd[1].id = 2u;
    fd[1].type = SU_TYPE_U32;
    fd[1].access = SU_ACCESS_READ;
    fd[1].name = "velocity_x";
    memset(&d, 0, sizeof d);
    d.schema_id = "demo.actor";
    d.schema_version = 1u;
    d.layout_generation = 1u;
    d.row_count = rows;
    d.entity_ids = ids;
    d.fields = fd;
    d.field_count = 2u;
    if (su_world_create_ex(&d, cap, &w) != SU_OK) {
        w = NULL;
    }
    free(ids);
    return w;
}

/* Deterministic initial values: positions in [0,300), velocities in [0,40): a steady trickle of
 * deaths over the ticks. Same generator for the world and the oracle. */
static uint32_t pos0(uint32_t i) {
    return (i * 2654435761u) % 300u;
}
static uint32_t vel0(uint32_t i) {
    return (i * 40503u + 17u) % 40u;
}

static uint32_t init_world(SuWorld *w, uint32_t rows) {
    uint32_t *p = malloc((size_t)rows * sizeof *p);
    uint32_t *v = malloc((size_t)rows * sizeof *v);
    uint32_t i;
    uint32_t st = SU_E_RESOURCE_LIMIT;
    if (p == NULL || v == NULL) {
        goto out;
    }
    for (i = 0; i < rows; i++) {
        p[i] = pos0(i);
        v[i] = vel0(i);
    }
    st = su_world_init_committed(w, 1u, p, rows);
    if (st == SU_OK) {
        st = su_world_init_committed(w, 2u, v, rows);
    }
out:
    free(p);
    free(v);
    return st;
}

/* ---------- independent oracle (contract rules, no engine code) ---------- */

typedef struct OrRec {
    uint64_t event_seq;
    uint64_t tick;
    uint32_t kind;
    uint32_t slot;
    uint32_t seq;
    uint32_t a;
    uint32_t b;
} OrRec;

typedef struct Oracle {
    uint32_t rows;
    uint32_t *p;
    uint32_t *v;
    uint8_t *alive;
    uint64_t tick;
    uint64_t next_seq;
    OrRec *recs;
    uint64_t nrecs;
    uint64_t cap;
} Oracle;

static int oracle_init(Oracle *o, uint32_t rows, uint64_t cap) {
    uint32_t i;
    memset(o, 0, sizeof *o);
    o->rows = rows;
    o->p = malloc((size_t)rows * sizeof *o->p);
    o->v = malloc((size_t)rows * sizeof *o->v);
    o->alive = malloc(rows);
    o->recs = malloc((size_t)cap * sizeof *o->recs);
    o->cap = cap;
    if (o->p == NULL || o->v == NULL || o->alive == NULL || o->recs == NULL) {
        return 0;
    }
    for (i = 0; i < rows; i++) {
        o->p[i] = pos0(i);
        o->v[i] = vel0(i);
        o->alive[i] = 1;
    }
    return 1;
}

static void oracle_free(Oracle *o) {
    free(o->p);
    free(o->v);
    free(o->alive);
    free(o->recs);
}

static void oracle_push(Oracle *o, uint64_t tick, uint32_t kind, uint32_t slot, uint32_t seq, uint32_t a,
                        uint32_t b) {
    OrRec *r;
    if (o->nrecs >= o->cap) {
        return;
    }
    r = &o->recs[o->nrecs++];
    r->event_seq = o->next_seq++;
    r->tick = tick;
    r->kind = kind;
    r->slot = slot;
    r->seq = seq;
    r->a = a;
    r->b = b;
    (void)tick;
}

/* One tick: rows in ascending slot. Alive row: c = p < v; emit kind 7 (slot, next) when c, despawn
 * when c; the store writes next to position_x for every alive row. Dead rows are zero. The record
 * order within a despawning row is the implemented one (event first, see the header note). */
static void oracle_tick(Oracle *o) {
    uint32_t i;
    o->tick++;
    for (i = 0; i < o->rows; i++) {
        uint32_t next;
        int c;
        if (!o->alive[i]) {
            continue;
        }
        next = o->p[i] + o->v[i];
        c = o->p[i] < o->v[i];
        if (c) {
            /* The program is despawn_self (per-row seq 0) then emit_event (seq 1). */
            oracle_push(o, o->tick, SU_EVENT_DESPAWNED, i, 0u, i, 1u);
            oracle_push(o, o->tick, 7u, i, 1u, i, next);
            o->alive[i] = 0;
            o->p[i] = 0u;
            o->v[i] = 0u;
        } else {
            o->p[i] = next;
        }
    }
}

static int oracle_matches_world(const Oracle *o, SuWorld *w) {
    const uint32_t *pd = NULL;
    const uint32_t *vd = NULL;
    uint64_t rc = 0;
    uint32_t i;
    const SuEnts *t = su_world_entities_committed(w);
    if (su_world_read_committed(w, 1u, &pd, &rc) != SU_OK || su_world_read_committed(w, 2u, &vd, &rc) != SU_OK) {
        return 0;
    }
    for (i = 0; i < o->rows; i++) {
        if (su_ents_slot_alive(t, i) != (int)o->alive[i] || pd[i] != o->p[i] || vd[i] != o->v[i]) {
            return 0;
        }
    }
    return 1;
}

/* True when the log holds exactly the oracle's records (the oracle run never wraps the ring). */
static int log_matches_oracle(const SuEventLog *log, const Oracle *o) {
    SuEvent buf[256];
    uint64_t seen = 0;
    int ok = 1;
    if (su_evlog_next_seq(log) != o->next_seq || su_evlog_dropped(log) != 0u) {
        return 0;
    }
    while (seen < o->nrecs) {
        uint64_t n = 0;
        uint64_t next_cur = 0;
        uint64_t skipped = 0;
        uint64_t dropped = 0;
        uint64_t k;
        if (su_evlog_read(log, seen, 256u, buf, &n, &next_cur, &skipped, &dropped) != SU_OK || n == 0u ||
            skipped != 0u) {
            return 0;
        }
        for (k = 0; k < n && seen < o->nrecs; k++, seen++) {
            const OrRec *r = &o->recs[seen];
            if (buf[k].event_seq != r->event_seq || buf[k].tick != r->tick || buf[k].kind != r->kind ||
                buf[k].slot != r->slot || buf[k].seq != r->seq || buf[k].a != r->a || buf[k].b != r->b) {
                ok = 0;
            }
        }
    }
    return ok;
}

/* ---------- canonical bytes ---------- */

static uint8_t *world_bytes(SuWorld *w, size_t *len) {
    size_t sz = 0;
    uint8_t *b;
    if (su_world_canonical_size(w, &sz) != SU_OK) {
        return NULL;
    }
    b = malloc(sz > 0u ? sz : 1u);
    if (b == NULL || su_world_canonical_write(w, b, sz, len) != SU_OK) {
        free(b);
        return NULL;
    }
    return b;
}

static uint8_t *log_bytes(SuEventLog *log, uint64_t *len) {
    uint64_t sz = 0;
    uint8_t *b;
    if (su_evlog_dump(log, NULL, 0u, &sz) != SU_OK) {
        return NULL;
    }
    b = malloc(sz > 0u ? (size_t)sz : 1u);
    if (b == NULL || su_evlog_dump(log, b, sz, len) != SU_OK) {
        free(b);
        return NULL;
    }
    return b;
}

/* ---------- runs ---------- */

typedef struct RunOut {
    uint8_t *world;
    size_t world_len;
    uint8_t *log;
    uint64_t log_len;
} RunOut;

static void run_out_free(RunOut *r) {
    free(r->world);
    free(r->log);
    memset(r, 0, sizeof *r);
}

/* Runs TICKS ticks with the given worker count; fills r with the final canonical bytes. */
static uint32_t run_workers(uint32_t workers, RunOut *r, SuSchedule **keep_sched, SuWorld **keep_world) {
    SuSchedule *s = malloc(sizeof *s);
    SuWorld *w = make_world(N_ROWS, MAX_ENT);
    SuSchedTickOpts opt;
    uint64_t done = 0;
    uint32_t fi = UINT32_MAX;
    uint32_t st;

    memset(r, 0, sizeof *r);
    if (s == NULL || w == NULL) {
        free(s);
        su_world_destroy(w);
        return SU_E_RESOURCE_LIMIT;
    }
    st = build_sched(s, 4096u, 4096u, 1u << 20);
    if (st == SU_OK) {
        st = init_world(w, N_ROWS);
    }
    memset(&opt, 0, sizeof opt);
    opt.workers = workers;
    opt.chunk_min_rows = 1u;
    if (st == SU_OK) {
        st = su_sched_run_ticks_opt(w, s, TICKS, NULL, NULL, &opt, &done, &fi);
    }
    if (st == SU_OK) {
        r->world = world_bytes(w, &r->world_len);
        r->log = log_bytes(su_ent_rt_log(s->ent), &r->log_len);
    }
    if (keep_sched != NULL && keep_world != NULL) {
        *keep_sched = s;
        *keep_world = w;
    } else {
        su_sched_free(s);
        free(s);
        su_world_destroy(w);
    }
    return st;
}

/* Oracle vs the engine, tick by tick. */
static void test_oracle(void) {
    SuSchedule *s = malloc(sizeof *s);
    SuWorld *w = make_world(N_ROWS, MAX_ENT);
    SuSchedTickOpts opt;
    Oracle o;
    uint64_t t;
    uint64_t done = 0;
    uint32_t fi = UINT32_MAX;
    uint64_t max_recs = (uint64_t)N_ROWS * 2u * TICKS + 16u;

    CHECK(s != NULL && w != NULL);
    if (s == NULL || w == NULL) {
        return;
    }
    CHECK_ST(build_sched(s, 4096u, 4096u, 1u << 20), SU_OK);
    CHECK_ST(init_world(w, N_ROWS), SU_OK);
    CHECK(oracle_init(&o, N_ROWS, max_recs));
    memset(&opt, 0, sizeof opt);
    opt.workers = 2u;
    opt.chunk_min_rows = 1u;
    for (t = 0; t < TICKS; t++) {
        uint32_t st = su_sched_run_ticks_opt(w, s, 1u, NULL, NULL, &opt, &done, &fi);
        oracle_tick(&o);
        CHECK(st == SU_OK);
        CHECK(oracle_matches_world(&o, w));
        CHECK(log_matches_oracle(su_ent_rt_log(s->ent), &o));
    }
    CHECK(o.nrecs > 0u);
    {
        uint32_t alive_oracle = 0;
        uint32_t i;
        for (i = 0; i < o.rows; i++) {
            alive_oracle += o.alive[i];
        }
        CHECK(alive_oracle < N_ROWS);
        CHECK(su_ents_alive_count(su_world_entities_committed(w)) == alive_oracle);
    }
    oracle_free(&o);
    su_sched_free(s);
    free(s);
    su_world_destroy(w);
}

static void test_workers_equal(void) {
    RunOut r1;
    RunOut r2;
    RunOut r4;
    uint32_t st;

    st = run_workers(1u, &r1, NULL, NULL);
    CHECK_ST(st, SU_OK);
    st = run_workers(2u, &r2, NULL, NULL);
    CHECK_ST(st, SU_OK);
    st = run_workers(4u, &r4, NULL, NULL);
    CHECK_ST(st, SU_OK);
    CHECK(r1.world != NULL && r2.world != NULL && r4.world != NULL);
    CHECK(r1.log != NULL && r2.log != NULL && r4.log != NULL);
    if (r1.world != NULL && r2.world != NULL && r4.world != NULL) {
        CHECK(r1.world_len == r2.world_len && r1.world_len == r4.world_len);
        CHECK(memcmp(r1.world, r2.world, r1.world_len) == 0);
        CHECK(memcmp(r1.world, r4.world, r1.world_len) == 0);
    }
    if (r1.log != NULL && r2.log != NULL && r4.log != NULL) {
        CHECK(r1.log_len == r2.log_len && r1.log_len == r4.log_len);
        CHECK(memcmp(r1.log, r2.log, (size_t)r1.log_len) == 0);
        CHECK(memcmp(r1.log, r4.log, (size_t)r1.log_len) == 0);
    }
    run_out_free(&r1);
    run_out_free(&r2);
    run_out_free(&r4);
}

/* A per-tick cap of 1 despawn overflows in the first tick that kills two rows: the tick aborts and
 * committed table, columns and log are the ones from before the call. */
static void test_cap_abort(void) {
    SuSchedule *s = malloc(sizeof *s);
    SuWorld *w = make_world(N_ROWS, MAX_ENT);
    SuSchedTickOpts opt;
    uint64_t done = 0;
    uint32_t fi = UINT32_MAX;
    uint8_t *before_w;
    uint8_t *after_w;
    uint8_t *before_l;
    uint8_t *after_l;
    size_t lw0 = 0;
    size_t lw1 = 0;
    uint64_t ll0 = 0;
    uint64_t ll1 = 0;
    uint32_t st;

    CHECK(s != NULL && w != NULL);
    if (s == NULL || w == NULL) {
        return;
    }
    CHECK_ST(build_sched(s, 1u, 4096u, 1u << 20), SU_OK);
    CHECK_ST(init_world(w, N_ROWS), SU_OK);
    memset(&opt, 0, sizeof opt);
    opt.workers = 4u;
    opt.chunk_min_rows = 1u;
    before_w = world_bytes(w, &lw0);
    before_l = log_bytes(su_ent_rt_log(s->ent), &ll0);
    CHECK(before_w != NULL && before_l != NULL);
    st = su_sched_run_ticks_opt(w, s, TICKS, NULL, NULL, &opt, &done, &fi);
    CHECK_ST(st, SU_E_RESOURCE_LIMIT);
    CHECK(done == 0u);
    CHECK(fi == 0u);
    after_w = world_bytes(w, &lw1);
    after_l = log_bytes(su_ent_rt_log(s->ent), &ll1);
    CHECK(after_w != NULL && after_l != NULL);
    if (before_w != NULL && after_w != NULL) {
        CHECK(lw0 == lw1 && memcmp(before_w, after_w, lw0) == 0);
    }
    if (before_l != NULL && after_l != NULL) {
        CHECK(ll0 == ll1 && memcmp(before_l, after_l, (size_t)ll0) == 0);
    }
    CHECK(su_world_tick(w) == 0u);
    CHECK(su_world_in_tick(w) == 0);
    CHECK(su_ents_alive_count(su_world_entities_committed(w)) == N_ROWS);
    free(before_w);
    free(after_w);
    free(before_l);
    free(after_l);
    su_sched_free(s);
    free(s);
    su_world_destroy(w);
}

/* Event ring: log capacity 64 over a run that emits far more; events.read reports the skipped count
 * from the oldest retained record, and dropped matches the oracle's eviction count. */
static void test_ring_wrap(void) {
    SuSchedule *s = malloc(sizeof *s);
    SuWorld *w = make_world(N_ROWS, MAX_ENT);
    SuSchedTickOpts opt;
    Oracle o;
    uint64_t done = 0;
    uint32_t fi = UINT32_MAX;
    uint32_t st;
    SuEvent buf[256];
    uint64_t n = 0;
    uint64_t next_cur = 0;
    uint64_t skipped = 0;
    uint64_t dropped = 0;

    CHECK(s != NULL && w != NULL);
    if (s == NULL || w == NULL) {
        return;
    }
    CHECK_ST(build_sched(s, 4096u, 4096u, 64u), SU_OK);
    CHECK_ST(init_world(w, N_ROWS), SU_OK);
    CHECK(oracle_init(&o, N_ROWS, (uint64_t)N_ROWS * 2u * TICKS + 16u));
    memset(&opt, 0, sizeof opt);
    opt.workers = 2u;
    opt.chunk_min_rows = 1u;
    st = su_sched_run_ticks_opt(w, s, TICKS, NULL, NULL, &opt, &done, &fi);
    CHECK_ST(st, SU_OK);
    {
        uint64_t t;
        for (t = 0; t < TICKS; t++) {
            oracle_tick(&o);
        }
    }
    CHECK(su_evlog_capacity(su_ent_rt_log(s->ent)) == 64u);
    CHECK(su_evlog_count(su_ent_rt_log(s->ent)) == 64u);
    CHECK(su_evlog_next_seq(su_ent_rt_log(s->ent)) == o.next_seq);
    CHECK(su_evlog_dropped(su_ent_rt_log(s->ent)) == o.next_seq - 64u);
    CHECK_ST(su_evlog_read(su_ent_rt_log(s->ent), 0u, 256u, buf, &n, &next_cur, &skipped, &dropped), SU_OK);
    CHECK(n == 64u);
    CHECK(skipped == o.next_seq - 64u);
    CHECK(dropped == o.next_seq - 64u);
    CHECK(next_cur == o.next_seq);
    CHECK(n > 0u && buf[0].event_seq == o.next_seq - 64u);
    CHECK_ST(su_evlog_read(su_ent_rt_log(s->ent), o.next_seq + 1u, 256u, buf, &n, &next_cur, &skipped, &dropped),
             SU_E_INVALID_ARGUMENT);
    oracle_free(&o);
    su_sched_free(s);
    free(s);
    su_world_destroy(w);
}

int main(void) {
    test_oracle();
    test_workers_equal();
    test_cap_abort();
    test_ring_wrap();
    printf("test_entity_tick: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
