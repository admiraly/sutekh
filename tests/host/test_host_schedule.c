/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Issue 144: schedules of up to SU_HOST_MAX_CAPSULES (64) systems and world schemas declared independently of the
 * capsules (include/sutekh/host.h, "declared world schemas"). Built against the static library and the DLL.
 *
 *   1. A 64-system chain (sched_fixtures.h): schedule order, every column against a reference model after every
 *      tick (the result depends on capsule order), two worlds digest-identical, a pinned digest; snapshot
 *      (allocating and in-place restore) and replay round trips; the struct_size of the first release.
 *   2. Admission: 65 capsules (SU_E_RESOURCE_LIMIT, err->index 64); effect analysis at 64 systems (a missing edge is
 *      a named read/write conflict, an edge against the capsule order is refused with its capsule index); the
 *      entity tick budget of 64 event-emitting systems (refused at a capacity that 8 of them fit), and 64 such
 *      systems stepping with events in (system, slot) order; with the native backend, the 32-kernel limit.
 *   3. A declared schema: fields only the host writes (input records, spawn values, i64 lanes) read by later IR
 *      systems (IR 0.1 and an IR 0.3 i64 system), a registered operator reading a field no IR system binds, a
 *      field nobody binds; field info; snapshot and replay round trips; the identity differs from the same world
 *      without a declared schema.
 *   2b. Operator data on a host entry at schedule index 40 (the per-entry data mask is 64 bits wide).
 *   2c. (static build) the schedule itself: su_sched_init keeps the control plane's cap of 8, a raised cap admits 64
 *      and refuses the 65th, and su_sched_declare_schema only on an empty schedule.
 *   4. Refusals: malformed declarations (SU_E_INVALID_ARGUMENT with the field index), capsules or operator records
 *      that disagree with the declared schema in ID, name, value type, schema ID, version or generation, or bind a
 *      field it lacks (SU_E_SCHEMA_MISMATCH with the capsule index), a fixture missing a declared column. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"
#include "sutekh/replay.h"
#include "sched_fixtures.h"
#ifdef SU_TEST_INTERNAL
#include "ir/su_lir.h"
#include "runtime/schedule.h"
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

static void desc_init(SuHostWorldDesc *d, const SuHostCapsule *caps, uint32_t n, const Buf *fixture,
                      uint64_t max_entities) {
    memset(d, 0, sizeof *d);
    d->struct_size = (uint32_t)sizeof *d;
    d->capsule_count = n;
    d->capsules = caps;
    if (fixture != NULL) {
        d->fixture_json = (const uint8_t *)fixture->p;
        d->fixture_len = fixture->n;
    }
    d->max_entities = max_entities;
    d->tick_rate_hz = 60u;
    d->event_log_max = 1024u;
    d->seed = 144u;
}

static uint8_t *snapshot_of(const SuHostWorld *w, size_t *len) {
    size_t n = 0, got = 0;
    uint8_t *b;
    if (su_host_snapshot_size(w, &n) != SU_OK || (b = malloc(n)) == NULL) {
        return NULL;
    }
    if (su_host_snapshot_write(w, b, n, &got) != SU_OK || got != n) {
        free(b);
        return NULL;
    }
    *len = n;
    return b;
}

static void digest_hex(const SuHostWorld *w, char hex[65]) {
    uint8_t d[32];
    memset(d, 0, sizeof d);
    su_host_digest(w, d);
    su_host_hex(d, hex);
}

/* ---------- 1. a 64-system chain ---------- */

#define CHAIN_ROWS 8u
#define CHAIN_TICKS 10u
/* SHA-256 digest of the 64-system chain at tick 10 (first recorded by this test; any change is a behaviour change). */
#define CHAIN64_DIGEST "14661fd4e393e22ea9c92d3681e2d1b9732f47d85c72877a9dca14dbb73be251"

static int chain_matches(const SuHostWorld *w, uint32_t n, uint32_t model[][CHAIN_MAX + 1u]) {
    uint32_t f, r;
    for (f = 0; f <= n; f++) {
        uint32_t col[CHAIN_ROWS];
        if (su_host_read(w, f + 1u, 0, CHAIN_ROWS, col) != SU_OK) {
            return 0;
        }
        for (r = 0; r < CHAIN_ROWS; r++) {
            if (col[r] != model[r][f]) {
                return 0;
            }
        }
    }
    return 1;
}

static void test_chain64(void) {
    Chain c;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL, *w2 = NULL, *wv1 = NULL;
    SuHostError err;
    uint32_t model[CHAIN_ROWS][CHAIN_MAX + 1u];
    uint32_t r, f, t, k;
    char hex[65], hex2[65], d5[65] = "", d10[65];
    uint8_t *snap5 = NULL, *rep = NULL;
    size_t snap5_len = 0, rep_len = 0;
    uint64_t done = 0;
    int order_ok = 1, model_ok = 1, same = 1;

    chain_build(&c, 64u, CHAIN_ROWS, NULL);
    desc_init(&d, c.caps, 64u, &c.fixture, 0u);
    check(su_host_world_create(&d, &w, &err) == SU_OK, "64-system chain world: %s", err.message);
    check(su_host_world_create(&d, &w2, &err) == SU_OK, "second 64-system chain world: %s", err.message);
    if (w == NULL || w2 == NULL) {
        su_host_world_destroy(w);
        su_host_world_destroy(w2);
        chain_free(&c);
        return;
    }
    check(su_host_system_count(w) == 64u, "64 systems, got %u", su_host_system_count(w));
    check(su_host_field_count(w) == 65u, "65 lanes, got %u", su_host_field_count(w));
    for (k = 0; k < 64u; k++) {
        const char *sid = NULL;
        char want[32];
        snprintf(want, sizeof want, "chain.s%02u", (unsigned)k);
        order_ok &= su_host_system_info(w, k, &sid, NULL) == SU_OK && sid != NULL && strcmp(sid, want) == 0;
    }
    check(order_ok, "schedule order is the capsule order for 64 systems");
    check(su_host_system_info(w, 64u, NULL, NULL) == SU_E_INVALID_ARGUMENT, "system index 64 is out of range");

    for (r = 0; r < CHAIN_ROWS; r++) {
        for (f = 0; f <= 64u; f++) {
            model[r][f] = chain_init(r, f);
        }
    }
    for (t = 0; t < CHAIN_TICKS; t++) {
        check(su_host_step(w, 1u, &done, &err) == SU_OK && done == 1u, "chain tick %u: %s", (unsigned)t, err.message);
        check(su_host_step(w2, 1u, &done, &err) == SU_OK && done == 1u, "chain2 tick %u", (unsigned)t);
        for (r = 0; r < CHAIN_ROWS; r++) {
            chain_model_tick(model[r], 64u);
        }
        model_ok &= chain_matches(w, 64u, model);
        digest_hex(w, hex);
        digest_hex(w2, hex2);
        same &= strcmp(hex, hex2) == 0;
        if (t + 1u == 5u) {
            snap5 = snapshot_of(w, &snap5_len);
            snprintf(d5, sizeof d5, "%s", hex);
        }
    }
    check(model_ok, "every column of the 64-system chain equals the reference model after every tick");
    check(same, "two 64-system worlds have the same digest after every tick");
    digest_hex(w, d10);
    check(strcmp(d10, CHAIN64_DIGEST) == 0, "64-system chain digest at tick 10 is %s, pinned %s", d10, CHAIN64_DIGEST);

    /* Snapshot round trips: allocating and in-place restore, then re-simulation to tick 10. */
    check(snap5 != NULL, "snapshot at tick 5");
    if (snap5 != NULL) {
        check(su_host_snapshot_restore(w, snap5, snap5_len, &err) == SU_OK && su_host_tick(w) == 5u,
              "restore at tick 5: %s", err.message);
        digest_hex(w, hex);
        check(strcmp(hex, d5) == 0, "restored digest equals tick 5");
        check(su_host_step(w, 5u, &done, &err) == SU_OK && done == 5u, "re-simulate to 10");
        digest_hex(w, hex);
        check(strcmp(hex, d10) == 0, "re-simulated digest equals tick 10");
        check(su_host_snapshot_restore_inplace(w2, snap5, snap5_len, &err) == SU_OK && su_host_tick(w2) == 5u,
              "in-place restore at tick 5: %s", err.message);
        check(su_host_step(w2, 5u, &done, &err) == SU_OK && done == 5u, "re-simulate in place to 10");
        digest_hex(w2, hex);
        check(strcmp(hex, d10) == 0, "in-place re-simulated digest equals tick 10");
    }

    /* Replay: record 10 ticks from a fresh world (inputs at tick 3), seek and play in another one. */
    {
        SuHostWorld *a = NULL, *b = NULL;
        SuReplayWriter *rw = NULL;
        SuReplayOptions ro;
        SuReplay *rp = NULL;
        SuReplayInfo info;
        char end_hex[65];
        memset(&ro, 0, sizeof ro);
        ro.struct_size = (uint32_t)sizeof ro;
        ro.keyframe_interval = 4u;
        ro.digest_interval = 1u;
        check(su_host_world_create(&d, &a, &err) == SU_OK && su_host_world_create(&d, &b, &err) == SU_OK,
              "replay worlds");
        if (a != NULL && b != NULL) {
            SuHostInput in[CHAIN_ROWS];
            check(su_replay_writer_begin(a, &ro, &rw, &err) == SU_OK, "replay begin: %s", err.message);
            check(su_replay_writer_step(rw, a, 3u, &done, &err) == SU_OK && done == 3u, "record 3 ticks");
            for (r = 0; r < CHAIN_ROWS; r++) {
                su_host_entity_at(a, r, &in[r].entity);
                in[r].field_id = 1u + 63u; /* v63: read by chain.s63, written by chain.s62 */
                in[r].value = 1000u + r;
            }
            check(su_host_input_push(a, 3u, in, CHAIN_ROWS, &err) == SU_OK, "input at tick 3: %s", err.message);
            check(su_replay_writer_step(rw, a, 7u, &done, &err) == SU_OK && done == 7u, "record 7 ticks");
            check(su_replay_writer_finish(rw, a, &rep, &rep_len) == SU_OK, "replay finish");
            su_replay_writer_destroy(rw);
            digest_hex(a, end_hex);
            check(su_replay_open(rep, rep_len, &rp, &err) == SU_OK, "replay open: %s", err.message);
            if (rp != NULL) {
                const char *sid = NULL;
                check(su_replay_info(rp, &info) == SU_OK && info.system_count == 64u && info.input_count == 1u,
                      "replay header: 64 systems, 1 input");
                check(su_replay_system(rp, 63u, &sid, NULL, NULL) == SU_OK && sid != NULL &&
                          strcmp(sid, "chain.s63") == 0,
                      "replay system 63");
                check(su_replay_seek(rp, b, 6u, &err) == SU_OK && su_host_tick(b) == 6u, "replay seek to 6: %s",
                      err.message);
                check(su_replay_play(rp, b, 100u, &done, &err) == SU_OK && done == 4u, "replay play to the end: %s",
                      err.message);
                digest_hex(b, hex);
                check(strcmp(hex, end_hex) == 0, "replayed digest equals the recording");
                su_replay_close(rp);
            }
        }
        su_host_world_destroy(a);
        su_host_world_destroy(b);
    }

    /* The first release's descriptor size (no schema field) creates the same world. */
    d.struct_size = (uint32_t)SU_HOST_WORLD_DESC_V1_SIZE;
    check(su_host_world_create(&d, &wv1, &err) == SU_OK, "v1-size descriptor: %s", err.message);
    if (wv1 != NULL) {
        uint8_t i1[32], i2[32];
        su_host_world_identity(wv1, i1);
        su_host_world_identity(w2, i2);
        check(memcmp(i1, i2, 32u) == 0, "v1-size descriptor: same identity");
        su_host_world_destroy(wv1);
    }
    d.struct_size = (uint32_t)SU_HOST_WORLD_DESC_V1_SIZE - 8u;
    check(su_host_world_create(&d, &wv1, &err) == SU_E_INVALID_ARGUMENT && wv1 == NULL, "a short descriptor");
    d.struct_size = (uint32_t)SU_HOST_WORLD_DESC_V1_SIZE + 4u;
    check(su_host_world_create(&d, &wv1, &err) == SU_E_INVALID_ARGUMENT && wv1 == NULL,
          "a size ending inside the schema pointer");
    /* Larger than the library's descriptor: a zero tail is accepted, a nonzero one refused. */
    {
        unsigned char big[sizeof(SuHostWorldDesc) + 24u];
        SuHostWorldDesc *bd = (SuHostWorldDesc *)(void *)big;
        uint8_t i1[32], i2[32];
        d.struct_size = (uint32_t)sizeof d;
        memset(big, 0, sizeof big);
        memcpy(big, &d, sizeof d);
        bd->struct_size = (uint32_t)sizeof big;
        check(su_host_world_create(bd, &wv1, &err) == SU_OK, "a larger descriptor with a zero tail: %s", err.message);
        if (wv1 != NULL) {
            su_host_world_identity(wv1, i1);
            su_host_world_identity(w2, i2);
            check(memcmp(i1, i2, 32u) == 0, "larger descriptor: same identity");
            su_host_world_destroy(wv1);
            wv1 = NULL;
        }
        big[sizeof(SuHostWorldDesc) + 17u] = 0x80u;
        check(su_host_world_create(bd, &wv1, &err) == SU_E_INVALID_ARGUMENT && wv1 == NULL &&
                  strstr(err.message, "does not know") != NULL,
              "a larger descriptor with a nonzero tail: %s", err.message);
    }

    su_replay_free(rep);
    free(snap5);
    su_host_world_destroy(w);
    su_host_world_destroy(w2);
    chain_free(&c);
}

/* ---------- 2. admission ---------- */

static void test_admission(void) {
    Chain c;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError err;
    static const char *edges[CHAIN_MAX];
    uint32_t k;

    /* 65 capsules: a structured refusal naming the first capsule that does not fit. */
    chain_build(&c, 65u, CHAIN_ROWS, NULL);
    desc_init(&d, c.caps, 65u, &c.fixture, 0u);
    check(su_host_world_create(&d, &w, &err) == SU_E_RESOURCE_LIMIT && w == NULL, "65 capsules are refused (%s)",
          su_host_status_name(err.code));
    check(err.code == SU_E_RESOURCE_LIMIT && err.index == 64u && strstr(err.message, "at most 64") != NULL,
          "65 capsules: index %u, message %s", err.index, err.message);
    chain_free(&c);
    desc_init(&d, NULL, 0u, NULL, 4u);
    check(su_host_world_create(&d, &w, &err) == SU_E_INVALID_ARGUMENT, "0 capsules are SU_E_INVALID_ARGUMENT");

    /* Effect analysis at 64 systems: system 40 without its edge conflicts with system 39 (read/write on v40). */
    for (k = 0; k < 64u; k++) {
        edges[k] = NULL;
    }
    edges[40] = "";
    chain_build(&c, 64u, CHAIN_ROWS, edges);
    desc_init(&d, c.caps, 64u, &c.fixture, 0u);
    check(su_host_world_create(&d, &w, &err) == SU_E_SCHEMA_MISMATCH && w == NULL, "missing edge is refused");
    check(strstr(err.message, "chain.s39") != NULL && strstr(err.message, "chain.s40") != NULL &&
              strstr(err.message, "field 41") != NULL,
          "missing edge names the pair and field: %s", err.message);
    chain_free(&c);
    /* An edge against the capsule order (system 10 after system 20): with the chain's edges it closes a cycle through
     * systems 10..20, refused by the effect analysis. */
    edges[40] = NULL;
    edges[10] = "chain.s20";
    chain_build(&c, 64u, CHAIN_ROWS, edges);
    desc_init(&d, c.caps, 64u, &c.fixture, 0u);
    check(su_host_world_create(&d, &w, &err) != SU_OK && w == NULL, "edge against the order is refused");
    check(err.code == SU_E_INVALID_ARGUMENT && strstr(err.message, "ordering cycle") != NULL,
          "edge against the order: %s", err.message);
    chain_free(&c);
    edges[10] = NULL;

    /* Edge lists are refused, not truncated: more than 64 items (SU_E_RESOURCE_LIMIT), a non-string item or a
     * non-array (SU_E_TYPE), each with the capsule index; exactly 64 items (repeats of one edge) are accepted. */
    {
        static char many[65u * 12u + 1u], sixty_four[64u * 12u + 1u];
        uint32_t q;
        size_t at = 0, at64 = 0;
        for (q = 0; q < 65u; q++) {
            /* chain_capsule writes ["<edge>"]: items joined by "," give a list of q + 1 strings. */
            at += (size_t)snprintf(many + at, sizeof many - at, "%schain.s00", q > 0u ? "\",\"" : "");
            if (q < 64u) {
                at64 += (size_t)snprintf(sixty_four + at64, sizeof sixty_four - at64, "%schain.s00",
                                         q > 0u ? "\",\"" : "");
            }
        }
        edges[1] = many;
        chain_build(&c, 3u, CHAIN_ROWS, edges);
        desc_init(&d, c.caps, 3u, &c.fixture, 0u);
        check(su_host_world_create(&d, &w, &err) == SU_E_RESOURCE_LIMIT && w == NULL && err.index == 1u &&
                  strstr(err.message, "schedule.after lists 65 items") != NULL,
              "65 after items are refused: %s", err.message);
        chain_free(&c);
        edges[1] = sixty_four;
        chain_build(&c, 3u, CHAIN_ROWS, edges);
        desc_init(&d, c.caps, 3u, &c.fixture, 0u);
        check(su_host_world_create(&d, &w, &err) == SU_OK, "64 after items are accepted: %s", err.message);
        su_host_world_destroy(w);
        w = NULL;
        chain_free(&c);
        edges[1] = "chain.s00\",7,\"chain.s00";
        chain_build(&c, 3u, CHAIN_ROWS, edges);
        desc_init(&d, c.caps, 3u, &c.fixture, 0u);
        check(su_host_world_create(&d, &w, &err) == SU_E_TYPE && w == NULL && err.index == 1u &&
                  strstr(err.message, "schedule.after item 1 is not a string") != NULL,
              "a non-string after item is refused: %s (%s)", err.message, su_host_status_name(err.code));
        chain_free(&c);
        edges[1] = NULL;
    }
}

/* Event-emitting systems for the entity budget: "emit.sKK" reads x and emits kind 100 + K for every row with x != 0. */
static Buf emit_capsule(uint32_t k) {
    Buf b = {NULL, 0, 0};
    put(&b, "{\"capsule_format\":\"0.1\",\"ir_version\":\"0.2\",\"system_id\":\"emit.s%02u\",\"source\":\"inline.lir.json\",",
        (unsigned)k);
    put(&b, "\"schema\":{\"id\":\"t.emit\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"u32_mod_v1\",");
    put(&b, "\"fields\":[{\"id\":1,\"name\":\"x\",\"type\":\"u32\",\"read\":true,\"write\":false}],");
    put(&b, "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},");
    put(&b, "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\","
            "\"max_struct_per_row\":0,\"max_events_per_row\":1},");
    put(&b, "\"tests\":[\"tests/host/test_host_schedule.c\"],\"capabilities\":[\"emit_event\"]}");
    return b;
}

static Buf emit_lir(uint32_t k) {
    Buf b = {NULL, 0, 0};
    put(&b, "{\"ir_version\":\"0.2\",\"system_id\":\"emit.s%02u\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"t.emit\",",
        (unsigned)k);
    put(&b, "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"x\"},{\"op\":\"const_u32\",\"dst\":\"z\","
            "\"value\":0},{\"op\":\"lt_u32\",\"dst\":\"c\",\"a\":\"z\",\"b\":\"x\"},{\"op\":\"slot_u32\",\"dst\":\"s\"},");
    put(&b, "{\"op\":\"emit_event\",\"cond\":\"c\",\"kind\":%u,\"a\":\"s\",\"b\":\"x\"}]}", (unsigned)(100u + k));
    return b;
}

static void test_entity_budget(void) {
    Buf cap[64], lir[64];
    SuHostCapsule caps[64];
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t k;

    for (k = 0; k < 64u; k++) {
        cap[k] = emit_capsule(k);
        lir[k] = emit_lir(k);
        caps[k].capsule_json = (const uint8_t *)cap[k].p;
        caps[k].capsule_len = cap[k].n;
        caps[k].lir_json = (const uint8_t *)lir[k].p;
        caps[k].lir_len = lir[k].n;
    }
    /* 100,000 rows: 8 emitting systems need 800,000 event slots per tick (fits the 4,194,304 budget), 64 need
     * 6,400,000 (does not). */
    desc_init(&d, caps, 8u, NULL, 100000u);
    check(su_host_world_create(&d, &w, &err) == SU_OK, "8 emitting systems at 100,000 rows: %s", err.message);
    su_host_world_destroy(w);
    w = NULL;
    desc_init(&d, caps, 64u, NULL, 100000u);
    check(su_host_world_create(&d, &w, &err) == SU_E_RESOURCE_LIMIT && w == NULL &&
              strstr(err.message, "entity tick buffers") != NULL,
          "64 emitting systems at 100,000 rows exceed the entity budget: %s", err.message);

    /* 64 emitting systems at 4 rows step; events come in (system, slot) order. */
    desc_init(&d, caps, 64u, NULL, 4u);
    check(su_host_world_create(&d, &w, &err) == SU_OK, "64 emitting systems at 4 rows: %s", err.message);
    if (w != NULL) {
        SuHostValue v = {1u, 5u};
        SuHostEvent ev[300];
        uint64_t ent = 0, done = 0, next = 0;
        uint32_t n = 0, i;
        int ordered = 1;
        for (i = 0; i < 4u; i++) {
            check(su_host_spawn(w, &v, 1u, 0u, &ent, &err) == SU_OK, "spawn %u", (unsigned)i);
        }
        check(su_host_step(w, 1u, &done, &err) == SU_OK && done == 1u, "64 emitting systems step: %s", err.message);
        check(su_host_events_read(w, 4u, ev, 300u, &n, &next, NULL) == SU_OK && n == 256u,
              "256 events after the 4 spawn records, got %u", (unsigned)n);
        for (i = 0; i < n; i++) {
            ordered &= ev[i].system_index == i / 4u && ev[i].slot == i % 4u && ev[i].kind == 100u + i / 4u &&
                       ev[i].tick == 1u;
        }
        check(ordered, "events are in (system, slot) order with system indices 0..63");
        su_host_world_destroy(w);
    }
    for (k = 0; k < 64u; k++) {
        buf_free(&cap[k]);
        buf_free(&lir[k]);
    }
}

static void test_native_limit(void) {
    Chain c;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL, *ref = NULL;
    SuHostError err;
    uint32_t k, ok_n = 0, refused = 0, first_refused = UINT32_MAX;
    uint64_t done = 0;
    char a[65], b[65], why[320] = "";
    if (!su_host_native_available()) {
        printf("native backend not built: the native kernel limit is not exercised\n");
        return;
    }
    chain_build(&c, 64u, CHAIN_ROWS, NULL);
    desc_init(&d, c.caps, 64u, &c.fixture, 0u);
    check(su_host_world_create(&d, &w, &err) == SU_OK && su_host_world_create(&d, &ref, &err) == SU_OK,
          "native chain worlds");
    if (w != NULL && ref != NULL) {
        for (k = 0; k < 64u; k++) {
            uint32_t st = su_host_system_set_tier(w, k, SU_HOST_TIER_NATIVE, &err);
            if (st == SU_OK) {
                ok_n++;
            } else {
                if (first_refused == UINT32_MAX) {
                    first_refused = k;
                    snprintf(why, sizeof why, "%s (%s)", err.message, su_host_status_name(st));
                    check(err.index == k && err.message[0] != '\0', "native refusal names system %u: %s",
                          (unsigned)k, err.message);
                }
                refused++;
            }
        }
        printf("native tier: %u of 64 systems installed, first refusal at system %u: %s\n", (unsigned)ok_n,
               (unsigned)first_refused, why);
        check(ok_n >= 1u && ok_n <= 32u && ok_n + refused == 64u,
              "at most 32 native kernels (%u installed, %u refused)", (unsigned)ok_n, (unsigned)refused);
        check(su_host_step(w, CHAIN_TICKS, &done, &err) == SU_OK && done == CHAIN_TICKS, "mixed-tier chain steps");
        check(su_host_step(ref, CHAIN_TICKS, &done, &err) == SU_OK, "VM chain steps");
        digest_hex(w, a);
        digest_hex(ref, b);
        check(strcmp(a, b) == 0 && strcmp(a, CHAIN64_DIGEST) == 0, "native and VM tiers agree at 64 systems");
    }
    su_host_world_destroy(w);
    su_host_world_destroy(ref);
    chain_free(&c);
}

/* ---------- 2b. operator data at a high schedule index ---------- */

#define F_OPD 200u

static const SuHostOperatorField k_opd_fields[2] = {{1u, SU_TYPE_U32, SU_ACCESS_READ, 0, "v00"},
                                                     {F_OPD, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "opd"}};
static unsigned g_opd_parses;

static uint32_t opd_parse(const uint8_t *data, size_t len, void **parsed, char *why, size_t why_cap, void *user) {
    uint32_t *v;
    (void)user;
    if (len != 4u) {
        snprintf(why, why_cap, "4 bytes expected");
        return SU_E_PARSE;
    }
    v = malloc(sizeof *v);
    if (v == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    *v = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    *parsed = v;
    g_opd_parses++;
    return SU_OK;
}

static void opd_release(void *parsed, void *user) {
    (void)user;
    free(parsed);
}

/* opd = v00 + data (0xDEAD0000 without data). */
static uint32_t opd_run(const SuHostOperatorRun *r, void *user) {
    uint64_t i;
    uint32_t add = r->data != NULL ? *(const uint32_t *)r->data : 0xDEAD0000u;
    (void)user;
    for (i = 0; i < r->rows; i++) {
        r->columns[1].out[i] = r->columns[0].in[i] + add;
    }
    return SU_OK;
}

static Buf opd_record(void) {
    Buf b = {NULL, 0, 0};
    uint32_t i;
    put(&b, "{\"capsule_format\":\"0.1\",\"system_id\":\"test.opd\",\"system_rev\":1,\"capabilities\":[],");
    put(&b, "\"schema\":{\"id\":\"t.chain\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"u32_mod_v1\",");
    put(&b, "\"schedule\":{\"phase\":\"query\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},");
    put(&b, "\"host\":{\"operator\":\"opd_ops_v1\",\"operator_version\":1,\"constants_hash\":\"");
    for (i = 0; i < 32u; i++) {
        put(&b, "%02x", (unsigned)(0x40u + i));
    }
    put(&b, "\",\"fields\":[{\"id\":1,\"name\":\"v00\",\"type\":\"u32\",\"access\":\"read\"},");
    put(&b, "{\"id\":%u,\"name\":\"opd\",\"type\":\"u32\",\"access\":\"write\"}]}}", (unsigned)F_OPD);
    return b;
}

static void test_opdata_high_index(void) {
    SuHostOperatorDesc od;
    Chain c;
    Buf rec;
    SuHostCapsule caps[41];
    SuHostWorldDesc d;
    SuHostWorld *w = NULL, *plain = NULL;
    SuHostError err;
    static const uint8_t data[4] = {0x10, 0x20, 0x00, 0x00};
    uint8_t ia[32], ib[32];
    uint32_t k;
    uint64_t done = 0;

    SU_HOST_OPERATOR_DESC_INIT(&od);
    od.system_id = "test.opd";
    od.schema_id = "t.chain";
    od.operator_name = "opd_ops_v1";
    od.operator_version = 1u;
    od.field_count = 2u;
    od.fields = k_opd_fields;
    for (k = 0; k < 32u; k++) {
        od.constants_hash[k] = (uint8_t)(0x40u + k);
    }
    od.run = opd_run;
    od.parse_data = opd_parse;
    od.release_data = opd_release;
    check(su_host_register_operator(&od, &err) == SU_OK, "register test.opd: %s", err.message);

    chain_build(&c, 40u, 1u, NULL);
    rec = opd_record();
    memcpy(caps, c.caps, 40u * sizeof caps[0]);
    caps[40].capsule_json = (const uint8_t *)rec.p;
    caps[40].capsule_len = rec.n;
    caps[40].lir_json = NULL;
    caps[40].lir_len = 0u;
    desc_init(&d, caps, 41u, NULL, 4u);
    check(su_host_world_create(&d, &w, &err) == SU_OK && su_host_world_create(&d, &plain, &err) == SU_OK,
          "40 IR systems and an operator at index 40: %s", err.message);
    if (w != NULL && plain != NULL) {
        SuHostValue v = {1u, 7u};
        uint64_t ent = 0;
        uint32_t out[1] = {0}, out2[1] = {0};
        su_host_world_identity(w, ia);
        check(su_host_set_operator_data(w, "test.opd", data, sizeof data, &err) == SU_OK, "operator data at index 40: %s",
              err.message);
        su_host_world_identity(w, ib);
        check(memcmp(ia, ib, 32u) != 0, "operator data joins the identity");
        check(su_host_set_operator_data(w, "test.opd", data, sizeof data, &err) == SU_E_INVALID_ARGUMENT &&
                  err.index == 40u && strstr(err.message, "already has data") != NULL,
              "a second call names entry 40: %s", err.message);
        check(su_host_set_operator_data(w, NULL, data, sizeof data, &err) == SU_E_INVALID_ARGUMENT && err.index == 40u,
              "a second call without a name names entry 40: %s", err.message);
        su_host_spawn(w, &v, 1u, 0u, &ent, &err);
        su_host_spawn(plain, &v, 1u, 0u, &ent, &err);
        check(su_host_step(w, 1u, &done, &err) == SU_OK && su_host_step(plain, 1u, &done, &err) == SU_OK,
              "operator-data worlds step");
        check(su_host_read(w, F_OPD, 0, 1, out) == SU_OK && out[0] == 7u + 0x2010u, "entry 40 uses its data: %08x",
              (unsigned)out[0]);
        check(su_host_read(plain, F_OPD, 0, 1, out2) == SU_OK && out2[0] == 7u + 0xDEAD0000u,
              "without data: %08x", (unsigned)out2[0]);
    }
    su_host_world_destroy(w);
    su_host_world_destroy(plain);
    buf_free(&rec);
    chain_free(&c);
}

#ifdef SU_TEST_INTERNAL
/* ---------- 2c. the schedule itself (static build only: internal symbols) ---------- */

static SuLirModule *verified(const Chain *c, uint32_t k) {
    SuLirModule *m = calloc(1, sizeof *m);
    SuDiag diag;
    su_diag_init(&diag);
    if (m == NULL || su_lir_verify(c->caps[k].capsule_json, c->caps[k].capsule_len, c->caps[k].lir_json,
                                   c->caps[k].lir_len, m, &diag) != SU_OK) {
        free(m);
        return NULL;
    }
    return m;
}

static void test_schedule_internal(void) {
    Chain c;
    SuSchedule s;
    SuSchedField lane;
    SuLirModule *m;
    uint32_t k, st = SU_OK;

    chain_build(&c, 65u, 1u, NULL);
    su_sched_init(&s);
    check(su_sched_max_systems(&s) == SU_SCHED_DEFAULT_SYSTEMS && SU_SCHED_DEFAULT_SYSTEMS == 8u,
          "a schedule from su_sched_init admits 8 systems (control plane unchanged)");
    for (k = 0; k < 8u && st == SU_OK; k++) {
        m = verified(&c, k);
        st = m != NULL ? su_sched_add(&s, m, 1u, 1u, 0u, 1u, 0u) : SU_E_INTERNAL;
    }
    check(st == SU_OK && s.count == 8u, "8 systems join a default schedule");
    m = verified(&c, 8u);
    check(m != NULL && su_sched_add(&s, m, 1u, 1u, 0u, 1u, 0u) == SU_E_RESOURCE_LIMIT, "the 9th is refused");
    check(su_sched_set_max_systems(&s, 7u) == SU_E_INVALID_ARGUMENT, "the cap cannot drop below the count");
    check(su_sched_set_max_systems(&s, 65u) == SU_E_INVALID_ARGUMENT && su_sched_set_max_systems(&s, 0u) != SU_OK,
          "the cap is 1..SU_SCHED_MAX_SYSTEMS");
    check(su_sched_set_max_systems(&s, SU_SCHED_MAX_SYSTEMS) == SU_OK, "raise the cap to 64");
    st = m != NULL ? su_sched_add(&s, m, 1u, 1u, 0u, 1u, 0u) : SU_E_INTERNAL;
    if (st != SU_OK) {
        su_lir_module_free(m);
        free(m);
    }
    for (k = 9u; k < 64u && st == SU_OK; k++) {
        m = verified(&c, k);
        st = m != NULL ? su_sched_add(&s, m, 1u, 1u, 0u, 1u, 0u) : SU_E_INTERNAL;
    }
    check(st == SU_OK && s.count == 64u, "64 systems join a raised schedule");
    m = verified(&c, 64u);
    check(m != NULL && su_sched_add(&s, m, 1u, 1u, 0u, 1u, 0u) == SU_E_RESOURCE_LIMIT, "the 65th is refused");
    su_lir_module_free(m);
    free(m);
    check(su_sched_finalize(&s) == SU_OK && s.field_count == 65u, "the union table of 64 systems");
    memset(&lane, 0, sizeof lane);
    lane.id = 1u;
    lane.type = SU_TYPE_U32;
    lane.value_type = SU_TYPE_U32;
    snprintf(lane.name, sizeof lane.name, "v00");
    check(su_sched_declare_schema(&s, "t.chain", 1u, 1u, &lane, 1u) == SU_E_INVALID_ARGUMENT,
          "a schema is declared only on an empty schedule");
    su_sched_free(&s);
    check(su_sched_max_systems(&s) == SU_SCHED_DEFAULT_SYSTEMS, "su_sched_free resets the cap");
    chain_free(&c);
}
#endif

/* ---------- 3. declared schemas ---------- */

enum { F_POS = 1, F_TARGET = 2, F_KIND = 3, F_WIDE = 10, F_WSUM = 12, F_OPIN = 20, F_OPOUT = 21, F_ACC = 30 };

static const SuHostSchemaField k_schema[] = {
    {F_ACC, SU_TYPE_U32, "acc"},       /* written by sch.use */
    {F_POS, SU_TYPE_U32, "pos"},       /* sch.follow: pos += target */
    {F_TARGET, SU_TYPE_U32, "target"}, /* written only by the host (input records), read by sch.follow */
    {F_KIND, SU_TYPE_U32, "kind"},     /* bound by nothing: spawn values and inputs, read back by the host */
    {F_WIDE, SU_TYPE_I64, "wide"},     /* lanes 10, 11: written only by the host, read by sch.wide (IR 0.3) */
    {F_WSUM, SU_TYPE_I64, "wsum"},     /* lanes 12, 13: sch.wide: wsum += wide */
    {F_OPIN, SU_TYPE_U32, "opin"},     /* written only by the host, read only by the registered operator */
    {F_OPOUT, SU_TYPE_U32, "opout"},   /* written by the operator, read by sch.use in the same tick */
};

static void schema_init(SuHostSchema *s, const SuHostSchemaField *f, uint32_t n) {
    memset(s, 0, sizeof *s);
    s->struct_size = (uint32_t)sizeof *s;
    s->field_count = n;
    s->fields = f;
    s->schema_id = "t.sch";
    s->schema_version = 1u;
    s->layout_generation = 1u;
}

typedef struct CapField {
    uint32_t id;
    const char *name;
    const char *type;
    int read, write;
} CapField;

static Buf sch_capsule(const char *system_id, const char *schema, uint32_t version, uint32_t gen, const char *ir,
                       const char *profile, const CapField *f, uint32_t n, const char *phase, const char *after) {
    Buf b = {NULL, 0, 0};
    uint32_t i;
    put(&b, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"source\":\"inline.lir.json\",", system_id);
    if (strcmp(ir, "0.1") != 0) {
        put(&b, "\"ir_version\":\"%s\",", ir);
    }
    put(&b, "\"schema\":{\"id\":\"%s\",\"version\":%u,\"layout_generation\":%u},\"numeric_profile\":\"%s\",", schema,
        (unsigned)version, (unsigned)gen, profile);
    put(&b, "\"fields\":[");
    for (i = 0; i < n; i++) {
        put(&b, "%s{\"id\":%u,\"name\":\"%s\",\"type\":\"%s\",\"read\":%s,\"write\":%s}", i > 0u ? "," : "",
            (unsigned)f[i].id, f[i].name, f[i].type, f[i].read ? "true" : "false", f[i].write ? "true" : "false");
    }
    put(&b, "],\"schedule\":{\"phase\":\"%s\",\"after\":[%s],\"before\":[],\"parallel\":\"serial\"},", phase, after);
    put(&b, "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},");
    put(&b, "\"tests\":[\"tests/host/test_host_schedule.c\"]}");
    return b;
}

/* Variants of the declared-schema world, for the refusals. */
enum {
    V_OK,
    V_NAME,         /* sch.follow binds id 2 as "tgt" */
    V_UNDECLARED,   /* sch.follow binds id 99, not declared */
    V_VTYPE,        /* sch.wide binds "wide" as q32 (declared i64) */
    V_SCHEMA_ID,    /* sch.use names schema t.other */
    V_VERSION,      /* sch.use has schema version 2 */
    V_GEN,          /* sch.use has layout generation 2 */
    V_OP_MISMATCH,  /* the record of an operator whose "opin" lane is i32 */
    V_ID_SWAP       /* sch.follow binds "target" at id 3 (the id of "kind") */
};

typedef struct SchWorld {
    Buf follow_cap, follow_lir, wide_cap, wide_lir, use_cap, use_lir, record;
    SuHostCapsule caps[4];
} SchWorld;

static const uint8_t k_sq_hash[32] = {0x14, 0x40, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                      0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
                                      0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e};

static Buf sq_record(const char *system_id, const char *opin_type) {
    Buf b = {NULL, 0, 0};
    uint32_t i;
    put(&b, "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"system_rev\":1,\"capabilities\":[],", system_id);
    put(&b, "\"schema\":{\"id\":\"t.sch\",\"version\":1,\"layout_generation\":1},");
    put(&b, "\"numeric_profile\":\"%s\",", strcmp(opin_type, "u32") == 0 ? "u32_mod_v1" : "fixed_q32_v1");
    put(&b, "\"schedule\":{\"phase\":\"query\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},");
    put(&b, "\"host\":{\"operator\":\"sq_ops_v1\",\"operator_version\":1,\"constants_hash\":\"");
    for (i = 0; i < 32u; i++) {
        put(&b, "%02x", (unsigned)k_sq_hash[i]);
    }
    put(&b, "\",\"fields\":[{\"id\":%u,\"name\":\"opin\",\"type\":\"%s\",\"access\":\"read\"},", (unsigned)F_OPIN,
        opin_type);
    put(&b, "{\"id\":%u,\"name\":\"opout\",\"type\":\"u32\",\"access\":\"write\"}]}}", (unsigned)F_OPOUT);
    return b;
}

static void sch_build(SchWorld *s, int v) {
    const CapField follow_ok[2] = {{F_POS, "pos", "u32", 1, 1}, {F_TARGET, "target", "u32", 1, 0}};
    const CapField follow_name[2] = {{F_POS, "pos", "u32", 1, 1}, {F_TARGET, "tgt", "u32", 1, 0}};
    const CapField follow_undecl[2] = {{F_POS, "pos", "u32", 1, 1}, {99u, "target2", "u32", 1, 0}};
    const CapField follow_swap[2] = {{F_POS, "pos", "u32", 1, 1}, {F_KIND, "target", "u32", 1, 0}};
    const char *wt = v == V_VTYPE ? "q32" : "i64";
    const CapField wide[2] = {{F_WIDE, "wide", wt, 1, 0}, {F_WSUM, "wsum", wt, 1, 1}};
    const CapField use[2] = {{F_OPOUT, "opout", "u32", 1, 0}, {F_ACC, "acc", "u32", 1, 1}};
    const CapField *ff = v == V_NAME ? follow_name : v == V_UNDECLARED ? follow_undecl : v == V_ID_SWAP ? follow_swap
                                                                                                       : follow_ok;
    memset(s, 0, sizeof *s);
    s->follow_cap = sch_capsule("sch.follow", "t.sch", 1u, 1u, "0.1", "u32_mod_v1", ff, 2u, "simulate", "");
    put(&s->follow_lir,
        "{\"ir_version\":\"0.1\",\"system_id\":\"sch.follow\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"t.sch\","
        "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"p\",\"field\":\"pos\"},"
        "{\"op\":\"load_u32\",\"dst\":\"t\",\"field\":\"%s\"},{\"op\":\"add_u32\",\"dst\":\"n\",\"a\":\"p\",\"b\":\"t\"},"
        "{\"op\":\"store_u32\",\"field\":\"pos\",\"src\":\"n\"}]}",
        v == V_NAME ? "tgt" : v == V_UNDECLARED ? "target2" : "target");
    s->wide_cap = sch_capsule("sch.wide", "t.sch", 1u, 1u, "0.3", "fixed_q32_v1", wide, 2u, "simulate", "");
    put(&s->wide_lir,
        "{\"ir_version\":\"0.3\",\"system_id\":\"sch.wide\",\"numeric_profile\":\"fixed_q32_v1\",\"query\":\"t.sch\","
        "\"instructions\":[{\"op\":\"load_%s\",\"dst\":\"a\",\"field\":\"wide\"},"
        "{\"op\":\"load_%s\",\"dst\":\"b\",\"field\":\"wsum\"},{\"op\":\"add_%s\",\"dst\":\"s\",\"a\":\"a\",\"b\":\"b\"},"
        "{\"op\":\"store_%s\",\"field\":\"wsum\",\"src\":\"s\"}]}",
        wt, wt, wt, wt);
    s->use_cap = sch_capsule("sch.use", v == V_SCHEMA_ID ? "t.other" : "t.sch", v == V_VERSION ? 2u : 1u,
                             v == V_GEN ? 2u : 1u, "0.1", "u32_mod_v1", use, 2u, "simulate",
                             v == V_OP_MISMATCH ? "\"test.sq_i32\"" : "\"test.sq\"");
    put(&s->use_lir,
        "{\"ir_version\":\"0.1\",\"system_id\":\"sch.use\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"%s\","
        "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"o\",\"field\":\"opout\"},"
        "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"acc\"},{\"op\":\"add_u32\",\"dst\":\"n\",\"a\":\"a\",\"b\":\"o\"},"
        "{\"op\":\"store_u32\",\"field\":\"acc\",\"src\":\"n\"}]}",
        v == V_SCHEMA_ID ? "t.other" : "t.sch");
    s->record = v == V_OP_MISMATCH ? sq_record("test.sq_i32", "i32") : sq_record("test.sq", "u32");
    /* Capsule order: sch.follow, sch.wide, the operator, sch.use (after the operator). */
    s->caps[0].capsule_json = (const uint8_t *)s->follow_cap.p;
    s->caps[0].capsule_len = s->follow_cap.n;
    s->caps[0].lir_json = (const uint8_t *)s->follow_lir.p;
    s->caps[0].lir_len = s->follow_lir.n;
    s->caps[1].capsule_json = (const uint8_t *)s->wide_cap.p;
    s->caps[1].capsule_len = s->wide_cap.n;
    s->caps[1].lir_json = (const uint8_t *)s->wide_lir.p;
    s->caps[1].lir_len = s->wide_lir.n;
    s->caps[2].capsule_json = (const uint8_t *)s->record.p;
    s->caps[2].capsule_len = s->record.n;
    s->caps[3].capsule_json = (const uint8_t *)s->use_cap.p;
    s->caps[3].capsule_len = s->use_cap.n;
    s->caps[3].lir_json = (const uint8_t *)s->use_lir.p;
    s->caps[3].lir_len = s->use_lir.n;
}

static void sch_free(SchWorld *s) {
    buf_free(&s->follow_cap);
    buf_free(&s->follow_lir);
    buf_free(&s->wide_cap);
    buf_free(&s->wide_lir);
    buf_free(&s->use_cap);
    buf_free(&s->use_lir);
    buf_free(&s->record);
}

/* The registered operator: opout = opin * opin + tick, for every row. */
static uint32_t sq_run(const SuHostOperatorRun *r, void *user) {
    uint64_t i;
    (void)user;
    for (i = 0; i < r->rows; i++) {
        r->columns[1].out[i] = r->columns[0].in[i] * r->columns[0].in[i] + (uint32_t)r->tick;
    }
    return SU_OK;
}

static const SuHostOperatorField k_sq_fields[2] = {{F_OPIN, SU_TYPE_U32, SU_ACCESS_READ, 0, "opin"},
                                                    {F_OPOUT, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "opout"}};
static const SuHostOperatorField k_sq_fields_i32[2] = {{F_OPIN, SU_TYPE_I32, SU_ACCESS_READ, 0, "opin"},
                                                        {F_OPOUT, SU_TYPE_U32, SU_ACCESS_WRITE, 0, "opout"}};

static void register_ops(void) {
    SuHostOperatorDesc d;
    SuHostError err;
    SU_HOST_OPERATOR_DESC_INIT(&d);
    d.system_id = "test.sq";
    d.schema_id = "t.sch";
    d.operator_name = "sq_ops_v1";
    d.operator_version = 1u;
    d.field_count = 2u;
    d.fields = k_sq_fields;
    memcpy(d.constants_hash, k_sq_hash, 32u);
    d.run = sq_run;
    check(su_host_register_operator(&d, &err) == SU_OK, "register test.sq: %s", err.message);
    d.system_id = "test.sq_i32";
    d.fields = k_sq_fields_i32;
    check(su_host_register_operator(&d, &err) == SU_OK, "register test.sq_i32: %s", err.message);
}

#define SCH_CAP 8u
#define SCH_ENTS 5u

typedef struct SchModel {
    uint32_t pos[SCH_CAP], target[SCH_CAP], kind[SCH_CAP], opin[SCH_CAP], opout[SCH_CAP], acc[SCH_CAP];
    int64_t wide[SCH_CAP], wsum[SCH_CAP];
} SchModel;

/* Queues tick t's inputs (every entity, every host-written field) and applies them to the model. */
static int sch_inputs(SuHostWorld *w, uint64_t t, SchModel *m) {
    SuHostInput in[SCH_ENTS * 3u];
    SuHostError err;
    uint32_t i, n = 0;
    int ok = 1;
    for (i = 0; i < SCH_ENTS; i++) {
        uint64_t ent = 0;
        ok &= su_host_entity_at(w, i, &ent) == SU_OK;
        in[n].entity = ent, in[n].field_id = F_TARGET, in[n].value = (uint32_t)(t * 10u + i);
        m->target[i] = in[n++].value;
        in[n].entity = ent, in[n].field_id = F_OPIN, in[n].value = (uint32_t)(t + i + 2u);
        m->opin[i] = in[n++].value;
        if (t % 3u == 1u) {
            in[n].entity = ent, in[n].field_id = F_KIND, in[n].value = (uint32_t)(0x700u + t + i);
            m->kind[i] = in[n++].value;
        }
        m->wide[i] = (int64_t)(t + 1u) * -1000000007LL + (int64_t)i;
        ok &= su_host_input_push_i64(w, t, ent, F_WIDE, m->wide[i], &err) == SU_OK;
    }
    ok &= su_host_input_push(w, t, in, n, &err) == SU_OK;
    return ok;
}

static void sch_model_tick(SchModel *m, uint64_t t) {
    uint32_t i;
    for (i = 0; i < SCH_ENTS; i++) {
        m->pos[i] += m->target[i];
        m->wsum[i] = (int64_t)((uint64_t)m->wsum[i] + (uint64_t)m->wide[i]);
        m->opout[i] = m->opin[i] * m->opin[i] + (uint32_t)t;
        m->acc[i] += m->opout[i];
    }
}

static int sch_matches(const SuHostWorld *w, const SchModel *m) {
    uint32_t c[SCH_CAP];
    int64_t v[SCH_CAP];
    uint32_t i;
    int ok = 1;
#define COL(id, arr)                                                                                                   \
    ok &= su_host_read(w, id, 0, SCH_CAP, c) == SU_OK;                                                                \
    for (i = 0; i < SCH_ENTS; i++) {                                                                                   \
        ok &= c[i] == m->arr[i];                                                                                       \
    }
    COL(F_POS, pos)
    COL(F_TARGET, target)
    COL(F_KIND, kind)
    COL(F_OPIN, opin)
    COL(F_OPOUT, opout)
    COL(F_ACC, acc)
#undef COL
    ok &= su_host_read_i64(w, F_WIDE, 0, SCH_CAP, v) == SU_OK;
    for (i = 0; i < SCH_ENTS; i++) {
        ok &= v[i] == m->wide[i];
    }
    ok &= su_host_read_i64(w, F_WSUM, 0, SCH_CAP, v) == SU_OK;
    for (i = 0; i < SCH_ENTS; i++) {
        ok &= v[i] == m->wsum[i];
    }
    return ok;
}

/* A declared-schema world with SCH_ENTS spawned entities (pos = 3 * i, kind = 0x500 + i). */
static SuHostWorld *sch_world(const SchWorld *s, const SuHostSchema *schema, SchModel *m, SuHostError *err) {
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    uint32_t i;
    desc_init(&d, s->caps, 4u, NULL, SCH_CAP);
    d.schema = schema;
    if (su_host_world_create(&d, &w, err) != SU_OK) {
        return NULL;
    }
    memset(m, 0, sizeof *m);
    for (i = 0; i < SCH_ENTS; i++) {
        SuHostValue v[2] = {{F_POS, 3u * i}, {F_KIND, 0x500u + i}};
        uint64_t ent = 0;
        if (su_host_spawn(w, v, 2u, 0u, &ent, err) != SU_OK) {
            su_host_world_destroy(w);
            return NULL;
        }
        m->pos[i] = 3u * i;
        m->kind[i] = 0x500u + i;
    }
    return w;
}

static void test_declared_schema(void) {
    SchWorld s;
    SuHostSchema schema;
    SuHostWorld *w = NULL, *w2 = NULL;
    SuHostError err;
    SchModel m, m5;
    uint64_t t, done = 0;
    uint8_t *snap5 = NULL, *rep = NULL;
    size_t snap5_len = 0, rep_len = 0;
    char d5[65], d10[65], hex[65];
    int model_ok = 1;

    memset(&m5, 0, sizeof m5); /* set at tick 5; initialised for MSVC C4701 */
    memset(d5, 0, sizeof d5);

    sch_build(&s, V_OK);
    schema_init(&schema, k_schema, (uint32_t)(sizeof k_schema / sizeof k_schema[0]));
    w = sch_world(&s, &schema, &m, &err);
    check(w != NULL, "declared-schema world: %s", err.message);
    if (w == NULL) {
        sch_free(&s);
        return;
    }
    /* Field info: ten lanes, schema-only lanes with access 0, value types and lanes of i64 fields. */
    {
        SuHostFieldInfo fi;
        uint32_t i, vt = 0, lane = 0, id = 0;
        static const uint32_t want_id[10] = {1, 2, 3, 10, 11, 12, 13, 20, 21, 30};
        static const uint32_t want_access[10] = {SU_ACCESS_READ | SU_ACCESS_WRITE, SU_ACCESS_READ, 0u, SU_ACCESS_READ,
                                                 SU_ACCESS_READ, SU_ACCESS_READ | SU_ACCESS_WRITE,
                                                 SU_ACCESS_READ | SU_ACCESS_WRITE, SU_ACCESS_READ, SU_ACCESS_READ | SU_ACCESS_WRITE,
                                                 SU_ACCESS_READ | SU_ACCESS_WRITE};
        int info_ok = su_host_field_count(w) == 10u;
        for (i = 0; info_ok && i < 10u; i++) {
            info_ok &= su_host_field_info(w, i, &fi) == SU_OK && fi.id == want_id[i] && fi.type == SU_TYPE_U32 &&
                       fi.access == want_access[i];
        }
        check(info_ok, "field table of the declared schema (ids, storage type, access; 0 = no system binds it)");
        check(su_host_field_id(w, "wide.hi", &id) == SU_OK && id == 11u, "lane name wide.hi is id 11");
        check(su_host_field_value_type(w, F_WIDE + 1u, &vt, &lane) == SU_OK && vt == SU_TYPE_I64 && lane == 1u,
              "wide.hi: i64 lane 1");
        check(su_host_field_value_type(w, F_KIND, &vt, &lane) == SU_OK && vt == SU_TYPE_U32 && lane == 0u,
              "kind: u32 lane 0");
        check(strcmp(su_host_schema_id(w), "t.sch") == 0, "schema id");
        check(su_host_system_count(w) == 4u, "four systems");
    }
    for (t = 0; t < 10u; t++) {
        check(sch_inputs(w, t, &m), "inputs for tick %u", (unsigned)t);
        check(su_host_step(w, 1u, &done, &err) == SU_OK && done == 1u, "declared-schema tick %u: %s", (unsigned)t,
              err.message);
        sch_model_tick(&m, t);
        model_ok &= sch_matches(w, &m);
        if (t + 1u == 5u) {
            snap5 = snapshot_of(w, &snap5_len);
            digest_hex(w, d5);
            m5 = m;
        }
    }
    check(model_ok, "host-written lanes reach IR systems and the operator in the same tick; results match the model");
    digest_hex(w, d10);

    /* Snapshot round trip: restore tick 5 (allocating, then in place), re-push the inputs, re-simulate. */
    check(snap5 != NULL, "declared-schema snapshot");
    if (snap5 != NULL) {
        int k;
        for (k = 0; k < 2; k++) {
            SchModel mm = m5;
            uint32_t st = k == 0 ? su_host_snapshot_restore(w, snap5, snap5_len, &err)
                                 : su_host_snapshot_restore_inplace(w, snap5, snap5_len, &err);
            check(st == SU_OK && su_host_tick(w) == 5u, "declared-schema restore (%s): %s",
                  k == 0 ? "allocating" : "in place", err.message);
            digest_hex(w, hex);
            check(strcmp(hex, d5) == 0 && sch_matches(w, &mm), "restored state equals tick 5");
            for (t = 5u; t < 10u; t++) {
                sch_inputs(w, t, &mm);
                check(su_host_step(w, 1u, &done, &err) == SU_OK, "re-simulate tick %u", (unsigned)t);
                sch_model_tick(&mm, t);
            }
            digest_hex(w, hex);
            check(strcmp(hex, d10) == 0 && sch_matches(w, &mm), "re-simulated declared-schema world equals tick 10");
        }
    }

    /* Replay round trip in a fresh world: record 10 ticks with inputs, seek to 3, play to the end. */
    {
        SuHostWorld *a, *b;
        SchModel ma, mb;
        SuReplayWriter *rw = NULL;
        SuReplayOptions ro;
        SuReplay *rp = NULL;
        char end_hex[65];
        memset(&ro, 0, sizeof ro);
        ro.struct_size = (uint32_t)sizeof ro;
        ro.keyframe_interval = 4u;
        ro.digest_interval = 1u;
        a = sch_world(&s, &schema, &ma, &err);
        b = sch_world(&s, &schema, &mb, &err);
        check(a != NULL && b != NULL, "replay worlds with a declared schema");
        if (a != NULL && b != NULL) {
            check(su_replay_writer_begin(a, &ro, &rw, &err) == SU_OK, "replay begin: %s", err.message);
            for (t = 0; t < 10u; t++) {
                sch_inputs(a, t, &ma);
                check(su_replay_writer_step(rw, a, 1u, &done, &err) == SU_OK && done == 1u, "record tick %u",
                      (unsigned)t);
            }
            check(su_replay_writer_finish(rw, a, &rep, &rep_len) == SU_OK, "replay finish");
            su_replay_writer_destroy(rw);
            digest_hex(a, end_hex);
            check(strcmp(end_hex, d10) == 0, "a recorded fresh world reaches the same state");
            check(su_replay_open(rep, rep_len, &rp, &err) == SU_OK, "replay open: %s", err.message);
            if (rp != NULL) {
                SuReplayInfo info;
                check(su_replay_info(rp, &info) == SU_OK && info.system_count == 4u && info.input_count == 10u,
                      "replay header of the declared-schema world");
                check(su_replay_seek(rp, b, 3u, &err) == SU_OK, "seek: %s", err.message);
                check(su_replay_play(rp, b, 100u, &done, &err) == SU_OK && done == 7u, "play: %s", err.message);
                digest_hex(b, hex);
                check(strcmp(hex, end_hex) == 0, "replayed declared-schema world equals the recording");
                su_replay_close(rp);
            }
        }
        su_host_world_destroy(a);
        su_host_world_destroy(b);
    }

    /* The declared schema is part of the identity: the same capsules with the union schema (every declared lane
     * bound: a world without "kind") differ; a snapshot of one does not load into the other. And a declared schema
     * equal to the union still differs from no declaration. */
    {
        static const SuHostSchemaField no_kind[] = {
            {F_ACC, SU_TYPE_U32, "acc"},    {F_POS, SU_TYPE_U32, "pos"},   {F_TARGET, SU_TYPE_U32, "target"},
            {F_WIDE, SU_TYPE_I64, "wide"},  {F_WSUM, SU_TYPE_I64, "wsum"}, {F_OPIN, SU_TYPE_U32, "opin"},
            {F_OPOUT, SU_TYPE_U32, "opout"}};
        SuHostSchema sk;
        SuHostWorldDesc d;
        SuHostWorld *plain = NULL, *decl = NULL;
        uint8_t ia[32], ib[32];
        size_t plen = 0;
        uint8_t *ps;
        desc_init(&d, s.caps, 4u, NULL, SCH_CAP);
        check(su_host_world_create(&d, &plain, &err) == SU_OK, "the same capsules without a schema: %s", err.message);
        schema_init(&sk, no_kind, (uint32_t)(sizeof no_kind / sizeof no_kind[0]));
        d.schema = &sk;
        check(su_host_world_create(&d, &decl, &err) == SU_OK, "a declared schema equal to the union: %s", err.message);
        if (plain != NULL && decl != NULL) {
            su_host_world_identity(plain, ia);
            su_host_world_identity(decl, ib);
            check(memcmp(ia, ib, 32u) != 0, "a declared schema changes the identity");
            /* The value type of a lane no system binds is part of the identity too (kind as i32). */
            {
                SuHostSchemaField alt[sizeof k_schema / sizeof k_schema[0]];
                SuHostSchema sa;
                SuHostWorld *wa = NULL;
                uint8_t i1[32], i2[32];
                uint32_t q, vt = 0, lane = 0;
                memcpy(alt, k_schema, sizeof alt);
                for (q = 0; q < sizeof alt / sizeof alt[0]; q++) {
                    if (alt[q].id == F_KIND) {
                        alt[q].type = SU_TYPE_I32;
                    }
                }
                schema_init(&sa, alt, (uint32_t)(sizeof alt / sizeof alt[0]));
                d.schema = &sa;
                check(su_host_world_create(&d, &wa, &err) == SU_OK, "kind as i32: %s", err.message);
                if (wa != NULL) {
                    su_host_world_identity(wa, i1);
                    su_host_world_identity(w, i2);
                    check(memcmp(i1, i2, 32u) != 0, "the value type of a schema-only lane changes the identity");
                    check(su_host_field_value_type(wa, F_KIND, &vt, &lane) == SU_OK && vt == SU_TYPE_I32,
                          "kind reports i32");
                    su_host_world_destroy(wa);
                }
                d.schema = &sk;
            }
            check(su_host_field_count(plain) == su_host_field_count(decl), "same lanes");
            ps = snapshot_of(plain, &plen);
            check(ps != NULL && su_host_snapshot_restore(decl, ps, plen, &err) == SU_E_SCHEMA_MISMATCH,
                  "a snapshot without a schema does not load into a declared-schema world");
            free(ps);
            w2 = plain;
            plain = NULL;
        }
        su_host_world_destroy(plain);
        su_host_world_destroy(decl);
    }
    su_replay_free(rep);
    free(snap5);
    su_host_world_destroy(w);
    su_host_world_destroy(w2);
    sch_free(&s);
}

/* ---------- 4. refusals ---------- */

static void expect_refused(int variant, uint32_t code, uint32_t index, const char *needle, const char *what) {
    SchWorld s;
    SuHostSchema schema;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t st;
    sch_build(&s, variant);
    schema_init(&schema, k_schema, (uint32_t)(sizeof k_schema / sizeof k_schema[0]));
    desc_init(&d, s.caps, 4u, NULL, SCH_CAP);
    d.schema = &schema;
    st = su_host_world_create(&d, &w, &err);
    check(st == code && w == NULL && err.index == index && (needle == NULL || strstr(err.message, needle) != NULL),
          "%s: got %s index %u: %s", what, su_host_status_name(st), err.index, err.message);
    su_host_world_destroy(w);
    sch_free(&s);
}

static void expect_bad_schema(const SuHostSchemaField *f, uint32_t n, uint32_t index, const char *what) {
    SchWorld s;
    SuHostSchema schema;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError err;
    uint32_t st;
    sch_build(&s, V_OK);
    schema_init(&schema, f, n);
    desc_init(&d, s.caps, 4u, NULL, SCH_CAP);
    d.schema = &schema;
    st = su_host_world_create(&d, &w, &err);
    check(st == SU_E_INVALID_ARGUMENT && w == NULL && err.index == index, "%s: got %s index %u: %s", what,
          su_host_status_name(st), err.index, err.message);
    su_host_world_destroy(w);
    sch_free(&s);
}

static void test_refusals(void) {
    /* Capsules and records that disagree with the declared schema: SU_E_SCHEMA_MISMATCH, the capsule's index. */
    expect_refused(V_NAME, SU_E_SCHEMA_MISMATCH, 0u, "\"tgt\"", "a capsule names a declared id differently");
    expect_refused(V_UNDECLARED, SU_E_SCHEMA_MISMATCH, 0u, "does not have", "a capsule binds an undeclared field");
    expect_refused(V_ID_SWAP, SU_E_SCHEMA_MISMATCH, 0u, "sch.follow", "a capsule binds a declared name at another id");
    expect_refused(V_VTYPE, SU_E_SCHEMA_MISMATCH, 1u, "(q32)", "a capsule binds an i64 field as q32");
    expect_refused(V_SCHEMA_ID, SU_E_SCHEMA_MISMATCH, 3u, "t.other", "a capsule of another schema id");
    expect_refused(V_VERSION, SU_E_SCHEMA_MISMATCH, 3u, "version 2", "a capsule of another schema version");
    expect_refused(V_GEN, SU_E_SCHEMA_MISMATCH, 3u, "generation 2", "a capsule of another layout generation");
    expect_refused(V_OP_MISMATCH, SU_E_SCHEMA_MISMATCH, 2u, "test.sq_i32",
                   "an operator lane whose value type differs from the declaration");

    /* Malformed declarations: SU_E_INVALID_ARGUMENT with the field index. */
    {
        static const SuHostSchemaField dup_id[] = {{1, SU_TYPE_U32, "a"}, {1, SU_TYPE_U32, "b"}};
        static const SuHostSchemaField dup_name[] = {{1, SU_TYPE_U32, "a"}, {2, SU_TYPE_U32, "a"}};
        static const SuHostSchemaField hi_clash[] = {{1, SU_TYPE_U32, "a"}, {4, SU_TYPE_I64, "w"},
                                                     {5, SU_TYPE_U32, "b"}};
        static const SuHostSchemaField lane_name[] = {{4, SU_TYPE_I64, "w"}, {9, SU_TYPE_U32, "w.hi"}};
        static const SuHostSchemaField bad_type[] = {{1, SU_TYPE_U32, "a"}, {2, 77u, "b"}};
        static const SuHostSchemaField empty[] = {{1, SU_TYPE_U32, ""}};
        static const SuHostSchemaField null_name[] = {{1, SU_TYPE_U32, "a"}, {2, SU_TYPE_U32, NULL}};
        static const SuHostSchemaField top[] = {{UINT32_MAX, SU_TYPE_Q32, "w"}};
        expect_bad_schema(dup_id, 2u, 1u, "duplicate lane id");
        expect_bad_schema(dup_name, 2u, 1u, "duplicate lane name");
        expect_bad_schema(hi_clash, 3u, 2u, "an i64 high lane on another field's id");
        expect_bad_schema(lane_name, 2u, 1u, "a field named like another field's lane");
        expect_bad_schema(bad_type, 2u, 1u, "an unknown type");
        expect_bad_schema(empty, 1u, 0u, "an empty name");
        expect_bad_schema(null_name, 2u, 1u, "a NULL name");
        expect_bad_schema(top, 1u, 0u, "a q32 field at UINT32_MAX");
        expect_bad_schema(k_schema, 0u, UINT32_MAX, "no fields");
        {
            /* Names follow the capsule rule; IDs are >= 1. */
            static const SuHostSchemaField dash[] = {{1, SU_TYPE_U32, "a"}, {2, SU_TYPE_U32, "b-c"}};
            static const SuHostSchemaField digit[] = {{1, SU_TYPE_U32, "1x"}};
            static const SuHostSchemaField dot[] = {{1, SU_TYPE_U32, "a.b"}};
            static const SuHostSchemaField quote[] = {{1, SU_TYPE_U32, "a\"b"}};
            static const SuHostSchemaField lo[] = {{1, SU_TYPE_U32, "a"}, {2, SU_TYPE_U32, "x.lo"}};
            static const SuHostSchemaField hi[] = {{2, SU_TYPE_I32, "x.hi"}};
            static const SuHostSchemaField id0[] = {{1, SU_TYPE_U32, "a"}, {0, SU_TYPE_U32, "z"}};
            static const SuHostSchemaField id0w[] = {{0, SU_TYPE_I64, "w"}};
            expect_bad_schema(dash, 2u, 1u, "a name with '-'");
            expect_bad_schema(digit, 1u, 0u, "a name starting with a digit");
            expect_bad_schema(dot, 1u, 0u, "a name with '.'");
            expect_bad_schema(quote, 1u, 0u, "a name with a quote");
            expect_bad_schema(lo, 2u, 1u, "a plain field named like a low lane");
            expect_bad_schema(hi, 1u, 0u, "a plain field named like a high lane");
            expect_bad_schema(id0, 2u, 1u, "field id 0");
            expect_bad_schema(id0w, 1u, 0u, "an i64 field at id 0");
        }
    }
    /* struct_size of the schema, and a NULL schema id. */
    {
        SchWorld s;
        SuHostSchema schema;
        SuHostWorldDesc d;
        SuHostWorld *w = NULL;
        SuHostError err;
        sch_build(&s, V_OK);
        schema_init(&schema, k_schema, (uint32_t)(sizeof k_schema / sizeof k_schema[0]));
        schema.struct_size = 8u;
        desc_init(&d, s.caps, 4u, NULL, SCH_CAP);
        d.schema = &schema;
        check(su_host_world_create(&d, &w, &err) == SU_E_INVALID_ARGUMENT, "schema struct_size");
        schema_init(&schema, k_schema, (uint32_t)(sizeof k_schema / sizeof k_schema[0]));
        schema.schema_id = NULL;
        check(su_host_world_create(&d, &w, &err) == SU_E_INVALID_ARGUMENT, "schema without an id");
        /* A message about the lane rule for x.lo. */
        {
            static const SuHostSchemaField lo[] = {{2, SU_TYPE_U32, "x.lo"}};
            schema_init(&schema, lo, 1u);
            check(su_host_world_create(&d, &w, &err) == SU_E_INVALID_ARGUMENT &&
                      strstr(err.message, "lane name") != NULL,
                  "x.lo is named as a lane: %s", err.message);
        }
        /* schema_version and layout_generation >= 1: refused as a declaration, not a capsule mismatch. */
        schema_init(&schema, k_schema, (uint32_t)(sizeof k_schema / sizeof k_schema[0]));
        schema.schema_version = 0u;
        check(su_host_world_create(&d, &w, &err) == SU_E_INVALID_ARGUMENT && err.index == UINT32_MAX &&
                  strstr(err.message, "schema_version and layout_generation") != NULL,
              "schema version 0: %s", err.message);
        schema_init(&schema, k_schema, (uint32_t)(sizeof k_schema / sizeof k_schema[0]));
        schema.layout_generation = 0u;
        check(su_host_world_create(&d, &w, &err) == SU_E_INVALID_ARGUMENT && err.index == UINT32_MAX,
              "layout generation 0: %s", err.message);
        /* Versioned sizes of SuHostSchema: below v1, v1, larger with a zero tail, larger with a nonzero tail. */
        {
            unsigned char big[sizeof(SuHostSchema) + 16u];
            SuHostSchema *bs = (SuHostSchema *)(void *)big;
            check(SU_HOST_SCHEMA_V1_SIZE == sizeof(SuHostSchema), "SuHostSchema v1 size");
            schema_init(&schema, k_schema, (uint32_t)(sizeof k_schema / sizeof k_schema[0]));
            schema.struct_size = (uint32_t)SU_HOST_SCHEMA_V1_SIZE - 4u;
            check(su_host_world_create(&d, &w, &err) == SU_E_INVALID_ARGUMENT, "schema below the v1 size");
            schema.struct_size = (uint32_t)SU_HOST_SCHEMA_V1_SIZE;
            check(su_host_world_create(&d, &w, &err) == SU_OK, "schema of the v1 size: %s", err.message);
            su_host_world_destroy(w);
            w = NULL;
            memset(big, 0, sizeof big);
            memcpy(big, &schema, sizeof schema);
            bs->struct_size = (uint32_t)sizeof big;
            d.schema = bs;
            check(su_host_world_create(&d, &w, &err) == SU_OK, "larger schema with a zero tail: %s", err.message);
            su_host_world_destroy(w);
            w = NULL;
            big[sizeof(SuHostSchema) + 5u] = 1u;
            check(su_host_world_create(&d, &w, &err) == SU_E_INVALID_ARGUMENT && w == NULL &&
                      strstr(err.message, "does not know") != NULL,
                  "larger schema with a nonzero tail: %s", err.message);
            d.schema = &schema;
        }
        /* A fixture must hold every declared column, the schema-only ones included. */
        {
            static const char *fx = "{\"fixture_format\":\"0.1\",\"schema_id\":\"t.sch\",\"schema_version\":1,"
                                    "\"tick\":0,\"entity_ids\":[1],\"columns\":{\"acc\":[0],\"pos\":[0],\"target\":[0],"
                                    "\"wide.lo\":[0],\"wide.hi\":[0],\"wsum.lo\":[0],\"wsum.hi\":[0],\"opin\":[0],"
                                    "\"opout\":[0]}}";
            static const char *fx_ok = "{\"fixture_format\":\"0.1\",\"schema_id\":\"t.sch\",\"schema_version\":1,"
                                       "\"tick\":0,\"entity_ids\":[1],\"columns\":{\"acc\":[0],\"pos\":[0],"
                                       "\"target\":[0],\"kind\":[7],\"wide.lo\":[0],\"wide.hi\":[0],\"wsum.lo\":[0],"
                                       "\"wsum.hi\":[0],\"opin\":[0],\"opout\":[0]}}";
            uint32_t v = 0;
            schema_init(&schema, k_schema, (uint32_t)(sizeof k_schema / sizeof k_schema[0]));
            d.fixture_json = (const uint8_t *)fx;
            d.fixture_len = strlen(fx);
            check(su_host_world_create(&d, &w, &err) != SU_OK && err.index == F_KIND &&
                      strstr(err.message, "kind") != NULL,
                  "a fixture without the schema-only column kind: %s", err.message);
            d.fixture_json = (const uint8_t *)fx_ok;
            d.fixture_len = strlen(fx_ok);
            check(su_host_world_create(&d, &w, &err) == SU_OK, "a fixture with every declared column: %s",
                  err.message);
            check(w != NULL && su_host_read(w, F_KIND, 0, 1, &v) == SU_OK && v == 7u, "fixture value of kind");
            su_host_world_destroy(w);
        }
        sch_free(&s);
    }
}

int main(void) {
    register_ops();
    test_chain64();
    test_admission();
    test_entity_budget();
    test_native_limit();
    test_opdata_high_index();
#ifdef SU_TEST_INTERNAL
    test_schedule_internal();
#endif
    test_declared_schema();
    test_refusals();
    printf("test_host_schedule: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
