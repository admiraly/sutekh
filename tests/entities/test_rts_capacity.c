/* T-RTS-CAPS-3: actual six-system RTS ticks above the old uniform-buffer ceiling. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdio.h>
#include <stdlib.h>

#include "rts_fixture.h"
#include "sutekh/entities.h"
#include "world/entities/su_cmdq.h"

static unsigned g_checks;
static unsigned g_failures;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t no_despawns(void *ctx, SuEntity entity, uint32_t *generation) {
    (void)ctx; (void)entity; (void)generation;
    CHECK(0); /* Empty queues must never call a despawn callback. */
    return SU_E_INTERNAL;
}

static int read_file(const char *path, unsigned char **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    *data = NULL;
    if (f == NULL) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || n > 256 * 1024 ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    *data = malloc((size_t)n + 1u);
    if (*data == NULL) { fclose(f); return 0; }
    *len = fread(*data, 1u, (size_t)n, f);
    fclose(f);
    if (*len != (size_t)n) { free(*data); *data = NULL; return 0; }
    return 1;
}

static uint32_t add_ir(SuSchedule *s, const char *name) {
    unsigned char *cap = NULL;
    unsigned char *src = NULL;
    size_t nc = 0u;
    size_t ns = 0u;
    char path[160];
    SuLirModule *m = calloc(1u, sizeof *m);
    SuDiag diag;
    uint32_t st = SU_E_IO;
    if (m == NULL) return SU_E_RESOURCE_LIMIT;
    snprintf(path, sizeof path, "capsules/rts/%s.capsule.json", name);
    if (!read_file(path, &cap, &nc)) goto out;
    snprintf(path, sizeof path, "capsules/rts/%s.lir.json", name);
    if (!read_file(path, &src, &ns)) goto out;
    st = su_lir_verify(cap, nc, src, ns, m, &diag);
    if (st == SU_OK) st = su_sched_add(s, m, 1u, 1u, 0u, 1u, 0u);
out:
    free(cap);
    free(src);
    if (st != SU_OK) { su_lir_module_free(m); free(m); }
    return st;
}

static uint32_t add_host(SuSchedule *s, const char *name, const SuHostOpDesc *d) {
    SuLirModule *m = NULL;
    uint32_t sv;
    uint32_t gen;
    char path[160];
    char why[256];
    uint32_t st;
    snprintf(path, sizeof path, "capsules/rts/host/%s.capsule.json", name);
    st = su_host_load(path, NULL, d, &m, &sv, &gen, why, sizeof why);
    if (st == SU_OK) st = su_sched_add_host(s, d, m, sv, gen, 0u, 1u, 0u);
    if (st != SU_OK) su_host_decl_free(m);
    return st;
}

static int full_schedule(SuSchedule *s, uint32_t capacity) {
    uint32_t st;
    su_sched_init(s);
    st = add_host(s, "spatial", &g_rts_spatial_desc);
    if (st == SU_OK) st = add_ir(s, "goal");
    if (st == SU_OK) st = add_ir(s, "combat");
    if (st == SU_OK) st = add_ir(s, "move");
    if (st == SU_OK) st = add_host(s, "scatter", &g_rts_scatter_desc);
    if (st == SU_OK) st = add_ir(s, "damage");
    if (st == SU_OK) st = su_sched_finalize(s);
    if (st == SU_OK) st = su_sched_enable_entities(s, capacity, capacity, 16u);
    CHECK(st == SU_OK);
    return st == SU_OK;
}

static void queue_capacity(uint32_t capacity) {
    const uint32_t ps[6] = {0u, 0u, 0u, 0u, 0u, 1u};
    const uint32_t pe[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    SuCmdQ *q = NULL;
    SuCmdQMemory memory;
    SuCmdView view;
    SuEventLog *log = NULL;
    SuCommitReport report;
    SuCmdCallbacks callbacks = {NULL, no_despawns};
    /* Reproduce the old sizing refusal without requiring the superseded runtime. */
    CHECK(su_cmdq_create(6u, capacity, SU_CHUNK_SLOTS, 1u, 0u, capacity, capacity, &q)
          == SU_E_RESOURCE_LIMIT);
    CHECK(q == NULL);
    CHECK(su_cmdq_create_ex(6u, capacity, SU_CHUNK_SLOTS, ps, pe, capacity, capacity, &q) == SU_OK);
    if (q == NULL) { CHECK(0); return; }
    CHECK(su_cmdq_memory(q, &memory) == SU_OK);
    CHECK(memory.structural == (uint64_t)capacity * sizeof(SuStructCmd));
    /* Empty zero-cap systems still merge safely. Then append in the short final chunk. */
    CHECK(su_cmdq_merge(q, &view) == SU_OK && view.n_cmds == 0u && view.n_events == 0u);
    CHECK(su_evlog_create(1u, 0u, &log) == SU_OK);
    CHECK(su_cmdq_commit(q, &callbacks, 1u, log, &report) == SU_OK);
    CHECK(su_evlog_count(log) == 0u && su_evlog_next_seq(log) == 0u);
    su_evlog_destroy(log);
    CHECK(su_cmdq_append_struct(q, 5u, (capacity - 1u) / SU_CHUNK_SLOTS, capacity - 1u,
                               0u, SU_STRUCT_DESPAWN, 1u, 0u, 0u) == SU_OK);
    CHECK(su_cmdq_merge(q, &view) == SU_OK && view.n_cmds == 1u);
    if (view.n_cmds == 1u) CHECK(view.cmds[0].slot == capacity - 1u);
    su_cmdq_destroy(q);
}

static void world_capacity(uint32_t capacity) {
    SuWorld *w = rts_world_create(capacity);
    SuSchedule s;
    RtsRow row = rts_unit(0u, 123u, 456u, 0u);
    SuEntity entity = 0u;
    uint64_t done = 0u;
    uint32_t failed = UINT32_MAX;
    uint32_t st;
    CHECK(w != NULL);
    if (w == NULL) return;
    if (!full_schedule(&s, capacity)) { su_sched_free(&s); su_world_destroy(w); return; }
    CHECK(s.count == 6u);
    CHECK(su_world_row_count(w) == capacity);
    CHECK(rts_spawn(w, &row, &entity) == SU_OK);
    CHECK(su_ent_budget(&s, capacity, capacity, capacity) == SU_OK);
    st = su_sched_run_ticks(w, &s, 1u, NULL, NULL, &done, &failed);
    CHECK(st == SU_OK);
    CHECK(done == 1u && failed == UINT32_MAX && su_world_tick(w) == 1u);
    CHECK(rts_read(w, F_POS_X, 0u) == 123u && rts_read(w, F_POS_Y, 0u) == 456u);
    CHECK(rts_read(w, F_HP, 0u) == 10u && rts_read(w, F_TARGET_SLOT, 0u) == RTS_NONE);
    CHECK(rts_read(w, F_HP, capacity - 1u) == 0u && rts_read(w, F_TARGET_SLOT, capacity - 1u) == 0u);
    su_sched_free(&s);
    su_world_destroy(w);
    printf("six-system tick at capacity %u: %s\n", capacity, su_status_name(st));
}

int main(void) {
    {
        const uint32_t ps[2] = {0u, 1u};
        const uint32_t pe[2] = {0u, 0u};
        SuCmdQ *q = NULL;
        CHECK(su_cmdq_create_ex_checked(2u, 4u, 1u, ps, 1u, pe, 2u, 4u, 4u, &q)
              == SU_E_INVALID_ARGUMENT);
        CHECK(q == NULL);
        CHECK(su_cmdq_create_ex_checked(2u, 4u, 1u, ps, 2u, pe, 1u, 4u, 4u, &q)
              == SU_E_INVALID_ARGUMENT);
        CHECK(q == NULL);
        CHECK(su_cmdq_create_ex_checked(2u, 4u, 1u, ps, 2u, pe, 2u, 4u, 4u, &q) == SU_OK);
        su_cmdq_destroy(q);
    }
    queue_capacity(699051u);
    queue_capacity(1048576u);
    world_capacity(699051u);
    world_capacity(1000000u);
    /* OD-21 retains this world bound; a larger queue is not a larger admitted world. */
    {
        SuWorld *w = rts_world_create(1048576u);
        CHECK(w == NULL);
        su_world_destroy(w);
    }
    printf("test_rts_capacity: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures != 0u;
}
