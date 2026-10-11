/* Chunked execution of one verified system over disjoint row ranges (M1-04).
 *
 * A u32_mod_v1 system has no cross-row effects: row r reads only row r of its
 * working columns and stores only row r of its staging columns. A table can
 * therefore be split into disjoint contiguous row chunks and each chunk run by
 * the reference VM (su_vm_invoke_v1) over SLICED column views: every binding
 * data pointer is advanced by the chunk's begin row and row_count is the chunk
 * length. The ABI is unchanged; each slice satisfies the wrapper checks of
 * M0_CONTRACT.md section 3 on its own.
 *
 * Determinism rules:
 *   - Chunk boundaries depend only on row_count and the chunk size. They never
 *     depend on the worker count, so results are canonical for any N.
 *   - Chunks are not cancelled on failure. Every chunk runs, so the reported
 *     failure (the LOWEST failing chunk index) is independent of completion
 *     order.
 *   - All-or-nothing: staging is copied to working only when every chunk
 *     returned SU_OK. On failure the committed and working roots are untouched.
 *
 * Staging memory is written by several workers at once, but each chunk writes a
 * disjoint byte range, and no worker reads staging. The caller still owns the
 * tick protocol (su_world.h): begin, run, publish on success, abort on failure.
 *
 * Status codes used: SU_E_WORKER_FAILED (thread creation), SU_E_INVALID_ARGUMENT
 * (bad arguments, not in a tick, bad permutation), SU_E_SCHEMA_MISMATCH (module
 * fields absent from the world, layout generation mismatch), SU_E_RESOURCE_LIMIT
 * (row_count above the module row limit, allocation failure). Chunk-level
 * statuses are the VM's own statuses. */
#ifndef SU_RUNTIME_JOBS_SU_JOBS_H
#define SU_RUNTIME_JOBS_SU_JOBS_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"
#include "sutekh/status.h"
#include "world/su_world.h"

#define SU_JOBS_MAX_WORKERS 64u
/* Auto chunking: about SU_JOBS_TARGET_CHUNKS chunks, never below
 * SU_JOBS_MIN_CHUNK_ROWS rows per chunk. */
#define SU_JOBS_MIN_CHUNK_ROWS 1024u
#define SU_JOBS_TARGET_CHUNKS 64u

/* ---- deterministic partition ------------------------------------------ */

/* Effective chunk size: chunk_rows when nonzero, otherwise
 * max(SU_JOBS_MIN_CHUNK_ROWS, ceil(rows / SU_JOBS_TARGET_CHUNKS)). */
uint64_t su_jobs_effective_chunk_rows(uint64_t rows, uint64_t chunk_rows);
/* Number of chunks: 0 when rows == 0, otherwise ceil(rows / effective size). */
uint64_t su_jobs_chunk_count(uint64_t rows, uint64_t chunk_rows);
/* Chunk index -> [begin, begin + len). Chunk i covers rows
 * [i * eff, min((i + 1) * eff, rows)). SU_E_INVALID_ARGUMENT when index is out
 * of range or an output pointer is NULL. */
uint32_t su_jobs_chunk_bounds(uint64_t rows, uint64_t chunk_rows, uint64_t index, uint64_t *begin,
                              uint64_t *len);

/* Fills perm (n entries) with a permutation of 0..n-1 derived from seed
 * (splitmix64, Fisher-Yates). Same seed and n give the same permutation. */
void su_jobs_seeded_permutation(uint32_t n, uint64_t seed, uint32_t *perm);

/* ---- worker pool -------------------------------------------------------- */

typedef struct SuJobPool SuJobPool;

/* Task callback. worker is in 0..workers-1 and identifies the thread running
 * the task, so the callback can use per-worker scratch state. */
typedef void (*SuJobFn)(uint32_t task, uint32_t worker, void *arg);

/* Creates a pool of `workers` workers (1..SU_JOBS_MAX_WORKERS). With one worker,
 * tasks run inline on the caller's thread and no threads are created. Otherwise
 * `workers` POSIX threads are created. SU_E_INVALID_ARGUMENT for a bad count,
 * SU_E_WORKER_FAILED when a thread cannot be created (nothing is retained). */
uint32_t su_jobs_pool_create(uint32_t workers, SuJobPool **out);
/* NULL-safe. Joins all threads. Must not be called while a batch is running. */
void su_jobs_pool_destroy(SuJobPool *p);
uint32_t su_jobs_pool_workers(const SuJobPool *p);

/* Runs fn(task, worker, arg) for every task in 0..tasks-1 and returns when all
 * have finished. One caller at a time; not reentrant from inside a task.
 *
 * order == NULL: tasks are claimed in index order and completion is unordered.
 * order != NULL: order must be a permutation of 0..tasks-1. Tasks are claimed in
 *   order[] and their completions are gated so they are recorded in exactly the
 *   order[] sequence. This is the completion-order test hook: the bodies still
 *   run concurrently, but the physical completion sequence is forced.
 * log: optional. Receives the completion sequence: log[k] is the task whose
 *   completion was k-th. With order != NULL the gate makes this equal order[];
 *   without order it shows the physical completion order. Must hold tasks entries.
 *
 * SU_E_INVALID_ARGUMENT for NULL fn, a non-permutation order, or a reentrant
 * call. */
uint32_t su_jobs_pool_run(SuJobPool *p, uint32_t tasks, SuJobFn fn, void *arg, const uint32_t *order,
                          uint32_t *log);

/* ---- chunked system execution ------------------------------------------ */

/* Test seam: called on the worker before a chunk runs. Nonzero fails that chunk
 * with SU_E_CANDIDATE_FAILED without running it. NULL outside tests. */
typedef int (*SuJobsFault)(uint32_t chunk, void *ctx);

typedef struct SuJobsOptions {
    uint32_t workers;         /* 1..SU_JOBS_MAX_WORKERS */
    uint64_t chunk_rows;      /* 0 = auto (see su_jobs_effective_chunk_rows) */
    int seeded_completion;    /* nonzero: force completion order from completion_seed */
    uint64_t completion_seed;
    SuJobsFault fault;        /* test seam, may be NULL */
    void *fault_ctx;
} SuJobsOptions;

typedef struct SuJobsReport {
    uint64_t chunk_count;
    uint32_t *chunk_status;   /* owned, chunk_count entries; SU_OK per successful chunk */
    uint32_t *completion_log; /* owned, only when seeded_completion; chunk_count entries */
    uint32_t failed_chunk;    /* lowest failing chunk, UINT32_MAX when none failed */
    uint32_t status;          /* SU_OK, or the status of failed_chunk */
} SuJobsReport;

/* Runs the system over the world's open tick in chunks. Preconditions: the
 * world is in a tick; mod is verified; layout_generation equals the world's.
 *
 * On SU_OK every chunk succeeded and the full staging is committed to working for
 * the module's writable fields (su_world_commit_staging). On failure nothing is
 * committed; the caller aborts the tick.
 *
 * rep is zeroed by the call and filled on every path. Free it with
 * su_jobs_report_free. Returns rep->status. */
uint32_t su_jobs_run_system_chunked(SuWorld *w, const SuLirModule *mod, uint32_t layout_generation,
                                    const SuJobsOptions *opt, SuJobsReport *rep);
void su_jobs_report_free(SuJobsReport *rep);

#endif
