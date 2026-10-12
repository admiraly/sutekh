/* M1 closure C: chunked native kernels (tick workers over native tiers).
 *
 * Usage: test_native_chunked <repo root>
 * In process, against su_agent_test (native registry, schedule and jobs compiled with the seams).
 *
 * For N in {1, 2, 4} and tier in {vm, native scalar, native AVX2 (when the host has AVX2)}:
 *   - 50 ticks over a 100,001-row world (the chunk threshold is 4096 rows, so N > 1 is chunked);
 *   - 3 ticks over a 1,000,000-row world;
 * every final committed column must equal the serial reference (the VM, one worker). The chunk seam
 * must be called once per chunk and tick when N > 1 and never when N = 1.
 * Failure injection: one chunk of one tick fails; the tick reports the failure, the committed columns
 * are unchanged, and the next clean tick equals the reference. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backends/native/su_native.h"
#include "ir/su_lir.h"
#include "runtime/jobs/su_jobs.h"
#include "runtime/native_versions.h"
#include "runtime/schedule.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            g_failures++;                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n  ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

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
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    b->p = malloc((size_t)sz + 1u);
    if (b->p == NULL) {
        fclose(f);
        return 0;
    }
    b->n = fread(b->p, 1u, (size_t)sz, f);
    fclose(f);
    return b->n == (size_t)sz;
}

static int load_movement(const char *root, SuLirModule *m) {
    char cap_path[1024];
    char lir_path[1024];
    Buf cap = {NULL, 0};
    Buf lir = {NULL, 0};
    SuDiag d;
    int ok = 0;
    snprintf(cap_path, sizeof cap_path, "%s/examples/movement.capsule.json", root);
    snprintf(lir_path, sizeof lir_path, "%s/examples/movement.lir.json", root);
    su_diag_init(&d);
    if (read_file(cap_path, &cap) && read_file(lir_path, &lir)) {
        ok = su_lir_verify(cap.p, cap.n, lir.p, lir.n, m, &d) == SU_OK;
        if (!ok) {
            fprintf(stderr, "verify failed: %s\n", d.message);
        }
    }
    free(cap.p);
    free(lir.p);
    return ok;
}

static uint32_t schedule_movement(SuSchedule *s, const char *root) {
    SuLirModule *m = malloc(sizeof *m);
    uint32_t st;
    if (m == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memset(m, 0, sizeof *m);
    if (!load_movement(root, m)) {
        su_lir_module_free(m);
        free(m);
        return SU_E_INTERNAL;
    }
    su_sched_init(s);
    st = su_sched_add(s, m, 1u, 1u, 0, 1, 0);
    if (st != SU_OK) {
        su_lir_module_free(m);
        free(m);
        return st;
    }
    return su_sched_finalize(s);
}

/* tier: 0 = VM, else the SU_NCAP_* set of the native kernel. */
static uint64_t install_tier(SuSchedule *s, uint32_t caps) {
    uint64_t seq = 0;
    uint32_t cases = 0;
    uint64_t rows = 0;
    uint64_t bad = 1;
    char msg[256];
    if (caps == 0u) {
        return 0;
    }
    if (su_nv_prepare(s->sys[0].mod, 1u, caps, &seq, msg, sizeof msg) != SU_OK ||
        su_nv_admit(seq, 7u, &cases, &rows, &bad) != SU_OK || bad != 0u || su_nv_install(seq) != SU_OK) {
        fprintf(stderr, "native tier could not be installed: %s\n", msg);
        return 0;
    }
    s->sys[0].native_seq = seq;
    return seq;
}

static uint32_t mix32(uint64_t x) {
    uint64_t z = x + UINT64_C(0x9E3779B97F4A7C15);
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return (uint32_t)(z ^ (z >> 31));
}

static SuWorld *make_world_with(uint64_t rows) {
    SuWorldFieldDesc fd[2];
    SuWorldDesc d;
    SuWorld *w = NULL;
    uint32_t *ids = calloc((size_t)rows, sizeof *ids);
    uint32_t *a = calloc((size_t)rows, sizeof *a);
    uint32_t *b = calloc((size_t)rows, sizeof *b);
    uint64_t i;
    if (ids == NULL || a == NULL || b == NULL) {
        goto done;
    }
    for (i = 0; i < rows; i++) {
        ids[i] = (uint32_t)(i + 1u);
        a[i] = mix32(i * 2u + 21u);
        b[i] = mix32(i * 2u + 22u);
        if (i % 9u == 4u) {
            b[i] = a[i]; /* equality hits */
        }
        if (i % 11u == 1u) {
            a[i] = UINT32_MAX; /* edge values */
        }
        if (i % 13u == 5u) {
            b[i] = 0u;
        }
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
    d.layout_generation = 1u;
    d.row_count = rows;
    d.entity_ids = ids;
    d.fields = fd;
    d.field_count = 2u;
    if (su_world_create(&d, &w) != SU_OK) {
        w = NULL;
        goto done;
    }
    if (su_world_init_committed(w, 1u, a, rows) != SU_OK || su_world_init_committed(w, 2u, b, rows) != SU_OK) {
        su_world_destroy(w);
        w = NULL;
    }
done:
    free(ids);
    free(a);
    free(b);
    return w;
}

/* Committed columns, field 1 then field 2, into out (2 * rows words). */
static int copy_columns(const SuWorld *w, uint64_t rows, uint32_t *out) {
    const uint32_t *p = NULL;
    uint64_t rc = 0;
    uint32_t f;
    for (f = 1; f <= 2; f++) {
        if (su_world_read_committed(w, f, &p, &rc) != SU_OK || rc != rows) {
            return 0;
        }
        memcpy(out + (size_t)(f - 1u) * (size_t)rows, p, (size_t)rows * sizeof *out);
    }
    return 1;
}

/* Counts the chunk-seam calls (the seam itself never fails here unless armed). */
typedef struct SeamState {
    uint64_t calls;
    int64_t fail_chunk; /* -1: never fail */
    uint64_t fail_tick_call;
} SeamState;

/* Called from the pool's threads: the counter is updated atomically. */
static int seam_fn(uint32_t chunk, void *ctx) {
    SeamState *s = ctx;
    __atomic_fetch_add(&s->calls, 1u, __ATOMIC_SEQ_CST);
    return s->fail_chunk >= 0 && (int64_t)chunk == s->fail_chunk;
}

/* Runs `ticks` ticks on a fresh world of `rows` rows with the given tier and workers. Writes the final
 * committed columns into out (2 * rows words). Returns the schedule status; *chunk_calls receives the
 * number of chunk-seam calls. */
static uint32_t run_case(const char *root, uint64_t rows, uint64_t ticks, uint32_t caps, uint32_t workers,
                         uint32_t *out, uint64_t *chunk_calls, int *installed) {
    SuSchedule s;
    SuWorld *w;
    SuSchedTickOpts opt;
    SeamState seam;
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;

    *chunk_calls = 0;
    *installed = 0;
    w = make_world_with(rows);
    if (w == NULL || schedule_movement(&s, root) != SU_OK) {
        su_world_destroy(w);
        return SU_E_INTERNAL;
    }
    if (caps != 0u) {
        if (install_tier(&s, caps) == 0u) {
            su_sched_free(&s);
            su_world_destroy(w);
            return SU_E_INTERNAL;
        }
        *installed = 1;
    }
    memset(&opt, 0, sizeof opt);
    memset(&seam, 0, sizeof seam);
    seam.fail_chunk = -1;
    opt.workers = workers;
    opt.chunk_min_rows = 4096u;
    opt.chunk_fault = seam_fn;
    opt.chunk_ctx = &seam;
    st = su_sched_run_ticks_opt(w, &s, ticks, NULL, NULL, &opt, &done, &fi);
    if (st == SU_OK && done == ticks) {
        if (!copy_columns(w, rows, out)) {
            st = SU_E_INTERNAL;
        }
    } else if (st == SU_OK) {
        st = SU_E_INTERNAL;
    }
    *chunk_calls = seam.calls;
    su_sched_free(&s);
    su_world_destroy(w);
    return st;
}

static void check_matrix(const char *root, uint64_t rows, uint64_t ticks, const char *label) {
    uint32_t *ref = malloc((size_t)rows * 2u * sizeof *ref);
    uint32_t *got = malloc((size_t)rows * 2u * sizeof *got);
    uint32_t caps_list[3];
    const char *names[3] = {"vm", "native_scalar", "native_avx2"};
    int ntiers = 2;
    uint32_t n_idx;
    uint32_t t;
    const uint32_t workers_list[3] = {1u, 2u, 4u};
    uint64_t chunks_per_tick = su_jobs_chunk_count(rows, su_jobs_effective_chunk_rows(rows, 0u));
    int installed;
    uint64_t calls;
    uint32_t st;

    if (ref == NULL || got == NULL) {
        CHECK(0, "allocation for %s", label);
        free(ref);
        free(got);
        return;
    }
    /* Reference: the VM, one worker, serial. */
    st = run_case(root, rows, ticks, 0u, 1u, ref, &calls, &installed);
    CHECK(st == SU_OK, "%s: serial VM reference: %s", label, su_status_name(st));
    caps_list[0] = SU_NCAP_X86_64_SCALAR;
    caps_list[1] = SU_NCAP_X86_64_AVX2;
    if ((su_native_host_caps() & SU_NCAP_X86_64_AVX2) != 0u) {
        ntiers = 3;
    }
    for (t = 0; t < (uint32_t)ntiers + 0u; t++) {
        uint32_t caps = t == 0u ? 0u : caps_list[t - 1u];
        const char *tier = t == 0u ? names[0] : names[t];
        for (n_idx = 0; n_idx < 3u; n_idx++) {
            uint32_t n = workers_list[n_idx];
            char what[160];
            snprintf(what, sizeof what, "%s tier=%s workers=%u", label, tier, (unsigned)n);
            st = run_case(root, rows, ticks, caps, n, got, &calls, &installed);
            CHECK(st == SU_OK, "%s: status %s", what, su_status_name(st));
            CHECK(caps == 0u || installed, "%s: native tier installed", what);
            CHECK(memcmp(ref, got, (size_t)rows * 2u * sizeof *ref) == 0, "%s: final columns differ from the VM reference",
                  what);
            if (n > 1u) {
                CHECK(calls == ticks * chunks_per_tick, "%s: chunk seam called %llu times, expected %llu", what,
                      (unsigned long long)calls, (unsigned long long)(ticks * chunks_per_tick));
            } else {
                CHECK(calls == 0u, "%s: a single worker never chunks (seam called %llu times)", what,
                      (unsigned long long)calls);
            }
        }
    }
    free(ref);
    free(got);
}

/* One chunk of one tick fails. The failed tick leaves the committed columns unchanged; the clean tick
 * after it equals the reference. */
static void check_failure(const char *root) {
    const uint64_t rows = 100001u;
    SuSchedule s;
    SuWorld *w = make_world_with(rows);
    SuSchedTickOpts opt;
    SeamState seam;
    uint32_t *before = malloc((size_t)rows * 2u * sizeof *before);
    uint32_t *after = malloc((size_t)rows * 2u * sizeof *after);
    uint32_t *ref1 = malloc((size_t)rows * 2u * sizeof *ref1);
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;
    uint64_t dummy = 0;
    int installed = 0;

    CHECK(w != NULL && before != NULL && after != NULL && ref1 != NULL, "failure test allocation");
    if (w == NULL || before == NULL || after == NULL || ref1 == NULL) {
        free(before);
        free(after);
        free(ref1);
        su_world_destroy(w);
        return;
    }
    CHECK(copy_columns(w, rows, before), "snapshot before the failing tick");
    CHECK(run_case(root, rows, 1u, 0u, 1u, ref1, &dummy, &installed) == SU_OK, "one-tick VM reference");
    CHECK(schedule_movement(&s, root) == SU_OK, "failure schedule");
    CHECK(install_tier(&s, SU_NCAP_X86_64_SCALAR) != 0u, "scalar tier for the failure test");
    memset(&opt, 0, sizeof opt);
    memset(&seam, 0, sizeof seam);
    seam.fail_chunk = 2; /* the third chunk of the only tick */
    opt.workers = 4u;
    opt.chunk_min_rows = 4096u;
    opt.chunk_fault = seam_fn;
    opt.chunk_ctx = &seam;
    st = su_sched_run_ticks_opt(w, &s, 1u, NULL, NULL, &opt, &done, &fi);
    CHECK(st == SU_E_CANDIDATE_FAILED, "a failing chunk fails the tick: %s", su_status_name(st));
    CHECK(done == 0u && fi == 0u, "no tick published, failing system index 0 (done %llu, index %u)",
          (unsigned long long)done, (unsigned)fi);
    CHECK(seam.calls > 0u, "the chunk seam was reached");
    CHECK(copy_columns(w, rows, after), "snapshot after the failing tick");
    CHECK(memcmp(before, after, (size_t)rows * 2u * sizeof *before) == 0,
          "committed columns are unchanged by the failing tick");
    seam.fail_chunk = -1;
    st = su_sched_run_ticks_opt(w, &s, 1u, NULL, NULL, &opt, &done, &fi);
    CHECK(st == SU_OK && done == 1u, "the clean retry publishes one tick: %s", su_status_name(st));
    CHECK(copy_columns(w, rows, after), "snapshot after the retry");
    CHECK(memcmp(ref1, after, (size_t)rows * 2u * sizeof *ref1) == 0, "the retry equals the VM reference");
    su_sched_free(&s);
    su_world_destroy(w);
    free(before);
    free(after);
    free(ref1);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <repo root>\n", argv[0]);
        return 2;
    }
    check_matrix(argv[1], 100001u, 50u, "100001 rows x 50 ticks");
    check_matrix(argv[1], 1000000u, 3u, "1000000 rows x 3 ticks");
    check_failure(argv[1]);
    printf("test_native_chunked: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
