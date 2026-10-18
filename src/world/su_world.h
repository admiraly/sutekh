/* Sutekh fixed-capacity SoA u32 world (M0-03; entity table integrated in M2-00c1).
 *
 * One table of u32 columns with a fixed capacity, chosen at creation. Each field has a committed
 * root (last accepted full tick), a working root (state being computed for the next tick) and, for
 * writable fields, a staging column that a system's output is written into. The entity table
 * (src/world/entities/su_ents.h, contract M2_ENTITIES_CONTRACT.md) has a committed and a working
 * copy with the same discipline. All storage is allocated at creation.
 *
 * Capacity: capacity = max(row_count, desc.max_entities) (max_entities 0 means row_count). Columns and
 * the entity table are allocated for capacity rows. su_world_row_count() reports capacity. Rows
 * beyond the initial row_count start dead, with zero values.
 *
 * Tick protocol (M0_CONTRACT.md section 4, T09):
 *   su_world_begin_tick    copy committed -> working (columns and entity table)
 *   per system:
 *     su_world_build_invocation  views over working (reads)
 *     su_world_build_outputs     views over staging (writes)
 *     on SU_OK: su_world_commit_staging(ids, n) copies staging -> working
 *               su_world_zero_dead(w)  zeroes dead rows of working (runtime, after each commit)
 *     on failure: discard (no commit)
 *   su_world_publish       on whole-tick success: dead rows zeroed, working -> committed, tick++
 *   su_world_abort         on any failure: discard working; committed untouched
 *
 * Spawn, despawn and fixture initialization are control-plane mutations of the committed root and
 * are accepted only between ticks.
 *
 * Views returned by build_* point into world-owned binding arrays and column memory. They are valid
 * until the next call that changes the world or its phase, and must not be retained (ABI section 3).
 * Pointers in views are borrowed; the world owns all memory. */
#ifndef SUTEKH_WORLD_SU_WORLD_H
#define SUTEKH_WORLD_SU_WORLD_H

#include <stddef.h>
#include <stdint.h>
#include "sutekh/abi.h"
#include "sutekh/entities.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "world/entities/su_ents.h"

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
    uint64_t row_count;           /* initial alive entities, <= SU_LIMIT_ROWS */
    const uint32_t *entity_ids;   /* row_count entries; nonzero and unique; may be NULL when row_count == 0 */
    const SuWorldFieldDesc *fields; /* any order; sorted internally by id */
    uint32_t field_count;         /* <= SU_LIMIT_FIELDS */
} SuWorldDesc;

/* One field value for su_world_spawn. */
typedef struct SuWorldSpawnValue {
    uint32_t field_id;
    uint32_t value;
} SuWorldSpawnValue;

/* Creation. On failure *out is NULL and nothing is retained.
 * SU_E_INVALID_ARGUMENT: NULL pointer, empty/invalid access, zero or duplicate
 *   entity ID.
 * SU_E_TYPE: field type other than SU_TYPE_U32.
 * SU_E_SCHEMA_MISMATCH: duplicate field ID or name.
 * SU_E_RESOURCE_LIMIT: row_count, max_entities or field_count over the SU_LIMIT_* caps, a
 *   name over SU_WORLD_NAME_MAX, any size overflow, or allocation failure. */
uint32_t su_world_create(const SuWorldDesc *desc, SuWorld **out);
/* As su_world_create with a capacity request (M2-00c1). The capacity is max(row_count, max_entities);
 * max_entities 0 means row_count. Kept separate from SuWorldDesc so that existing descriptors that are
 * not zero-initialized keep their meaning. SU_E_RESOURCE_LIMIT if the capacity exceeds SU_LIMIT_ROWS.
 * Other errors as su_world_create. */
uint32_t su_world_create_ex(const SuWorldDesc *desc, uint64_t max_entities, SuWorld **out);
/* Schema extensions (M2-01a, contract M2_MIGRATION_CONTRACT.md sections 1, 6). Every field has a u32
 * default (0 unless a migration set one) that su_world_spawn writes for fields the request does not name.
 * The world owns the retired field-ID list (ascending, never shrunk within a lineage) and the lineage
 * (raw SHA-256 migration hashes, oldest first). Both are persisted by snapshot 0.3.
 * ext may be NULL (all defaults 0, no retired IDs, empty lineage). defaults has one entry per desc field in
 * desc order. SU_E_SCHEMA_MISMATCH: a retired ID is a live field, or retired IDs are not strictly ascending.
 * SU_E_RESOURCE_LIMIT: more than SU_WORLD_MAX_HISTORY retired IDs or lineage entries. Other errors as
 * su_world_create. */
#define SU_WORLD_MAX_HISTORY 4096u
typedef struct SuWorldSchemaExt {
    const uint32_t *defaults;       /* desc->field_count entries, desc order; NULL means all 0 */
    const uint32_t *retired_ids;    /* strictly ascending */
    uint32_t retired_count;
    const uint8_t (*lineage)[32];   /* migration hashes */
    uint32_t lineage_count;
} SuWorldSchemaExt;
uint32_t su_world_create_ext(const SuWorldDesc *desc, uint64_t max_entities, const SuWorldSchemaExt *ext,
                             SuWorld **out);
/* Builds the successor of old for a migration commit, aside from old: the entity table (including
 * generations, retired slots and capacity), the tick and the committed table are copied from old; the
 * columns are zero. desc describes the NEW schema (row_count 0, entity_ids NULL; desc->layout_generation is
 * the new generation). The caller fills the columns with su_world_put_committed. old is not modified.
 * SU_E_INVALID_ARGUMENT: old in a tick, desc->row_count != 0. */
uint32_t su_world_create_successor(const SuWorld *old, const SuWorldDesc *desc, const SuWorldSchemaExt *ext,
                                   SuWorld **out);
/* Writes committed (and working) values for one field: the first min(count, capacity) rows from values,
 * the rest zero, dead rows zeroed. Outside a tick only. Used by migration prepare.
 * SU_E_INVALID_ARGUMENT: NULL values with count > 0, in a tick, count > capacity. SU_E_SCHEMA_MISMATCH:
 * unknown field ID. */
uint32_t su_world_put_committed(SuWorld *w, uint32_t field_id, const uint32_t *values, uint64_t count);
/* Schema identity (fixed at creation; a migration keeps the schema ID). */
const char *su_world_schema_id(const SuWorld *w);
uint32_t su_world_schema_version(const SuWorld *w);
/* Default of a field (the value su_world_spawn writes when the request does not name it). */
uint32_t su_world_field_default(const SuWorld *w, uint32_t field_id, uint32_t *out);
/* Field table entry by index (ascending field ID). name points into the world. */
uint32_t su_world_field_at(const SuWorld *w, uint32_t index, SuWorldFieldDesc *out, uint32_t *default_value);
/* Retired IDs (ascending) and lineage (oldest first). Pointers are valid until the world is destroyed. */
uint32_t su_world_retired_ids(const SuWorld *w, const uint32_t **ids, uint32_t *count);
uint32_t su_world_lineage(const SuWorld *w, const uint8_t (**hashes)[32], uint32_t *count);
/* Sets the layout generation of an idle world (migration commit only). */
uint32_t su_world_set_layout_generation(SuWorld *w, uint32_t layout_generation);
/* Schema descriptor digest: SHA-256 of "SUWSCHEMA" 0x00 | u32 1 | u32 field_count | per field in ascending id:
 * u32 id | u32 type | u32 default | u32 name_length | name bytes, all integers little-endian. Access is not
 * covered (contract section 1). */
uint32_t su_world_schema_digest(const SuWorld *w, uint8_t out[32]);
/* Recovery digest (contract section 6): SHA-256(canonical_state || u32 layout_generation LE || schema digest),
 * where canonical_state is su_world_canonical_write of the committed root. */
uint32_t su_world_recovery_digest(const SuWorld *w, uint8_t out[32]);

/* NULL-safe. Frees all storage. */
void su_world_destroy(SuWorld *w);

/* Capacity (column length). Equals the initial row count for fixture worlds. */
uint64_t su_world_row_count(const SuWorld *w);
uint32_t su_world_field_count(const SuWorld *w);
uint32_t su_world_layout_generation(const SuWorld *w);
uint64_t su_world_tick(const SuWorld *w);
/* Nonzero while a tick is open (between begin_tick and publish/abort). */
int su_world_in_tick(const SuWorld *w);

/* Entity table accessors. The committed table is the one external readers see; the working table is
 * the one a tick computes. The pointers stay valid until the world is destroyed. NULL w gives NULL. */
const SuEnts *su_world_entities_committed(const SuWorld *w);
const SuEnts *su_world_entities_working(const SuWorld *w);

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
/* Zeroes every column of every DEAD slot in the working root. Only in a tick. Full chunks are skipped;
 * the cost is proportional to the dead slots of the other chunks. Alive rows are never touched.
 * SU_E_INVALID_ARGUMENT outside a tick. */
uint32_t su_world_zero_dead(SuWorld *w);
/* Working -> committed for every field and the entity table; dead rows are zeroed first; tick++.
 * Only call after the whole tick succeeded. */
uint32_t su_world_publish(SuWorld *w);
/* Discards the open tick. Committed state is not modified. Outside a tick this
 * is a no-op returning SU_OK. */
uint32_t su_world_abort(SuWorld *w);

/* Views for one system, valid only during a tick. Reads are working columns for
 * READ fields (ascending field ID); writes are staging columns for WRITE fields
 * (ascending field ID). SuOutputs.failed_insn is set to UINT32_MAX. */
uint32_t su_world_build_invocation(SuWorld *w, SuInvocation *out);
uint32_t su_world_build_outputs(SuWorld *w, SuOutputs *out);

/* Alive and generation views for slots [base, base+n). In a tick they read the working table, otherwise
 * the committed table. alive receives 0 or 1 per slot; generation receives the stored generation
 * (RETIRED for retired slots).
 * SU_E_INVALID_ARGUMENT: base + n > capacity, NULL buffer with n > 0, or NULL w. */
uint32_t su_world_alive_view(const SuWorld *w, uint32_t base, uint32_t n, uint8_t *alive);
uint32_t su_world_generation_view(const SuWorld *w, uint32_t base, uint32_t n, uint32_t *generation);

/* Control-plane spawn, applied to the committed root between ticks. Writes the given field values
 * (all other fields 0) into the new slot. Field values may name any field of the schema, readable or
 * writable. external_id may be 0. The new handle is written to *out.
 * SU_E_INVALID_ARGUMENT: in a tick, NULL out, count > 0 with NULL values, count > field_count, or a
 *   duplicate field ID.
 * SU_E_SCHEMA_MISMATCH: unknown field ID.
 * SU_E_RESOURCE_LIMIT: no free slot (nothing changes). */
uint32_t su_world_spawn(SuWorld *w, const SuWorldSpawnValue *values, uint32_t count,
                        uint32_t external_id, SuEntity *out);
/* Control-plane despawn between ticks. Zeroes the slot's columns in the committed root and retires
 * the slot's handle. SU_E_INVALID_ARGUMENT in a tick; SU_E_STALE_HANDLE for stale, dead, retired or
 * out-of-range handles (nothing changes). */
uint32_t su_world_despawn(SuWorld *w, SuEntity handle);

/* Fixture initialization: replaces committed column values. Only allowed
 * outside a tick at tick 0. count must be row_count (the capacity) or the initial row count; in the
 * latter case only the first count rows are written and the rest stay zero.
 * SU_E_INVALID_ARGUMENT: NULL values, not allowed phase/tick, count mismatch.
 * SU_E_SCHEMA_MISMATCH: unknown field ID. */
uint32_t su_world_init_committed(SuWorld *w, uint32_t field_id, const uint32_t *values, uint64_t count);
/* Replaces the committed entity table with a copy of t (snapshot restore). Only outside a tick at
 * tick 0, before any publish. t must have the world's capacity.
 * SU_E_INVALID_ARGUMENT: NULL, phase or tick, or capacity mismatch. Dead rows of committed columns
 * are not modified here; the caller supplies zero values for them. */
uint32_t su_world_adopt_entities(SuWorld *w, const SuEnts *t);

/* Inspection of the committed root (what external readers see). The pointer is
 * valid until the world is destroyed or published over.
 * SU_E_SCHEMA_MISMATCH: unknown field ID or name. */
uint32_t su_world_read_committed(const SuWorld *w, uint32_t field_id, const uint32_t **data, uint64_t *row_count);
uint32_t su_world_read_committed_by_name(const SuWorld *w, const char *name, const uint32_t **data, uint64_t *row_count);

/* Canonical state encoding of the committed root (no JSON, no names, no padding), version 2
 * (contract section 1). Layout, all integers little-endian:
 *   "SUWSTATE" 0x00 | u32 encoding_version (2) | u64 tick | u32 field_count |
 *   entity table (su_ents_canonical_write: "SUENTABL", capacity, high_water, alive and retired counts,
 *     free policy "lowest_slot", then generation, external id and alive byte per slot below high_water) |
 *   per field in ascending field_id: u32 field_id | u32 elem_type | u32 values[high_water]
 * Rows at or above high_water are implicitly dead, zero and generation 1. Dead rows below high_water
 * are zero by invariant.
 * su_world_canonical_size reports the byte size; su_world_canonical_write writes into buf if
 * cap >= size and returns SU_E_INVALID_ARGUMENT otherwise (written is set to the required size). */
uint32_t su_world_canonical_size(const SuWorld *w, size_t *size);
uint32_t su_world_canonical_write(const SuWorld *w, uint8_t *buf, size_t cap, size_t *written);
/* Restores a world from canonical bytes into a NEW world created from desc. Accepts version 2 (as
 * written above) and version 1 (the M0 layout: "SUWSTATE" 0x00 | u32 1 | u64 tick | u64 row_count |
 * u32 field_count | u32 entity_ids[row_count] | per field u32 id | u32 type | u32 values[row_count];
 * all slots alive with generation 1, row_count must equal desc->row_count).
 * Strict: length must match exactly, field table and entity table must agree with desc, and dead
 * rows must be zero. *out is NULL on failure.
 * SU_E_PARSE: bad magic, version, length, or entity table. SU_E_SCHEMA_MISMATCH: field or row
 * identity does not match desc. SU_E_TYPE: nonzero dead row. */
uint32_t su_world_restore_canonical(const SuWorldDesc *desc, const uint8_t *buf, size_t len, SuWorld **out);
/* In-place restore of version 2 bytes into an existing world of the same schema and capacity, with no
 * allocation (rollback), in two steps. su_world_restore_canonical_check validates every byte with the
 * checks of su_world_restore_canonical, in the same order and with the same codes (SU_E_SCHEMA_MISMATCH
 * also when the encoded capacity differs from the world's), parsing the entity table into scratch: a
 * caller-owned table of the world's capacity, distinct from the world's own. It does not change w. After
 * SU_OK, and with no change to w or scratch in between, su_world_restore_canonical_commit installs the same
 * bytes: it swaps scratch with the committed table (scratch then holds the previous one) and writes the
 * columns and the tick. It cannot fail. Only between ticks. */
uint32_t su_world_restore_canonical_check(const SuWorld *w, const uint8_t *buf, size_t len, SuEnts *scratch);
void su_world_restore_canonical_commit(SuWorld *w, const uint8_t *buf, SuEnts *scratch);
/* Compares canonical encodings of two committed roots. *equal is 1 or 0. */
uint32_t su_world_state_equal(const SuWorld *a, const SuWorld *b, int *equal);

/* Exact per-root heap accounting (M3-08a, contract 9.3, OD-10). Each byte count is the sum of the
 * sizes the world asked the allocator for and still holds (no RSS, no allocator overhead).
 * columns_*: fields * max(capacity, 1) * 4 for committed, working, and staging (writable fields only).
 * entity_table: committed and working entity tables (su_ents_memory_bytes of each).
 * metadata: the world struct, schema and field name copies, the field table, the column pointer arrays,
 *   the binding arrays, and the retired-ID and lineage arrays.
 * total: the sum of the five categories above. Event log, spool and snapshot bytes are not owned by
 *   the world root and are not included (see su_evlog_memory_bytes and the snapshot owner).
 * SU_E_INVALID_ARGUMENT: NULL w or out. */
typedef struct SuWorldMemory {
    uint64_t columns_committed;
    uint64_t columns_working;
    uint64_t columns_staging;
    uint64_t entity_table;
    uint64_t metadata;
    uint64_t total;
} SuWorldMemory;
uint32_t su_world_memory(const SuWorld *w, SuWorldMemory *out);

#endif
