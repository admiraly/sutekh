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
} SuSchedule;

/* Test seam: called before each system of a tick. Returns nonzero to fail that system
 * with SU_E_CANDIDATE_FAILED. The worker passes NULL outside tests. */
typedef int (*SuSchedFault)(uint32_t system_index, void *ctx);

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

/* Swaps entry idx with *fresh (ownership of its module and context moves into the
 * schedule) and returns the replaced entry in *old for the caller to destroy. */
void su_sched_swap(SuSchedule *s, uint32_t idx, SuSysEntry *fresh, SuSysEntry *old);
/* Destroys a standalone entry's context and module. Releases its native kernel, if any. */
void su_sched_entry_destroy(SuSysEntry *e);

#endif
