/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Host operators through the in-process host API (include/sutekh/host.h, "host operators", "execution tiers"),
 * built twice: against the static library and against the DLL. The world is the 6dof_racer_microcosm (two IR 0.3
 * systems and the space3d.query host record), created from capsule bytes, the space3d record bytes and the
 * fixture racer_microcosm dumps (argv[1]; ctest runs host_ops_fixture first).
 *
 *   1. operator listing, record refusals (unknown operator, malformed binding, lir_len without lir_json);
 *   2. 720 ticks: the state section and the event log section of the host snapshot hash to the digests pinned in
 *      bench/space3d/expected.json for the in-process schedule run (racer_microcosm), every 120 ticks as well;
 *   3. rollback: snapshot at an anchor before a gate event, then in-place restore (and the allocating restore) +
 *      re-step K = 6 and 12 in one call, 25 times each: every tick's SHA-256 and fast digest and events equal the
 *      first pass, the gate event is re-emitted with the same sequence number;
 *   4. replay: recording through the writer, seek + play verification in a fresh world;
 *   5. an identity-bound record (every lane binds itself): same states, different world identity;
 *   6. two worlds stepped concurrently on two threads, and two worlds interleaved on one thread at different
 *      ticks: both reach the pinned digest (per-world operator scratch);
 *   7. tiers: reference for both IR systems, and native for racer.move when the library has the backend
 *      (racer.race uses emit_event and is refused): the pinned digest every time. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_sha256.h"
#include "sutekh/host.h"
#include "sutekh/replay.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

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

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *b;
    *len = 0;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    b = malloc((size_t)sz + 1u);
    if (b != NULL && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b != NULL) {
        b[sz] = 0;
        *len = (size_t)sz;
    }
    return b;
}

typedef struct Bytes {
    uint8_t *p;
    size_t n;
} Bytes;

static Bytes g_race_cap, g_race_lir, g_move_cap, g_move_lir, g_query, g_fixture;
static char g_pin_state[65], g_pin_events[65];
static uint64_t g_pin_event_count;

#define RACERS 8u

static void desc_init(SuHostWorldDesc *d, SuHostCapsule caps[3], const Bytes *record) {
    memset(caps, 0, 3u * sizeof caps[0]);
    caps[0].capsule_json = g_race_cap.p;
    caps[0].capsule_len = g_race_cap.n;
    caps[0].lir_json = g_race_lir.p;
    caps[0].lir_len = g_race_lir.n;
    caps[1].capsule_json = g_move_cap.p;
    caps[1].capsule_len = g_move_cap.n;
    caps[1].lir_json = g_move_lir.p;
    caps[1].lir_len = g_move_lir.n;
    caps[2].capsule_json = record->p; /* host operator record: lir_json NULL, lir_len 0 */
    caps[2].capsule_len = record->n;
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d;
    d->capsule_count = 3;
    d->capsules = caps;
    d->fixture_json = g_fixture.p;
    d->fixture_len = g_fixture.n;
    d->tick_rate_hz = 120;
    /* The entity caps of racer_microcosm, so that the event log section is the same bytes. */
    d->max_struct_per_tick = 1;
    d->max_events_per_tick = 2u * RACERS + 2u;
    d->event_log_max = 1048576u;
}

static SuHostWorld *make_world(const Bytes *record) {
    SuHostWorldDesc d;
    SuHostCapsule caps[3];
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t st;
    desc_init(&d, caps, record);
    st = su_host_world_create(&d, &w, &err);
    check(st == SU_OK, "world create: %s (%s)", su_host_status_name(st), err.message);
    return st == SU_OK ? w : NULL;
}

/* SHA-256 of the snapshot's state section and of its event log section (hex). */
static void section_digests(const SuHostWorld *w, char state_hex[65], char log_hex[65]) {
    size_t size = 0, got = 0;
    uint8_t *buf;
    uint64_t state_len, log_len;
    uint8_t h[32];
    SuSha256 c;
    state_hex[0] = log_hex[0] = '\0';
    if (su_host_snapshot_size(w, &size) != SU_OK || (buf = malloc(size)) == NULL) {
        check(0, "snapshot size");
        return;
    }
    if (su_host_snapshot_write(w, buf, size, &got) != SU_OK || got != size) {
        check(0, "snapshot write");
        free(buf);
        return;
    }
    memcpy(&state_len, buf + 52, 8); /* little-endian hosts only (x86-64, arm64) */
    memcpy(&log_len, buf + 60 + state_len, 8);
    su_sha256_init(&c);
    su_sha256_update(&c, buf + 60, (size_t)state_len);
    su_sha256_final(&c, h);
    su_sha256_hex(h, state_hex);
    su_sha256_init(&c);
    su_sha256_update(&c, buf + 60 + state_len + 8, (size_t)log_len);
    su_sha256_final(&c, h);
    su_sha256_hex(h, log_hex);
    free(buf);
}

static int step_n(SuHostWorld *w, uint64_t n) {
    uint64_t done = 0;
    SuHostError err;
    uint32_t st = su_host_step(w, n, &done, &err);
    check(st == SU_OK && done == n, "step %llu: %s (%s)", (unsigned long long)n, su_host_status_name(st),
          err.message);
    return st == SU_OK && done == n;
}

/* Pinned digests of the N = 8, 720-tick run from bench/space3d/expected.json. */
static int read_pins(void) {
    size_t n = 0;
    uint8_t *b = slurp("bench/space3d/expected.json", &n);
    const char *p;
    if (b == NULL) {
        return 0;
    }
    p = strstr((const char *)b, "\"final_state_sha256\": \"");
    if (p != NULL) {
        memcpy(g_pin_state, p + 23, 64);
    }
    p = strstr((const char *)b, "\"event_log_sha256\": \"");
    if (p != NULL) {
        memcpy(g_pin_events, p + 21, 64);
    }
    p = strstr((const char *)b, "\"events\": ");
    if (p != NULL) {
        g_pin_event_count = strtoull(p + 10, NULL, 10);
    }
    free(b);
    return strlen(g_pin_state) == 64u && strlen(g_pin_events) == 64u && g_pin_event_count > 0u;
}

/* ---------- 1. listing and refusals ---------- */

static void test_refusals(void) {
    SuHostWorldDesc d;
    SuHostCapsule caps[3];
    SuHostWorld *w = NULL;
    SuHostError err;
    const char *sid = NULL, *op = NULL;
    uint32_t st;
    Bytes bad;
    char *t, *at;

    check(su_host_operator_count() >= 1u, "at least one built-in operator");
    st = su_host_operator_info(0, &sid, &op);
    check(st == SU_OK && sid != NULL && strcmp(sid, "space3d.query") == 0 && op != NULL &&
              strcmp(op, "space3d_ops_v1") == 0,
          "operator 0 is space3d.query / space3d_ops_v1");
    check(su_host_operator_info(su_host_operator_count(), &sid, &op) == SU_E_INVALID_ARGUMENT, "operator index range");

    /* Unknown operator: the record names a system no operator of this library implements. */
    t = malloc(g_query.n + 16u);
    memcpy(t, g_query.p, g_query.n + 1u);
    at = strstr(t, "\"space3d.query\"");
    check(at != NULL, "record has its system id");
    if (at != NULL) {
        memcpy(at, "\"space3d.qverb\"", 15);
    }
    bad.p = (uint8_t *)t;
    bad.n = g_query.n;
    desc_init(&d, caps, &bad);
    st = su_host_world_create(&d, &w, &err);
    check(st == SU_E_CAPABILITY_DENIED && err.index == 2u && w == NULL, "unknown operator: %s (%s)",
          su_host_status_name(st), err.message);

    /* A record whose operator lane changed type: the loader refuses it with its reason. */
    memcpy(t, g_query.p, g_query.n + 1u);
    at = strstr(t, "\"name\": \"hit_count\",\n        \"type\": \"u32\"");
    check(at != NULL, "record has hit_count");
    if (at != NULL) {
        memcpy(strstr(at, "\"u32\""), "\"i32\"", 5);
    }
    st = su_host_world_create(&d, &w, &err);
    check(st == SU_E_SCHEMA_MISMATCH && err.index == 2u && w == NULL && strstr(err.message, "host record") != NULL,
          "altered record: %s (%s)", su_host_status_name(st), err.message);
    free(t);

    /* lir_len without lir_json is not a host record. */
    desc_init(&d, caps, &g_query);
    caps[2].lir_len = 1;
    st = su_host_world_create(&d, &w, &err);
    check(st == SU_E_INVALID_ARGUMENT && w == NULL, "lir_len without lir_json: %s", su_host_status_name(st));
}

/* ---------- 2. pinned digests ---------- */

static void test_pinned(void) {
    SuHostWorld *w = make_world(&g_query);
    char sh[65], lh[65];
    uint32_t tier = 0, i;
    SuHostError err;
    if (w == NULL) {
        return;
    }
    check(su_host_system_count(w) == 3u, "three systems");
    for (i = 0; i < 3u; i++) {
        const char *id = NULL, *hash = NULL;
        check(su_host_system_info(w, i, &id, &hash) == SU_OK && hash != NULL && strlen(hash) == 64u, "system info %u",
              (unsigned)i);
    }
    check(su_host_system_tier(w, 2, &tier) == SU_OK && tier == SU_HOST_TIER_OPERATOR, "space3d.query is an operator");
    check(su_host_system_set_tier(w, 2, SU_HOST_TIER_REFERENCE, &err) == SU_E_INVALID_ARGUMENT,
          "an operator's tier is fixed");
    check(su_host_system_set_tier(w, 0, 99u, &err) == SU_E_INVALID_ARGUMENT, "unknown tier refused");
    for (i = 0; i < 6u; i++) {
        if (!step_n(w, 120)) {
            break;
        }
    }
    section_digests(w, sh, lh);
    check(su_host_tick(w) == 720u, "tick 720");
    check(strcmp(sh, g_pin_state) == 0, "final state %s equals the pinned in-process digest %s", sh, g_pin_state);
    check(strcmp(lh, g_pin_events) == 0, "event log %s equals the pinned digest %s", lh, g_pin_events);
    check(su_host_event_next_seq(w) == g_pin_event_count, "event count %llu", (unsigned long long)su_host_event_next_seq(w));
    su_host_world_destroy(w);
}

/* ---------- 3. rollback through the host API ---------- */

#define MAX_K 12u

typedef struct TickRef {
    uint8_t sha[32];
    uint64_t fast;
    uint32_t nev;
    SuHostEvent ev[8];
} TickRef;

static void tick_ref(const SuHostWorld *w, TickRef *r) {
    uint32_t total = 0, complete = 0;
    memset(r, 0, sizeof *r);
    check(su_host_digest(w, r->sha) == SU_OK, "digest");
    check(su_host_digest_fast(w, &r->fast) == SU_OK, "fast digest");
    check(su_host_events_for_tick(w, su_host_tick(w), r->ev, 8u, &r->nev, &total, &complete) == SU_OK &&
              total == r->nev && complete == 1u,
          "events of tick %llu", (unsigned long long)su_host_tick(w));
}

static int same_tick(const TickRef *a, const TickRef *b) {
    return memcmp(a->sha, b->sha, 32) == 0 && a->fast == b->fast && a->nev == b->nev &&
           memcmp(a->ev, b->ev, (size_t)a->nev * sizeof a->ev[0]) == 0;
}

static void test_rollback(void) {
    SuHostWorld *w = make_world(&g_query);
    SuHostEvent evs[64];
    uint32_t n = 0, i, k, it;
    uint64_t next = 0, anchor = 0, gate_seq = UINT64_MAX;
    uint8_t *snap = NULL;
    size_t snap_len = 0, got = 0;
    TickRef ref[MAX_K + 1u];
    SuHostError err;
    if (w == NULL) {
        return;
    }
    /* Find the first gate event of the run (kind 1 from racer.race) and anchor 6 ticks before it. */
    if (!step_n(w, 720)) {
        su_host_world_destroy(w);
        return;
    }
    check(su_host_events_read(w, 0, evs, 64u, &n, &next, NULL) == SU_OK && n > 0u, "events of the run");
    for (i = 0; i < n; i++) {
        if (evs[i].system_index == 0u && evs[i].tick > 10u) {
            anchor = evs[i].tick - 6u;
            gate_seq = evs[i].event_seq;
            break;
        }
    }
    check(anchor > 0u, "a gate event to anchor on");
    su_host_world_destroy(w);
    w = make_world(&g_query);
    if (w == NULL || anchor == 0u) {
        su_host_world_destroy(w);
        return;
    }
    step_n(w, anchor);
    su_host_snapshot_size(w, &snap_len);
    snap = malloc(snap_len);
    check(snap != NULL && su_host_snapshot_write(w, snap, snap_len, &got) == SU_OK && got == snap_len, "anchor snapshot");
    tick_ref(w, &ref[0]);
    for (k = 1; k <= MAX_K; k++) {
        step_n(w, 1);
        tick_ref(w, &ref[k]);
    }
    /* The gate event is inside the window and was emitted by the first pass. */
    {
        int seen = 0;
        for (k = 1; k <= MAX_K; k++) {
            for (i = 0; i < ref[k].nev; i++) {
                seen |= ref[k].ev[i].event_seq == gate_seq;
            }
        }
        check(seen, "gate event %llu inside the rollback window", (unsigned long long)gate_seq);
    }
    for (it = 0; it < 50u; it++) {
        uint32_t kk = it % 2u == 0u ? 6u : 12u;
        uint32_t st = it % 5u == 4u ? su_host_snapshot_restore(w, snap, snap_len, &err)
                                    : su_host_snapshot_restore_inplace(w, snap, snap_len, &err);
        TickRef r;
        check(st == SU_OK && su_host_tick(w) == anchor, "restore %u: %s", (unsigned)it, su_host_status_name(st));
        tick_ref(w, &r);
        check(same_tick(&r, &ref[0]), "restored state equals the anchor (iteration %u)", (unsigned)it);
        if (it % 3u == 0u) {
            /* Re-simulate tick by tick and compare every tick. */
            for (k = 1; k <= kk; k++) {
                step_n(w, 1);
                tick_ref(w, &r);
                check(same_tick(&r, &ref[k]), "iteration %u tick +%u equals the first pass", (unsigned)it, (unsigned)k);
            }
        } else {
            /* K ticks in one call (the rollback path), compare the end state and every tick's events. */
            step_n(w, kk);
            tick_ref(w, &r);
            check(same_tick(&r, &ref[kk]), "iteration %u: K = %u in one call equals the first pass", (unsigned)it,
                  (unsigned)kk);
            for (k = 1; k <= kk; k++) {
                SuHostEvent ev[8];
                uint32_t c = 0;
                su_host_events_for_tick(w, anchor + k, ev, 8u, &c, NULL, NULL);
                check(c == ref[k].nev && memcmp(ev, ref[k].ev, (size_t)c * sizeof ev[0]) == 0,
                      "iteration %u: events of tick +%u re-emitted identically", (unsigned)it, (unsigned)k);
            }
        }
    }
    free(snap);
    su_host_world_destroy(w);
}

/* ---------- 4. replay ---------- */

static void test_replay(void) {
    SuHostWorld *w = make_world(&g_query), *v = NULL;
    SuReplayWriter *rw = NULL;
    SuReplayOptions o;
    SuReplay *r = NULL;
    SuReplayInfo info;
    SuHostError err;
    uint8_t *bytes = NULL;
    size_t len = 0;
    uint64_t done = 0;
    uint32_t st;
    char sh[65], lh[65];
    if (w == NULL) {
        return;
    }
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.keyframe_interval = 120;
    o.digest_interval = 30;
    st = su_replay_writer_begin(w, &o, &rw, &err);
    check(st == SU_OK, "replay begin: %s", err.message);
    if (st == SU_OK) {
        st = su_replay_writer_step(rw, w, 720, &done, &err);
        check(st == SU_OK && done == 720u, "replay step: %s", err.message);
        st = su_replay_writer_finish(rw, w, &bytes, &len);
        check(st == SU_OK && len > 0u, "replay finish");
        su_replay_writer_destroy(rw);
    }
    if (bytes != NULL) {
        st = su_replay_open(bytes, len, &r, &err);
        check(st == SU_OK, "replay open: %s", err.message);
        v = make_world(&g_query);
        if (r != NULL && v != NULL) {
            check(su_replay_info(r, &info) == SU_OK && info.system_count == 3u && info.end_tick == 720u,
                  "replay info");
            st = su_replay_seek(r, v, 500, &err);
            check(st == SU_OK && su_host_tick(v) == 500u, "replay seek 500: %s", err.message);
            st = su_replay_play(r, v, 220, &done, &err);
            check(st == SU_OK && done == 220u, "replay play to the end: %s", err.message);
            section_digests(v, sh, lh);
            check(strcmp(sh, g_pin_state) == 0, "replayed end state equals the pin");
            /* The seek restored a keyframe, so the log matches from the keyframe on: compare whole snapshots. */
            {
                uint8_t a[32], b[32];
                su_host_digest(w, a);
                su_host_digest(v, b);
                check(memcmp(a, b, 32) == 0, "replayed snapshot equals the recorded world's");
            }
        }
        su_host_world_destroy(v);
        su_replay_close(r);
        su_replay_free(bytes);
    }
    su_host_world_destroy(w);
}

/* ---------- 5. identity-bound record ---------- */

static Bytes identity_bound_record(void) {
    /* Every host.fields entry gains "binds": "<its own name>" (a schema-relative binding onto the same lanes). */
    Bytes out;
    const char *src = (const char *)g_query.p;
    char *dst = malloc(g_query.n * 2u + 1u), *o = dst;
    const char *p = src, *q;
    while ((q = strstr(p, "\"name\": \"")) != NULL) {
        const char *name = q + 9, *end = strchr(name, '"');
        size_t nl = (size_t)(end - name);
        memcpy(o, p, (size_t)(end + 1 - p));
        o += end + 1 - p;
        o += sprintf(o, ", \"binds\": \"%.*s\"", (int)nl, name);
        p = end + 1;
    }
    strcpy(o, p);
    out.p = (uint8_t *)dst;
    out.n = strlen(dst);
    return out;
}

static void test_identity_bound(void) {
    Bytes rec = identity_bound_record();
    SuHostWorld *a = make_world(&g_query), *b = make_world(&rec);
    uint8_t ia[32], ib[32];
    char sa[65], sb[65], la[65], lb[65];
    if (a != NULL && b != NULL) {
        su_host_world_identity(a, ia);
        su_host_world_identity(b, ib);
        check(memcmp(ia, ib, 32) != 0, "a bound record changes the world identity (its semantic hash covers the binding)");
        step_n(a, 240);
        step_n(b, 240);
        section_digests(a, sa, la);
        section_digests(b, sb, lb);
        check(strcmp(sa, sb) == 0 && strcmp(la, lb) == 0, "identity-bound record: same state and events");
    }
    su_host_world_destroy(a);
    su_host_world_destroy(b);
    free(rec.p);
}

/* ---------- 6. independence ---------- */

typedef struct ThreadJob {
    SuHostWorld *w;
    char state[65], log[65];
} ThreadJob;

#if defined(_WIN32)
static unsigned __stdcall thread_main(void *arg) {
#else
static void *thread_main(void *arg) {
#endif
    ThreadJob *j = arg;
    uint64_t done = 0, t;
    for (t = 0; t < 720u; t++) {
        if (su_host_step(j->w, 1, &done, NULL) != SU_OK) {
            break;
        }
    }
    section_digests(j->w, j->state, j->log);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_independence(void) {
    ThreadJob a, b;
    SuHostWorld *x, *y;
    char sx[65], lx[65];
    uint32_t t;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.w = make_world(&g_query);
    b.w = make_world(&g_query);
    if (a.w != NULL && b.w != NULL) {
#if defined(_WIN32)
        HANDLE ha = (HANDLE)_beginthreadex(NULL, 0, thread_main, &a, 0, NULL);
        HANDLE hb = (HANDLE)_beginthreadex(NULL, 0, thread_main, &b, 0, NULL);
        check(ha != 0 && hb != 0, "threads start");
        if (ha != 0) {
            WaitForSingleObject(ha, INFINITE);
            CloseHandle(ha);
        }
        if (hb != 0) {
            WaitForSingleObject(hb, INFINITE);
            CloseHandle(hb);
        }
#else
        pthread_t ta, tb;
        int ra = pthread_create(&ta, NULL, thread_main, &a);
        int rb = pthread_create(&tb, NULL, thread_main, &b);
        check(ra == 0 && rb == 0, "threads start");
        if (ra == 0) {
            pthread_join(ta, NULL);
        }
        if (rb == 0) {
            pthread_join(tb, NULL);
        }
#endif
        check(strcmp(a.state, g_pin_state) == 0 && strcmp(a.log, g_pin_events) == 0, "thread 1 reaches the pin");
        check(strcmp(b.state, g_pin_state) == 0 && strcmp(b.log, g_pin_events) == 0, "thread 2 reaches the pin");
    }
    su_host_world_destroy(a.w);
    su_host_world_destroy(b.w);
    /* Interleaved on one thread, y 97 ticks behind x. */
    x = make_world(&g_query);
    y = make_world(&g_query);
    if (x != NULL && y != NULL) {
        step_n(x, 97);
        for (t = 0; t < 720u; t++) {
            if (su_host_tick(x) < 720u) {
                step_n(x, 1);
            }
            step_n(y, 1);
        }
        section_digests(x, sx, lx);
        check(strcmp(sx, g_pin_state) == 0, "interleaved world x reaches the pin");
        section_digests(y, sx, lx);
        check(strcmp(sx, g_pin_state) == 0 && strcmp(lx, g_pin_events) == 0, "interleaved world y reaches the pin");
    }
    su_host_world_destroy(x);
    su_host_world_destroy(y);
}

/* ---------- 7. tiers ---------- */

static void tier_run(uint32_t race_tier, uint32_t move_tier, const char *name) {
    SuHostWorld *w = make_world(&g_query);
    SuHostError err;
    char sh[65], lh[65];
    uint32_t got = 0, st;
    if (w == NULL) {
        return;
    }
    st = su_host_system_set_tier(w, 0, race_tier, &err);
    check(st == SU_OK, "%s: racer.race tier %u: %s", name, (unsigned)race_tier, err.message);
    st = su_host_system_set_tier(w, 1, move_tier, &err);
    check(st == SU_OK, "%s: racer.move tier %u: %s (%s)", name, (unsigned)move_tier, su_host_status_name(st),
          err.message);
    check(su_host_system_tier(w, 1, &got) == SU_OK && got == move_tier, "%s: racer.move reports tier %u", name,
          (unsigned)got);
    step_n(w, 720);
    section_digests(w, sh, lh);
    check(strcmp(sh, g_pin_state) == 0 && strcmp(lh, g_pin_events) == 0, "%s: the pinned digests", name);
    su_host_world_destroy(w);
}

static void test_tiers(void) {
    tier_run(SU_HOST_TIER_REFERENCE, SU_HOST_TIER_REFERENCE, "reference");
    tier_run(SU_HOST_TIER_PLANNED, SU_HOST_TIER_PLANNED, "planned");
    if (su_host_native_available()) {
        SuHostWorld *w = make_world(&g_query);
        SuHostError err;
        uint32_t tier = 0;
        tier_run(SU_HOST_TIER_PLANNED, SU_HOST_TIER_NATIVE, "native racer.move");
        if (w != NULL) {
            uint32_t st = su_host_system_set_tier(w, 0, SU_HOST_TIER_NATIVE, &err);
            check(st == SU_E_CAPABILITY_DENIED, "racer.race (emit_event) stays on the VM: %s", su_host_status_name(st));
            /* Native, then back to the reference tier mid-run: still the pin. */
            check(su_host_system_set_tier(w, 1, SU_HOST_TIER_NATIVE, &err) == SU_OK, "native install: %s", err.message);
            step_n(w, 300);
            check(su_host_system_set_tier(w, 1, SU_HOST_TIER_REFERENCE, &err) == SU_OK &&
                      su_host_system_tier(w, 1, &tier) == SU_OK && tier == SU_HOST_TIER_REFERENCE,
                  "back to the reference tier");
            step_n(w, 420);
            {
                char sh[65], lh[65];
                section_digests(w, sh, lh);
                check(strcmp(sh, g_pin_state) == 0, "native then reference: the pin");
            }
            su_host_world_destroy(w);
        }
        printf("host_ops: native tier exercised\n");
    } else {
        SuHostWorld *w = make_world(&g_query);
        SuHostError err;
        if (w != NULL) {
            check(su_host_system_set_tier(w, 1, SU_HOST_TIER_NATIVE, &err) == SU_E_CAPABILITY_DENIED,
                  "no native backend in this build");
            su_host_world_destroy(w);
        }
        printf("host_ops: native tier not built (SUTEKH_NATIVE=OFF); refusal checked\n");
    }
}

static int load(Bytes *b, const char *path) {
    b->p = slurp(path, &b->n);
    if (b->p == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_host_ops <racer fixture json>  (run from the repository root)\n");
        return 2;
    }
    if (!load(&g_race_cap, "capsules/space3d_racer/race.capsule.json") ||
        !load(&g_race_lir, "capsules/space3d_racer/race.lir.json") ||
        !load(&g_move_cap, "capsules/space3d_racer/move.capsule.json") ||
        !load(&g_move_lir, "capsules/space3d_racer/move.lir.json") ||
        !load(&g_query, "profiles/space3d/space3d_query.capsule.json") || !load(&g_fixture, argv[1]) ||
        !read_pins()) {
        return 2;
    }
    test_refusals();
    test_pinned();
    test_rollback();
    test_replay();
    test_identity_bound();
    test_independence();
    test_tiers();
    printf("host_ops: %u checks, %u failures\n", g_checks, g_failures);
    free(g_race_cap.p);
    free(g_race_lir.p);
    free(g_move_cap.p);
    free(g_move_lir.p);
    free(g_query.p);
    free(g_fixture.p);
    return g_failures == 0u ? 0 : 1;
}
