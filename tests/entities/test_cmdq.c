/* M2-00b tests: structural-command buffers, canonical merge, apply/commit, bounded event log.
 * Build (from the repository root):
 *   gcc -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -pthread -Iinclude -Isrc \
 *     src/world/entities/su_cmdq.c tests/entities/test_cmdq.c -o <scratch>/test_cmdq
 * Run from any directory (no fixtures are read). Exit status 0 when every check passes. */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/entities.h"
#include "sutekh/status.h"
#include "world/entities/su_cmdq.h"

static unsigned long g_checks;
static unsigned long g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)

#define NSYS 3u
#define NROWS 1000u
#define PER_ROW 3u
#define PER_TICK 16384u
#define TICK 7u
#define NTEMPLATE_UNUSED 0u

/* ---- deterministic workload -------------------------------------------- */

static uint64_t mix(uint64_t x) {
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    return x ^ (x >> 31);
}

static uint64_t h3(uint32_t sys, uint32_t slot, uint32_t salt) {
    return mix(((uint64_t)sys << 40) ^ ((uint64_t)slot << 8) ^ salt);
}

/* Every row has at least one command, so the first row of each buffer writes. */
static uint32_t n_cmds_of(uint32_t sys, uint32_t slot) {
    return 1u + (uint32_t)(h3(sys, slot, 1u) % PER_ROW);
}
static uint32_t n_evts_of(uint32_t sys, uint32_t slot) {
    return (uint32_t)(h3(sys, slot, 2u) % (PER_ROW + 1u));
}
static uint32_t cmd_kind_of(uint32_t sys, uint32_t slot, uint32_t i) {
    /* Mostly despawn; a few unknown kinds exercise the rejected path. */
    return (h3(sys, slot, 10u + i) % 10u) == 0u ? 7u : SU_STRUCT_DESPAWN;
}
static uint64_t cmd_arg_of(uint32_t sys, uint32_t slot, uint32_t i) {
    uint64_t h = h3(sys, slot, 20u + i);
    uint32_t tslot = (uint32_t)(h % NROWS);
    uint32_t gen = 1u + (uint32_t)((h >> 20) % 3u);
    return SU_ENTITY_MAKE(tslot, gen);
}
static uint32_t evt_kind_of(uint32_t sys, uint32_t slot, uint32_t j) {
    return 0x100u + (uint32_t)(h3(sys, slot, 30u + j) % 3u);
}
static uint32_t evt_a_of(uint32_t sys, uint32_t slot, uint32_t j) {
    return (uint32_t)h3(sys, slot, 40u + j);
}
static uint32_t evt_b_of(uint32_t sys, uint32_t slot, uint32_t j) {
    return (uint32_t)(h3(sys, slot, 50u + j) >> 32);
}

static unsigned long g_emit_bad; /* updated from writer threads: use __atomic builtins only */

static void emit_row(SuCmdQ *q, uint64_t cs, uint32_t sys, uint32_t slot) {
    uint64_t chunk = slot / cs;
    uint32_t i;
    uint32_t j;
    unsigned long bad = 0u;
    for (i = 0; i < n_cmds_of(sys, slot); i++) {
        if (su_cmdq_append_struct(q, sys, chunk, slot, cmd_kind_of(sys, slot, i),
                                  cmd_arg_of(sys, slot, i), slot + i, sys) != SU_OK) {
            bad++;
        }
    }
    for (j = 0; j < n_evts_of(sys, slot); j++) {
        if (su_cmdq_append_event(q, sys, chunk, slot, evt_kind_of(sys, slot, j),
                                 evt_a_of(sys, slot, j), evt_b_of(sys, slot, j)) != SU_OK) {
            bad++;
        }
    }
    if (bad != 0u) {
        __atomic_fetch_add(&g_emit_bad, bad, __ATOMIC_RELAXED);
    }
}

/* ---- fake entity table (test double for the apply callback) ------------ */

typedef struct Table {
    uint8_t alive[NROWS];
    uint32_t gen[NROWS];
} Table;

static void table_init(Table *t) {
    uint32_t i;
    for (i = 0; i < NROWS; i++) {
        t->alive[i] = i < 400u ? 1u : 0u;
        t->gen[i] = SU_GEN_FIRST;
    }
}

static uint32_t table_despawn(Table *t, uint64_t h, uint32_t *old_gen) {
    uint32_t slot = SU_ENTITY_SLOT(h);
    uint32_t gen = SU_ENTITY_GEN(h);
    if (gen == 0u || slot >= NROWS || !t->alive[slot] || t->gen[slot] != gen ||
        t->gen[slot] == SU_GEN_RETIRED) {
        return SU_E_STALE_HANDLE;
    }
    *old_gen = gen;
    t->alive[slot] = 0u;
    if (gen + 1u == SU_GEN_RETIRED) {
        t->gen[slot] = SU_GEN_RETIRED;
    } else {
        t->gen[slot] = gen + 1u;
    }
    return SU_OK;
}

typedef struct CbCtx {
    Table *table;
    uint64_t calls;
} CbCtx;

static uint32_t cb_despawn(void *ctx, uint64_t h, uint32_t *old_gen) {
    CbCtx *c = ctx;
    c->calls++;
    return table_despawn(c->table, h, old_gen);
}

static SuCmdCallbacks make_cb(CbCtx *c, Table *t) {
    SuCmdCallbacks cb;
    c->table = t;
    c->calls = 0u;
    cb.ctx = c;
    cb.despawn = cb_despawn;
    return cb;
}

/* ---- canonical reference ------------------------------------------------ */

static SuStructCmd *ref_cmds;
static uint64_t ref_n_cmds;
static SuEvent *ref_sys_evts;
static uint64_t ref_n_sys_evts;
static SuEvent *ref_log;          /* expected log, in canonical order, with seq and tick */
static uint64_t ref_n_log;
static Table ref_table_after;
static unsigned long ref_despawned;
static unsigned long ref_rejected;

static void build_reference(void) {
    uint32_t sys;
    uint32_t slot;
    uint64_t ci = 0u;
    uint64_t ei = 0u;
    uint32_t i;
    uint32_t j;

    ref_cmds = calloc(NSYS * NROWS * PER_ROW, sizeof *ref_cmds);
    ref_sys_evts = calloc(NSYS * NROWS * PER_ROW, sizeof *ref_sys_evts);
    for (sys = 0; sys < NSYS; sys++) {
        for (slot = 0; slot < NROWS; slot++) {
            for (i = 0; i < n_cmds_of(sys, slot); i++) {
                SuStructCmd *c = &ref_cmds[ci++];
                c->kind = cmd_kind_of(sys, slot, i);
                c->system_index = sys;
                c->slot = slot;
                c->seq = i;
                c->arg = cmd_arg_of(sys, slot, i);
                c->init[0] = slot + i;
                c->init[1] = sys;
            }
            for (j = 0; j < n_evts_of(sys, slot); j++) {
                SuEvent *e = &ref_sys_evts[ei++];
                memset(e, 0, sizeof *e);
                e->kind = evt_kind_of(sys, slot, j);
                e->system_index = sys;
                e->slot = slot;
                e->seq = j;
                e->a = evt_a_of(sys, slot, j);
                e->b = evt_b_of(sys, slot, j);
            }
        }
    }
    ref_n_cmds = ci;
    ref_n_sys_evts = ei;

    /* Sequential oracle: apply in canonical order, one record per command. */
    {
        Table t;
        SuEvent *recs = calloc(ref_n_cmds + 1u, sizeof *recs);
        SuEvent *all = calloc(ref_n_cmds + ref_n_sys_evts + 1u, sizeof *all);
        uint64_t *src = calloc(ref_n_cmds + ref_n_sys_evts + 1u, sizeof *src);
        uint64_t n = 0u;
        uint64_t k;
        uint64_t a = 0u;
        uint64_t b = 0u;
        table_init(&t);
        for (k = 0; k < ref_n_cmds; k++) {
            const SuStructCmd *c = &ref_cmds[k];
            SuEvent *r = &recs[k];
            uint32_t old = 0u;
            uint32_t st = c->kind == SU_STRUCT_DESPAWN ? table_despawn(&t, c->arg, &old)
                                                       : SU_E_INVALID_ARGUMENT;
            memset(r, 0, sizeof *r);
            r->tick = TICK;
            r->system_index = c->system_index;
            r->slot = c->slot;
            r->seq = c->seq;
            if (st == SU_OK) {
                r->kind = SU_EVENT_DESPAWNED;
                r->a = SU_ENTITY_SLOT(c->arg);
                r->b = old;
                ref_despawned++;
            } else {
                r->kind = SU_EVENT_REJECTED;
                r->a = c->kind;
                r->b = st;
                ref_rejected++;
            }
        }
        ref_table_after = t;
        /* Merge system events and records by (system, slot, seq, source), system first on ties. */
        for (k = 0; k < ref_n_sys_evts; k++) {
            all[n] = ref_sys_evts[k];
            all[n].tick = TICK;
            src[n] = 0u;
            n++;
        }
        for (k = 0; k < ref_n_cmds; k++) {
            all[n] = recs[k];
            src[n] = 1u;
            n++;
        }
        /* Insertion sort on indices is quadratic, so use a simple stable merge via repeated
         * selection on keys. n is about 10^4; selection is still fast enough. */
        ref_log = calloc(n + 1u, sizeof *ref_log);
        {
            uint64_t *idx = calloc(n + 1u, sizeof *idx);
            uint64_t m;
            for (m = 0; m < n; m++) {
                idx[m] = m;
            }
            /* Stable sort via bottom-up merge of indices. */
            {
                uint64_t *tmp = calloc(n + 1u, sizeof *tmp);
                uint64_t w;
                for (w = 1; w < n; w *= 2u) {
                    uint64_t lo;
                    for (lo = 0; lo < n; lo += 2u * w) {
                        uint64_t mid = lo + w < n ? lo + w : n;
                        uint64_t hi = lo + 2u * w < n ? lo + 2u * w : n;
                        uint64_t x = lo;
                        uint64_t y = mid;
                        uint64_t z = lo;
                        while (x < mid && y < hi) {
                            const SuEvent *ex = &all[idx[x]];
                            const SuEvent *ey = &all[idx[y]];
                            int less_y = 0;
                            if (ey->system_index != ex->system_index) {
                                less_y = ey->system_index < ex->system_index;
                            } else if (ey->slot != ex->slot) {
                                less_y = ey->slot < ex->slot;
                            } else if (ey->seq != ex->seq) {
                                less_y = ey->seq < ex->seq;
                            } else {
                                less_y = src[idx[y]] < src[idx[x]];
                            }
                            tmp[z++] = less_y ? idx[y++] : idx[x++];
                        }
                        while (x < mid) {
                            tmp[z++] = idx[x++];
                        }
                        while (y < hi) {
                            tmp[z++] = idx[y++];
                        }
                    }
                    memcpy(idx, tmp, n * sizeof *idx);
                }
                free(tmp);
            }
            for (m = 0; m < n; m++) {
                ref_log[m] = all[idx[m]];
                ref_log[m].event_seq = m;
            }
            free(idx);
        }
        ref_n_log = n;
        (void)a;
        (void)b;
        free(recs);
        free(all);
        free(src);
    }
}

/* ---- concurrent writers (gated completion and fill order) ------------- */

typedef struct Run {
    SuCmdQ *q;
    uint64_t cs;
    uint32_t nchunks;
    uint32_t ntasks;
    const uint32_t *perm;
    int gated;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t next_claim;
    uint32_t fill_turn;
    uint32_t done_turn;
} Run;

static void *writer_main(void *arg) {
    Run *r = arg;
    for (;;) {
        uint32_t pos;
        uint32_t t;
        uint32_t sys;
        uint32_t chunk;
        uint64_t begin;
        uint64_t end;
        uint64_t s;

        pthread_mutex_lock(&r->mu);
        if (r->next_claim >= r->ntasks) {
            pthread_mutex_unlock(&r->mu);
            break;
        }
        pos = r->next_claim++;
        pthread_mutex_unlock(&r->mu);
        t = r->perm[pos];
        sys = t / r->nchunks;
        chunk = t % r->nchunks;
        begin = (uint64_t)chunk * r->cs;
        end = begin + r->cs < NROWS ? begin + r->cs : NROWS;

        if (r->gated) {
            pthread_mutex_lock(&r->mu);
            while (r->fill_turn != pos) {
                pthread_cond_wait(&r->cv, &r->mu);
            }
            pthread_mutex_unlock(&r->mu);
        }
        emit_row(r->q, r->cs, sys, (uint32_t)begin);
        if (r->gated) {
            pthread_mutex_lock(&r->mu);
            r->fill_turn++;
            pthread_cond_broadcast(&r->cv);
            pthread_mutex_unlock(&r->mu);
        }
        for (s = begin + 1u; s < end; s++) {
            emit_row(r->q, r->cs, sys, (uint32_t)s);
        }
        if (r->gated) {
            pthread_mutex_lock(&r->mu);
            while (r->done_turn != pos) {
                pthread_cond_wait(&r->cv, &r->mu);
            }
            r->done_turn++;
            pthread_cond_broadcast(&r->cv);
            pthread_mutex_unlock(&r->mu);
        }
    }
    return NULL;
}

static uint64_t fnv_bytes(const uint8_t *p, uint64_t n, uint64_t h) {
    uint64_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(0x100000001B3);
    }
    return h;
}

/* Runs one full tick with T writers. seed selects chunk size and interleaving. Returns the FNV
 * hash of the log dump, or 0 on a check failure (checks are recorded in globals). */
static uint64_t run_tick(uint32_t T, uint64_t seed, uint64_t cs, int gated) {
    SuCmdQ *q = NULL;
    SuEventLog *log = NULL;
    SuCmdView v;
    uint32_t nchunks = (uint32_t)((NROWS + cs - 1u) / cs);
    uint32_t ntasks = NSYS * nchunks;
    uint32_t *perm = calloc(ntasks, sizeof *perm);
    pthread_t *th = calloc(T, sizeof *th);
    Run r;
    uint32_t i;
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    Table t;
    CbCtx ctx;
    SuCmdCallbacks cb;
    SuCommitReport rep;
    uint32_t st;
    uint64_t k;
    uint8_t *dump;
    uint64_t dlen = 0u;
    SuEventLog *log2 = NULL;

    /* Seeded permutation of tasks (Fisher-Yates over splitmix). */
    {
        uint64_t x = seed;
        for (i = 0; i < ntasks; i++) {
            perm[i] = i;
        }
        for (i = ntasks; i > 1u; i--) {
            uint64_t z;
            uint32_t j;
            x += UINT64_C(0x9E3779B97F4A7C15);
            z = x;
            z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
            z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
            z ^= z >> 31;
            j = (uint32_t)(z % i);
            {
                uint32_t tmp = perm[i - 1u];
                perm[i - 1u] = perm[j];
                perm[j] = tmp;
            }
        }
    }

    CHECK(su_cmdq_create(NSYS, NROWS, cs, PER_ROW, PER_ROW, PER_TICK, PER_TICK, &q) == SU_OK);
    CHECK(q != NULL);
    CHECK(su_evlog_create(1u << 16, 0u, &log) == SU_OK);
    if (q == NULL || log == NULL) {
        goto done;
    }
    CHECK(su_cmdq_chunk_count(q) == nchunks);

    memset(&r, 0, sizeof r);
    r.q = q;
    r.cs = cs;
    r.nchunks = nchunks;
    r.ntasks = ntasks;
    r.perm = perm;
    r.gated = gated;
    pthread_mutex_init(&r.mu, NULL);
    pthread_cond_init(&r.cv, NULL);
    for (i = 0; i < T; i++) {
        if (pthread_create(&th[i], NULL, writer_main, &r) != 0) {
            CHECK(0);
            goto done;
        }
    }
    for (i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
    }
    pthread_mutex_destroy(&r.mu);
    pthread_cond_destroy(&r.cv);
    CHECK(__atomic_load_n(&g_emit_bad, __ATOMIC_RELAXED) == 0u);

    CHECK(!su_cmdq_overflowed(q));
    CHECK(su_cmdq_merge(q, &v) == SU_OK);
    CHECK(v.n_cmds == ref_n_cmds);
    CHECK(v.n_events == ref_n_sys_evts);
    CHECK(v.cmd_fallback == 0u && v.event_fallback == 0u);
    if (v.n_cmds == ref_n_cmds) {
        CHECK(memcmp(v.cmds, ref_cmds, (size_t)ref_n_cmds * sizeof *ref_cmds) == 0);
    }
    if (v.n_events == ref_n_sys_evts) {
        CHECK(memcmp(v.events, ref_sys_evts, (size_t)ref_n_sys_evts * sizeof *ref_sys_evts) == 0);
    }

    table_init(&t);
    cb = make_cb(&ctx, &t);
    st = su_cmdq_commit(q, &cb, TICK, log, &rep);
    CHECK(st == SU_OK);
    CHECK(rep.n_cmds == ref_n_cmds);
    CHECK(rep.n_despawned == ref_despawned && rep.n_rejected == ref_rejected);
    CHECK(rep.n_events == ref_n_log);
    CHECK(memcmp(&t, &ref_table_after, sizeof t) == 0);
    CHECK(su_evlog_next_seq(log) == ref_n_log);

    /* Readback equals the canonical expectation. */
    {
        SuEvent *out = calloc(ref_n_log + 1u, sizeof *out);
        uint64_t n_out = 0u;
        uint64_t nc = 0u;
        uint64_t sk = 0u;
        uint64_t dr = 0u;
        CHECK(su_evlog_read(log, 0u, ref_n_log, out, &n_out, &nc, &sk, &dr) == SU_OK);
        CHECK(n_out == ref_n_log && nc == ref_n_log && sk == 0u && dr == 0u);
        if (n_out == ref_n_log) {
            CHECK(memcmp(out, ref_log, (size_t)ref_n_log * sizeof *out) == 0);
        }
        free(out);
    }

    CHECK(su_evlog_dump(log, NULL, 0u, &dlen) == SU_OK);
    dump = calloc((size_t)dlen, 1u);
    CHECK(su_evlog_dump(log, dump, dlen, &dlen) == SU_OK);
    /* Parse round trip and equality. */
    CHECK(su_evlog_parse(dump, dlen, &log2) == SU_OK);
    CHECK(log2 != NULL && su_evlog_equal(log, log2));
    h = fnv_bytes(dump, dlen, h);
    free(dump);
    k = 0u;
    (void)k;

done:
    su_evlog_destroy(log2);
    su_evlog_destroy(log);
    su_cmdq_destroy(q);
    free(perm);
    free(th);
    return h;
}

static void test_canonical_interleavings(uint64_t *final_hash) {
    static const uint64_t chunks[] = {1u, 7u, 8u, 9u, 1000u, 1024u};
    static const uint32_t workers[] = {1u, 2u, 4u, 8u};
    uint64_t seed;
    uint64_t hash_ref = 0u;
    int have_ref = 0;
    uint32_t wi;

    for (wi = 0; wi < 4u; wi++) {
        for (seed = 0; seed < 60u; seed++) {
            uint64_t cs = chunks[seed % 6u];
            int gated = (int)(seed & 1u);
            uint64_t h = run_tick(workers[wi], seed * 7919u + 1u, cs, gated);
            if (!have_ref) {
                hash_ref = h;
                have_ref = 1;
            }
            /* Canonical bytes depend on the workload, not on chunking or interleaving. */
            CHECK(h == hash_ref);
        }
    }
    *final_hash = hash_ref;
}

/* ---- overflow ---------------------------------------------------------- */

typedef struct OverflowArg {
    SuCmdQ *q;
    uint32_t sys;
} OverflowArg;

static void *overflow_writer(void *p) {
    OverflowArg *a = p;
    uint32_t s;
    for (s = 0; s < 20u; s++) {
        (void)su_cmdq_append_struct(a->q, a->sys, 0u, s, SU_STRUCT_DESPAWN, s, 0, 0);
    }
    return NULL;
}

static void test_overflow(void) {
    SuCmdQ *q = NULL;
    SuEventLog *log = NULL;
    SuEventLog *snap = NULL;
    uint8_t *before;
    uint8_t *after;
    uint64_t lb = 0u;
    uint64_t la = 0u;
    uint64_t ns = 0u;
    uint64_t ne = 0u;
    Table t;
    CbCtx ctx;
    SuCmdCallbacks cb;
    SuCmdView v;
    SuCommitReport rep;
    uint64_t i;

    /* A: per-row struct cap. */
    CHECK(su_cmdq_create(1, 10, 10, 1, 1, 100, 100, &q) == SU_OK);
    CHECK(su_evlog_create(8, 0, &log) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 3, SU_STRUCT_DESPAWN, 1, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 3, SU_STRUCT_DESPAWN, 2, 0, 0) == SU_E_RESOURCE_LIMIT);
    CHECK(su_cmdq_overflowed(q) == 1);
    su_cmdq_counts(q, &ns, &ne);
    CHECK(ns == 1u && ne == 0u);
    CHECK(su_cmdq_merge(q, &v) == SU_E_RESOURCE_LIMIT);
    table_init(&t);
    cb = make_cb(&ctx, &t);
    CHECK(su_cmdq_commit(q, &cb, TICK, log, &rep) == SU_E_RESOURCE_LIMIT);
    CHECK(ctx.calls == 0u);
    CHECK(su_evlog_count(log) == 0u && su_evlog_next_seq(log) == 0u);
    su_cmdq_reset(q);
    CHECK(!su_cmdq_overflowed(q));
    su_cmdq_destroy(q);
    q = NULL;

    /* B: per-tick struct cap is exact. Five appends fit, the sixth overflows. */
    CHECK(su_cmdq_create(1, 10, 10, 3, 3, 5, 100, &q) == SU_OK);
    for (i = 0; i < 5u; i++) {
        CHECK(su_cmdq_append_struct(q, 0, 0, (uint32_t)i, SU_STRUCT_DESPAWN, i, 0, 0) == SU_OK);
    }
    CHECK(!su_cmdq_overflowed(q));
    su_cmdq_counts(q, &ns, &ne);
    CHECK(ns == 5u);
    CHECK(su_cmdq_append_struct(q, 0, 0, 5, SU_STRUCT_DESPAWN, 9, 0, 0) == SU_E_RESOURCE_LIMIT);
    CHECK(su_cmdq_overflowed(q));
    su_cmdq_counts(q, &ns, &ne);
    CHECK(ns == 5u);
    su_cmdq_destroy(q);
    q = NULL;

    /* C: per-tick event cap is exact too, and records count toward it at commit. */
    CHECK(su_cmdq_create(1, 10, 10, 3, 3, 10, 4, &q) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 0, 0x100, 1, 1) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 1, 0x100, 1, 1) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 2, 0x100, 1, 1) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 3, 0x100, 1, 1) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 4, 0x100, 1, 1) == SU_E_RESOURCE_LIMIT);
    CHECK(su_cmdq_overflowed(q));
    su_cmdq_destroy(q);
    q = NULL;
    /* 3 system events plus 2 commands is 5 records, over the cap of 4: refused before callbacks. */
    CHECK(su_cmdq_create(1, 10, 10, 3, 3, 10, 4, &q) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 0, 0x100, 1, 1) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 1, 0x100, 1, 1) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 2, 0x100, 1, 1) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 0, SU_STRUCT_DESPAWN, SU_ENTITY_MAKE(0, 1), 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 1, SU_STRUCT_DESPAWN, SU_ENTITY_MAKE(1, 1), 0, 0) == SU_OK);
    CHECK(!su_cmdq_overflowed(q));
    table_init(&t);
    cb = make_cb(&ctx, &t);
    CHECK(su_cmdq_commit(q, &cb, TICK, log, &rep) == SU_E_RESOURCE_LIMIT);
    CHECK(ctx.calls == 0u);
    CHECK(su_evlog_count(log) == 0u);
    su_cmdq_destroy(q);
    q = NULL;

    /* D: buffer capacity guard. Reappearing slots reset row caps but never exceed the buffer. */
    CHECK(su_cmdq_create(1, 4, 2, 2, 1, 100, 100, &q) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 0, SU_STRUCT_DESPAWN, 1, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 1, SU_STRUCT_DESPAWN, 2, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 0, SU_STRUCT_DESPAWN, 3, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 1, SU_STRUCT_DESPAWN, 4, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 0, SU_STRUCT_DESPAWN, 5, 0, 0) == SU_E_RESOURCE_LIMIT);
    CHECK(su_cmdq_overflowed(q));
    su_cmdq_destroy(q);
    q = NULL;

    /* E: concurrent overflow. 8 writers attempt 160 appends against a per-tick cap of 100. The
     * accepted count (100) and the overflow flag are the same for every interleaving. */
    {
        uint64_t run;
        for (run = 0; run < 50u; run++) {
            pthread_t th[8];
            OverflowArg oa[8];
            uint32_t w;
            CHECK(su_cmdq_create(8, 1024, 1024, 20, 1, 100, 100, &q) == SU_OK);
            for (w = 0; w < 8u; w++) {
                oa[w].q = q;
                oa[w].sys = w;
            }
            for (w = 0; w < 8u; w++) {
                CHECK(pthread_create(&th[w], NULL, overflow_writer, &oa[w]) == 0);
            }
            for (w = 0; w < 8u; w++) {
                pthread_join(th[w], NULL);
            }
            CHECK(su_cmdq_overflowed(q) == 1);
            su_cmdq_counts(q, &ns, &ne);
            CHECK(ns == 100u);
            su_cmdq_destroy(q);
            q = NULL;
        }
    }

    /* F: a refused append leaves the committed log bytes unchanged. */
    CHECK(su_cmdq_create(1, 10, 10, 1, 1, 10, 10, &q) == SU_OK);
    {
        uint64_t n1 = 0u;
        uint64_t n2 = 0u;
        SuEvent e;
        memset(&e, 0, sizeof e);
        e.event_seq = 0u;
        e.kind = 0x100u;
        CHECK(su_evlog_append(log, &e, 1) == SU_OK);
        CHECK(su_evlog_dump(log, NULL, 0u, &lb) == SU_OK);
        before = calloc((size_t)lb, 1u);
        CHECK(su_evlog_dump(log, before, lb, &lb) == SU_OK);
        CHECK(su_cmdq_append_struct(q, 0, 0, 0, SU_STRUCT_DESPAWN, 1, 0, 0) == SU_OK);
        CHECK(su_cmdq_append_struct(q, 0, 0, 0, SU_STRUCT_DESPAWN, 2, 0, 0) == SU_E_RESOURCE_LIMIT);
        table_init(&t);
        cb = make_cb(&ctx, &t);
        CHECK(su_cmdq_commit(q, &cb, TICK, log, &rep) == SU_E_RESOURCE_LIMIT);
        (void)n1;
        (void)n2;
        CHECK(su_evlog_dump(log, NULL, 0u, &la) == SU_OK);
        CHECK(la == lb);
        after = calloc((size_t)la, 1u);
        CHECK(su_evlog_dump(log, after, la, &la) == SU_OK);
        CHECK(memcmp(before, after, (size_t)lb) == 0);
        free(before);
        free(after);
    }
    su_cmdq_destroy(q);
    q = NULL;
    su_evlog_destroy(log);
    log = NULL;
    su_evlog_destroy(snap);
}

/* ---- out-of-order detection, fallback, reset ------------------------- */

static void test_out_of_order_and_reset(void) {
    SuCmdQ *q = NULL;
    SuCmdView v;
    SuCmdView v2;
    uint32_t st;

    CHECK(su_cmdq_create(1, 10, 10, 4, 1, 100, 100, &q) == SU_OK);
    /* Slots 5, 3 in one buffer: out of order. Canonical output is sorted by slot. */
    CHECK(su_cmdq_append_struct(q, 0, 0, 5, SU_STRUCT_DESPAWN, 50, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 3, SU_STRUCT_DESPAWN, 30, 0, 0) == SU_OK);
    st = su_cmdq_merge(q, &v);
    CHECK(st == SU_OK);
    CHECK(v.cmd_fallback == 1u && v.cmd_unsorted_buffers == 1u);
    CHECK(v.n_cmds == 2u);
    if (v.n_cmds == 2u) {
        CHECK(v.cmds[0].slot == 3u && v.cmds[0].arg == 30u);
        CHECK(v.cmds[1].slot == 5u && v.cmds[1].arg == 50u);
    }
    /* Reappearing slot 2 with 4 in between: stable order keeps emission order for equal keys. */
    su_cmdq_reset(q);
    CHECK(su_cmdq_append_struct(q, 0, 0, 2, SU_STRUCT_DESPAWN, 1, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 4, SU_STRUCT_DESPAWN, 2, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 2, SU_STRUCT_DESPAWN, 3, 0, 0) == SU_OK);
    CHECK(su_cmdq_merge(q, &v) == SU_OK);
    CHECK(v.cmd_fallback == 1u);
    if (v.n_cmds == 3u) {
        CHECK(v.cmds[0].slot == 2u && v.cmds[0].arg == 1u);
        CHECK(v.cmds[1].slot == 2u && v.cmds[1].arg == 3u);
        CHECK(v.cmds[2].slot == 4u && v.cmds[2].arg == 2u);
    }
    /* Sorted input: no fallback. */
    su_cmdq_reset(q);
    CHECK(su_cmdq_append_struct(q, 0, 0, 1, SU_STRUCT_DESPAWN, 1, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 7, SU_STRUCT_DESPAWN, 2, 0, 0) == SU_OK);
    CHECK(su_cmdq_merge(q, &v) == SU_OK);
    CHECK(v.cmd_fallback == 0u && v.cmd_unsorted_buffers == 0u);

    /* Reset reuses the same storage: pointers and results are identical. */
    su_cmdq_reset(q);
    CHECK(su_cmdq_append_struct(q, 0, 0, 1, SU_STRUCT_DESPAWN, 1, 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 0, 7, SU_STRUCT_DESPAWN, 2, 0, 0) == SU_OK);
    CHECK(su_cmdq_merge(q, &v2) == SU_OK);
    CHECK(v2.cmds == v.cmds && v2.n_cmds == v.n_cmds);
    CHECK(v2.n_cmds == 2u && v2.cmds[0].arg == 1u && v2.cmds[1].arg == 2u);
    su_cmdq_destroy(q);

    /* Invalid arguments. */
    CHECK(su_cmdq_create(0, 10, 10, 1, 1, 1, 1, &q) == SU_E_INVALID_ARGUMENT);
    CHECK(su_cmdq_create(1, 10, 10, 1, 1, 0, 1, &q) == SU_E_INVALID_ARGUMENT);
    CHECK(su_cmdq_create(1, 10, 10, 65, 1, 1, 1, &q) == SU_E_INVALID_ARGUMENT);
    CHECK(su_cmdq_create(1, 10, 10, 1, 1, 1, 1, NULL) == SU_E_INVALID_ARGUMENT);
    CHECK(su_cmdq_create(1u << 30, 1u << 30, 1, 1, 1, 1, 1, &q) == SU_E_RESOURCE_LIMIT);
    CHECK(su_cmdq_create(1, 10, 10, 1, 1, 1, 1, &q) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 0, 1, 0, SU_STRUCT_DESPAWN, 1, 0, 0) == SU_E_INVALID_ARGUMENT);
    CHECK(su_cmdq_append_struct(q, 1, 0, 0, SU_STRUCT_DESPAWN, 1, 0, 0) == SU_E_INVALID_ARGUMENT);
    CHECK(su_cmdq_append_event(q, 0, 0, 10, 0x100, 0, 0) == SU_E_INVALID_ARGUMENT);
    su_cmdq_destroy(q);
}

/* ---- apply and commit semantics -------------------------------------- */

static void test_apply_rejection_order(void) {
    SuCmdQ *q = NULL;
    SuEventLog *log = NULL;
    Table t;
    CbCtx ctx;
    SuCmdCallbacks cb;
    SuCommitReport rep;
    SuEvent out[8];
    uint64_t n = 0u;
    uint64_t nc = 0u;
    uint64_t sk = 0u;
    uint64_t dr = 0u;
    SuEvent recs[4];
    SuStructCmd cmds[2];
    uint64_t nrej = 0u;

    /* apply: NULL despawn with a despawn command is refused before any call. */
    memset(cmds, 0, sizeof cmds);
    cmds[0].kind = SU_STRUCT_DESPAWN;
    CHECK(su_cmdq_apply(cmds, 1, &(SuCmdCallbacks){0}, TICK, recs, 4) == SU_E_INVALID_ARGUMENT);
    CHECK(su_cmdq_apply(cmds, 2, &(SuCmdCallbacks){0}, TICK, recs, 1) == SU_E_RESOURCE_LIMIT);

    /* Two systems despawn the same entity in one tick: the second is rejected at its position. */
    CHECK(su_cmdq_create(2, 10, 10, 1, 1, 10, 10, &q) == SU_OK);
    CHECK(su_evlog_create(16, 0, &log) == SU_OK);
    table_init(&t);
    cb = make_cb(&ctx, &t);
    CHECK(su_cmdq_append_struct(q, 0, 0, 2, SU_STRUCT_DESPAWN, SU_ENTITY_MAKE(2, 1), 0, 0) == SU_OK);
    CHECK(su_cmdq_append_event(q, 0, 0, 2, 0x200, 5, 6) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 1, 0, 2, SU_STRUCT_DESPAWN, SU_ENTITY_MAKE(2, 1), 0, 0) == SU_OK);
    CHECK(su_cmdq_append_struct(q, 1, 0, 9, 7, 0, 0, 0) == SU_OK);
    CHECK(su_cmdq_commit(q, &cb, TICK, log, &rep) == SU_OK);
    CHECK(rep.n_despawned == 1u && rep.n_rejected == 2u && rep.n_events == 4u);
    CHECK(su_evlog_read(log, 0u, 8u, out, &n, &nc, &sk, &dr) == SU_OK);
    CHECK(n == 4u && sk == 0u && dr == 0u && nc == 4u);
    if (n == 4u) {
        /* Canonical: (sys0,slot2,seq0) despawn, (sys0,slot2,seq0) system event (after the record? no:
         * system event first on ties), then (sys1,slot2,seq0) rejected, (sys1,slot9,seq0) rejected. */
        CHECK(out[0].kind == 0x200u && out[0].system_index == 0u);
        CHECK(out[1].kind == SU_EVENT_DESPAWNED && out[1].system_index == 0u && out[1].a == 2u &&
              out[1].b == 1u);
        CHECK(out[2].kind == SU_EVENT_REJECTED && out[2].system_index == 1u && out[2].a == SU_STRUCT_DESPAWN &&
              out[2].b == SU_E_STALE_HANDLE);
        CHECK(out[3].kind == SU_EVENT_REJECTED && out[3].system_index == 1u && out[3].slot == 9u &&
              out[3].a == 7u && out[3].b == SU_E_INVALID_ARGUMENT);
        for (nrej = 0; nrej < 4u; nrej++) {
            CHECK(out[nrej].event_seq == nrej && out[nrej].tick == TICK);
        }
    }
    CHECK(ctx.calls == 2u);
    su_cmdq_destroy(q);
    su_evlog_destroy(log);
}

/* ---- ring model -------------------------------------------------------- */

static uint64_t rng_state = 88172645463325252ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_ring_model(void) {
    static const uint64_t caps[] = {1u, 2u, 3u, 8u, 64u};
    static const uint64_t bases[] = {0u, 5u};
    uint32_t ci;
    uint32_t bi;
    for (ci = 0; ci < 5u; ci++) {
        for (bi = 0; bi < 2u; bi++) {
            uint64_t cap = caps[ci];
            uint64_t base = bases[bi];
            SuEventLog *log = NULL;
            SuEvent *all = calloc(4096u, sizeof *all);
            SuEvent *out = calloc(64u, sizeof *out);
            uint64_t n = 0u;
            uint32_t op;
            CHECK(su_evlog_create(cap, base, &log) == SU_OK);
            for (op = 0; op < 400u; op++) {
                if (rng() % 3u != 0u) {
                    uint64_t k = rng() % 6u;
                    uint64_t j;
                    SuEvent batch[8];
                    for (j = 0; j < k; j++) {
                        memset(&batch[j], 0, sizeof batch[j]);
                        batch[j].event_seq = base + n + j;
                        batch[j].kind = (uint32_t)rng();
                        batch[j].a = (uint32_t)rng();
                        batch[j].slot = (uint32_t)j;
                    }
                    CHECK(su_evlog_append(log, batch, k) == SU_OK);
                    for (j = 0; j < k; j++) {
                        all[n + j] = batch[j];
                    }
                    n += k;
                }
                {
                    uint64_t dropped = n > cap ? n - cap : 0u;
                    uint64_t oldest = base + dropped;
                    uint64_t next = base + n;
                    uint64_t cursor = rng() % (next - base + 3u) + (rng() % 2u ? 0u : base);
                    uint64_t max = rng() % 7u;
                    uint64_t nout = 0u;
                    uint64_t nc = 0u;
                    uint64_t sk = 0u;
                    uint64_t dr = 0u;
                    uint32_t st;
                    if (cursor > next) {
                        CHECK(su_evlog_read(log, cursor, max, out, &nout, &nc, &sk, &dr) ==
                              SU_E_INVALID_ARGUMENT);
                    } else {
                        uint64_t start = cursor < oldest ? oldest : cursor;
                        uint64_t avail = next - start;
                        uint64_t cnt = avail < max ? avail : max;
                        uint64_t x;
                        st = su_evlog_read(log, cursor, max, out, &nout, &nc, &sk, &dr);
                        CHECK(st == SU_OK);
                        CHECK(nout == cnt);
                        CHECK(sk == (cursor < oldest ? oldest - cursor : 0u));
                        CHECK(dr == dropped);
                        CHECK(nc == start + cnt);
                        for (x = 0; x < cnt && x < nout; x++) {
                            CHECK(out[x].event_seq == start + x);
                            CHECK(memcmp(&out[x], &all[start - base + x], sizeof out[x]) == 0);
                        }
                    }
                    CHECK(su_evlog_count(log) == (n > cap ? cap : n));
                    CHECK(su_evlog_dropped(log) == dropped);
                    CHECK(su_evlog_oldest_seq(log) == oldest);
                    CHECK(su_evlog_next_seq(log) == next);
                }
            }
            /* Wrong sequence is refused and changes nothing. */
            {
                SuEvent bad;
                uint64_t before = su_evlog_next_seq(log);
                memset(&bad, 0, sizeof bad);
                bad.event_seq = before + 1u;
                CHECK(su_evlog_append(log, &bad, 1) == SU_E_INVALID_ARGUMENT);
                CHECK(su_evlog_next_seq(log) == before);
            }
            su_evlog_destroy(log);
            free(all);
            free(out);
        }
    }
}

/* ---- dump, parse and corruption --------------------------------------- */

static void test_dump_parse(void) {
    SuEventLog *log = NULL;
    SuEventLog *p = NULL;
    uint8_t *buf;
    uint64_t len = 0u;
    uint64_t i;
    SuEvent ev[6];

    CHECK(su_evlog_create(4, 100, &log) == SU_OK);
    for (i = 0; i < 6u; i++) {
        memset(&ev[i], 0, sizeof ev[i]);
        ev[i].event_seq = 100u + i;
        ev[i].tick = 3u;
        ev[i].kind = 0x101u;
        ev[i].system_index = (uint32_t)i;
        ev[i].slot = (uint32_t)(i * 3u);
        ev[i].seq = 1u;
        ev[i].a = 0xDEADBEEFu;
        ev[i].b = (uint32_t)i;
    }
    CHECK(su_evlog_append(log, ev, 6) == SU_OK); /* capacity 4: two evicted */
    CHECK(su_evlog_dropped(log) == 2u && su_evlog_count(log) == 4u);
    CHECK(su_evlog_dump(log, NULL, 0u, &len) == SU_OK);
    CHECK(len == 44u + 40u * 4u);
    buf = calloc((size_t)len, 1u);
    CHECK(su_evlog_dump(log, buf, len - 1u, &len) == SU_E_RESOURCE_LIMIT);
    CHECK(su_evlog_dump(log, buf, len, &len) == SU_OK);
    CHECK(su_evlog_parse(buf, len, &p) == SU_OK && p != NULL && su_evlog_equal(log, p));
    CHECK(su_evlog_oldest_seq(p) == 102u && su_evlog_dropped(p) == 2u);
    su_evlog_destroy(p);
    p = NULL;

    /* Corruptions. Each must be rejected and must not write through the input. */
    {
        uint8_t *c = malloc((size_t)len + 16u);
#define REJECT_WITH(expect, mutate_expr, outlen)                                        \
        do {                                                                            \
            memcpy(c, buf, (size_t)len);                                                \
            mutate_expr;                                                                \
            CHECK(su_evlog_parse(c, (outlen), &p) == (expect));                         \
            CHECK(p == NULL);                                                           \
        } while (0)
        REJECT_WITH(SU_E_PARSE, (void)0, len - 1u);                       /* truncated */
        REJECT_WITH(SU_E_PARSE, c[len] = 0u, len + 1u);                   /* trailing byte */
        REJECT_WITH(SU_E_PARSE, c[0] = 'X', len);                         /* magic */
        REJECT_WITH(SU_E_PARSE, c[4] = 2u, len);                          /* version */
        REJECT_WITH(SU_E_PARSE, c[8] = 1u, len);                          /* reserved */
        REJECT_WITH(SU_E_PARSE, c[36] = 5u, len);                         /* count > capacity or len mismatch */
        REJECT_WITH(SU_E_PARSE, c[12] = 0u, len);                         /* capacity 0 */
        REJECT_WITH(SU_E_PARSE, c[28] = 0u, len);                         /* dropped 0 with full ring is ok? see below */
        REJECT_WITH(SU_E_PARSE, c[20] = 99u, len);                        /* base wrong: seq mismatch */
        REJECT_WITH(SU_E_PARSE, (void)0, 10u);                            /* shorter than header */
        REJECT_WITH(SU_E_PARSE, (void)0, 0u);
        REJECT_WITH(SU_E_PARSE, memcpy(c + 44, buf + 84, 40), len);       /* seq order: entry 0 replaced */
        REJECT_WITH(SU_E_PARSE, (void)0, len - 40u);                      /* one entry short */
        free(c);
#undef REJECT_WITH
    }
    /* Oversize capacity and length are refused before allocation. The buffer is only 44 bytes, so
     * any read past it would be a sanitizer report. */
    {
        uint8_t hdr[44];
        memcpy(hdr, buf, 44);
        memset(hdr + 12, 0xFF, 8);
        CHECK(su_evlog_parse(hdr, 44u, &p) == SU_E_RESOURCE_LIMIT && p == NULL);
        CHECK(su_evlog_parse(hdr, UINT64_C(1) << 40, &p) == SU_E_RESOURCE_LIMIT && p == NULL);
        CHECK(su_evlog_parse(NULL, 5u, &p) == SU_E_INVALID_ARGUMENT);
    }
    /* A non-full ring with dropped > 0 is inconsistent. */
    {
        uint8_t *c = malloc((size_t)len);
        memcpy(c, buf, (size_t)len);
        c[36] = 3u; /* count 3 with capacity 4 and dropped 2 */
        CHECK(su_evlog_parse(c, 44u + 40u * 3u, &p) == SU_E_PARSE && p == NULL);
        free(c);
    }
    /* Empty log round trip. */
    {
        SuEventLog *e = NULL;
        SuEventLog *e2 = NULL;
        uint8_t eb[44];
        uint64_t el = 0u;
        CHECK(su_evlog_create(1, 0, &e) == SU_OK);
        CHECK(su_evlog_dump(e, eb, sizeof eb, &el) == SU_OK && el == 44u);
        CHECK(su_evlog_parse(eb, el, &e2) == SU_OK && su_evlog_equal(e, e2));
        su_evlog_destroy(e2);
        su_evlog_destroy(e);
    }
    free(buf);
    su_evlog_destroy(log);
}

int main(void) {
    uint64_t hash = 0u;
    build_reference();
    test_canonical_interleavings(&hash);
    test_overflow();
    test_out_of_order_and_reset();
    test_apply_rejection_order();
    test_ring_model();
    test_dump_parse();
    printf("test_cmdq: checks=%lu failures=%lu canonical_dump_fnv=%016llx\n", g_checks, g_failures,
           (unsigned long long)hash);
    free(ref_cmds);
    free(ref_sys_evts);
    free(ref_log);
    (void)NTEMPLATE_UNUSED;
    return g_failures == 0u ? 0 : 1;
}
