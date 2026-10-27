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
 * each chunk runs on that worker. base is the world slot of the table's local slot 0 (issue 140): the VM emits
 * world slots (slot_u32 = base + row), and the queue indexes rows by local slot, so the writer subtracts it. */
typedef struct EntEmit {
    SuCmdQ *q;
    uint32_t sys;
    uint64_t chunk;
    uint64_t base;
} EntEmit;

/* State of one entity system for one run call. Column views are rebuilt per tick (publish swaps the
 * column buffers); contexts and scratch arrays live for the call. */
typedef struct EntSys {
    const SuLirModule *mod;
    SuWorld *w;
    SuCmdQ *q;
    uint32_t sys;
    uint32_t table;             /* table this entry iterates (issue 140; 0 for a classic world) */
    uint64_t base;              /* world slot of that table's local slot 0 */
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

/* Per-call state kept between su_ent_run_ticks_tables calls (2026-10-18): the command queue, job pool, per-system
 * VM contexts (with their plans) and scratch arrays depend only on the key below, so a caller that steps one tick
 * per call (or re-simulates K ticks after a rollback) does not rebuild them every call. The key holds every
 * input of their construction; any difference rebuilds. Results do not depend on reuse: every tick resets the
 * queue, rebinds the column views and sets each chunk's entity view, exactly as a fresh call does.
 *
 * Tables (issue 140): one queue for the whole world (so a tick's records merge in schedule order across tables)
 * sized for the largest table, one entry per schedule system with its own table's world, rows, layout generation
 * and base. The key covers the table count, every table's rows and layout generation, and every entry as before. */
typedef struct EntCache {
    int valid;
    const SuSchedule *s;
    uint32_t count;
    uint32_t nw;
    uint32_t layout_generation[SU_SCHED_MAX_TABLES];
    uint64_t rows[SU_SCHED_MAX_TABLES];
    uint64_t base[SU_SCHED_MAX_TABLES];
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
    /* Host operator events of the tick (issue 172): one buffer of the sum of the host entries' declared per-tick
     * caps, filled in schedule order, and the count so far. Not world state: reset every tick. */
    SuEvent *hev;
    uint64_t hev_cap;
    uint64_t hev_n;
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
    free(c->hev);
    su_jobs_pool_destroy(c->pool);
    su_cmdq_destroy(c->q);
    memset(c, 0, sizeof *c);
}

/* ---- emitter callbacks (called by the VM on the worker running the chunk) ---- */

static int ent_emit_despawn(void *user, uint32_t slot, uint32_t seq, uint64_t expected_handle) {
    EntEmit *e = user;
    if ((uint64_t)slot < e->base) {
        return -1;
    }
    return su_cmdq_append_struct(e->q, e->sys, e->chunk, (uint32_t)((uint64_t)slot - e->base), seq,
                                 SU_STRUCT_DESPAWN, expected_handle, 0u, 0u) == SU_OK
               ? 0
               : -1;
}

static int ent_emit_event(void *user, uint32_t slot, uint32_t seq, uint32_t kind, uint32_t a, uint32_t b) {
    EntEmit *e = user;
    if ((uint64_t)slot < e->base) {
        return -1;
    }
    return su_cmdq_append_event(e->q, e->sys, e->chunk, (uint32_t)((uint64_t)slot - e->base), seq, kind, a, b) ==
                   SU_OK
               ? 0
               : -1;
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
    e->emit[worker].base = e->base;
    /* slot_u32, despawn_self and emit_event name WORLD slots (issue 140): the base of the table plus the row. A
     * single-table world's base is 0, so its slots are the row indices, as before. */
    st = su_vm_context_set_entity_view(e->ctx[worker], e->alive[worker], e->gen[worker],
                                       (uint32_t)(e->base + begin), e->tick_low, &e->vem[worker]);
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
        /* IR 0.4 cross-row reads see the whole table (entry state), never just this chunk. Their slot operands stay
         * row indices of this table (host.h, "tables": gather_* and find_eq_u32 never cross a table). */
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
         * at the boundary), never just this chunk. Its slot operand is a world slot. */
        st = su_vm_context_set_entity_table_base(e->ctx[worker], e->all_alive, e->all_gen, e->rows,
                                                 (uint32_t)e->base);
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
                               uint32_t wk, uint64_t rows, uint64_t base, uint32_t layout_generation,
                               uint32_t *status) {
    const SuLirModule *m = s->sys[i].mod;
    uint32_t k;
    uint32_t st = SU_E_RESOURCE_LIMIT;
    size_t nf = m->field_count > 0u ? m->field_count : 1u;

    memset(e, 0, sizeof *e);
    e->mod = m;
    e->w = w;
    e->q = q;
    e->sys = i;
    e->table = s->sys[i].table;
    e->base = base;
    e->rows = rows;
    /* The system's own table's layout generation (issue 140): a table's systems are verified against that
     * table's schema, which may have a different generation from table 0's. */
    e->layout_generation = layout_generation;
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
        st = su_vm_context_create(m, layout_generation, &e->ctx[k]);
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

/* One table's despawn target (issue 140): the working entity table and the world slot of its local slot 0, so a
 * world handle from an emitter becomes the table-local handle the entity table knows. */
typedef struct EntDespawn {
    SuEnts *ents;
    uint64_t base;
} EntDespawn;

static uint32_t despawn_cb(void *ctx, uint64_t handle, uint32_t *old_gen) {
    EntDespawn *d = ctx;
    uint32_t slot = SU_ENTITY_SLOT(handle);
    uint32_t st;
    if ((uint64_t)slot < d->base) {
        return SU_E_STALE_HANDLE;
    }
    st = su_ents_despawn(d->ents, SU_ENTITY_MAKE(slot - (uint32_t)d->base, SU_ENTITY_GEN(handle)));
    if (st == SU_OK && old_gen != NULL) {
        *old_gen = SU_ENTITY_GEN(handle);
    }
    return st;
}

/* Copies one host entry's records into the tick's host event buffer (issue 172), numbering them by system so the
 * commit can interleave them with the IR systems' events. The records carry the world slot the operator named
 * (issue 140); the engine checks it against the world's slots at creation of the buffer's cap, never here. */
static uint32_t cache_host_events(EntCache *c, uint32_t sys, const SuHostOpEvents *ev) {
    uint64_t k;
    if (ev->count == 0u) {
        return SU_OK;
    }
    if (c->hev == NULL || (uint64_t)ev->count > c->hev_cap - c->hev_n) {
        return SU_E_INTERNAL;
    }
    for (k = 0u; k < ev->count; k++) {
        c->hev[c->hev_n + k] = ev->events[k];
        c->hev[c->hev_n + k].system_index = sys;
    }
    c->hev_n += ev->count;
    return SU_OK;
}

/* Merge, apply to each table's working entity table, log, zero the despawned columns. The caller publishes. */
static uint32_t boundary(SuWorld *const *ws, uint32_t nw, const int *begun, const SuSchedule *s, SuCmdQ *q,
                         SuEventLog *log, const EntCache *c, uint64_t tick) {
    SuCmdCallbacks cb[SU_SCHED_MAX_TABLES];
    EntDespawn ctx[SU_SCHED_MAX_TABLES];
    uint32_t sys_table[SU_SCHED_MAX_SYSTEMS];
    uint64_t base[SU_SCHED_MAX_TABLES];
    SuCmdTables t;
    SuCommitReport rep;
    uint32_t st;
    uint32_t i;

    memset(cb, 0, sizeof cb);
    memset(&t, 0, sizeof t);
    for (i = 0; i < nw; i++) {
        /* The working table belongs to the world and is mutated only inside the open tick, which the caller
         * guarantees. The cast drops const only for this boundary call. */
        ctx[i].ents = (SuEnts *)su_world_entities_working(ws[i]);
        ctx[i].base = c->base[i];
        if (ctx[i].ents == NULL) {
            return SU_E_INTERNAL;
        }
        cb[i].ctx = &ctx[i];
        cb[i].despawn = despawn_cb;
        base[i] = c->base[i];
    }
    for (i = 0; i < s->count; i++) {
        sys_table[i] = s->sys[i].table;
    }
    t.cb = cb;
    t.sys_table = sys_table;
    t.base = base;
    t.table_count = nw;
    memset(&rep, 0, sizeof rep);
    st = su_cmdq_commit_tables(q, &t, tick, log, c->hev, c->hev_n, &rep);
    if (st != SU_OK) {
        return st;
    }
    for (i = 0; i < nw; i++) {
        if (begun[i]) {
            st = su_world_zero_dead(ws[i]);
            if (st != SU_OK) {
                return st;
            }
        }
    }
    return SU_OK;
}

/* ---- tick loop ---- */

static uint64_t max_chunks(const uint64_t *rows, uint32_t nw) {
    uint64_t chunks = 0u;
    uint32_t i;
    for (i = 0; i < nw; i++) {
        uint64_t n = (rows[i] + SU_CHUNK_SLOTS - 1u) / SU_CHUNK_SLOTS;
        chunks = n > chunks ? n : chunks;
    }
    return chunks;
}

uint32_t su_ent_run_ticks_tables(SuWorld *const *ws, uint32_t nw, SuSchedule *s, SuEntRt *rt, uint64_t ticks,
                                 SuSchedFault fault, void *fault_ctx, const SuSchedTickOpts *opt, uint64_t *done,
                                 uint32_t *failed_index) {
    SuCmdQ *q = NULL;
    SuJobPool *pool = NULL;
    EntSys *es = NULL;
    EntCache *c = NULL;
    int hit;
    uint32_t *perm = NULL; /* test seam: forced chunk completion order (opt->seeded_completion) */
    uint64_t rows[SU_SCHED_MAX_TABLES];
    uint64_t base[SU_SCHED_MAX_TABLES];
    uint32_t gen[SU_SCHED_MAX_TABLES];
    int begun[SU_SCHED_MAX_TABLES];
    int has[SU_SCHED_MAX_TABLES];
    uint64_t chunks;
    uint64_t t;
    uint32_t ps[SU_SCHED_MAX_SYSTEMS];
    uint32_t pe[SU_SCHED_MAX_SYSTEMS];
    uint32_t wk = 1u;
    uint32_t i;
    uint32_t j;
    uint32_t st = SU_OK;

    if (done == NULL || failed_index == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *done = 0u;
    *failed_index = UINT32_MAX;
    if (ws == NULL || s == NULL || rt == NULL || nw == 0u || nw > SU_SCHED_MAX_TABLES ||
        nw < su_sched_table_count(s) || (ticks > 0u && s->count == 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (opt != NULL) {
        wk = opt->workers;
    }
    if (wk == 0u || wk > SU_JOBS_MAX_WORKERS) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(has, 0, sizeof has);
    for (i = 0; i < s->count; i++) {
        if (s->sys[i].table >= nw) {
            return SU_E_INVALID_ARGUMENT;
        }
        has[s->sys[i].table] = 1;
    }
    for (i = 0; i < nw; i++) {
        if (ws[i] == NULL) {
            return SU_E_INVALID_ARGUMENT;
        }
        rows[i] = su_world_row_count(ws[i]);
        base[i] = 0u;
        gen[i] = su_world_layout_generation(ws[i]);
    }
    for (i = 1; i < nw; i++) {
        if (rows[i - 1u] > SU_LIMIT_ROWS - base[i - 1u]) {
            return SU_E_INVALID_ARGUMENT;
        }
        base[i] = base[i - 1u] + rows[i - 1u];
    }
    chunks = max_chunks(rows, nw);
    if (ticks == 0u) {
        return SU_OK;
    }
    sys_row_caps(s, ps, pe);
    c = &rt->cache;
    hit = c->valid && c->s == s && c->count == s->count && c->nw == nw && c->wk == wk &&
          c->vm_tier == su_vm_default_tier();
    for (i = 0; hit && i < nw; i++) {
        hit = c->rows[i] == rows[i] && c->layout_generation[i] == gen[i] && c->base[i] == base[i];
    }
    for (i = 0; hit && i < s->count; i++) {
        const SuSysEntry *e = &s->sys[i];
        hit = c->mods[i] == e->mod && memcmp(c->sem[i], e->mod->semantic_hash, 32u) == 0 &&
              c->rev[i] == e->system_rev && c->txid[i] == e->txid && c->ps[i] == ps[i] && c->pe[i] == pe[i] &&
              c->tier[i] == e->vm_tier;
    }
    if (!hit) {
        uint64_t qrows = 0u;
        for (i = 0; i < nw; i++) {
            qrows = rows[i] > qrows ? rows[i] : qrows;
        }
        ent_cache_drop(c);
        st = su_cmdq_create_ex_checked(s->count, qrows, SU_CHUNK_SLOTS,
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
        if (st == SU_OK) {
            /* One buffer for every host entry's declared events (issue 172); at most SU_SCHED_MAX_SYSTEMS entries
             * of at most SU_CMDQ_MAX_TICK_CAP each, so the sum cannot wrap. */
            uint64_t cap = 0u;
            for (i = 0; i < s->count; i++) {
                cap += s->sys[i].host_event_cap;
            }
            if (cap > 0u) {
                c->hev = calloc((size_t)cap, sizeof *c->hev);
                if (c->hev == NULL) {
                    st = SU_E_RESOURCE_LIMIT;
                } else {
                    c->hev_cap = cap;
                }
            }
        }
        c->count = s->count;
        c->nw = nw;
        for (i = 0; i < nw; i++) {
            c->rows[i] = rows[i];
            c->base[i] = base[i];
            c->layout_generation[i] = gen[i];
        }
        for (i = 0; i < s->count; i++) {
            c->mods[i] = s->sys[i].mod;
        }
        for (i = 0; st == SU_OK && i < s->count; i++) {
            uint32_t table = s->sys[i].table;
            if (s->sys[i].mod->uses_entity_ops) {
                st = ent_sys_create(&c->es[i], s, i, ws[table], c->q, wk, rows[table], base[table],
                                    gen[table], c->status);
                c->has_es[i] = st == SU_OK; /* on failure ent_sys_create freed its own state */
            }
        }
        if (st != SU_OK) {
            ent_cache_drop(c);
            return st;
        }
        c->s = s;
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
            es[i].w = ws[s->sys[i].table]; /* the world may be a different object between calls (restore) */
        }
    }
    if (opt != NULL && opt->seeded_completion && chunks > 0u) {
        perm = calloc((size_t)chunks, sizeof *perm);
        if (perm == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
    }
    /* The worlds of the tick, so a host entry bound to a target table can read it (issue 140). */
    s->tick_worlds = ws;
    s->tick_nw = nw;

    for (t = 0; t < ticks; t++) {
        /* The world's tick is the tick of the first table that has a system (a table with no system is only kept
         * in step, never stepped: issue 140). */
        uint64_t tick = 0u;
        uint32_t first = UINT32_MAX;
        uint32_t tick_low;
        uint64_t next_tick;
        for (i = 0; i < nw; i++) {
            if (has[i]) {
                first = i;
                break;
            }
        }
        if (first == UINT32_MAX) {
            s->tick_worlds = NULL;
            s->tick_nw = 0u;
            free(perm);
            return SU_E_INVALID_ARGUMENT;
        }
        tick = su_world_tick(ws[first]);
        tick_low = (uint32_t)(tick + 1u);
        next_tick = tick + 1u;
        memset(begun, 0, sizeof begun);
        for (j = 0; j < nw; j++) {
            if (has[j]) {
                st = su_world_begin_tick(ws[j]);
                if (st != SU_OK) {
                    *failed_index = UINT32_MAX;
                    goto fail;
                }
                begun[j] = 1;
            }
        }
        su_cmdq_reset(q);
        c->hev_n = 0u;
        for (i = 0; i < s->count && st == SU_OK; i++) {
            uint32_t table = s->sys[i].table;
            if (fault != NULL && fault(i, fault_ctx) != 0) {
                st = SU_E_CANDIDATE_FAILED;
                break;
            }
            if (s->sys[i].mod->uses_entity_ops) {
                const uint32_t *order = NULL;
                uint64_t n = (rows[table] + SU_CHUNK_SLOTS - 1u) / SU_CHUNK_SLOTS;
                es[i].tick_low = tick_low;
                if (perm != NULL) {
                    /* A different seeded completion order for every (tick, system): the merge must not care. */
                    su_jobs_seeded_permutation((uint32_t)n, opt->completion_seed + t * 1000003u + i * 7919u,
                                               perm);
                    order = perm;
                }
                st = ent_sys_run(&es[i], pool, n, order);
            } else if (s->sys[i].host_event_cap != 0u) {
                SuHostOpEvents hev;
                st = su_sched_run_system_events(ws[table], s, i, opt, &hev);
                if (st == SU_OK) {
                    st = cache_host_events(c, i, &hev);
                }
            } else {
                st = su_sched_run_system(ws[table], s, i, opt);
            }
            if (st == SU_OK) {
                /* Dead rows are canonically zero after every system's commit (contract B1), per table. */
                st = su_world_zero_dead(ws[table]);
            }
            if (st != SU_OK) {
                break; /* keep i at the failing system */
            }
        }
        if (st == SU_OK && su_cmdq_overflowed(q)) {
            st = SU_E_RESOURCE_LIMIT;
        }
        if (st != SU_OK) {
            *failed_index = i < s->count ? i : UINT32_MAX;
            goto fail;
        }
        st = boundary(ws, nw, begun, s, q, rt->log, c, next_tick);
        if (st != SU_OK) {
            *failed_index = UINT32_MAX;
            goto fail;
        }
        for (j = 0; j < nw; j++) {
            if (begun[j]) {
                st = su_world_publish(ws[j]);
                if (st != SU_OK) {
                    *failed_index = UINT32_MAX;
                    goto fail; /* the failing table is still open: fail aborts every begun table */
                }
                begun[j] = 0;
            }
        }
        /* A table with no scheduled system was not begun or published; it keeps the world's one tick. */
        for (j = 0; j < nw; j++) {
            if (!has[j]) {
                (void)su_world_set_tick(ws[j], next_tick);
            }
        }
        *done = t + 1u;
    }
    st = SU_OK;
    s->tick_worlds = NULL;
    s->tick_nw = 0u;
    free(perm);
    return st;

fail:
    for (j = 0; j < nw; j++) {
        if (begun[j]) {
            (void)su_world_abort(ws[j]);
        }
    }
    s->tick_worlds = NULL;
    s->tick_nw = 0u;
    free(perm);
    return st;
}

uint32_t su_ent_run_ticks(SuWorld *w, SuSchedule *s, SuEntRt *rt, uint64_t ticks, SuSchedFault fault,
                          void *fault_ctx, const SuSchedTickOpts *opt, uint64_t *done,
                          uint32_t *failed_index) {
    return su_ent_run_ticks_tables(&w, 1u, s, rt, ticks, fault, fault_ctx, opt, done, failed_index);
}
