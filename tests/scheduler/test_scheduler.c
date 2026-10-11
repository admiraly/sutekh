/* M1-04 tests: chunked system execution, worker pool, effect analysis.
 *
 * Covers:
 *   - partition: exact coverage, boundaries independent of worker count;
 *   - pool: every task runs once; seeded completion order is applied exactly
 *     (the completion log equals the permutation);
 *   - serial vs chunked equivalence over worker counts 1,2,3,4,8, chunk sizes
 *     (auto, 1, 7, 8, 9, 1024), awkward row counts, and >= 200 completion seeds;
 *     fixtures tests/fixtures/t02, tests/fixtures/t03 (expected columns from the
 *     fixture files) and the two-system pipeline in tests/scheduler/fixtures/pipe;
 *   - failure: lowest failing chunk is reported, nothing from the failed system
 *     is committed, the world stays usable;
 *   - effect analysis: write/write and read/write conflicts, ordering cycles,
 *     unknown and cross-phase edges, declared-vs-IR mismatch, canonical order.
 *
 * Run from the repository root (fixture paths are relative). Build with:
 *   gcc -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -pthread \
 *       -Iinclude -Isrc src/ir/su_json.c src/ir/su_lir.c src/ir/su_sha256.c \
 *       src/world/su_world.c src/vm/su_vm.c src/runtime/jobs/su_jobs.c \
 *       src/runtime/jobs/su_effects.c tests/scheduler/test_scheduler.c -o <scratch>/test_scheduler
 * Then run <scratch>/test_scheduler from the repository root. Add --bench to
 * measure the 1,000,000-row speedup instead (observation only, not a gate).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "runtime/jobs/su_effects.h"
#include "runtime/jobs/su_jobs.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
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

static const uint64_t k_awkward_rows[] = {0u, 1u, 7u, 8u, 9u, 1023u, 1024u, 100001u};
static const uint32_t k_worker_counts[] = {1u, 2u, 3u, 4u, 8u};
static const uint64_t k_chunk_sizes[] = {0u, 1u, 7u, 8u, 9u, 1024u}; /* 0 = auto */

/* ---------- file and JSON helpers (as in tests/vm/test_vm.c) ---------- */

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
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if (sz < 0) {
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

static int parse_file(const char *path, Buf *b, SuJsonDoc *doc) {
    SuJsonError err;
    uint32_t st;
    if (!read_file(path, b)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    st = su_json_parse(b->p, b->n, &k_limits, doc, &err);
    if (st != SU_OK) {
        fprintf(stderr, "%s: parse %s at %zu\n", path, su_status_name(st), err.offset);
        free(b->p);
        b->p = NULL;
        return 0;
    }
    return 1;
}

static const char *json_str(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    return v != NULL && v->type == SU_JSON_STRING ? v->u.string.data : NULL;
}

static const SuJsonValue *json_obj(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    return v != NULL && v->type == SU_JSON_OBJECT ? v : NULL;
}

static int json_u32_array(const SuJsonValue *arr, uint32_t **out, size_t *n) {
    size_t i;
    uint32_t *vals;
    if (arr == NULL || arr->type != SU_JSON_ARRAY) {
        return 0;
    }
    vals = calloc(arr->u.array.count + 1u, sizeof *vals);
    if (vals == NULL) {
        return 0;
    }
    for (i = 0; i < arr->u.array.count; i++) {
        uint64_t v = 0;
        if (!su_json_get_uint(arr->u.array.items[i], &v) || v > UINT32_MAX) {
            free(vals);
            return 0;
        }
        vals[i] = (uint32_t)v;
    }
    *out = vals;
    *n = arr->u.array.count;
    return 1;
}

/* ---------- module, world and runner helpers ---------- */

typedef struct Loaded {
    SuLirModule mod;
    uint32_t gen;
} Loaded;

/* Verifies a capsule with the IR named by its "source" (relative to the capsule). */
static int load_sys(const char *cap_path, Loaded *L) {
    Buf cb = {0};
    Buf lb = {0};
    SuJsonDoc cdoc;
    SuDiag diag;
    char lir_path[512];
    uint64_t gen = 0;
    const SuJsonValue *schema;
    const char *src;
    uint32_t st;
    int ok = 0;

    memset(L, 0, sizeof *L);
    if (!parse_file(cap_path, &cb, &cdoc)) {
        return 0;
    }
    schema = json_obj(cdoc.root, "schema");
    if (schema == NULL || !su_json_get_uint(su_json_member(schema, "layout_generation"), &gen) ||
        gen > UINT32_MAX) {
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
    ok = 1;
done:
    su_json_doc_free(&cdoc);
    free(cb.p);
    free(lb.p);
    return ok;
}

/* World of the two fixture fields (ids 1 and 2, both readable and writable).
 * Every fixture system in this suite is over demo.actor with these two fields. */
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

/* Deterministic initial columns with frequent equality hits and edge values. */
static void gen_columns(uint64_t rows, uint64_t seed, uint32_t *pos, uint32_t *vel) {
    uint64_t i;
    for (i = 0; i < rows; i++) {
        uint64_t k = seed * UINT64_C(1000003) + i * 2u;
        pos[i] = mix32(k);
        vel[i] = mix32(k + 1u);
        if (i % 5u == 0u) {
            vel[i] = pos[i];
        }
        if (i % 11u == 3u) {
            pos[i] = UINT32_MAX;
        }
        if (i % 13u == 4u) {
            vel[i] = 0u;
        }
    }
}

static int init_world(SuWorld *w, uint64_t rows, const uint32_t *pos, const uint32_t *vel) {
    uint32_t *a = malloc((rows > 0u ? (size_t)rows : 1u) * sizeof *a);
    uint32_t *b = malloc((rows > 0u ? (size_t)rows : 1u) * sizeof *b);
    int ok = 0;
    if (a == NULL || b == NULL) {
        goto done;
    }
    if (rows > 0u) {
        memcpy(a, pos, (size_t)rows * sizeof *a);
        memcpy(b, vel, (size_t)rows * sizeof *b);
    }
    ok = su_world_init_committed(w, 1u, a, rows) == SU_OK &&
         su_world_init_committed(w, 2u, b, rows) == SU_OK;
done:
    free(a);
    free(b);
    return ok;
}

/* Copies both committed columns into one malloc'd buffer: pos then vel. */
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
    if (rows == 0u) {
        return 1;
    }
    return memcmp(a, b, (size_t)(2u * (rows > 0u ? rows : 1u)) * sizeof *a) == 0;
}

typedef struct RunCfg {
    int chunked;
    SuJobsOptions opt;
} RunCfg;

static RunCfg serial_cfg(void) {
    RunCfg c;
    memset(&c, 0, sizeof c);
    return c;
}

static RunCfg chunk_cfg(uint32_t workers, uint64_t chunk_rows) {
    RunCfg c;
    memset(&c, 0, sizeof c);
    c.chunked = 1;
    c.opt.workers = workers;
    c.opt.chunk_rows = chunk_rows;
    return c;
}

/* Runs `ticks` ticks; in each tick the systems run in order and the tick is
 * published on success or aborted on failure. last_rep (optional) receives the
 * report of the failing or last chunked system. */
static uint32_t run_ticks(SuWorld *w, const Loaded *sys, uint32_t nsys, uint64_t ticks, const RunCfg *cfg,
                          SuJobsReport *last_rep) {
    uint64_t t;
    uint32_t i;
    SuExecContext **ctx = calloc(nsys > 0u ? nsys : 1u, sizeof *ctx);
    uint32_t st = SU_OK;
    if (ctx == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (!cfg->chunked) {
        for (i = 0; i < nsys; i++) {
            st = su_vm_context_create(&sys[i].mod, sys[i].gen, &ctx[i]);
            if (st != SU_OK) {
                goto done;
            }
        }
    }
    for (t = 0; t < ticks && st == SU_OK; t++) {
        st = su_world_begin_tick(w);
        if (st != SU_OK) {
            break;
        }
        for (i = 0; i < nsys && st == SU_OK; i++) {
            if (cfg->chunked) {
                SuJobsReport rep;
                st = su_jobs_run_system_chunked(w, &sys[i].mod, sys[i].gen, &cfg->opt, &rep);
                if (last_rep != NULL) {
                    su_jobs_report_free(last_rep);
                    *last_rep = rep;
                } else {
                    su_jobs_report_free(&rep);
                }
            } else {
                st = su_vm_run_system(w, ctx[i]);
            }
        }
        if (st != SU_OK) {
            su_world_abort(w);
            break;
        }
        st = su_world_publish(w);
    }
done:
    for (i = 0; i < nsys; i++) {
        su_vm_context_destroy(ctx[i]);
    }
    free(ctx);
    return st;
}

/* ---------- partition ---------- */

static void test_partition(void) {
    uint64_t r;
    uint64_t cr;
    for (r = 0; r < sizeof k_awkward_rows / sizeof k_awkward_rows[0]; r++) {
        for (cr = 0; cr < sizeof k_chunk_sizes / sizeof k_chunk_sizes[0]; cr++) {
            uint64_t rows = k_awkward_rows[r];
            uint64_t chunk = k_chunk_sizes[cr];
            uint64_t n = su_jobs_chunk_count(rows, chunk);
            uint64_t i;
            uint64_t next = 0;
            for (i = 0; i < n; i++) {
                uint64_t b = 0;
                uint64_t l = 0;
                CHECK_ST(su_jobs_chunk_bounds(rows, chunk, i, &b, &l), SU_OK);
                CHECK(b == next);
                CHECK(l > 0u);
                CHECK(l <= su_jobs_effective_chunk_rows(rows, chunk));
                next = b + l;
            }
            CHECK(next == rows);
            if (rows == 0u) {
                CHECK(n == 0u);
            }
            {
                uint64_t b = 0;
                uint64_t l = 0;
                CHECK_ST(su_jobs_chunk_bounds(rows, chunk, n, &b, &l), SU_E_INVALID_ARGUMENT);
            }
        }
    }
    /* Auto size: floor of 1024 rows, about 64 chunks at 1e6 rows, independent of workers. */
    CHECK(su_jobs_effective_chunk_rows(1023u, 0u) == SU_JOBS_MIN_CHUNK_ROWS);
    CHECK(su_jobs_chunk_count(1023u, 0u) == 1u);
    CHECK(su_jobs_chunk_count(1000000u, 0u) == 64u);
    CHECK(su_jobs_chunk_count(100001u, 1u) == 100001u);
    /* Bad arguments. */
    {
        uint64_t b = 0;
        uint64_t l = 0;
        CHECK_ST(su_jobs_chunk_bounds(10u, 4u, 0u, NULL, &l), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_jobs_chunk_bounds(10u, 4u, 0u, &b, NULL), SU_E_INVALID_ARGUMENT);
    }
}

static void test_permutation(void) {
    uint32_t perm[256];
    uint32_t again[256];
    uint32_t sizes[] = {1u, 2u, 7u, 256u};
    uint32_t s;
    uint32_t distinct_first = 0;
    uint32_t seen_first[256];
    memset(seen_first, 0, sizeof seen_first);
    for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint32_t n = sizes[s];
        uint64_t seed;
        for (seed = 1; seed <= 200u; seed++) {
            uint32_t i;
            uint8_t hit[256];
            memset(hit, 0, sizeof hit);
            su_jobs_seeded_permutation(n, seed, perm);
            su_jobs_seeded_permutation(n, seed, again);
            CHECK(memcmp(perm, again, n * sizeof perm[0]) == 0);
            for (i = 0; i < n; i++) {
                CHECK(perm[i] < n);
                if (perm[i] < n) {
                    CHECK(hit[perm[i]] == 0u);
                    hit[perm[i]] = 1u;
                }
            }
            if (n == 7u) {
                uint32_t f = perm[0];
                seen_first[f]++;
                if (seen_first[f] == 1u) {
                    distinct_first++;
                }
            }
        }
    }
    /* Seeds must actually vary the order. */
    CHECK(distinct_first == 7u);
}

/* ---------- pool ---------- */

typedef struct PoolRec {
    uint32_t *hits;     /* per task execution count */
    uint32_t *workers; /* worker id seen by each task */
} PoolRec;

static void pool_task(uint32_t task, uint32_t worker, void *arg) {
    PoolRec *r = arg;
    r->hits[task]++;
    r->workers[task] = worker;
}

static void test_pool(void) {
    static const uint32_t counts[] = {1u, 2u, 3u, 4u, 8u};
    uint32_t c;
    SuJobPool *pool = NULL;
    uint32_t bad_perm[4] = {0u, 1u, 1u, 3u};

    CHECK_ST(su_jobs_pool_create(0u, &pool), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_jobs_pool_create(SU_JOBS_MAX_WORKERS + 1u, &pool), SU_E_INVALID_ARGUMENT);
    CHECK(pool == NULL);

    for (c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        uint32_t tasks = 37u;
        uint32_t *hits = calloc(tasks, sizeof *hits);
        uint32_t *wk = calloc(tasks, sizeof *wk);
        PoolRec rec;
        uint32_t i;
        rec.hits = hits;
        rec.workers = wk;
        CHECK_ST(su_jobs_pool_create(counts[c], &pool), SU_OK);
        CHECK(su_jobs_pool_workers(pool) == counts[c]);
        CHECK_ST(su_jobs_pool_run(pool, tasks, pool_task, &rec, NULL, NULL), SU_OK);
        for (i = 0; i < tasks; i++) {
            CHECK(hits[i] == 1u);
            CHECK(wk[i] < counts[c]);
        }
        /* Second batch on the same pool, forced completion order. */
        {
            uint32_t perm[37];
            uint32_t log[37];
            uint64_t seed;
            memset(hits, 0, tasks * sizeof *hits);
            for (seed = 1; seed <= 200u; seed++) {
                uint32_t p;
                su_jobs_seeded_permutation(tasks, seed, perm);
                memset(log, 0, sizeof log);
                CHECK_ST(su_jobs_pool_run(pool, tasks, pool_task, &rec, perm, log), SU_OK);
                CHECK(memcmp(log, perm, sizeof perm) == 0);
                for (p = 0; p < tasks; p++) {
                    CHECK(log[p] == perm[p]);
                }
            }
            for (i = 0; i < tasks; i++) {
                CHECK(hits[i] == 200u);
            }
        }
        CHECK_ST(su_jobs_pool_run(pool, 4u, pool_task, &rec, bad_perm, NULL), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_jobs_pool_run(pool, 0u, NULL, &rec, NULL, NULL), SU_E_INVALID_ARGUMENT);
        su_jobs_pool_destroy(pool);
        pool = NULL;
        free(hits);
        free(wk);
    }
}

/* ---------- serial vs chunked equivalence ---------- */

/* Compares one configuration against the serial reference state. */
static void check_equivalent(const Loaded *sys, uint32_t nsys, uint64_t rows, uint64_t ticks, uint64_t seed,
                             const RunCfg *cfg, const char *label) {
    SuWorld *ref = make_world(rows, sys[0].gen);
    SuWorld *w = make_world(rows, sys[0].gen);
    uint32_t *pos = malloc((rows > 0u ? (size_t)rows : 1u) * sizeof *pos);
    uint32_t *vel = malloc((rows > 0u ? (size_t)rows : 1u) * sizeof *vel);
    uint32_t st_ref;
    uint32_t st;
    uint32_t *want = NULL;
    uint32_t *got = NULL;
    RunCfg serial = serial_cfg();
    if (ref == NULL || w == NULL || pos == NULL || vel == NULL) {
        CHECK(0);
        goto done;
    }
    gen_columns(rows, seed, pos, vel);
    CHECK(init_world(ref, rows, pos, vel));
    CHECK(init_world(w, rows, pos, vel));
    st_ref = run_ticks(ref, sys, nsys, ticks, &serial, NULL);
    st = run_ticks(w, sys, nsys, ticks, cfg, NULL);
    CHECK_ST(st_ref, SU_OK);
    CHECK_ST(st, SU_OK);
    {
        int eq = 0;
        CHECK_ST(su_world_state_equal(ref, w, &eq), SU_OK);
        if (!eq) {
            fprintf(stderr, "MISMATCH %s rows=%llu seed=%llu\n", label, (unsigned long long)rows,
                    (unsigned long long)seed);
        }
        CHECK(eq == 1);
    }
    want = snapshot(ref, rows);
    got = snapshot(w, rows);
    CHECK(same_snapshot(want, got, rows));
done:
    free(want);
    free(got);
    free(pos);
    free(vel);
    su_world_destroy(ref);
    su_world_destroy(w);
}

/* Equivalence over the full matrix for one system set and row count. */
static void test_matrix(const Loaded *sys, uint32_t nsys, const char *label) {
    uint32_t r;
    uint32_t wi;
    uint32_t ci;
    for (r = 0; r < sizeof k_awkward_rows / sizeof k_awkward_rows[0]; r++) {
        for (wi = 0; wi < sizeof k_worker_counts / sizeof k_worker_counts[0]; wi++) {
            for (ci = 0; ci < sizeof k_chunk_sizes / sizeof k_chunk_sizes[0]; ci++) {
                RunCfg cfg = chunk_cfg(k_worker_counts[wi], k_chunk_sizes[ci]);
                check_equivalent(sys, nsys, k_awkward_rows[r], 1u, k_awkward_rows[r] + 17u, &cfg, label);
            }
        }
    }
    /* Several ticks per run, so each tick starts from the previous committed root. */
    for (wi = 0; wi < sizeof k_worker_counts / sizeof k_worker_counts[0]; wi++) {
        RunCfg cfg = chunk_cfg(k_worker_counts[wi], 3u);
        check_equivalent(sys, nsys, 1023u, 4u, 99u, &cfg, label);
        check_equivalent(sys, nsys, 9u, 6u, 5u, &cfg, label);
    }
}

/* Completion orders: 200 seeds, each must complete in exactly the permutation
 * order and produce the serial result. */
static void test_seeded(void) {
    static const uint64_t rows_list[] = {9u, 1023u};
    static const uint32_t workers_list[] = {2u, 3u, 4u, 8u};
    Loaded mv;
    size_t ri;
    size_t wi;
    CHECK(load_sys("tests/fixtures/t02/movement.capsule.json", &mv));
    for (ri = 0; ri < sizeof rows_list / sizeof rows_list[0]; ri++) {
        uint64_t rows = rows_list[ri];
        uint64_t chunk = rows == 9u ? 1u : 7u;
        uint32_t *pos = malloc((size_t)rows * sizeof *pos);
        uint32_t *vel = malloc((size_t)rows * sizeof *vel);
        SuWorld *ref = make_world(rows, mv.gen);
        uint32_t *want;
        uint32_t *perm;
        uint64_t count = su_jobs_chunk_count(rows, chunk);
        uint64_t seed;
        if (pos == NULL || vel == NULL || ref == NULL) {
            CHECK(0);
            free(pos);
            free(vel);
            su_world_destroy(ref);
            continue;
        }
        gen_columns(rows, 7u, pos, vel);
        CHECK(init_world(ref, rows, pos, vel));
        CHECK_ST(run_ticks(ref, &mv, 1u, 1u, &(RunCfg){0}, NULL), SU_OK);
        want = snapshot(ref, rows);
        perm = malloc((size_t)count * sizeof *perm);
        for (wi = 0; wi < sizeof workers_list / sizeof workers_list[0]; wi++) {
            for (seed = 1; seed <= 200u; seed++) {
                RunCfg cfg = chunk_cfg(workers_list[wi], chunk);
                SuWorld *w = make_world(rows, mv.gen);
                SuJobsReport rep;
                uint64_t i;
                memset(&rep, 0, sizeof rep);
                cfg.opt.seeded_completion = 1;
                cfg.opt.completion_seed = seed;
                if (w == NULL || perm == NULL) {
                    CHECK(0);
                    su_world_destroy(w);
                    break;
                }
                CHECK(init_world(w, rows, pos, vel));
                CHECK_ST(run_ticks(w, &mv, 1u, 1u, &cfg, &rep), SU_OK);
                CHECK_ST(rep.status, SU_OK);
                CHECK(rep.chunk_count == count);
                su_jobs_seeded_permutation((uint32_t)count, seed, perm);
                if (rep.completion_log != NULL && perm != NULL) {
                    CHECK(memcmp(rep.completion_log, perm, (size_t)count * sizeof *perm) == 0);
                } else {
                    CHECK(0);
                }
                for (i = 0; i < count && rep.chunk_status != NULL; i++) {
                    CHECK(rep.chunk_status[i] == SU_OK);
                }
                {
                    uint32_t *got = snapshot(w, rows);
                    CHECK(same_snapshot(want, got, rows));
                    free(got);
                }
                su_jobs_report_free(&rep);
                su_world_destroy(w);
            }
        }
        free(want);
        free(perm);
        free(pos);
        free(vel);
        su_world_destroy(ref);
    }
    su_jobs_report_free(NULL);
    su_lir_module_free(&mv.mod);
}

/* Fixture-driven runs: the expected columns come from the fixture files. */
static int run_fixture(const char *test_path, const RunCfg *cfg) {
    Buf tb = {0};
    Buf wb = {0};
    SuJsonDoc tdoc;
    SuJsonDoc wdoc;
    char cap_path[512];
    char world_path[512];
    const SuJsonValue *cases;
    const SuJsonValue *cols;
    Loaded L;
    uint32_t *ids = NULL;
    uint32_t *pos = NULL;
    uint32_t *vel = NULL;
    size_t n = 0;
    size_t nc = 0;
    size_t ci;
    int ok = 0;

    if (!parse_file(test_path, &tb, &tdoc)) {
        return 0;
    }
    if (json_str(tdoc.root, "capsule") == NULL || json_str(tdoc.root, "world") == NULL) {
        goto done_test;
    }
    join_rel(test_path, json_str(tdoc.root, "capsule"), cap_path, sizeof cap_path);
    join_rel(test_path, json_str(tdoc.root, "world"), world_path, sizeof world_path);
    if (!load_sys(cap_path, &L)) {
        goto done_test;
    }
    if (!parse_file(world_path, &wb, &wdoc)) {
        goto done_loaded;
    }
    if (!json_u32_array(su_json_member(wdoc.root, "entity_ids"), &ids, &n)) {
        goto done_world;
    }
    cols = json_obj(wdoc.root, "columns");
    if (!json_u32_array(su_json_member(cols, "position_x"), &pos, &nc) || nc != n ||
        !json_u32_array(su_json_member(cols, "velocity_x"), &vel, &nc) || nc != n) {
        goto done_world;
    }
    cases = su_json_member(tdoc.root, "cases");
    if (cases == NULL || cases->type != SU_JSON_ARRAY) {
        goto done_world;
    }
    ok = 1;
    for (ci = 0; ci < cases->u.array.count; ci++) {
        const SuJsonValue *c = cases->u.array.items[ci];
        const SuJsonValue *exp = json_obj(c, "expected_columns");
        uint64_t ticks = 0;
        SuWorld *w;
        size_t k;
        if (exp == NULL || !su_json_get_uint(su_json_member(c, "ticks"), &ticks)) {
            ok = 0;
            break;
        }
        w = make_world((uint64_t)n, L.gen);
        if (w == NULL || !init_world(w, (uint64_t)n, pos, vel)) {
            su_world_destroy(w);
            ok = 0;
            break;
        }
        CHECK_ST(run_ticks(w, &L, 1u, ticks, cfg, NULL), SU_OK);
        for (k = 0; k < exp->u.object.count; k++) {
            const SuJsonMember *mem = &exp->u.object.members[k];
            uint32_t *want = NULL;
            size_t wn = 0;
            const uint32_t *got = NULL;
            uint64_t rc = 0;
            uint32_t fid = strcmp(mem->key, "position_x") == 0 ? 1u : 2u;
            if (json_u32_array(mem->value, &want, &wn)) {
                CHECK_ST(su_world_read_committed(w, fid, &got, &rc), SU_OK);
                CHECK(rc == (uint64_t)wn);
                if (got != NULL && rc == (uint64_t)wn) {
                    size_t q;
                    for (q = 0; q < wn; q++) {
                        if (got[q] != want[q]) {
                            fprintf(stderr, "%s case %zu: %s row %zu got %u want %u\n", test_path, ci,
                                    mem->key, q, got[q], want[q]);
                            g_failures++;
                            break;
                        }
                    }
                }
            } else {
                CHECK(0);
            }
            free(want);
        }
        su_world_destroy(w);
    }
done_world:
    free(ids);
    free(pos);
    free(vel);
    su_json_doc_free(&wdoc);
    free(wb.p);
done_loaded:
    su_lir_module_free(&L.mod);
done_test:
    su_json_doc_free(&tdoc);
    free(tb.p);
    return ok;
}

static void test_fixtures(void) {
    static const char *const t02[] = {
        "tests/fixtures/t02/movement_0.test.json",
        "tests/fixtures/t02/movement_1.test.json",
        "tests/fixtures/t02/movement_7.test.json",
        "tests/fixtures/t02/movement_8.test.json",
        "tests/fixtures/t02/movement_9.test.json",
        "tests/fixtures/t02/movement_1024.test.json",
    };
    static const char *const t03[] = {
        "tests/fixtures/t03/eq_edges/test.json",
        "tests/fixtures/t03/lt_edges/test.json",
        "tests/fixtures/t03/mul_overflow/test.json",
        "tests/fixtures/t03/select_min/test.json",
        "tests/fixtures/t03/sub_underflow/test.json",
        "tests/fixtures/t03/wrap_add/test.json",
    };
    static const char *const pipe_tests[] = {
        "tests/scheduler/fixtures/pipe/advance.test.json",
        "tests/scheduler/fixtures/pipe/damp.test.json",
    };
    static const uint32_t workers[] = {1u, 2u, 3u, 4u, 8u};
    static const uint64_t chunks[] = {1u, 7u, 0u};
    size_t i;
    size_t wi;
    size_t ci;
    for (i = 0; i < sizeof pipe_tests / sizeof pipe_tests[0]; i++) {
        for (wi = 0; wi < sizeof workers / sizeof workers[0]; wi++) {
            for (ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
                RunCfg cfg = chunk_cfg(workers[wi], chunks[ci]);
                CHECK(run_fixture(pipe_tests[i], &cfg));
            }
        }
    }
    for (i = 0; i < sizeof t02 / sizeof t02[0]; i++) {
        for (wi = 0; wi < sizeof workers / sizeof workers[0]; wi++) {
            for (ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
                RunCfg cfg = chunk_cfg(workers[wi], chunks[ci]);
                CHECK(run_fixture(t02[i], &cfg));
            }
        }
    }
    for (i = 0; i < sizeof t03 / sizeof t03[0]; i++) {
        for (wi = 0; wi < sizeof workers / sizeof workers[0]; wi++) {
            for (ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
                RunCfg cfg = chunk_cfg(workers[wi], chunks[ci]);
                CHECK(run_fixture(t03[i], &cfg));
            }
        }
    }
    {
        RunCfg serial = serial_cfg();
        CHECK(run_fixture(t03[0], &serial));
        CHECK(run_fixture(t02[5], &serial));
    }
}

/* Two-system pipeline: advance (position += velocity), then damp (velocity -= position).
 * Hand-computed values for three rows, then the serial/chunked equivalence matrix. */
static void test_pipeline_truth(void) {
    Loaded sys[2];
    SuWorld *w = make_world(3u, 1u);
    static const uint32_t pos[3] = {5u, UINT32_MAX, 10u};
    static const uint32_t vel[3] = {7u, 1u, 20u};
    static const uint32_t want_pos[3] = {12u, 0u, 30u};
    static const uint32_t want_vel[3] = {4294967291u, 1u, 4294967286u};
    RunCfg cfg = chunk_cfg(3u, 1u);
    const uint32_t *got = NULL;
    uint64_t rc = 0;
    size_t i;

    CHECK(load_sys("tests/scheduler/fixtures/pipe/advance.capsule.json", &sys[0]));
    CHECK(load_sys("tests/scheduler/fixtures/pipe/damp.capsule.json", &sys[1]));
    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    CHECK(init_world(w, 3u, pos, vel));
    CHECK_ST(run_ticks(w, sys, 2u, 1u, &cfg, NULL), SU_OK);
    CHECK_ST(su_world_read_committed(w, 1u, &got, &rc), SU_OK);
    for (i = 0; got != NULL && i < 3u; i++) {
        CHECK(got[i] == want_pos[i]);
    }
    CHECK_ST(su_world_read_committed(w, 2u, &got, &rc), SU_OK);
    for (i = 0; got != NULL && i < 3u; i++) {
        CHECK(got[i] == want_vel[i]);
    }
    su_world_destroy(w);
    su_lir_module_free(&sys[0].mod);
    su_lir_module_free(&sys[1].mod);
}

static void test_pipeline_matrix(void) {
    Loaded sys[2];
    CHECK(load_sys("tests/scheduler/fixtures/pipe/advance.capsule.json", &sys[0]));
    CHECK(load_sys("tests/scheduler/fixtures/pipe/damp.capsule.json", &sys[1]));
    test_matrix(sys, 2u, "pipeline");
    su_lir_module_free(&sys[0].mod);
    su_lir_module_free(&sys[1].mod);
}

static void test_movement_matrix(void) {
    Loaded mv;
    CHECK(load_sys("tests/fixtures/t02/movement.capsule.json", &mv));
    test_matrix(&mv, 1u, "movement");
    su_lir_module_free(&mv.mod);
}

/* Failure: the lowest failing chunk is reported; nothing from the failed system is
 * committed, even if the caller publishes the open tick. */
typedef struct FaultSet {
    uint32_t idx[4];
    uint32_t n;
} FaultSet;

static int fault_fn(uint32_t chunk, void *ctx) {
    const FaultSet *f = ctx;
    uint32_t i;
    for (i = 0; i < f->n; i++) {
        if (f->idx[i] == chunk) {
            return 1;
        }
    }
    return 0;
}

static void test_failure(void) {
    static FaultSet sets[] = {
        {{3u, 5u, 9u, 0u}, 3u},
        {{146u, 0u, 0u, 0u}, 1u},
        {{0u, 0u, 0u, 0u}, 1u},
        {{146u, 88u, 0u, 0u}, 2u},
    };
    static const uint32_t lowest[] = {3u, 146u, 0u, 88u};
    static const uint32_t workers[] = {1u, 2u, 4u, 8u};
    Loaded mv;
    uint64_t rows = 1024u;
    uint32_t *pos = malloc((size_t)rows * sizeof *pos);
    uint32_t *vel = malloc((size_t)rows * sizeof *vel);
    size_t si;
    size_t wi;

    CHECK(load_sys("tests/fixtures/t02/movement.capsule.json", &mv));
    if (pos == NULL || vel == NULL) {
        CHECK(0);
        goto done;
    }
    gen_columns(rows, 3u, pos, vel);
    for (si = 0; si < sizeof sets / sizeof sets[0]; si++) {
        for (wi = 0; wi < sizeof workers / sizeof workers[0]; wi++) {
            SuWorld *w = make_world(rows, mv.gen);
            uint32_t *before;
            uint32_t *after;
            SuJobsOptions opt;
            SuJobsReport rep;
            uint32_t st;
            uint32_t k;
            int seeded;
            if (w == NULL || !init_world(w, rows, pos, vel)) {
                CHECK(0);
                su_world_destroy(w);
                continue;
            }
            before = snapshot(w, rows);
            CHECK_ST(su_world_begin_tick(w), SU_OK);
            memset(&opt, 0, sizeof opt);
            opt.workers = workers[wi];
            opt.chunk_rows = 7u;
            opt.fault = fault_fn;
            opt.fault_ctx = (void *)&sets[si];
            seeded = (si % 2u) == 1u;
            opt.seeded_completion = seeded;
            opt.completion_seed = 40u + si;
            st = su_jobs_run_system_chunked(w, &mv.mod, mv.gen, &opt, &rep);
            CHECK_ST(st, SU_E_CANDIDATE_FAILED);
            CHECK(rep.failed_chunk == lowest[si]);
            CHECK(rep.status == SU_E_CANDIDATE_FAILED);
            for (k = 0; k < (uint32_t)rep.chunk_count && rep.chunk_status != NULL; k++) {
                uint32_t j;
                int faulted = 0;
                for (j = 0; j < sets[si].n; j++) {
                    if (sets[si].idx[j] == k) {
                        faulted = 1;
                    }
                }
                CHECK(rep.chunk_status[k] == (faulted ? SU_E_CANDIDATE_FAILED : SU_OK));
            }
            /* Publish the open tick anyway: committed columns must be unchanged. */
            CHECK_ST(su_world_publish(w), SU_OK);
            after = snapshot(w, rows);
            CHECK(same_snapshot(before, after, rows));
            su_jobs_report_free(&rep);
            free(before);
            free(after);
            su_world_destroy(w);
        }
    }
    /* A world whose tick was aborted after a failure runs cleanly afterwards. */
    {
        SuWorld *w = make_world(rows, mv.gen);
        SuWorld *ref = make_world(rows, mv.gen);
        RunCfg cfg = chunk_cfg(4u, 7u);
        RunCfg faulty = chunk_cfg(4u, 7u);
        SuJobsReport rep;
        RunCfg serial = serial_cfg();
        memset(&rep, 0, sizeof rep);
        faulty.opt.fault = fault_fn;
        faulty.opt.fault_ctx = (void *)&sets[0];
        CHECK(w != NULL && ref != NULL);
        if (w != NULL && ref != NULL) {
            CHECK(init_world(w, rows, pos, vel));
            CHECK(init_world(ref, rows, pos, vel));
            CHECK_ST(run_ticks(w, &mv, 1u, 1u, &faulty, &rep), SU_E_CANDIDATE_FAILED);
            su_jobs_report_free(&rep);
            CHECK_ST(run_ticks(w, &mv, 1u, 1u, &cfg, NULL), SU_OK);
            CHECK_ST(run_ticks(ref, &mv, 1u, 1u, &serial, NULL), SU_OK);
            {
                int eq = 0;
                CHECK_ST(su_world_state_equal(w, ref, &eq), SU_OK);
                CHECK(eq == 1);
            }
        }
        su_world_destroy(w);
        su_world_destroy(ref);
    }
done:
    free(pos);
    free(vel);
    su_lir_module_free(&mv.mod);
}

/* ---------- effect analysis ---------- */

static SuEffectInput eff_in(const char *id, const Loaded *L, const char *phase, uint32_t prio, uint32_t par) {
    SuEffectInput e;
    memset(&e, 0, sizeof e);
    e.system_id = id;
    e.mod = &L->mod;
    e.phase = phase;
    e.priority = prio;
    e.declared_parallel = par;
    return e;
}

static void expect_error(const SuEffectInput *in, uint32_t n, uint32_t want, const char *needle,
                         const char *label) {
    SuEffectGraph *g = NULL;
    char err[512];
    uint32_t st = su_effects_build(in, n, &g, err, sizeof err);
    if (st != want) {
        fprintf(stderr, "%s: got %s (%s), want %s\n", label, su_status_name(st), err, su_status_name(want));
    }
    CHECK(st == want);
    CHECK(g == NULL);
    if (needle != NULL) {
        if (strstr(err, needle) == NULL) {
            fprintf(stderr, "%s: message \"%s\" lacks \"%s\"\n", label, err, needle);
        }
        CHECK(strstr(err, needle) != NULL);
    }
    su_effects_free(g);
}

static void test_effects(void) {
    Loaded mv;
    Loaded adv;
    Loaded dmp;
    Loaded mv2;
    static const char *const after_adv[] = {"pipe.advance"};
    static const char *const after_dmp[] = {"pipe.damp"};
    static const char *const after_mov[] = {"demo.movement"};
    static const char *const after_self[] = {"pipe.advance"};
    static const char *const unknown[] = {"no.such.system"};
    SuEffectGraph *g = NULL;
    char err[512];
    uint32_t order[8];
    uint32_t st;

    CHECK(load_sys("tests/fixtures/t02/movement.capsule.json", &mv));
    CHECK(load_sys("tests/scheduler/fixtures/pipe/advance.capsule.json", &adv));
    CHECK(load_sys("tests/scheduler/fixtures/pipe/damp.capsule.json", &dmp));
    CHECK(load_sys("tests/fixtures/t02/movement.capsule.json", &mv2));

    /* Declared effects equal the IR effects on every fixture. */
    CHECK_ST(su_effects_check_declared(&mv.mod, err, sizeof err), SU_OK);
    CHECK_ST(su_effects_check_declared(&adv.mod, err, sizeof err), SU_OK);
    CHECK_ST(su_effects_check_declared(&dmp.mod, err, sizeof err), SU_OK);
    /* Derived sets for movement: reads position and velocity, writes position. */
    {
        uint8_t rd[SU_LIMIT_FIELDS];
        uint8_t wr[SU_LIMIT_FIELDS];
        uint32_t i;
        CHECK_ST(su_effects_derive(&mv.mod, rd, wr), SU_OK);
        for (i = 0; i < mv.mod.field_count; i++) {
            if (mv.mod.fields[i].id == 1u) {
                CHECK(rd[i] == 1u && wr[i] == 1u);
            } else {
                CHECK(rd[i] == 1u && wr[i] == 0u);
            }
        }
    }
    /* Declared-vs-IR mismatch: velocity declared writable without any store. */
    mv2.mod.fields[1].access |= SU_ACCESS_WRITE;
    CHECK_ST(su_effects_check_declared(&mv2.mod, err, sizeof err), SU_E_SCHEMA_MISMATCH);
    CHECK(strstr(err, "velocity_x") != NULL);
    {
        SuEffectInput e = eff_in("demo.movement", &mv2, "simulate", 0u, 0u);
        expect_error(&e, 1u, SU_E_SCHEMA_MISMATCH, "velocity_x", "mismatch build");
    }

    /* Accepted: one parallel system; two serial conflicting systems (serial order). */
    {
        SuEffectInput e = eff_in("pipe.advance", &adv, "simulate", 0u, 1u);
        CHECK_ST(su_effects_build(&e, 1u, &g, err, sizeof err), SU_OK);
        CHECK(su_effects_count(g) == 1u);
        su_effects_free(g);
        g = NULL;
    }
    {
        SuEffectInput in[2];
        in[0] = eff_in("pipe.damp", &dmp, "simulate", 0u, 0u);
        in[1] = eff_in("pipe.advance", &adv, "simulate", 0u, 0u);
        in[0].after = NULL;
        in[1].after = NULL;
        CHECK_ST(su_effects_build(in, 2u, &g, err, sizeof err), SU_OK);
        CHECK(su_effects_conflict(g, 0u, 1u) == 1);
        CHECK(su_effects_ordered(g, 0u, 1u) == 0);
        CHECK_ST(su_effects_serial_order(g, order, 2u), SU_OK);
        CHECK(order[0] == 1u && order[1] == 0u); /* pipe.advance before pipe.damp */
        su_effects_free(g);
        g = NULL;
    }
    /* Accepted: the same pair declared parallel with the ordering edge damp after advance. */
    {
        SuEffectInput in[2];
        in[0] = eff_in("pipe.advance", &adv, "simulate", 0u, 1u);
        in[1] = eff_in("pipe.damp", &dmp, "simulate", 0u, 1u);
        in[1].after = after_adv;
        in[1].after_count = 1u;
        CHECK_ST(su_effects_build(in, 2u, &g, err, sizeof err), SU_OK);
        CHECK(su_effects_ordered(g, 0u, 1u) == 1);
        CHECK(su_effects_conflict(g, 0u, 1u) == 1);
        CHECK_ST(su_effects_serial_order(g, order, 2u), SU_OK);
        CHECK(order[0] == 0u && order[1] == 1u);
        su_effects_free(g);
        g = NULL;
    }
    /* Rejected: parallel, unordered, read/write (advance writes position, damp reads it). */
    {
        SuEffectInput in[2];
        in[0] = eff_in("pipe.advance", &adv, "simulate", 0u, 1u);
        in[1] = eff_in("pipe.damp", &dmp, "simulate", 0u, 1u);
        expect_error(in, 2u, SU_E_SCHEMA_MISMATCH, "read/write", "read/write parallel");
    }
    /* Rejected: parallel, unordered, write/write (both write position). */
    {
        SuEffectInput in[2];
        in[0] = eff_in("demo.movement", &mv, "simulate", 0u, 1u);
        in[1] = eff_in("pipe.advance", &adv, "simulate", 0u, 1u);
        expect_error(in, 2u, SU_E_SCHEMA_MISMATCH, "write/write", "write/write parallel");
    }
    /* Ordering cycles, named in the message. */
    {
        SuEffectInput in[2];
        in[0] = eff_in("pipe.advance", &adv, "simulate", 0u, 0u);
        in[1] = eff_in("pipe.damp", &dmp, "simulate", 0u, 0u);
        in[0].after = after_dmp;
        in[0].after_count = 1u;
        in[1].after = after_adv;
        in[1].after_count = 1u;
        expect_error(in, 2u, SU_E_INVALID_ARGUMENT, "ordering cycle", "2-cycle");
    }
    {
        SuEffectInput in[3];
        in[0] = eff_in("demo.movement", &mv, "simulate", 0u, 0u);
        in[1] = eff_in("pipe.advance", &adv, "simulate", 0u, 0u);
        in[2] = eff_in("pipe.damp", &dmp, "simulate", 0u, 0u);
        in[0].after = after_dmp;
        in[0].after_count = 1u;
        in[1].after = after_mov;
        in[1].after_count = 1u;
        in[2].after = after_adv;
        in[2].after_count = 1u;
        expect_error(in, 3u, SU_E_INVALID_ARGUMENT, "demo.movement -> pipe.advance -> pipe.damp", "3-cycle");
    }
    {
        SuEffectInput e = eff_in("pipe.advance", &adv, "simulate", 0u, 0u);
        e.before = after_self;
        e.before_count = 1u;
        expect_error(&e, 1u, SU_E_INVALID_ARGUMENT, "pipe.advance -> pipe.advance", "self cycle");
    }
    /* Unknown target, cross-phase edge, duplicate ID. */
    {
        SuEffectInput e = eff_in("pipe.advance", &adv, "simulate", 0u, 0u);
        e.after = unknown;
        e.after_count = 1u;
        expect_error(&e, 1u, SU_E_INVALID_ARGUMENT, "unknown", "unknown target");
    }
    {
        SuEffectInput in[2];
        in[0] = eff_in("pipe.advance", &adv, "simulate", 0u, 0u);
        in[1] = eff_in("pipe.damp", &dmp, "late", 0u, 0u);
        in[1].after = after_adv;
        in[1].after_count = 1u;
        expect_error(in, 2u, SU_E_INVALID_ARGUMENT, "crosses phases", "cross phase");
    }
    {
        SuEffectInput in[2];
        in[0] = eff_in("pipe.advance", &adv, "simulate", 0u, 0u);
        in[1] = eff_in("pipe.advance", &dmp, "simulate", 0u, 0u);
        expect_error(in, 2u, SU_E_INVALID_ARGUMENT, "duplicate", "duplicate id");
    }

    /* Canonical order: independent of input order and by (phase, priority, id). */
    {
        static const uint32_t perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        static const char *const want_ids[3] = {"demo.movement", "pipe.advance", "pipe.damp"};
        size_t p;
        for (p = 0; p < 6; p++) {
            SuEffectInput base[3];
            SuEffectInput in[3];
            uint32_t i;
            base[0] = eff_in("demo.movement", &mv, "simulate", 0u, 0u);
            base[1] = eff_in("pipe.advance", &adv, "simulate", 0u, 0u);
            base[2] = eff_in("pipe.damp", &dmp, "simulate", 0u, 0u);
            for (i = 0; i < 3; i++) {
                in[i] = base[perms[p][i]];
            }
            CHECK_ST(su_effects_build(in, 3u, &g, err, sizeof err), SU_OK);
            CHECK_ST(su_effects_serial_order(g, order, 3u), SU_OK);
            for (i = 0; i < 3; i++) {
                CHECK(strcmp(in[order[i]].system_id, want_ids[i]) == 0);
            }
            su_effects_free(g);
            g = NULL;
        }
    }
    /* Priority beats id; phase name orders the phases. */
    {
        SuEffectInput in[3];
        in[0] = eff_in("demo.movement", &mv, "simulate", 2u, 0u);
        in[1] = eff_in("pipe.advance", &adv, "simulate", 1u, 0u);
        in[2] = eff_in("pipe.damp", &dmp, "simulate", 0u, 0u);
        CHECK_ST(su_effects_build(in, 3u, &g, err, sizeof err), SU_OK);
        CHECK_ST(su_effects_serial_order(g, order, 3u), SU_OK);
        CHECK(order[0] == 2u && order[1] == 1u && order[2] == 0u);
        su_effects_free(g);
        g = NULL;
        in[1].phase = "early";
        in[0].phase = "simulate";
        CHECK_ST(su_effects_build(in, 3u, &g, err, sizeof err), SU_OK);
        CHECK_ST(su_effects_serial_order(g, order, 3u), SU_OK);
        CHECK(order[0] == 1u);
        su_effects_free(g);
        g = NULL;
    }
    /* Edge forces order against priority. */
    {
        SuEffectInput in[2];
        in[0] = eff_in("pipe.advance", &adv, "simulate", 5u, 0u);
        in[1] = eff_in("pipe.damp", &dmp, "simulate", 0u, 0u);
        in[1].after = after_adv;
        in[1].after_count = 1u;
        CHECK_ST(su_effects_build(in, 2u, &g, err, sizeof err), SU_OK);
        CHECK_ST(su_effects_serial_order(g, order, 2u), SU_OK);
        CHECK(order[0] == 0u && order[1] == 1u);
        su_effects_free(g);
        g = NULL;
    }
    st = su_effects_build(NULL, 1u, &g, err, sizeof err);
    CHECK(st == SU_E_INVALID_ARGUMENT);

    su_lir_module_free(&mv.mod);
    su_lir_module_free(&adv.mod);
    su_lir_module_free(&dmp.mod);
    su_lir_module_free(&mv2.mod);
}

/* ---------- speedup observation (not a gate) ---------- */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static void run_bench(void) {
    enum { REPS = 7 };
    static const uint32_t workers[] = {1u, 2u, 4u};
    uint64_t rows = 1000000u;
    Loaded mv;
    SuWorld *w;
    uint32_t *pos = malloc((size_t)rows * sizeof *pos);
    uint32_t *vel = malloc((size_t)rows * sizeof *vel);
    double serial_median = 0.0;
    size_t wi;
    if (!load_sys("tests/fixtures/t02/movement.capsule.json", &mv) || pos == NULL || vel == NULL) {
        fprintf(stderr, "bench setup failed\n");
        return;
    }
    gen_columns(rows, 11u, pos, vel);
    w = make_world(rows, mv.gen);
    if (w == NULL || !init_world(w, rows, pos, vel)) {
        fprintf(stderr, "bench world failed\n");
        return;
    }
    printf("bench rows=%llu reps=%d online_cpus=%ld (shared host; observation only)\n",
           (unsigned long long)rows, REPS, sysconf(_SC_NPROCESSORS_ONLN));
    for (wi = 0; wi < sizeof workers / sizeof workers[0] + 1u; wi++) {
        double t[REPS];
        int r;
        int is_serial = wi == 0u;
        uint32_t nw = is_serial ? 1u : workers[wi - 1u];
        RunCfg cfg = chunk_cfg(nw, 0u);
        RunCfg serial = serial_cfg();
        for (r = 0; r < REPS; r++) {
            double t0 = now_ms();
            uint32_t st = run_ticks(w, &mv, 1u, 1u, is_serial ? &serial : &cfg, NULL);
            t[r] = now_ms() - t0;
            if (st != SU_OK) {
                fprintf(stderr, "bench run failed: %s\n", su_status_name(st));
                break;
            }
        }
        qsort(t, REPS, sizeof t[0], cmp_double);
        if (is_serial) {
            serial_median = t[REPS / 2];
        }
        printf("bench mode=%s workers=%u min_ms=%.2f median_ms=%.2f max_ms=%.2f speedup_vs_serial=%.2f\n",
               is_serial ? "serial" : "chunked", nw, t[0], t[REPS / 2], t[REPS - 1],
               serial_median > 0.0 ? serial_median / t[REPS / 2] : 0.0);
    }
    su_world_destroy(w);
    su_lir_module_free(&mv.mod);
    free(pos);
    free(vel);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        run_bench();
        return 0;
    }
    test_partition();
    test_permutation();
    test_pool();
    test_movement_matrix();
    test_pipeline_matrix();
    test_pipeline_truth();
    test_seeded();
    test_fixtures();
    test_failure();
    test_effects();
    printf("test_scheduler: checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
