/* Canonical JSON snapshots of a committed world (M0_CONTRACT.md section 6, M0-08; format 0.2 from
 * M2-00c1, contract M2_ENTITIES_CONTRACT.md section 4).
 *
 * A snapshot is captured only at a committed tick boundary. Its bytes are the canonical encoding:
 * compact JSON (no whitespace), fixed key order, fields in ascending id order, columns keyed by field
 * name in the same order. Identity is the SHA-256 of those bytes, lowercase hex.
 *
 * Format 0.2 is what capture writes. It adds, after the 0.1 keys and in this fixed order: capacity,
 * high_water, free_policy, retired_count, slots ([generation, alive, external_id] for slots below
 * high_water), event_next_seq, event_dropped, event_log_max, events ([event_seq, tick, kind,
 * system_index, slot, seq_in_row, a, b] per retained event). In 0.2 the columns and entity_ids hold
 * high_water rows; rows at or above high_water are implicitly dead, zero and generation 1.
 * Format 0.1 (the M0 layout) still parses and restores; its capacity must equal its row count.
 *
 * Parsing is strict: size and depth limits apply before any allocation, then JSON syntax, duplicate
 * keys, shape, schema, counts, value ranges, entity table canonical form, and finally canonical form
 * (the input must be byte-identical to its own re-encoding). Nothing is produced for the caller unless
 * the whole snapshot validates.
 *
 * Restore builds a NEW world. The live world is never modified, so a failed restore leaves it
 * unchanged, and the caller swaps its instance pointer only after SU_OK. Tick is advanced by
 * publishing empty ticks because su_world has no accessor to set the tick of an existing world. Cost
 * is therefore O(snapshot tick) publishes; that is acceptable for M0/M2 only.
 *
 * Schema information comes from the SuWorldDesc the world was created with. The caller passes the
 * descriptor; capture cross-checks it against the world through the public accessors. */
#ifndef SUTEKH_WORLD_SNAPSHOT_H
#define SUTEKH_WORLD_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/entities.h"
#include "sutekh/status.h"
#include "world/su_world.h"

#define SU_SNAPSHOT_FORMAT "0.2"                     /* written by capture */
#define SU_SNAPSHOT_FORMAT_01 "0.1"                  /* accepted by parse and restore */
#define SU_SNAPSHOT_FORMAT_03 "0.3"                  /* written by capture when lineage or retired ids exist */
#define SU_SNAPSHOT_MAX_BYTES (512u * 1024u * 1024u) /* runtime limits v2; input/output cap */
#define SU_SNAPSHOT_MAX_DEPTH 8u                     /* JSON nesting depth */
#define SU_SNAPSHOT_MAX_MANIFEST 1024u               /* manifest entries */
#define SU_SNAPSHOT_MAX_NAME 255u                    /* system IDs, revisions */
#define SU_SNAPSHOT_DEFAULT_EVENT_LOG_MAX UINT64_C(65536) /* capture without an event log */
#define SU_SNAPSHOT_EVENT_LOG_MAX_LIMIT UINT64_C(1048576)

typedef struct SuSnapshotManifestEntry {
    const char *system_id;   /* 1..SU_SNAPSHOT_MAX_NAME bytes, valid UTF-8, no NUL */
    char semantic_hash[65];  /* 64 lowercase hex characters plus NUL */
} SuSnapshotManifestEntry;

/* Event log state captured with a snapshot. The entries are the retained events in ascending
 * event_seq order; the log object itself belongs to the runtime. next_seq is the event_seq the next
 * event will receive. entries must be contiguous and end at next_seq - 1; count <= log_max. */
typedef struct SuSnapshotEvents {
    uint64_t next_seq;
    uint64_t dropped;
    uint64_t log_max;
    const SuEvent *entries;
    uint64_t count;
} SuSnapshotEvents;

/* Captured snapshot. Owned; release with su_snapshot_destroy. */
typedef struct SuSnapshot {
    uint8_t *bytes;     /* canonical encoding */
    size_t len;
    uint64_t tick;
    uint8_t digest[32]; /* SHA-256 of bytes */
    char id[65];        /* lowercase hex of digest */
} SuSnapshot;

/* Human-readable detail for the last parse failure (optional). */
typedef struct SuSnapshotError {
    char message[160];
} SuSnapshotError;

/* Validated, decoded snapshot. Owned; release with su_snapshot_data_free. */
typedef struct SuSnapshotData {
    uint64_t tick;
    char *world_revision;                 /* NUL-terminated copy */
    SuSnapshotManifestEntry *manifest;    /* schedule order; system_id owned */
    uint32_t manifest_count;
    uint32_t format_minor;                /* 1 for "0.1", 2 for "0.2" */
    uint64_t capacity;                    /* entity table capacity (0.1: row count) */
    uint64_t row_count;                   /* stored rows: high_water in 0.2, the row count in 0.1 */
    uint32_t field_count;                 /* ascending field id */
    uint32_t **columns;                   /* per field, row_count values each */
    SuEnts *entities;                     /* owned; generations, alive, external ids, retired */
    uint64_t event_next_seq;              /* 0.2 only; 0 for 0.1 */
    uint64_t event_dropped;
    uint64_t event_log_max;
    SuEvent *events;                      /* owned; event_count retained events */
    uint64_t event_count;
    /* Schema extensions (format 0.3; zero and empty for 0.1 and 0.2). */
    uint32_t *field_defaults;             /* per field, ascending id; 0 when absent */
    uint8_t (*lineage)[32];               /* migration hashes, oldest first */
    uint32_t lineage_count;
    uint32_t *retired;                    /* retired field IDs, ascending */
    uint32_t retired_count;
} SuSnapshotData;

/* Captures the committed root of w, with an empty event log (next_seq 0, log_max
 * SU_SNAPSHOT_DEFAULT_EVENT_LOG_MAX).
 * desc must describe w: same field count and layout generation, every desc field present in w with the
 * same name, and desc->row_count <= capacity. Schema identity comes from desc; entity identity from the
 * world's entity table.
 * SU_E_INVALID_ARGUMENT: NULL pointer, world inside a tick, invalid revision or manifest (empty or
 *   over-long strings, non-UTF-8, hash not 64 lowercase hex), invalid descriptor.
 * SU_E_SCHEMA_MISMATCH: world and descriptor disagree on row or field counts.
 * SU_E_RESOURCE_LIMIT: canonical size over SU_SNAPSHOT_MAX_BYTES or allocation failure.
 * On failure out is zeroed and nothing is retained. */
uint32_t su_snapshot_capture(const SuWorld *w, const SuWorldDesc *desc,
                             const char *world_revision,
                             const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                             SuSnapshot *out);
/* As su_snapshot_capture with the runtime's event log state (events may be NULL for an empty log).
 * SU_E_INVALID_ARGUMENT additionally for an invalid log: log_max 0 or over the limit, count > log_max,
 *   count > next_seq, NULL entries with count > 0, or entries not contiguous ending at next_seq - 1. */
uint32_t su_snapshot_capture_ex(const SuWorld *w, const SuWorldDesc *desc,
                                const char *world_revision,
                                const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                                const SuSnapshotEvents *events, SuSnapshot *out);
/* NULL-safe. Frees the bytes and zeroes the struct. */
void su_snapshot_destroy(SuSnapshot *s);

/* Strict parse and validation against desc. Order of checks (first failure):
 *   size (SU_E_RESOURCE_LIMIT, before allocation), empty input (SU_E_PARSE),
 *   JSON syntax/UTF-8 (SU_E_PARSE), duplicate keys (SU_E_DUPLICATE_KEY),
 *   top-level and nested shape, unknown or missing keys, JSON types, value
 *   ranges, counts (SU_E_TYPE), schema identity and field table, entity IDs and
 *   row count (SU_E_SCHEMA_MISMATCH), entity table and event log (SU_E_TYPE),
 *   canonical form (SU_E_TYPE).
 * Unknown format versions, wrong numeric profile, and non-u32 field types are
 * reported as above; see the handoff for the exact mapping.
 * On failure out is zeroed. err (optional) receives a short message. */
uint32_t su_snapshot_parse(const uint8_t *bytes, size_t len, const SuWorldDesc *desc,
                           SuSnapshotData *out, SuSnapshotError *err);
/* NULL-safe. */
void su_snapshot_data_free(SuSnapshotData *d);

/* Restores a snapshot into a NEW world created from desc. Requires data from
 * su_snapshot_parse with the same desc. On success *out_world holds the world
 * (tick == data->tick, entity table == snapshot, committed columns == snapshot, dead rows zero), and,
 * if manifest is non-NULL, *manifest and *manifest_count point at data's manifest (borrowed; valid
 * while data lives). The event log is returned in data (events, event_next_seq, event_dropped,
 * event_log_max). On failure *out_world is NULL and no existing world is touched.
 * SU_E_SCHEMA_MISMATCH if data does not fit desc. */
uint32_t su_snapshot_restore(const SuWorldDesc *desc, const SuSnapshotData *data,
                             SuWorld **out_world,
                             const SuSnapshotManifestEntry **manifest, uint32_t *manifest_count);

/* Schema-aware restore (M2-01a). With allow_schema_change 0, bytes must match desc exactly (schema
 * identity, field table, layout generation) and the restored world takes the snapshot's defaults, lineage
 * and retired IDs. With allow_schema_change 1 (rollback and recovery only) desc is ignored and the schema is
 * taken from the snapshot header: the world is built WITHOUT the original fixture. Its fields carry
 * SU_ACCESS_READ, because access is not stored in snapshots; installed systems re-establish access.
 * data_out (optional) receives the parsed data, to be freed with su_snapshot_data_free.
 * SU_E_SCHEMA_MISMATCH: schema or generation differs and allow_schema_change is 0. Other errors as parse. */
uint32_t su_snapshot_restore_world(const uint8_t *bytes, size_t len, const SuWorldDesc *desc,
                                   int allow_schema_change, SuWorld **out_world,
                                   SuSnapshotData *data_out, SuSnapshotError *err);

#endif
