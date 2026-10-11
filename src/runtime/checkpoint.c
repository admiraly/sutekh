/* Bounded checkpoint store (M0-08). See checkpoint.h. */
#include "runtime/checkpoint.h"

#include <stdlib.h>
#include <string.h>

#include "platform/su_platform.h"

typedef struct CheckpointEntry {
    uint64_t id;
    SuSnapshot snap;
    int ns_known;
    uint64_t ns;
} CheckpointEntry;

struct SuCheckpointStore {
    uint32_t limit;
    uint32_t count;
    CheckpointEntry *entries; /* [0, count) live, oldest first; fixed capacity limit */
    uint64_t next_id;
    uint64_t created_total;
    uint64_t evicted_total;
    uint64_t released_total;
    int last_ns_known;
    uint64_t last_ns;
};

uint32_t su_checkpoint_store_create(uint32_t retain_limit, SuCheckpointStore **out) {
    SuCheckpointStore *s;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (retain_limit == 0u) {
        retain_limit = SU_CHECKPOINT_DEFAULT_RETAIN;
    }
    if (retain_limit > SU_CHECKPOINT_MAX_RETAIN) {
        return SU_E_RESOURCE_LIMIT;
    }
    s = calloc(1, sizeof *s);
    if (s == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    s->entries = calloc(retain_limit, sizeof *s->entries);
    if (s->entries == NULL) {
        free(s);
        return SU_E_RESOURCE_LIMIT;
    }
    s->limit = retain_limit;
    s->next_id = 1u;
    *out = s;
    return SU_OK;
}

static void entry_release(CheckpointEntry *e) {
    su_snapshot_destroy(&e->snap);
    e->id = 0u;
    e->ns_known = 0;
    e->ns = 0u;
}

void su_checkpoint_store_destroy(SuCheckpointStore *s) {
    uint32_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->count; i++) {
        entry_release(&s->entries[i]);
    }
    free(s->entries);
    free(s);
}

/* Index of a live checkpoint, or -1. Ids are ascending in the array, so a
 * linear scan is exact; the array is bounded by SU_CHECKPOINT_MAX_RETAIN. */
static long find_index(const SuCheckpointStore *s, uint64_t id) {
    uint32_t i;
    if (id == 0u) {
        return -1;
    }
    for (i = 0; i < s->count; i++) {
        if (s->entries[i].id == id) {
            return (long)i;
        }
    }
    return -1;
}

/* Removes entry at index i, shifting later entries down (keeps oldest-first). */
static void remove_at(SuCheckpointStore *s, uint32_t i) {
    entry_release(&s->entries[i]);
    if (i + 1u < s->count) {
        memmove(&s->entries[i], &s->entries[i + 1u], (s->count - i - 1u) * sizeof *s->entries);
    }
    s->count--;
    memset(&s->entries[s->count], 0, sizeof s->entries[s->count]);
}

uint32_t su_checkpoint_create(SuCheckpointStore *s, const SuWorld *w, const SuWorldDesc *desc,
                              const char *world_revision,
                              const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                              uint64_t *out_id) {
    SuSnapshot snap;
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    int t0_ok;
    int t1_ok;
    int ns_known;
    uint64_t ns = 0;
    uint32_t st;

    if (out_id == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out_id = 0u;
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }

    t0_ok = su_platform_monotonic_ns(&t0);
    st = su_snapshot_capture(w, desc, world_revision, manifest, manifest_count, &snap);
    t1_ok = su_platform_monotonic_ns(&t1);
    if (st != SU_OK) {
        return st;
    }
    ns_known = t0_ok && t1_ok && t1 >= t0;
    if (ns_known) {
        ns = t1 - t0;
    }

    if (s->count == s->limit) {
        s->evicted_total++;
        remove_at(s, 0u);
    }
    s->entries[s->count].id = s->next_id;
    s->entries[s->count].snap = snap;
    s->entries[s->count].ns_known = ns_known;
    s->entries[s->count].ns = ns;
    s->count++;

    *out_id = s->next_id;
    s->next_id++;
    s->created_total++;
    s->last_ns_known = ns_known;
    s->last_ns = ns;
    return SU_OK;
}

uint32_t su_checkpoint_get(const SuCheckpointStore *s, uint64_t id, const SuSnapshot **out) {
    long i;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    i = find_index(s, id);
    if (i < 0) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = &s->entries[i].snap;
    return SU_OK;
}

uint32_t su_checkpoint_info(const SuCheckpointStore *s, uint64_t id, SuCheckpointInfo *out) {
    long i;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    i = find_index(s, id);
    if (i < 0) {
        return SU_E_INVALID_ARGUMENT;
    }
    out->id = s->entries[i].id;
    out->tick = s->entries[i].snap.tick;
    out->bytes = (uint64_t)s->entries[i].snap.len;
    memcpy(out->snapshot_id, s->entries[i].snap.id, sizeof out->snapshot_id);
    out->capture_ns_known = s->entries[i].ns_known;
    out->capture_ns = s->entries[i].ns_known ? s->entries[i].ns : 0u;
    return SU_OK;
}

uint32_t su_checkpoint_release(SuCheckpointStore *s, uint64_t id) {
    long i;
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    i = find_index(s, id);
    if (i < 0) {
        return SU_E_INVALID_ARGUMENT;
    }
    remove_at(s, (uint32_t)i);
    s->released_total++;
    return SU_OK;
}

uint32_t su_checkpoint_stats(const SuCheckpointStore *s, SuCheckpointStats *out) {
    uint32_t i;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < s->count; i++) {
        out->retained_bytes += (uint64_t)s->entries[i].snap.len;
    }
    out->retain_limit = s->limit;
    out->retained_count = s->count;
    out->overhead_bytes = (uint64_t)sizeof *s + (uint64_t)s->limit * sizeof *s->entries;
    out->created_total = s->created_total;
    out->evicted_total = s->evicted_total;
    out->released_total = s->released_total;
    out->last_capture_ns_known = s->last_ns_known;
    out->last_capture_ns = s->last_ns_known ? s->last_ns : 0u;
    return SU_OK;
}
