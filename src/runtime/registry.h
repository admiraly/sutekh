/* Version registry of the active worker (M0-07, HOT_RELOAD_AND_STATE.md sections 2 and 9).
 *
 * Every candidate install receives a unique transaction ID (txid) when it reaches the
 * worker. Each record carries the immutable source, capsule and semantic hashes of the
 * revision it describes and moves forward through the candidate lifecycle:
 *
 *   RECEIVED -> SOURCE_VALIDATED -> EFFECTS_VERIFIED -> PREPARED -> PREVIEW_TESTED
 *            -> ADMITTED -> PENDING_BOUNDARY -> INSTALLED -> RETIRED
 *   any non-terminal state -> REJECTED | STALE | CANCELLED
 *
 * Retention is bounded: the table holds SU_REG_CAPACITY records. Active (INSTALLED)
 * records are never evicted, because the schedule refers to them. Terminal records
 * (RETIRED, REJECTED, STALE, CANCELLED) are evicted oldest-txid first when a new
 * transaction needs a slot. An evicted txid no longer resolves: su_reg_find returns
 * NULL, so no caller can hold a reference to a freed revision through this table.
 * Records store metadata and hashes only, never modules or contexts. */
#ifndef SU_RUNTIME_REGISTRY_H
#define SU_RUNTIME_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"

#define SU_REG_CAPACITY 128u
#define SU_REG_ID_MAX 32u
#define SU_REG_SYSTEM_MAX SU_DIAG_SYSTEM_BYTES
#define SU_REG_HASH_LEN 65u
#define SU_REG_REASON_MAX 160u

typedef enum SuRevState {
    SU_REV_RECEIVED = 0,
    SU_REV_SOURCE_VALIDATED,
    SU_REV_EFFECTS_VERIFIED,
    SU_REV_PREPARED,
    SU_REV_PREVIEW_TESTED,
    SU_REV_ADMITTED,
    SU_REV_PENDING_BOUNDARY,
    SU_REV_INSTALLED,
    SU_REV_RETIRED,
    SU_REV_REJECTED,
    SU_REV_STALE,
    SU_REV_CANCELLED
} SuRevState;

const char *su_rev_state_name(SuRevState s);
/* Nonzero for RETIRED, REJECTED, STALE and CANCELLED (evictable). */
int su_rev_state_terminal(SuRevState s);

typedef struct SuRevRecord {
    int used;
    uint64_t txid;           /* unique, monotonic, never reused in this worker */
    uint64_t system_rev;     /* per-system revision number assigned at install (0 if none) */
    uint64_t effective_tick; /* tick of the first simulation that runs the revision */
    SuRevState state;
    uint32_t code;           /* SU_E_* reason for REJECTED/STALE/CANCELLED; SU_OK otherwise */
    char world_id[SU_REG_ID_MAX];
    char candidate_id[SU_REG_ID_MAX];
    char system_id[SU_REG_SYSTEM_MAX];
    char source_hash[SU_REG_HASH_LEN];
    char capsule_hash[SU_REG_HASH_LEN];
    char semantic_hash[SU_REG_HASH_LEN];
    char reason[SU_REG_REASON_MAX];
} SuRevRecord;

typedef struct SuRegistry {
    SuRevRecord recs[SU_REG_CAPACITY];
    uint64_t next_txid;
    uint64_t evictions;      /* terminal records dropped to make room */
} SuRegistry;

void su_reg_init(SuRegistry *r);

/* Starts a transaction for a candidate received for world_id. Returns the new txid,
 * or 0 when no slot is free and no terminal record can be evicted. */
uint64_t su_reg_begin(SuRegistry *r, const char *world_id, const char *candidate_id,
                      const char *source_hash, const char *capsule_hash);
/* Forward pipeline transitions only (a later stage than the current one), or entry to
 * a terminal state from any non-terminal state. Returns 0, or -1 when txid is unknown
 * or the transition is not allowed. */
int su_reg_advance(SuRegistry *r, uint64_t txid, SuRevState to);
/* Records the system a transaction concerns. */
void su_reg_set_system(SuRegistry *r, uint64_t txid, const char *system_id);
/* Enters a terminal state with a reason code and message (truncated). */
void su_reg_finish(SuRegistry *r, uint64_t txid, SuRevState terminal, uint32_t code,
                   const char *reason);
/* Marks an INSTALLED record with its effective tick, system revision and semantic hash. */
int su_reg_install(SuRegistry *r, uint64_t txid, uint64_t system_rev, uint64_t effective_tick,
                   const char *semantic_hash);
/* INSTALLED -> RETIRED once a later revision replaces it. Returns -1 otherwise. */
int su_reg_retire(SuRegistry *r, uint64_t txid);
/* NULL when the txid is unknown or was evicted. */
SuRevRecord *su_reg_find(SuRegistry *r, uint64_t txid);

/* Frees every record bound to world_id (world.close). Freed txids no longer resolve,
 * and the counter that issues txids is not reset, so a txid is never reused. Returns the
 * number of records freed. */
uint32_t su_reg_release_world(SuRegistry *r, const char *world_id);

/* Counts records in use (any state) and the number of INSTALLED records. */
void su_reg_stats(const SuRegistry *r, uint32_t *in_use, uint32_t *installed);

#endif
