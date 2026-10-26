/* Entity-aware tick execution (M2-00c3). See entity_tick.h for the protocol. */
#define _POSIX_C_SOURCE 200809L

#include "runtime/entity_tick.h"

#include <stdlib.h>
#include <string.h>

#include "runtime/jobs/su_jobs.h"
#include "runtime/schedule.h"
#include "vm/su_vm.h"
#include "world/entities/su_ents.h"
#include "world/su_world.h"

/* Per-chunk emission target: the writer of (system, chunk). One per worker; `chunk` is set before
 * each chunk runs on that worker. */
typedef struct EntEmit {
    SuCmdQ *q;
    uint32_t sys;
    uint64_t chunk;
} EntEmit;

/* State of one entity system for one run call. Column views are rebuilt per tick (publish swaps the
 * column buffers); contexts and scratch arrays live for the call. */
typedef struct EntSys {
    const SuLirModule *mod;
    SuWorld *w;
    SuCmdQ *q;
    uint32_t sys;
    uint64_t rows;
    uint32_t tick_low;
    uint32_t layout_generation;
    uint32_t nin;
    uint32_t nout;
    uint32_t wk;
    SuColumnBinding *base_in;   /* nin, whole-table views for this tick */
    SuColumnBinding *base_out;  /* nout */
    SuColumnBinding *scr_in;    /* wk * nin slices */
    SuColumnBinding *scr_out;   /* wk * nout slices */
    uint32_t *write_ids;        /* nout */
    SuExecContext **ctx;        /* wk */
    uint8_t **alive;            /* wk buffers of SU_CHUNK_SLOTS bytes */
    uint32_t **gen;             /* wk buffers of SU_CHUNK_SLOTS u32 */
    uint8_t *all_alive;         /* IR 0.4 handle_live: whole-table alive bytes and generations, filled per tick */
    uint32_t *all_gen;
    EntEmit *emit;              /* wk */
    SuVmEmitter *vem;           /* wk */
    uint32_t *status;           /* one per chunk of this run */
} EntSys;

/* Per-call state kept between su_ent_run_ticks calls (2026-10-18): the command queue, job pool, per-system VM
 * contexts (with their plans) and scratch arrays depend only on the key below, so a caller that steps one tick
 * per call (or re-simulates K ticks after a rollback) does not rebuild them every call. The key holds every
 * input of their construction; any difference rebuilds. Results do not depend on reuse: every tick resets the
 * queue, rebinds the column views and sets each chunk's entity view, exactly as a fresh call does. */
typedef struct EntCache {
    int valid;
    const SuSchedule *s;
    uint32_t count;
    uint32_t layout_generation;
    uint64_t rows;
    uint32_t wk;
    uint32_t vm_tier;
    const SuLirModule *mods[SU_SCHED_MAX_SYSTEMS];
    uint8_t sem[SU_SCHED_MAX_SYSTEMS][32];
    uint64_t rev[SU_SCHED_MAX_SYSTEMS];
    uint64_t txid[SU_SCHED_MAX_SYSTEMS];
    uint32_t ps[SU_SCHED_MAX_SYSTEMS];
    uint32_t pe[SU_SCHED_MAX_SYSTEMS];
    uint32_t tier[SU_SCHED_MAX_SYSTEMS];  /* SuSysEntry.vm_tier (0 = the process default) */
    uint8_t has_es[SU_SCHED_MAX_SYSTEMS]; /* es[i] holds contexts (never read a cached module: it may be freed) */
    SuCmdQ *q;
    SuJobPool *pool;
    EntSys *es;
    uint32_t *status;
} EntCache;

struct SuEntRt {
    uint64_t max_struct_tick;
    uint64_t max_events_tick;
    SuEventLog *log;
    EntCache cache;
};

static void ent_sys_free(EntSys *e);

static void ent_cache_drop(EntCache *c) {
    uint32_t i;
    if (c->es != NULL) {
        for (i = 0; i < c->count; i++) {
            if (c->has_es[i]) {
                ent_sys_free(&c->es[i]);
            }
        }
    }
    free(c->es);
    free(c->status);
    su_jobs_pool_destroy(c->pool);
    su_cmdq_destroy(c->q);
    memset(c, 0, sizeof *c);
}

/* ---- emitter callbacks (called by the VM on the worker running the chunk) ---- */

static int ent_emit_despawn(void *user, uint32_t slot, uint32_t seq, uint64_t expected_handle) {
    EntEmit *e = user;
    return su_cmdq_append_struct(e->q, e->sys, e->chunk, slot, seq, SU_STRUCT_DESPAWN, expected_handle,
                                 0u, 0u) == SU_OK
               ? 0
               : -1;
}

static int ent_emit_event(void *user, uint32_t slot, uint32_t seq, uint32_t kind, uint32_t a, uint32_t b) {
    EntEmit *e = user;
    return su_cmdq_append_event(e->q, e->sys, e->chunk, slot, seq, kind, a, b) == SU_OK ? 0 : -1;
}

/* ---- lifecycle ---- */

uint32_t su_ent_rt_create(uint64_t max_struct_tick, uint64_t max_events_tick, uint64_t event_log_max,
                          SuEntRt **out) {
    SuEntRt *rt;
    uint32_t st;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (max_struct_tick == 0u || max_struct_tick > SU_CMDQ_MAX_TICK_CAP || max_events_tick == 0u ||
        max_events_tick > SU_CMDQ_MAX_TICK_CAP || event_log_max == 0u ||
        event_log_max > SU_CMDQ_MAX_LOG_CAPACITY) {
        return SU_E_RESOURCE_LIMIT;
    }
    rt = calloc(1u, sizeof *rt);
    if (rt == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_evlog_create(event_log_max, 0u, &rt->log);
    if (st != SU_OK) {
        free(rt);
        return st;
    }
    rt->max_struct_tick = max_struct_tick;
    rt->max_events_tick = max_events_tick;
    *out = rt;
    return SU_OK;
}

void su_ent_rt_destroy(SuEntRt *rt) {
    if (rt == NULL) {
        return;
    }
    ent_cache_drop(&rt->cache);
    su_evlog_destroy(rt->log);
    free(rt);
}

SuEventLog *su_ent_rt_log(SuEntRt *rt) {
    return rt == NULL ? NULL : rt->log;
}

/* Per-system structural and event caps per row (OD-20, M3 contract 9.5). Only systems that use entity
 * operations have a cap; every other system's buffers are empty. ps and pe hold s->count entries. */
static void sys_row_caps(const SuSchedule *s, uint32_t *ps, uint32_t *pe) {
    uint32_t i;

    for (i = 0; i < s->count; i++) {
        const SuLirModule *m = s->sys[i].mod;
        ps[i] = m->uses_entity_ops ? m->max_struct_per_row : 0u;
        pe[i] = m->uses_entity_ops ? m->max_events_per_row : 0u;
    }
}

/* Admission check (contract B8): the command and event buffers a tick over rows slots would allocate, summed
 * over every (system, chunk) exactly as su_cmdq_create_ex sizes them, must fit SU_CMDQ_MAX_ENTRIES per class.
 * Returns SU_E_RESOURCE_LIMIT otherwise; nothing is allocated. */
uint32_t su_ent_budget(const SuSchedule *s, uint64_t rows, uint64_t max_struct_tick, uint64_t max_events_tick) {
    uint32_t ps[SU_SCHED_MAX_SYSTEMS];
    uint32_t pe[SU_SCHED_MAX_SYSTEMS];
    uint64_t nchunks;
    uint64_t c;
    uint64_t total_s = 0;
    uint64_t total_e = 0;
    uint32_t i;

    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (s->count == 0u || rows == 0u) {
        return SU_OK;
    }
    sys_row_caps(s, ps, pe);
    nchunks = (rows + SU_CHUNK_SLOTS - 1u) / SU_CHUNK_SLOTS;
    for (i = 0; i < s->count; i++) {
        for (c = 0; c < nchunks; c++) {
            uint64_t size = rows - c * SU_CHUNK_SLOTS < SU_CHUNK_SLOTS ? rows - c * SU_CHUNK_SLOTS : SU_CHUNK_SLOTS;
            uint64_t cs = size * ps[i] < max_struct_tick ? size * ps[i] : max_struct_tick;
            uint64_t ce = size * pe[i] < max_events_tick ? size * pe[i] : max_events_tick;
            total_s += cs;
            total_e += ce;
        }
    }
    if (total_s > SU_CMDQ_MAX_ENTRIES || total_e > SU_CMDQ_MAX_ENTRIES) {
        return SU_E_RESOURCE_LIMIT;
    }
    return SU_OK;
}

uint32_t su_ent_rt_caps(const SuEntRt *rt, uint64_t *max_struct_tick, uint64_t *max_events_tick,
                        uint64_t *event_log_max) {
    if (rt == NULL || max_struct_tick == NULL || max_events_tick == NULL || event_log_max == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *max_struct_tick = rt->max_struct_tick;
    *max_events_tick = rt->max_events_tick;
    *event_log_max = su_evlog_capacity(rt->log);
    return SU_OK;
}

/* Restored runtime (snapshot 0.2): caps and the log state as recorded. */
uint32_t su_ent_rt_create_restored(uint64_t max_struct_tick, uint64_t max_events_tick, uint64_t event_log_max,
                                   uint64_t next_seq, uint64_t dropped, const SuEvent *ev, uint64_t n,
                                   SuEntRt **out) {
    SuEntRt *rt;
    SuEventLog *log = NULL;
    uint32_t st;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (max_struct_tick == 0u || max_struct_tick > SU_CMDQ_MAX_TICK_CAP || max_events_tick == 0u ||
        max_events_tick > SU_CMDQ_MAX_TICK_CAP || event_log_max == 0u ||
        event_log_max > SU_CMDQ_MAX_LOG_CAPACITY) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_evlog_restore(event_log_max, next_seq, dropped, ev, n, &log);
    if (st != SU_OK) {
        return st;
    }
    rt = calloc(1u, sizeof *rt);
    if (rt == NULL) {
        su_evlog_destroy(log);
        return SU_E_RESOURCE_LIMIT;
    }
    rt->max_struct_tick = max_struct_tick;
    rt->max_events_tick = max_events_tick;
    rt->log = log;
    *out = rt;
    return SU_OK;
}

/* ---- per-system chunk execution ---- */

static const SuColumnBinding *find_binding(const SuColumnBinding *b, uint32_t n, uint32_t id) {
    return su_world_find_binding(b, n, id);
}

/* Slices one whole-table binding to rows [begin, begin+len). */
static void slice(const SuColumnBinding *src, SuColumnBinding *dst, uint64_t begin, uint64_t len) {
    *dst = *src;
    dst->data = (uint8_t *)src->data + begin * sizeof(uint32_t);
    dst->row_count = len;
}

static void ent_chunk(uint32_t chunk, uint32_t worker, void *argp) {
    EntSys *e = argp;
    uint64_t begin = (uint64_t)chunk * SU_CHUNK_SLOTS;
    uint64_t len = e->rows - begin;
    SuColumnBinding *si = e->scr_in + (size_t)worker * e->nin;
    SuColumnBinding *so = e->scr_out + (size_t)worker * e->nout;
    SuInvocation inv;
    SuOutputs outs;
    uint32_t i;
    uint32_t st;

    if (len > SU_CHUNK_SLOTS) {
        len = SU_CHUNK_SLOTS;
    }
    st = su_world_alive_view(e->w, (uint32_t)begin, (uint32_t)len, e->alive[worker]);
    if (st == SU_OK) {
        st = su_world_generation_view(e->w, (uint32_t)begin, (uint32_t)len, e->gen[worker]);
    }
    if (st != SU_OK) {
        e->status[chunk] = st;
        return;
    }
    e->emit[worker].q = e->q;
    e->emit[worker].sys = e->sys;
    e->emit[worker].chunk = chunk;
    st = su_vm_context_set_entity_view(e->ctx[worker], e->alive[worker], e->gen[worker], (uint32_t)begin,
                                       e->tick_low, &e->vem[worker]);
    if (st != SU_OK) {
        e->status[chunk] = st;
        return;
    }
    for (i = 0; i < e->nin; i++) {
        slice(&e->base_in[i], &si[i], begin, len);
    }
    for (i = 0; i < e->nout; i++) {
        slice(&e->base_out[i], &so[i], begin, len);
    }
    inv.struct_size = (uint32_t)sizeof(SuInvocation);
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = e->layout_generation;
    inv.binding_count = e->nin;
    inv.row_count = len;
    inv.bindings = si;
    outs.struct_size = (uint32_t)sizeof(SuOutputs);
    outs.binding_count = e->nout;
    outs.failed_insn = UINT32_MAX;
    outs.reserved = 0u;
    outs.bindings = so;
    if (e->mod->uses_gather) {
        /* IR 0.4 cross-row reads see the whole table (entry state), never just this chunk. */
        SuInvocation whole = inv;
        whole.bindings = e->base_in;
        whole.row_count = e->rows;
        st = su_vm_context_set_table_view(e->ctx[worker], &whole);
        if (st != SU_OK) {
            e->status[chunk] = st;
            return;
        }
    }
    if (e->mod->uses_handle_live) {
        /* IR 0.4 handle_live answers from the whole entity table of the tick start (structural changes apply only
         * at the boundary), never just this chunk. */
        st = su_vm_context_set_entity_table(e->ctx[worker], e->all_alive, e->all_gen, e->rows);
        if (st != SU_OK) {
            e->status[chunk] = st;
            return;
        }
    }
    e->status[chunk] = su_vm_invoke_v1(e->ctx[worker], &inv, &outs);
}

static void ent_sys_free(EntSys *e) {
    uint32_t k;
    if (e->ctx != NULL) {
        for (k = 0; k < e->wk; k++) {
            su_vm_context_destroy(e->ctx[k]);
        }
    }
    if (e->alive != NULL) {
        for (k = 0; k < e->wk; k++) {
            free(e->alive[k]);
        }
    }
    if (e->gen != NULL) {
        for (k = 0; k < e->wk; k++) {
            free(e->gen[k]);
        }
    }
    free(e->ctx);
    free(e->alive);
    free(e->gen);
    free(e->all_alive);
    free(e->all_gen);
    free(e->emit);
    free(e->vem);
    free(e->base_in);
    free(e->base_out);
    free(e->scr_in);
    free(e->scr_out);
    free(e->write_ids);
    memset(e, 0, sizeof *e);
}

static uint32_t ent_sys_create(EntSys *e, const SuSchedule *s, uint32_t i, SuWorld *w, SuCmdQ *q,
                               uint32_t wk, uint64_t rows, uint32_t *status) {
    const SuLirModule *m = s->sys[i].mod;
    uint32_t k;
    uint32_t st = SU_E_RESOURCE_LIMIT;
    size_t nf = m->field_count > 0u ? m->field_count : 1u;

    memset(e, 0, sizeof *e);
    e->mod = m;
    e->w = w;
    e->q = q;
    e->sys = i;
    e->rows = rows;
    e->layout_generation = s->layout_generation;
    e->wk = wk;
    e->status = status;
    e->base_in = calloc(nf, sizeof *e->base_in);
    e->base_out = calloc(nf, sizeof *e->base_out);
    e->write_ids = calloc(nf, sizeof *e->write_ids);
    e->scr_in = calloc((size_t)wk * nf, sizeof *e->scr_in);
    e->scr_out = calloc((size_t)wk * nf, sizeof *e->scr_out);
    e->ctx = calloc(wk, sizeof *e->ctx);
    e->alive = calloc(wk, sizeof *e->alive);
    e->gen = calloc(wk, sizeof *e->gen);
    e->emit = calloc(wk, sizeof *e->emit);
    e->vem = calloc(wk, sizeof *e->vem);
    if (e->base_in == NULL || e->base_out == NULL || e->write_ids == NULL || e->scr_in == NULL ||
        e->scr_out == NULL || e->ctx == NULL || e->alive == NULL || e->gen == NULL || e->emit == NULL ||
        e->vem == NULL) {
        goto fail;
    }
    if (m->uses_handle_live) {
        e->all_alive = malloc(rows > 0u ? (size_t)rows : 1u);
        e->all_gen = malloc((rows > 0u ? (size_t)rows : 1u) * sizeof(uint32_t));
        if (e->all_alive == NULL || e->all_gen == NULL) {
            goto fail;
        }
    }
    for (k = 0; k < wk; k++) {
        e->alive[k] = malloc(SU_CHUNK_SLOTS);
        e->gen[k] = malloc((size_t)SU_CHUNK_SLOTS * sizeof(uint32_t));
        if (e->alive[k] == NULL || e->gen[k] == NULL) {
            goto fail;
        }
        st = su_vm_context_create(m, s->layout_generation, &e->ctx[k]);
        if (st == SU_OK && s->sys[i].vm_tier != 0u) {
            st = su_vm_context_set_tier(e->ctx[k], s->sys[i].vm_tier);
        }
        if (st != SU_OK) {
            goto fail;
        }
        e->vem[k].user = &e->emit[k];
        e->vem[k].emit_despawn = ent_emit_despawn;
        e->vem[k].emit_event = ent_emit_event;
    }
    return SU_OK;
fail:
    ent_sys_free(e);
    return st == SU_OK ? SU_E_RESOURCE_LIMIT : st;
}

/* Binds the module's declared fields to this tick's whole-table views (module field order, as
 * su_vm_run_system does). */
static uint32_t ent_sys_bind(EntSys *e) {
    SuInvocation wi;
    SuOutputs wo = {0};
    uint32_t st;
    uint32_t i;
    uint32_t nin = 0;
    uint32_t nout = 0;

    st = su_world_build_invocation(e->w, &wi);
    if (st == SU_OK) {
        st = su_world_build_outputs(e->w, &wo);
    }
    if (st != SU_OK) {
        return st;
    }
    for (i = 0; i < e->mod->field_count; i++) {
        uint32_t acc = e->mod->fields[i].access;
        uint32_t id = e->mod->fields[i].id;
        if ((acc & SU_ACCESS_READ) != 0u) {
            const SuColumnBinding *b = find_binding(wi.bindings, wi.binding_count, id);
            if (b == NULL) {
                return SU_E_SCHEMA_MISMATCH;
            }
            e->base_in[nin++] = *b;
        }
        if ((acc & SU_ACCESS_WRITE) != 0u) {
            const SuColumnBinding *b = find_binding(wo.bindings, wo.binding_count, id);
            if (b == NULL) {
                return SU_E_SCHEMA_MISMATCH;
            }
            e->write_ids[nout] = id;
            e->base_out[nout++] = *b;
        }
    }
    e->nin = nin;
    e->nout = nout;
    return SU_OK;
}

/* Runs every chunk of one entity system into the queue. Returns SU_OK when all chunks succeeded.
 * On failure the lowest failing chunk's status is returned and nothing is committed. */
static uint32_t ent_sys_run(EntSys *e, SuJobPool *pool, uint64_t chunks, const uint32_t *order) {
    uint32_t st;
    uint64_t c;

    st = ent_sys_bind(e);
    if (st == SU_OK && e->mod->uses_handle_live) {
        st = su_world_alive_view(e->w, 0u, (uint32_t)e->rows, e->all_alive);
        if (st == SU_OK) {
            st = su_world_generation_view(e->w, 0u, (uint32_t)e->rows, e->all_gen);
        }
    }
    if (st != SU_OK) {
        return st;
    }
    if (chunks > 0u) {
        st = su_jobs_pool_run(pool, (uint32_t)chunks, ent_chunk, e, order, NULL);
        if (st != SU_OK) {
            return st;
        }
    }
    for (c = 0; c < chunks; c++) {
        if (e->status[c] != SU_OK) {
            return e->status[c];
        }
    }
    /* Commit this system's staging for its write fields (the caller zeroes dead rows, contract B1). */
    return su_world_commit_staging(e->w, e->write_ids, e->nout);
}

/* ---- boundary ---- */

static uint32_t despawn_cb(void *ctx, uint64_t handle, uint32_t *old_gen) {
    SuEnts *t = ctx;
    uint32_t st = su_ents_despawn(t, handle);
    if (st == SU_OK && old_gen != NULL) {
        *old_gen = SU_ENTITY_GEN(handle);
    }
    return st;
}

/* Merge, apply to the working table, log, zero the despawned columns. The caller publishes. */
static uint32_t boundary(SuWorld *w, SuCmdQ *q, SuEventLog *log, uint64_t tick) {
    SuCmdCallbacks cb;
    SuCommitReport rep;
    uint32_t st;
    /* The working table belongs to the world and is mutated only inside the open tick, which the
     * caller guarantees. The cast drops const only for this boundary call. */
    cb.ctx = (SuEnts *)su_world_entities_working(w);
    cb.despawn = despawn_cb;
    memset(&rep, 0, sizeof rep);
    if (cb.ctx == NULL) {
        return SU_E_INTERNAL;
    }
    st = su_cmdq_commit(q, &cb, tick, log, &rep);
    if (st != SU_OK) {
        return st;
    }
    return su_world_zero_dead(w);
}

/* ---- tick loop ---- */

uint32_t su_ent_run_ticks(SuWorld *w, SuSchedule *s, SuEntRt *rt, uint64_t ticks, SuSchedFault fault,
                          void *fault_ctx, const SuSchedTickOpts *opt, uint64_t *done,
                          uint32_t *failed_index) {
    SuCmdQ *q = NULL;
    SuJobPool *pool = NULL;
    EntSys *es = NULL;
    EntCache *c = NULL;
    int hit;
    uint32_t *perm = NULL; /* test seam: forced chunk completion order (opt->seeded_completion) */
    uint64_t rows;
    uint64_t chunks;
    uint64_t t;
    uint32_t ps[SU_SCHED_MAX_SYSTEMS];
    uint32_t pe[SU_SCHED_MAX_SYSTEMS];
    uint32_t wk = 1u;
    uint32_t i;
    uint32_t st = SU_OK;

    *done = 0u;
    *failed_index = UINT32_MAX;
    if (w == NULL || s == NULL || rt == NULL || (ticks > 0u && s->count == 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (opt != NULL) {
        wk = opt->workers;
    }
    if (wk == 0u || wk > SU_JOBS_MAX_WORKERS) {
        return SU_E_INVALID_ARGUMENT;
    }
    rows = su_world_row_count(w);
    chunks = (rows + SU_CHUNK_SLOTS - 1u) / SU_CHUNK_SLOTS;
    if (ticks == 0u) {
        return SU_OK;
    }
    sys_row_caps(s, ps, pe);
    c = &rt->cache;
    hit = c->valid && c->s == s && c->count == s->count && c->layout_generation == s->layout_generation &&
          c->rows == rows && c->wk == wk && c->vm_tier == su_vm_default_tier();
    for (i = 0; hit && i < s->count; i++) {
        const SuSysEntry *e = &s->sys[i];
        hit = c->mods[i] == e->mod && memcmp(c->sem[i], e->mod->semantic_hash, 32u) == 0 &&
              c->rev[i] == e->system_rev && c->txid[i] == e->txid && c->ps[i] == ps[i] && c->pe[i] == pe[i] &&
              c->tier[i] == e->vm_tier;
    }
    if (!hit) {
        ent_cache_drop(c);
        st = su_cmdq_create_ex_checked(s->count, rows, SU_CHUNK_SLOTS,
                                      ps, SU_SCHED_MAX_SYSTEMS, pe, SU_SCHED_MAX_SYSTEMS,
                                      rt->max_struct_tick, rt->max_events_tick, &c->q);
        if (st == SU_OK) {
            st = su_jobs_pool_create(wk, &c->pool);
        }
        if (st == SU_OK) {
            c->es = calloc(s->count > 0u ? s->count : 1u, sizeof *c->es);
            c->status = calloc(chunks > 0u ? (size_t)chunks : 1u, sizeof *c->status);
            if (c->es == NULL || c->status == NULL) {
                st = SU_E_RESOURCE_LIMIT;
            }
        }
        c->count = s->count;
        for (i = 0; i < s->count; i++) {
            c->mods[i] = s->sys[i].mod;
        }
        for (i = 0; st == SU_OK && i < s->count; i++) {
            if (s->sys[i].mod->uses_entity_ops) {
                st = ent_sys_create(&c->es[i], s, i, w, c->q, wk, rows, c->status);
                c->has_es[i] = st == SU_OK; /* on failure ent_sys_create freed its own state */
            }
        }
        if (st != SU_OK) {
            ent_cache_drop(c);
            return st;
        }
        c->s = s;
        c->layout_generation = s->layout_generation;
        c->rows = rows;
        c->wk = wk;
        c->vm_tier = su_vm_default_tier();
        for (i = 0; i < s->count; i++) {
            memcpy(c->sem[i], s->sys[i].mod->semantic_hash, 32u);
            c->rev[i] = s->sys[i].system_rev;
            c->txid[i] = s->sys[i].txid;
            c->ps[i] = ps[i];
            c->pe[i] = pe[i];
            c->tier[i] = s->sys[i].vm_tier;
        }
        c->valid = 1;
    }
    q = c->q;
    pool = c->pool;
    es = c->es;
    for (i = 0; i < s->count; i++) {
        if (s->sys[i].mod->uses_entity_ops) {
            es[i].w = w; /* the world may be a different object between calls (restore) */
        }
    }
    if (opt != NULL && opt->seeded_completion && chunks > 0u) {
        perm = calloc((size_t)chunks, sizeof *perm);
        if (perm == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
    }

    for (t = 0; t < ticks; t++) {
        uint32_t tick_low = (uint32_t)(su_world_tick(w) + 1u);
        uint64_t next_tick = su_world_tick(w) + 1u;
        st = su_world_begin_tick(w);
        if (st != SU_OK) {
            goto out;
        }
        su_cmdq_reset(q);
        for (i = 0; i < s->count && st == SU_OK; i++) {
            if (fault != NULL && fault(i, fault_ctx) != 0) {
                st = SU_E_CANDIDATE_FAILED;
                break;
            }
            if (s->sys[i].mod->uses_entity_ops) {
                const uint32_t *order = NULL;
                es[i].tick_low = tick_low;
                if (perm != NULL) {
                    /* A different seeded completion order for every (tick, system): the merge must not care. */
                    su_jobs_seeded_permutation((uint32_t)chunks, opt->completion_seed + t * 1000003u + i * 7919u,
                                               perm);
                    order = perm;
                }
                st = ent_sys_run(&es[i], pool, chunks, order);
            } else {
                st = su_sched_run_system(w, s, i, opt);
            }
            if (st == SU_OK) {
                st = su_world_zero_dead(w);
            }
            if (st != SU_OK) {
                break; /* keep i at the failing system */
            }
        }
        if (st == SU_OK && su_cmdq_overflowed(q)) {
            st = SU_E_RESOURCE_LIMIT;
        }
        if (st != SU_OK) {
            su_world_abort(w);
            *failed_index = i < s->count ? i : UINT32_MAX;
            goto out;
        }
        st = boundary(w, q, rt->log, next_tick);
        if (st != SU_OK) {
            su_world_abort(w);
            *failed_index = UINT32_MAX;
            goto out;
        }
        st = su_world_publish(w);
        if (st != SU_OK) {
            su_world_abort(w);
            *failed_index = UINT32_MAX;
            goto out;
        }
        *done = t + 1u;
    }
    st = SU_OK;
out:
    free(perm);
    return st;
}
