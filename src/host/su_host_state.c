/* Operator state (include/sutekh/host.h, "operator state"; issue 139). See su_host_state.h for the model.
 *
 * Layout: a side (C or W) is an entry table sorted by key, strictly ascending, and the index of the arena holding
 * its values. A value is a contiguous range of its arena; arena bytes outside every live range are free (garbage
 * until a compaction). Values are allocated by bumping the side's top. Rules that keep C intact while W changes:
 *   - W starts as a copy of C's table in C's arena with base = C's top: offsets below base belong to C as well and
 *     are never written during the tick (copy-on-write: modify copies such a value to top first);
 *   - a compaction never runs in C's arena: while W shares it, the live values are copied, in key order, into the
 *     other arena; once W has its own arena, the compaction is in place (values moved down in offset order).
 * So whether a change fits depends only on the entry count and the sum of value lengths (the logical state). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "host/su_host_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/entities.h"
#include "sutekh/status.h"

#define NONE UINT32_MAX
#define ENTRY_WIRE 20u   /* u64 entity | u64 sub | u32 len */
#define HEAD_WIRE 12u    /* u32 count | u64 bytes */

typedef struct HsEntry {
    uint64_t entity;
    uint64_t sub;
    uint32_t off;
    uint32_t len;
} HsEntry;

typedef struct HsSide {
    HsEntry *e;     /* one of the two tables */
    uint32_t n;
    uint32_t ar;    /* arena index, 0 or 1 */
    uint64_t top;   /* next free byte of the arena */
    uint64_t live;  /* sum of len */
} HsSide;

struct SuHostState {
    uint32_t flags;
    uint32_t max_entries;
    uint64_t max_bytes;
    uint8_t *arena[2];
    HsEntry *table[2];
    uint32_t *perm;          /* max_entries: in-place compaction order */
    HsSide c;                /* committed */
    HsSide w;                /* working (active) or loaded (between ticks) */
    uint64_t base;           /* active and w.ar == c.ar: offsets >= base were written in this tick */
    int active;              /* W diverged from C in this tick */
    int writable;            /* inside run */
    uint32_t cap_refusals;
    const uint8_t *alive;    /* run's working entity views (DROP_DEAD checks) */
    const uint32_t *gen;
    uint64_t rows;
};

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

static int key_cmp(uint64_t ea, uint64_t sa, uint64_t eb, uint64_t sb) {
    if (ea != eb) {
        return ea < eb ? -1 : 1;
    }
    if (sa != sb) {
        return sa < sb ? -1 : 1;
    }
    return 0;
}

/* ---------- lifetime ---------- */

uint32_t su_hstate_create(uint32_t max_entries, uint32_t max_bytes, uint32_t flags, SuHostState **out) {
    SuHostState *s;
    *out = NULL;
    if (max_entries == 0u || max_entries > SU_HOST_STATE_MAX_ENTRIES || max_bytes == 0u ||
        max_bytes > SU_HOST_STATE_MAX_BYTES || (flags & ~SU_HOST_STATE_DROP_DEAD) != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    s = calloc(1u, sizeof *s);
    if (s == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    s->flags = flags;
    s->max_entries = max_entries;
    s->max_bytes = max_bytes;
    s->arena[0] = calloc(1u, max_bytes);
    s->arena[1] = calloc(1u, max_bytes);
    s->table[0] = calloc(max_entries, sizeof(HsEntry));
    s->table[1] = calloc(max_entries, sizeof(HsEntry));
    s->perm = calloc(max_entries, sizeof(uint32_t));
    if (s->arena[0] == NULL || s->arena[1] == NULL || s->table[0] == NULL || s->table[1] == NULL || s->perm == NULL) {
        su_hstate_destroy(s);
        return SU_E_RESOURCE_LIMIT;
    }
    s->c.e = s->table[0];
    s->c.ar = 0u;
    s->w.e = s->table[1];
    s->w.ar = 1u;
    *out = s;
    return SU_OK;
}

void su_hstate_destroy(SuHostState *s) {
    if (s == NULL) {
        return;
    }
    free(s->arena[0]);
    free(s->arena[1]);
    free(s->table[0]);
    free(s->table[1]);
    free(s->perm);
    free(s);
}

/* ---------- views ---------- */

static const HsSide *cur(const SuHostState *s) {
    return s->active ? &s->w : &s->c;
}

static uint32_t lower_bound(const HsSide *d, uint64_t entity, uint64_t sub) {
    uint32_t lo = 0, hi = d->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (key_cmp(d->e[mid].entity, d->e[mid].sub, entity, sub) < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static uint32_t find(const HsSide *d, uint64_t entity, uint64_t sub) {
    uint32_t i = lower_bound(d, entity, sub);
    return i < d->n && d->e[i].entity == entity && d->e[i].sub == sub ? i : NONE;
}

static uint8_t *value_ptr(const SuHostState *s, const HsSide *d, uint32_t i) {
    return s->arena[d->ar] + d->e[i].off;
}

/* ---------- the working side ---------- */

static void activate(SuHostState *s) {
    if (s->active) {
        return;
    }
    if (s->c.n > 0u) {
        memcpy(s->w.e, s->c.e, (size_t)s->c.n * sizeof(HsEntry));
    }
    s->w.n = s->c.n;
    s->w.ar = s->c.ar;
    s->w.top = s->c.top;
    s->w.live = s->c.live;
    s->base = s->c.top;
    s->active = 1;
}

/* 1 when W's value at i may be written in place (it is not shared with C). */
static int is_private(const SuHostState *s, uint32_t i) {
    return s->w.ar != s->c.ar || s->w.e[i].off >= s->base;
}

/* Heap sort of perm[0..n) by the entries' offsets (no allocation, deterministic). */
static void sift(uint32_t *p, const HsEntry *e, uint32_t root, uint32_t n) {
    for (;;) {
        uint32_t child = 2u * root + 1u;
        uint32_t t;
        if (child >= n) {
            return;
        }
        if (child + 1u < n && e[p[child + 1u]].off > e[p[child]].off) {
            child++;
        }
        if (e[p[root]].off >= e[p[child]].off) {
            return;
        }
        t = p[root];
        p[root] = p[child];
        p[child] = t;
        root = child;
    }
}

static void sort_by_offset(uint32_t *p, const HsEntry *e, uint32_t n) {
    uint32_t i;
    if (n < 2u) {
        return;
    }
    for (i = n / 2u; i-- > 0u;) {
        sift(p, e, i, n);
    }
    for (i = n - 1u; i > 0u; i--) {
        uint32_t t = p[0];
        p[0] = p[i];
        p[i] = t;
        sift(p, e, 0u, i);
    }
}

/* Packs W's live values (all but entry skip, whose value is being replaced) to the start of an arena that C does
 * not use; afterwards every W value is private. */
static void compact(SuHostState *s, uint32_t skip) {
    HsSide *w = &s->w;
    uint64_t pos = 0;
    uint32_t i;
    if (w->ar == s->c.ar) {
        uint32_t to = 1u - s->c.ar;
        for (i = 0; i < w->n; i++) {
            if (i == skip) {
                continue;
            }
            memcpy(s->arena[to] + pos, s->arena[w->ar] + w->e[i].off, w->e[i].len);
            w->e[i].off = (uint32_t)pos;
            pos += w->e[i].len;
        }
        w->ar = to;
    } else {
        uint32_t k = 0;
        for (i = 0; i < w->n; i++) {
            if (i != skip) {
                s->perm[k++] = i;
            }
        }
        sort_by_offset(s->perm, w->e, k);
        for (i = 0; i < k; i++) {
            HsEntry *e = &w->e[s->perm[i]];
            if (e->off != pos) {
                memmove(s->arena[w->ar] + pos, s->arena[w->ar] + e->off, e->len);
                e->off = (uint32_t)pos;
            }
            pos += e->len;
        }
    }
    if (skip != NONE) {
        w->e[skip].off = 0u;
        w->e[skip].len = 0u;
    }
    w->top = pos;
}

static int entity_alive(const SuHostState *s, uint64_t entity) {
    uint32_t slot = SU_ENTITY_SLOT(entity);
    uint32_t gen = SU_ENTITY_GEN(entity);
    return gen != 0u && slot < s->rows && s->alive != NULL && s->alive[slot] != 0u && s->gen[slot] == gen;
}

/* Inserts or replaces (entity, sub) with len uninitialized bytes; *out = the bytes. */
static uint32_t place(SuHostState *s, uint64_t entity, uint64_t sub, uint32_t len, uint8_t **out) {
    const HsSide *d = cur(s);
    uint32_t i = lower_bound(d, entity, sub);
    int exists = i < d->n && d->e[i].entity == entity && d->e[i].sub == sub;
    uint64_t old = exists ? d->e[i].len : 0u;
    HsSide *w;
    if (len == 0u || len > s->max_bytes) {
        return SU_E_INVALID_ARGUMENT;
    }
    if ((s->flags & SU_HOST_STATE_DROP_DEAD) != 0u && !entity_alive(s, entity)) {
        return SU_E_STALE_HANDLE;
    }
    if ((!exists && d->n >= s->max_entries) || d->live - old + len > s->max_bytes) {
        s->cap_refusals++;
        return SU_E_RESOURCE_LIMIT;
    }
    activate(s);
    w = &s->w;
    if (exists && old == len && is_private(s, i)) {
        *out = s->arena[w->ar] + w->e[i].off; /* already written in this tick: replaced in place */
        return SU_OK;
    }
    if (w->top + len > s->max_bytes) {
        compact(s, exists ? i : NONE);
    }
    if (!exists) {
        memmove(&w->e[i + 1u], &w->e[i], (size_t)(w->n - i) * sizeof(HsEntry));
        w->n++;
        w->e[i].entity = entity;
        w->e[i].sub = sub;
    }
    w->e[i].off = (uint32_t)w->top;
    w->e[i].len = len;
    w->top += len;
    w->live = w->live - old + len;
    *out = s->arena[w->ar] + w->e[i].off;
    return SU_OK;
}

static int writable(const SuHostState *s) {
    return s != NULL && s->writable;
}

/* ---------- engine side ---------- */

void su_hstate_begin_run(SuHostState *s, const uint8_t *alive, const uint32_t *gen, uint64_t rows) {
    s->alive = alive;
    s->gen = gen;
    s->rows = rows;
    s->cap_refusals = 0u;
    s->writable = 1;
}

void su_hstate_end_run(SuHostState *s) {
    s->writable = 0;
    s->alive = NULL;
    s->gen = NULL;
    s->rows = 0u;
}

uint32_t su_hstate_cap_refusals(const SuHostState *s) {
    return s->cap_refusals;
}

void su_hstate_commit(SuHostState *s, const SuEnts *ents) {
    HsSide t;
    if ((s->flags & SU_HOST_STATE_DROP_DEAD) != 0u && ents != NULL) {
        const HsSide *d = cur(s);
        uint32_t i, first = NONE;
        for (i = 0; i < d->n; i++) {
            if (su_ents_validate(ents, (SuEntity)d->e[i].entity) != SU_OK) {
                first = i;
                break;
            }
        }
        if (first != NONE) {
            HsSide *w;
            uint32_t k;
            uint64_t last = 0;
            int last_dead = 1;
            activate(s);
            w = &s->w;
            k = first;
            last = w->e[first].entity;
            for (i = first; i < w->n; i++) {
                if (i == first || w->e[i].entity != last) {
                    last = w->e[i].entity;
                    last_dead = su_ents_validate(ents, (SuEntity)last) != SU_OK;
                }
                if (last_dead) {
                    w->live -= w->e[i].len;
                } else {
                    w->e[k++] = w->e[i];
                }
            }
            w->n = k;
        }
    }
    if (s->active) {
        t = s->c;
        s->c = s->w;
        s->w = t;
        s->active = 0;
    }
}

void su_hstate_abort(SuHostState *s) {
    s->active = 0;
}

size_t su_hstate_encoded_size(const SuHostState *s) {
    return HEAD_WIRE + (size_t)s->c.n * ENTRY_WIRE + (size_t)s->c.live;
}

void su_hstate_encode(const SuHostState *s, uint8_t *out) {
    const HsSide *c = &s->c;
    uint8_t *v = out + HEAD_WIRE + (size_t)c->n * ENTRY_WIRE;
    uint32_t i;
    put_u32(out, c->n);
    put_u64(out + 4, c->live);
    for (i = 0; i < c->n; i++) {
        uint8_t *p = out + HEAD_WIRE + (size_t)i * ENTRY_WIRE;
        put_u64(p, c->e[i].entity);
        put_u64(p + 8, c->e[i].sub);
        put_u32(p + 16, c->e[i].len);
        memcpy(v, s->arena[c->ar] + c->e[i].off, c->e[i].len);
        v += c->e[i].len;
    }
}

uint32_t su_hstate_load(SuHostState *s, const uint8_t *p, size_t avail, size_t *used, char *why, size_t why_cap) {
    HsSide *w = &s->w;
    uint32_t n, i;
    uint64_t bytes, sum = 0, pos = 0;
    const uint8_t *v;
    *used = 0;
    s->active = 0;
    if (avail < HEAD_WIRE) {
        snprintf(why, why_cap, "truncated");
        return SU_E_PARSE;
    }
    n = rd_u32(p);
    bytes = rd_u64(p + 4);
    if (n > s->max_entries || bytes > s->max_bytes) {
        snprintf(why, why_cap, "%u entries / %llu bytes exceed the declared %u / %llu", (unsigned)n,
                 (unsigned long long)bytes, (unsigned)s->max_entries, (unsigned long long)s->max_bytes);
        return SU_E_PARSE;
    }
    if ((uint64_t)(avail - HEAD_WIRE) < (uint64_t)n * ENTRY_WIRE + bytes) {
        snprintf(why, why_cap, "truncated");
        return SU_E_PARSE;
    }
    w->ar = 1u - s->c.ar;
    v = p + HEAD_WIRE + (size_t)n * ENTRY_WIRE;
    for (i = 0; i < n; i++) {
        const uint8_t *q = p + HEAD_WIRE + (size_t)i * ENTRY_WIRE;
        uint64_t entity = rd_u64(q);
        uint64_t sub = rd_u64(q + 8);
        uint32_t len = rd_u32(q + 16);
        if (len == 0u || len > bytes - sum) {
            snprintf(why, why_cap, "entry %u: length %u is zero or exceeds the total", (unsigned)i, (unsigned)len);
            return SU_E_PARSE;
        }
        if (i > 0u && key_cmp(w->e[i - 1u].entity, w->e[i - 1u].sub, entity, sub) >= 0) {
            snprintf(why, why_cap, "entry %u: keys are not strictly ascending", (unsigned)i);
            return SU_E_PARSE;
        }
        if ((s->flags & SU_HOST_STATE_DROP_DEAD) != 0u &&
            (SU_ENTITY_GEN(entity) == 0u || SU_ENTITY_GEN(entity) == UINT32_C(0xFFFFFFFF))) {
            snprintf(why, why_cap, "entry %u: entity is not a handle", (unsigned)i);
            return SU_E_PARSE;
        }
        w->e[i].entity = entity;
        w->e[i].sub = sub;
        w->e[i].off = (uint32_t)pos;
        w->e[i].len = len;
        memcpy(s->arena[w->ar] + pos, v + pos, len);
        pos += len;
        sum += len;
    }
    if (sum != bytes) {
        snprintf(why, why_cap, "value lengths sum to %llu, not %llu", (unsigned long long)sum,
                 (unsigned long long)bytes);
        return SU_E_PARSE;
    }
    w->n = n;
    w->top = bytes;
    w->live = bytes;
    *used = HEAD_WIRE + (size_t)n * ENTRY_WIRE + (size_t)bytes;
    return SU_OK;
}

void su_hstate_swap(SuHostState *s) {
    HsSide t = s->c;
    s->c = s->w;
    s->w = t;
    s->active = 0;
}

/* ---------- public functions (host.h) ---------- */

uint32_t su_host_state_info(const SuHostState *s, SuHostStateInfo *out) {
    const HsSide *d;
    if (s == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    d = cur(s);
    memset(out, 0, sizeof *out);
    out->entries = d->n;
    out->max_entries = s->max_entries;
    out->bytes = d->live;
    out->max_bytes = s->max_bytes;
    out->flags = s->flags;
    out->writable = s->writable ? 1u : 0u;
    return SU_OK;
}

uint32_t su_host_state_count(const SuHostState *s) {
    return s != NULL ? cur(s)->n : 0u;
}

uint32_t su_host_state_lower_bound(const SuHostState *s, uint64_t entity, uint64_t sub) {
    return s != NULL ? lower_bound(cur(s), entity, sub) : 0u;
}

uint32_t su_host_state_entry(const SuHostState *s, uint32_t index, SuHostStateEntry *out) {
    const HsSide *d;
    if (s == NULL || out == NULL || index >= cur(s)->n) {
        return SU_E_INVALID_ARGUMENT;
    }
    d = cur(s);
    memset(out, 0, sizeof *out);
    out->entity = d->e[index].entity;
    out->sub = d->e[index].sub;
    out->data = value_ptr(s, d, index);
    out->len = d->e[index].len;
    return SU_OK;
}

uint32_t su_host_state_get(const SuHostState *s, uint64_t entity, uint64_t sub, const void **data, uint32_t *len) {
    const HsSide *d;
    uint32_t i;
    if (s == NULL || data == NULL || len == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    d = cur(s);
    i = find(d, entity, sub);
    *data = i != NONE ? value_ptr(s, d, i) : NULL;
    *len = i != NONE ? d->e[i].len : 0u;
    return SU_OK;
}

uint32_t su_host_state_put(SuHostState *s, uint64_t entity, uint64_t sub, const void *data, uint32_t len) {
    uint8_t *dst = NULL;
    uintptr_t a, b;
    uint32_t k, st;
    if (!writable(s) || data == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    a = (uintptr_t)data;
    b = a + len;
    for (k = 0; k < 2u; k++) {
        uintptr_t lo = (uintptr_t)s->arena[k], hi = lo + (uintptr_t)s->max_bytes;
        if (a < hi && b > lo) {
            return SU_E_INVALID_ARGUMENT; /* data points into the state: a compaction could move it */
        }
    }
    st = place(s, entity, sub, len, &dst);
    if (st == SU_OK) {
        memcpy(dst, data, len);
    }
    return st;
}

uint32_t su_host_state_alloc(SuHostState *s, uint64_t entity, uint64_t sub, uint32_t len, void **data) {
    uint8_t *dst = NULL;
    uint32_t st;
    if (!writable(s) || data == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *data = NULL;
    st = place(s, entity, sub, len, &dst);
    if (st == SU_OK) {
        memset(dst, 0, len);
        *data = dst;
    }
    return st;
}

uint32_t su_host_state_modify(SuHostState *s, uint64_t entity, uint64_t sub, void **data, uint32_t *len) {
    HsSide *w;
    uint32_t i;
    if (!writable(s) || data == NULL || len == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *data = NULL;
    *len = 0u;
    if (find(cur(s), entity, sub) == NONE) {
        return SU_OK;
    }
    activate(s);
    w = &s->w;
    i = find(w, entity, sub);
    if (!is_private(s, i)) {
        if (w->top + w->e[i].len <= s->max_bytes) {
            memcpy(s->arena[w->ar] + w->top, s->arena[w->ar] + w->e[i].off, w->e[i].len);
            w->e[i].off = (uint32_t)w->top;
            w->top += w->e[i].len;
        } else {
            compact(s, NONE); /* W shares C's arena here, so this copies everything into W's own arena */
        }
    }
    *data = value_ptr(s, w, i);
    *len = w->e[i].len;
    return SU_OK;
}

uint32_t su_host_state_erase(SuHostState *s, uint64_t entity, uint64_t sub, uint32_t *erased) {
    HsSide *w;
    uint32_t i;
    if (erased != NULL) {
        *erased = 0u;
    }
    if (!writable(s)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (find(cur(s), entity, sub) == NONE) {
        return SU_OK;
    }
    activate(s);
    w = &s->w;
    i = find(w, entity, sub);
    w->live -= w->e[i].len;
    memmove(&w->e[i], &w->e[i + 1u], (size_t)(w->n - i - 1u) * sizeof(HsEntry));
    w->n--;
    if (erased != NULL) {
        *erased = 1u;
    }
    return SU_OK;
}

uint32_t su_host_state_erase_entity(SuHostState *s, uint64_t entity, uint32_t *erased) {
    HsSide *w;
    uint32_t i0, i1, k;
    if (erased != NULL) {
        *erased = 0u;
    }
    if (!writable(s)) {
        return SU_E_INVALID_ARGUMENT;
    }
    i0 = lower_bound(cur(s), entity, 0u);
    i1 = entity != UINT64_MAX ? lower_bound(cur(s), entity + 1u, 0u) : cur(s)->n;
    if (i1 == i0) {
        return SU_OK;
    }
    activate(s);
    w = &s->w;
    for (k = i0; k < i1; k++) {
        w->live -= w->e[k].len;
    }
    memmove(&w->e[i0], &w->e[i1], (size_t)(w->n - i1) * sizeof(HsEntry));
    w->n -= i1 - i0;
    if (erased != NULL) {
        *erased = i1 - i0;
    }
    return SU_OK;
}
