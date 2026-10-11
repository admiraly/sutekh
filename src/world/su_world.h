/* Sutekh M0 fixed-row SoA u32 world (M0-03).
 *
 * One table, fixed row count, stable entity IDs, u32 columns. Each field has
 * a committed root (last accepted full tick), a working root (state being
 * computed for the next tick) and, for writable fields, a staging column that
 * a system's output is written into. All storage is allocated at creation.
 *
 * Tick protocol (M0_CONTRACT.md section 4, T09):
 *   su_world_begin_tick    copy committed -> working
 *   per system:
 *     su_world_build_invocation  views over working (reads)
 *     su_world_build_outputs     views over staging (writes)
 *     on SU_OK: su_world_commit_staging(ids, n) copies staging -> working
 *     on failure: discard (no commit)
 *   su_world_publish       on whole-tick success: working -> committed, tick++
 *   su_world_abort         on any failure: discard working; committed untouched
 *
 * Views returned by build_* point into world-owned binding arrays and column
 * memory. They are valid until the next call that changes the world or its
 * phase, and must not be retained (ABI section 3). Pointers in views are
 * borrowed; the world owns all memory. */
#ifndef SUTEKH_WORLD_SU_WORLD_H
#define SUTEKH_WORLD_SU_WORLD_H

#include <stddef.h>
#include <stdint.h>
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"

/* Implementation cap for schema IDs and field names (bytes, excluding NUL). */
#define SU_WORLD_NAME_MAX 255u

typedef struct SuWorld SuWorld;

typedef struct SuWorldFieldDesc {
    uint32_t id;          /* stable field ID; unique within the schema */
    uint32_t type;        /* must be SU_TYPE_U32 in M0 */
    uint32_t access;      /* nonzero subset of SU_ACCESS_READ | SU_ACCESS_WRITE */
    const char *name;     /* NUL-terminated, 1..SU_WORLD_NAME_MAX bytes; unique */
} SuWorldFieldDesc;

typedef struct SuWorldDesc {
    const char *schema_id;        /* NUL-terminated, 1..SU_WORLD_NAME_MAX bytes */
    uint32_t schema_version;
    uint32_t layout_generation;
    uint64_t row_count;           /* <= SU_LIMIT_ROWS */
    const uint32_t *entity_ids;   /* row_count entries; nonzero and unique; may be NULL when row_count == 0 */
    const SuWorldFieldDesc *fields; /* any order; sorted internally by id */
    uint32_t field_count;         /* <= SU_LIMIT_FIELDS */
} SuWorldDesc;

/* Creation. On failure *out is NULL and nothing is retained.
 * SU_E_INVALID_ARGUMENT: NULL pointer, empty/invalid access, zero or duplicate
 *   entity ID.
 * SU_E_TYPE: field type other than SU_TYPE_U32.
 * SU_E_SCHEMA_MISMATCH: duplicate field ID or name.
 * SU_E_RESOURCE_LIMIT: row_count or field_count over the SU_LIMIT_* caps, a
 *   name over SU_WORLD_NAME_MAX, any size overflow, or allocation failure. */
uint32_t su_world_create(const SuWorldDesc *desc, SuWorld **out);
/* NULL-safe. Frees all storage. */
void su_world_destroy(SuWorld *w);

uint64_t su_world_row_count(const SuWorld *w);
uint32_t su_world_field_count(const SuWorld *w);
uint32_t su_world_layout_generation(const SuWorld *w);
uint64_t su_world_tick(const SuWorld *w);
/* Nonzero while a tick is open (between begin_tick and publish/abort). */
int su_world_in_tick(const SuWorld *w);

/* Tick control. Each returns SU_E_INVALID_ARGUMENT on NULL or wrong phase
 * and leaves the committed root untouched. */
uint32_t su_world_begin_tick(SuWorld *w);
/* Copies staging -> working for the listed writable fields. Validates the
 * whole list before copying anything. count == 0 is a no-op.
 * SU_E_INVALID_ARGUMENT: NULL list with count > 0, count > field_count,
 *   duplicate IDs, or not in a tick.
 * SU_E_SCHEMA_MISMATCH: ID not in the schema.
 * SU_E_UNDECLARED_WRITE: field is not writable. */
uint32_t su_world_commit_staging(SuWorld *w, const uint32_t *field_ids, uint32_t count);
/* Working -> committed for every field; tick++. Only call after the whole tick
 * succeeded. */
uint32_t su_world_publish(SuWorld *w);
/* Discards the open tick. Committed state is not modified. Outside a tick this
 * is a no-op returning SU_OK. */
uint32_t su_world_abort(SuWorld *w);

/* Views for one system, valid only during a tick. Reads are working columns for
 * READ fields (ascending field ID); writes are staging columns for WRITE fields
 * (ascending field ID). SuOutputs.failed_insn is set to UINT32_MAX. */
uint32_t su_world_build_invocation(SuWorld *w, SuInvocation *out);
uint32_t su_world_build_outputs(SuWorld *w, SuOutputs *out);

/* Fixture initialization: replaces committed column values. Only allowed
 * outside a tick at tick 0. values must hold row_count entries.
 * SU_E_INVALID_ARGUMENT: NULL values, not allowed phase/tick, count mismatch.
 * SU_E_SCHEMA_MISMATCH: unknown field ID. */
uint32_t su_world_init_committed(SuWorld *w, uint32_t field_id, const uint32_t *values, uint64_t count);

/* Inspection of the committed root (what external readers see). The pointer is
 * valid until the world is destroyed or published over.
 * SU_E_SCHEMA_MISMATCH: unknown field ID or name. */
uint32_t su_world_read_committed(const SuWorld *w, uint32_t field_id, const uint32_t **data, uint64_t *row_count);
uint32_t su_world_read_committed_by_name(const SuWorld *w, const char *name, const uint32_t **data, uint64_t *row_count);

/* Canonical state encoding of the committed root (no JSON, no names, no
 * padding). Layout, all integers little-endian:
 *   "SUWSTATE" 0x00 | u32 encoding_version (1) | u64 tick | u64 row_count |
 *   u32 field_count | u32 entity_ids[row_count] |
 *   per field in ascending field_id: u32 field_id | u32 elem_type |
 *                                    u32 values[row_count]
 * su_world_canonical_size reports the byte size; su_world_canonical_write
 * writes into buf if cap >= size and returns SU_E_INVALID_ARGUMENT otherwise
 * (written is set to the required size). */
uint32_t su_world_canonical_size(const SuWorld *w, size_t *size);
uint32_t su_world_canonical_write(const SuWorld *w, uint8_t *buf, size_t cap, size_t *written);
/* Compares canonical encodings of two committed roots. *equal is 1 or 0. */
uint32_t su_world_state_equal(const SuWorld *a, const SuWorld *b, int *equal);

#endif
