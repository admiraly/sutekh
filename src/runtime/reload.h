/* Candidate installation into the active worker (M0-07, HOT_RELOAD_AND_STATE.md 2-3).
 *
 * su_reload_install is the whole code-only transaction, executed inside one worker
 * request (so it runs between ticks and never inside a tick):
 *   1 begin a registry transaction (RECEIVED)
 *   2 recheck the expected world revision                       -> STALE on mismatch
 *   3 read and verify the capsule and source; check the hashes the
 *     supervisor recorded at submission (bytes unchanged)        -> REJECTED
 *   4 the candidate's system must already be in the world's schedule
 *   5 layout compatibility with the world (schema, generation, fields) -> REJECTED
 *   6 prepare: new heap module and VM context (PREPARED)
 *   7 evidence gate: the supervisor's evaluation must have passed (PREVIEW_TESTED)
 *   8 admission: revision and layout generation rechecked (ADMITTED)
 *   9 boundary: the world tick must equal allowed_effective_tick (PENDING_BOUNDARY)
 *  10 publish: swap the schedule entry, retire the old revision, free the old module
 *     and context (INSTALLED). World state and the worker process are untouched.
 * Any failure before step 10 leaves the schedule, the committed state and the active
 * module unchanged. The critical section is steps 10's registry update and schedule swap:
 * it starts before the old schedule entry is replaced and ends when the new entry is in
 * place (retiring and freeing the old entry is after the section). */
#ifndef SU_RUNTIME_RELOAD_H
#define SU_RUNTIME_RELOAD_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"
#include "runtime/registry.h"
#include "runtime/schedule.h"

/* Reads and verifies a capsule and its source. Expected hashes (NULL to skip) must
 * match the captured bytes. On SU_OK *out is a heap module (caller frees with
 * su_reload_module_free). On failure msg receives the reason. */
uint32_t su_reload_load(const char *capsule_path, const char *source_path,
                        const char *expect_source_hash, const char *expect_capsule_hash,
                        SuLirModule **out, uint32_t *schema_version, uint32_t *layout_generation,
                        char *msg, size_t msg_cap);
void su_reload_module_free(SuLirModule *m);

typedef struct SuReloadRequest {
    const char *world_id;
    const char *candidate_id;
    const char *capsule_path;
    const char *source_path;
    const char *expect_source_hash;
    const char *expect_capsule_hash;
    const char *evidence;          /* passed | failed | not_run | worker_failed */
    const char *expected_revision; /* world_revision the client based the install on */
    const char *current_revision;  /* world_revision now */
    uint64_t current_tick;
    uint64_t allowed_effective_tick;
} SuReloadRequest;

typedef struct SuReloadResult {
    uint64_t txid;             /* 0 only when the registry had no free slot */
    uint64_t previous_txid;
    uint64_t system_rev;
    uint64_t effective_tick;
    char system_id[SU_DIAG_SYSTEM_BYTES];
    char source_hash[65];
    char capsule_hash[65];
    char previous_semantic_hash[65];
    char semantic_hash[65];
    char msg[256];             /* "txid=N state=S: reason" on failure */
    /* Timings on the monotonic clock (su_platform.h), set only when the install succeeded.
     * clock_ok is 0 when the clock was unreadable; the timings are then meaningless. */
    int clock_ok;
    int bindings_prepared;     /* 1 when the VM context (bindings) existed before publish */
    uint64_t prepare_ns;       /* install entry -> start of the critical section */
    uint64_t critical_section_ns; /* stop of the old schedule -> new schedule published */
} SuReloadResult;

/* Returns SU_OK when the candidate is installed (out is filled), otherwise the SU_E_*
 * code of the first failing step (out->msg holds the reason; the registry records the
 * terminal state). The schedule is changed only on SU_OK. */
uint32_t su_reload_install(SuRegistry *reg, SuSchedule *s, const SuReloadRequest *req,
                           SuReloadResult *out);

#endif
