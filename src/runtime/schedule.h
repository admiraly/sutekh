/* Prepared schedule of one world (M0-07): an ordered, serial list of verified systems.
 *
 * All systems of a world share one schema (query, schema_version and layout_generation)
 * and one field table. The field table is the union of the systems' field bindings:
 * a field's world access is the union of its per-system access, so a system that
 * writes a field makes that field writable for the world (staging column), and every
 * system's reads see the committed/working state written by earlier systems in the
 * same tick (su_vm_run_system commits each system's staging on success).
 *
 * Each entry owns its verified module on the heap. The VM context borrows that module,
 * so replacing an entry (code-only install) never moves a module that a context points
 * to. Entries are swapped as whole structs; the caller destroys the old one. */
#ifndef SU_RUNTIME_SCHEDULE_H
#define SU_RUNTIME_SCHEDULE_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"
#include "runtime/entity_tick.h"
#include "runtime/jobs/su_jobs.h"
#include "sutekh/abi.h"
#include "world/su_world.h"

#define SU_SCHED_MAX_SYSTEMS 8u

typedef struct SuSchedField {
    uint32_t id;
    uint32_t type;
    uint32_t access;
    char name[SU_WORLD_NAME_MAX + 1u];
} SuSchedField;

typedef struct SuSysEntry {
    char system_id[SU_DIAG_SYSTEM_BYTES];
    SuLirModule *mod;        /* owned, heap */
    SuExecContext *ctx;      /* owned; borrows mod */
    uint64_t txid;           /* registry transaction that made this module live; 0 = creation */
    uint64_t system_rev;     /* 1 at creation, +1 per installed revision */
    uint64_t installed_tick; /* effective tick of the install (0 at creation) */
    char source_hash[65];
    char capsule_hash[65];
    char semantic_hash[65];
#ifdef SU_HAVE_NATIVE
    uint64_t native_seq;     /* native kernel handle (native_versions.h); 0 = VM tier */
#endif
} SuSysEntry;

typedef struct SuSchedule {
    char schema_id[SU_WORLD_NAME_MAX + 1u];
    uint32_t schema_version;
    uint32_t layout_generation;
    uint32_t count;
    SuSysEntry sys[SU_SCHED_MAX_SYSTEMS];
    uint32_t field_count;
    SuSchedField *fields;    /* owned union table, ascending field id */
    SuEntRt *ent;            /* owned entity runtime (event log, caps); NULL until enabled (M2-00c3) */
} SuSchedule;

/* Test seam: called before each system of a tick. Returns nonzero to fail that system
 * with SU_E_CANDIDATE_FAILED. The worker passes NULL outside tests. */
typedef int (*SuSchedFault)(uint32_t system_index, void *ctx);

/* Tick execution options (M1 closure B). workers == 1 (default) runs every system serially
 * through its VM context or native kernel, exactly as before. workers > 1 runs a system whose
 * world has at least chunk_min_rows rows through su_jobs_run_system_chunked (VM tier) or
 * su_nv_run_system_chunked (native tier): the same canonical chunks on sliced column views.
 * chunk_fault is a test seam (NULL outside tests) that fails one chunk with SU_E_CANDIDATE_FAILED.
 * Tick workers stay opt-in: the default (1) is the serial path. */
typedef struct SuSchedTickOpts {
    uint32_t workers;          /* 1..SU_JOBS_MAX_WORKERS */
    uint64_t chunk_min_rows;   /* chunked execution only when rows >= this */
    SuJobsFault chunk_fault;   /* test seam, may be NULL */
    void *chunk_ctx;
    /* Test seam (M2-00 audit blocker 1): nonzero forces the chunk completion order of every entity system
     * in every tick to a seeded permutation (su_jobs_pool_run order). The merge must not depend on it. */
    int seeded_completion;
    uint64_t completion_seed;
} SuSchedTickOpts;

void su_sched_init(SuSchedule *s);
/* Destroys every context and module, and frees the field table. */
void su_sched_free(SuSchedule *s);

/* Appends a system. Takes ownership of mod on SU_OK only. Fails with
 * SU_E_SCHEMA_MISMATCH when query, schema_version or layout_generation differ from the
 * entries already present, SU_E_INVALID_ARGUMENT for a duplicate system_id, and
 * SU_E_RESOURCE_LIMIT when the schedule is full or the VM context cannot be created. */
uint32_t su_sched_add(SuSchedule *s, SuLirModule *mod, uint32_t schema_version, uint32_t gen,
                      uint64_t txid, uint64_t system_rev, uint64_t tick);
/* Builds the union field table from the current entries. Conflicting ID/name/type for
 * one field is SU_E_SCHEMA_MISMATCH. Call after the last su_sched_add. */
uint32_t su_sched_finalize(SuSchedule *s);
/* Index of the entry with this system_id, or -1. */
int su_sched_find(const SuSchedule *s, const char *system_id);

/* Checks that a verified module can replace an entry of this schedule without changing
 * the world: same query, schema_version and layout_generation, and every field the
 * module binds exists in the world table with the same ID, name and type, with access
 * contained in the world's access. Returns SU_OK or SU_E_SCHEMA_MISMATCH and writes the
 * reason into why. */
uint32_t su_sched_check_candidate(const SuSchedule *s, const SuLirModule *m, uint32_t schema_version,
                                  uint32_t gen, char *why, size_t cap);

/* Runs `ticks` complete ticks over the world: begin, every system in order, publish.
 * On failure the open tick is aborted (committed root unchanged), *failed_index is the
 * failing system (UINT32_MAX for begin/publish), and *done counts published ticks. */
uint32_t su_sched_run_ticks(SuWorld *w, SuSchedule *s, uint64_t ticks, SuSchedFault fault,
                            void *fault_ctx, uint64_t *done, uint32_t *failed_index);
/* As su_sched_run_ticks, with the tick options. opt == NULL means the serial path. The result
 * is canonical for every workers value: chunk boundaries depend only on the row count. */
uint32_t su_sched_run_ticks_opt(SuWorld *w, SuSchedule *s, uint64_t ticks, SuSchedFault fault,
                                void *fault_ctx, const SuSchedTickOpts *opt, uint64_t *done,
                                uint32_t *failed_index);

/* Enables entity state for this schedule: an event log of event_log_max records and per-tick caps.
 * Required before a schedule with uses_entity_ops systems can run (SU_E_INVALID_ARGUMENT otherwise).
 * Replaces an existing entity runtime (its log is discarded; callers enable once at creation). */
uint32_t su_sched_enable_entities(SuSchedule *s, uint64_t max_struct_tick, uint64_t max_events_tick,
                                  uint64_t event_log_max);
/* Installs an entity runtime built elsewhere (snapshot restore). Takes ownership of rt. */
uint32_t su_sched_adopt_entities(SuSchedule *s, SuEntRt *rt);
/* Runs one system of the schedule on the serial or chunked non-entity path (M0/M1 semantics). */
uint32_t su_sched_run_system(SuWorld *w, SuSchedule *s, uint32_t i, const SuSchedTickOpts *opt);

/* Swaps entry idx with *fresh (ownership of its module and context moves into the
 * schedule) and returns the replaced entry in *old for the caller to destroy. */
void su_sched_swap(SuSchedule *s, uint32_t idx, SuSysEntry *fresh, SuSysEntry *old);
/* Destroys a standalone entry's context and module. Releases its native kernel, if any. */
void su_sched_entry_destroy(SuSysEntry *e);

#endif
