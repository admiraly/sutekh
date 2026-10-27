/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Operator state (include/sutekh/host.h, "operator state"; issue 139), built against the static library and the DLL.
 * The operator is defined here, outside the engine: game.hull keeps sparse per-entity damage bricks keyed by
 * (entity handle, brick index), values of 8..32 bytes, plus a "heat" entry per entity (sub 1000) that it rewrites
 * every tick without any input (so the tick changes state on its own, with garbage and compactions). An IR system
 * (game.clear) clears the input lanes after it. An independent reference model (a plain sorted array here) checks
 * the state and the output columns after every tick.
 *
 * Schema game.hull (capacity 16, 12 ships): hit (read: brick + 1, 0 none), amt (read: damage, or a command below),
 * bricks / bytes / sum (write: entries, value bytes and the sum of every value byte but the last, per entity),
 * err (write: the last refusal a lenient command saw in that row).
 *
 * In-process checks (no arguments):
 *   1. registration rules of the state fields; read-only views; version 2 snapshots;
 *   2. mutation against the model, snapshot, allocating and in-place restore, re-simulation: identical digests
 *      (rollback over ticks that changed the state);
 *   3. the digests (SHA-256 and fast) change when only the state changes (columns equal);
 *   4. canonicality: one logical state reached by other mutation orders, churn and compactions gives the same bytes;
 *   5. caps: refusals change nothing; a run that returns the refusal fails the tick, world unchanged, message says so;
 *      many replacements beyond the arena size in one tick are never refused (no fragmentation effect);
 *   6. corrupted, oversized and truncated state sections refused by both restores, world unchanged;
 *   7. replay: keyframes carry the state (seek, play; a tampered keyframe value is a desync);
 *   8. SU_HOST_STATE_DROP_DEAD: dead entities' entries erased at publish, stale handles refused; without the flag
 *      the operator decides;
 *   9. no allocation in the tick or in a repeated in-place restore (static Linux build, -Wl,--wrap);
 *  10. misuse: changes outside run or through a view, put from the state's own memory;
 *  11. the world's operator-state budget (issue 153): the exact reservation, one operator that fits and one that
 *      does not, several that fit only under a raised budget, the default (0 and older descriptor sizes), nothing
 *      allocated for a refused world, identity independent of the budget.
 * Cross-process: record <dir>, then verify <dir> same|entries|bytes|flags|nostate (same registration: restores and
 * replays to the recorded digests; another declaration: SU_E_SCHEMA_MISMATCH). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"
#include "sutekh/replay.h"

static unsigned g_checks, g_failures;

static void check(int cond, const char *fmt, ...) {
    g_checks++;
    if (!cond) {
        va_list ap;
        g_failures++;
        fprintf(stderr, "FAIL: ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

#ifdef SU_TEST_WRAP_ALLOC
static volatile int g_count_alloc;
static unsigned long g_allocs;
static size_t g_alloc_max; /* largest single request while counting */
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t m);
void *__wrap_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n) {
    if (g_count_alloc) {
        g_allocs++;
        g_alloc_max = n > g_alloc_max ? n : g_alloc_max;
    }
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t m) {
    if (g_count_alloc) {
        g_allocs++;
        g_alloc_max = m != 0u && n <= SIZE_MAX / m && n * m > g_alloc_max ? n * m : g_alloc_max;
    }
    return __real_calloc(n, m);
}
void *__wrap_realloc(void *p, size_t n) {
    if (g_count_alloc) {
        g_allocs++;
        g_alloc_max = n > g_alloc_max ? n : g_alloc_max;
    }
    return __real_realloc(p, n);
}
#endif

/* ---------- schema and operator ---------- */

#define CAP 16u
#define SHIPS 12u
#define MAX_ENTRIES 128u
#define MAX_BYTES 4096u
#define HEAT_SUB 1000u
#define JUNK_SUB 5000u

#define CMD_ERASE UINT32_C(0xFFFF0000)        /* erase brick hit - 1 */
#define CMD_ERASE_ENTITY UINT32_C(0xFFFF0001) /* erase every entry of the entity */
#define CMD_FLOOD UINT32_C(0xFFFF0002)        /* alloc bricks 0 .. hit - 1, return the first refusal */
#define CMD_FLOOD_LENIENT UINT32_C(0xFFFF0003) /* the same, stop at the first refusal, report it in err */
#define CMD_CHURN UINT32_C(0xFFFF0004)        /* replace brick hit - 1 300 times with 64/60-byte values, then 8 bytes */
#define CMD_CHURN_FAIL UINT32_C(0xFFFF0005)   /* CHURN, then fail the run */
#define CMD_STALE UINT32_C(0xFFFF0006)        /* put a key for every dead slot's handle (DROP_DEAD: refused) */
#define CMD_ALIAS UINT32_C(0xFFFF0007)        /* put from a pointer into the state (refused) */
#define CMD_SHUFFLE UINT32_C(0xFFFF0008)      /* CHURN, re-put the entity's other bricks inverted and 8 bytes longer in
                                               * descending key order, CHURN again (in-place compaction of values that
                                               * are no longer in key order) */
#define CMD_FILL UINT32_C(0xFFFF0009)         /* put 64-byte values at subs 2000.. until refused (err) */
#define CMD_KILL UINT32_C(0xFFFF000A)         /* game.clear despawns the row in the tick (despawn_self) */
#define CMD_FLOOD_FAIL UINT32_C(0xFFFF000B)   /* lenient flood, then the run fails with another code */

enum { F_HIT = 1, F_AMT = 2, F_BRICKS = 3, F_BYTES = 4, F_SUM = 5, F_ERR = 6 };
enum { C_HIT, C_AMT, C_BRICKS, C_BYTES, C_SUM, C_ERR, C_COUNT };

static const SuHostOperatorField k_fields[C_COUNT] = {
    {F_HIT, SU_TYPE_U32, SU_ACCESS_READ, 0, "hit"},       {F_AMT, SU_TYPE_U32, SU_ACCESS_READ, 0, "amt"},
    {F_BRICKS, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "bricks"}, {F_BYTES, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "bytes"},
    {F_SUM, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "sum"},       {F_ERR, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "err"},
};

/* Test switches read by run (they never change what a correct engine must store, see each test). */
static int g_order;   /* 0: rows ascending, heat last; 1: descending, heat first, churn and put-instead-of-modify */
static int g_hidden;  /* change the last byte of every brick of slot 0 (not reflected in any column) */
static uint32_t g_misuse_status = SU_OK; /* CMD_ALIAS result */
static uint64_t g_placed;  /* value bytes placed by successful put/alloc/modify-copies (lower bound of arena use) */

static uint32_t brick_len(uint32_t brick) {
    return 8u + 8u * (brick % 4u);
}

static uint32_t heat_len(uint64_t tick) {
    return 8u + (uint32_t)(tick % 5u) * 8u;
}

static uint64_t handle_of(const SuHostOperatorRun *r, uint64_t slot) {
    return (uint64_t)slot | ((uint64_t)r->generation[slot] << 32);
}

/* One damage hit on (e, brick): byte[0] counts hits, byte[1 + tick % (len - 2)] += amt. */
static uint32_t damage(SuHostState *s, uint64_t e, uint32_t brick, uint32_t amt, uint64_t tick) {
    uint8_t *v = NULL;
    uint32_t len = 0, st;
    if (g_order == 0) {
        st = su_host_state_modify(s, e, brick, (void **)&v, &len);
        if (st != SU_OK) {
            return st;
        }
        if (v == NULL) {
            len = brick_len(brick);
            st = su_host_state_alloc(s, e, brick, len, (void **)&v);
            if (st != SU_OK) {
                return st;
            }
            g_placed += len;
        }
        v[0]++;
        v[1u + tick % (len - 2u)] = (uint8_t)(v[1u + tick % (len - 2u)] + amt);
        return SU_OK;
    } else {
        /* Same logical result through get + put (a replacement), with a junk entry put and erased around it. */
        const void *old = NULL;
        uint8_t tmp[64];
        uint32_t n = brick_len(brick);
        st = su_host_state_put(s, e, JUNK_SUB, "junkjunkjunkjunkjunkjunk", 24u);
        if (st != SU_OK) {
            return st;
        }
        g_placed += 24u;
        st = su_host_state_get(s, e, brick, &old, &len);
        if (st != SU_OK) {
            return st;
        }
        memset(tmp, 0, sizeof tmp);
        if (old != NULL) {
            memcpy(tmp, old, len);
            n = len;
        }
        tmp[0]++;
        tmp[1u + tick % (n - 2u)] = (uint8_t)(tmp[1u + tick % (n - 2u)] + amt);
        st = su_host_state_put(s, e, brick, tmp, n);
        if (st != SU_OK) {
            return st;
        }
        g_placed += n;
        return su_host_state_erase(s, e, JUNK_SUB, NULL);
    }
}

static uint32_t heat(const SuHostOperatorRun *r, SuHostState *s) {
    uint64_t slot = r->tick % r->rows;
    uint32_t st = SU_OK;
    if (r->alive[slot]) {
        uint8_t v[48];
        uint32_t k, n = heat_len(r->tick);
        for (k = 0; k < n; k++) {
            v[k] = (uint8_t)(r->tick + k);
        }
        st = su_host_state_put(s, handle_of(r, slot), HEAT_SUB, v, n);
        if (st == SU_OK) {
            g_placed += n;
        } else if (st == SU_E_RESOURCE_LIMIT) {
            st = SU_OK; /* heat is best effort: at a cap it is skipped (only the cap tests get there) */
        }
    }
    if (st == SU_OK && r->tick % 7u == 0u) {
        uint64_t s2 = (r->tick + 3u) % r->rows;
        if (r->alive[s2]) {
            st = su_host_state_erase(s, handle_of(r, s2), HEAT_SUB, NULL);
        }
    }
    return st;
}

static uint32_t churn(SuHostState *s, uint64_t e, uint32_t brick) {
    uint8_t v[64];
    uint32_t k, st;
    for (k = 0; k < 300u; k++) {
        uint32_t n = (k & 1u) != 0u ? 60u : 64u; /* alternate lengths: a same-length rewrite would reuse the bytes */
        memset(v, (int)(k & 0xFFu), sizeof v);
        st = su_host_state_put(s, e, brick, v, n);
        if (st != SU_OK) {
            return st;
        }
        g_placed += n;
    }
    memset(v, 0x5A, 8u);
    st = su_host_state_put(s, e, brick, v, 8u);
    g_placed += 8u;
    return st;
}

static uint32_t row_op(const SuHostOperatorRun *r, SuHostState *s, uint64_t i, uint32_t *err_out) {
    uint32_t hit = r->columns[C_HIT].in[i], amt = r->columns[C_AMT].in[i];
    uint64_t e = handle_of(r, i);
    uint32_t b, st = SU_OK;
    if (!r->alive[i] || hit == 0u) {
        return SU_OK;
    }
    switch (amt) {
    case CMD_ERASE:
        return su_host_state_erase(s, e, hit - 1u, NULL);
    case CMD_ERASE_ENTITY:
        return su_host_state_erase_entity(s, e, NULL);
    case CMD_FLOOD:
    case CMD_FLOOD_LENIENT:
    case CMD_FLOOD_FAIL:
        for (b = 0; b < hit && st == SU_OK; b++) {
            void *v = NULL;
            st = su_host_state_alloc(s, e, b, brick_len(b), &v);
            if (st == SU_OK) {
                memset(v, (int)(b + 1u), brick_len(b));
                g_placed += brick_len(b);
            }
        }
        if (amt == CMD_FLOOD_LENIENT) {
            *err_out = st;
            return SU_OK;
        }
        return amt == CMD_FLOOD_FAIL ? SU_E_CANDIDATE_FAILED : st;
    case CMD_CHURN:
    case CMD_CHURN_FAIL:
        st = churn(s, e, hit - 1u);
        return st != SU_OK ? st : amt == CMD_CHURN_FAIL ? SU_E_CANDIDATE_FAILED : SU_OK;
    case CMD_SHUFFLE: {
        uint32_t idx = su_host_state_lower_bound(s, e, 0u), end = idx;
        st = churn(s, e, hit - 1u);
        while (end < su_host_state_count(s)) {
            SuHostStateEntry en;
            if (su_host_state_entry(s, end, &en) != SU_OK || en.entity != e) {
                break;
            }
            end++;
        }
        for (b = end; st == SU_OK && b-- > idx;) {
            SuHostStateEntry en;
            uint8_t tmp[64];
            uint32_t k;
            if (su_host_state_entry(s, b, &en) != SU_OK || en.sub == hit - 1u || en.sub == HEAT_SUB || en.len > 56u) {
                continue;
            }
            for (k = 0; k < en.len; k++) {
                tmp[k] = (uint8_t)~((const uint8_t *)en.data)[k];
            }
            memset(tmp + en.len, 0xEE, 8u);
            st = su_host_state_put(s, e, en.sub, tmp, en.len + 8u);
            g_placed += en.len + 8u;
        }
        return st != SU_OK ? st : churn(s, e, hit - 1u);
    }
    case CMD_FILL: {
        uint8_t v[64];
        for (b = 0; st == SU_OK; b++) {
            memset(v, (int)(b + 1u), sizeof v);
            st = su_host_state_put(s, e, 2000u + b, v, sizeof v);
            g_placed += st == SU_OK ? sizeof v : 0u;
        }
        *err_out = st;
        return SU_OK;
    }
    case CMD_STALE: {
        uint64_t k;
        for (k = 0; k < r->rows; k++) {
            if (!r->alive[k]) {
                *err_out = su_host_state_put(s, handle_of(r, k), 7u, "stale", 5u);
                if (*err_out == SU_OK) {
                    g_placed += 5u;
                }
            }
        }
        return SU_OK;
    }
    case CMD_ALIAS: {
        const void *old = NULL;
        uint32_t len = 0;
        st = su_host_state_get(s, e, hit - 1u, &old, &len);
        g_misuse_status = st == SU_OK && old != NULL ? su_host_state_put(s, e, 9u, old, len) : SU_E_INTERNAL;
        return SU_OK;
    }
    case CMD_KILL:
        return SU_OK;
    default:
        return damage(s, e, hit - 1u, amt, r->tick);
    }
}

static uint32_t hull_run(const SuHostOperatorRun *r, void *user) {
    SuHostState *s = r->state;
    uint64_t i, k;
    uint32_t st = SU_OK;
    (void)user;
    if (r->struct_size < sizeof *r || s == NULL || r->column_count != C_COUNT) {
        snprintf(r->reason, r->reason_cap, "no state handle");
        return SU_E_INTERNAL;
    }
    for (i = 0; i < r->rows; i++) {
        r->columns[C_ERR].out[i] = 0u;
    }
    if (g_order == 1) {
        st = heat(r, s);
    }
    for (k = 0; k < r->rows && st == SU_OK; k++) {
        i = g_order == 0 ? k : r->rows - 1u - k;
        st = row_op(r, s, i, &r->columns[C_ERR].out[i]);
    }
    if (st == SU_OK && g_order == 0) {
        st = heat(r, s);
    }
    if (st != SU_OK) {
        snprintf(r->reason, r->reason_cap, "state call returned %s", su_host_status_name(st));
        return st;
    }
    if (g_hidden && r->alive[0]) {
        uint64_t e = handle_of(r, 0);
        uint32_t idx;
        for (idx = su_host_state_lower_bound(s, e, 0u); idx < su_host_state_count(s); idx++) {
            SuHostStateEntry en;
            uint8_t *v = NULL;
            uint32_t len = 0;
            if (su_host_state_entry(s, idx, &en) != SU_OK || en.entity != e) {
                break;
            }
            if (su_host_state_modify(s, e, en.sub, (void **)&v, &len) == SU_OK && v != NULL) {
                v[len - 1u] ^= 0x80u;
            }
        }
    }
    /* Outputs: per alive entity, entries, value bytes and the sum of every value byte but the last. */
    for (i = 0; i < r->rows; i++) {
        uint32_t n = 0, bytes = 0, sum = 0, idx;
        if (r->alive[i]) {
            uint64_t e = handle_of(r, i);
            for (idx = su_host_state_lower_bound(s, e, 0u); idx < su_host_state_count(s); idx++) {
                SuHostStateEntry en;
                uint32_t b;
                if (su_host_state_entry(s, idx, &en) != SU_OK || en.entity != e) {
                    break;
                }
                n++;
                bytes += en.len;
                for (b = 0; b + 1u < en.len; b++) {
                    sum += ((const uint8_t *)en.data)[b];
                }
            }
        }
        r->columns[C_BRICKS].out[i] = n;
        r->columns[C_BYTES].out[i] = bytes;
        r->columns[C_SUM].out[i] = sum;
    }
    return SU_OK;
}

enum { V_SAME, V_ENTRIES, V_BYTES, V_FLAGS, V_NOSTATE };

static void hull_desc(SuHostOperatorDesc *d, const char *system_id, uint32_t flags, int variant) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(d);
    d->system_id = system_id;
    d->schema_id = "game.hull";
    d->operator_name = "hull_ops_v1";
    d->operator_version = 1u;
    d->field_count = C_COUNT;
    d->fields = k_fields;
    for (i = 0; i < 32u; i++) {
        d->constants_hash[i] = (uint8_t)(i * 13u + 5u);
    }
    d->run = hull_run;
    if (variant != V_NOSTATE) {
        d->state_flags = variant == V_FLAGS ? (flags ^ SU_HOST_STATE_DROP_DEAD) : flags;
        d->state_max_entries = variant == V_ENTRIES ? MAX_ENTRIES + 1u : MAX_ENTRIES;
        d->state_max_bytes = variant == V_BYTES ? MAX_BYTES * 2u : MAX_BYTES;
    }
}

/* ---------- JSON ---------- */

typedef struct Buf {
    char *p;
    size_t n, cap;
} Buf;

static void put(Buf *b, const char *fmt, ...) {
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->p != NULL ? b->p + b->n : NULL, b->p != NULL ? b->cap - b->n : 0u, fmt, ap);
        va_end(ap);
        if (k < 0) {
            exit(2);
        }
        if (b->p != NULL && b->n + (size_t)k < b->cap) {
            b->n += (size_t)k;
            return;
        }
        b->cap = (b->cap + (size_t)k + 256u) * 2u;
        b->p = realloc(b->p, b->cap);
        if (b->p == NULL) {
            exit(2);
        }
    }
}

typedef struct Game {
    Buf record, clear_cap, clear_lir, fixture;
    SuHostCapsule caps[2];
    SuHostWorldDesc desc;
} Game;

/* The host record of d's operator; after: a JSON string list ("" for none). */
static void record_json(Buf *b, const SuHostOperatorDesc *d, const char *after) {
    uint32_t i;
    put(b, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"system_rev\":1,\"capabilities\":[],", d->system_id);
    put(b, "\"schema\":{\"id\":\"game.hull\",\"version\":1,\"layout_generation\":1},");
    put(b, "\"numeric_profile\":\"u32_mod_v1\",");
    put(b, "\"schedule\":{\"phase\":\"query\",\"after\":[%s],\"before\":[],\"parallel\":\"serial\"},", after);
    put(b, "\"host\":{\"operator\":\"%s\",\"operator_version\":%u,\"constants_hash\":\"", d->operator_name,
        (unsigned)d->operator_version);
    for (i = 0; i < 32u; i++) {
        put(b, "%02x", (unsigned)d->constants_hash[i]);
    }
    put(b, "\",\"fields\":[");
    for (i = 0; i < d->field_count; i++) {
        const SuHostOperatorField *f = &d->fields[i];
        put(b, "%s{\"id\":%u,\"name\":\"%s\",\"type\":\"u32\",\"access\":\"%s\"}", i > 0u ? "," : "",
            (unsigned)f->id, f->name, f->access == SU_ACCESS_READ ? "read" : "write");
    }
    put(b, "]}}");
}

static void game_build(Game *g, const SuHostOperatorDesc *d) {
    static const char *const cols[] = {"hit", "amt", "bricks", "bytes", "sum", "err"};
    uint32_t i, k;
    memset(g, 0, sizeof *g);
    record_json(&g->record, d, "");
    put(&g->clear_cap, "{\"capsule_format\":\"0.1\",\"system_id\":\"game.clear\",\"source\":\"inline.lir.json\","
                       "\"schema\":{\"id\":\"game.hull\",\"version\":1,\"layout_generation\":1},"
                       "\"numeric_profile\":\"u32_mod_v1\",\"fields\":["
                       "{\"id\":1,\"name\":\"hit\",\"type\":\"u32\",\"read\":false,\"write\":true},"
                       "{\"id\":2,\"name\":\"amt\",\"type\":\"u32\",\"read\":true,\"write\":true}],");
    put(&g->clear_cap, "\"schedule\":{\"phase\":\"simulate\",\"after\":[\"%s\"],\"before\":[],\"parallel\":\"serial\"},",
        d->system_id);
    put(&g->clear_cap, "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\","
                       "\"max_struct_per_row\":1,\"max_events_per_row\":0},"
                       "\"tests\":[\"tests/host/test_host_opstate.c\"],\"ir_version\":\"0.2\"}");
    put(&g->clear_lir, "{\"ir_version\":\"0.2\",\"system_id\":\"game.clear\",\"numeric_profile\":\"u32_mod_v1\","
                       "\"query\":\"game.hull\",\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"amt\"},"
                       "{\"op\":\"const_u32\",\"dst\":\"k\",\"value\":4294901770},"
                       "{\"op\":\"eq_u32\",\"dst\":\"c\",\"a\":\"a\",\"b\":\"k\"},"
                       "{\"op\":\"despawn_self\",\"cond\":\"c\"},"
                       "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},"
                       "{\"op\":\"store_u32\",\"field\":\"hit\",\"src\":\"z\"},"
                       "{\"op\":\"store_u32\",\"field\":\"amt\",\"src\":\"z\"}]}");
    put(&g->fixture, "{\"fixture_format\":\"0.1\",\"schema_id\":\"game.hull\",\"schema_version\":1,\"tick\":0,"
                     "\"entity_ids\":[");
    for (i = 0; i < SHIPS; i++) {
        put(&g->fixture, "%s%u", i > 0u ? "," : "", (unsigned)(i + 1u));
    }
    put(&g->fixture, "],\"columns\":{");
    for (k = 0; k < sizeof cols / sizeof cols[0]; k++) {
        put(&g->fixture, "%s\"%s\":[", k > 0u ? "," : "", cols[k]);
        for (i = 0; i < SHIPS; i++) {
            put(&g->fixture, "%s0", i > 0u ? "," : "");
        }
        put(&g->fixture, "]");
    }
    put(&g->fixture, "}}");
    g->caps[0].capsule_json = (const uint8_t *)g->record.p;
    g->caps[0].capsule_len = g->record.n;
    g->caps[1].capsule_json = (const uint8_t *)g->clear_cap.p;
    g->caps[1].capsule_len = g->clear_cap.n;
    g->caps[1].lir_json = (const uint8_t *)g->clear_lir.p;
    g->caps[1].lir_len = g->clear_lir.n;
    g->desc.struct_size = sizeof g->desc;
    g->desc.capsule_count = 2u;
    g->desc.capsules = g->caps;
    g->desc.fixture_json = (const uint8_t *)g->fixture.p;
    g->desc.fixture_len = g->fixture.n;
    g->desc.max_entities = CAP;
    g->desc.tick_rate_hz = 20u;
    g->desc.seed = 139u;
}

static void game_free(Game *g) {
    free(g->record.p);
    free(g->clear_cap.p);
    free(g->clear_lir.p);
    free(g->fixture.p);
}

static SuHostWorld *game_world(const Game *g) {
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t st = su_host_world_create(&g->desc, &w, &err);
    check(st == SU_OK, "world create: %s (%s)", su_host_status_name(st), err.message);
    return st == SU_OK ? w : NULL;
}

/* ---------- reference model ---------- */

#define REF_MAX 256u

typedef struct RefEntry {
    uint64_t entity, sub;
    uint32_t len;
    uint8_t v[64];
} RefEntry;

typedef struct Ref {
    RefEntry e[REF_MAX];
    uint32_t n;
} Ref;

static int ref_cmp(const RefEntry *a, uint64_t entity, uint64_t sub) {
    return a->entity != entity ? (a->entity < entity ? -1 : 1) : a->sub != sub ? (a->sub < sub ? -1 : 1) : 0;
}

static RefEntry *ref_find(Ref *r, uint64_t entity, uint64_t sub) {
    uint32_t i;
    for (i = 0; i < r->n; i++) {
        if (ref_cmp(&r->e[i], entity, sub) == 0) {
            return &r->e[i];
        }
    }
    return NULL;
}

static uint64_t ref_bytes(const Ref *r) {
    uint64_t b = 0;
    uint32_t i;
    for (i = 0; i < r->n; i++) {
        b += r->e[i].len;
    }
    return b;
}

static RefEntry *ref_put(Ref *r, uint64_t entity, uint64_t sub, uint32_t len) {
    RefEntry *f = ref_find(r, entity, sub);
    uint32_t i = 0;
    if (f != NULL) {
        f->len = len;
        return f;
    }
    while (i < r->n && ref_cmp(&r->e[i], entity, sub) < 0) {
        i++;
    }
    memmove(&r->e[i + 1u], &r->e[i], (r->n - i) * sizeof r->e[0]);
    r->n++;
    memset(&r->e[i], 0, sizeof r->e[i]);
    r->e[i].entity = entity;
    r->e[i].sub = sub;
    r->e[i].len = len;
    return &r->e[i];
}

static void ref_erase(Ref *r, uint64_t entity, uint64_t sub, int whole_entity) {
    uint32_t i = 0;
    while (i < r->n) {
        if (r->e[i].entity == entity && (whole_entity || r->e[i].sub == sub)) {
            memmove(&r->e[i], &r->e[i + 1u], (r->n - i - 1u) * sizeof r->e[0]);
            r->n--;
        } else {
            i++;
        }
    }
}

/* Inputs of tick t (scripted): one damage hit per tick, an erase every 9th tick, an entity erase at tick 13. */
typedef struct Hit {
    uint32_t slot, hit, amt;
} Hit;

static uint32_t script(uint64_t t, Hit *h) {
    uint32_t n = 0;
    h[n].slot = (uint32_t)((t * 5u) % SHIPS);
    h[n].hit = 1u + (uint32_t)((t * 7u) % 6u);
    h[n].amt = 1u + (uint32_t)(t % 11u);
    n++;
    if (t % 9u == 4u) {
        h[n].slot = (uint32_t)((t * 5u + 1u) % SHIPS);
        h[n].hit = 1u + (uint32_t)(t % 6u);
        h[n].amt = CMD_ERASE;
        n++;
    }
    if (t == 13u) {
        h[n].slot = 2u;
        h[n].hit = 1u;
        h[n].amt = CMD_ERASE_ENTITY;
        n++;
    }
    return n;
}

static uint64_t handle_at(const SuHostWorld *w, uint32_t slot) {
    uint64_t e = 0;
    (void)su_host_entity_at(w, slot, &e);
    return e;
}

static void push_script(SuHostWorld *w, uint64_t t) {
    Hit h[4];
    SuHostInput in[8];
    uint32_t n = script(t, h), i;
    SuHostError err;
    for (i = 0; i < n; i++) {
        in[2u * i].entity = handle_at(w, h[i].slot);
        in[2u * i].field_id = F_HIT;
        in[2u * i].value = h[i].hit;
        in[2u * i + 1u].entity = in[2u * i].entity;
        in[2u * i + 1u].field_id = F_AMT;
        in[2u * i + 1u].value = h[i].amt;
    }
    check(su_host_input_push(w, t, in, 2u * n, &err) == SU_OK, "push tick %llu: %s", (unsigned long long)t,
          err.message);
}

/* The model's tick t (order 0 semantics; all entities alive, generation 1). */
static void ref_tick(Ref *r, uint64_t t) {
    Hit h[4];
    uint32_t n = script(t, h), i, k;
    for (k = 0; k < SHIPS; k++) {
        for (i = 0; i < n; i++) {
            uint64_t e = (uint64_t)h[i].slot | (UINT64_C(1) << 32);
            RefEntry *f;
            if (h[i].slot != k) {
                continue;
            }
            if (h[i].amt == CMD_ERASE) {
                ref_erase(r, e, h[i].hit - 1u, 0);
            } else if (h[i].amt == CMD_ERASE_ENTITY) {
                ref_erase(r, e, 0, 1);
            } else {
                f = ref_find(r, e, h[i].hit - 1u);
                if (f == NULL) {
                    f = ref_put(r, e, h[i].hit - 1u, brick_len(h[i].hit - 1u));
                }
                f->v[0]++;
                f->v[1u + t % (f->len - 2u)] = (uint8_t)(f->v[1u + t % (f->len - 2u)] + h[i].amt);
            }
        }
    }
    {
        uint64_t slot = t % CAP;
        if (slot < SHIPS) {
            RefEntry *f = ref_put(r, slot | (UINT64_C(1) << 32), HEAT_SUB, heat_len(t));
            for (k = 0; k < f->len; k++) {
                f->v[k] = (uint8_t)(t + k);
            }
        }
        if (t % 7u == 0u && (t + 3u) % CAP < SHIPS) {
            ref_erase(r, ((t + 3u) % CAP) | (UINT64_C(1) << 32), HEAT_SUB, 0);
        }
    }
}

static int state_equals_ref(const SuHostState *s, const Ref *r, const char *what) {
    uint32_t i;
    SuHostStateInfo info;
    int ok = su_host_state_info(s, &info) == SU_OK && info.entries == r->n && info.bytes == ref_bytes(r);
    for (i = 0; ok && i < r->n; i++) {
        SuHostStateEntry en;
        ok = su_host_state_entry(s, i, &en) == SU_OK && en.entity == r->e[i].entity && en.sub == r->e[i].sub &&
             en.len == r->e[i].len && memcmp(en.data, r->e[i].v, en.len) == 0;
    }
    check(ok, "%s: state equals the model (%u entries)", what, (unsigned)r->n);
    return ok;
}

static int columns_equal_ref(const SuHostWorld *w, const Ref *r, const char *what) {
    uint32_t s, i;
    int ok = 1;
    for (s = 0; s < SHIPS && ok; s++) {
        uint32_t n = 0, bytes = 0, sum = 0, got[3] = {0, 0, 0};
        uint64_t e = (uint64_t)s | (UINT64_C(1) << 32);
        for (i = 0; i < r->n; i++) {
            if (r->e[i].entity == e) {
                uint32_t b;
                n++;
                bytes += r->e[i].len;
                for (b = 0; b + 1u < r->e[i].len; b++) {
                    sum += r->e[i].v[b];
                }
            }
        }
        ok = su_host_read(w, F_BRICKS, s, 1u, &got[0]) == SU_OK && su_host_read(w, F_BYTES, s, 1u, &got[1]) == SU_OK &&
             su_host_read(w, F_SUM, s, 1u, &got[2]) == SU_OK && got[0] == n && got[1] == bytes && got[2] == sum;
    }
    check(ok, "%s: output columns equal the model", what);
    return ok;
}

/* ---------- snapshot helpers ---------- */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static uint8_t *snapshot(const SuHostWorld *w, size_t *len) {
    size_t n = 0;
    uint8_t *b;
    check(su_host_snapshot_size(w, &n) == SU_OK, "snapshot size");
    b = malloc(n + 64u);
    check(b != NULL && su_host_snapshot_write(w, b, n, len) == SU_OK && *len == n, "snapshot write");
    return b;
}

/* Offset of the u64 operator state length (the section follows it). */
static size_t opstate_len_at(const uint8_t *snap) {
    uint64_t state_len = rd64(snap + 52);
    uint64_t log_len = rd64(snap + 60 + state_len);
    return (size_t)(60u + state_len + 8u + log_len);
}

static void digest(const SuHostWorld *w, uint8_t out[32]) {
    check(su_host_digest(w, out) == SU_OK, "digest");
}

static uint64_t fast(const SuHostWorld *w) {
    uint64_t v = 0;
    check(su_host_digest_fast(w, &v) == SU_OK, "fast digest");
    return v;
}

static void step(SuHostWorld *w, uint64_t ticks) {
    uint64_t done = 0;
    SuHostError err;
    uint32_t st = su_host_step(w, ticks, &done, &err);
    check(st == SU_OK && done == ticks, "step %llu: %s %s", (unsigned long long)ticks, su_host_status_name(st),
          err.message);
}

static const SuHostState *view(const SuHostWorld *w) {
    const SuHostState *s = NULL;
    check(su_host_operator_state(w, 0u, &s) == SU_OK && s != NULL, "state view");
    return s;
}

/* ---------- 1. registration and basics ---------- */

static void test_registration(void) {
    SuHostOperatorDesc d;
    SuHostError err;
    hull_desc(&d, "game.bad1", 0u, V_SAME);
    d.state_max_bytes = 0u;
    check(su_host_register_operator(&d, &err) == SU_E_INVALID_ARGUMENT, "entries without bytes refused");
    hull_desc(&d, "game.bad2", 0u, V_SAME);
    d.state_max_entries = 0u;
    check(su_host_register_operator(&d, &err) == SU_E_INVALID_ARGUMENT, "bytes without entries refused");
    hull_desc(&d, "game.bad3", 0u, V_NOSTATE);
    d.state_flags = SU_HOST_STATE_DROP_DEAD;
    check(su_host_register_operator(&d, &err) == SU_E_INVALID_ARGUMENT, "flags without state refused");
    hull_desc(&d, "game.bad4", 0u, V_SAME);
    d.state_flags = 2u;
    check(su_host_register_operator(&d, &err) == SU_E_INVALID_ARGUMENT, "unknown state flag refused");
    hull_desc(&d, "game.bad5", 0u, V_SAME);
    d.state_reserved = 1u;
    check(su_host_register_operator(&d, &err) == SU_E_INVALID_ARGUMENT, "state_reserved refused");
    hull_desc(&d, "game.bad6", 0u, V_SAME);
    d.state_max_entries = SU_HOST_STATE_MAX_ENTRIES + 1u;
    check(su_host_register_operator(&d, &err) == SU_E_INVALID_ARGUMENT, "too many entries refused");
    hull_desc(&d, "game.bad7", 0u, V_SAME);
    d.state_max_bytes = SU_HOST_STATE_MAX_BYTES + 1u;
    check(su_host_register_operator(&d, &err) == SU_E_INVALID_ARGUMENT, "too many bytes refused");
    check(SU_HOST_OPERATOR_DESC_V2_SIZE == (sizeof(void *) == 8u ? 136u : 100u) &&
              SU_HOST_OPERATOR_DESC_V3_SIZE == (sizeof(void *) == 8u ? 144u : 108u) &&
              SU_HOST_OPERATOR_DESC_V4_SIZE == sizeof(SuHostOperatorDesc) &&
              SU_HOST_OPERATOR_DESC_V4_SIZE == (sizeof(void *) == 8u ? 160u : 120u),
          "descriptor sizes: v2 ends at state_reserved, v3 at event_reserved, v4 is the library's");
}

static void register_ops(void) {
    SuHostOperatorDesc d;
    SuHostError err;
    hull_desc(&d, "game.hull", 0u, V_SAME);
    check(su_host_register_operator(&d, &err) == SU_OK, "register game.hull: %s", err.message);
    hull_desc(&d, "game.hull_dd", SU_HOST_STATE_DROP_DEAD, V_SAME);
    check(su_host_register_operator(&d, &err) == SU_OK, "register game.hull_dd: %s", err.message);
    hull_desc(&d, "game.hull_plain", 0u, V_NOSTATE);
    check(su_host_register_operator(&d, &err) == SU_OK, "register game.hull_plain: %s", err.message);
}

static void test_basics(const Game *g, const Game *plain) {
    SuHostWorld *w = game_world(g), *p = game_world(plain);
    const SuHostState *s;
    const SuHostState *none = (const SuHostState *)1;
    SuHostStateInfo info;
    uint8_t *snap;
    size_t len = 0;
    const void *data = (const void *)1;
    uint32_t dlen = 7u;
    if (w == NULL || p == NULL) {
        return;
    }
    s = view(w);
    check(su_host_state_info(s, &info) == SU_OK && info.entries == 0u && info.bytes == 0u &&
              info.max_entries == MAX_ENTRIES && info.max_bytes == MAX_BYTES && info.flags == 0u && info.writable == 0u,
          "empty state, declared caps, read-only view");
    check(su_host_state_get(s, 1u, 2u, &data, &dlen) == SU_OK && data == NULL && dlen == 0u, "get of a missing key");
    check(su_host_operator_state(w, 1u, &none) == SU_E_INVALID_ARGUMENT && none == NULL, "IR system has no state");
    check(su_host_operator_state(p, 0u, &none) == SU_E_INVALID_ARGUMENT, "operator without state has none");
    {
        /* su_host_operator_counters (issue 163) is for built-in query operators only. */
        SuHostOperatorCounters k;
        memset(&k, 0, sizeof k);
        k.struct_size = sizeof k;
        check(su_host_operator_counters(w, 0u, &k) == SU_E_UNSUPPORTED_METHOD, "registered operator has no counters");
        check(su_host_operator_counters(w, 1u, &k) == SU_E_INVALID_ARGUMENT, "IR system has no counters");
    }
    snap = snapshot(w, &len);
    check(rd32(snap + 8) == 2u && memcmp(snap + opstate_len_at(snap) + 8u, "SUOPSTAT", 9u) == 0,
          "a world with state writes version 2 with the section");
    check(rd64(snap + opstate_len_at(snap)) == len - opstate_len_at(snap) - 8u, "section length covers the rest");
    free(snap);
    snap = snapshot(p, &len);
    check(rd32(snap + 8) == 1u && opstate_len_at(snap) == len, "a world without state writes version 1 exactly");
    free(snap);
    su_host_world_destroy(w);
    su_host_world_destroy(p);
}

/* ---------- 2. mutation, snapshot, restore, re-simulation ---------- */

static void test_rollback(const Game *g) {
    SuHostWorld *w = game_world(g);
    Ref ref;
    uint8_t d[41][32], dd[32];
    uint64_t fd[41];
    uint8_t *snap10;
    size_t len10 = 0;
    uint64_t t;
    int pass;
    char what[64];
    if (w == NULL) {
        return;
    }
    memset(&ref, 0, sizeof ref);
    digest(w, d[0]);
    fd[0] = fast(w);
    snap10 = NULL;
    for (t = 0; t < 40u; t++) {
        if (t == 10u) {
            snap10 = snapshot(w, &len10);
        }
        push_script(w, t);
        step(w, 1u);
        ref_tick(&ref, t);
        snprintf(what, sizeof what, "tick %llu", (unsigned long long)(t + 1u));
        state_equals_ref(view(w), &ref, what);
        columns_equal_ref(w, &ref, what);
        digest(w, d[t + 1u]);
        fd[t + 1u] = fast(w);
        check(memcmp(d[t + 1u], d[t], 32u) != 0, "digest moves at tick %llu", (unsigned long long)(t + 1u));
    }
    check(su_host_state_count(view(w)) > 10u, "state has entries (%u)", (unsigned)su_host_state_count(view(w)));
    /* Roll back over 30 ticks that changed the state, both restores, twice each (in-place: the second call reuses its
     * scratch), then re-simulate with the same inputs. */
    for (pass = 0; pass < 4; pass++) {
        SuHostError err;
        uint32_t st = (pass & 1) == 0 ? su_host_snapshot_restore(w, snap10, len10, &err)
                                      : su_host_snapshot_restore_inplace(w, snap10, len10, &err);
        check(st == SU_OK, "restore pass %d: %s", pass, err.message);
        digest(w, dd);
        check(su_host_tick(w) == 10u && memcmp(dd, d[10], 32u) == 0 && fast(w) == fd[10],
              "restored state digests as at tick 10 (pass %d)", pass);
        for (t = 10u; t < 40u; t++) {
            push_script(w, t);
            step(w, 1u);
            digest(w, dd);
            check(memcmp(dd, d[t + 1u], 32u) == 0 && fast(w) == fd[t + 1u],
                  "re-simulated tick %llu equals the first run (pass %d)", (unsigned long long)(t + 1u), pass);
        }
        state_equals_ref(view(w), &ref, "after re-simulation");
    }
    free(snap10);
    su_host_world_destroy(w);
}

/* ---------- 3. digests see the state ---------- */

static void test_digest_sees_state(const Game *g) {
    SuHostWorld *a = game_world(g), *b = game_world(g);
    uint8_t da[32], db[32];
    uint64_t t;
    int cols_equal = 1;
    uint32_t f, s;
    if (a == NULL || b == NULL) {
        return;
    }
    for (t = 0; t < 12u; t++) {
        push_script(a, t);
        push_script(b, t);
        step(a, 1u);
        if (t == 11u) {
            g_hidden = 1; /* b's last tick also flips the last byte of slot 0's values: no column shows it */
        }
        step(b, 1u);
        g_hidden = 0;
    }
    for (f = F_HIT; f <= F_ERR; f++) {
        for (s = 0; s < CAP; s++) {
            uint32_t va = 0, vb = 0;
            (void)su_host_read(a, f, s, 1u, &va);
            (void)su_host_read(b, f, s, 1u, &vb);
            cols_equal &= va == vb;
        }
    }
    check(cols_equal, "columns are equal");
    check(su_host_state_count(view(a)) == su_host_state_count(view(b)), "same keys");
    digest(a, da);
    digest(b, db);
    check(memcmp(da, db, 32u) != 0, "su_host_digest differs when only the state differs");
    check(fast(a) != fast(b), "su_host_digest_fast differs when only the state differs");
    su_host_world_destroy(a);
    su_host_world_destroy(b);
}

/* ---------- 4. canonicality ---------- */

static void test_canonical(const Game *g) {
    SuHostWorld *a = game_world(g), *b = game_world(g);
    uint8_t *sa, *sb;
    size_t la = 0, lb = 0;
    uint64_t t, placed_b;
    if (a == NULL || b == NULL) {
        return;
    }
    placed_b = 0;
    for (t = 0; t < 300u; t++) {
        uint64_t p0;
        push_script(a, t);
        push_script(b, t);
        g_order = 0;
        step(a, 1u);
        g_order = 1;
        p0 = g_placed;
        step(b, 1u);
        placed_b += g_placed - p0;
        g_order = 0;
    }
    sa = snapshot(a, &la);
    sb = snapshot(b, &lb);
    check(la == lb && memcmp(sa, sb, la) == 0, "other mutation orders, churn and compactions give identical bytes");
    check(placed_b > 2u * MAX_BYTES, "the reordered run placed %llu value bytes, more than an arena (2 x max): "
                                     "compactions ran",
          (unsigned long long)placed_b);
    free(sa);
    free(sb);
    su_host_world_destroy(a);
    su_host_world_destroy(b);
}

/* ---------- 5. caps ---------- */

static void push_cmd(SuHostWorld *w, uint32_t slot, uint32_t hit, uint32_t amt) {
    SuHostInput in[2];
    SuHostError err;
    in[0].entity = handle_at(w, slot);
    in[0].field_id = F_HIT;
    in[0].value = hit;
    in[1].entity = in[0].entity;
    in[1].field_id = F_AMT;
    in[1].value = amt;
    check(su_host_input_push(w, su_host_tick(w), in, 2u, &err) == SU_OK, "push command: %s", err.message);
}

/* w and twin have taken the same successful ticks: a failed tick must not leak into later ones. */
static void same_as_twin(SuHostWorld *w, SuHostWorld *twin, const char *what) {
    uint8_t a[32], b[32];
    push_cmd(w, 7u, 3u, 9u);
    push_cmd(twin, 7u, 3u, 9u);
    step(w, 1u);
    step(twin, 1u);
    digest(w, a);
    digest(twin, b);
    check(memcmp(a, b, 32u) == 0, "%s: the next tick equals a world that never ran the failed tick", what);
}

static void test_caps(const Game *g) {
    SuHostWorld *w = game_world(g), *twin = game_world(g);
    uint8_t before[32], after[32];
    uint64_t done = 99;
    SuHostError err;
    uint32_t st, e0, v = 0;
    SuHostStateInfo info;
    uint64_t t;
    if (w == NULL || twin == NULL) {
        return;
    }
    /* 30 ticks: copy-on-write and heat replacements leave free gaps between the committed values, so a compaction
     * that ran in the committed arena would move committed bytes. */
    for (t = 0; t < 30u; t++) {
        push_script(w, t);
        push_script(twin, t);
        step(w, 1u);
        step(twin, 1u);
    }
    /* Many replacements of one key in one tick (19,000+ bytes through a 4,096-byte arena, with copy and in-place
     * compactions) are never refused: only the logical size counts. A failing churn leaves everything as it was. */
    digest(w, before);
    push_cmd(w, 1u, 2u, CMD_CHURN_FAIL);
    st = su_host_step(w, 1u, &done, &err);
    check(st == SU_E_CANDIDATE_FAILED, "failing churn fails the tick (%s)", su_host_status_name(st));
    digest(w, after);
    check(memcmp(before, after, 32u) == 0, "failed churn left the committed state intact");
    su_host_input_clear(w, 0u);
    same_as_twin(w, twin, "failed churn");
    {
        /* Every entry but the churned key and the heat entries must come out of the churn's compactions intact. */
        static SuHostStateEntry keep[MAX_ENTRIES];
        static uint8_t bytes[2u * MAX_BYTES];
        const SuHostState *vs = view(w);
        uint32_t nk = 0, k, intact = 1;
        size_t used = 0;
        uint64_t churned = handle_at(w, 1u);
        for (k = 0; k < su_host_state_count(vs); k++) {
            SuHostStateEntry en;
            if (su_host_state_entry(vs, k, &en) != SU_OK || en.sub == HEAT_SUB || (en.entity == churned && en.sub == 1u)) {
                continue;
            }
            memcpy(bytes + used, en.data, en.len);
            en.data = bytes + used;
            used += en.len;
            keep[nk++] = en;
        }
        push_cmd(w, 1u, 2u, CMD_CHURN);
        push_cmd(twin, 1u, 2u, CMD_CHURN);
        step(w, 1u);
        step(twin, 1u);
        for (k = 0; k < nk; k++) {
            const void *data = NULL;
            uint32_t len = 0;
            intact &= su_host_state_get(vs, keep[k].entity, keep[k].sub, &data, &len) == SU_OK && data != NULL &&
                      len == keep[k].len && memcmp(data, keep[k].data, len) == 0;
        }
        check(intact && nk > 10u, "churn compactions kept the other %u entries byte for byte", (unsigned)nk);
        /* Shuffle on slot 1: its other bricks come back inverted, everything else intact, through copy and in-place
         * compactions of values that are no longer in key order. */
        nk = 0;
        used = 0;
        for (k = 0; k < su_host_state_count(vs); k++) {
            SuHostStateEntry en;
            if (su_host_state_entry(vs, k, &en) != SU_OK || en.sub == HEAT_SUB || (en.entity == churned && en.sub == 1u)) {
                continue;
            }
            memcpy(bytes + used, en.data, en.len);
            if (en.entity == churned) {
                uint32_t q;
                for (q = 0; q < en.len; q++) {
                    bytes[used + q] = (uint8_t)~bytes[used + q];
                }
                memset(bytes + used + en.len, 0xEE, 8u);
                en.len += 8u;
            }
            en.data = bytes + used;
            used += en.len;
            keep[nk++] = en;
        }
        push_cmd(w, 1u, 2u, CMD_SHUFFLE);
        push_cmd(twin, 1u, 2u, CMD_SHUFFLE);
        step(w, 1u);
        step(twin, 1u);
        intact = 1;
        for (k = 0; k < nk; k++) {
            const void *data = NULL;
            uint32_t len = 0;
            intact &= su_host_state_get(vs, keep[k].entity, keep[k].sub, &data, &len) == SU_OK && data != NULL &&
                      len == keep[k].len && memcmp(data, keep[k].data, len) == 0;
        }
        check(intact && su_host_state_lower_bound(vs, churned, 0u) + 2u < su_host_state_lower_bound(vs, churned + 1u, 0u),
              "shuffle: slot 1's bricks inverted, the other entries intact (%u)", (unsigned)nk);
    }
    {
        const void *data = NULL;
        uint32_t len = 0;
        check(su_host_state_get(view(w), handle_at(w, 1u), 1u, &data, &len) == SU_OK && len == 8u && data != NULL &&
                  memcmp(data, "\x5a\x5a\x5a\x5a\x5a\x5a\x5a\x5a", 8u) == 0,
              "churned key holds the last value");
    }
    /* Strict flood: the run returns the refusal, the tick fails, nothing changes (an earlier row's damage too). */
    digest(w, before);
    e0 = su_host_state_count(view(w));
    t = su_host_tick(w);
    push_cmd(w, 0u, 1u, 3u);
    push_cmd(w, 5u, 200u, CMD_FLOOD);
    st = su_host_step(w, 1u, &done, &err);
    check(st == SU_E_RESOURCE_LIMIT && done == 0u && err.index == 0u && err.tick == t,
          "flood past the cap fails the tick (%s, index %u)", su_host_status_name(st), (unsigned)err.index);
    check(strstr(err.message, "operator state cap reached") != NULL && strstr(err.message, "SU_E_RESOURCE_LIMIT"),
          "message says the state cap was reached: %s", err.message);
    digest(w, after);
    check(memcmp(before, after, 32u) == 0 && su_host_tick(w) == t && su_host_state_count(view(w)) == e0,
          "world unchanged after the refused tick");
    su_host_input_clear(w, 0u);
    same_as_twin(w, twin, "refused flood");
    /* A run that saw refusals but fails with another code: the message does not blame the cap. */
    push_cmd(w, 5u, 200u, CMD_FLOOD_FAIL);
    st = su_host_step(w, 1u, &done, &err);
    check(st == SU_E_CANDIDATE_FAILED && strstr(err.message, "cap reached") == NULL,
          "another failure code is not reported as the cap: %s", err.message);
    su_host_input_clear(w, 0u);
    /* Lenient flood: the operator handles the refusal; the state holds what fitted, exactly at a cap. */
    push_cmd(w, 5u, 200u, CMD_FLOOD_LENIENT);
    step(w, 1u);
    (void)su_host_read(w, F_ERR, 5u, 1u, &v);
    check(v == SU_E_RESOURCE_LIMIT, "lenient flood saw SU_E_RESOURCE_LIMIT (%u)", (unsigned)v);
    check(su_host_state_info(view(w), &info) == SU_OK &&
              (info.entries == MAX_ENTRIES || info.bytes > MAX_BYTES - 32u),
          "state stopped at a cap (%u entries, %llu bytes)", (unsigned)info.entries,
          (unsigned long long)info.bytes);
    /* Fill the bytes to the cap with 64-byte values, then replace one of them 300 times: at the cap a replacement of
     * equal size still fits (the compaction drops the value being replaced). */
    push_cmd(w, 5u, 1u, CMD_ERASE_ENTITY);
    step(w, 1u);
    push_cmd(w, 6u, 1u, CMD_FILL);
    step(w, 1u);
    (void)su_host_read(w, F_ERR, 6u, 1u, &v);
    check(v == SU_E_RESOURCE_LIMIT && su_host_state_info(view(w), &info) == SU_OK && info.bytes > MAX_BYTES - 64u,
          "fill stopped at the byte cap (%llu bytes)", (unsigned long long)info.bytes);
    {
        /* Slot 6's entries: same keys, 56 bytes less (sub 2000 ends as 8 bytes); the tick's heat may add one entry
         * elsewhere now that there is room. */
        uint64_t e6 = handle_at(w, 6u);
        uint32_t n6, n6b, k6;
        uint64_t b6 = 0, b6b = 0;
        const void *data = NULL;
        uint32_t len = 0;
        const SuHostState *vs = view(w);
        n6 = su_host_state_lower_bound(vs, e6 + 1u, 0u) - su_host_state_lower_bound(vs, e6, 0u);
        for (k6 = su_host_state_lower_bound(vs, e6, 0u); k6 < su_host_state_lower_bound(vs, e6 + 1u, 0u); k6++) {
            SuHostStateEntry en;
            (void)su_host_state_entry(vs, k6, &en);
            b6 += en.len;
        }
        push_cmd(w, 6u, 2001u, CMD_CHURN);
        step(w, 1u);
        n6b = su_host_state_lower_bound(vs, e6 + 1u, 0u) - su_host_state_lower_bound(vs, e6, 0u);
        for (k6 = su_host_state_lower_bound(vs, e6, 0u); k6 < su_host_state_lower_bound(vs, e6 + 1u, 0u); k6++) {
            SuHostStateEntry en;
            (void)su_host_state_entry(vs, k6, &en);
            b6b += en.len;
        }
        check(su_host_state_get(vs, e6, 2000u, &data, &len) == SU_OK && len == 8u && n6b == n6 && b6b == b6 - 56u,
              "churn at the byte cap succeeded");
    }
    {
        /* With the live state at the cap, 100 ticks that each rewrite one 64-byte value (plus the heat) compact only
         * when the arena's slack is used up: about once per state_max_bytes written, not once per tick. */
        SuHostStateInfo i0, i1;
        uint32_t k;
        (void)su_host_state_info(view(w), &i0);
        for (k = 0; k < 100u; k++) {
            push_cmd(w, 6u, 2002u + k % 40u, 7u);
            step(w, 1u);
        }
        (void)su_host_state_info(view(w), &i1);
        check(i0.bytes > MAX_BYTES - 128u && i1.compactions - i0.compactions <= 4u,
              "near the cap, 100 changing ticks compacted %llu times (live %llu of %u bytes)",
              (unsigned long long)(i1.compactions - i0.compactions), (unsigned long long)i0.bytes,
              (unsigned)MAX_BYTES);
    }
    su_host_world_destroy(w);
    su_host_world_destroy(twin);
}

/* ---------- 6. corrupted state sections ---------- */

typedef struct Variant {
    const char *what;
    uint8_t *b;
    size_t len;
} Variant;

/* The body of the only system's state: section + 9 magic + 4 version + 4 count + 4 index. */
static size_t body_at(const uint8_t *snap) {
    return opstate_len_at(snap) + 8u + 21u;
}

static uint8_t *copy_of(const uint8_t *s, size_t len, size_t extra) {
    uint8_t *b = malloc(len + extra + 1u);
    memcpy(b, s, len);
    return b;
}

static void test_corrupt(const Game *g) {
    SuHostWorld *w = game_world(g);
    uint8_t *snap, *b;
    size_t len = 0, bo, ol;
    uint8_t before[32], after[32];
    Variant v[20];
    uint32_t nv = 0, i, n;
    uint64_t t;
    if (w == NULL) {
        return;
    }
    for (t = 0; t < 20u; t++) {
        push_script(w, t);
        step(w, 1u);
    }
    snap = snapshot(w, &len);
    bo = body_at(snap);
    ol = opstate_len_at(snap);
    n = rd32(snap + bo);
    check(n >= 3u, "enough entries to corrupt (%u)", (unsigned)n);
    step(w, 3u); /* the world moves on, so a refused restore is visible as "unchanged" */
    digest(w, before);

    v[nv].what = "truncated by one byte";
    v[nv].b = copy_of(snap, len, 0);
    v[nv++].len = len - 1u;
    v[nv].what = "one trailing byte";
    v[nv].b = copy_of(snap, len, 1);
    v[nv].b[len] = 0;
    v[nv++].len = len + 1u;
    v[nv].what = "keys out of order";
    b = copy_of(snap, len, 0);
    {
        uint8_t k[16];
        memcpy(k, b + bo + 12u, 16u);
        memcpy(b + bo + 12u, b + bo + 12u + 20u, 16u);
        memcpy(b + bo + 12u + 20u, k, 16u);
    }
    v[nv].b = b;
    v[nv++].len = len;
    v[nv].what = "duplicate key";
    b = copy_of(snap, len, 0);
    memcpy(b + bo + 12u + 20u, b + bo + 12u, 16u);
    v[nv].b = b;
    v[nv++].len = len;
    v[nv].what = "zero length";
    b = copy_of(snap, len, 0);
    wr32(b + bo + 12u + 20u + 16u, rd32(b + bo + 12u + 20u + 16u) + rd32(b + bo + 12u + 16u));
    wr32(b + bo + 12u + 16u, 0u);
    v[nv].b = b;
    v[nv++].len = len;
    v[nv].what = "lengths disagree with the total";
    b = copy_of(snap, len, 0);
    wr32(b + bo + 12u + 16u, rd32(b + bo + 12u + 16u) + 1u);
    v[nv].b = b;
    v[nv++].len = len;
    {
        /* A well-formed section of state_max_entries + 1 one-byte entries (well within state_max_bytes). */
        uint32_t k, ne = MAX_ENTRIES + 1u;
        size_t body = 12u + 20u * ne + ne, nl = ol + 8u + 21u + body;
        b = calloc(1u, nl);
        memcpy(b, snap, ol);
        wr64(b + ol, 21u + body);
        memcpy(b + ol + 8u, snap + ol + 8u, 21u);
        wr32(b + ol + 8u + 21u, ne);
        wr64(b + ol + 8u + 25u, ne);
        for (k = 0; k < ne; k++) {
            uint8_t *q = b + ol + 8u + 33u + 20u * k;
            wr64(q, handle_at(w, 0u));
            wr64(q + 8u, k);
            wr32(q + 16u, 1u);
        }
        v[nv].what = "a well-formed section with more entries than declared";
        v[nv].b = b;
        v[nv++].len = nl;
    }
    v[nv].what = "more entries than declared";
    b = copy_of(snap, len, 0);
    wr32(b + bo, MAX_ENTRIES + 1u);
    v[nv].b = b;
    v[nv++].len = len;
    v[nv].what = "section magic";
    b = copy_of(snap, len, 0);
    b[ol + 8u + 2u] ^= 1u;
    v[nv].b = b;
    v[nv++].len = len;
    v[nv].what = "system index";
    b = copy_of(snap, len, 0);
    wr32(b + bo - 4u, 1u);
    v[nv].b = b;
    v[nv++].len = len;
    v[nv].what = "snapshot version 1";
    b = copy_of(snap, len, 0);
    wr32(b + 8, 1u);
    v[nv].b = b;
    v[nv++].len = len;
    v[nv].what = "section length one short (truncated section)";
    b = copy_of(snap, len, 0);
    wr64(b + ol, rd64(b + ol) - 1u);
    v[nv].b = b;
    v[nv++].len = len - 1u;
    {
        /* Oversized: a well-formed section holding one value of state_max_bytes + 1 bytes. */
        size_t body = 12u + 20u + MAX_BYTES + 1u, nl = ol + 8u + 21u + body;
        b = calloc(1u, nl);
        memcpy(b, snap, ol);
        wr64(b + ol, 21u + body);
        memcpy(b + ol + 8u, snap + ol + 8u, 21u);
        wr32(b + ol + 8u + 21u, 1u);
        wr64(b + ol + 8u + 25u, MAX_BYTES + 1u);
        wr64(b + ol + 8u + 33u, handle_at(w, 0u));
        wr64(b + ol + 8u + 41u, 0u);
        wr32(b + ol + 8u + 49u, MAX_BYTES + 1u);
        v[nv].what = "a value larger than state_max_bytes";
        v[nv].b = b;
        v[nv++].len = nl;
    }
    for (i = 0; i < nv; i++) {
        SuHostError err;
        uint32_t s1 = su_host_snapshot_restore(w, v[i].b, v[i].len, &err);
        uint32_t s2 = su_host_snapshot_restore_inplace(w, v[i].b, v[i].len, &err);
        digest(w, after);
        check(s1 == SU_E_PARSE && s2 == SU_E_PARSE && memcmp(before, after, 32u) == 0 && su_host_tick(w) == 23u,
              "%s: refused by both restores (%s, %s), world unchanged", v[i].what, su_host_status_name(s1),
              su_host_status_name(s2));
        free(v[i].b);
    }
    {
        SuHostError err;
        check(su_host_snapshot_restore_inplace(w, snap, len, &err) == SU_OK && su_host_tick(w) == 20u,
              "the original still restores");
    }
    free(snap);
    su_host_world_destroy(w);
}

/* ---------- 7. replay ---------- */

static void test_replay(const Game *g) {
    SuHostWorld *w = game_world(g), *x = game_world(g);
    SuReplayWriter *rw = NULL;
    SuReplay *rp = NULL;
    SuReplayOptions opt;
    SuReplayInfo info;
    SuHostError err;
    uint8_t *bytes = NULL, d[41][32], dd[32];
    size_t len = 0, k;
    uint64_t t, done = 0;
    uint32_t st;
    if (w == NULL || x == NULL) {
        return;
    }
    memset(&opt, 0, sizeof opt);
    opt.struct_size = sizeof opt;
    opt.keyframe_interval = 5u;
    opt.digest_interval = 1u;
    check(su_replay_writer_begin(w, &opt, &rw, &err) == SU_OK, "writer begin: %s", err.message);
    digest(w, d[0]);
    for (t = 0; t < 40u; t++) {
        push_script(w, t);
        check(su_replay_writer_step(rw, w, 1u, &done, &err) == SU_OK && done == 1u, "writer step %llu",
              (unsigned long long)t);
        digest(w, d[t + 1u]);
    }
    check(su_replay_writer_finish(rw, w, &bytes, &len) == SU_OK, "writer finish");
    su_replay_writer_destroy(rw);
    check(su_replay_open(bytes, len, &rp, &err) == SU_OK, "replay open: %s", err.message);
    check(su_replay_info(rp, &info) == SU_OK && info.keyframe_count == 9u && info.end_tick == 40u, "replay info");
    st = su_replay_seek(rp, x, 23u, &err);
    digest(x, dd);
    check(st == SU_OK && memcmp(dd, d[23], 32u) == 0, "seek to 23 (keyframe 20 + 3 ticks): %s", err.message);
    st = su_replay_seek(rp, x, 7u, &err);
    check(st == SU_OK, "seek to 7");
    st = su_replay_play(rp, x, 100u, &done, &err);
    digest(x, dd);
    check(st == SU_OK && done == 33u && memcmp(dd, d[40], 32u) == 0, "play to the end: %s", err.message);
    st = su_replay_seek(rp, x, 35u, &err);
    digest(x, dd);
    check(st == SU_OK && memcmp(dd, d[35], 32u) == 0, "seek to a keyframe tick");
    su_replay_close(rp);
    /* Tamper with a value byte inside the keyframe of tick 5 (its last state section): seeking through it desyncs. */
    {
        size_t hits = 0, kf5 = 0;
        for (k = 0; k + 9u <= len; k++) {
            if (memcmp(bytes + k, "SUOPSTAT", 9u) == 0) {
                hits++;
                if (hits == 2u) {
                    kf5 = k;
                }
            }
        }
        check(hits == 9u && kf5 != 0u, "every keyframe carries a state section (%u)", (unsigned)hits);
        if (kf5 != 0u) {
            uint32_t n = rd32(bytes + kf5 + 21u);
            size_t first_value = kf5 + 21u + 12u + (size_t)n * 20u;
            bytes[first_value + 1u] ^= 0x40u;
            check(su_replay_open(bytes, len, &rp, &err) == SU_OK, "tampered replay still parses");
            st = su_replay_seek(rp, x, 7u, &err);
            check(st == SU_E_INTERNAL && err.index == SU_REPLAY_DESYNC && err.tick == 6u,
                  "a changed keyframe state byte is a desync at tick 6 (%s, tick %llu)", su_host_status_name(st),
                  (unsigned long long)err.tick);
            su_replay_close(rp);
        }
    }
    su_replay_free(bytes);
    su_host_world_destroy(w);
    su_host_world_destroy(x);
}

/* ---------- 8. dead entities ---------- */

static void test_drop_dead(const Game *dd, const Game *g) {
    SuHostWorld *w = game_world(dd), *p = game_world(g);
    SuHostError err;
    uint64_t t, e3, e3p, enew = 0;
    uint32_t v = 0, i, before, ok = 1;
    if (w == NULL || p == NULL) {
        return;
    }
    for (t = 0; t < 20u; t++) {
        push_script(w, t);
        push_script(p, t);
        step(w, 1u);
        step(p, 1u);
    }
    e3 = handle_at(w, 3u);
    e3p = handle_at(p, 3u);
    check(su_host_state_lower_bound(view(w), e3, 0u) < su_host_state_count(view(w)), "slot 3 has entries");
    before = su_host_state_count(view(w));
    check(su_host_despawn(w, e3, &err) == SU_OK && su_host_despawn(p, e3p, &err) == SU_OK, "despawn slot 3");
    check(su_host_state_count(view(w)) == before, "entries stay until the next tick publishes");
    {
        /* A snapshot taken now holds the pending entries; after a purge, restoring it must purge them again. */
        size_t sl = 0;
        uint8_t *pending = snapshot(w, &sl);
        SuHostError e2;
        step(w, 1u);
        check(su_host_state_count(view(w)) < before + 2u &&
                  su_host_state_lower_bound(view(w), e3, 0u) == su_host_state_lower_bound(view(w), e3 + 1u, 0u),
              "purged after one tick");
        check(su_host_snapshot_restore_inplace(w, pending, sl, &e2) == SU_OK &&
                  su_host_state_lower_bound(view(w), e3, 0u) < su_host_state_lower_bound(view(w), e3 + 1u, 0u),
              "the restored snapshot has the pending entries again");
        free(pending);
    }
    push_cmd(w, 0u, 1u, CMD_STALE);
    push_cmd(p, 0u, 1u, CMD_STALE);
    step(w, 1u);
    step(p, 1u);
    for (i = 0; i < su_host_state_count(view(w)); i++) {
        SuHostStateEntry en;
        ok &= su_host_state_entry(view(w), i, &en) == SU_OK && en.entity != e3;
    }
    check(ok, "DROP_DEAD: the dead entity's entries were erased at publish");
    {
        /* Everything else is exactly what the world without the flag holds, less its dead slots' entries. */
        const SuHostState *ws = view(w), *ps = view(p);
        uint32_t kept = 0, same = 1;
        for (i = 0; i < su_host_state_count(ps); i++) {
            SuHostStateEntry pe;
            const void *data = NULL;
            uint32_t len = 0;
            if (su_host_state_entry(ps, i, &pe) != SU_OK || (uint32_t)pe.entity == 3u || (uint32_t)pe.entity >= SHIPS) {
                continue;
            }
            kept++;
            same &= su_host_state_get(ws, pe.entity, pe.sub, &data, &len) == SU_OK && data != NULL && len == pe.len &&
                    memcmp(data, pe.data, len) == 0;
        }
        check(same && kept == su_host_state_count(ws), "DROP_DEAD erased exactly the dead entities' entries (%u kept)",
              (unsigned)kept);
    }
    (void)su_host_read(w, F_ERR, 0u, 1u, &v);
    check(v == SU_E_STALE_HANDLE, "DROP_DEAD: a put for a dead slot's handle is SU_E_STALE_HANDLE (%u)", (unsigned)v);
    (void)su_host_read(p, F_ERR, 0u, 1u, &v);
    check(v == SU_OK, "without the flag the put is the operator's choice (accepted)");
    {
        const void *data = NULL;
        uint32_t len = 0;
        SuHostState const *ps = view(p);
        uint32_t lb = su_host_state_lower_bound(ps, e3p, 0u);
        SuHostStateEntry en;
        check(lb < su_host_state_count(ps) && su_host_state_entry(ps, lb, &en) == SU_OK && en.entity == e3p,
              "without the flag the dead entity's entries stay (operator responsibility)");
        (void)data;
        (void)len;
    }
    {
        /* A despawn applied by the tick itself (game.clear's despawn_self): purged at that tick's publish. */
        uint64_t e8 = handle_at(w, 8u), e8p = handle_at(p, 8u);
        uint32_t gone = 1;
        check(su_host_state_lower_bound(view(w), e8, 0u) < su_host_state_lower_bound(view(w), e8 + 1u, 0u),
              "slot 8 has entries");
        push_cmd(w, 8u, 0u, CMD_KILL);
        push_cmd(p, 8u, 0u, CMD_KILL);
        step(w, 1u);
        step(p, 1u);
        gone = su_host_entity_at(w, 8u, &enew) == SU_E_STALE_HANDLE &&
               su_host_state_lower_bound(view(w), e8, 0u) == su_host_state_lower_bound(view(w), e8 + 1u, 0u);
        check(gone, "DROP_DEAD: entries of an entity despawned inside the tick are erased at its publish");
        check(su_host_state_lower_bound(view(p), e8p, 0u) < su_host_state_lower_bound(view(p), e8p + 1u, 0u),
              "without the flag they stay");
    }
    /* A new entity in the slot has a new generation: it starts without entries. */
    check(su_host_spawn(w, NULL, 0u, 99u, &enew, &err) == SU_OK && (uint32_t)enew == 3u && enew != e3,
          "respawn in slot 3");
    step(w, 1u);
    (void)su_host_read(w, F_BRICKS, 3u, 1u, &v);
    check(v == 0u || v == 1u, "the new entity sees none of the old entries (%u, at most its heat)", (unsigned)v);
    {
        /* Snapshot round trip keeps the rule; a key with generation 0 is not a handle (refused). */
        size_t len = 0, bo;
        uint8_t *snap = snapshot(w, &len), *bad = copy_of(snap, len, 0);
        uint8_t before_d[32], after_d[32];
        bo = body_at(snap);
        wr32(bad + bo + 12u + 4u, 0u); /* generation of the first key */
        digest(w, before_d);
        check(su_host_snapshot_restore_inplace(w, bad, len, &err) == SU_E_PARSE, "generation 0 key refused");
        digest(w, after_d);
        check(memcmp(before_d, after_d, 32u) == 0, "world unchanged");
        check(su_host_snapshot_restore(w, snap, len, &err) == SU_OK, "own snapshot restores");
        free(bad);
        free(snap);
    }
    su_host_world_destroy(w);
    su_host_world_destroy(p);
}

/* ---------- 9. allocation ---------- */

static void test_alloc(const Game *g, const Game *dd) {
#ifdef SU_TEST_WRAP_ALLOC
    SuHostWorld *w = game_world(g), *x = game_world(dd);
    SuHostError err;
    uint8_t *snap;
    size_t len = 0;
    uint64_t t, placed0, placed_w, placed_x;
    if (w == NULL || x == NULL) {
        return;
    }
    step(w, 1u);
    step(x, 1u);
    snap = snapshot(w, &len);
    check(su_host_snapshot_restore_inplace(w, snap, len, &err) == SU_OK, "first in-place restore (allocates scratch)");
    (void)su_host_despawn(x, handle_at(x, 4u), &err); /* the purge runs in the counted window */
    g_allocs = 0;
    g_count_alloc = 1;
    placed0 = g_placed;
    for (t = 0; t < 600u; t++) {
        (void)su_host_step(w, 1u, NULL, &err);
    }
    placed_w = g_placed - placed0;
    placed0 = g_placed;
    for (t = 0; t < 600u; t++) {
        (void)su_host_step(x, 1u, NULL, &err);
    }
    placed_x = g_placed - placed0;
    (void)su_host_snapshot_restore_inplace(w, snap, len, &err);
    g_count_alloc = 0;
    check(g_allocs == 0u, "no allocation in 1,200 ticks with state changes, compactions, a purge and an in-place "
                          "restore (%lu)",
          g_allocs);
    check(placed_w > 2u * MAX_BYTES && placed_x > 2u * MAX_BYTES,
          "each world placed more bytes than an arena (%llu, %llu): compactions ran in the counted ticks",
          (unsigned long long)placed_w, (unsigned long long)placed_x);
    check(su_host_tick(w) == 1u && su_host_tick(x) == 601u, "worlds stepped");
    free(snap);
    su_host_world_destroy(w);
    su_host_world_destroy(x);
#else
    (void)g;
    (void)dd;
    fprintf(stderr, "allocation counting not built in this variant\n");
#endif
}

/* ---------- 10. misuse ---------- */

static void test_misuse(const Game *g) {
    SuHostWorld *w = game_world(g);
    SuHostState *s;
    void *data = NULL;
    uint32_t len = 0;
    uint64_t t;
    if (w == NULL) {
        return;
    }
    for (t = 0; t < 4u; t++) {
        push_script(w, t);
        step(w, 1u);
    }
    s = (SuHostState *)(uintptr_t)view(w); /* a view is read-only even with the const cast away */
    check(su_host_state_put(s, 1u, 1u, "x", 1u) == SU_E_INVALID_ARGUMENT &&
              su_host_state_alloc(s, 1u, 1u, 4u, &data) == SU_E_INVALID_ARGUMENT &&
              su_host_state_modify(s, 1u, 1u, &data, &len) == SU_E_INVALID_ARGUMENT &&
              su_host_state_erase(s, 1u, 1u, NULL) == SU_E_INVALID_ARGUMENT &&
              su_host_state_erase_entity(s, 1u, NULL) == SU_E_INVALID_ARGUMENT,
          "changes outside run are refused");
    check(su_host_state_put(NULL, 1u, 1u, "x", 1u) == SU_E_INVALID_ARGUMENT && su_host_state_count(NULL) == 0u,
          "NULL state");
    /* The script's tick 0 hit brick 0 of slot 0: put from that value's own memory is refused. */
    push_cmd(w, 0u, 1u, CMD_ALIAS);
    g_misuse_status = SU_OK;
    step(w, 1u);
    check(g_misuse_status == SU_E_INVALID_ARGUMENT, "put from the state's own memory refused (%s)",
          su_host_status_name(g_misuse_status));
    su_host_world_destroy(w);
}

/* ---------- cross-process ---------- */

static int write_file(const char *dir, const char *name, const uint8_t *b, size_t n) {
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    if (fwrite(b, 1u, n, f) != n) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static uint8_t *read_file(const char *dir, const char *name, size_t *n) {
    char path[1024];
    FILE *f;
    uint8_t *b;
    long sz;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        if (f != NULL) {
            fclose(f);
        }
        return NULL;
    }
    b = malloc((size_t)sz + 1u);
    if (b == NULL || fread(b, 1u, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int run_record(const char *dir) {
    SuHostOperatorDesc d;
    SuHostError err;
    Game g;
    SuHostWorld *w;
    SuReplayWriter *rw = NULL;
    SuReplayOptions opt;
    uint8_t *bytes = NULL, *snap, dg[32];
    size_t len = 0, slen = 0;
    uint64_t t, done = 0;
    hull_desc(&d, "game.hull", 0u, V_SAME);
    check(su_host_register_operator(&d, &err) == SU_OK, "register");
    game_build(&g, &d);
    w = game_world(&g);
    if (w == NULL) {
        return 1;
    }
    memset(&opt, 0, sizeof opt);
    opt.struct_size = sizeof opt;
    opt.keyframe_interval = 8u;
    opt.digest_interval = 1u;
    check(su_replay_writer_begin(w, &opt, &rw, &err) == SU_OK, "writer begin");
    for (t = 0; t < 30u; t++) {
        push_script(w, t);
        check(su_replay_writer_step(rw, w, 1u, &done, &err) == SU_OK, "writer step");
    }
    check(su_replay_writer_finish(rw, w, &bytes, &len) == SU_OK, "finish");
    su_replay_writer_destroy(rw);
    snap = snapshot(w, &slen);
    digest(w, dg);
    check(write_file(dir, "opstate_replay.bin", bytes, len) && write_file(dir, "opstate_snap.bin", snap, slen) &&
              write_file(dir, "opstate_digest.bin", dg, 32u),
          "write files");
    su_replay_free(bytes);
    free(snap);
    su_host_world_destroy(w);
    game_free(&g);
    return 0;
}

static int run_verify(const char *dir, const char *what) {
    SuHostOperatorDesc d;
    SuHostError err;
    Game g;
    SuHostWorld *w;
    SuReplay *rp = NULL;
    uint8_t *bytes, *snap, *dg, now[32];
    size_t len = 0, slen = 0, dlen = 0;
    int variant = strcmp(what, "same") == 0      ? V_SAME
                  : strcmp(what, "entries") == 0 ? V_ENTRIES
                  : strcmp(what, "bytes") == 0   ? V_BYTES
                  : strcmp(what, "flags") == 0   ? V_FLAGS
                  : strcmp(what, "nostate") == 0 ? V_NOSTATE
                                                 : -1;
    uint32_t want = variant == V_SAME ? SU_OK : SU_E_SCHEMA_MISMATCH, st;
    if (variant < 0) {
        fprintf(stderr, "unknown verify mode %s\n", what);
        return 2;
    }
    hull_desc(&d, "game.hull", 0u, variant);
    check(su_host_register_operator(&d, &err) == SU_OK, "register %s", what);
    game_build(&g, &d);
    w = game_world(&g);
    bytes = read_file(dir, "opstate_replay.bin", &len);
    snap = read_file(dir, "opstate_snap.bin", &slen);
    dg = read_file(dir, "opstate_digest.bin", &dlen);
    if (w == NULL || bytes == NULL || snap == NULL || dg == NULL || dlen != 32u) {
        fprintf(stderr, "missing world or files\n");
        return 1;
    }
    st = su_host_snapshot_restore(w, snap, slen, &err);
    check(st == want, "%s: snapshot restore %s (want %s)", what, su_host_status_name(st), su_host_status_name(want));
    st = su_host_snapshot_restore_inplace(w, snap, slen, &err);
    check(st == want, "%s: in-place restore %s", what, su_host_status_name(st));
    if (want == SU_OK) {
        digest(w, now);
        check(memcmp(now, dg, 32u) == 0, "restored digest equals the recording process's");
    }
    check(su_replay_open(bytes, len, &rp, &err) == SU_OK, "replay open");
    st = su_replay_seek(rp, w, 30u, &err);
    check(st == want, "%s: replay seek to the end %s (%s)", what, su_host_status_name(st), err.message);
    if (want == SU_OK) {
        uint64_t done = 0;
        check(su_replay_seek(rp, w, 3u, &err) == SU_OK && su_replay_play(rp, w, 100u, &done, &err) == SU_OK &&
                  done == 27u,
              "replay seek and play verify every digest");
        digest(w, now);
        check(memcmp(now, dg, 32u) == 0, "replayed end digest equals the recording process's");
    }
    su_replay_close(rp);
    free(bytes);
    free(snap);
    free(dg);
    su_host_world_destroy(w);
    game_free(&g);
    return 0;
}

/* ---------- 11. operator-state budget (issue 153) ---------- */

static uint32_t noop_run(const SuHostOperatorRun *run, void *user) {
    (void)run;
    (void)user;
    return SU_OK;
}

static const SuHostOperatorField k_read_sum[1] = {{F_SUM, SU_TYPE_U32, SU_ACCESS_READ, 0, "sum"}};

/* A read-only stateful operator scheduled after game.hull (so several of them never conflict). */
static void reader_desc(SuHostOperatorDesc *d, const char *system_id, const char *operator_name, uint32_t entries,
                        uint32_t bytes) {
    uint32_t i;
    SU_HOST_OPERATOR_DESC_INIT(d);
    d->system_id = system_id;
    d->schema_id = "game.hull";
    d->operator_name = operator_name;
    d->operator_version = 1u;
    d->field_count = 1u;
    d->fields = k_read_sum;
    for (i = 0; i < 32u; i++) {
        d->constants_hash[i] = (uint8_t)(i * 7u + 1u);
    }
    d->run = noop_run;
    d->state_max_entries = entries;
    d->state_max_bytes = bytes;
}

/* The base game (game.hull, game.clear) plus one record per reader, in this order. */
typedef struct BudgetWorld {
    Buf rec[4];
    SuHostCapsule caps[6];
    SuHostWorldDesc desc;
} BudgetWorld;

static void budget_world(BudgetWorld *b, const Game *g, const SuHostOperatorDesc *readers, uint32_t n) {
    uint32_t i;
    memset(b, 0, sizeof *b);
    b->caps[0] = g->caps[0];
    b->caps[1] = g->caps[1];
    for (i = 0; i < n && i < 4u; i++) {
        record_json(&b->rec[i], &readers[i], "\"game.hull\"");
        b->caps[2u + i].capsule_json = (const uint8_t *)b->rec[i].p;
        b->caps[2u + i].capsule_len = b->rec[i].n;
    }
    b->desc = g->desc;
    b->desc.capsules = b->caps;
    b->desc.capsule_count = 2u + n;
}

static void budget_free(BudgetWorld *b) {
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        free(b->rec[i].p);
    }
}

/* Creates the world with this budget; returns the status, destroys the world (identity into id when non-NULL). */
static uint32_t budget_try(const SuHostWorldDesc *base, uint32_t struct_size, uint64_t budget, SuHostError *err,
                           uint8_t id[32]) {
    SuHostWorldDesc d = *base;
    SuHostWorld *w = (SuHostWorld *)(uintptr_t)1u;
    uint32_t st;
    d.struct_size = struct_size;
    d.state_budget_bytes = budget;
    st = su_host_world_create(&d, &w, err);
    check((st == SU_OK) == (w != NULL), "world create: *out is set exactly on success");
    if (w != NULL && id != NULL) {
        su_host_world_identity(w, id);
    }
    su_host_world_destroy(w);
    return st;
}

/* The reservation the engine reports for the first stateful operator (budget 1: every stateful world is refused). */
static uint64_t reported_reservation(const SuHostWorldDesc *d) {
    SuHostError err;
    const char *p;
    if (budget_try(d, (uint32_t)sizeof *d, 1u, &err, NULL) != SU_E_RESOURCE_LIMIT) {
        return 0u;
    }
    p = strstr(err.message, " (");
    return p != NULL ? strtoull(p + 2, NULL, 10) : 0u;
}

static void test_budget(const Game *g) {
    enum { RE = 64u, RB = 1000u };
    SuHostOperatorDesc rd[4];
    SuHostError err;
    BudgetWorld one, big, several, huge;
    uint8_t id0[32], id1[32];
    uint64_t r_hull, r_reader, r_big;
    char want[256];
    uint32_t st;

    reader_desc(&rd[0], "budget.r1", "budget_r1_v1", RE, RB);
    reader_desc(&rd[1], "budget.r2", "budget_r2_v1", RE, RB);
    reader_desc(&rd[2], "budget.r3", "budget_r3_v1", RE, RB);
    reader_desc(&rd[3], "budget.big", "budget_big_v1", RE, 64u * RB);
    check(su_host_register_operator(&rd[0], &err) == SU_OK && su_host_register_operator(&rd[1], &err) == SU_OK &&
              su_host_register_operator(&rd[2], &err) == SU_OK && su_host_register_operator(&rd[3], &err) == SU_OK,
          "register the budget readers: %s", err.message);

    /* The exact reservation: 4 x bytes + 52 x entries + a header under 256 bytes, the same for every operator. */
    r_hull = reported_reservation(&g->desc);
    check(r_hull > 4u * MAX_BYTES + 52u * MAX_ENTRIES && r_hull < 4u * MAX_BYTES + 52u * MAX_ENTRIES + 256u,
          "game.hull reserves 4 x %u + 52 x %u bytes plus a header (%llu)", MAX_BYTES, MAX_ENTRIES,
          (unsigned long long)r_hull);
    check(budget_try(&g->desc, (uint32_t)sizeof g->desc, r_hull, &err, NULL) == SU_OK,
          "a budget of exactly the reservation admits the world: %s", err.message);
    st = budget_try(&g->desc, (uint32_t)sizeof g->desc, r_hull - 1u, &err, NULL);
    snprintf(want, sizeof want,
             "capsule 0: the operator state of game.hull (%llu bytes) brings the world to %llu bytes, over its "
             "operator-state budget of %llu bytes",
             (unsigned long long)r_hull, (unsigned long long)r_hull, (unsigned long long)(r_hull - 1u));
    check(st == SU_E_RESOURCE_LIMIT && err.index == 0u && strcmp(err.message, want) == 0,
          "one byte less is refused, naming the operator, the total and the budget: %s (%s)", su_host_status_name(st),
          err.message);

    /* One operator fits, a larger one does not, under the same budget (the base game is game.hull alone). */
    budget_world(&one, g, &rd[0], 1u);
    budget_world(&big, g, &rd[3], 1u);
    {
        /* budget.r1's reservation: the second stateful operator crosses a budget of r_hull and is reported. */
        st = budget_try(&one.desc, (uint32_t)sizeof one.desc, r_hull, &err, NULL);
        check(st == SU_E_RESOURCE_LIMIT && err.index == 2u && strstr(err.message, "of budget.r1 (") != NULL,
              "the second stateful operator crosses the budget: %s", err.message);
        r_reader = strtoull(strstr(err.message, " (") + 2, NULL, 10);
        check(r_reader > 4u * RB + 52u * RE && r_reader < 4u * RB + 52u * RE + 256u &&
                  r_reader - (4u * RB + 52u * RE) == r_hull - (4u * MAX_BYTES + 52u * MAX_ENTRIES),
              "budget.r1 reserves 4 x %u + 52 x %u bytes plus the same header (%llu)", RB, RE,
              (unsigned long long)r_reader);
    }
    check(budget_try(&one.desc, (uint32_t)sizeof one.desc, r_hull + r_reader, &err, NULL) == SU_OK,
          "game.hull and budget.r1 fit in %llu bytes: %s", (unsigned long long)(r_hull + r_reader), err.message);
    st = budget_try(&big.desc, (uint32_t)sizeof big.desc, r_hull + r_reader, &err, NULL);
    r_big = r_reader + 4u * 63u * RB;
    snprintf(want, sizeof want,
             "capsule 2: the operator state of budget.big (%llu bytes) brings the world to %llu bytes, over its "
             "operator-state budget of %llu bytes",
             (unsigned long long)r_big, (unsigned long long)(r_hull + r_big),
             (unsigned long long)(r_hull + r_reader));
    check(st == SU_E_RESOURCE_LIMIT && err.index == 2u && strcmp(err.message, want) == 0,
          "budget.big does not fit in the same budget: %s", err.message);

    /* Several operators fit only under a raised budget; the one that crosses is named. */
    budget_world(&several, g, rd, 3u);
    st = budget_try(&several.desc, (uint32_t)sizeof several.desc, r_hull + 3u * r_reader - 1u, &err, NULL);
    snprintf(want, sizeof want,
             "capsule 4: the operator state of budget.r3 (%llu bytes) brings the world to %llu bytes, over its "
             "operator-state budget of %llu bytes",
             (unsigned long long)r_reader, (unsigned long long)(r_hull + 3u * r_reader),
             (unsigned long long)(r_hull + 3u * r_reader - 1u));
    check(st == SU_E_RESOURCE_LIMIT && err.index == 4u && strcmp(err.message, want) == 0,
          "four stateful operators exceed a budget one byte short: %s", err.message);
    st = budget_try(&several.desc, (uint32_t)sizeof several.desc, r_hull + r_reader + 1u, &err, NULL);
    check(st == SU_E_RESOURCE_LIMIT && err.index == 3u && strstr(err.message, "of budget.r2 (") != NULL,
          "a smaller budget is crossed earlier, by budget.r2: %s", err.message);
    check(budget_try(&several.desc, (uint32_t)sizeof several.desc, r_hull + 3u * r_reader, &err, id0) == SU_OK,
          "four stateful operators fit under the raised budget: %s", err.message);
    {
        SuHostWorld *w = NULL;
        SuHostWorldDesc d = several.desc;
        d.state_budget_bytes = r_hull + 3u * r_reader;
        check(su_host_world_create(&d, &w, &err) == SU_OK && w != NULL, "several: world: %s", err.message);
        if (w != NULL) {
            step(w, 3u);
            check(su_host_tick(w) == 3u, "several: the admitted world steps");
            su_host_world_destroy(w);
        }
    }

    /* Identity: the budget decides admission only. */
    check(budget_try(&several.desc, (uint32_t)sizeof several.desc, UINT64_MAX, &err, id1) == SU_OK &&
              memcmp(id0, id1, 32u) == 0,
          "the identity does not depend on the budget");
    {
        uint8_t a[32], b[32], c[32];
        check(budget_try(&g->desc, (uint32_t)sizeof g->desc, 0u, &err, a) == SU_OK &&
                  budget_try(&g->desc, (uint32_t)sizeof g->desc, r_hull, &err, b) == SU_OK &&
                  budget_try(&g->desc, (uint32_t)SU_HOST_WORLD_DESC_V1_SIZE, 0u, &err, c) == SU_OK &&
                  memcmp(a, b, 32u) == 0 && memcmp(a, c, 32u) == 0,
              "game.hull: one identity for budgets 0, exact and the v1 descriptor");
    }

    /* The default: 0 and the older descriptor sizes mean SU_HOST_STATE_BUDGET_DEFAULT (not unlimited). An operator
     * reserving just over 1 GiB is refused by each (and allocates nothing, so this costs no memory). */
    {
        SuHostOperatorDesc hd;
        uint64_t r_huge;
        uint32_t sizes[3];
        uint32_t k;
        /* 4 x bytes + 52 x entries + header > 2^30 with bytes = 2^28 - 8 (the header makes up the rest). */
        reader_desc(&hd, "budget.huge", "budget_huge_v1", 1u, (uint32_t)(SU_HOST_STATE_BUDGET_DEFAULT / 4u) - 8u);
        check(su_host_register_operator(&hd, &err) == SU_OK, "register budget.huge: %s", err.message);
        budget_world(&huge, g, &hd, 1u);
        r_huge = r_reader - (4u * RB + 52u * RE) + 4u * (uint64_t)hd.state_max_bytes + 52u;
        check(r_huge > SU_HOST_STATE_BUDGET_DEFAULT && r_huge < SU_HOST_STATE_BUDGET_DEFAULT + 256u,
              "budget.huge reserves just over 1 GiB (%llu)", (unsigned long long)r_huge);
        sizes[0] = (uint32_t)sizeof huge.desc;
        sizes[1] = (uint32_t)SU_HOST_WORLD_DESC_V2_SIZE;
        sizes[2] = (uint32_t)SU_HOST_WORLD_DESC_V1_SIZE;
        for (k = 0; k < 3u; k++) {
            /* With a nonzero budget in the struct: sizes 1 and 2 do not cover it, so it must be ignored. */
            st = budget_try(&huge.desc, sizes[k], k == 0u ? 0u : UINT64_MAX, &err, NULL);
            snprintf(want, sizeof want, "over its operator-state budget of %llu bytes",
                     (unsigned long long)SU_HOST_STATE_BUDGET_DEFAULT);
            check(st == SU_E_RESOURCE_LIMIT && err.index == 2u && strstr(err.message, want) != NULL &&
                      strstr(err.message, "budget.huge") != NULL,
                  "struct_size %u: the default budget applies: %s", (unsigned)sizes[k], err.message);
        }
        check(budget_try(&huge.desc, (uint32_t)SU_HOST_WORLD_DESC_V2_SIZE + 4u, 0u, &err, NULL) ==
                  SU_E_INVALID_ARGUMENT,
              "a size ending inside the budget field is refused");
#ifdef SU_TEST_WRAP_ALLOC
        /* A refused world allocates nothing of its operator state: no request of even half an arena
         * (2 x state_max_bytes), while an admitted world requests its arenas. */
        g_allocs = 0;
        g_alloc_max = 0;
        g_count_alloc = 1;
        st = budget_try(&huge.desc, (uint32_t)sizeof huge.desc, 0u, &err, NULL);
        g_count_alloc = 0;
        check(st == SU_E_RESOURCE_LIMIT && g_alloc_max < (size_t)hd.state_max_bytes,
              "a refused world requests no state memory (largest request %lu bytes in %lu allocations)",
              (unsigned long)g_alloc_max, g_allocs);
        g_alloc_max = 0;
        g_count_alloc = 1;
        st = budget_try(&big.desc, (uint32_t)sizeof big.desc, 0u, &err, NULL);
        g_count_alloc = 0;
        check(st == SU_OK && g_alloc_max >= 2u * 64u * RB, "an admitted world allocates its arenas (largest %lu)",
              (unsigned long)g_alloc_max);
#endif
    }
    budget_free(&one);
    budget_free(&big);
    budget_free(&several);
    budget_free(&huge);
}

int main(int argc, char **argv) {
    Game g, dd, plain;
    SuHostOperatorDesc d;
    int rc = 0;
    if (argc >= 3 && strcmp(argv[1], "record") == 0) {
        rc = run_record(argv[2]);
    } else if (argc >= 4 && strcmp(argv[1], "verify") == 0) {
        rc = run_verify(argv[2], argv[3]);
    } else {
        test_registration();
        register_ops();
        hull_desc(&d, "game.hull", 0u, V_SAME);
        game_build(&g, &d);
        hull_desc(&d, "game.hull_dd", SU_HOST_STATE_DROP_DEAD, V_SAME);
        game_build(&dd, &d);
        hull_desc(&d, "game.hull_plain", 0u, V_NOSTATE);
        game_build(&plain, &d);
        test_basics(&g, &plain);
        test_rollback(&g);
        test_digest_sees_state(&g);
        test_canonical(&g);
        test_caps(&g);
        test_corrupt(&g);
        test_replay(&g);
        test_drop_dead(&dd, &g);
        test_alloc(&g, &dd);
        test_misuse(&g);
        test_budget(&g);
        game_free(&g);
        game_free(&dd);
        game_free(&plain);
    }
    printf("test_host_opstate: %u checks, %u failures\n", g_checks, g_failures);
    return rc != 0 ? rc : g_failures == 0u ? 0 : 1;
}
