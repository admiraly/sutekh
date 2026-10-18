/* M1 closure B tests: tick workers on the live tick path (runtime/schedule.c over runtime/jobs).
 *
 * The function under test is su_sched_run_ticks_opt, the one world.step calls in the worker, with
 * SuSchedTickOpts exactly as the worker builds it (workers from tick_workers, chunk_min_rows from
 * chunk_min_rows). Each check runs a serial world (workers 1) and a chunked world in lockstep, tick
 * by tick, and compares their committed columns byte for byte:
 *   - movement (tests/fixtures/t02), every t03 edge program, and the two-system pipeline
 *     (tests/scheduler/fixtures/pipe), each on a generated 100,001-row world (50 ticks) and a
 *     generated 1,000,000-row world (3 ticks), for workers 2 and 4;
 *   - the chunk seam proves the chunked path ran (calls per tick) and that workers 1 never reaches it;
 *   - a chunk threshold above the row count routes the system to the serial path;
 *   - failure: a chunk that fails inside a live tick aborts the whole tick (committed state unchanged
 *     after the failed call, the failed system index is reported), and the next clean tick matches the
 *     serial world again.
 *
 * Build and run from the repository root (fixture paths are relative):
 *   cmake --build <dir> --target test_tick_workers && <dir>/test_tick_workers
 */
#define _POSIX_C_SOURCE 200809L

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ir/su_json.h"
#include "platform/su_platform.h"
#include "ir/su_lir.h"
#include "runtime/jobs/su_jobs.h"
#include "runtime/schedule.h"
#include "sutekh/abi.h"
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

/* ---------- files and JSON (as in test_scheduler.c) ---------- */

typedef struct Buf {
    uint8_t *p;
    size_t n;
} Buf;

static int read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    long sz;
    size_t got;
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
    got = fread(b->p, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(b->p);
        b->p = NULL;
        return 0;
    }
    b->n = (size_t)sz;
    return 1;
}

static void join_rel(const char *base_file, const char *rel, char *out, size_t cap) {
    const char *slash = strrchr(base_file, '/');
    size_t dir_len = slash != NULL ? (size_t)(slash - base_file) + 1u : 0u;
    snprintf(out, cap, "%.*s%s", (int)dir_len, base_file, rel);
}

static const SuJsonLimits k_limits = {16u * 1024u * 1024u, 32u};

static const char *json_str(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    return v != NULL && v->type == SU_JSON_STRING ? v->u.string.data : NULL;
}

static const SuJsonValue *json_obj(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    return v != NULL && v->type == SU_JSON_OBJECT ? v : NULL;
}

/* ---------- modules and schedules ---------- */

typedef struct Loaded {
    SuLirModule mod;
    uint32_t gen;
    uint32_t schema_version;
} Loaded;

/* Verifies a capsule with the IR named by its "source" (relative to the capsule). */
static int load_sys(const char *cap_path, Loaded *L) {
    Buf cb = {0};
    Buf lb = {0};
    SuJsonDoc cdoc;
    SuJsonError jerr;
    SuDiag diag;
    char lir_path[512];
    uint64_t gen = 0;
    uint64_t sv = 0;
    const SuJsonValue *schema;
    const char *src;
    uint32_t st;
    int ok = 0;

    memset(L, 0, sizeof *L);
    if (!read_file(cap_path, &cb) || su_json_parse(cb.p, cb.n, &k_limits, &cdoc, &jerr) != SU_OK) {
        free(cb.p);
        return 0;
    }
    schema = json_obj(cdoc.root, "schema");
    if (schema == NULL || !su_json_get_uint(su_json_member(schema, "layout_generation"), &gen) ||
        gen > UINT32_MAX || !su_json_get_uint(su_json_member(schema, "version"), &sv) || sv > UINT32_MAX) {
        goto done;
    }
    src = json_str(cdoc.root, "source");
    if (src == NULL) {
        goto done;
    }
    join_rel(cap_path, src, lir_path, sizeof lir_path);
    if (!read_file(lir_path, &lb)) {
        fprintf(stderr, "cannot read %s\n", lir_path);
        goto done;
    }
    st = su_lir_verify(cb.p, cb.n, lb.p, lb.n, &L->mod, &diag);
    if (st != SU_OK) {
        fprintf(stderr, "verify %s: %s (%s)\n", lir_path, su_status_name(st), diag.message);
        goto done;
    }
    L->gen = (uint32_t)gen;
    L->schema_version = (uint32_t)sv;
    ok = 1;
done:
    su_json_doc_free(&cdoc);
    free(cb.p);
    free(lb.p);
    return ok;
}

/* Builds the schedule of the loaded systems in order. Ownership of each module moves to the schedule. */
static uint32_t build_schedule(SuSchedule *s, Loaded *L, uint32_t n) {
    uint32_t i;
    uint32_t st;
    su_sched_init(s);
    for (i = 0; i < n; i++) {
        SuLirModule *m = malloc(sizeof *m);
        if (m == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        *m = L[i].mod;
        memset(&L[i].mod, 0, sizeof L[i].mod);
        st = su_sched_add(s, m, L[i].schema_version, L[i].gen, 0, 1, 0);
        if (st != SU_OK) {
            su_lir_module_free(m);
            free(m);
            return st;
        }
    }
    return su_sched_finalize(s);
}

/* ---------- worlds ---------- */

static SuWorld *make_world(uint64_t rows, uint32_t gen) {
    SuWorldFieldDesc fd[2];
    SuWorldDesc d;
    SuWorld *w = NULL;
    uint32_t *ids = calloc(rows > 0u ? (size_t)rows : 1u, sizeof *ids);
    uint64_t i;
    if (ids == NULL) {
        return NULL;
    }
    for (i = 0; i < rows; i++) {
        ids[i] = (uint32_t)(i + 1u);
    }
    fd[0].id = 1u;
    fd[0].type = SU_TYPE_U32;
    fd[0].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
    fd[0].name = "position_x";
    fd[1].id = 2u;
    fd[1].type = SU_TYPE_U32;
    fd[1].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
    fd[1].name = "velocity_x";
    d.schema_id = "demo.actor";
    d.schema_version = 1u;
    d.layout_generation = gen;
    d.row_count = rows;
    d.entity_ids = ids;
    d.fields = fd;
    d.field_count = 2u;
    if (su_world_create(&d, &w) != SU_OK) {
        w = NULL;
    }
    free(ids);
    return w;
}

static uint32_t mix32(uint64_t x) {
    uint64_t z = x + UINT64_C(0x9E3779B97F4A7C15);
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return (uint32_t)(z ^ (z >> 31));
}

/* Deterministic columns with equality hits and the u32 edge values. */
static int init_world(SuWorld *w, uint64_t rows, uint64_t seed) {
    uint32_t *a = malloc((rows > 0u ? (size_t)rows : 1u) * sizeof *a);
    uint32_t *b = malloc((rows > 0u ? (size_t)rows : 1u) * sizeof *b);
    uint64_t i;
    int ok = 0;
    if (a == NULL || b == NULL) {
        goto done;
    }
    for (i = 0; i < rows; i++) {
        uint64_t k = seed * UINT64_C(1000003) + i * 2u;
        a[i] = mix32(k);
        b[i] = mix32(k + 1u);
        if (i % 5u == 0u) {
            b[i] = a[i];
        }
        if (i % 11u == 3u) {
            a[i] = UINT32_MAX;
        }
        if (i % 13u == 4u) {
            b[i] = 0u;
        }
    }
    ok = su_world_init_committed(w, 1u, a, rows) == SU_OK && su_world_init_committed(w, 2u, b, rows) == SU_OK;
done:
    free(a);
    free(b);
    return ok;
}

/* Both committed columns, pos then vel, into a malloc'd buffer (NULL on error). */
static uint32_t *snapshot(SuWorld *w, uint64_t rows) {
    uint32_t *out = calloc((size_t)(2u * (rows > 0u ? rows : 1u)), sizeof *out);
    const uint32_t *p = NULL;
    const uint32_t *v = NULL;
    uint64_t rc = 0;
    if (out == NULL) {
        return NULL;
    }
    if (su_world_read_committed(w, 1u, &p, &rc) != SU_OK || rc != rows ||
        su_world_read_committed(w, 2u, &v, &rc) != SU_OK || rc != rows) {
        free(out);
        return NULL;
    }
    if (rows > 0u) {
        memcpy(out, p, (size_t)rows * sizeof *out);
        memcpy(out + rows, v, (size_t)rows * sizeof *out);
    }
    return out;
}

static int same_snapshot(const uint32_t *a, const uint32_t *b, uint64_t rows) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    return rows == 0u || memcmp(a, b, (size_t)(2u * rows) * sizeof *a) == 0;
}

/* ---------- chunk seam ---------- */

/* Counts chunk callbacks. fail_ordinal (1-based, 0 = never) fails exactly the callback with that
 * ordinal in the current tick. Callbacks run on pool threads, so the counter is atomic. Chunks of
 * one system all run (nothing is cancelled), so the ordinal selects a system deterministically:
 * ordinals 1..chunks(sys 0) are system 0, the next ones system 1. */
typedef struct SeamState {
    atomic_uint calls;
    atomic_uint fail_ordinal;
} SeamState;

static int seam_fn(uint32_t chunk, void *ctx) {
    SeamState *s = ctx;
    uint32_t n = atomic_fetch_add(&s->calls, 1u) + 1u;
    (void)chunk;
    return atomic_load(&s->fail_ordinal) != 0u && n == atomic_load(&s->fail_ordinal);
}

static void seam_init(SeamState *s) {
    atomic_init(&s->calls, 0u);
    atomic_init(&s->fail_ordinal, 0u);
}

static void tick_opts(SuSchedTickOpts *o, uint32_t workers, uint64_t chunk_min_rows, SeamState *seam) {
    memset(o, 0, sizeof *o);
    o->workers = workers;
    o->chunk_min_rows = chunk_min_rows;
    o->chunk_fault = seam_fn;
    o->chunk_ctx = seam;
}

/* Chunk callbacks one tick makes over `rows` rows for `nsys` systems on the chunked path. */
static uint64_t expected_calls(uint64_t rows, uint32_t nsys) {
    return (uint64_t)nsys * su_jobs_chunk_count(rows, 0u);
}

static uint32_t run_one(SuWorld *w, SuSchedule *s, const SuSchedTickOpts *o, uint32_t *fi) {
    uint64_t done = 0;
    return su_sched_run_ticks_opt(w, s, 1u, NULL, NULL, o, &done, fi);
}

/* ---------- lockstep: serial and chunked worlds, tick by tick ---------- */

static const char *const k_movement[] = {"tests/fixtures/t02/movement.capsule.json"};
static const char *const k_t03_names[] = {"eq_edges", "lt_edges", "mul_overflow", "select_min", "sub_underflow",
                                          "wrap_add"};
static const char *const k_pipe[] = {"tests/scheduler/fixtures/pipe/advance.capsule.json",
                                     "tests/scheduler/fixtures/pipe/damp.capsule.json"};

/* Runs `ticks` ticks of the systems in caps on a serial world and on a chunked world with `workers`
 * workers (threshold 1 row), both over the same generated columns, and compares the committed
 * columns after every tick. Returns 1 when every tick is identical and the chunked world made exactly
 * the expected number of chunk callbacks each tick while the serial world made none. */
static int lockstep(const char *label, const char *const *caps, uint32_t ncaps, uint64_t rows, uint64_t ticks,
                    uint32_t workers, uint64_t seed) {
    Loaded la[SU_SCHED_MAX_SYSTEMS];
    Loaded lb[SU_SCHED_MAX_SYSTEMS];
    SuSchedule sa;
    SuSchedule sb;
    SuWorld *wa = NULL;
    SuWorld *wb = NULL;
    SeamState seam_a;
    SeamState seam_b;
    SuSchedTickOpts oa;
    SuSchedTickOpts ob;
    uint32_t i;
    uint64_t t;
    uint32_t *init = NULL;
    uint32_t *sa_snap = NULL;
    uint32_t *sb_snap = NULL;
    int ok = 0;
    int loaded = 1;
    uint32_t gen = 0;
    uint32_t st_a;
    uint32_t st_b;
    uint32_t fi = 0;

    memset(la, 0, sizeof la);
    memset(lb, 0, sizeof lb);
    for (i = 0; i < ncaps && loaded; i++) {
        loaded = load_sys(caps[i], &la[i]) && load_sys(caps[i], &lb[i]);
        if (!loaded) {
            fprintf(stderr, "%s: cannot load %s\n", label, caps[i]);
        }
    }
    CHECK(loaded);
    if (!loaded) {
        goto done;
    }
    gen = la[0].gen;
    if (build_schedule(&sa, la, ncaps) != SU_OK || build_schedule(&sb, lb, ncaps) != SU_OK) {
        fprintf(stderr, "%s: schedule build failed\n", label);
        goto done;
    }
    wa = make_world(rows, gen);
    wb = make_world(rows, gen);
    if (wa == NULL || wb == NULL || !init_world(wa, rows, seed) || !init_world(wb, rows, seed)) {
        fprintf(stderr, "%s: world setup failed\n", label);
        goto done;
    }
    init = snapshot(wa, rows);
    seam_init(&seam_a);
    seam_init(&seam_b);
    tick_opts(&oa, 1u, 1u, &seam_a);
    tick_opts(&ob, workers, 1u, &seam_b);
    for (t = 0; t < ticks; t++) {
        uint64_t calls_b;
        atomic_store(&seam_a.calls, 0u);
        atomic_store(&seam_b.calls, 0u);
        st_a = run_one(wa, &sa, &oa, &fi);
        st_b = run_one(wb, &sb, &ob, &fi);
        CHECK_ST(st_a, SU_OK);
        CHECK_ST(st_b, SU_OK);
        calls_b = atomic_load(&seam_b.calls);
        CHECK(atomic_load(&seam_a.calls) == 0u);
        CHECK(calls_b == expected_calls(rows, ncaps));
        free(sa_snap);
        free(sb_snap);
        sa_snap = snapshot(wa, rows);
        sb_snap = snapshot(wb, rows);
        if (!same_snapshot(sa_snap, sb_snap, rows)) {
            fprintf(stderr, "%s: workers=%u diverge from serial at tick %llu\n", label, (unsigned)workers,
                    (unsigned long long)(t + 1u));
            ok = 0;
            break;
        }
    }
    if (t == ticks) {
        ok = 1;
    }
    CHECK(ok);

done:
    free(init);
    free(sa_snap);
    free(sb_snap);
    su_world_destroy(wa);
    su_world_destroy(wb);
    su_sched_free(&sa);
    su_sched_free(&sb);
    for (i = 0; i < ncaps; i++) {
        su_lir_module_free(&la[i].mod);
        su_lir_module_free(&lb[i].mod);
    }
    return ok;
}

/* Lockstep over the configurations of the closure: every config at 100,001 rows for 50 ticks and at
 * 1,000,000 rows for 3 ticks, workers 2 and 4. */
static void test_lockstep_all(void) {
    static const uint32_t workers[] = {2u, 4u};
    uint32_t wi;
    size_t k;
    for (wi = 0; wi < 2u; wi++) {
        uint32_t n = workers[wi];
        CHECK(lockstep("movement 100001", k_movement, 1u, 100001u, 50u, n, 11u));
        CHECK(lockstep("pipeline 100001", k_pipe, 2u, 100001u, 50u, n, 12u));
        for (k = 0; k < sizeof k_t03_names / sizeof k_t03_names[0]; k++) {
            char cap[256];
            const char *one[1];
            snprintf(cap, sizeof cap, "tests/fixtures/t03/%s/capsule.json", k_t03_names[k]);
            one[0] = cap;
            CHECK(lockstep(k_t03_names[k], one, 1u, 100001u, 50u, n, 13u + k));
        }
        CHECK(lockstep("movement 1000000", k_movement, 1u, 1000000u, 3u, n, 21u));
        CHECK(lockstep("pipeline 1000000", k_pipe, 2u, 1000000u, 3u, n, 22u));
    }
}

/* Failure on the live path: a chunk fails inside a tick of a two-system pipeline. The failed call
 * must report the system, commit nothing (the committed columns equal their value before the call,
 * even when the caller publishes), and a clean tick afterwards must match the serial world. */
static void test_failure_live(void) {
    Loaded la[2];
    Loaded lb[2];
    SuSchedule sa;
    SuSchedule sb;
    SuWorld *wa = make_world(100001u, 1u);
    SuWorld *wb = make_world(100001u, 1u);
    SeamState seam;
    SeamState seam_s;
    SuSchedTickOpts ob;
    SuSchedTickOpts os;
    uint32_t fi = 0;
    uint32_t *before = NULL;
    uint32_t *after = NULL;
    uint32_t *ref = NULL;
    uint64_t chunks0 = su_jobs_chunk_count(100001u, 0u);
    uint64_t done = 0;
    uint32_t i;
    uint32_t st;
    int loaded = load_sys(k_pipe[0], &la[0]) && load_sys(k_pipe[1], &la[1]) && load_sys(k_pipe[0], &lb[0]) &&
                 load_sys(k_pipe[1], &lb[1]);

    CHECK(loaded);
    CHECK(wa != NULL && wb != NULL);
    if (!loaded || wa == NULL || wb == NULL) {
        return;
    }
    CHECK_ST(build_schedule(&sa, la, 2u), SU_OK);
    CHECK_ST(build_schedule(&sb, lb, 2u), SU_OK);
    CHECK(init_world(wa, 100001u, 31u));
    CHECK(init_world(wb, 100001u, 31u));
    seam_init(&seam);
    seam_init(&seam_s);
    tick_opts(&ob, 4u, 1u, &seam);
    tick_opts(&os, 1u, 1u, &seam_s);
    for (i = 0; i < 5u; i++) {
        CHECK_ST(run_one(wa, &sa, &os, &fi), SU_OK);
        CHECK_ST(run_one(wb, &sb, &ob, &fi), SU_OK);
    }
    before = snapshot(wb, 100001u);
    CHECK(before != NULL);

    /* Fail the first chunk of the second system in one tick. */
    atomic_store(&seam.calls, 0u);
    atomic_store(&seam.fail_ordinal, (unsigned)(chunks0 + 1u));
    st = su_sched_run_ticks_opt(wb, &sb, 1u, NULL, NULL, &ob, &done, &fi);
    CHECK_ST(st, SU_E_CANDIDATE_FAILED);
    CHECK(fi == 1u);
    CHECK(done == 0u);
    after = snapshot(wb, 100001u);
    CHECK(same_snapshot(before, after, 100001u));
    free(after);
    after = NULL;

    /* Fail the first chunk of the first system. The lowest-index rule is a unit property (test_scheduler);
     * here the observable rule is the reported system index. */
    atomic_store(&seam.calls, 0u);
    atomic_store(&seam.fail_ordinal, 1u);
    st = su_sched_run_ticks_opt(wb, &sb, 1u, NULL, NULL, &ob, &done, &fi);
    CHECK_ST(st, SU_E_CANDIDATE_FAILED);
    CHECK(fi == 0u);
    after = snapshot(wb, 100001u);
    CHECK(same_snapshot(before, after, 100001u));
    free(after);
    after = NULL;

    /* Clean ticks resume: 3 more on both worlds must match. */
    atomic_store(&seam.fail_ordinal, 0u);
    for (i = 0; i < 3u; i++) {
        CHECK_ST(run_one(wa, &sa, &os, &fi), SU_OK);
        CHECK_ST(run_one(wb, &sb, &ob, &fi), SU_OK);
    }
    ref = snapshot(wa, 100001u);
    after = snapshot(wb, 100001u);
    CHECK(same_snapshot(ref, after, 100001u));

    free(before);
    free(after);
    free(ref);
    su_world_destroy(wa);
    su_world_destroy(wb);
    su_sched_free(&sa);
    su_sched_free(&sb);
}

/* A threshold above the row count routes every system to the serial path: the chunk seam is never
 * called, even with workers 4 and an armed failure. */
static void test_threshold(void) {
    Loaded l[1];
    SuSchedule s;
    SuWorld *w = make_world(5000u, 1u);
    SeamState seam;
    SuSchedTickOpts o;
    uint32_t fi = 0;
    CHECK(load_sys(k_movement[0], &l[0]));
    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    CHECK_ST(build_schedule(&s, l, 1u), SU_OK);
    CHECK(init_world(w, 5000u, 41u));
    seam_init(&seam);
    tick_opts(&o, 4u, 5001u, &seam);
    atomic_store(&seam.fail_ordinal, 1u);
    CHECK_ST(run_one(w, &s, &o, &fi), SU_OK);
    CHECK(atomic_load(&seam.calls) == 0u);
    /* At the threshold the chunked path runs and the armed failure takes effect. */
    o.chunk_min_rows = 5000u;
    CHECK_ST(run_one(w, &s, &o, &fi), SU_E_CANDIDATE_FAILED);
    CHECK(atomic_load(&seam.calls) > 0u);
    su_world_destroy(w);
    su_sched_free(&s);
}

/* Observation, not a gate: ticks per second of movement on 1,000,000 rows for workers 1, 2 and 4.
 * Run with --bench. Shared hosts make these numbers indicative only. */
static double now_s(void) {
    uint64_t ns = 0;
    (void)su_platform_monotonic_ns(&ns);
    return (double)ns * 1e-9;
}

static void run_bench(void) {
    static const uint32_t workers[] = {1u, 2u, 4u};
    uint32_t wi;
    for (wi = 0; wi < 3u; wi++) {
        Loaded l[1];
        SuSchedule s;
        SuWorld *w = make_world(1000000u, 1u);
        SeamState seam;
        SuSchedTickOpts o;
        uint32_t fi = 0;
        double t0;
        double t1;
        int rep;
        if (!load_sys(k_movement[0], &l[0]) || w == NULL || build_schedule(&s, l, 1u) != SU_OK) {
            fprintf(stderr, "bench setup failed\n");
            return;
        }
        (void)init_world(w, 1000000u, 51u);
        seam_init(&seam);
        tick_opts(&o, workers[wi], 4096u, &seam);
        t0 = now_s();
        for (rep = 0; rep < 5; rep++) {
            (void)run_one(w, &s, &o, &fi);
        }
        t1 = now_s();
        printf("bench movement 1000000 rows workers=%u: %.2f ticks/s (5 ticks, %.3f s)\n", (unsigned)workers[wi],
               5.0 / (t1 - t0), t1 - t0);
        su_world_destroy(w);
        su_sched_free(&s);
        su_lir_module_free(&l[0].mod);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        run_bench();
        return 0;
    }
    test_lockstep_all();
    test_failure_live();
    test_threshold();
    printf("test_tick_workers: checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
