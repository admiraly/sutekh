/* Canonical JSON snapshots of a committed world (M0_CONTRACT.md section 6, M0-08).
 *
 * A snapshot is captured only at a committed tick boundary. Its bytes are the
 * canonical encoding: compact JSON (no whitespace), fixed key order, fields in
 * ascending id order, columns keyed by field name in the same order. Identity
 * is the SHA-256 of those bytes, lowercase hex.
 *
 * Parsing is strict: size and depth limits apply before any allocation, then
 * JSON syntax, duplicate keys, shape, schema, counts, value ranges, and finally
 * canonical form (the input must be byte-identical to its own re-encoding).
 * Nothing is produced for the caller unless the whole snapshot validates.
 *
 * Restore builds a NEW world. The live world is never modified, so a failed
 * restore leaves it unchanged, and the caller swaps its instance pointer only
 * after SU_OK. Tick is advanced by publishing empty ticks because su_world has
 * no accessor to set the tick of an existing world (see the M0-08 handoff). Cost
 * is therefore O(snapshot tick) publishes; that is acceptable for M0 only.
 *
 * Schema information comes from the SuWorldDesc the world was created with. The
 * world exposes no accessor for schema_id, versions, names or entity IDs, so the
 * caller must pass the descriptor, and capture cross-checks it against the world
 * through the public accessors. */
#ifndef SUTEKH_WORLD_SNAPSHOT_H
#define SUTEKH_WORLD_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/status.h"
#include "world/su_world.h"

#define SU_SNAPSHOT_FORMAT "0.1"
#define SU_SNAPSHOT_MAX_BYTES (16u * 1024u * 1024u)  /* input and output cap */
#define SU_SNAPSHOT_MAX_DEPTH 8u                     /* JSON nesting depth */
#define SU_SNAPSHOT_MAX_MANIFEST 1024u               /* manifest entries */
#define SU_SNAPSHOT_MAX_NAME 255u                    /* system IDs, revisions */

typedef struct SuSnapshotManifestEntry {
    const char *system_id;   /* 1..SU_SNAPSHOT_MAX_NAME bytes, valid UTF-8, no NUL */
    char semantic_hash[65];  /* 64 lowercase hex characters plus NUL */
} SuSnapshotManifestEntry;

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
    uint64_t row_count;
    uint32_t field_count;                 /* ascending field id */
    uint32_t **columns;                   /* per field, row_count values each */
} SuSnapshotData;

/* Captures the committed root of w.
 * desc must describe w: same row count, field count and layout generation,
 * every desc field present in w with the same name. Entity IDs are taken from
 * desc and checked against the world's canonical encoding.
 * SU_E_INVALID_ARGUMENT: NULL pointer, world inside a tick, invalid revision or
 *   manifest (empty or over-long strings, non-UTF-8, hash not 64 lowercase hex),
 *   invalid descriptor, or a descriptor that does not match the world.
 * SU_E_SCHEMA_MISMATCH: world and descriptor disagree on row or field counts.
 * SU_E_RESOURCE_LIMIT: canonical size over SU_SNAPSHOT_MAX_BYTES or allocation
 *   failure.
 * On failure out is zeroed and nothing is retained. */
uint32_t su_snapshot_capture(const SuWorld *w, const SuWorldDesc *desc,
                             const char *world_revision,
                             const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                             SuSnapshot *out);
/* NULL-safe. Frees the bytes and zeroes the struct. */
void su_snapshot_destroy(SuSnapshot *s);

/* Strict parse and validation against desc. Order of checks (first failure):
 *   size (SU_E_RESOURCE_LIMIT, before allocation), empty input (SU_E_PARSE),
 *   JSON syntax/UTF-8 (SU_E_PARSE), duplicate keys (SU_E_DUPLICATE_KEY),
 *   top-level and nested shape, unknown or missing keys, JSON types, value
 *   ranges, counts (SU_E_TYPE), schema identity and field table, entity IDs and
 *   row count (SU_E_SCHEMA_MISMATCH), canonical form (SU_E_TYPE).
 * Unknown format versions, wrong numeric profile, and non-u32 field types are
 * reported as above; see the handoff for the exact mapping.
 * On failure out is zeroed. err (optional) receives a short message. */
uint32_t su_snapshot_parse(const uint8_t *bytes, size_t len, const SuWorldDesc *desc,
                           SuSnapshotData *out, SuSnapshotError *err);
/* NULL-safe. */
void su_snapshot_data_free(SuSnapshotData *d);

/* Restores a snapshot into a NEW world created from desc. Requires data from
 * su_snapshot_parse with the same desc. On success *out_world holds the world
 * (tick == data->tick, committed columns == snapshot), and, if manifest is
 * non-NULL, *manifest and *manifest_count point at data's manifest (borrowed;
 * valid while data lives). On failure *out_world is NULL and no existing
 * world is touched. SU_E_SCHEMA_MISMATCH if data does not fit desc. */
uint32_t su_snapshot_restore(const SuWorldDesc *desc, const SuSnapshotData *data,
                             SuWorld **out_world,
                             const SuSnapshotManifestEntry **manifest, uint32_t *manifest_count);

#endif
