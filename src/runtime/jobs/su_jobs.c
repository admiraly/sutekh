/* Chunked system execution and worker pool (M1-04). See su_jobs.h. */
#define _POSIX_C_SOURCE 200809L

#include "runtime/jobs/su_jobs.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "vm/su_vm.h"

/* ---- partition ----------------------------------------------------------- */

uint64_t su_jobs_effective_chunk_rows(uint64_t rows, uint64_t chunk_rows) {
    uint64_t target;
    if (chunk_rows != 0u) {
        return chunk_rows;
    }
    target = rows / SU_JOBS_TARGET_CHUNKS + (rows % SU_JOBS_TARGET_CHUNKS != 0u ? 1u : 0u);
    return target > SU_JOBS_MIN_CHUNK_ROWS ? target : SU_JOBS_MIN_CHUNK_ROWS;
}

uint64_t su_jobs_chunk_count(uint64_t rows, uint64_t chunk_rows) {
    uint64_t eff;
    if (rows == 0u) {
        return 0u;
    }
    eff = su_jobs_effective_chunk_rows(rows, chunk_rows);
    return rows / eff + (rows % eff != 0u ? 1u : 0u);
}

uint32_t su_jobs_chunk_bounds(uint64_t rows, uint64_t chunk_rows, uint64_t index, uint64_t *begin,
                              uint64_t *len) {
    uint64_t eff;
    uint64_t b;
    if (begin == NULL || len == NULL || index >= su_jobs_chunk_count(rows, chunk_rows)) {
        return SU_E_INVALID_ARGUMENT;
    }
    eff = su_jobs_effective_chunk_rows(rows, chunk_rows);
    b = index * eff;
    *begin = b;
    *len = rows - b < eff ? rows - b : eff;
    return SU_OK;
}

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z;
    *state += UINT64_C(0x9E3779B97F4A7C15);
    z = *state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

void su_jobs_seeded_permutation(uint32_t n, uint64_t seed, uint32_t *perm) {
    uint64_t st = seed;
    uint32_t i;
    for (i = 0; i < n; i++) {
        perm[i] = i;
    }
    for (i = n; i > 1u; i--) {
        uint32_t j = (uint32_t)(splitmix64(&st) % i);
        uint32_t t = perm[i - 1u];
        perm[i - 1u] = perm[j];
        perm[j] = t;
    }
}

/* ---- pool ---------------------------------------------------------------- */

typedef struct PoolThreadArg {
    SuJobPool *pool;
    uint32_t id;
} PoolThreadArg;

struct SuJobPool {
    uint32_t workers;
    uint32_t started;             /* threads created so far */
    pthread_t *threads;
    PoolThreadArg *targs;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int mutex_ready;
    int shutdown;
    /* Current batch, guarded by mu. */
    int open;
    uint32_t tasks;
    uint32_t next_claim;
    uint32_t completed;           /* completions recorded so far (the physical sequence index) */
    uint32_t finished;
    SuJobFn fn;
    void *arg;
    const uint32_t *order;
    uint32_t *log;
};

static void *pool_main(void *argp) {
    PoolThreadArg *ta = argp;
    SuJobPool *p = ta->pool;
    uint32_t id = ta->id;

    pthread_mutex_lock(&p->mu);
    for (;;) {
        uint32_t pos;
        uint32_t task;
        SuJobFn fn;
        void *arg;
        while (!p->shutdown && !(p->open && p->next_claim < p->tasks)) {
            pthread_cond_wait(&p->cv, &p->mu);
        }
        if (p->shutdown) {
            break;
        }
        pos = p->next_claim++;
        task = p->order != NULL ? p->order[pos] : pos;
        fn = p->fn;
        arg = p->arg;
        pthread_mutex_unlock(&p->mu);

        fn(task, id, arg);

        pthread_mutex_lock(&p->mu);
        if (p->order != NULL) {
            /* Gate: with an order, position pos may complete only after every earlier
             * position has. The lowest unfinished position is always running, so this
             * cannot deadlock. */
            while (p->completed != pos) {
                pthread_cond_wait(&p->cv, &p->mu);
            }
        }
        /* The log records the completion sequence as it happened. With an order it
         * equals the order (the gate); without one it shows the physical order. */
        if (p->log != NULL) {
            p->log[p->completed] = task;
        }
        p->completed++;
        p->finished++;
        pthread_cond_broadcast(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

uint32_t su_jobs_pool_create(uint32_t workers, SuJobPool **out) {
    SuJobPool *p;
    uint32_t i;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (workers == 0u || workers > SU_JOBS_MAX_WORKERS) {
        return SU_E_INVALID_ARGUMENT;
    }
    p = calloc(1, sizeof *p);
    if (p == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    p->workers = workers;
    if (workers == 1u) {
        *out = p;
        return SU_OK;
    }
    p->threads = calloc(workers, sizeof *p->threads);
    p->targs = calloc(workers, sizeof *p->targs);
    if (p->threads == NULL || p->targs == NULL) {
        free(p->threads);
        free(p->targs);
        free(p);
        return SU_E_RESOURCE_LIMIT;
    }
    if (pthread_mutex_init(&p->mu, NULL) != 0) {
        free(p->threads);
        free(p->targs);
        free(p);
        return SU_E_WORKER_FAILED;
    }
    p->mutex_ready = 1;
    if (pthread_cond_init(&p->cv, NULL) != 0) {
        su_jobs_pool_destroy(p);
        return SU_E_WORKER_FAILED;
    }
    for (i = 0; i < workers; i++) {
        p->targs[i].pool = p;
        p->targs[i].id = i;
        if (pthread_create(&p->threads[i], NULL, pool_main, &p->targs[i]) != 0) {
            su_jobs_pool_destroy(p);
            return SU_E_WORKER_FAILED;
        }
        p->started++;
    }
    *out = p;
    return SU_OK;
}

void su_jobs_pool_destroy(SuJobPool *p) {
    uint32_t i;
    if (p == NULL) {
        return;
    }
    if (p->mutex_ready) {
        pthread_mutex_lock(&p->mu);
        p->shutdown = 1;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
    }
    for (i = 0; i < p->started; i++) {
        pthread_join(p->threads[i], NULL);
    }
    if (p->started > 0u) {
        pthread_cond_destroy(&p->cv);
    }
    if (p->mutex_ready) {
        pthread_mutex_destroy(&p->mu);
    }
    free(p->threads);
    free(p->targs);
    free(p);
}

uint32_t su_jobs_pool_workers(const SuJobPool *p) {
    return p != NULL ? p->workers : 0u;
}

/* True when order[] is a permutation of 0..n-1. */
static int is_permutation(const uint32_t *order, uint32_t n) {
    uint8_t *seen;
    uint32_t i;
    int ok = 1;
    seen = calloc(n > 0u ? n : 1u, 1u);
    if (seen == NULL) {
        return 0;
    }
    for (i = 0; i < n && ok; i++) {
        if (order[i] >= n || seen[order[i]] != 0u) {
            ok = 0;
        } else {
            seen[order[i]] = 1u;
        }
    }
    free(seen);
    return ok;
}

uint32_t su_jobs_pool_run(SuJobPool *p, uint32_t tasks, SuJobFn fn, void *arg, const uint32_t *order,
                          uint32_t *log) {
    uint32_t i;
    if (p == NULL || fn == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (order != NULL && !is_permutation(order, tasks)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (tasks == 0u) {
        return SU_OK;
    }
    if (p->workers == 1u) {
        for (i = 0; i < tasks; i++) {
            uint32_t task = order != NULL ? order[i] : i;
            fn(task, 0u, arg);
            if (log != NULL) {
                log[i] = task;
            }
        }
        return SU_OK;
    }

    pthread_mutex_lock(&p->mu);
    if (p->open) {
        pthread_mutex_unlock(&p->mu);
        return SU_E_INVALID_ARGUMENT;
    }
    p->tasks = tasks;
    p->next_claim = 0u;
    p->completed = 0u;
    p->finished = 0u;
    p->fn = fn;
    p->arg = arg;
    p->order = order;
    p->log = log;
    p->open = 1;
    pthread_cond_broadcast(&p->cv);
    while (p->finished < p->tasks) {
        pthread_cond_wait(&p->cv, &p->mu);
    }
    p->open = 0;
    p->order = NULL;
    p->log = NULL;
    p->fn = NULL;
    p->arg = NULL;
    pthread_mutex_unlock(&p->mu);
    return SU_OK;
}

/* ---- chunked system execution ------------------------------------------- */

typedef struct ChunkJob {
    uint64_t rows;
    uint64_t chunk_rows;
    uint32_t layout_generation;
    uint32_t nin;
    uint32_t nout;
    const SuColumnBinding *base_in;   /* nin entries, whole-table views */
    const SuColumnBinding *base_out;  /* nout entries, whole-table views */
    SuExecContext **ctx;              /* one per worker */
    SuColumnBinding *scr_in;          /* workers * nin */
    SuColumnBinding *scr_out;         /* workers * nout */
    uint32_t *status;                 /* one per chunk */
    SuJobsFault fault;
    void *fault_ctx;
} ChunkJob;

static void slice_binding(const SuColumnBinding *src, SuColumnBinding *dst, uint64_t begin,
                          uint64_t len) {
    *dst = *src;
    dst->data = (uint8_t *)src->data + begin * sizeof(uint32_t);
    dst->row_count = len;
}

static void chunk_task(uint32_t chunk, uint32_t worker, void *argp) {
    ChunkJob *j = argp;
    SuColumnBinding *si = j->scr_in + (size_t)worker * j->nin;
    SuColumnBinding *so = j->scr_out + (size_t)worker * j->nout;
    SuInvocation inv;
    SuOutputs outs;
    uint64_t begin = 0;
    uint64_t len = 0;
    uint32_t i;

    if (j->fault != NULL && j->fault(chunk, j->fault_ctx) != 0) {
        j->status[chunk] = SU_E_CANDIDATE_FAILED;
        return;
    }
    /* Bounds are valid by construction: chunk < chunk count. */
    (void)su_jobs_chunk_bounds(j->rows, j->chunk_rows, chunk, &begin, &len);
    for (i = 0; i < j->nin; i++) {
        slice_binding(&j->base_in[i], &si[i], begin, len);
    }
    for (i = 0; i < j->nout; i++) {
        slice_binding(&j->base_out[i], &so[i], begin, len);
    }

    inv.struct_size = (uint32_t)sizeof(SuInvocation);
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = j->layout_generation;
    inv.binding_count = j->nin;
    inv.row_count = len;
    inv.bindings = si;

    outs.struct_size = (uint32_t)sizeof(SuOutputs);
    outs.binding_count = j->nout;
    outs.failed_insn = UINT32_MAX;
    outs.reserved = 0u;
    outs.bindings = so;

    j->status[chunk] = su_vm_invoke_v1(j->ctx[worker], &inv, &outs);
}

void su_jobs_report_free(SuJobsReport *rep) {
    if (rep == NULL) {
        return;
    }
    free(rep->chunk_status);
    free(rep->completion_log);
    rep->chunk_status = NULL;
    rep->completion_log = NULL;
    rep->chunk_count = 0u;
    rep->failed_chunk = UINT32_MAX;
    rep->status = SU_OK;
}

static uint64_t module_row_limit(const SuLirModule *m) {
    return (m->max_rows != 0u && m->max_rows < SU_LIMIT_ROWS) ? m->max_rows : SU_LIMIT_ROWS;
}

static void free_workers(SuExecContext **ctx, uint32_t workers) {
    uint32_t w;
    if (ctx == NULL) {
        return;
    }
    for (w = 0; w < workers; w++) {
        su_vm_context_destroy(ctx[w]);
    }
    free(ctx);
}

/* Finds the world binding with this field ID in a view. */
static int find_binding(const SuColumnBinding *b, uint32_t n, uint32_t id, SuColumnBinding *out) {
    uint32_t j;
    for (j = 0; j < n; j++) {
        if (b[j].field_id == id) {
            *out = b[j];
            return 1;
        }
    }
    return 0;
}

uint32_t su_jobs_run_system_chunked(SuWorld *w, const SuLirModule *mod, uint32_t layout_generation,
                                    const SuJobsOptions *opt, SuJobsReport *rep) {
    SuInvocation wi;
    SuOutputs wo;
    SuColumnBinding *base_in = NULL;
    SuColumnBinding *base_out = NULL;
    SuColumnBinding *scr_in = NULL;
    SuColumnBinding *scr_out = NULL;
    SuExecContext **ctx = NULL;
    uint32_t *write_ids = NULL;
    uint32_t *status = NULL;
    uint32_t *log = NULL;
    uint32_t *perm = NULL;
    SuJobPool *pool = NULL;
    ChunkJob job;
    uint64_t rows;
    uint64_t count;
    uint64_t chunk_rows;
    uint32_t nin = 0;
    uint32_t nout = 0;
    uint32_t i;
    uint32_t st;
    uint32_t wk = 0;
    uint32_t failed = UINT32_MAX;

    if (rep == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(rep, 0, sizeof *rep);
    rep->failed_chunk = UINT32_MAX;
    if (w == NULL || mod == NULL || opt == NULL || opt->workers == 0u ||
        opt->workers > SU_JOBS_MAX_WORKERS || !su_world_in_tick(w)) {
        rep->status = SU_E_INVALID_ARGUMENT;
        return rep->status;
    }
    rows = su_world_row_count(w);
    if (rows > module_row_limit(mod)) {
        rep->status = SU_E_RESOURCE_LIMIT;
        return rep->status;
    }
    if (layout_generation != su_world_layout_generation(w)) {
        rep->status = SU_E_SCHEMA_MISMATCH;
        return rep->status;
    }
    st = su_world_build_invocation(w, &wi);
    if (st == SU_OK) {
        st = su_world_build_outputs(w, &wo);
    }
    if (st != SU_OK) {
        rep->status = st;
        return st;
    }

    /* Select the module's declared fields from the world views, in table order
     * (same selection as su_vm_run_system). */
    base_in = calloc(mod->field_count > 0u ? mod->field_count : 1u, sizeof *base_in);
    base_out = calloc(mod->field_count > 0u ? mod->field_count : 1u, sizeof *base_out);
    write_ids = calloc(mod->field_count > 0u ? mod->field_count : 1u, sizeof *write_ids);
    if (base_in == NULL || base_out == NULL || write_ids == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    for (i = 0; i < mod->field_count; i++) {
        uint32_t acc = mod->fields[i].access;
        uint32_t id = mod->fields[i].id;
        if ((acc & SU_ACCESS_READ) != 0u) {
            if (!find_binding(wi.bindings, wi.binding_count, id, &base_in[nin])) {
                st = SU_E_SCHEMA_MISMATCH;
                goto done;
            }
            nin++;
        }
        if ((acc & SU_ACCESS_WRITE) != 0u) {
            if (!find_binding(wo.bindings, wo.binding_count, id, &base_out[nout])) {
                st = SU_E_SCHEMA_MISMATCH;
                goto done;
            }
            write_ids[nout] = id;
            nout++;
        }
    }

    chunk_rows = opt->chunk_rows;
    count = su_jobs_chunk_count(rows, chunk_rows);
    if (count > (uint64_t)SIZE_MAX / sizeof(uint32_t)) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    rep->chunk_count = count;
    status = calloc(count > 0u ? (size_t)count : 1u, sizeof *status);
    if (status == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    rep->chunk_status = status;
    if (opt->seeded_completion) {
        perm = calloc(count > 0u ? (size_t)count : 1u, sizeof *perm);
        log = calloc(count > 0u ? (size_t)count : 1u, sizeof *log);
        if (perm == NULL || log == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            goto done;
        }
        su_jobs_seeded_permutation((uint32_t)count, opt->completion_seed, perm);
        rep->completion_log = log;
    }

    wk = opt->workers;
    ctx = calloc(wk, sizeof *ctx);
    if (ctx == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    scr_in = calloc((size_t)wk * (nin > 0u ? nin : 1u), sizeof *scr_in);
    scr_out = calloc((size_t)wk * (nout > 0u ? nout : 1u), sizeof *scr_out);
    if (ctx == NULL || scr_in == NULL || scr_out == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    for (i = 0; i < wk; i++) {
        st = su_vm_context_create(mod, layout_generation, &ctx[i]);
        if (st != SU_OK) {
            goto done;
        }
    }

    st = su_jobs_pool_create(wk, &pool);
    if (st != SU_OK) {
        goto done;
    }

    memset(&job, 0, sizeof job);
    job.rows = rows;
    job.chunk_rows = chunk_rows;
    job.layout_generation = layout_generation;
    job.nin = nin;
    job.nout = nout;
    job.base_in = base_in;
    job.base_out = base_out;
    job.ctx = ctx;
    job.scr_in = scr_in;
    job.scr_out = scr_out;
    job.status = status;
    job.fault = opt->fault;
    job.fault_ctx = opt->fault_ctx;

    if (count > 0u) {
        st = su_jobs_pool_run(pool, (uint32_t)count, chunk_task, &job, perm, log);
        if (st != SU_OK) {
            goto done;
        }
    }

    /* Deterministic merge: the lowest failing chunk decides. */
    for (i = 0; i < (uint32_t)count; i++) {
        if (status[i] != SU_OK) {
            failed = i;
            break;
        }
    }
    rep->failed_chunk = failed;
    if (failed != UINT32_MAX) {
        st = status[failed];
        rep->status = st;
        goto done;
    }

    /* All chunks succeeded: commit the full staging for every writable field. */
    st = su_world_commit_staging(w, write_ids, nout);
    rep->status = st;

done:
    if (st != SU_OK && rep->status == SU_OK) {
        rep->status = st;
    }
    su_jobs_pool_destroy(pool);
    free_workers(ctx, wk);
    free(scr_in);
    free(scr_out);
    free(base_in);
    free(base_out);
    free(write_ids);
    free(perm);
    return rep->status;
}
