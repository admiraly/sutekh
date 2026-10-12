/* Bounded store of committed-world checkpoints owned by the active worker (M0-08).
 *
 * Each checkpoint is a canonical snapshot (world/snapshot.h) plus its capture
 * cost. At most retain_limit snapshots are held; creating one more evicts the
 * oldest (FIFO by creation). Ids are monotonic and never reused, so a released
 * or evicted id is never served again: su_checkpoint_get returns an error and
 * no pointer for it.
 *
 * Borrowed pointers returned by su_checkpoint_get are valid until the next call
 * that releases, evicts or destroys that checkpoint. Peak memory during a create
 * is retain_limit + 1 snapshots, because the new capture completes before the
 * oldest entry is evicted. */
#ifndef SUTEKH_RUNTIME_CHECKPOINT_H
#define SUTEKH_RUNTIME_CHECKPOINT_H

#include <stdint.h>

#include "sutekh/status.h"
#include "world/snapshot.h"
#include "world/su_world.h"

#define SU_CHECKPOINT_DEFAULT_RETAIN 8u
#define SU_CHECKPOINT_MAX_RETAIN 1024u

typedef struct SuCheckpointStore SuCheckpointStore;

typedef struct SuCheckpointStats {
    uint32_t retain_limit;
    uint32_t retained_count;
    uint64_t retained_bytes;      /* sum of canonical snapshot bytes held */
    uint64_t overhead_bytes;      /* bookkeeping: store record plus entry slots */
    uint64_t created_total;
    uint64_t evicted_total;
    uint64_t released_total;
    int last_capture_ns_known;    /* 0 when the monotonic clock is unavailable */
    uint64_t last_capture_ns;
} SuCheckpointStats;

typedef struct SuCheckpointInfo {
    uint64_t id;
    uint64_t tick;
    uint64_t bytes;
    char snapshot_id[65];         /* SHA-256 of the canonical bytes, lowercase hex */
    int capture_ns_known;         /* 0 when not measurable: capture_ns is then 0 */
    uint64_t capture_ns;          /* monotonic time spent in the capture */
} SuCheckpointInfo;

/* retain_limit 0 selects SU_CHECKPOINT_DEFAULT_RETAIN.
 * SU_E_RESOURCE_LIMIT if retain_limit > SU_CHECKPOINT_MAX_RETAIN or allocation
 * fails. SU_E_INVALID_ARGUMENT for NULL out. */
uint32_t su_checkpoint_store_create(uint32_t retain_limit, SuCheckpointStore **out);
/* NULL-safe. Releases every retained snapshot. */
void su_checkpoint_store_destroy(SuCheckpointStore *s);

/* Captures the committed root of w as a new checkpoint and stores it.
 * Arguments are as su_snapshot_capture. On failure nothing is stored, nothing is
 * evicted and *out_id is 0. */
uint32_t su_checkpoint_create(SuCheckpointStore *s, const SuWorld *w, const SuWorldDesc *desc,
                              const char *world_revision,
                              const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                              uint64_t *out_id);
/* As su_checkpoint_create with the runtime event log (snapshot 0.2). events may be NULL. */
uint32_t su_checkpoint_create_ex(SuCheckpointStore *s, const SuWorld *w, const SuWorldDesc *desc,
                                 const char *world_revision,
                                 const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                                 const SuSnapshotEvents *events, uint64_t *out_id);
/* SU_E_INVALID_ARGUMENT if the id is unknown, released or evicted (*out NULL). */
uint32_t su_checkpoint_get(const SuCheckpointStore *s, uint64_t id, const SuSnapshot **out);
uint32_t su_checkpoint_info(const SuCheckpointStore *s, uint64_t id, SuCheckpointInfo *out);
/* Frees the checkpoint. A second release of the same id returns
 * SU_E_INVALID_ARGUMENT and changes nothing. */
uint32_t su_checkpoint_release(SuCheckpointStore *s, uint64_t id);
uint32_t su_checkpoint_stats(const SuCheckpointStore *s, SuCheckpointStats *out);

#endif
