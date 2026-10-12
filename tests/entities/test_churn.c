/* World-level entity churn (M2-00c3b, contract M2_ENTITIES_CONTRACT.md sections 1 and 6).
 *
 * Runs 100,000 control-plane spawn and despawn operations on a real SuWorld (su_world_spawn and
 * su_world_despawn between ticks) and checks every result against an independent model: lowest-slot
 * reuse, generation bumps, stale refusal (generation 0, wrong generation, out of range, zero, a retired
 * slot), counts and high_water. A retired slot is produced through the test seam
 * su_ents_fault_set_generation (generation 0xFFFFFFFE, then one despawn) and is never reissued.
 *
 * Build and run from the repository root: cmake --build <dir> --target test_churn && <dir>/test_churn */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/entities.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "world/su_world.h"
#include "world/entities/su_ents.h"

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

#define CHECK_ST(expr, want)                                                        \
    do {                                                                            \
        uint32_t got_ = (expr);                                                     \
        g_checks++;                                                                 \
        if (got_ != (want)) {                                                       \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: %s -> %s, want %s\n", __FILE__, __LINE__, #expr, \
                    su_status_name(got_), su_status_name(want));                    \
        }                                                                           \
    } while (0)

#define CAPACITY 64u
#define OPS 100000u
#define ID_A 10u

static const SuWorldFieldDesc k_fields[] = {{ID_A, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "a"}};

/* Independent model: liveness, stored generation, retirement and the high water mark. */
typedef struct Model {
    uint8_t alive[CAPACITY];
    uint32_t gen[CAPACITY];
    uint8_t retired[CAPACITY];
    uint32_t hw;
    uint64_t n_alive;
    uint64_t n_retired;
} Model;

static void model_init(Model *m) {
    uint32_t i;
    memset(m, 0, sizeof *m);
    for (i = 0; i < CAPACITY; i++) {
        m->gen[i] = SU_GEN_FIRST;
    }
}

/* Lowest slot that is neither alive nor retired; CAPACITY when none. */
static uint32_t model_lowest_free(const Model *m) {
    uint32_t i;
    for (i = 0; i < CAPACITY; i++) {
        if (!m->alive[i] && !m->retired[i]) {
            return i;
        }
    }
    return CAPACITY;
}

static int model_is_live(const Model *m, uint64_t h) {
    uint32_t slot = SU_ENTITY_SLOT(h);
    uint32_t gen = SU_ENTITY_GEN(h);
    return gen != 0u && slot < CAPACITY && m->alive[slot] && m->gen[slot] == gen && !m->retired[slot];
}

static void model_despawn(Model *m, uint64_t h) {
    uint32_t slot = SU_ENTITY_SLOT(h);
    m->alive[slot] = 0u;
    m->n_alive--;
    if (m->gen[slot] + 1u == SU_GEN_RETIRED) {
        m->gen[slot] = SU_GEN_RETIRED;
        m->retired[slot] = 1u;
        m->n_retired++;
    } else {
        m->gen[slot]++;
    }
}

/* Deterministic generator (xorshift64*). */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 2685821657736338717ull;
}

static void check_tables(SuWorld *w, const Model *m, uint32_t where) {
    const SuEnts *e = su_world_entities_committed(w);
    uint32_t i;
    int ok = 1;
    if (su_ents_alive_count(e) != m->n_alive || su_ents_retired_count(e) != m->n_retired ||
        su_ents_high_water(e) != m->hw) {
        ok = 0;
    }
    for (i = 0; i < CAPACITY && ok; i++) {
        uint32_t g = 0;
        if (su_ents_slot_generation(e, i, &g) != SU_OK || g != m->gen[i] || su_ents_slot_alive(e, i) != m->alive[i]) {
            ok = 0;
        }
    }
    if (!ok) {
        fprintf(stderr, "test_churn: model and world disagree at operation %u\n", (unsigned)where);
        g_failures++;
    }
    g_checks++;
}

static void test_churn(void) {
    SuWorldDesc d;
    SuWorld *w = NULL;
    Model m;
    uint64_t n;
    uint64_t spawns = 0;
    uint64_t despawns = 0;
    uint64_t stale_refused = 0;
    uint64_t *live = calloc(CAPACITY, sizeof *live);
    uint64_t n_live = 0;

    model_init(&m);
    memset(&d, 0, sizeof d);
    d.schema_id = "churn";
    d.schema_version = 1u;
    d.layout_generation = 1u;
    d.row_count = 0u;
    d.fields = k_fields;
    d.field_count = 1u;
    CHECK(live != NULL);
    CHECK_ST(su_world_create_ex(&d, CAPACITY, &w), SU_OK);
    if (w == NULL || live == NULL) {
        free(live);
        return;
    }

    /* Retirement first, deterministically: slots 0 and 1 spawn, slot 0 despawns (gen 2) and is set to
     * generation 0xFFFFFFFE through the seam; the next spawn takes it, its despawn retires it. */
    {
        SuEntity h0 = 0;
        SuEntity h1 = 0;
        SuEntity h2 = 0;
        SuWorldSpawnValue v = {ID_A, 7u};
        SuEnts *ents = (SuEnts *)su_world_entities_committed(w);
        CHECK_ST(su_world_spawn(w, &v, 1u, 1u, &h0), SU_OK);
        CHECK(h0 == SU_ENTITY_MAKE(0, 1));
        CHECK_ST(su_world_spawn(w, &v, 1u, 2u, &h1), SU_OK);
        CHECK(h1 == SU_ENTITY_MAKE(1, 1));
        m.alive[0] = m.alive[1] = 1u;
        m.n_alive = 2u;
        m.hw = 2u;
        CHECK_ST(su_world_despawn(w, h0), SU_OK);
        model_despawn(&m, h0);
        CHECK_ST(su_ents_fault_set_generation(ents, 0u, 0xFFFFFFFEu), SU_OK);
        m.gen[0] = 0xFFFFFFFEu;
        CHECK_ST(su_world_spawn(w, &v, 1u, 3u, &h2), SU_OK);
        CHECK(h2 == SU_ENTITY_MAKE(0, 0xFFFFFFFEu));
        m.alive[0] = 1u;
        m.n_alive++;
        CHECK_ST(su_world_despawn(w, h2), SU_OK);
        model_despawn(&m, h2);
        CHECK(su_ents_slot_retired(su_world_entities_committed(w), 0u) == 1);
        CHECK_ST(su_world_despawn(w, h2), SU_E_STALE_HANDLE);
        check_tables(w, &m, 0u);
    }

    for (n = 0; n < OPS; n++) {
        uint64_t r = rnd();
        uint32_t lowest = model_lowest_free(&m);
        /* Operation mix: half spawns, a quarter despawns, a quarter stale probes. */
        if ((((r >> 1) & 3u) < 2u) || n_live == 0u) {
            /* Spawn: the lowest free slot, or resource refusal when none is free. */
            SuEntity h = 0;
            SuWorldSpawnValue v = {ID_A, (uint32_t)(r >> 8)};
            uint32_t st = su_world_spawn(w, &v, 1u, (uint32_t)(r >> 40), &h);
            if (lowest == CAPACITY) {
                CHECK_ST(st, SU_E_RESOURCE_LIMIT);
            } else {
                CHECK_ST(st, SU_OK);
                CHECK(h == SU_ENTITY_MAKE(lowest, m.gen[lowest]));
                m.alive[lowest] = 1u;
                m.n_alive++;
                if (lowest + 1u > m.hw) {
                    m.hw = lowest + 1u;
                }
                live[n_live++] = h;
                spawns++;
            }
        } else if (((r >> 1) & 3u) == 3u) {
            /* Stale probes: generation 0, a generation that is not stored, out of range, zero. */
            uint64_t pick = live[(r >> 16) % n_live];
            uint64_t probe;
            uint32_t st;
            switch ((r >> 8) % 4u) {
            case 0:
                probe = SU_ENTITY_MAKE(SU_ENTITY_SLOT(pick), 0u);
                break;
            case 1:
                probe = SU_ENTITY_MAKE(SU_ENTITY_SLOT(pick), SU_ENTITY_GEN(pick) + 1u);
                break;
            case 2:
                probe = SU_ENTITY_MAKE(CAPACITY + (uint32_t)((r >> 20) % 100u), 1u);
                break;
            default:
                probe = 0u;
                break;
            }
            st = su_world_despawn(w, probe);
            CHECK_ST(st, SU_E_STALE_HANDLE);
            stale_refused++;
        } else {
            /* Despawn a live entity: the stored generation advances, the slot is free again. */
            uint64_t pick_i = (r >> 16) % n_live;
            uint64_t h = live[pick_i];
            CHECK(model_is_live(&m, h));
            CHECK_ST(su_world_despawn(w, h), SU_OK);
            model_despawn(&m, h);
            live[pick_i] = live[--n_live];
            despawns++;
        }
        if (n % 1000u == 999u) {
            check_tables(w, &m, (uint32_t)n);
        }
    }
    check_tables(w, &m, OPS);

    /* A retired slot is never reissued: spawn until none is free and count the refusals. */
    {
        uint64_t guard = 0;
        SuWorldSpawnValue v = {ID_A, 1u};
        while (model_lowest_free(&m) != CAPACITY && guard++ < CAPACITY) {
            SuEntity h = 0;
            uint32_t slot = model_lowest_free(&m);
            CHECK_ST(su_world_spawn(w, &v, 1u, 9u, &h), SU_OK);
            CHECK(SU_ENTITY_SLOT(h) == slot);
            m.alive[slot] = 1u;
            m.n_alive++;
            if (slot + 1u > m.hw) {
                m.hw = slot + 1u;
            }
        }
        CHECK_ST(su_world_spawn(w, &v, 1u, 9u, &(SuEntity){0}), SU_E_RESOURCE_LIMIT);
        check_tables(w, &m, OPS + 1u);
    }

    CHECK(spawns > 0u && despawns > 0u && stale_refused > 0u);
    printf("test_churn: operations=%u spawns=%llu despawns=%llu stale_refused=%llu high_water=%u retired=%llu\n",
           (unsigned)OPS, (unsigned long long)spawns, (unsigned long long)despawns,
           (unsigned long long)stale_refused, (unsigned)m.hw, (unsigned long long)m.n_retired);
    su_world_destroy(w);
    free(live);
}

int main(void) {
    test_churn();
    printf("test_churn: checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
