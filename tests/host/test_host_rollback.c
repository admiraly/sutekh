/* Host event reading, in-place snapshot restore and the fast digest (include/sutekh/host.h, additive part).
 *
 * Events are checked against expectations derived by hand from the emit_then_despawn fixture (rows with
 * position_x < 2 emit kind 5 with a = slot, b = position_x, then despawn). In-place restore is checked
 * differentially against su_host_snapshot_restore: the same snapshots, valid and corrupted, must give the
 * same status and the same resulting bytes, and a refused restore must leave the world unchanged. The fast
 * digest is checked against XXH64 reference vectors produced by python-xxhash 4.0.1.
 *
 * Usage: test_host_rollback   (run from the repository root: it reads tests/fixtures/entities/) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/host.h"
#ifndef SUTEKH_HOST_DLL
#include "host/su_xxh64.h"
#endif

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

typedef struct Bytes {
    uint8_t *data;
    size_t len;
} Bytes;

static Bytes slurp(const char *path) {
    Bytes b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b.data = malloc((size_t)n + 1u);
    if (b.data == NULL || fread(b.data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    b.len = (size_t)n;
    return b;
}

static SuHostCapsule g_emit;

#define F_POS 1u
#define F_VEL 2u
#define K_EMIT 5u

static SuHostWorld *make_world(uint64_t capacity, uint64_t log_max) {
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t st;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 1;
    d.capsules = &g_emit;
    d.max_entities = capacity;
    d.tick_rate_hz = 120;
    d.event_log_max = log_max;
    st = su_host_world_create(&d, &w, &e);
    if (st != SU_OK) {
        fprintf(stderr, "create failed: %s %s\n", su_host_status_name(st), e.message);
        exit(1);
    }
    return w;
}

static uint64_t spawn(SuHostWorld *w, uint32_t pos, uint32_t vel) {
    SuHostValue v[2];
    uint64_t h = 0;
    SuHostError e;
    v[0].field_id = F_POS;
    v[0].value = pos;
    v[1].field_id = F_VEL;
    v[1].value = vel;
    CHECK(su_host_spawn(w, v, 2, 0, &h, &e) == SU_OK);
    return h;
}

static void step(SuHostWorld *w, uint64_t n) {
    uint64_t done = 0;
    SuHostError e;
    uint32_t st = su_host_step(w, n, &done, &e);
    CHECK(st == SU_OK && done == n);
    if (st != SU_OK) {
        fprintf(stderr, "step: %s %s\n", su_host_status_name(st), e.message);
    }
}

static void set_pos(SuHostWorld *w, uint64_t tick, uint64_t ent, uint32_t pos) {
    SuHostInput in;
    SuHostError e;
    in.entity = ent;
    in.field_id = F_POS;
    in.value = pos;
    CHECK(su_host_input_push(w, tick, &in, 1, &e) == SU_OK);
}

static Bytes snap(const SuHostWorld *w) {
    Bytes b = {NULL, 0};
    CHECK(su_host_snapshot_size(w, &b.len) == SU_OK);
    b.data = malloc(b.len);
    CHECK(b.data != NULL && su_host_snapshot_write(w, b.data, b.len, &b.len) == SU_OK);
    return b;
}

static int same_bytes(Bytes a, Bytes b) {
    return a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
}

/* ---------- XXH64 ---------- */

static void test_xxh64_vectors(void) {
#ifndef SUTEKH_HOST_DLL
    static const struct {
        size_t n;
        uint64_t want;
    } k[] = {{3, UINT64_C(0x31D2363F52E564C9)},  {4, UINT64_C(0x9BB64B7D66EE9FDA)},
             {7, UINT64_C(0x9A7B149959CE60D8)},  {8, UINT64_C(0xDAB99D95C6F90092)},
             {12, UINT64_C(0xD52E407833AF5133)}, {31, UINT64_C(0xA2AA5F33CC4A6119)},
             {32, UINT64_C(0x23C3C17EF790FD97)}, {33, UINT64_C(0x50A7CFC7BA588784)},
             {63, UINT64_C(0x5E3E54B431C7493C)}, {64, UINT64_C(0x0EB64B3EF6EEB01F)},
             {100, UINT64_C(0xA61F8D4C170FE531)}, {1000, UINT64_C(0x5F235FA033F1A3FB)}};
    uint8_t d[1000];
    size_t i;
    for (i = 0; i < sizeof d; i++) {
        d[i] = (uint8_t)((i * 7u + 3u) & 0xFFu);
    }
    CHECK(su_xxh64("", 0, 0) == UINT64_C(0xEF46DB3751D8E999));
    CHECK(su_xxh64("a", 1, 0) == UINT64_C(0xD24EC4F1A98C6E5B));
    CHECK(su_xxh64("abc", 3, 0) == UINT64_C(0x44BC2CF5AD770999));
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        CHECK(su_xxh64(d, k[i].n, 0) == k[i].want);
    }
    CHECK(su_xxh64(d, 100, UINT64_C(0x9E3779B97F4A7C15)) == UINT64_C(0xF6D8F65C625ABB4F));
#endif
}

/* ---------- events ---------- */

static void check_event(const SuHostEvent *ev, uint64_t seq, uint64_t tick, uint32_t kind, uint32_t sys,
                        uint32_t slot, uint32_t a, uint32_t b) {
    CHECK(ev->event_seq == seq);
    CHECK(ev->tick == tick);
    CHECK(ev->kind == kind);
    CHECK(ev->system_index == sys);
    CHECK(ev->slot == slot);
    CHECK(ev->a == a);
    CHECK(ev->b == b);
}

static void test_events_basic(void) {
    SuHostWorld *w = make_world(8, 0);
    static const uint32_t pos[6] = {5, 1, 7, 0, 9, 3};
    uint64_t h[6];
    SuHostEvent ev[16];
    uint32_t n = 0;
    uint32_t total = 0;
    uint32_t complete = 0;
    uint64_t cur = 0;
    uint64_t skipped = 99;
    uint32_t i;

    CHECK(su_host_event_next_seq(w) == 0u);
    CHECK(su_host_events_read(w, 0, ev, 16, &n, &cur, &skipped) == SU_OK && n == 0u && cur == 0u && skipped == 0u);
    for (i = 0; i < 6; i++) {
        h[i] = spawn(w, pos[i], 1);
    }
    /* Six host spawn records at tick 0, slots 0..5, generation 1. */
    CHECK(su_host_event_next_seq(w) == 6u);
    CHECK(su_host_events_for_tick(w, 0, ev, 16, &n, &total, &complete) == SU_OK && n == 6u && total == 6u &&
          complete == 1u);
    for (i = 0; i < 6 && i < n; i++) {
        check_event(&ev[i], i, 0, SU_HOST_EVENT_SPAWNED, SU_HOST_EVENT_SYSTEM_HOST, i, i, 1);
    }
    /* Tick 0 -> 1: slots 1 (p=1) and 3 (p=0) emit and despawn; order by (system, slot, seq). */
    step(w, 1);
    CHECK(su_host_event_next_seq(w) == 10u);
    CHECK(su_host_events_for_tick(w, 1, ev, 16, &n, &total, &complete) == SU_OK && n == 4u && total == 4u &&
          complete == 1u);
    if (n == 4u) {
        check_event(&ev[0], 6, 1, K_EMIT, 0, 1, 1, 1);
        check_event(&ev[1], 7, 1, SU_HOST_EVENT_DESPAWNED, 0, 1, 1, 1);
        check_event(&ev[2], 8, 1, K_EMIT, 0, 3, 3, 0);
        check_event(&ev[3], 9, 1, SU_HOST_EVENT_DESPAWNED, 0, 3, 3, 1);
        CHECK(ev[0].seq == 0u && ev[1].seq == 1u);
    }
    /* Truncated copy reports the total. */
    CHECK(su_host_events_for_tick(w, 1, ev, 3, &n, &total, NULL) == SU_OK && n == 3u && total == 4u);
    CHECK(su_host_events_for_tick(w, 7, ev, 16, &n, &total, NULL) == SU_OK && n == 0u && total == 0u);
    /* Cursor reading in pieces covers the whole log in order. */
    cur = 0;
    {
        uint64_t seen = 0;
        while (cur < su_host_event_next_seq(w)) {
            uint64_t next = 0;
            CHECK(su_host_events_read(w, cur, ev, 3, &n, &next, &skipped) == SU_OK && skipped == 0u);
            for (i = 0; i < n; i++) {
                CHECK(ev[i].event_seq == seen + i);
            }
            seen += n;
            CHECK(next == cur + n);
            if (n == 0u) {
                break;
            }
            cur = next;
        }
        CHECK(seen == 10u);
    }
    CHECK(su_host_events_read(w, 11, ev, 16, &n, &cur, NULL) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_events_read(w, 0, NULL, 4, &n, &cur, NULL) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_events_read(w, 10, NULL, 0, &n, &cur, NULL) == SU_OK && n == 0u && cur == 10u);
    /* An input sets slot 0 to p = 1 for tick 1: tick 1 -> 2 emits for slot 0 only. */
    set_pos(w, 1, h[0], 1);
    step(w, 1);
    CHECK(su_host_events_for_tick(w, 2, ev, 16, &n, &total, NULL) == SU_OK && n == 2u);
    if (n == 2u) {
        check_event(&ev[0], 10, 2, K_EMIT, 0, 0, 0, 1);
        check_event(&ev[1], 11, 2, SU_HOST_EVENT_DESPAWNED, 0, 0, 0, 1);
    }
    /* A host despawn at committed tick 2. */
    {
        SuHostError e;
        CHECK(su_host_despawn(w, h[2], &e) == SU_OK);
        CHECK(su_host_events_for_tick(w, 2, ev, 16, &n, &total, NULL) == SU_OK && n == 3u);
        if (n == 3u) {
            check_event(&ev[2], 12, 2, SU_HOST_EVENT_DESPAWNED, SU_HOST_EVENT_SYSTEM_HOST, 2, 2, 1);
        }
    }
    su_host_world_destroy(w);
}

/* A ring of 4 events: older events are evicted and readers are told. */
static void test_events_ring(void) {
    SuHostWorld *w = make_world(8, 4);
    SuHostEvent ev[8];
    uint32_t n = 0;
    uint32_t total = 0;
    uint32_t complete = 9;
    uint64_t cur = 0;
    uint64_t skipped = 0;
    uint32_t i;
    for (i = 0; i < 6; i++) {
        (void)spawn(w, 10u + i, 0);
    }
    /* Seqs 0..5 at tick 0; only 2..5 are retained. */
    CHECK(su_host_event_next_seq(w) == 6u);
    CHECK(su_host_events_read(w, 0, ev, 8, &n, &cur, &skipped) == SU_OK && n == 4u && skipped == 2u && cur == 6u);
    CHECK(n == 4u && ev[0].event_seq == 2u && ev[3].event_seq == 5u);
    CHECK(su_host_events_for_tick(w, 0, ev, 8, &n, &total, &complete) == SU_OK && n == 4u && complete == 0u);
    /* Nothing of tick 1 can have been evicted: the oldest retained event is from tick 0. */
    step(w, 1);
    CHECK(su_host_events_for_tick(w, 1, ev, 8, &n, &total, &complete) == SU_OK && n == 0u && complete == 1u);
    su_host_world_destroy(w);
}

/* Rollback: restore restores the log; re-simulating the same inputs re-emits identical events. */
static void test_events_rollback(int inplace) {
    SuHostWorld *w = make_world(16, 64);
    uint64_t h[8];
    SuHostEvent first[64];
    SuHostEvent again[64];
    uint32_t n1 = 0;
    uint32_t n2 = 0;
    uint64_t cur = 0;
    uint64_t seq_at_snap;
    Bytes s;
    SuHostError e;
    uint32_t i;
    for (i = 0; i < 8; i++) {
        h[i] = spawn(w, 100u + i, 1);
    }
    step(w, 2);
    s = snap(w);
    seq_at_snap = su_host_event_next_seq(w);
    for (i = 0; i < 4; i++) {
        set_pos(w, su_host_tick(w), h[2 * i + 1], 0);
        step(w, 1);
    }
    CHECK(su_host_events_read(w, seq_at_snap, first, 64, &n1, &cur, NULL) == SU_OK && n1 == 8u);
    CHECK(inplace ? su_host_snapshot_restore_inplace(w, s.data, s.len, &e) == SU_OK
                  : su_host_snapshot_restore(w, s.data, s.len, &e) == SU_OK);
    CHECK(su_host_event_next_seq(w) == seq_at_snap);
    CHECK(su_host_events_read(w, cur, again, 64, &n2, &cur, NULL) == SU_E_INVALID_ARGUMENT);
    for (i = 0; i < 4; i++) {
        set_pos(w, su_host_tick(w), h[2 * i + 1], 0);
        step(w, 1);
    }
    CHECK(su_host_events_read(w, seq_at_snap, again, 64, &n2, &cur, NULL) == SU_OK && n2 == n1);
    CHECK(n1 == n2 && memcmp(first, again, (size_t)n1 * sizeof first[0]) == 0);
    free(s.data);
    su_host_world_destroy(w);
}

/* ---------- in-place restore ---------- */

/* Builds churned state: spawns, despawns by the system, host despawns and inputs over several ticks. */
static void churn(SuHostWorld *w, uint32_t rounds, uint64_t *x) {
    uint32_t r;
    for (r = 0; r < rounds; r++) {
        uint64_t ent = 0;
        SuHostError e;
        *x ^= *x << 13;
        *x ^= *x >> 7;
        *x ^= *x << 17;
        if (su_host_capacity(w) > 0u) {
            uint32_t slot = (uint32_t)(*x % su_host_capacity(w));
            if (su_host_entity_at(w, slot, &ent) == SU_OK) {
                if ((*x >> 8) % 3u == 0u) {
                    CHECK(su_host_despawn(w, ent, &e) == SU_OK);
                } else {
                    set_pos(w, su_host_tick(w), ent, (uint32_t)((*x >> 16) % 4u));
                }
            } else {
                (void)spawn(w, (uint32_t)(2u + (*x >> 20) % 50u), (uint32_t)(*x >> 40));
            }
        }
        step(w, 1);
    }
}

static void test_inplace_equivalence(void) {
    SuHostWorld *a = make_world(64, 32);
    SuHostWorld *b = make_world(64, 32);
    uint64_t x = UINT64_C(0x9E3779B97F4A7C15);
    uint64_t y = x;
    Bytes saved[6];
    uint32_t k;
    SuHostError e;
    /* Same history in both worlds; keep snapshots along the way (including the empty start). */
    for (k = 0; k < 6; k++) {
        saved[k] = snap(a);
        churn(a, 7, &x);
        churn(b, 7, &y);
    }
    CHECK(same_bytes(snap(a), snap(b)));
    /* Restore every saved state, in a scrambled order, through both paths: identical results. */
    for (k = 0; k < 12; k++) {
        uint32_t i = (k * 5u + 3u) % 6u;
        Bytes ra;
        Bytes rb;
        uint8_t da[32];
        uint8_t db[32];
        uint64_t fa = 0;
        uint64_t fb = 0;
        CHECK(su_host_snapshot_restore(a, saved[i].data, saved[i].len, &e) == SU_OK);
        CHECK(su_host_snapshot_restore_inplace(b, saved[i].data, saved[i].len, &e) == SU_OK);
        ra = snap(a);
        rb = snap(b);
        CHECK(same_bytes(ra, rb));
        CHECK(same_bytes(rb, saved[i]));
        CHECK(su_host_digest(a, da) == SU_OK && su_host_digest(b, db) == SU_OK && memcmp(da, db, 32) == 0);
        CHECK(su_host_digest_fast(a, &fa) == SU_OK && su_host_digest_fast(b, &fb) == SU_OK && fa == fb);
        /* Continue from the restored state: the worlds stay identical. */
        churn(a, 3, &x);
        churn(b, 3, &y);
        free(ra.data);
        free(rb.data);
        ra = snap(a);
        rb = snap(b);
        CHECK(same_bytes(ra, rb));
        free(ra.data);
        free(rb.data);
    }
    for (k = 0; k < 6; k++) {
        free(saved[k].data);
    }
    su_host_world_destroy(a);
    su_host_world_destroy(b);
}

/* A restore to a state with a lower high water clears the rows the later state used. */
static void test_inplace_lower_high_water(void) {
    SuHostWorld *w = make_world(16, 0);
    Bytes s0;
    Bytes s1;
    uint32_t col[16];
    uint8_t alive[16];
    SuHostError e;
    uint32_t i;
    (void)spawn(w, 50, 1);
    s0 = snap(w);
    for (i = 0; i < 10; i++) {
        (void)spawn(w, 60u + i, 2u + i);
    }
    step(w, 3);
    CHECK(su_host_snapshot_restore_inplace(w, s0.data, s0.len, &e) == SU_OK);
    s1 = snap(w);
    CHECK(same_bytes(s0, s1));
    CHECK(su_host_read(w, F_POS, 0, 16, col) == SU_OK && col[0] == 50u);
    CHECK(su_host_alive(w, 0, 16, alive) == SU_OK && alive[0] == 1u);
    for (i = 1; i < 16; i++) {
        CHECK(col[i] == 0u && alive[i] == 0u);
    }
    CHECK(su_host_read(w, F_VEL, 1, 15, col) == SU_OK);
    for (i = 0; i < 15; i++) {
        CHECK(col[i] == 0u);
    }
    /* The world keeps working: spawn reuses slot 1 with generation 1. */
    {
        uint64_t h = spawn(w, 9, 9);
        CHECK(h == ((UINT64_C(1) << 32) | 1u));
    }
    free(s0.data);
    free(s1.data);
    su_host_world_destroy(w);
}

static void test_inplace_errors_and_inputs(void) {
    SuHostWorld *w = make_world(8, 16);
    SuHostWorld *other = make_world(9, 16);
    Bytes s;
    Bytes before;
    Bytes after;
    SuHostError e;
    size_t enc = 0;
    uint64_t h = spawn(w, 40, 1);
    step(w, 1);
    s = snap(w);
    /* Wrong identity (capacity differs). */
    CHECK(su_host_snapshot_restore_inplace(other, s.data, s.len, &e) == SU_E_SCHEMA_MISMATCH);
    CHECK(e.code == SU_E_SCHEMA_MISMATCH);
    CHECK(su_host_snapshot_restore_inplace(NULL, s.data, s.len, &e) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_snapshot_restore_inplace(w, NULL, 0, &e) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_snapshot_restore_inplace(w, s.data, 10, NULL) == SU_E_PARSE);
    /* Queued inputs survive a restore. */
    step(w, 2);
    set_pos(w, 3, h, 1);
    before = snap(w);
    CHECK(su_host_snapshot_restore_inplace(w, s.data, s.len - 1u, &e) == SU_E_PARSE);
    after = snap(w);
    CHECK(same_bytes(before, after));
    CHECK(su_host_snapshot_restore_inplace(w, s.data, s.len, &e) == SU_OK && su_host_tick(w) == 1u);
    CHECK(su_host_input_encode(w, 3, NULL, 0, &enc) == SU_OK && enc == 24u + 16u);
    free(s.data);
    free(before.data);
    free(after.data);
    su_host_world_destroy(w);
    su_host_world_destroy(other);
}

/* Differential fuzz: corrupted snapshots through both restore paths, from the same starting state. */
static void test_inplace_fuzz(void) {
    SuHostWorld *src = make_world(40, 16);
    uint64_t x = UINT64_C(0x123456789ABCDEF1);
    Bytes valid;
    uint32_t round;
    long accepted = 0;
    long rejected = 0;
    long code_mismatch = 0;
    churn(src, 40, &x);
    valid = snap(src);
    for (round = 0; round < 3000u; round++) {
        SuHostWorld *a = make_world(40, 16);
        SuHostWorld *b = make_world(40, 16);
        uint64_t xa = UINT64_C(0xDEADBEEF) + round;
        uint64_t xb = xa;
        Bytes m;
        Bytes pa;
        Bytes pb;
        Bytes qa;
        Bytes qb;
        SuHostError ea;
        SuHostError eb;
        uint32_t sa;
        uint32_t sb;
        uint32_t edits;
        uint32_t j;
        churn(a, 5, &xa);
        churn(b, 5, &xb);
        pa = snap(a);
        pb = snap(b);
        m.len = valid.len;
        m.data = malloc(valid.len + 16u);
        memcpy(m.data, valid.data, valid.len);
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        edits = 1u + (uint32_t)(x % 3u);
        for (j = 0; j < edits; j++) {
            size_t at;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            at = (size_t)((x >> 8) % m.len);
            switch ((x >> 40) % 8u) {
            case 0: m.data[at] ^= (uint8_t)(1u << ((x >> 50) % 8u)); break;
            case 1: m.data[at] = (uint8_t)(x >> 52); break;
            case 2: m.data[at] = 0u; break;
            case 3: m.data[at] = 1u; break;
            case 4: m.len = at > 0u ? at : 1u; break;              /* truncate */
            case 5: m.data[m.len] = (uint8_t)x; m.len++; break; /* trailing byte */
            case 6: m.data[at] = 0xFFu; break;
            default: m.data[at] = (uint8_t)(m.data[at] + 1u); break;
            }
            if (m.len > valid.len + 8u) {
                m.len = valid.len + 8u;
            }
        }
        sa = su_host_snapshot_restore(a, m.data, m.len, &ea);
        sb = su_host_snapshot_restore_inplace(b, m.data, m.len, &eb);
        CHECK((sa == SU_OK) == (sb == SU_OK));
        if (sa != sb) {
            code_mismatch++;
            if (code_mismatch <= 5) {
                fprintf(stderr, "fuzz round %u: reference %s (%s), in-place %s (%s)\n", (unsigned)round,
                        su_host_status_name(sa), ea.message, su_host_status_name(sb), eb.message);
            }
        }
        qa = snap(a);
        qb = snap(b);
        if (sa == SU_OK && sb == SU_OK) {
            accepted++;
            CHECK(same_bytes(qa, qb));
            CHECK(qb.len == m.len && memcmp(qb.data, m.data, m.len) == 0);
        } else {
            rejected++;
            CHECK(same_bytes(qa, pa));
            CHECK(same_bytes(qb, pb));
        }
        free(m.data);
        free(pa.data);
        free(pb.data);
        free(qa.data);
        free(qb.data);
        su_host_world_destroy(a);
        su_host_world_destroy(b);
    }
    CHECK(code_mismatch == 0);
    CHECK(accepted > 0 && rejected > 0);
    printf("fuzz: %ld accepted, %ld rejected, %ld status-code differences\n", accepted, rejected, code_mismatch);
    free(valid.data);
    su_host_world_destroy(src);
}

/* ---------- fast digest ---------- */

static void test_fast_digest(void) {
    SuHostWorld *a = make_world(8, 0);
    SuHostWorld *b = make_world(8, 0);
    uint64_t fa = 0;
    uint64_t fb = 0;
    uint64_t f2 = 0;
    CHECK(su_host_digest_fast(a, &fa) == SU_OK && su_host_digest_fast(b, &fb) == SU_OK && fa == fb);
#ifndef SUTEKH_HOST_DLL
    {
        Bytes s = snap(a);
        CHECK(fa == su_xxh64(s.data, s.len, 0));
        free(s.data);
    }
#endif
    (void)spawn(a, 3, 1);
    CHECK(su_host_digest_fast(a, &f2) == SU_OK && f2 != fa);
    (void)spawn(b, 3, 1);
    CHECK(su_host_digest_fast(b, &fb) == SU_OK && fb == f2);
    step(a, 1);
    CHECK(su_host_digest_fast(a, &fa) == SU_OK && fa != f2);
    CHECK(su_host_digest_fast(NULL, &fa) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_digest_fast(a, NULL) == SU_E_INVALID_ARGUMENT);
    CHECK(SU_HOST_FAST_DIGEST_VERSION == 1u);
    su_host_world_destroy(a);
    su_host_world_destroy(b);
}

int main(void) {
    Bytes cap = slurp("tests/fixtures/entities/emit_then_despawn.capsule.json");
    Bytes lir = slurp("tests/fixtures/entities/emit_then_despawn.lir.json");
    g_emit.capsule_json = cap.data;
    g_emit.capsule_len = cap.len;
    g_emit.lir_json = lir.data;
    g_emit.lir_len = lir.len;

    test_xxh64_vectors();
    test_events_basic();
    test_events_ring();
    test_events_rollback(0);
    test_events_rollback(1);
    test_inplace_equivalence();
    test_inplace_lower_high_water();
    test_inplace_errors_and_inputs();
    test_inplace_fuzz();
    test_fast_digest();

    free(cap.data);
    free(lir.data);
    printf("test_host_rollback: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
