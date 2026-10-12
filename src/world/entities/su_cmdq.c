/* Bounded structural-command and event buffers, canonical merge, apply driver and event log
 * (M2-00b). See su_cmdq.h. */
#define _POSIX_C_SOURCE 200809L

#include "world/entities/su_cmdq.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct BufMeta {
    uint64_t n_s;
    uint64_t n_e;
    uint64_t fill;      /* 1 + global first-write rank; 0 = never written. Diagnostic only. */
    uint32_t cur_slot;
    uint32_t have_row;
    uint32_t row_s;
    uint32_t row_e;
    uint32_t last_seq;  /* last per-row seq appended to this buffer for cur_slot (any kind) */
    uint32_t have_seq;
} BufMeta;

typedef struct RecLayout {
    size_t esize;
    size_t off_sys;
    size_t off_slot;
    size_t off_seq;
} RecLayout;

static const RecLayout S_LAYOUT = {sizeof(SuStructCmd), offsetof(SuStructCmd, system_index),
                                   offsetof(SuStructCmd, slot), offsetof(SuStructCmd, seq)};
static const RecLayout E_LAYOUT = {sizeof(SuEvent), offsetof(SuEvent, system_index),
                                   offsetof(SuEvent, slot), offsetof(SuEvent, seq)};

struct SuCmdQ {
    uint32_t nsys;
    uint64_t nslots;
    uint64_t chunk_slots;
    uint64_t nchunks;
    uint64_t nbuf;
    uint32_t per_row_s;
    uint32_t per_row_e;
    uint64_t per_tick_s;
    uint64_t per_tick_e;
    BufMeta *meta;          /* nbuf */
    uint64_t *off_s;        /* nbuf + 1 prefix offsets into sbuf */
    uint64_t *off_e;        /* nbuf + 1 prefix offsets into ebuf */
    SuStructCmd *sbuf;
    SuEvent *ebuf;
    uint64_t *cursor;       /* nbuf, merge scratch */
    uint64_t *heap;         /* nbuf, merge scratch */
    _Atomic uint64_t tick_s;
    _Atomic uint64_t tick_e;
    _Atomic uint64_t fill_counter;
    _Atomic uint32_t overflow;
    SuStructCmd *m_cmd;     /* per_tick_s: merged commands */
    SuStructCmd *t_cmd;     /* per_tick_s: stable-sort temporary */
    SuEvent *m_evt;         /* per_tick_e: merged system events */
    SuEvent *t_evt;         /* per_tick_e: stable-sort temporary */
    SuEvent *rec;           /* per_tick_s: one record per command */
    SuEvent *fin;           /* per_tick_e: combined and sequenced tick events */
    uint64_t n_m_cmd;
    uint64_t n_m_evt;
    uint32_t cmd_fallback;
    uint32_t evt_fallback;
    uint64_t cmd_unsorted;
    uint64_t evt_unsorted;
};

static uint32_t rd_u32(const void *rec, size_t off) {
    uint32_t v;
    memcpy(&v, (const unsigned char *)rec + off, sizeof v);
    return v;
}

/* Canonical order (system_index, slot, seq). */
static int rec_cmp(const void *a, const void *b, const RecLayout *L) {
    uint32_t x = rd_u32(a, L->off_sys);
    uint32_t y = rd_u32(b, L->off_sys);
    if (x != y) {
        return x < y ? -1 : 1;
    }
    x = rd_u32(a, L->off_slot);
    y = rd_u32(b, L->off_slot);
    if (x != y) {
        return x < y ? -1 : 1;
    }
    x = rd_u32(a, L->off_seq);
    y = rd_u32(b, L->off_seq);
    if (x != y) {
        return x < y ? -1 : 1;
    }
    return 0;
}

static uint64_t chunk_size(const SuCmdQ *q, uint64_t c) {
    uint64_t rem = q->nslots - c * q->chunk_slots;
    return rem < q->chunk_slots ? rem : q->chunk_slots;
}

void su_cmdq_destroy(SuCmdQ *q) {
    if (q == NULL) {
        return;
    }
    free(q->meta);
    free(q->off_s);
    free(q->off_e);
    free(q->sbuf);
    free(q->ebuf);
    free(q->cursor);
    free(q->heap);
    free(q->m_cmd);
    free(q->t_cmd);
    free(q->m_evt);
    free(q->t_evt);
    free(q->rec);
    free(q->fin);
    free(q);
}

uint32_t su_cmdq_create(uint32_t nsystems, uint64_t nslots, uint64_t chunk_slots,
                        uint32_t per_row_struct_cap, uint32_t per_row_event_cap,
                        uint64_t per_tick_struct_cap, uint64_t per_tick_event_cap, SuCmdQ **out) {
    SuCmdQ *q;
    uint64_t b;
    uint64_t tot_s = 0;
    uint64_t tot_e = 0;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (nsystems == 0u || nslots == 0u || nslots > UINT64_C(0xFFFFFFFF) || chunk_slots == 0u ||
        per_tick_struct_cap == 0u || per_tick_event_cap == 0u ||
        per_tick_struct_cap > SU_CMDQ_MAX_TICK_CAP || per_tick_event_cap > SU_CMDQ_MAX_TICK_CAP ||
        per_row_struct_cap > 64u || per_row_event_cap > 64u) {
        return SU_E_INVALID_ARGUMENT;
    }

    q = calloc(1, sizeof *q);
    if (q == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    q->nsys = nsystems;
    q->nslots = nslots;
    q->chunk_slots = chunk_slots;
    q->nchunks = nslots / chunk_slots + (nslots % chunk_slots != 0u ? 1u : 0u);
    if (q->nchunks > SU_CMDQ_MAX_BUFFERS || nsystems > SU_CMDQ_MAX_BUFFERS / q->nchunks) {
        free(q);
        return SU_E_RESOURCE_LIMIT;
    }
    q->nbuf = (uint64_t)nsystems * q->nchunks;
    q->per_row_s = per_row_struct_cap;
    q->per_row_e = per_row_event_cap;
    q->per_tick_s = per_tick_struct_cap;
    q->per_tick_e = per_tick_event_cap;

    q->meta = calloc((size_t)q->nbuf, sizeof *q->meta);
    q->off_s = calloc((size_t)q->nbuf + 1u, sizeof *q->off_s);
    q->off_e = calloc((size_t)q->nbuf + 1u, sizeof *q->off_e);
    q->cursor = calloc((size_t)q->nbuf, sizeof *q->cursor);
    q->heap = calloc((size_t)q->nbuf, sizeof *q->heap);
    if (q->meta == NULL || q->off_s == NULL || q->off_e == NULL || q->cursor == NULL ||
        q->heap == NULL) {
        su_cmdq_destroy(q);
        return SU_E_RESOURCE_LIMIT;
    }

    /* Buffer capacity: min(chunk_size * max_per_row, per_tick_cap). Last chunk may be short. */
    for (b = 0; b < q->nbuf; b++) {
        uint64_t size = chunk_size(q, b % q->nchunks);
        uint64_t cs = size * per_row_struct_cap;
        uint64_t ce = size * per_row_event_cap;
        cs = cs < per_tick_struct_cap ? cs : per_tick_struct_cap;
        ce = ce < per_tick_event_cap ? ce : per_tick_event_cap;
        tot_s += cs;
        tot_e += ce;
        if (tot_s > SU_CMDQ_MAX_ENTRIES || tot_e > SU_CMDQ_MAX_ENTRIES) {
            su_cmdq_destroy(q);
            return SU_E_RESOURCE_LIMIT;
        }
        q->off_s[b + 1] = tot_s;
        q->off_e[b + 1] = tot_e;
    }

    q->sbuf = calloc(tot_s > 0u ? (size_t)tot_s : 1u, sizeof *q->sbuf);
    q->ebuf = calloc(tot_e > 0u ? (size_t)tot_e : 1u, sizeof *q->ebuf);
    q->m_cmd = calloc((size_t)per_tick_struct_cap, sizeof *q->m_cmd);
    q->t_cmd = calloc((size_t)per_tick_struct_cap, sizeof *q->t_cmd);
    q->rec = calloc((size_t)per_tick_struct_cap, sizeof *q->rec);
    q->m_evt = calloc((size_t)per_tick_event_cap, sizeof *q->m_evt);
    q->t_evt = calloc((size_t)per_tick_event_cap, sizeof *q->t_evt);
    q->fin = calloc((size_t)per_tick_event_cap, sizeof *q->fin);
    if (q->sbuf == NULL || q->ebuf == NULL || q->m_cmd == NULL || q->t_cmd == NULL ||
        q->rec == NULL || q->m_evt == NULL || q->t_evt == NULL || q->fin == NULL) {
        su_cmdq_destroy(q);
        return SU_E_RESOURCE_LIMIT;
    }

    atomic_init(&q->tick_s, 0u);
    atomic_init(&q->tick_e, 0u);
    atomic_init(&q->fill_counter, 0u);
    atomic_init(&q->overflow, 0u);
    *out = q;
    return SU_OK;
}

uint64_t su_cmdq_chunk_count(const SuCmdQ *q) {
    return q == NULL ? 0u : q->nchunks;
}

void su_cmdq_reset(SuCmdQ *q) {
    if (q == NULL) {
        return;
    }
    memset(q->meta, 0, (size_t)q->nbuf * sizeof *q->meta);
    atomic_store(&q->tick_s, 0u);
    atomic_store(&q->tick_e, 0u);
    atomic_store(&q->fill_counter, 0u);
    atomic_store(&q->overflow, 0u);
    q->n_m_cmd = 0u;
    q->n_m_evt = 0u;
    q->cmd_fallback = 0u;
    q->evt_fallback = 0u;
    q->cmd_unsorted = 0u;
    q->evt_unsorted = 0u;
}

static int buf_of(const SuCmdQ *q, uint32_t sys, uint64_t chunk, uint32_t slot, uint64_t *b) {
    if (sys >= q->nsys || chunk >= q->nchunks || (uint64_t)slot >= q->nslots) {
        return 0;
    }
    if ((uint64_t)slot / q->chunk_slots != chunk) {
        return 0;
    }
    *b = (uint64_t)sys * q->nchunks + chunk;
    return 1;
}

static void touch_row(BufMeta *m, uint32_t slot) {
    if (m->have_row == 0u || m->cur_slot != slot) {
        m->have_row = 1u;
        m->cur_slot = slot;
        m->row_s = 0u;
        m->row_e = 0u;
        m->have_seq = 0u;
    }
}

static void note_first_write(SuCmdQ *q, BufMeta *m) {
    if (m->fill == 0u) {
        m->fill = atomic_fetch_add(&q->fill_counter, 1u) + 1u;
    }
}

/* One per-row sequence for both record kinds: seq must strictly increase within a row (in the
 * buffer's emission order). Checked before any state changes. */
static uint32_t check_row_seq(BufMeta *m, uint32_t slot, uint32_t seq) {
    (void)slot;
    if (m->have_seq != 0u && seq <= m->last_seq) {
        return SU_E_INVALID_ARGUMENT;
    }
    return SU_OK;
}

uint32_t su_cmdq_append_struct(SuCmdQ *q, uint32_t sys, uint64_t chunk, uint32_t slot, uint32_t seq,
                               uint32_t kind, uint64_t arg, uint32_t init0, uint32_t init1) {
    uint64_t b;
    uint64_t cap;
    uint64_t cur;
    BufMeta *m;
    SuStructCmd *d;

    if (q == NULL || !buf_of(q, sys, chunk, slot, &b)) {
        return SU_E_INVALID_ARGUMENT;
    }
    m = &q->meta[b];
    touch_row(m, slot);
    if (check_row_seq(m, slot, seq) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    cap = q->off_s[b + 1u] - q->off_s[b];
    if (m->row_s >= q->per_row_s || m->n_s >= cap) {
        atomic_store(&q->overflow, 1u);
        return SU_E_RESOURCE_LIMIT;
    }
    cur = atomic_load(&q->tick_s);
    do {
        if (cur >= q->per_tick_s) {
            atomic_store(&q->overflow, 1u);
            return SU_E_RESOURCE_LIMIT;
        }
    } while (!atomic_compare_exchange_weak(&q->tick_s, &cur, cur + 1u));

    note_first_write(q, m);
    d = &q->sbuf[q->off_s[b] + m->n_s];
    d->kind = kind;
    d->system_index = sys;
    d->slot = slot;
    d->seq = seq;
    d->arg = arg;
    d->init[0] = init0;
    d->init[1] = init1;
    m->n_s++;
    m->row_s++;
    m->last_seq = seq;
    m->have_seq = 1u;
    return SU_OK;
}

uint32_t su_cmdq_append_event(SuCmdQ *q, uint32_t sys, uint64_t chunk, uint32_t slot, uint32_t seq,
                              uint32_t kind, uint32_t a, uint32_t b_val) {
    uint64_t b;
    uint64_t cap;
    uint64_t cur;
    BufMeta *m;
    SuEvent *d;

    if (q == NULL || !buf_of(q, sys, chunk, slot, &b)) {
        return SU_E_INVALID_ARGUMENT;
    }
    m = &q->meta[b];
    touch_row(m, slot);
    if (check_row_seq(m, slot, seq) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    cap = q->off_e[b + 1u] - q->off_e[b];
    if (m->row_e >= q->per_row_e || m->n_e >= cap) {
        atomic_store(&q->overflow, 1u);
        return SU_E_RESOURCE_LIMIT;
    }
    cur = atomic_load(&q->tick_e);
    do {
        if (cur >= q->per_tick_e) {
            atomic_store(&q->overflow, 1u);
            return SU_E_RESOURCE_LIMIT;
        }
    } while (!atomic_compare_exchange_weak(&q->tick_e, &cur, cur + 1u));

    note_first_write(q, m);
    d = &q->ebuf[q->off_e[b] + m->n_e];
    d->event_seq = 0u;
    d->tick = 0u;
    d->kind = kind;
    d->system_index = sys;
    d->slot = slot;
    d->seq = seq;
    d->a = a;
    d->b = b_val;
    m->n_e++;
    m->row_e++;
    m->last_seq = seq;
    m->have_seq = 1u;
    return SU_OK;
}

int su_cmdq_overflowed(SuCmdQ *q) {
    return q != NULL && atomic_load(&q->overflow) != 0u;
}

/* Sums the stored records of every buffer. Not for the append path. Call only when writers are
 * quiescent (after joins), so the result is the stored state, not a racing snapshot. */
void su_cmdq_counts(SuCmdQ *q, uint64_t *n_struct, uint64_t *n_event) {
    uint64_t b;
    uint64_t s = 0u;
    uint64_t e = 0u;
    if (q != NULL) {
        for (b = 0u; b < q->nbuf; b++) {
            s += q->meta[b].n_s;
            e += q->meta[b].n_e;
        }
    }
    if (n_struct != NULL) {
        *n_struct = s;
    }
    if (n_event != NULL) {
        *n_event = e;
    }
}

/* ---- merge ---------------------------------------------------------------- */

static uint64_t cnt_of(const SuCmdQ *q, int is_event, uint64_t b) {
    return is_event ? q->meta[b].n_e : q->meta[b].n_s;
}

static uint64_t off_of(const SuCmdQ *q, int is_event, uint64_t b) {
    return is_event ? q->off_e[b] : q->off_s[b];
}

typedef struct MCtx {
    SuCmdQ *q;
    unsigned char *base;
    const RecLayout *L;
    int is_event;
    uint64_t *heap;
    uint64_t hn;
} MCtx;

static const unsigned char *head_of(const MCtx *c, uint64_t b) {
    return c->base + (off_of(c->q, c->is_event, b) + c->q->cursor[b]) * c->L->esize;
}

/* Heap order: canonical key, then buffer index. Buffer keys never tie across buffers. */
static int heap_less(const MCtx *c, uint64_t b1, uint64_t b2) {
    int r = rec_cmp(head_of(c, b1), head_of(c, b2), c->L);
    if (r != 0) {
        return r < 0;
    }
    return b1 < b2;
}

static void sift_down(MCtx *c, uint64_t i) {
    for (;;) {
        uint64_t l = 2u * i + 1u;
        uint64_t r = l + 1u;
        uint64_t m = i;
        uint64_t t;
        if (l < c->hn && heap_less(c, c->heap[l], c->heap[m])) {
            m = l;
        }
        if (r < c->hn && heap_less(c, c->heap[r], c->heap[m])) {
            m = r;
        }
        if (m == i) {
            return;
        }
        t = c->heap[i];
        c->heap[i] = c->heap[m];
        c->heap[m] = t;
        i = m;
    }
}

/* Bottom-up stable merge sort of n records in a, using tmp (n records). */
static void stable_sort(unsigned char *a, unsigned char *tmp, uint64_t n, const RecLayout *L) {
    size_t es = L->esize;
    unsigned char *src = a;
    unsigned char *dst = tmp;
    uint64_t w;
    for (w = 1u; w < n; w *= 2u) {
        uint64_t lo;
        for (lo = 0u; lo < n; lo += 2u * w) {
            uint64_t mid = lo + w < n ? lo + w : n;
            uint64_t hi = lo + 2u * w < n ? lo + 2u * w : n;
            uint64_t i = lo;
            uint64_t j = mid;
            uint64_t k = lo;
            while (i < mid && j < hi) {
                if (rec_cmp(src + j * es, src + i * es, L) < 0) {
                    memcpy(dst + k * es, src + j * es, es);
                    j++;
                } else {
                    memcpy(dst + k * es, src + i * es, es);
                    i++;
                }
                k++;
            }
            while (i < mid) {
                memcpy(dst + k * es, src + i * es, es);
                i++;
                k++;
            }
            while (j < hi) {
                memcpy(dst + k * es, src + j * es, es);
                j++;
                k++;
            }
        }
        src = (src == a) ? tmp : a;
        dst = (src == a) ? tmp : a;
    }
    if (src != a) {
        memcpy(a, src, (size_t)n * es);
    }
}

static uint32_t merge_block(SuCmdQ *q, int is_event, unsigned char *base, unsigned char *out,
                            unsigned char *tmp, uint64_t cap_out, uint64_t *n_out,
                            uint32_t *fallback, uint64_t *unsorted) {
    const RecLayout *L = is_event ? &E_LAYOUT : &S_LAYOUT;
    size_t es = L->esize;
    uint64_t b;
    uint64_t total = 0u;
    uint64_t bad = 0u;

    for (b = 0u; b < q->nbuf; b++) {
        uint64_t cnt = cnt_of(q, is_event, b);
        const unsigned char *p = base + off_of(q, is_event, b) * es;
        uint64_t i;
        total += cnt;
        for (i = 1u; i < cnt; i++) {
            if (rec_cmp(p + (i - 1u) * es, p + i * es, L) >= 0) {
                bad++;
                break;
            }
        }
        q->cursor[b] = 0u;
    }
    if (total > cap_out) {
        return SU_E_INTERNAL;
    }
    *unsorted = bad;
    *fallback = bad != 0u ? 1u : 0u;
    *n_out = total;

    if (bad == 0u) {
        MCtx c;
        uint64_t k;
        c.q = q;
        c.base = base;
        c.L = L;
        c.is_event = is_event;
        c.heap = q->heap;
        c.hn = 0u;
        for (b = 0u; b < q->nbuf; b++) {
            if (cnt_of(q, is_event, b) > 0u) {
                q->heap[c.hn++] = b;
            }
        }
        for (k = c.hn / 2u; k > 0u; k--) {
            sift_down(&c, k - 1u);
        }
        for (k = 0u; k < total; k++) {
            uint64_t top = q->heap[0];
            memcpy(out + k * es, head_of(&c, top), es);
            q->cursor[top]++;
            if (q->cursor[top] >= cnt_of(q, is_event, top)) {
                q->heap[0] = q->heap[--c.hn];
            }
            if (c.hn > 0u) {
                sift_down(&c, 0u);
            }
        }
    } else {
        uint64_t k = 0u;
        for (b = 0u; b < q->nbuf; b++) {
            uint64_t cnt = cnt_of(q, is_event, b);
            memcpy(out + k * es, base + off_of(q, is_event, b) * es, (size_t)cnt * es);
            k += cnt;
        }
        stable_sort(out, tmp, total, L);
    }
    return SU_OK;
}

uint32_t su_cmdq_merge(SuCmdQ *q, SuCmdView *view) {
    uint32_t st;
    if (q == NULL || view == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (atomic_load(&q->overflow) != 0u) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = merge_block(q, 0, (unsigned char *)q->sbuf, (unsigned char *)q->m_cmd,
                     (unsigned char *)q->t_cmd, q->per_tick_s, &q->n_m_cmd, &q->cmd_fallback,
                     &q->cmd_unsorted);
    if (st != SU_OK) {
        return st;
    }
    st = merge_block(q, 1, (unsigned char *)q->ebuf, (unsigned char *)q->m_evt,
                     (unsigned char *)q->t_evt, q->per_tick_e, &q->n_m_evt, &q->evt_fallback,
                     &q->evt_unsorted);
    if (st != SU_OK) {
        return st;
    }
    view->cmds = q->m_cmd;
    view->n_cmds = q->n_m_cmd;
    view->events = q->m_evt;
    view->n_events = q->n_m_evt;
    view->cmd_fallback = q->cmd_fallback;
    view->event_fallback = q->evt_fallback;
    view->cmd_unsorted_buffers = q->cmd_unsorted;
    view->event_unsorted_buffers = q->evt_unsorted;
    return SU_OK;
}

/* ---- apply and commit ----------------------------------------------------- */

uint32_t su_cmdq_apply(const SuStructCmd *cmds, uint64_t n, const SuCmdCallbacks *cb, uint64_t tick,
                       SuEvent *rec, uint64_t rec_cap) {
    uint64_t i;
    if (cb == NULL || (n != 0u && (cmds == NULL || rec == NULL))) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (rec_cap < n) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0u; i < n; i++) {
        if (cmds[i].kind == SU_STRUCT_DESPAWN && cb->despawn == NULL) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    for (i = 0u; i < n; i++) {
        const SuStructCmd *c = &cmds[i];
        SuEvent *r = &rec[i];
        uint32_t st;
        uint32_t old_gen = 0u;

        if (c->kind == SU_STRUCT_DESPAWN) {
            st = cb->despawn(cb->ctx, c->arg, &old_gen);
        } else {
            st = SU_E_INVALID_ARGUMENT; /* unknown or reserved kind */
        }
        r->event_seq = 0u;
        r->tick = tick;
        r->system_index = c->system_index;
        r->slot = c->slot;
        r->seq = c->seq;
        if (st == SU_OK) {
            r->kind = SU_EVENT_DESPAWNED;
            r->a = SU_ENTITY_SLOT(c->arg);
            r->b = old_gen;
        } else {
            r->kind = SU_EVENT_REJECTED;
            r->a = c->kind;
            r->b = st;
        }
    }
    return SU_OK;
}

uint32_t su_cmdq_commit(SuCmdQ *q, const SuCmdCallbacks *cb, uint64_t tick, SuEventLog *log,
                        SuCommitReport *rep) {
    SuCmdView v;
    uint32_t st;
    uint64_t base;
    uint64_t i = 0u;
    uint64_t j = 0u;
    uint64_t k = 0u;
    uint64_t n_despawned = 0u;
    uint64_t n_rejected = 0u;

    if (q == NULL || cb == NULL || log == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_cmdq_merge(q, &v);
    if (st != SU_OK) {
        return st;
    }
    /* Every command yields one record that counts toward the per-tick event cap. Checked before
     * any callback runs. */
    if (v.n_events + v.n_cmds > q->per_tick_e) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_cmdq_apply(v.cmds, v.n_cmds, cb, tick, q->rec, q->per_tick_s);
    if (st != SU_OK) {
        return st;
    }
    for (j = 0u; j < v.n_cmds; j++) {
        if (q->rec[j].kind == SU_EVENT_DESPAWNED) {
            n_despawned++;
        } else {
            n_rejected++;
        }
    }

    base = su_evlog_next_seq(log);
    i = 0u;
    j = 0u;
    while (i < v.n_events || j < v.n_cmds) {
        int take_sys;
        if (i >= v.n_events) {
            take_sys = 0;
        } else if (j >= v.n_cmds) {
            take_sys = 1;
        } else {
            take_sys = rec_cmp(&v.events[i], &q->rec[j], &E_LAYOUT) <= 0;
        }
        if (take_sys) {
            q->fin[k] = v.events[i++];
        } else {
            q->fin[k] = q->rec[j++];
        }
        q->fin[k].tick = tick;
        q->fin[k].event_seq = base + k;
        k++;
    }
    st = su_evlog_append(log, q->fin, k);
    if (st != SU_OK) {
        return SU_E_INTERNAL;
    }
    if (rep != NULL) {
        rep->n_cmds = v.n_cmds;
        rep->n_despawned = n_despawned;
        rep->n_rejected = n_rejected;
        rep->n_events = k;
        rep->first_event_seq = base;
        rep->cmd_fallback = v.cmd_fallback;
        rep->event_fallback = v.event_fallback;
    }
    return SU_OK;
}

/* ---- event log ---------------------------------------------------------- */

struct SuEventLog {
    uint64_t capacity;
    uint64_t base_seq;
    uint64_t dropped;
    uint64_t count;
    uint64_t head;
    SuEvent *ring;
};

static const uint64_t LOG_SEQ_LIMIT = UINT64_C(0x3FFFFFFFFFFFFFFF);

uint32_t su_evlog_create(uint64_t capacity, uint64_t base_seq, SuEventLog **out) {
    SuEventLog *log;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (capacity == 0u || capacity > SU_CMDQ_MAX_LOG_CAPACITY || base_seq > LOG_SEQ_LIMIT) {
        return SU_E_INVALID_ARGUMENT;
    }
    log = calloc(1, sizeof *log);
    if (log == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    log->ring = calloc((size_t)capacity, sizeof *log->ring);
    if (log->ring == NULL) {
        free(log);
        return SU_E_RESOURCE_LIMIT;
    }
    log->capacity = capacity;
    log->base_seq = base_seq;
    *out = log;
    return SU_OK;
}

void su_evlog_destroy(SuEventLog *log) {
    if (log == NULL) {
        return;
    }
    free(log->ring);
    free(log);
}

uint64_t su_evlog_capacity(const SuEventLog *log) {
    return log == NULL ? 0u : log->capacity;
}
uint64_t su_evlog_count(const SuEventLog *log) {
    return log == NULL ? 0u : log->count;
}
uint64_t su_evlog_dropped(const SuEventLog *log) {
    return log == NULL ? 0u : log->dropped;
}
uint64_t su_evlog_oldest_seq(const SuEventLog *log) {
    return log == NULL ? 0u : log->base_seq + log->dropped;
}
uint64_t su_evlog_next_seq(const SuEventLog *log) {
    return log == NULL ? 0u : log->base_seq + log->dropped + log->count;
}

uint32_t su_evlog_append(SuEventLog *log, const SuEvent *ev, uint64_t n) {
    uint64_t next;
    uint64_t i;
    if (log == NULL || (n != 0u && ev == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    next = su_evlog_next_seq(log);
    for (i = 0u; i < n; i++) {
        if (ev[i].event_seq != next + i) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    for (i = 0u; i < n; i++) {
        if (log->count == log->capacity) {
            /* Full: evict the oldest retained record. */
            log->ring[log->head] = ev[i];
            log->head = (log->head + 1u) % log->capacity;
            log->dropped++;
        } else {
            log->ring[(log->head + log->count) % log->capacity] = ev[i];
            log->count++;
        }
    }
    return SU_OK;
}

/* Rebuilds a log with the state a snapshot recorded (contract r2 section 4): next_seq, dropped and
 * the retained events, oldest first. The retained events must be contiguous and end at next_seq - 1,
 * and n must not exceed the capacity. The oldest retained seq is next_seq - n and the first event
 * ever appended was next_seq - n - dropped. */
uint32_t su_evlog_restore(uint64_t capacity, uint64_t next_seq, uint64_t dropped, const SuEvent *ev,
                          uint64_t n, SuEventLog **out) {
    SuEventLog *log = NULL;
    uint64_t oldest;
    uint64_t i;
    uint32_t st;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (n > 0u && ev == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (n > capacity || next_seq > LOG_SEQ_LIMIT || n > next_seq || dropped > next_seq - n) {
        return SU_E_INVALID_ARGUMENT;
    }
    oldest = next_seq - n;
    for (i = 0u; i < n; i++) {
        if (ev[i].event_seq != oldest + i) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    st = su_evlog_create(capacity, oldest - dropped, &log);
    if (st != SU_OK) {
        return st;
    }
    log->dropped = dropped;
    if (n > 0u) {
        memcpy(log->ring, ev, (size_t)n * sizeof *ev);
    }
    log->count = n;
    log->head = 0u;
    *out = log;
    return SU_OK;
}

/* Copies the retained events, oldest first, into out (room for su_evlog_count records). */
uint32_t su_evlog_copy_retained(const SuEventLog *log, SuEvent *out, uint64_t cap, uint64_t *n) {
    uint64_t i;
    if (log == NULL || n == NULL || (log->count > 0u && out == NULL) || cap < log->count) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0u; i < log->count; i++) {
        out[i] = log->ring[(log->head + i) % log->capacity];
    }
    *n = log->count;
    return SU_OK;
}

uint32_t su_evlog_read(const SuEventLog *log, uint64_t cursor, uint64_t max, SuEvent *out,
                       uint64_t *n_out, uint64_t *next_cursor, uint64_t *skipped, uint64_t *dropped) {
    uint64_t oldest;
    uint64_t next;
    uint64_t start;
    uint64_t avail;
    uint64_t n;
    uint64_t i;

    if (log == NULL || n_out == NULL || next_cursor == NULL || skipped == NULL ||
        dropped == NULL || (max != 0u && out == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    oldest = log->base_seq + log->dropped;
    next = oldest + log->count;
    if (cursor > next) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (cursor < oldest) {
        *skipped = oldest - cursor;
        start = oldest;
    } else {
        *skipped = 0u;
        start = cursor;
    }
    avail = next - start;
    n = avail < max ? avail : max;
    for (i = 0u; i < n; i++) {
        out[i] = log->ring[(log->head + (start - oldest) + i) % log->capacity];
    }
    *n_out = n;
    *next_cursor = start + n;
    *dropped = log->dropped;
    return SU_OK;
}

static void put_u32(uint8_t *p, uint32_t v) {
    int i;
    for (i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void put_u64(uint8_t *p, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t get_u32(const uint8_t *p) {
    uint32_t v = 0u;
    int i;
    for (i = 3; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0u;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

uint32_t su_evlog_dump(const SuEventLog *log, uint8_t *buf, uint64_t cap, uint64_t *len) {
    uint64_t need;
    uint64_t i;
    if (log == NULL || len == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    need = SU_CMDQ_LOG_HEADER_BYTES + SU_CMDQ_LOG_ENTRY_BYTES * log->count;
    *len = need;
    if (buf == NULL) {
        return SU_OK;
    }
    if (cap < need) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(buf, "SUEL", 4);
    put_u32(buf + 4, SU_CMDQ_LOG_VERSION);
    put_u32(buf + 8, 0u);
    put_u64(buf + 12, log->capacity);
    put_u64(buf + 20, log->base_seq);
    put_u64(buf + 28, log->dropped);
    put_u64(buf + 36, log->count);
    for (i = 0u; i < log->count; i++) {
        const SuEvent *e = &log->ring[(log->head + i) % log->capacity];
        uint8_t *p = buf + SU_CMDQ_LOG_HEADER_BYTES + SU_CMDQ_LOG_ENTRY_BYTES * i;
        put_u64(p, e->event_seq);
        put_u64(p + 8, e->tick);
        put_u32(p + 16, e->kind);
        put_u32(p + 20, e->system_index);
        put_u32(p + 24, e->slot);
        put_u32(p + 28, e->seq);
        put_u32(p + 32, e->a);
        put_u32(p + 36, e->b);
    }
    return SU_OK;
}

uint32_t su_evlog_parse(const uint8_t *buf, uint64_t len, SuEventLog **out) {
    uint64_t capacity;
    uint64_t base;
    uint64_t dropped;
    uint64_t count;
    uint64_t i;
    SuEventLog *log;
    uint32_t st;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (buf == NULL && len != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (len < SU_CMDQ_LOG_HEADER_BYTES) {
        return SU_E_PARSE;
    }
    /* Oversize input is refused before any field is trusted or any allocation happens. */
    if (len > SU_CMDQ_LOG_HEADER_BYTES + SU_CMDQ_LOG_ENTRY_BYTES * SU_CMDQ_MAX_LOG_CAPACITY) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (memcmp(buf, "SUEL", 4) != 0 || get_u32(buf + 4) != SU_CMDQ_LOG_VERSION ||
        get_u32(buf + 8) != 0u) {
        return SU_E_PARSE;
    }
    capacity = get_u64(buf + 12);
    base = get_u64(buf + 20);
    dropped = get_u64(buf + 28);
    count = get_u64(buf + 36);
    if (capacity == 0u || base > LOG_SEQ_LIMIT) {
        return capacity > SU_CMDQ_MAX_LOG_CAPACITY ? SU_E_RESOURCE_LIMIT : SU_E_PARSE;
    }
    if (capacity > SU_CMDQ_MAX_LOG_CAPACITY) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (count > capacity || dropped > LOG_SEQ_LIMIT || (dropped > 0u && count != capacity)) {
        return SU_E_PARSE;
    }
    if (len != SU_CMDQ_LOG_HEADER_BYTES + SU_CMDQ_LOG_ENTRY_BYTES * count) {
        return SU_E_PARSE;
    }
    for (i = 0u; i < count; i++) {
        const uint8_t *p = buf + SU_CMDQ_LOG_HEADER_BYTES + SU_CMDQ_LOG_ENTRY_BYTES * i;
        if (get_u64(p) != base + dropped + i) {
            return SU_E_PARSE;
        }
    }
    st = su_evlog_create(capacity, base, &log);
    if (st != SU_OK) {
        return st;
    }
    log->dropped = dropped;
    log->count = count;
    log->head = 0u;
    for (i = 0u; i < count; i++) {
        const uint8_t *p = buf + SU_CMDQ_LOG_HEADER_BYTES + SU_CMDQ_LOG_ENTRY_BYTES * i;
        SuEvent *e = &log->ring[i];
        e->event_seq = get_u64(p);
        e->tick = get_u64(p + 8);
        e->kind = get_u32(p + 16);
        e->system_index = get_u32(p + 20);
        e->slot = get_u32(p + 24);
        e->seq = get_u32(p + 28);
        e->a = get_u32(p + 32);
        e->b = get_u32(p + 36);
    }
    *out = log;
    return SU_OK;
}

int su_evlog_equal(const SuEventLog *a, const SuEventLog *b) {
    uint64_t i;
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (a->capacity != b->capacity || a->base_seq != b->base_seq || a->dropped != b->dropped ||
        a->count != b->count) {
        return 0;
    }
    for (i = 0u; i < a->count; i++) {
        const SuEvent *x = &a->ring[(a->head + i) % a->capacity];
        const SuEvent *y = &b->ring[(b->head + i) % b->capacity];
        if (memcmp(x, y, sizeof *x) != 0) {
            return 0;
        }
    }
    return 1;
}
