/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.4 handle_live through the in-process host API (issue 162). Rows store a target handle (slot, generation) in
 * two u32 fields; the system stores handle_live(slot, generation) as 0/1 and counts the live ticks. The table has
 * 2,500 slots (three SU_CHUNK_SLOTS chunks) and targets cross chunks, so an answer taken from the row's own chunk
 * would differ. Checked: a handle to a despawned entity is not live; after the slot is reused the old handle is not
 * live and the new one is; generation 0 and out-of-range slots are not live; the answers are the tick-start entity
 * table's (a despawn queued by the host between ticks shows on the next tick); snapshot restore (in place and into
 * a fresh world) reproduces the answers and digests; reference and planned tiers agree; a native request is
 * refused (entity operations stay on the VM). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/entities.h"
#include "sutekh/host.h"
#include "sutekh/status.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CAP 2500u
#define LIVE 2100u
_Static_assert(CAP > 2u * SU_CHUNK_SLOTS, "the table must span three entity chunks");

#define F_TS 1u
#define F_TG 2u
#define F_LIVE 10u
#define F_COUNT 11u

static const char CAPSULE[] =
    "{\"capsule_format\":\"0.1\",\"ir_version\":\"0.4\",\"system_id\":\"t.handle\",\"source\":\"t.lir.json\","
    "\"schema\":{\"id\":\"t.handle\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"fixed_q32_v1\","
    "\"fields\":[{\"id\":1,\"name\":\"ts\",\"type\":\"u32\",\"read\":true,\"write\":false},"
    "{\"id\":2,\"name\":\"tg\",\"type\":\"u32\",\"read\":true,\"write\":false},"
    "{\"id\":10,\"name\":\"live\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":11,\"name\":\"count\",\"type\":\"u32\",\"read\":true,\"write\":true}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":64,\"max_rows\":65536,\"allocation\":\"forbidden\"},"
    "\"tests\":[\"tests/ir04/test_host_handle.c\"]}";

static const char LIR[] =
    "{\"ir_version\":\"0.4\",\"system_id\":\"t.handle\",\"numeric_profile\":\"fixed_q32_v1\",\"query\":\"t.handle\","
    "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"s\",\"field\":\"ts\"},"
    "{\"op\":\"load_u32\",\"dst\":\"g\",\"field\":\"tg\"},"
    "{\"op\":\"handle_live\",\"dst\":\"h\",\"slot\":\"s\",\"gen\":\"g\"},"
    "{\"op\":\"const_u32\",\"dst\":\"one\",\"value\":1},{\"op\":\"const_u32\",\"dst\":\"zero\",\"value\":0},"
    "{\"op\":\"select_u32\",\"dst\":\"v\",\"cond\":\"h\",\"on_true\":\"one\",\"on_false\":\"zero\"},"
    "{\"op\":\"load_u32\",\"dst\":\"c\",\"field\":\"count\"},{\"op\":\"add_u32\",\"dst\":\"c1\",\"a\":\"c\",\"b\":\"v\"},"
    "{\"op\":\"store_u32\",\"field\":\"live\",\"src\":\"v\"},{\"op\":\"store_u32\",\"field\":\"count\",\"src\":\"c1\"}]}";

static uint64_t g_handle[CAP]; /* handle of each initial entity */

/* Row r's initial target: slot (r * 977 + 3) mod LIVE (mostly another chunk), current generation; every 50th row
 * has generation 0 and every 70th an out-of-range slot. */
static void initial_target(uint32_t r, uint32_t *ts, uint32_t *tg) {
    *ts = (r * 977u + 3u) % LIVE;
    *tg = SU_GEN_FIRST;
    if (r % 50u == 7u) {
        *tg = 0u;
    } else if (r % 70u == 11u) {
        *ts = r % 2u == 0u ? CAP : 0xFFFFFFFFu;
    }
}

static SuHostWorld *make_world(uint32_t tier) {
    SuHostCapsule cap;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t i;
    cap.capsule_json = (const uint8_t *)CAPSULE;
    cap.capsule_len = sizeof CAPSULE - 1u;
    cap.lir_json = (const uint8_t *)LIR;
    cap.lir_len = sizeof LIR - 1u;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.max_entities = CAP;
    d.tick_rate_hz = 60;
    if (su_host_world_create(&d, &w, &e) != SU_OK) {
        fprintf(stderr, "create: %s\n", e.message);
        return NULL;
    }
    if (su_host_system_set_tier(w, 0u, tier, &e) != SU_OK) {
        fprintf(stderr, "tier: %s\n", e.message);
        su_host_world_destroy(w);
        return NULL;
    }
    for (i = 0; i < LIVE; i++) {
        SuHostValue v[2];
        v[0].field_id = F_TS;
        v[1].field_id = F_TG;
        initial_target(i, &v[0].value, &v[1].value);
        if (su_host_spawn(w, v, 2u, i + 1u, &g_handle[i], &e) != SU_OK || SU_ENTITY_SLOT(g_handle[i]) != i) {
            fprintf(stderr, "spawn: %s\n", e.message);
            su_host_world_destroy(w);
            return NULL;
        }
    }
    return w;
}

static uint32_t at(const SuHostWorld *w, uint32_t field, uint32_t slot) {
    uint32_t v = 0xDEADBEEFu;
    (void)su_host_read(w, field, slot, 1u, &v);
    return v;
}

static int step1(SuHostWorld *w) {
    uint64_t done = 0;
    SuHostError e;
    uint32_t st = su_host_step(w, 1, &done, &e);
    if (st != SU_OK || done != 1u) {
        fprintf(stderr, "step: %s (%s)\n", su_status_name(st), e.message);
        return 0;
    }
    return 1;
}

/* Points row's target at handle h (slot, generation), applied at the next tick. */
static int retarget(SuHostWorld *w, uint32_t row, uint64_t h) {
    SuHostInput in[2];
    SuHostError e;
    uint64_t ent = 0;
    if (su_host_entity_at(w, row, &ent) != SU_OK) {
        return 0;
    }
    in[0].entity = ent;
    in[0].field_id = F_TS;
    in[0].value = SU_ENTITY_SLOT(h);
    in[1].entity = ent;
    in[1].field_id = F_TG;
    in[1].value = SU_ENTITY_GEN(h);
    return su_host_input_push(w, su_host_tick(w), in, 2u, &e) == SU_OK;
}

/* Runs the scenario on one world; returns the end digest in d. Victim A = slot 5 (targeted by rows 2050 and 1),
 * victim B = slot 2099 (third chunk, targeted by row 3). */
static void scenario(SuHostWorld *w, uint8_t d[32], uint8_t mid[32], uint8_t **snap, size_t *snap_len) {
    SuHostError e;
    uint64_t newA = 0;
    uint32_t r, ts, tg, live = 0;
    CHECK(retarget(w, 2050u, g_handle[5]));
    CHECK(retarget(w, 1u, g_handle[5]));
    CHECK(retarget(w, 3u, g_handle[2099]));
    CHECK(step1(w));
    /* Tick 1: every row's answer is the oracle's over the initial table. */
    for (r = 0; r < LIVE; r++) {
        initial_target(r, &ts, &tg);
        if (r == 2050u || r == 1u) {
            ts = 5u;
        } else if (r == 3u) {
            ts = 2099u;
        }
        live += at(w, F_LIVE, r);
        CHECK(at(w, F_LIVE, r) == (uint32_t)(ts < LIVE && tg == SU_GEN_FIRST));
    }
    CHECK(live > LIVE / 2u && live < LIVE);
    /* Despawn A and B between ticks: the next tick sees them gone. */
    CHECK(su_host_despawn(w, g_handle[5], &e) == SU_OK);
    CHECK(su_host_despawn(w, g_handle[2099], &e) == SU_OK);
    CHECK(step1(w));
    CHECK(at(w, F_LIVE, 2050u) == 0u && at(w, F_LIVE, 1u) == 0u && at(w, F_LIVE, 3u) == 0u);
    CHECK(at(w, F_COUNT, 2050u) == 1u);
    /* Reuse slot 5 (lowest free slot): same slot, generation 2. Row 1 retargets to the new handle. */
    CHECK(su_host_spawn(w, NULL, 0u, 0u, &newA, &e) == SU_OK);
    CHECK(SU_ENTITY_SLOT(newA) == 5u && SU_ENTITY_GEN(newA) == SU_GEN_FIRST + 1u);
    CHECK(retarget(w, 1u, newA));
    CHECK(step1(w));
    CHECK(at(w, F_LIVE, 2050u) == 0u); /* the old handle: the slot is alive again, but with another generation */
    CHECK(at(w, F_LIVE, 1u) == 1u);    /* the new handle */
    CHECK(at(w, F_LIVE, 5u) == 0u);    /* the new entity's own fields are zero: handle (0, 0), generation 0 */
    CHECK(at(w, F_LIVE, 3u) == 0u);    /* slot 2099 is still free */
    /* Snapshot here, then go on: despawn the new A as well. */
    CHECK(su_host_digest(w, mid) == SU_OK);
    CHECK(su_host_snapshot_size(w, snap_len) == SU_OK);
    *snap = malloc(*snap_len);
    CHECK(*snap != NULL && su_host_snapshot_write(w, *snap, *snap_len, snap_len) == SU_OK);
    CHECK(su_host_despawn(w, newA, &e) == SU_OK);
    CHECK(step1(w));
    CHECK(at(w, F_LIVE, 1u) == 0u && at(w, F_COUNT, 1u) == 2u);
    CHECK(step1(w));
    CHECK(su_host_digest(w, d) == SU_OK);
}

/* After restoring the mid snapshot, the same continuation must give the same answers and digest. */
static void continue_from_mid(SuHostWorld *w, const uint8_t mid[32], const uint8_t end[32]) {
    SuHostError e;
    uint64_t a = 0;
    uint8_t d[32];
    CHECK(su_host_digest(w, d) == SU_OK && memcmp(d, mid, 32) == 0);
    CHECK(su_host_entity_at(w, 5u, &a) == SU_OK && SU_ENTITY_GEN(a) == SU_GEN_FIRST + 1u);
    CHECK(at(w, F_LIVE, 1u) == 1u && at(w, F_LIVE, 2050u) == 0u);
    CHECK(su_host_despawn(w, a, &e) == SU_OK);
    CHECK(step1(w));
    CHECK(at(w, F_LIVE, 1u) == 0u && at(w, F_COUNT, 1u) == 2u);
    CHECK(step1(w));
    CHECK(su_host_digest(w, d) == SU_OK && memcmp(d, end, 32) == 0);
}

int main(void) {
    uint8_t d_ref[32], d_pl[32], mid_ref[32], mid_pl[32];
    uint8_t *snap_ref = NULL, *snap_pl = NULL;
    size_t len_ref = 0, len_pl = 0;
    SuHostWorld *w = make_world(SU_HOST_TIER_REFERENCE);
    SuHostWorld *p = make_world(SU_HOST_TIER_PLANNED);
    SuHostError e;
    CHECK(w != NULL && p != NULL);
    if (w == NULL || p == NULL) {
        return 1;
    }
    if (su_host_native_available()) {
        uint32_t tier = 0;
        CHECK(su_host_system_set_tier(w, 0u, SU_HOST_TIER_NATIVE, &e) == SU_E_CAPABILITY_DENIED);
        CHECK(su_host_system_tier(w, 0u, &tier) == SU_OK && tier == SU_HOST_TIER_REFERENCE);
    }
    scenario(w, d_ref, mid_ref, &snap_ref, &len_ref);
    scenario(p, d_pl, mid_pl, &snap_pl, &len_pl);
    CHECK(memcmp(d_ref, d_pl, 32) == 0 && memcmp(mid_ref, mid_pl, 32) == 0); /* tiers agree */
    if (snap_ref != NULL) {
        /* In place (the world that ran), then into a fresh world on the other tier. */
        SuHostWorld *f = make_world(SU_HOST_TIER_PLANNED);
        CHECK(su_host_snapshot_restore_inplace(w, snap_ref, len_ref, &e) == SU_OK);
        continue_from_mid(w, mid_ref, d_ref);
        CHECK(f != NULL);
        if (f != NULL) {
            CHECK(su_host_snapshot_restore(f, snap_ref, len_ref, &e) == SU_OK);
            continue_from_mid(f, mid_ref, d_ref);
            su_host_world_destroy(f);
        }
    }
    free(snap_ref);
    free(snap_pl);
    su_host_world_destroy(w);
    su_host_world_destroy(p);
    printf("test_host_handle: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
