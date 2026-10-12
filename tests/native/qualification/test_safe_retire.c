/* M1 closure C: safe retirement of native kernels, and the quarantine table key.
 *
 * Usage: test_safe_retire <repo root>
 * Built against su_agent_test (the agent library compiled with SU_TEST_FAULT_HOOKS), in process.
 *
 * (1) The in-flight counter is exposed through su_nv_inflight and is zero before every kernel call
 *     of a live tick and after the run. Mutant M07 (the decrement on the run path removed) keeps it
 *     at 1 after the first call and fails this check.
 * (2) Retirement from inside a kernel call defers: while the call runs the artifact is RETIRING, still
 *     mapped and counted in flight. The call's result is still committed, equal to the VM. Once the call
 *     returns the artifact is freed. Mutants that free at once ("ignore in-flight") or one call early
 *     ("free at in-flight 1") fail the observation; M07 fails the freed-after-return check.
 * (3) The quarantine table key compares whole artifact hashes. Two hashes that share a 64-character
 *     prefix except the last digit never match (mutant M10 compares only an 8-byte prefix).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backends/native/su_native.h"
#include "ir/su_lir.h"
#include "runtime/native_versions.h"
#include "runtime/schedule.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

/* Test-only, src/agent/su_agent.c (SU_TEST_FAULT_HOOKS). */
int su_agent_test_quar_hit(const char *record_hash, const char *query_hash);

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

/* Verifies examples/movement into m (schema version 1, layout generation 1). */
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

/* A schedule with one system, the verified movement module, owned by the schedule. */
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

/* Prepares, admits and installs the scalar kernel of the schedule's system; returns its handle, 0 on
 * failure. The schedule owns the module the artifact borrows, so the schedule outlives the artifact. */
static uint64_t install_native(SuSchedule *s) {
    uint64_t seq = 0;
    uint32_t cases = 0;
    uint64_t rows = 0;
    uint64_t bad = 1;
    char msg[256];
    if (su_nv_prepare(s->sys[0].mod, 1u, SU_NCAP_X86_64_SCALAR, &seq, msg, sizeof msg) != SU_OK) {
        fprintf(stderr, "prepare: %s\n", msg);
        return 0;
    }
    if (su_nv_admit(seq, 7u, &cases, &rows, &bad) != SU_OK || bad != 0u || su_nv_install(seq) != SU_OK) {
        fprintf(stderr, "admit or install failed\n");
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

/* A 64-row demo.actor world with the movement fields (position_x = 1, velocity_x = 2). */
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
        a[i] = mix32(i * 2u + 11u);
        b[i] = mix32(i * 2u + 12u);
        if (i % 7u == 2u) {
            a[i] = UINT32_MAX;
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

/* Same committed columns in two worlds. */
static int same_columns(const SuWorld *x, const SuWorld *y, uint64_t rows) {
    const uint32_t *px = NULL;
    const uint32_t *py = NULL;
    uint64_t rx = 0;
    uint64_t ry = 0;
    uint32_t f;
    for (f = 1; f <= 2; f++) {
        if (su_world_read_committed(x, f, &px, &rx) != SU_OK || su_world_read_committed(y, f, &py, &ry) != SU_OK ||
            rx != rows || ry != rows || memcmp(px, py, (size_t)rows * sizeof *px) != 0) {
            return 0;
        }
    }
    return 1;
}

/* (1) in-flight count zero at every system boundary of a native tick. */
typedef struct InflightCtx {
    uint64_t seq;
    uint32_t calls;
    uint32_t nonzero;
} InflightCtx;

static int inflight_probe(uint32_t system_index, void *ctx) {
    InflightCtx *c = ctx;
    (void)system_index;
    c->calls++;
    if (su_nv_inflight(c->seq) != 0u) {
        c->nonzero++;
    }
    return 0; /* never fails the system */
}

static void test_inflight_zero(const char *root) {
    SuSchedule s;
    SuWorld *w = make_world_with(64u);
    InflightCtx c;
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;
    memset(&c, 0, sizeof c);
    CHECK(w != NULL, "world");
    CHECK(schedule_movement(&s, root) == SU_OK, "schedule");
    c.seq = install_native(&s);
    CHECK(c.seq != 0u, "native kernel installed");
    if (w == NULL || c.seq == 0u) {
        su_sched_free(&s);
        su_world_destroy(w);
        return;
    }
    st = su_sched_run_ticks(w, &s, 3u, inflight_probe, &c, &done, &fi);
    CHECK(st == SU_OK && done == 3u, "3 native ticks: status %s, done %llu", su_status_name(st),
          (unsigned long long)done);
    CHECK(c.calls == 3u, "one kernel call per tick: %u", (unsigned)c.calls);
    CHECK(c.nonzero == 0u, "in-flight count was nonzero at %u of %u system boundaries", (unsigned)c.nonzero,
          (unsigned)c.calls);
    CHECK(su_nv_inflight(c.seq) == 0u, "in-flight count is zero after the run: %u", (unsigned)su_nv_inflight(c.seq));
    CHECK(su_nv_state(c.seq) == SU_NV_STATE_INSTALLED, "kernel still installed after normal ticks");
    su_sched_free(&s);
    su_world_destroy(w);
}

/* (2) retirement from inside the kernel call. */
static void test_retire_in_call(const char *root) {
    SuSchedule s;
    SuSchedule vm;
    SuWorld *w = make_world_with(64u);
    SuWorld *ref = make_world_with(64u);
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;
    uint32_t fired = 0;
    uint32_t obs_state = 0;
    uint32_t obs_inflight = 0;
    uint32_t obs_live = 0;
    uint32_t live_before;
    uint64_t seq;
    CHECK(w != NULL && ref != NULL, "worlds");
    CHECK(schedule_movement(&s, root) == SU_OK, "native schedule");
    CHECK(schedule_movement(&vm, root) == SU_OK, "vm schedule");
    seq = install_native(&s);
    CHECK(seq != 0u, "native kernel installed for the retirement test");
    if (w == NULL || ref == NULL || seq == 0u) {
        su_sched_free(&s);
        su_sched_free(&vm);
        su_world_destroy(w);
        su_world_destroy(ref);
        return;
    }
    live_before = su_nv_live_count();
    su_nv_test_arm(SU_NV_FAULT_RELEASE_IN_CALL, 0u);
    st = su_sched_run_ticks(w, &s, 1u, NULL, NULL, &done, &fi);
    su_nv_test_arm(SU_NV_FAULT_NONE, 0u);
    su_nv_test_release_observation(&fired, &obs_state, &obs_inflight, &obs_live);
    CHECK(fired == 1u, "the release seam ran inside the kernel call");
    CHECK(obs_state == SU_NV_STATE_RETIRING, "inside the call the artifact is RETIRING (state %u)", (unsigned)obs_state);
    CHECK(obs_inflight == 1u, "inside the call the artifact is in flight once (count %u)", (unsigned)obs_inflight);
    CHECK(obs_live == live_before, "inside the call the artifact stays mapped (live %u, before %u)",
          (unsigned)obs_live, (unsigned)live_before);
    CHECK(st == SU_OK && done == 1u, "the tick whose kernel retired mid-call still succeeds: %s",
          su_status_name(st));
    CHECK(su_nv_state(seq) == SU_NV_STATE_NONE, "after the call returns the artifact is freed (state %u)",
          (unsigned)su_nv_state(seq));
    CHECK(su_nv_inflight(seq) == 0u, "no call is in flight after the return");
    CHECK(su_nv_live_count() + 1u == live_before, "the freed artifact left the registry (live %u, before %u)",
          (unsigned)su_nv_live_count(), (unsigned)live_before);
    /* The committed result of the retiring call equals the VM reference. */
    st = su_sched_run_ticks(ref, &vm, 1u, NULL, NULL, &done, &fi);
    CHECK(st == SU_OK && done == 1u, "vm reference tick");
    CHECK(same_columns(w, ref, 64u), "the kernel result committed during retirement equals the VM");
    su_sched_free(&s);
    su_sched_free(&vm);
    su_world_destroy(w);
    su_world_destroy(ref);
}

/* (3) quarantine key: whole-hash equality, not a prefix. */
static void test_quarantine_key(void) {
    const char *base = "3c8e1f0a9b7d25e4"
                       "3c8e1f0a9b7d25e4"
                       "3c8e1f0a9b7d25e4"
                       "3c8e1f0a9b7d25e0";
    char other_last[65];
    char other_mid[65];
    char other_first[65];
    snprintf(other_last, sizeof other_last, "%s", base);
    other_last[63] = '1';
    snprintf(other_mid, sizeof other_mid, "%s", base);
    other_mid[9] = 'f';
    snprintf(other_first, sizeof other_first, "%s", base);
    other_first[0] = '4';
    CHECK(strlen(base) == 64u, "hash fixture length");
    CHECK(su_agent_test_quar_hit(base, base) == 1, "a quarantined hash is found");
    CHECK(su_agent_test_quar_hit(base, other_last) == 0,
          "a hash differing only in its last digit is not quarantined with it");
    CHECK(su_agent_test_quar_hit(base, other_mid) == 0, "a hash differing at digit 9 is not quarantined with it");
    CHECK(su_agent_test_quar_hit(base, other_first) == 0, "a hash differing at digit 0 is not quarantined with it");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <repo root>\n", argv[0]);
        return 2;
    }
    test_inflight_zero(argv[1]);
    test_retire_in_call(argv[1]);
    test_quarantine_key();
    printf("test_safe_retire: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
