/* RTS host-operator known-answer tests (M3-04; contract M3_RTS_CONTRACT r4 sections 4.1 to 4.4, 10.1, 10.7 T-HOST-05).
 *
 * Every case runs one tick of the real scheduler (run_host: views, staging, commit, zero-dead) on a 39-field
 * rts world, and compares the committed fields with values derived from the contract text. The fixtures and
 * expected values are written here from the contract; they are not copied from the Python model.
 *
 * Usage: test_rts_ops   (CTest runs it from the repository root; the record check is test_rts_records)
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_sha256.h"
#include "runtime/rts/host_decl.h"
#include "runtime/rts/host_registry.h"
#include "rts_fixture.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_failures++;                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
        }                                                       \
    } while (0)

/* Handle of the live row at slot (generation from the committed table) and despawn. */
static uint32_t despawn_slot(SuWorld *w, uint32_t slot) {
    uint32_t gen = 0;
    uint64_t handle;
    if (su_world_generation_view(w, slot, 1u, &gen) != SU_OK) {
        return SU_E_INTERNAL;
    }
    handle = ((uint64_t)gen << 32) | slot;
    return su_world_despawn(w, handle);
}

/* Spawns rows in slot order from slot 0 (the lowest free slot of an empty world). */
static uint32_t spawn_rows(SuWorld *w, const RtsRow *rows, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        SuEntity h = 0;
        if (rts_spawn(w, &rows[i], &h) != SU_OK) {
            return SU_E_RESOURCE_LIMIT;
        }
        if ((uint32_t)(h & 0xFFFFFFFFu) != i) {
            return SU_E_INTERNAL;
        }
    }
    return SU_OK;
}

/* Scenario helper: capacity cap, n rows spawned in slot order, one tick. Returns the tick status. */
static uint32_t scenario(SuWorld **wout, SuSchedule *s, uint32_t cap, const RtsRow *rows, uint32_t n) {
    SuWorld *w = rts_world_create(cap);
    uint32_t st;
    su_sched_init(s);
    *wout = w;
    if (w == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = spawn_rows(w, rows, n);
    if (st != SU_OK) {
        return st;
    }
    st = rts_schedule_build(s);
    if (st != SU_OK) {
        return st;
    }
    return rts_tick(w, s);
}

static void finish(SuWorld *w, SuSchedule *s) {
    su_sched_free(s);
    su_world_destroy(w);
}

/* ---------- descriptors, registry, constants hash (3.1, 4.4) ---------- */

static void test_descriptors(void) {
    SuSha256 c;
    uint8_t h[32];
    CHECK(su_host_desc_validate(&g_rts_spatial_desc) == SU_OK, "spatial descriptor validates");
    CHECK(su_host_desc_validate(&g_rts_scatter_desc) == SU_OK, "scatter descriptor validates");
    CHECK(su_host_find("rts.spatial") == &g_rts_spatial_desc, "registry finds rts.spatial");
    CHECK(su_host_find("rts.scatter") == &g_rts_scatter_desc, "registry finds rts.scatter");
    CHECK(su_host_find("rts.goal") == NULL, "registry has no other entry (rts.goal is IR)");
    su_sha256_init(&c);
    su_sha256_update(&c, rts_spatial_constants_text(), strlen(rts_spatial_constants_text()));
    su_sha256_final(&c, h);
    CHECK(memcmp(h, g_rts_spatial_desc.constants_hash, 32) == 0, "spatial constants_hash equals SHA-256 of its text");
    su_sha256_init(&c);
    su_sha256_update(&c, rts_scatter_constants_text(), strlen(rts_scatter_constants_text()));
    su_sha256_final(&c, h);
    CHECK(memcmp(h, g_rts_scatter_desc.constants_hash, 32) == 0, "scatter constants_hash equals SHA-256 of its text");
    CHECK(g_rts_spatial_desc.field_count == 18u && g_rts_scatter_desc.field_count == 5u, "declared field counts");
}

/* ---------- SPATIAL (4.1, 10.1) ---------- */

/* Nearest by (dist_sq, slot): two enemies at dist_sq 100; the lower slot wins (both orders). */
static void test_sp_tie(void) {
    RtsRow rows[3];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    rows[0] = rts_unit(0u, 100u, 100u, 400u);
    rows[1] = rts_unit(1u, 110u, 100u, 0u);
    rows[2] = rts_unit(1u, 100u, 90u, 0u);
    st = scenario(&w, &s, 4u, rows, 3u);
    CHECK(st == SU_OK, "tie scenario ticks (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == 1u, "tie: lower slot 1 wins, got %u", rts_read(w, F_TARGET_SLOT, 0));
        CHECK(rts_read(w, F_TARGET_GEN, 0) == 1u, "tie: target generation 1");
        CHECK(rts_read(w, F_TARGET_DIST, 0) == 100u, "tie: dist_sq 100");
        CHECK(rts_read(w, F_TARGET_VALID, 0) == 1u, "tie: valid");
        CHECK(rts_read(w, F_TARGET_X, 0) == 110u && rts_read(w, F_TARGET_Y, 0) == 100u, "tie: target position");
        CHECK(rts_read(w, F_TRUNCATED, 0) == 0u, "tie: not truncated");
    }
    finish(w, &s);
    /* Reversed geometry: the lower slot is now the other enemy; the lower slot still wins. */
    rows[1] = rts_unit(1u, 100u, 90u, 0u);
    rows[2] = rts_unit(1u, 110u, 100u, 0u);
    st = scenario(&w, &s, 4u, rows, 3u);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == 1u, "tie reversed: lower slot 1 wins");
        CHECK(rts_read(w, F_TARGET_X, 0) == 100u && rts_read(w, F_TARGET_Y, 0) == 90u, "tie reversed: position");
    }
    CHECK(st == SU_OK, "tie reversed ticks (%u)", st);
    finish(w, &s);
}

/* Exact range: dist_sq == range_sq qualifies; range_sq - 1 does not (dist_sq 2500 from (0,0) to (30,40)). */
static void test_sp_range_exact(void) {
    RtsRow rows[2];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    rows[0] = rts_unit(0u, 0u, 0u, 2500u);
    rows[1] = rts_unit(1u, 30u, 40u, 0u);
    st = scenario(&w, &s, 2u, rows, 2u);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_VALID, 0) == 1u && rts_read(w, F_TARGET_DIST, 0) == 2500u,
              "range: dist_sq == range_sq qualifies");
    }
    CHECK(st == SU_OK, "range exact ticks (%u)", st);
    finish(w, &s);
    rows[0] = rts_unit(0u, 0u, 0u, 2499u);
    st = scenario(&w, &s, 2u, rows, 2u);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_VALID, 0) == 0u && rts_read(w, F_TARGET_SLOT, 0) == RTS_NONE,
              "range: range_sq - 1 does not qualify");
    }
    CHECK(st == SU_OK, "range minus one ticks (%u)", st);
    finish(w, &s);
}

/* Cell boundaries at x = 63/64 and y = 63/64, 255/256 (cell edges of 64 units). */
static void test_sp_cell_boundary(void) {
    struct Case {
        uint32_t qx, qy, ex, ey, range_sq, dist;
    } cases[3] = {
        {63u, 10u, 64u, 10u, 1u, 1u},
        {64u, 10u, 63u, 10u, 1u, 1u},
        {10u, 255u, 10u, 256u, 1u, 1u},
    };
    uint32_t i;
    for (i = 0; i < 3u; i++) {
        RtsRow rows[2];
        SuWorld *w = NULL;
        SuSchedule s;
        uint32_t st;
        rows[0] = rts_unit(0u, cases[i].qx, cases[i].qy, cases[i].range_sq);
        rows[1] = rts_unit(1u, cases[i].ex, cases[i].ey, 0u);
        st = scenario(&w, &s, 2u, rows, 2u);
        CHECK(st == SU_OK, "boundary case %u ticks (%u)", i, st);
        if (st == SU_OK) {
            CHECK(rts_read(w, F_TARGET_SLOT, 0) == 1u && rts_read(w, F_TARGET_DIST, 0) == cases[i].dist,
                  "boundary case %u: enemy across the cell edge is found", i);
        }
        finish(w, &s);
    }
}

/* Domain edges: a query at x = 0 and at x = 32767 clips the box; the enemy one unit away is found. */
static void test_sp_domain_edges(void) {
    RtsRow rows[2];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    rows[0] = rts_unit(0u, 0u, 0u, 1u);
    rows[1] = rts_unit(1u, 0u, 1u, 0u);
    st = scenario(&w, &s, 2u, rows, 2u);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == 1u, "edge x=0,y=0 finds (0,1)");
    }
    CHECK(st == SU_OK, "edge low ticks (%u)", st);
    finish(w, &s);
    rows[0] = rts_unit(0u, 32767u, 32767u, 1u);
    rows[1] = rts_unit(1u, 32766u, 32767u, 0u);
    st = scenario(&w, &s, 2u, rows, 2u);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == 1u && rts_read(w, F_TARGET_DIST, 0) == 1u,
              "edge x=32767 finds (32766,32767)");
    }
    CHECK(st == SU_OK, "edge high ticks (%u)", st);
    finish(w, &s);
}

/* Clipping at the low domain edge: a querier in grid row 0 finds an enemy one ring below (cy - d would wrap). */
static void test_sp_low_edge_ring(void) {
    RtsRow rows[2];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    rows[0] = rts_unit(0u, 10u, 10u, 3600u);
    rows[1] = rts_unit(1u, 10u, 70u, 0u);
    st = scenario(&w, &s, 2u, rows, 2u);
    CHECK(st == SU_OK, "low edge ticks (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == 1u && rts_read(w, F_TARGET_DIST, 0) == 3600u,
              "low edge: enemy in ring 1 below a row-0 querier is found");
    }
    finish(w, &s);
}

/* Self, same team, dead, and anchor entries are never targets. */
static void test_sp_exclusions(void) {
    RtsRow rows[4];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    /* self only */
    rows[0] = rts_unit(0u, 500u, 500u, 1000u);
    st = scenario(&w, &s, 1u, rows, 1u);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_VALID, 0) == 0u && rts_read(w, F_TARGET_SLOT, 0) == RTS_NONE, "self excluded");
    }
    CHECK(st == SU_OK, "self-only ticks (%u)", st);
    finish(w, &s);
    /* same team in range */
    rows[0] = rts_unit(0u, 500u, 500u, 1000u);
    rows[1] = rts_unit(0u, 510u, 500u, 0u);
    st = scenario(&w, &s, 2u, rows, 2u);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_VALID, 0) == 0u, "same team excluded");
    }
    CHECK(st == SU_OK, "same-team ticks (%u)", st);
    finish(w, &s);
    /* anchor (kind 2) in range is never a target */
    rows[0] = rts_unit(0u, 500u, 500u, 1000u);
    rows[1] = rts_anchor(1u, 501u, 500u);
    st = scenario(&w, &s, 2u, rows, 2u);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_VALID, 0) == 0u, "anchor (kind 2) is never indexed");
    }
    CHECK(st == SU_OK, "anchor ticks (%u)", st);
    finish(w, &s);
    /* dead enemy: spawned, then despawned before the (single) tick */
    {
        SuWorld *w2 = rts_world_create(2u);
        SuSchedule s2;
        CHECK(w2 != NULL, "dead enemy world");
        if (w2 != NULL) {
            rows[0] = rts_unit(0u, 500u, 500u, 1000u);
            rows[1] = rts_unit(1u, 501u, 500u, 0u);
            CHECK(spawn_rows(w2, rows, 2u) == SU_OK, "dead enemy spawn");
            CHECK(despawn_slot(w2, 1u) == SU_OK, "dead enemy despawn");
            CHECK(rts_schedule_build(&s2) == SU_OK, "dead enemy schedule");
            st = rts_tick(w2, &s2);
            CHECK(st == SU_OK, "dead enemy tick (%u)", st);
            if (st == SU_OK) {
                CHECK(rts_read(w2, F_TARGET_VALID, 0) == 0u, "dead enemy excluded");
            }
            finish(w2, &s2);
        }
    }
}

/* Budget (4.1(c)3, 10.1): versioned K examined entries, self included. box_total 128 is not truncated; 129 is. */
static void test_sp_budget(void) {
    enum { K = RTS_OPS_BUDGET_K };
    RtsRow rows[K + 2u];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    uint32_t i;
    rows[0] = rts_unit(0u, 10u, 10u, 1u);
    for (i = 1u; i < K - 1u; i++) rows[i] = rts_unit(0u, 10u, 10u, 0u);
    rows[K - 1u] = rts_unit(1u, 10u, 11u, 0u);
    st = scenario(&w, &s, K, rows, K);
    CHECK(st == SU_OK, "budget K ticks (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TRUNCATED, 0) == 0u, "box_total K gives truncated 0");
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == K - 1u, "the Kth examined entry qualifies");
    }
    finish(w, &s);
    rows[K - 1u] = rts_unit(0u, 10u, 10u, 0u);
    rows[K] = rts_unit(1u, 10u, 11u, 0u);
    st = scenario(&w, &s, K + 1u, rows, K + 1u);
    CHECK(st == SU_OK, "budget K+1 ticks (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TRUNCATED, 0) == 1u, "box_total K+1 gives truncated 1");
        CHECK(rts_read(w, F_TARGET_VALID, 0) == 0u, "K+1th entry is not examined (budget spent)");
    }
    finish(w, &s);
}

/* Ring-order fixture (10.1, audit N-i): 200 friendly entries in the querier's ring-0 cell and an enemy in
 * ring 1 within range. The 128 examinations are spent on ring 0, so target_valid 0 and truncated 1. */
static void test_sp_ring_fixture(void) {
    RtsRow rows[202];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    uint32_t i;
    rows[0] = rts_unit(0u, 1000u, 1000u, 900u);
    for (i = 1; i <= 200u; i++) {
        rows[i] = rts_unit(0u, 990u, 990u, 0u);
    }
    rows[201] = rts_unit(1u, 1030u, 1000u, 0u);
    st = scenario(&w, &s, 202u, rows, 202u);
    CHECK(st == SU_OK, "ring fixture ticks (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_VALID, 0) == 0u, "ring fixture: target_valid 0");
        CHECK(rts_read(w, F_TRUNCATED, 0) == 1u, "ring fixture: truncated 1");
    }
    finish(w, &s);
    /* With 101 friends (box of rings 0 and 1 is 103 entries) the budget is not spent on ring 0: the ring-1
     * enemy is examined and found, and nothing is truncated. */
    for (i = 1; i <= 101u; i++) {
        rows[i] = rts_unit(0u, 990u, 990u, 0u);
    }
    rows[102] = rts_unit(1u, 1030u, 1000u, 0u);
    st = scenario(&w, &s, 103u, rows, 103u);
    CHECK(st == SU_OK, "ring 1 ticks (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == 102u, "ring 1 enemy found when the budget remains");
        CHECK(rts_read(w, F_TRUNCATED, 0) == 0u, "ring 1 case not truncated");
    }
    finish(w, &s);
}

/* target_gen is the generation of the target after churn (slot 1 respawned: generation 2). */
static void test_sp_target_gen(void) {
    SuWorld *w = rts_world_create(4u);
    SuSchedule s;
    RtsRow q = rts_unit(0u, 100u, 100u, 400u);
    RtsRow e = rts_unit(1u, 120u, 100u, 0u);
    uint32_t st;
    SuEntity h = 0;
    CHECK(w != NULL, "gen world");
    if (w == NULL) {
        return;
    }
    CHECK(rts_spawn(w, &q, &h) == SU_OK, "gen: querier");
    CHECK(rts_spawn(w, &e, &h) == SU_OK, "gen: first enemy");
    CHECK(despawn_slot(w, 1u) == SU_OK, "gen: despawn enemy");
    CHECK(rts_spawn(w, &e, &h) == SU_OK && (uint32_t)(h & 0xFFFFFFFFu) == 1u && (h >> 32) == 2u,
          "gen: enemy respawned in slot 1 with generation 2");
    st = rts_schedule_build(&s);
    CHECK(st == SU_OK, "gen: schedule");
    st = rts_tick(w, &s);
    CHECK(st == SU_OK, "gen: tick (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == 1u && rts_read(w, F_TARGET_GEN, 0) == 2u,
              "target_gen is the current generation of the target");
        CHECK(rts_read(w, F_TARGET_X, 0) == 120u, "target_x from the target row");
    }
    finish(w, &s);
}

/* Domain and range violations are SU_E_INTERNAL (4.1(a), 2.1), and the tick aborts. */
static void test_sp_domain_refusal(void) {
    RtsRow rows[2];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    rows[0] = rts_unit(0u, 32768u, 5u, 0u);
    st = scenario(&w, &s, 1u, rows, 1u);
    CHECK(st == SU_E_INTERNAL, "position above the domain is SU_E_INTERNAL (got %u)", st);
    finish(w, &s);
    rows[0] = rts_unit(0u, 5u, 5u, 262145u);
    st = scenario(&w, &s, 1u, rows, 1u);
    CHECK(st == SU_E_INTERNAL, "range_sq above 262144 is SU_E_INTERNAL (got %u)", st);
    finish(w, &s);
}

/* ---------- GATHER (4.1(a), 10.1) ---------- */

static void test_gather(void) {
    RtsRow rows[10];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    rows[0] = rts_anchor(0u, 500u, 600u);      /* slot 0 anchor, gen 1 */
    rows[1] = rts_unit(0u, 1u, 1u, 0u);        /* valid member */
    rows[1].v[F_FSLOT] = 0u;
    rows[1].v[F_FGEN] = 1u;
    rows[2] = rts_anchor(0u, 0u, 0u);          /* zero-valued legitimate position */
    rows[3] = rts_unit(0u, 1u, 1u, 0u);        /* member of slot 2 (position 0,0) */
    rows[3].v[F_FSLOT] = 2u;
    rows[3].v[F_FGEN] = 1u;
    rows[4] = rts_unit(0u, 1u, 1u, 0u);        /* stale generation */
    rows[4].v[F_FSLOT] = 0u;
    rows[4].v[F_FGEN] = 2u;
    rows[5] = rts_anchor(0u, 7u, 7u);          /* despawned below */
    rows[6] = rts_unit(0u, 1u, 1u, 0u);        /* anchor despawned */
    rows[6].v[F_FSLOT] = 5u;
    rows[6].v[F_FGEN] = 1u;
    rows[7] = rts_unit(0u, 1u, 1u, 0u);        /* NONE */
    rows[8] = rts_unit(0u, 1u, 1u, 0u);        /* out of range (slot >= capacity) */
    rows[8].v[F_FSLOT] = 100u;
    rows[8].v[F_FGEN] = 1u;
    /* forged generation: formation_gen 2 is the generation of the dead anchor in slot 5 (the control plane accepts
     * any nonzero formation_gen, so the alive check is not implied by the generation rule). */
    rows[9] = rts_unit(0u, 1u, 1u, 0u);
    rows[9].v[F_FSLOT] = 5u;
    rows[9].v[F_FGEN] = 2u;
    w = rts_world_create(10u);
    CHECK(w != NULL, "gather world");
    if (w == NULL) {
        return;
    }
    CHECK(spawn_rows(w, rows, 10u) == SU_OK, "gather spawn");
    CHECK(despawn_slot(w, 5u) == SU_OK, "gather despawn anchor");
    st = rts_schedule_build(&s);
    CHECK(st == SU_OK, "gather schedule");
    st = rts_tick(w, &s);
    CHECK(st == SU_OK, "gather tick (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_ANCHOR_VALID, 1) == 1u && rts_read(w, F_ANCHOR_X, 1) == 500u &&
                  rts_read(w, F_ANCHOR_Y, 1) == 600u,
              "gather: valid anchor gives its position");
        CHECK(rts_read(w, F_ANCHOR_VALID, 3) == 1u && rts_read(w, F_ANCHOR_X, 3) == 0u &&
                  rts_read(w, F_ANCHOR_Y, 3) == 0u,
              "gather: zero position is distinguishable through the flag");
        CHECK(rts_read(w, F_ANCHOR_VALID, 4) == 0u && rts_read(w, F_ANCHOR_X, 4) == 0u &&
                  rts_read(w, F_ANCHOR_Y, 4) == 0u,
              "gather: stale generation invalid, zeros");
        CHECK(rts_read(w, F_ANCHOR_VALID, 6) == 0u && rts_read(w, F_ANCHOR_X, 6) == 0u,
              "gather: despawned anchor invalid");
        CHECK(rts_read(w, F_ANCHOR_VALID, 7) == 0u, "gather: NONE invalid");
        CHECK(rts_read(w, F_ANCHOR_VALID, 8) == 0u, "gather: out of range invalid");
        CHECK(rts_read(w, F_ANCHOR_VALID, 9) == 0u && rts_read(w, F_ANCHOR_X, 9) == 0u,
              "gather: forged generation of a dead anchor is invalid (alive check)");
    }
    finish(w, &s);
}

/* ---------- SCATTER (4.3, 10.1) ---------- */

/* Two arrivals of 0xFFFFFFF0 saturate to 0xFFFFFFFF (a wrapping add would give 0xFFFFFFE0). */
static void test_scatter_saturation(void) {
    RtsRow rows[3];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    rows[0] = rts_unit(1u, 100u, 100u, 0u);
    rows[1] = rts_unit(0u, 110u, 100u, 0u);
    rows[2] = rts_unit(0u, 120u, 100u, 0u);
    rows[1].v[F_DMG_OUT] = 0xFFFFFFF0u;
    rows[1].v[F_DMG_TARGET] = 0u;
    rows[1].v[F_DMG_TARGET_GEN] = 1u;
    rows[2].v[F_DMG_OUT] = 0xFFFFFFF0u;
    rows[2].v[F_DMG_TARGET] = 0u;
    rows[2].v[F_DMG_TARGET_GEN] = 1u;
    st = scenario(&w, &s, 3u, rows, 3u);
    CHECK(st == SU_OK, "saturation ticks (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_INCOMING, 0) == 0xFFFFFFFFu, "two arrivals of 0xFFFFFFF0 saturate, got 0x%08x",
              rts_read(w, F_INCOMING, 0));
    }
    finish(w, &s);
}

/* Defence in depth (the engine keeps dead rows zero, so this state is built on purpose): a dead slot whose columns
 * say "unit" and whose generation matches the shooter's aim must still receive nothing (alive check of 4.3). */
static void test_scatter_dead_defensive(void) {
    SuWorld *w = rts_world_create(4u);
    SuSchedule s;
    RtsRow t = rts_unit(1u, 100u, 100u, 0u);
    RtsRow sh = rts_unit(0u, 110u, 100u, 0u);
    uint32_t vals[4];
    uint32_t i;
    uint32_t st;
    SuEntity h = 0;
    const uint32_t *kind = NULL;
    uint64_t rows = 0;
    CHECK(w != NULL, "dead defensive world");
    if (w == NULL) {
        return;
    }
    CHECK(rts_spawn(w, &t, &h) == SU_OK, "dead defensive: target at slot 0");
    CHECK(rts_spawn(w, &sh, &h) == SU_OK, "dead defensive: shooter at slot 1");
    CHECK(rts_spawn(w, &t, &h) == SU_OK, "dead defensive: slot 2");
    CHECK(despawn_slot(w, 2u) == SU_OK, "dead defensive: despawn slot 2 (generation 2)");
    CHECK(su_world_read_committed(w, F_KIND, &kind, &rows) == SU_OK && kind != NULL && rows == 4u,
          "dead defensive: read kind column");
    if (kind != NULL) {
        for (i = 0; i < 4u; i++) {
            vals[i] = kind[i];
        }
        vals[2] = 1u; /* outside the invariant: a dead slot that looks like a unit */
        CHECK(su_world_init_committed(w, F_KIND, vals, 4u) == SU_OK, "dead defensive: write kind of slot 2");
    }
    sh.v[F_DMG_OUT] = 6u;
    sh.v[F_DMG_TARGET] = 2u;
    sh.v[F_DMG_TARGET_GEN] = 2u;
    CHECK(su_world_init_committed(w, F_DMG_OUT, (uint32_t[4]){0u, 6u, 0u, 0u}, 4u) == SU_OK,
          "dead defensive: shooter damage");
    CHECK(su_world_init_committed(w, F_DMG_TARGET, (uint32_t[4]){RTS_NONE, 2u, RTS_NONE, RTS_NONE}, 4u) == SU_OK,
          "dead defensive: shooter target");
    CHECK(su_world_init_committed(w, F_DMG_TARGET_GEN, (uint32_t[4]){0u, 2u, 0u, 0u}, 4u) == SU_OK,
          "dead defensive: shooter generation");
    st = rts_schedule_build(&s);
    CHECK(st == SU_OK, "dead defensive: schedule");
    st = rts_tick(w, &s);
    CHECK(st == SU_OK, "dead defensive: tick (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_INCOMING, 2) == 0u, "dead slot receives nothing (alive check)");
    }
    finish(w, &s);
}

/* Multiple shooters summed; the preset incoming of the target is kept (read_write), the other rows keep theirs. */
static void test_scatter_sum_and_keep(void) {
    RtsRow rows[5];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    rows[0] = rts_unit(1u, 100u, 100u, 0u);
    rows[0].v[F_INCOMING] = 5u;
    rows[1] = rts_unit(0u, 110u, 100u, 0u);
    rows[1].v[F_DMG_OUT] = 3u;
    rows[1].v[F_DMG_TARGET] = 0u;
    rows[1].v[F_DMG_TARGET_GEN] = 1u;
    rows[2] = rts_unit(0u, 120u, 100u, 0u);
    rows[2].v[F_DMG_OUT] = 4u;
    rows[2].v[F_DMG_TARGET] = 0u;
    rows[2].v[F_DMG_TARGET_GEN] = 1u;
    rows[3] = rts_unit(1u, 300u, 300u, 0u);
    rows[3].v[F_INCOMING] = 9u;
    rows[4] = rts_unit(0u, 310u, 300u, 0u);
    rows[4].v[F_DMG_OUT] = 0u;
    rows[4].v[F_DMG_TARGET] = RTS_NONE;
    rows[4].v[F_DMG_TARGET_GEN] = 1u;
    st = scenario(&w, &s, 5u, rows, 5u);
    CHECK(st == SU_OK, "sum ticks (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_INCOMING, 0) == 12u, "5 preset + 3 + 4 = 12, got %u", rts_read(w, F_INCOMING, 0));
        CHECK(rts_read(w, F_INCOMING, 3) == 9u, "non-target row keeps its incoming");
    }
    finish(w, &s);
}

/* Stale generation, non-unit target, dead shooter, and a shooter with dmg_out 0 or NONE target: no damage. */
static void test_scatter_refusals(void) {
    SuWorld *w = rts_world_create(8u);
    SuSchedule s;
    RtsRow t = rts_unit(1u, 100u, 100u, 0u);
    RtsRow sh = rts_unit(0u, 110u, 100u, 0u);
    uint32_t st;
    SuEntity h = 0;
    CHECK(w != NULL, "refusal world");
    if (w == NULL) {
        return;
    }
    /* slot 0: unit target, gen 1. slot 1: unit shooter with stale generation for slot 0 (gen 2 expected). */
    CHECK(rts_spawn(w, &t, &h) == SU_OK, "refusal target spawn");
    /* slot 1: unit target that is respawned: slot 1 gen 2 */
    CHECK(rts_spawn(w, &t, &h) == SU_OK, "refusal stale target spawn");
    CHECK(despawn_slot(w, 1u) == SU_OK, "refusal stale despawn");
    CHECK(rts_spawn(w, &t, &h) == SU_OK && (h >> 32) == 2u, "refusal stale respawn gen 2");
    /* slot 2: anchor (kind 2) target, gen 1 */
    {
        RtsRow a = rts_anchor(1u, 200u, 200u);
        CHECK(rts_spawn(w, &a, &h) == SU_OK, "refusal anchor spawn");
    }
    /* slot 3: shooter A, stale for slot 1 (gen 1 where gen is 2): ignored */
    sh.v[F_DMG_OUT] = 50u;
    sh.v[F_DMG_TARGET] = 1u;
    sh.v[F_DMG_TARGET_GEN] = 1u;
    CHECK(rts_spawn(w, &sh, &h) == SU_OK, "refusal stale shooter spawn");
    /* slot 4: shooter B, aimed at the anchor (kind 2): ignored */
    sh.v[F_DMG_TARGET] = 2u;
    sh.v[F_DMG_TARGET_GEN] = 1u;
    CHECK(rts_spawn(w, &sh, &h) == SU_OK, "refusal anchor shooter spawn");
    /* slot 5: shooter C, dmg_out 0 at slot 0: ignored */
    sh.v[F_DMG_OUT] = 0u;
    sh.v[F_DMG_TARGET] = 0u;
    sh.v[F_DMG_TARGET_GEN] = 1u;
    CHECK(rts_spawn(w, &sh, &h) == SU_OK, "refusal zero shooter spawn");
    /* slot 6: shooter D, to be despawned before the tick (its arrival must not happen) */
    sh.v[F_DMG_OUT] = 77u;
    sh.v[F_DMG_TARGET] = 0u;
    sh.v[F_DMG_TARGET_GEN] = 1u;
    CHECK(rts_spawn(w, &sh, &h) == SU_OK, "refusal dead shooter spawn");
    CHECK(despawn_slot(w, 6u) == SU_OK, "refusal dead shooter despawn");
    /* slot 7: shooter E with NONE target: ignored */
    sh.v[F_DMG_OUT] = 9u;
    sh.v[F_DMG_TARGET] = RTS_NONE;
    sh.v[F_DMG_TARGET_GEN] = 1u;
    CHECK(rts_spawn(w, &sh, &h) == SU_OK, "refusal NONE shooter spawn");
    st = rts_schedule_build(&s);
    CHECK(st == SU_OK, "refusal schedule");
    st = rts_tick(w, &s);
    CHECK(st == SU_OK, "refusal tick (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_INCOMING, 0) == 0u, "no damage: stale generation, anchor, zero, dead, NONE");
        CHECK(rts_read(w, F_INCOMING, 1) == 0u, "no damage on the respawned unit");
        CHECK(rts_read(w, F_INCOMING, 2) == 0u, "no damage on the anchor");
    }
    finish(w, &s);
}

/* ---------- store set and views (4.4, T-HOST-13 shape) ---------- */

static void test_store_set(void) {
    RtsRow rows[6];
    SuWorld *w = NULL;
    SuSchedule s;
    uint32_t st;
    uint32_t i;
    uint8_t *seen;
    RtsOpsInstr instr;
    RtsOpsCtx *sp = rts_spatial_ctx();
    RtsOpsCtx *sc = rts_scatter_ctx();
    uint64_t before_stores;
    rows[0] = rts_unit(0u, 100u, 100u, 400u);
    rows[1] = rts_unit(1u, 110u, 100u, 0u);
    rows[2] = rts_anchor(0u, 50u, 50u);
    rows[3] = rts_unit(0u, 60u, 60u, 0u);
    rows[3].v[F_DMG_OUT] = 2u;
    rows[3].v[F_DMG_TARGET] = 1u;
    rows[3].v[F_DMG_TARGET_GEN] = 1u;
    rows[4] = rts_unit(1u, 120u, 100u, 0u);
    rows[4].v[F_INCOMING] = 7u;
    rows[5] = rts_unit(1u, 900u, 900u, 0u); /* dead below */
    seen = calloc(6u * 11u, 1u);
    CHECK(seen != NULL, "store-set buffer");
    if (seen == NULL) {
        return;
    }
    memset(&instr, 0, sizeof(instr));
    instr.rows = 6u;
    instr.seen = seen;
    instr.n_write = 10u;
    w = rts_world_create(6u);
    CHECK(w != NULL, "store world");
    if (w == NULL) {
        free(seen);
        return;
    }
    CHECK(spawn_rows(w, rows, 5u) == SU_OK, "store spawn");
    CHECK(rts_schedule_build(&s) == SU_OK, "store schedule");
    /* the spatial instrument: write-only fields, every row of every field stored */
    sp->instr = &instr;
    before_stores = instr.stores;
    st = rts_tick(w, &s);
    CHECK(st == SU_OK, "store tick (%u)", st);
    CHECK(instr.violations == 0u, "no store outside the declared write set (%u)", instr.violations);
    CHECK(instr.stores - before_stores == 10u * 6u, "spatial stores exactly 10 per row (%llu)",
          (unsigned long long)(instr.stores - before_stores));
    for (i = 0; i < 10u * 6u; i++) {
        if (i < 60u && seen[i] != 1u) {
            CHECK(0, "spatial write %u row %u not stored", i / 6u, i % 6u);
            break;
        }
    }
    sp->instr = NULL;
    /* scatter writes only target rows of incoming (field 0 of its own write list) */
    memset(seen, 0, 6u * 11u);
    instr.n_write = 1u;
    instr.violations = 0u;
    instr.stores = 0u;
    finish(w, &s);
    w = rts_world_create(6u);
    CHECK(w != NULL && spawn_rows(w, rows, 5u) == SU_OK, "store scatter world");
    CHECK(rts_schedule_build(&s) == SU_OK, "store scatter schedule");
    sc->instr = &instr;
    st = rts_tick(w, &s);
    CHECK(st == SU_OK, "scatter store tick (%u)", st);
    CHECK(instr.stores == 1u, "scatter stores once (one arrival) (%llu)", (unsigned long long)instr.stores);
    CHECK(seen[1] == 1u && seen[0] == 0u && seen[2] == 0u && seen[3] == 0u && seen[4] == 0u,
          "scatter stores only the target row");
    CHECK(instr.violations == 0u, "scatter: no undeclared store");
    CHECK(rts_read(w, F_INCOMING, 1) == 2u, "scatter: arrival of 2 at slot 1");
    CHECK(rts_read(w, F_INCOMING, 4) == 7u, "scatter: non-target keeps 7");
    sc->instr = NULL;
    finish(w, &s);
    free(seen);
}

/* ---------- fault seams (N9, 10.3) ---------- */

static void test_fault_seams(void) {
    RtsRow rows[4];
    SuWorld *w;
    SuSchedule s;
    uint32_t st;
    uint32_t n;
    uint8_t *buf;
    size_t size = 0;
    size_t written = 0;
    RtsOpsCtx *sp = rts_spatial_ctx();
    RtsOpsCtx *sc = rts_scatter_ctx();
    rows[0] = rts_unit(0u, 100u, 100u, 400u);
    rows[1] = rts_unit(1u, 110u, 100u, 0u);
    rows[2] = rts_unit(0u, 50u, 50u, 0u);
    rows[2].v[F_DMG_OUT] = 4u;
    rows[2].v[F_DMG_TARGET] = 1u;
    rows[2].v[F_DMG_TARGET_GEN] = 1u;
    rows[3] = rts_unit(1u, 0u, 0u, 0u);
    w = rts_world_create(4u);
    CHECK(w != NULL, "fault world");
    if (w == NULL) {
        return;
    }
    CHECK(spawn_rows(w, rows, 4u) == SU_OK, "fault spawn");
    CHECK(rts_schedule_build(&s) == SU_OK, "fault schedule");
    CHECK(su_world_canonical_size(w, &size) == SU_OK, "fault canonical size");
    buf = malloc(size);
    CHECK(buf != NULL, "fault buffer");
    if (buf == NULL) {
        finish(w, &s);
        return;
    }
    CHECK(su_world_canonical_write(w, buf, size, &written) == SU_OK, "fault canonical before");
    /* Spatial seams: the gather, the grid and the one query row (3 fault points in this fixture). Each failing
     * tick leaves the committed root unchanged. */
    for (n = 0; n < 3u; n++) {
        uint8_t *after = malloc(size);
        size_t w2 = 0;
        sp->fault_at = n;
        st = rts_tick(w, &s);
        sp->fault_at = RTS_OPS_NO_FAULT;
        CHECK(st == SU_E_INTERNAL, "spatial seam %u returns SU_E_INTERNAL (got %u)", n, st);
        CHECK(after != NULL && su_world_canonical_write(w, after, size, &w2) == SU_OK && w2 == size &&
                  memcmp(after, buf, size) == 0,
              "spatial seam %u: committed root unchanged", n);
        free(after);
    }
    /* Scatter seam at its first shooter. */
    sc->fault_at = 0u;
    st = rts_tick(w, &s);
    sc->fault_at = RTS_OPS_NO_FAULT;
    CHECK(st == SU_E_INTERNAL, "scatter seam returns SU_E_INTERNAL (got %u)", st);
    CHECK(rts_read(w, F_INCOMING, 1) == 0u, "scatter seam: no partial arrival committed");
    st = rts_tick(w, &s);
    CHECK(st == SU_OK, "retry after the seam succeeds (%u)", st);
    if (st == SU_OK) {
        CHECK(rts_read(w, F_INCOMING, 1) == 4u, "retry applies the arrival once");
        CHECK(rts_read(w, F_TARGET_SLOT, 0) == 1u, "retry: spatial result");
    }
    free(buf);
    finish(w, &s);
}

int main(void) {
    test_descriptors();
    test_sp_tie();
    test_sp_range_exact();
    test_sp_cell_boundary();
    test_sp_domain_edges();
    test_sp_low_edge_ring();
    test_sp_exclusions();
    test_sp_budget();
    test_sp_ring_fixture();
    test_sp_target_gen();
    test_sp_domain_refusal();
    test_gather();
    test_scatter_saturation();
    test_scatter_sum_and_keep();
    test_scatter_refusals();
    test_scatter_dead_defensive();
    test_store_set();
    test_fault_seams();
    printf("test_rts_ops: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
