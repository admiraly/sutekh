/* Schema migration engine for one world (M2-01a; contract planning/contracts/M2_MIGRATION_CONTRACT.md
 * sections 1-4, 9).
 *
 * Lifecycle, all on one world:
 *   su_migrate_plan     strict validation of the migration document and of the transform document, the
 *                       destination schema, the transform verified by the UNCHANGED su_lir_verify against a
 *                       synthesized capsule, per-system impact, and resource admission. Reads the old world only.
 *   su_migrate_prepare  builds the complete successor world aside: entity table, capacity, tick and generations
 *                       are copied; columns are filled (defaults for alive rows below high_water, copies of
 *                       unchanged columns, the transform through the reference VM); self-checks run. Reads the
 *                       old world only, so any failure leaves it bit-identical.
 *   su_migrate_swap     atomic publication at a boundary: the live pointer moves to the candidate and the old
 *                       world is handed back to the caller in *retired (the caller destroys it after the reply).
 *
 * Ownership: a plan owns its compiled state (free with su_migrate_plan_free). The candidate returned by prepare
 * is owned by the caller until swap; after a successful swap the live pointer owns it. The retired world is owned
 * by the caller. Migration inputs are borrowed for the call only; the plan keeps its own copies.
 *
 * Transform (contract section 2): the migration document's transform op names a transform_format 0.1 document
 * (inputs.transform) whose "source" is an IR 0.1 document (inputs.lir). The engine synthesizes a capsule whose
 * field table is the union of the source and destination schemas (readable = source IDs, writable = "writes"
 * IDs; a field's name is its destination name, or its source name when it is removed). Only one transform op is
 * allowed per migration. Its layout generation label is the source generation for both sides.
 *
 * Estimates (contract section 3): resource numbers are ESTIMATES from row and field counts, labelled as such in the
 * plan. They are not measurements. */
#ifndef SUTEKH_MIGRATION_SU_MIGRATE_H
#define SUTEKH_MIGRATION_SU_MIGRATE_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/status.h"
#include "world/su_world.h"

#define SU_MIGRATE_DOC_MAX_BYTES (256u * 1024u)
#define SU_MIGRATE_MAX_OPS 64u
#define SU_MIGRATE_MAX_DEPTH 16u
#define SU_MIGRATE_ID_MAX 64u
#define SU_MIGRATE_MESSAGE_MAX 240u

/* Per-system impact (contract section 3). */
#define SU_MIGRATE_IMPACT_COMPATIBLE 1u        /* compatible_after_resubmit */
#define SU_MIGRATE_IMPACT_NEEDS_RESUBMISSION 2u
#define SU_MIGRATE_IMPACT_BROKEN 3u

/* Reason bits for an impact. */
#define SU_MIGRATE_REASON_LAYOUT (UINT32_C(1) << 0)       /* layout generation changes: resubmit required */
#define SU_MIGRATE_REASON_RENAMED (UINT32_C(1) << 1)
#define SU_MIGRATE_REASON_TRANSFORMED (UINT32_C(1) << 2)
#define SU_MIGRATE_REASON_FIELD_REMOVED (UINT32_C(1) << 3)
#define SU_MIGRATE_REASON_UNKNOWN_FIELD (UINT32_C(1) << 4)

/* Why a plan is not valid (plan.reason; 0 when valid). */
#define SU_MIGRATE_REFUSED_NONE 0u
#define SU_MIGRATE_REFUSED_FIELD_IN_USE 1u
#define SU_MIGRATE_REFUSED_SPOOL_BUDGET 2u
#define SU_MIGRATE_REFUSED_MEMORY 3u

typedef struct SuMigrateInputs {
    const uint8_t *migration;  /* migration document bytes */
    size_t migration_len;
    const uint8_t *transform;  /* transform_format 0.1 document, required iff the migration has a transform op */
    size_t transform_len;
    const uint8_t *lir;        /* the transform's IR 0.1 document */
    size_t lir_len;
} SuMigrateInputs;

/* One installed system: its field table as its capsule declares it (access is READ, WRITE or both). */
typedef struct SuMigrateSystem {
    const char *system_id;
    const SuWorldFieldDesc *fields;
    uint32_t field_count;
} SuMigrateSystem;

typedef struct SuMigrateBudget {
    uint64_t spool_bytes;   /* admission: pre_migration_bytes + post_commit_bytes_bound */
    uint64_t memory_bytes;  /* admission: bytes_peak */
} SuMigrateBudget;

typedef struct SuMigrateSystemImpact {
    char system_id[256];
    uint32_t impact;        /* SU_MIGRATE_IMPACT_* */
    uint32_t reasons;       /* SU_MIGRATE_REASON_* bits */
} SuMigrateSystemImpact;

typedef struct SuMigrateCompiled SuMigrateCompiled;

typedef struct SuMigratePlan {
    uint32_t valid;                 /* 1 when admitted and no installed system is broken (and not dropped) */
    uint32_t reason;                /* SU_MIGRATE_REFUSED_* when not valid */
    char message[SU_MIGRATE_MESSAGE_MAX];
    char migration_id[SU_MIGRATE_ID_MAX + 1];
    uint32_t from_layout_generation;
    uint32_t to_layout_generation;
    uint8_t migration_hash[32];     /* SHA-256 of the migration, transform and LIR bytes (length-prefixed) */
    char migration_hash_hex[65];
    uint8_t plan_hash[32];
    char plan_hash_hex[65];
    uint32_t schema_before_fields;
    uint32_t schema_after_fields;
    uint8_t schema_before_digest[32];
    uint8_t schema_after_digest[32];
    uint32_t columns_added;
    uint32_t columns_removed;
    uint32_t columns_renamed;
    uint32_t has_transform;
    uint8_t transform_semantic_hash[32];
    uint32_t transform_writes;
    uint64_t rows;                  /* high_water of the old world */
    uint64_t capacity;
    /* ESTIMATES (bytes), see the header comment. */
    uint64_t bytes_new_columns;
    uint64_t bytes_peak;
    uint64_t pre_migration_bytes;
    uint64_t post_commit_bytes_bound;
    uint32_t system_count;
    SuMigrateSystemImpact *systems; /* owned, system_count entries, in input order */
    SuMigrateCompiled *impl;        /* owned */
} SuMigratePlan;

/* Failure injection for tests. fail_at = N makes the N-th injection point of prepare fail with
 * SU_E_RESOURCE_LIMIT (an allocation point); 0 disables. points counts the injection points passed. The
 * injection points are: each buffer allocation, the entity scratch, the transform verification, the VM context
 * and binding arrays, the successor world, and each column put. fail_transform makes the transform execution
 * fail with SU_E_CANDIDATE_FAILED. */
typedef struct SuMigrateHooks {
    uint64_t fail_at;
    uint64_t points;
    uint32_t fail_transform;
} SuMigrateHooks;

typedef struct SuMigrateTimings {
    uint64_t prepare_ns;    /* whole prepare, monotonic clock */
    uint64_t transform_ns;  /* the VM invocation alone */
} SuMigrateTimings;

/* Returns SU_OK with out filled (valid may be 0 with a reason) or a status for a rejected document:
 * SU_E_RESOURCE_LIMIT (size, depth, op count), SU_E_PARSE, SU_E_DUPLICATE_KEY, SU_E_TYPE (shape),
 * SU_E_STALE_REVISION (from generation), SU_E_SCHEMA_MISMATCH (ids, names, retired IDs),
 * SU_E_UNSUPPORTED_MIGRATION (unknown op, second transform), SU_E_CANDIDATE_FAILED (transform verification).
 * On error out keeps only its message (no compiled state, no systems). drop_systems lists systems that will be
 * dropped at commit (may be NULL with count 0). */
uint32_t su_migrate_plan(const SuWorld *w, const SuMigrateInputs *in,
                         const SuMigrateSystem *systems, uint32_t system_count,
                         const char *const *drop_systems, uint32_t drop_count,
                         const SuMigrateBudget *budget, SuMigratePlan *out);
/* NULL-safe. */
void su_migrate_plan_free(SuMigratePlan *p);

/* Builds the successor world aside. old is not modified. p must be valid and must still describe old (tick,
 * layout generation and recovery digest), else SU_E_STALE_REVISION. *out is NULL on failure. hooks and timings
 * are optional. */
uint32_t su_migrate_prepare(const SuWorld *old, const SuMigratePlan *p, SuMigrateHooks *hooks,
                            SuWorld **out, SuMigrateTimings *timings);

/* Atomic publication. Checks that *live still matches the plan (generation, tick, recovery digest) and that the
 * candidate matches the plan (generation, tick, schema digest). On success *retired = *live and *live =
 * candidate. On failure nothing changes. The caller destroys *retired after its reply. */
uint32_t su_migrate_swap(SuWorld **live, const SuMigratePlan *p, SuWorld *candidate, SuWorld **retired);

/* The checks of su_migrate_swap without the publication: SU_OK when a swap of live for candidate would succeed now.
 * The worker runs it before any side effect (a refused swap must leave no event behind). */
uint32_t su_migrate_swap_check(const SuWorld *live, const SuMigratePlan *p, const SuWorld *candidate);

#endif
