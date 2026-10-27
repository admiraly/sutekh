/* Registered host operators (include/sutekh/host.h, "registered host operators", 2026-10-26): operators a game
 * defines outside this library and adds at run time with su_host_register_operator.
 *
 * Each registration is copied into a HostRegOp: an internal SuHostOpDesc (the declared field table, with
 * reg_run as its run) and a HostOperator whose callbacks create a per-world instance (RegInst: the column table,
 * the alive/generation views, the input hashes and the scratch, all allocated at world creation). reg_run adapts
 * the schedule's host-entry call (schedule.c run_host: the declared lanes under the descriptor's IDs, read-write
 * staging already holding the working values) to the public SuHostOperatorRun: it zero-fills write-only staging,
 * hashes the readable lanes, calls the game's run and re-hashes them. It allocates nothing.
 *
 * The registry is a fixed array of pointers to immutable entries, append-only for the life of the process, guarded
 * by one lock. Entries are never freed, so a pointer handed out stays valid. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "host/su_host_internal.h"
#include "host/su_host_state.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/su_xxh64.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "platform/su_thread.h"
#include "runtime/rts/host_decl.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "world/su_world.h"

struct HostRegOp {
    HostOperator op;              /* what lookups return */
    SuHostOpDesc desc;            /* internal descriptor; run = reg_run, host_ctx NULL (every entry has an instance) */
    SuHostField *fields;          /* desc.fields */
    SuHostOperatorDesc pub;       /* the caller's descriptor, strings and fields pointing at the copies below */
    SuHostOperatorField *pub_fields;
    char *system_id;
    char *schema_id;
    char *operator_name;
};

/* Parsed operator data, shared by the instances of one su_host_set_operator_data call (one world, one thread). */
typedef struct RegData {
    uint32_t refs;
    void *parsed;
    const HostRegOp *reg;
} RegData;

typedef struct RegInst {
    const HostRegOp *reg;
    uint64_t rows;
    SuHostOperatorColumn *cols;   /* field_count; field_id and access fixed, pointers set per call */
    uint64_t *in_hash;            /* field_count */
    uint8_t *alive;               /* rows */
    uint32_t *gen;                /* rows */
    void *scratch;
    size_t scratch_bytes;
    RegData *data;
    SuHostState *state;           /* operator state (issue 139), NULL when the descriptor declares none */
    SuEvent *ev;                  /* max_events_per_tick records of this tick (issue 172), or NULL */
    uint32_t ev_cap;
    uint32_t ev_n;
    uint32_t ev_refused;          /* SU_OK, or the code of an emission the operator did not see returned */
    char reason[128];             /* SuHostOperatorRun.reason, "" at every call */
    char why[256];                /* reason of the last failed run, "" otherwise */
} RegInst;

/* The v1 descriptor size is frozen: inserting a field before `user` must not move it (host.h, "Versioning"). */
_Static_assert(SU_HOST_OPERATOR_DESC_V1_SIZE == (sizeof(void *) == 8u ? 120u : 84u),
               "SU_HOST_OPERATOR_DESC_V1_SIZE moved: v1 descriptor fields may not change");
_Static_assert(sizeof(void *) == 8u || sizeof(void *) == 4u, "unsupported pointer size");
/* Version 2 appends four u32 without padding (issue 139). */
_Static_assert(SU_HOST_OPERATOR_DESC_V2_SIZE == (sizeof(void *) == 8u ? 136u : 100u),
               "SU_HOST_OPERATOR_DESC_V2_SIZE moved");
/* Version 3 appends two u32 without padding (issue 172). */
_Static_assert(SU_HOST_OPERATOR_DESC_V3_SIZE == (sizeof(void *) == 8u ? 144u : 108u),
               "SU_HOST_OPERATOR_DESC_V3_SIZE moved");
_Static_assert(sizeof(SuHostOperatorDesc) == SU_HOST_OPERATOR_DESC_V3_SIZE, "descriptor has trailing padding");
/* SuHostOperatorRun through `state` (the layout an operator built against the 2026-10-26 header reads). */
_Static_assert(SU_HOST_OPERATOR_RUN_STATE_SIZE == (sizeof(void *) == 8u ? 120u : 84u),
               "SU_HOST_OPERATOR_RUN_STATE_SIZE moved: the run struct's released fields may not change");
_Static_assert(sizeof(SuHostOperatorRun) == (sizeof(void *) == 8u ? 136u : 92u), "run struct has trailing padding");
_Static_assert(offsetof(SuHostOperatorDesc, constants_hash) == (sizeof(void *) == 8u ? 48u : 32u),
               "v1 descriptor layout changed");

/* POSIX: a statically initialized pthread mutex; Windows: a zeroed SRWLOCK is SRWLOCK_INIT. */
#if defined(_WIN32)
static SuMutex g_lock = {0};
#else
static SuMutex g_lock = {PTHREAD_MUTEX_INITIALIZER};
#endif
static HostRegOp *g_reg[SU_HOST_MAX_REGISTERED_OPERATORS];
static uint32_t g_reg_count;

static const char *const k_reserved_prefixes[] = {"rts.", "space3d.", "sutekh."};
/* Operator names of the engine's families (rts_ops_v<N>, space3d_ops_v<N>) and the engine's own namespace. */
static const char *const k_reserved_names[] = {"rts_ops", "space3d_ops", "sutekh"};

/* ASCII letter case folded, and '-' counted as '_' (so "space3d-ops" is "space3d_ops"). */
static int fold(char c) {
    return c == '-' ? '_' : tolower((unsigned char)c);
}

/* 1 when s begins with prefix, folding as above. */
static int has_prefix_ci(const char *s, const char *prefix) {
    size_t i;
    for (i = 0; prefix[i] != '\0'; i++) {
        if (s[i] == '\0' || fold(s[i]) != fold(prefix[i])) {
            return 0;
        }
    }
    return 1;
}

/* 1 when a and b are equal ignoring ASCII letter case. */
static int equal_ci(const char *a, const char *b) {
    size_t i;
    for (i = 0; a[i] != '\0' || b[i] != '\0'; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

static uint32_t reg_err(SuHostError *err, uint32_t code, uint32_t index, const char *fmt, ...) {
    if (err != NULL) {
        va_list ap;
        err->code = code;
        err->index = index;
        err->tick = UINT64_MAX;
        va_start(ap, fmt);
        vsnprintf(err->message, sizeof err->message, fmt, ap);
        va_end(ap);
    }
    return code;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1u);
    if (p != NULL) {
        memcpy(p, s, n + 1u);
    }
    return p;
}

/* 1 when s is 1..max bytes of [A-Za-z0-9_.-] with at least one letter or digit. */
static int ident_ok(const char *s, size_t max) {
    size_t i, n;
    int alnum = 0;
    if (s == NULL) {
        return 0;
    }
    n = strlen(s);
    if (n == 0u || n > max) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-')) {
            return 0;
        }
        alnum |= c != '_' && c != '.' && c != '-';
    }
    return alnum;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n > m && memcmp(s + n - m, suffix, m) == 0;
}

static void reg_entry_free(HostRegOp *r) {
    uint32_t i;
    if (r == NULL) {
        return;
    }
    if (r->fields != NULL) {
        for (i = 0; i < r->desc.field_count; i++) {
            free((char *)r->fields[i].name);
        }
    }
    free(r->fields);
    free(r->pub_fields);
    free(r->system_id);
    free(r->schema_id);
    free(r->operator_name);
    free(r);
}

/* ---------- per-world instances ---------- */

static void data_unref(RegData *d) {
    if (d != NULL && --d->refs == 0u) {
        if (d->reg->pub.release_data != NULL) {
            d->reg->pub.release_data(d->parsed, d->reg->pub.user);
        }
        free(d);
    }
}

static void reg_destroy(void *p) {
    RegInst *ri = (RegInst *)p;
    if (ri == NULL) {
        return;
    }
    data_unref(ri->data);
    su_hstate_destroy(ri->state);
    free(ri->cols);
    free(ri->in_hash);
    free(ri->alive);
    free(ri->gen);
    free(ri->scratch);
    free(ri->ev);
    free(ri);
}

static uint32_t reg_create(const HostOperator *op, uint64_t rows, void **inst) {
    const HostRegOp *r = op->reg;
    RegInst *ri;
    uint32_t i, n;
    *inst = NULL;
    if (r == NULL) {
        return SU_E_INTERNAL;
    }
    if (rows == 0u || rows > UINT32_MAX) {
        return SU_E_RESOURCE_LIMIT;
    }
    n = r->desc.field_count;
    ri = calloc(1u, sizeof *ri);
    if (ri == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    ri->reg = r;
    ri->rows = rows;
    ri->cols = calloc(n, sizeof *ri->cols);
    ri->in_hash = calloc(n, sizeof *ri->in_hash);
    ri->alive = malloc((size_t)rows);
    ri->gen = malloc((size_t)rows * sizeof *ri->gen);
    if (ri->cols == NULL || ri->in_hash == NULL || ri->alive == NULL || ri->gen == NULL) {
        reg_destroy(ri);
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < n; i++) {
        ri->cols[i].field_id = r->desc.fields[i].id;
        ri->cols[i].access = r->desc.fields[i].access;
    }
    if (r->pub.state_max_entries != 0u) {
        uint32_t st = su_hstate_create(r->pub.state_max_entries, r->pub.state_max_bytes, r->pub.state_flags,
                                       &ri->state);
        if (st != SU_OK) {
            reg_destroy(ri);
            return st;
        }
    }
    if (r->pub.max_events_per_tick != 0u) {
        ri->ev = calloc(r->pub.max_events_per_tick, sizeof *ri->ev);
        if (ri->ev == NULL) {
            reg_destroy(ri);
            return SU_E_RESOURCE_LIMIT;
        }
        ri->ev_cap = r->pub.max_events_per_tick;
    }
    if (r->pub.scratch_size != NULL) {
        size_t bytes = 0;
        uint32_t st = r->pub.scratch_size(rows, &bytes, r->pub.user);
        if (st != SU_OK) {
            reg_destroy(ri);
            return st;
        }
        if (bytes > 0u) {
            ri->scratch = calloc(1u, bytes);
            if (ri->scratch == NULL) {
                reg_destroy(ri);
                return SU_E_RESOURCE_LIMIT;
            }
            ri->scratch_bytes = bytes;
        }
    }
    *inst = ri;
    return SU_OK;
}

/* One event appended by the operator during run (issue 172). The engine numbers the record (system index, tick,
 * event_seq) at commit; the operator supplies the slot, the kind and the payload. A refusal is sticky: the code is
 * remembered and reg_run fails the tick with it even when run returns SU_OK, so a swallowed refusal is never a
 * silent drop (host.h, "events from host operators"). */
static uint32_t reg_emit_event(void *ctx, uint32_t slot, uint32_t kind, uint32_t a, uint32_t b) {
    RegInst *ri = (RegInst *)ctx;
    SuEvent *e;
    if (ri == NULL || ri->ev == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (kind >= SU_EVENT_KIND_RESERVED_MIN) {
        ri->ev_refused = SU_E_TYPE;
        return SU_E_TYPE;
    }
    if ((uint64_t)slot >= ri->rows) {
        ri->ev_refused = SU_E_INVALID_ARGUMENT;
        return SU_E_INVALID_ARGUMENT;
    }
    if (ri->ev_n >= ri->ev_cap) {
        ri->ev_refused = SU_E_RESOURCE_LIMIT;
        return SU_E_RESOURCE_LIMIT;
    }
    e = &ri->ev[ri->ev_n];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->slot = slot;
    e->seq = ri->ev_n; /* one serial call: the emission order within the run */
    e->a = a;
    e->b = b;
    ri->ev_n++;
    return SU_OK;
}

/* The events of the last run, in emission order (schedule.c run_host, su_sched_set_host_events). */
static SuEvent *reg_events(void *inst, uint32_t *count) {
    RegInst *ri = (RegInst *)inst;
    if (count != NULL) {
        *count = ri != NULL ? ri->ev_n : 0u;
    }
    return ri != NULL ? ri->ev : NULL;
}

void su_host_operator_event_config(const HostOperator *op, uint32_t *cap, SuHostEventDrain *drain) {
    if (cap != NULL) {
        *cap = 0u;
    }
    if (drain != NULL) {
        *drain = NULL;
    }
    if (op == NULL || op->reg == NULL || op->reg->pub.max_events_per_tick == 0u) {
        return;
    }
    if (cap != NULL) {
        *cap = op->reg->pub.max_events_per_tick;
    }
    if (drain != NULL) {
        *drain = reg_events;
    }
}

/* The schedule's host-entry call (schedule.c run_host): in holds the declared readable lanes (working columns),
 * out the declared writable lanes (staging; read-write lanes already hold their working values), both under the
 * descriptor's IDs. Allocates nothing. */
static uint32_t reg_run(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    RegInst *ri = (RegInst *)host_ctx;
    const HostRegOp *r;
    SuHostOperatorRun run;
    size_t bytes;
    int check_inputs;
    uint32_t i, st;

    if (ri == NULL || w == NULL || in == NULL || out == NULL || in->row_count != ri->rows) {
        return SU_E_INTERNAL;
    }
    r = ri->reg;
    bytes = (size_t)ri->rows * sizeof(uint32_t);
    check_inputs = (r->pub.flags & SU_HOST_OPERATOR_TRUST_INPUTS) == 0u;
    ri->why[0] = '\0';
    for (i = 0; i < r->desc.field_count; i++) {
        const SuHostField *f = &r->desc.fields[i];
        SuHostOperatorColumn *c = &ri->cols[i];
        c->in = NULL;
        c->out = NULL;
        if ((f->access & SU_ACCESS_READ) != 0u) {
            const SuColumnBinding *b = su_world_find_binding(in->bindings, in->binding_count, f->id);
            if (b == NULL || b->row_count != ri->rows) {
                return SU_E_INTERNAL;
            }
            c->in = (const uint32_t *)b->data;
            if (check_inputs) {
                ri->in_hash[i] = su_xxh64(c->in, bytes, 0u);
            }
        }
        if ((f->access & SU_ACCESS_WRITE) != 0u) {
            const SuColumnBinding *b = su_world_find_binding(out->bindings, out->binding_count, f->id);
            if (b == NULL || b->row_count != ri->rows) {
                return SU_E_INTERNAL;
            }
            c->out = (uint32_t *)b->data;
            if ((f->access & SU_ACCESS_READ) == 0u) {
                memset(c->out, 0, bytes); /* a write-only lane must not expose the previous values */
            }
        }
    }
    st = su_world_alive_view(w, 0u, (uint32_t)ri->rows, ri->alive);
    if (st == SU_OK) {
        st = su_world_generation_view(w, 0u, (uint32_t)ri->rows, ri->gen);
    }
    if (st != SU_OK) {
        return SU_E_INTERNAL;
    }
    if (ri->scratch != NULL) {
        memset(ri->scratch, 0, ri->scratch_bytes); /* always: scratch is not state */
    }
    ri->reason[0] = '\0';
    ri->ev_n = 0u;
    ri->ev_refused = SU_OK;
    memset(&run, 0, sizeof run);
    run.struct_size = (uint32_t)sizeof run;
    run.column_count = r->desc.field_count;
    run.columns = ri->cols;
    run.rows = ri->rows;
    run.row_begin = 0u;
    run.row_end = ri->rows;
    run.chunk_index = 0u;
    run.chunk_count = 1u;
    run.tick = su_world_tick(w);
    run.alive = ri->alive;
    run.generation = ri->gen;
    run.scratch = ri->scratch;
    run.scratch_bytes = ri->scratch_bytes;
    run.data = ri->data != NULL ? ri->data->parsed : NULL;
    run.reason = ri->reason;
    run.reason_cap = sizeof ri->reason;
    run.state = ri->state;
    run.emit_event = ri->ev != NULL ? reg_emit_event : NULL;
    run.emit_ctx = ri->ev != NULL ? ri : NULL;
    if (ri->state != NULL) {
        su_hstate_begin_run(ri->state, ri->alive, ri->gen, ri->rows);
    }
    st = r->pub.run(&run, r->pub.user);
    if (ri->state != NULL) {
        su_hstate_end_run(ri->state); /* changes stay in the working state: su_host_step commits or drops them */
    }
    if (st != SU_OK) {
        ri->reason[sizeof ri->reason - 1u] = '\0';
        snprintf(ri->why, sizeof ri->why, "operator run returned %s%s%s%s%s", su_host_status_name(st),
                 ri->reason[0] != '\0' ? ": " : "", ri->reason,
                 st == SU_E_RESOURCE_LIMIT && ri->state != NULL && su_hstate_cap_refusals(ri->state) > 0u
                     ? " (operator state cap reached)"
                     : "",
                 st == SU_E_RESOURCE_LIMIT && ri->ev_refused == SU_E_RESOURCE_LIMIT ? " (event cap reached)" : "");
        return st;
    }
    if (ri->ev_refused != SU_OK) {
        snprintf(ri->why, sizeof ri->why, "event emission refused: %s (declared cap %u)",
                 ri->ev_refused == SU_E_RESOURCE_LIMIT
                     ? "the per-tick event cap was reached"
                     : ri->ev_refused == SU_E_TYPE ? "the kind is reserved for engine records"
                                                   : "the slot is not a row of this world",
                 (unsigned)ri->ev_cap);
        return ri->ev_refused;
    }
    if (check_inputs) {
        for (i = 0; i < r->desc.field_count; i++) {
            if (ri->cols[i].in != NULL && su_xxh64(ri->cols[i].in, bytes, 0u) != ri->in_hash[i]) {
                snprintf(ri->why, sizeof ri->why, "operator wrote lane %s (id %u), which it may only read",
                         r->desc.fields[i].name, (unsigned)r->desc.fields[i].id);
                return SU_E_UNDECLARED_WRITE;
            }
        }
    }
    return SU_OK;
}

/* The world-specific part of a registered system's semantic hash: the descriptor flags and the scratch size for
 * this world's capacity. SHA-256("sutekh.host.registered.v1\0" || record semantic hash || u32 flags LE ||
 * u64 scratch bytes LE). */
static void reg_identity(const void *inst, uint8_t semantic[32]) {
    const RegInst *ri = (const RegInst *)inst;
    SuSha256 c;
    uint8_t b[12];
    uint64_t sb = (uint64_t)ri->scratch_bytes;
    uint32_t i, f = ri->reg->pub.flags;
    for (i = 0; i < 4u; i++) {
        b[i] = (uint8_t)(f >> (8u * i));
    }
    for (i = 0; i < 8u; i++) {
        b[4u + i] = (uint8_t)(sb >> (8u * i));
    }
    su_sha256_init(&c);
    su_sha256_update(&c, (const uint8_t *)"sutekh.host.registered.v1", 26u);
    su_sha256_update(&c, semantic, 32u);
    su_sha256_update(&c, b, sizeof b);
    su_sha256_final(&c, semantic);
    if (ri->reg->pub.max_events_per_tick != 0u) {
        /* Events (issue 172): SHA-256("sutekh.host.events.v1\0" || hash above || u32 max_events_per_tick). */
        uint32_t k;
        for (k = 0; k < 4u; k++) {
            b[k] = (uint8_t)(ri->reg->pub.max_events_per_tick >> (8u * k));
        }
        su_sha256_init(&c);
        su_sha256_update(&c, (const uint8_t *)"sutekh.host.events.v1", 21u);
        su_sha256_update(&c, semantic, 32u);
        su_sha256_update(&c, b, 4u);
        su_sha256_final(&c, semantic);
    }
    if (ri->state != NULL) {
        /* Operator state (issue 139): SHA-256("sutekh.host.state.v1\0" || hash above || u32 state_flags ||
         * u32 state_max_entries || u32 state_max_bytes), little-endian. Without state the hash is the one above. */
        const uint32_t v[3] = {ri->reg->pub.state_flags, ri->reg->pub.state_max_entries, ri->reg->pub.state_max_bytes};
        uint32_t k;
        for (k = 0; k < 3u; k++) {
            for (i = 0; i < 4u; i++) {
                b[4u * k + i] = (uint8_t)(v[k] >> (8u * i));
            }
        }
        su_sha256_init(&c);
        su_sha256_update(&c, (const uint8_t *)"sutekh.host.state.v1", 21u);
        su_sha256_update(&c, semantic, 32u);
        su_sha256_update(&c, b, 12u);
        su_sha256_final(&c, semantic);
    }
}

static SuHostState *reg_state(void *inst) {
    return inst != NULL ? ((RegInst *)inst)->state : NULL;
}

static const char *reg_last_error(const void *inst) {
    const RegInst *ri = (const RegInst *)inst;
    return ri != NULL ? ri->why : "";
}

/* Called by su_host_step before every tick, so a reason can only belong to this tick's attempt. */
static void reg_clear_error(void *inst) {
    RegInst *ri = (RegInst *)inst;
    if (ri != NULL) {
        ri->why[0] = '\0';
    }
}

static uint32_t reg_set_data(const HostOperator *op, void *inst, const uint8_t *data, size_t len, void **shared,
                             char *why, size_t why_cap) {
    const HostRegOp *r = op->reg;
    RegInst *ri = (RegInst *)inst;
    RegData *d = (RegData *)*shared;
    if (r == NULL || ri == NULL || r->pub.parse_data == NULL || ri->data != NULL) {
        return SU_E_INTERNAL;
    }
    if (d == NULL) {
        void *parsed = NULL;
        uint32_t st = r->pub.parse_data(data, len, &parsed, why, why_cap, r->pub.user);
        if (st != SU_OK) {
            return st;
        }
        d = malloc(sizeof *d);
        if (d == NULL) {
            if (r->pub.release_data != NULL) {
                r->pub.release_data(parsed, r->pub.user);
            }
            return SU_E_RESOURCE_LIMIT;
        }
        d->refs = 1u; /* held by the call until data_release */
        d->parsed = parsed;
        d->reg = r;
        *shared = d;
    }
    if (d->reg != r) {
        return SU_E_INTERNAL; /* su_host_set_operator_data never mixes registrations in one call */
    }
    d->refs++;
    ri->data = d;
    return SU_OK;
}

static void reg_data_release(void *shared) {
    data_unref((RegData *)shared);
}

/* ---------- registry ---------- */

const HostOperator *su_host_registered_find(const char *system_id) {
    const HostOperator *found = NULL;
    uint32_t i;
    if (system_id == NULL) {
        return NULL;
    }
    su_mutex_lock(&g_lock);
    for (i = 0; i < g_reg_count; i++) {
        if (strcmp(g_reg[i]->system_id, system_id) == 0) {
            found = &g_reg[i]->op;
            break;
        }
    }
    su_mutex_unlock(&g_lock);
    return found;
}

uint64_t su_host_operator_state_reservation(const HostOperator *op) {
    /* Registered descriptors are immutable once registered, so no lock is needed (as in reg_create). */
    if (op == NULL || op->reg == NULL) {
        return 0u;
    }
    return su_hstate_reservation(op->reg->pub.state_max_entries, op->reg->pub.state_max_bytes);
}

uint32_t su_host_registered_count(void) {
    uint32_t n;
    su_mutex_lock(&g_lock);
    n = g_reg_count;
    su_mutex_unlock(&g_lock);
    return n;
}

const HostOperator *su_host_registered_at(uint32_t index) {
    const HostOperator *op = NULL;
    su_mutex_lock(&g_lock);
    if (index < g_reg_count) {
        op = &g_reg[index]->op;
    }
    su_mutex_unlock(&g_lock);
    return op;
}

/* Field table rules of host.h (SuHostOperatorField). Returns SU_OK or SU_E_INVALID_ARGUMENT with err set. */
static uint32_t check_fields(const SuHostOperatorDesc *d, SuHostError *err) {
    uint32_t i, k;
    for (i = 0; i < d->field_count; i++) {
        const SuHostOperatorField *f = &d->fields[i];
        uint32_t t = f->type;
        if (!ident_ok(f->name, SU_WORLD_NAME_MAX)) {
            return reg_err(err, SU_E_INVALID_ARGUMENT, i, "field %u: name must be 1..%u bytes of [A-Za-z0-9_.-]",
                           (unsigned)i, (unsigned)SU_WORLD_NAME_MAX);
        }
        if (f->id == 0u || f->id >= SU_OPERAND_NONE || (i > 0u && f->id <= d->fields[i - 1u].id)) {
            return reg_err(err, SU_E_INVALID_ARGUMENT, i, "field %u (%s): ids must be nonzero and strictly ascending",
                           (unsigned)i, f->name);
        }
        if (t != SU_TYPE_U32 && t != SU_TYPE_I32 && t != SU_TYPE_I64 && t != SU_TYPE_Q32) {
            return reg_err(err, SU_E_INVALID_ARGUMENT, i, "field %u (%s): type must be u32, i32, i64 or q32",
                           (unsigned)i, f->name);
        }
        if (f->access != SU_ACCESS_READ && f->access != SU_ACCESS_WRITE &&
            f->access != (SU_ACCESS_READ | SU_ACCESS_WRITE)) {
            return reg_err(err, SU_E_INVALID_ARGUMENT, i, "field %u (%s): access must be read, write or both",
                           (unsigned)i, f->name);
        }
        if (f->reserved != 0u) {
            return reg_err(err, SU_E_INVALID_ARGUMENT, i, "field %u (%s): reserved must be 0", (unsigned)i, f->name);
        }
        for (k = 0; k < i; k++) {
            if (strcmp(d->fields[k].name, f->name) == 0) {
                return reg_err(err, SU_E_INVALID_ARGUMENT, i, "field %u: name %s is not unique", (unsigned)i,
                               f->name);
            }
        }
    }
    /* 64-bit fields: lanes I ("x.lo") and I + 1 ("x.hi"), same type and access (the IR 0.3 storage-lane rule). */
    for (i = 0; i < d->field_count; i++) {
        const SuHostOperatorField *f = &d->fields[i];
        const SuHostOperatorField *h;
        size_t stem;
        if (f->type != SU_TYPE_I64 && f->type != SU_TYPE_Q32) {
            continue;
        }
        h = i + 1u < d->field_count ? &d->fields[i + 1u] : NULL;
        stem = strlen(f->name) >= 3u ? strlen(f->name) - 3u : 0u;
        if (h == NULL || h->id != f->id + 1u || h->type != f->type || h->access != f->access ||
            !ends_with(f->name, ".lo") || !ends_with(h->name, ".hi") || strlen(h->name) != strlen(f->name) ||
            memcmp(f->name, h->name, stem) != 0) {
            return reg_err(err, SU_E_INVALID_ARGUMENT, i,
                           "field %u (%s): a 64-bit field is two lanes \"x.lo\" (id I) and \"x.hi\" (id I + 1) with "
                           "the same type and access",
                           (unsigned)i, f->name);
        }
        i++;
    }
    return SU_OK;
}

uint32_t su_host_register_operator(const SuHostOperatorDesc *caller, SuHostError *err) {
    HostRegOp *r;
    SuHostOperatorDesc local;
    const SuHostOperatorDesc *desc = &local;
    uint32_t i, st;
    size_t k;

    if (err != NULL) {
        memset(err, 0, sizeof *err);
        err->index = UINT32_MAX;
        err->tick = UINT64_MAX;
    }
    /* Versioning (host.h): any struct_size from the v1 size up; read only struct_size bytes; bytes beyond this
     * library's descriptor must be zero (fields it does not know). */
    if (caller == NULL || caller->struct_size < SU_HOST_OPERATOR_DESC_V1_SIZE) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "descriptor is NULL or struct_size is below %u",
                       (unsigned)SU_HOST_OPERATOR_DESC_V1_SIZE);
    }
    memset(&local, 0, sizeof local);
    memcpy(&local, caller, caller->struct_size < sizeof local ? caller->struct_size : sizeof local);
    for (k = sizeof local; k < caller->struct_size; k++) {
        if (((const unsigned char *)caller)[k] != 0u) {
            return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX,
                           "descriptor byte %u is a field this library does not know and is not zero", (unsigned)k);
        }
    }
    local.struct_size = (uint32_t)sizeof local;
    if ((desc->flags & ~SU_HOST_OPERATOR_TRUST_INPUTS) != 0u) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "unknown flags 0x%x",
                       (unsigned)(desc->flags & ~SU_HOST_OPERATOR_TRUST_INPUTS));
    }
    if (desc->run == NULL) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "run is NULL");
    }
    /* Operator state (version 2 fields; all zero for a version 1 descriptor). */
    if (desc->state_reserved != 0u || (desc->state_max_entries == 0u) != (desc->state_max_bytes == 0u) ||
        (desc->state_max_entries == 0u && desc->state_flags != 0u)) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX,
                       "state: state_max_entries and state_max_bytes must be both 0 (no state, flags 0) or both "
                       "nonzero, state_reserved 0");
    }
    if ((desc->state_flags & ~SU_HOST_STATE_DROP_DEAD) != 0u) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "unknown state_flags 0x%x",
                       (unsigned)(desc->state_flags & ~SU_HOST_STATE_DROP_DEAD));
    }
    if (desc->state_max_entries > SU_HOST_STATE_MAX_ENTRIES || desc->state_max_bytes > SU_HOST_STATE_MAX_BYTES) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX,
                       "state: at most %u entries and %u bytes", (unsigned)SU_HOST_STATE_MAX_ENTRIES,
                       (unsigned)SU_HOST_STATE_MAX_BYTES);
    }
    /* Events (version 3 fields; all zero for a version 1 or 2 descriptor). */
    if (desc->event_reserved != 0u) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "event_reserved must be 0");
    }
    if (desc->max_events_per_tick > SU_CMDQ_MAX_TICK_CAP) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX,
                       "max_events_per_tick must be 0 (no events) or 1..%u", (unsigned)SU_CMDQ_MAX_TICK_CAP);
    }
    if (desc->release_data != NULL && desc->parse_data == NULL) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "release_data without parse_data");
    }
    if (!ident_ok(desc->system_id, SU_DIAG_SYSTEM_BYTES - 1u)) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "system_id must be 1..%u bytes of [A-Za-z0-9_.-]",
                       (unsigned)(SU_DIAG_SYSTEM_BYTES - 1u));
    }
    if (!ident_ok(desc->schema_id, SU_WORLD_NAME_MAX)) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "schema_id must be 1..%u bytes of [A-Za-z0-9_.-]",
                       (unsigned)SU_WORLD_NAME_MAX);
    }
    if (!ident_ok(desc->operator_name, SU_WORLD_NAME_MAX)) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "operator_name must be 1..%u bytes of [A-Za-z0-9_.-]",
                       (unsigned)SU_WORLD_NAME_MAX);
    }
    if (desc->operator_version == 0u) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "operator_version must be >= 1");
    }
    if (desc->field_count == 0u || desc->field_count > SU_LIMIT_FIELDS || desc->fields == NULL) {
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "field_count must be 1..%u with a field table",
                       (unsigned)SU_LIMIT_FIELDS);
    }
    st = check_fields(desc, err);
    if (st != SU_OK) {
        return st;
    }
    if (su_host_builtin_find(desc->system_id) != NULL) {
        return reg_err(err, SU_E_DUPLICATE_KEY, UINT32_MAX, "system_id %s names an operator built into this library",
                       desc->system_id);
    }
    for (k = 0; k < sizeof k_reserved_prefixes / sizeof k_reserved_prefixes[0]; k++) {
        if (has_prefix_ci(desc->system_id, k_reserved_prefixes[k])) {
            return reg_err(err, SU_E_DUPLICATE_KEY, UINT32_MAX,
                           "system_id %s uses the prefix %s, which is reserved for engine operators (built in or in "
                           "the worker)",
                           desc->system_id, k_reserved_prefixes[k]);
        }
    }
    for (k = 0; k < sizeof k_reserved_names / sizeof k_reserved_names[0]; k++) {
        if (has_prefix_ci(desc->operator_name, k_reserved_names[k])) {
            return reg_err(err, SU_E_DUPLICATE_KEY, UINT32_MAX,
                           "operator_name %s begins with %s, which is reserved for engine operators",
                           desc->operator_name, k_reserved_names[k]);
        }
    }
    for (i = 0; i < su_host_builtin_count(); i++) {
        const char *bn = NULL;
        if (su_host_operator_info(i, NULL, &bn) == SU_OK && bn != NULL && strcmp(bn, desc->operator_name) == 0) {
            return reg_err(err, SU_E_DUPLICATE_KEY, UINT32_MAX, "operator_name %s is a built-in operator's name",
                           desc->operator_name);
        }
    }

    /* Copy everything the registry keeps. */
    r = calloc(1u, sizeof *r);
    if (r == NULL) {
        return reg_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, "out of memory");
    }
    r->system_id = dup_str(desc->system_id);
    r->schema_id = dup_str(desc->schema_id);
    r->operator_name = dup_str(desc->operator_name);
    r->fields = calloc(desc->field_count, sizeof *r->fields);
    r->pub_fields = calloc(desc->field_count, sizeof *r->pub_fields);
    r->desc.field_count = desc->field_count; /* reg_entry_free walks the names */
    if (r->system_id == NULL || r->schema_id == NULL || r->operator_name == NULL || r->fields == NULL ||
        r->pub_fields == NULL) {
        reg_entry_free(r);
        return reg_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, "out of memory");
    }
    for (i = 0; i < desc->field_count; i++) {
        char *nm = dup_str(desc->fields[i].name);
        if (nm == NULL) {
            reg_entry_free(r);
            return reg_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, "out of memory");
        }
        r->fields[i].id = desc->fields[i].id;
        r->fields[i].type = desc->fields[i].type;
        r->fields[i].access = desc->fields[i].access;
        r->fields[i].name = nm;
        r->pub_fields[i] = desc->fields[i];
        r->pub_fields[i].name = nm;
    }
    r->pub = *desc;
    r->pub.system_id = r->system_id;
    r->pub.schema_id = r->schema_id;
    r->pub.operator_name = r->operator_name;
    r->pub.fields = r->pub_fields;
    r->desc.system_id = r->system_id;
    r->desc.schema_id = r->schema_id;
    r->desc.operator_version = desc->operator_version;
    memcpy(r->desc.constants_hash, desc->constants_hash, 32u);
    r->desc.fields = r->fields;
    r->desc.run = reg_run;
    r->desc.host_ctx = NULL;
    if (su_host_desc_validate(&r->desc) != SU_OK) {
        reg_entry_free(r);
        return reg_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, "descriptor refused by the host declaration rules");
    }
    r->op.desc = &r->desc;
    r->op.operator_name = r->operator_name;
    r->op.create = reg_create;
    r->op.destroy = reg_destroy;
    r->op.set_data = desc->parse_data != NULL ? reg_set_data : NULL;
    r->op.data_release = desc->parse_data != NULL ? reg_data_release : NULL;
    r->op.reg = r;
    r->op.last_error = reg_last_error;
    r->op.identity = reg_identity;
    r->op.clear_error = reg_clear_error;
    r->op.state = reg_state;
    r->op.record_name = r->operator_name;

    su_mutex_lock(&g_lock);
    for (i = 0; i < g_reg_count; i++) {
        if (equal_ci(g_reg[i]->system_id, r->system_id)) {
            su_mutex_unlock(&g_lock);
            reg_entry_free(r);
            return reg_err(err, SU_E_DUPLICATE_KEY, UINT32_MAX, "system_id %s is already registered (as %s)",
                           desc->system_id, g_reg[i]->system_id);
        }
    }
    if (g_reg_count >= SU_HOST_MAX_REGISTERED_OPERATORS) {
        su_mutex_unlock(&g_lock);
        reg_entry_free(r);
        return reg_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, "the registry is full (%u operators)",
                       (unsigned)SU_HOST_MAX_REGISTERED_OPERATORS);
    }
    g_reg[g_reg_count++] = r;
    su_mutex_unlock(&g_lock);
    return SU_OK;
}
