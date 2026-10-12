/* M2-00a tests for the generational entity table (src/world/entities/su_ents.h, contract r2).
 * Self-contained assert harness. The churn test checks against an independent reference model: a
 * plain array of slot states with a linear lowest-free scan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/entities.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
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

#define GEN_MAX_LIVE UINT32_C(0xFFFFFFFE)
#define GEN_RETIRED UINT32_C(0xFFFFFFFF)
#define CHUNK UINT32_C(1024)

/* Dump layout (contract r2): header 60 bytes, then 9 bytes per slot below high_water. */
#define DUMP_HEADER 60u
#define DUMP_REC 9u
#define OFF_CAP 28u
#define OFF_HW 36u
#define OFF_ALIVE 44u
#define OFF_RET 52u
#define REC(i) (DUMP_HEADER + DUMP_REC * (size_t)(i))

/* ---- deterministic PRNG (xorshift64*) --------------------------------- */

typedef struct Rng {
    uint64_t s;
} Rng;

static uint64_t rng_next(Rng *r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * UINT64_C(0x2545F4914F6CDD1D);
}

static uint32_t rng_below(Rng *r, uint32_t n) {
    return (uint32_t)(rng_next(r) % n);
}

/* ---- helpers ---------------------------------------------------------- */

static void set_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void set_u64_le(uint8_t *p, uint64_t v) {
    set_u32_le(p, (uint32_t)(v & UINT64_C(0xFFFFFFFF)));
    set_u32_le(p + 4, (uint32_t)(v >> 32));
}

static SuEnts *make_table(uint64_t max_entities, uint64_t initial_alive, const uint32_t *ext) {
    SuEntsDesc d;
    SuEnts *e = NULL;
    uint32_t st;
    memset(&d, 0, sizeof d);
    d.max_entities = max_entities;
    d.initial_alive = initial_alive;
    d.external_ids = ext;
    st = su_ents_create(&d, &e);
    if (st != SU_OK || e == NULL) {
        fprintf(stderr, "make_table(%llu, %llu) failed: %s\n", (unsigned long long)max_entities,
                (unsigned long long)initial_alive, su_status_name(st));
        g_failures++;
        return NULL;
    }
    return e;
}

static uint8_t *dump_of(const SuEnts *e, size_t *len) {
    size_t n = 0;
    size_t w = 0;
    uint8_t *buf;
    CHECK_ST(su_ents_canonical_size(e, &n), SU_OK);
    buf = malloc(n > 0u ? n : 1u);
    if (buf == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    CHECK_ST(su_ents_canonical_write(e, buf, n, &w), SU_OK);
    CHECK(w == n);
    *len = n;
    return buf;
}

/* Dump, parse, compare logical state and re-dumped bytes. */
static void check_round_trip(const SuEnts *e) {
    size_t n = 0;
    uint8_t *buf = dump_of(e, &n);
    SuEnts *back = NULL;
    size_t n2 = 0;
    uint8_t *buf2;
    int eq = 0;
    CHECK_ST(su_ents_canonical_parse(buf, n, &back), SU_OK);
    if (back != NULL) {
        CHECK_ST(su_ents_state_equal(e, back, &eq), SU_OK);
        CHECK(eq == 1);
        buf2 = dump_of(back, &n2);
        CHECK(n2 == n && memcmp(buf, buf2, n) == 0);
        free(buf2);
        su_ents_destroy(back);
    }
    free(buf);
}

/* ---- reference model -------------------------------------------------- */

typedef struct MSlot {
    uint32_t alive;
    uint32_t retired;
    uint32_t gen;
    uint32_t ext;
} MSlot;

typedef struct Model {
    MSlot *s;
    uint32_t cap;
    uint32_t alive_n;
    uint32_t retired_n;
} Model;

static Model model_new(uint32_t cap) {
    Model m;
    uint32_t i;
    m.cap = cap;
    m.alive_n = 0;
    m.retired_n = 0;
    m.s = malloc((size_t)(cap > 0u ? cap : 1u) * sizeof(MSlot));
    if (m.s == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    for (i = 0; i < cap; i++) {
        m.s[i].alive = 0;
        m.s[i].retired = 0;
        m.s[i].gen = 1;
        m.s[i].ext = 0;
    }
    return m;
}

/* Lowest slot that is neither alive nor retired. */
static int model_alloc(Model *m, uint32_t ext, uint32_t *slot) {
    uint32_t i;
    for (i = 0; i < m->cap; i++) {
        if (!m->s[i].alive && !m->s[i].retired) {
            m->s[i].alive = 1;
            m->s[i].ext = ext;
            m->alive_n++;
            *slot = i;
            return 1;
        }
    }
    return 0;
}

static void model_despawn(Model *m, uint32_t slot) {
    MSlot *s = &m->s[slot];
    s->alive = 0;
    s->ext = 0;
    m->alive_n--;
    if (s->gen == GEN_MAX_LIVE) {
        s->gen = GEN_RETIRED;
        s->retired = 1;
        m->retired_n++;
    } else {
        s->gen++;
    }
}

/* Full comparison of the table against the model. Returns the number of mismatches. */
static int model_compare(const Model *m, const SuEnts *e) {
    int bad = 0;
    uint32_t i;
    uint32_t expect_next = 0;
    uint32_t chunk;
    uint32_t cur = 0;
    if (su_ents_capacity(e) != m->cap) {
        return 1;
    }
    if (su_ents_alive_count(e) != m->alive_n || su_ents_retired_count(e) != m->retired_n ||
        su_ents_free_count(e) != m->cap - m->alive_n - m->retired_n) {
        bad++;
    }
    for (i = 0; i < m->cap; i++) {
        uint32_t g = 0;
        SuEntity h = SU_ENTITY_INVALID;
        uint32_t ext = 0;
        if (su_ents_slot_alive(e, i) != (int)m->s[i].alive) {
            bad++;
        }
        if (su_ents_slot_retired(e, i) != (int)m->s[i].retired) {
            bad++;
        }
        if (su_ents_slot_generation(e, i, &g) != SU_OK || g != m->s[i].gen) {
            bad++;
        }
        if (m->s[i].alive) {
            if (su_ents_handle_for_slot(e, i, &h) != SU_OK || SU_ENTITY_GEN(h) != m->s[i].gen ||
                SU_ENTITY_SLOT(h) != i) {
                bad++;
            } else if (su_ents_get_external(e, h, &ext) != SU_OK || ext != m->s[i].ext) {
                bad++;
            }
        } else if (su_ents_handle_for_slot(e, i, &h) != SU_E_STALE_HANDLE) {
            bad++;
        }
    }
    /* Alive iteration visits exactly the model's alive slots, ascending. */
    for (;;) {
        uint32_t found = 0;
        if (!su_ents_next_alive(e, cur, &found)) {
            break;
        }
        while (expect_next < m->cap && !m->s[expect_next].alive) {
            expect_next++;
        }
        if (found != expect_next) {
            bad++;
            break;
        }
        expect_next++;
        cur = found + 1u;
    }
    while (expect_next < m->cap && !m->s[expect_next].alive) {
        expect_next++;
    }
    if (expect_next != m->cap) {
        bad++;
    }
    /* Per-chunk counts and bitmap popcounts agree with the model (last chunk may be short). */
    for (chunk = 0; chunk < su_ents_chunk_count(e); chunk++) {
        const uint64_t *words = NULL;
        uint32_t nw = 0;
        uint32_t cnt = 0;
        uint32_t want = 0;
        uint32_t k;
        uint32_t len = m->cap - chunk * CHUNK < CHUNK ? m->cap - chunk * CHUNK : CHUNK;
        uint32_t cc = 0;
        if (su_ents_chunk_alive_bits(e, chunk, &words, &nw) != SU_OK || nw != (len + 63u) / 64u) {
            bad++;
            continue;
        }
        for (k = 0; k < nw; k++) {
            uint64_t v = words[k];
            while (v != 0u) {
                v &= v - 1u;
                cnt++;
            }
        }
        for (k = 0; k < len; k++) {
            want += m->s[chunk * CHUNK + k].alive ? 1u : 0u;
        }
        if (cnt != want) {
            bad++;
        }
        if (su_ents_chunk_alive_count(e, chunk, &cc) != SU_OK || cc != want) {
            bad++;
        }
    }
    return bad;
}

/* ---- tests ------------------------------------------------------------ */

static void test_create_rules(void) {
    SuEntsDesc d;
    SuEnts *e = NULL;
    uint32_t ext[3] = {11u, 0u, 33u};
    SuEntity h = SU_ENTITY_INVALID;
    uint32_t got = 99u;

    /* Capacity is max(initial, max_entities), NOT rounded to chunks. */
    e = make_table(1u, 0u, NULL);
    CHECK(e != NULL && su_ents_capacity(e) == 1u && su_ents_chunk_count(e) == 1u);
    su_ents_destroy(e);
    e = make_table(1025u, 0u, NULL);
    CHECK(e != NULL && su_ents_capacity(e) == 1025u && su_ents_chunk_count(e) == 2u);
    su_ents_destroy(e);
    e = make_table(0u, 3u, NULL);
    CHECK(e != NULL && su_ents_capacity(e) == 3u && su_ents_alive_count(e) == 3u);
    su_ents_destroy(e);
    e = make_table(5u, 7u, NULL);
    CHECK(e != NULL && su_ents_capacity(e) == 7u);
    su_ents_destroy(e);
    e = make_table(0u, 0u, NULL);
    CHECK(e != NULL && su_ents_capacity(e) == 0u && su_ents_chunk_count(e) == 0u);
    CHECK_ST(su_ents_alloc(e, 0u, &h), SU_E_RESOURCE_LIMIT);
    CHECK(su_ents_validate(e, SU_ENTITY_MAKE(0u, 1u)) == SU_E_STALE_HANDLE);
    su_ents_destroy(e);
    e = make_table(SU_LIMIT_ROWS, 0u, NULL);
    CHECK(e != NULL && su_ents_capacity(e) == 1000000u && su_ents_chunk_count(e) == 977u);
    su_ents_destroy(e);

    /* Initial alive slots: generation 1, given external ids (0 allowed), lowest slots first. */
    e = make_table(2048u, 3u, ext);
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(su_ents_alive_count(e) == 3u && su_ents_free_count(e) == 2045u);
        CHECK(su_ents_high_water(e) == 3u);
        CHECK(su_ents_slot_alive(e, 0) && su_ents_slot_alive(e, 2) && !su_ents_slot_alive(e, 3));
        CHECK_ST(su_ents_handle_for_slot(e, 1, &h), SU_OK);
        CHECK(h == SU_ENTITY_MAKE(1u, 1u));
        CHECK_ST(su_ents_get_external(e, h, &got), SU_OK);
        CHECK(got == 0u);
        CHECK_ST(su_ents_handle_for_slot(e, 2, &h), SU_OK);
        CHECK_ST(su_ents_get_external(e, h, &got), SU_OK);
        CHECK(got == 33u);
        su_ents_destroy(e);
    }

    /* Rejections. */
    memset(&d, 0, sizeof d);
    CHECK_ST(su_ents_create(NULL, &e), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_create(&d, NULL), SU_E_INVALID_ARGUMENT);
    CHECK(e == NULL);
    d.max_entities = SU_LIMIT_ROWS + 1u;
    CHECK_ST(su_ents_create(&d, &e), SU_E_RESOURCE_LIMIT);
    CHECK(e == NULL);
    d.max_entities = UINT64_MAX;
    CHECK_ST(su_ents_create(&d, &e), SU_E_RESOURCE_LIMIT);
    CHECK(e == NULL);
    d.max_entities = 0u;
    d.initial_alive = SU_LIMIT_ROWS + 1u;
    CHECK_ST(su_ents_create(&d, &e), SU_E_RESOURCE_LIMIT);
    CHECK(e == NULL);
    su_ents_destroy(NULL);
}

static void test_lowest_slot_reuse(void) {
    SuEnts *e = make_table(4096u, 0u, NULL);
    SuEntity h[6];
    SuEntity again = SU_ENTITY_INVALID;
    uint32_t i;
    if (e == NULL) {
        return;
    }
    for (i = 0; i < 6; i++) {
        CHECK_ST(su_ents_alloc(e, 100u + i, &h[i]), SU_OK);
        CHECK(SU_ENTITY_SLOT(h[i]) == i && SU_ENTITY_GEN(h[i]) == 1u);
    }
    CHECK_ST(su_ents_despawn(e, h[2]), SU_OK);
    CHECK_ST(su_ents_despawn(e, h[4]), SU_OK);
    CHECK_ST(su_ents_alloc(e, 0u, &again), SU_OK);
    CHECK(SU_ENTITY_SLOT(again) == 2u && SU_ENTITY_GEN(again) == 2u);
    CHECK_ST(su_ents_alloc(e, 0u, &again), SU_OK);
    CHECK(SU_ENTITY_SLOT(again) == 4u && SU_ENTITY_GEN(again) == 2u);
    CHECK_ST(su_ents_despawn(e, again), SU_OK); /* slot 4 again: generation 3 next */
    CHECK_ST(su_ents_despawn(e, h[0]), SU_OK);
    CHECK_ST(su_ents_alloc(e, 0u, &again), SU_OK);
    CHECK(SU_ENTITY_SLOT(again) == 0u && SU_ENTITY_GEN(again) == 2u);
    CHECK_ST(su_ents_alloc(e, 0u, &again), SU_OK);
    CHECK(SU_ENTITY_SLOT(again) == 4u && SU_ENTITY_GEN(again) == 3u);
    CHECK(su_ents_high_water(e) == 6u);
    su_ents_destroy(e);
}

static void test_high_water(void) {
    SuEnts *e = make_table(3000u, 0u, NULL);
    SuEntity h[6];
    uint32_t i;
    size_t n = 0;
    uint8_t *d;
    if (e == NULL) {
        return;
    }
    CHECK(su_ents_high_water(e) == 0u);
    CHECK_ST(su_ents_alloc(e, 0u, &h[0]), SU_OK);
    CHECK(su_ents_high_water(e) == 1u);
    for (i = 1; i < 6; i++) {
        CHECK_ST(su_ents_alloc(e, 0u, &h[i]), SU_OK);
    }
    CHECK(su_ents_high_water(e) == 6u);
    for (i = 0; i < 6; i++) {
        CHECK_ST(su_ents_despawn(e, h[i]), SU_OK);
    }
    /* Despawning never lowers the high-water mark; reuse stays below it. */
    CHECK(su_ents_high_water(e) == 6u && su_ents_alive_count(e) == 0u);
    CHECK_ST(su_ents_alloc(e, 0u, &h[0]), SU_OK);
    CHECK(SU_ENTITY_SLOT(h[0]) == 0u && su_ents_high_water(e) == 6u);
    d = dump_of(e, &n);
    CHECK(n == DUMP_HEADER + DUMP_REC * 6u);
    free(d);
    check_round_trip(e);
    su_ents_destroy(e);
}

static void test_stale_handles(void) {
    SuEnts *e = make_table(1024u, 0u, NULL);
    SuEntity h0 = SU_ENTITY_INVALID;
    SuEntity h1 = SU_ENTITY_INVALID;
    SuEntity dead7 = SU_ENTITY_MAKE(7u, 1u);
    uint32_t ext = 0;
    size_t before_n = 0;
    uint8_t *before;
    size_t after_n = 0;
    uint8_t *after;
    if (e == NULL) {
        return;
    }
    CHECK_ST(su_ents_alloc(e, 5u, &h0), SU_OK);
    CHECK_ST(su_ents_alloc(e, 6u, &h1), SU_OK);
    CHECK_ST(su_ents_despawn(e, h0), SU_OK);
    CHECK_ST(su_ents_alloc(e, 7u, &h1), SU_OK); /* slot 0 again, generation 2 */
    CHECK(SU_ENTITY_SLOT(h0) == SU_ENTITY_SLOT(h1) && SU_ENTITY_GEN(h1) == 2u);

    CHECK_ST(su_ents_validate(e, h0), SU_E_STALE_HANDLE);         /* reuse: old generation */
    CHECK_ST(su_ents_validate(e, h1), SU_OK);
    CHECK_ST(su_ents_validate(e, SU_ENTITY_INVALID), SU_E_STALE_HANDLE); /* zero */
    CHECK_ST(su_ents_validate(e, SU_ENTITY_MAKE(0u, 0u)), SU_E_STALE_HANDLE); /* generation 0, live slot */
    CHECK_ST(su_ents_validate(e, SU_ENTITY_MAKE(5u, 0u)), SU_E_STALE_HANDLE); /* generation 0, dead slot */
    CHECK_ST(su_ents_validate(e, SU_ENTITY_MAKE(1024u, 1u)), SU_E_STALE_HANDLE); /* out of range */
    CHECK_ST(su_ents_validate(e, SU_ENTITY_MAKE(0xFFFFFFFFu, 2u)), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_validate(e, dead7), SU_E_STALE_HANDLE);      /* dead, never allocated */
    CHECK_ST(su_ents_validate(e, SU_ENTITY_MAKE(0u, 1u)), SU_E_STALE_HANDLE); /* wrong gen, alive */
    CHECK_ST(su_ents_validate(e, SU_ENTITY_MAKE(0u, 3u)), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_validate(NULL, h1), SU_E_STALE_HANDLE);

    /* Stale mutations change nothing. */
    before = dump_of(e, &before_n);
    CHECK_ST(su_ents_despawn(e, h0), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_despawn(e, SU_ENTITY_INVALID), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_despawn(e, SU_ENTITY_MAKE(0u, 0u)), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_despawn(e, dead7), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_set_external(e, h0, 9u), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_get_external(e, h0, &ext), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_get_external(e, SU_ENTITY_MAKE(0u, 0u), &ext), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_handle_for_slot(e, 7u, &h0), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_handle_for_slot(e, 5000u, &h0), SU_E_STALE_HANDLE);
    after = dump_of(e, &after_n);
    CHECK(before_n == after_n && memcmp(before, after, before_n) == 0);
    free(before);
    free(after);

    CHECK_ST(su_ents_get_external(e, h1, &ext), SU_OK);
    CHECK(ext == 7u);
    CHECK_ST(su_ents_set_external(e, h1, 77u), SU_OK);
    CHECK_ST(su_ents_get_external(e, h1, &ext), SU_OK);
    CHECK(ext == 77u);
    CHECK_ST(su_ents_despawn(e, h1), SU_OK);
    CHECK_ST(su_ents_validate(e, h1), SU_E_STALE_HANDLE);
    su_ents_destroy(e);
}

static void test_generation_exhaustion(void) {
    SuEnts *e = make_table(1024u, 0u, NULL);
    SuEntity h = SU_ENTITY_INVALID;
    SuEntity got = SU_ENTITY_INVALID;
    SuEntity hh;
    uint32_t i;
    uint32_t g = 0;
    if (e == NULL) {
        return;
    }
    /* Slot 0 must have been allocated before the seam can touch it (below high-water). */
    CHECK_ST(su_ents_alloc(e, 0u, &h), SU_OK);
    CHECK_ST(su_ents_fault_set_generation(e, 0u, GEN_MAX_LIVE), SU_OK);
    CHECK_ST(su_ents_handle_for_slot(e, 0u, &h), SU_OK);
    CHECK(h == SU_ENTITY_MAKE(0u, GEN_MAX_LIVE));
    CHECK_ST(su_ents_despawn(e, h), SU_OK); /* 0xFFFFFFFE -> 0xFFFFFFFF: retire */
    CHECK(su_ents_slot_retired(e, 0u) == 1);
    CHECK(su_ents_slot_alive(e, 0u) == 0);
    CHECK_ST(su_ents_slot_generation(e, 0u, &g), SU_OK);
    CHECK(g == GEN_RETIRED);
    CHECK_ST(su_ents_validate(e, h), SU_E_STALE_HANDLE);
    CHECK_ST(su_ents_despawn(e, h), SU_E_STALE_HANDLE);
    CHECK(su_ents_retired_count(e) == 1u);
    CHECK(su_ents_free_count(e) == 1023u);

    /* Retired slot is never returned; the rest of the table fills in ascending order. */
    for (i = 0; i < 1023u; i++) {
        CHECK_ST(su_ents_alloc(e, 0u, &got), SU_OK);
        CHECK(SU_ENTITY_SLOT(got) == i + 1u);
    }
    CHECK_ST(su_ents_alloc(e, 0u, &got), SU_E_RESOURCE_LIMIT);
    CHECK(got == SU_ENTITY_INVALID);
    CHECK(su_ents_alive_count(e) == 1023u && su_ents_retired_count(e) == 1u &&
          su_ents_free_count(e) == 0u);
    CHECK(su_ents_alive_count(e) + su_ents_retired_count(e) + su_ents_free_count(e) == 1024u);

    /* Retirement is permanent: the seam refuses to revive or fabricate values. */
    CHECK_ST(su_ents_fault_set_generation(e, 0u, 1u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_fault_set_generation(e, 0u, GEN_RETIRED), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_fault_set_generation(e, 1u, 0u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_fault_set_generation(e, 1u, GEN_RETIRED), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_fault_set_generation(e, 1024u, 1u), SU_E_INVALID_ARGUMENT);

    /* Despawning everything: the retired slot stays out of the free count and allocation. */
    for (i = 1; i < 1024u; i++) {
        CHECK_ST(su_ents_handle_for_slot(e, i, &hh), SU_OK);
        CHECK_ST(su_ents_despawn(e, hh), SU_OK);
    }
    CHECK(su_ents_alive_count(e) == 0u && su_ents_retired_count(e) == 1u && su_ents_free_count(e) == 1023u);
    CHECK_ST(su_ents_alloc(e, 0u, &got), SU_OK);
    CHECK(SU_ENTITY_SLOT(got) == 1u && SU_ENTITY_GEN(got) == 2u);
    check_round_trip(e);
    su_ents_destroy(e);

    /* Retirement with a fresh table of one entity. */
    e = make_table(1024u, 1u, NULL);
    if (e != NULL) {
        SuEntity live = SU_ENTITY_INVALID;
        CHECK_ST(su_ents_fault_set_generation(e, 0u, GEN_MAX_LIVE), SU_OK);
        CHECK_ST(su_ents_handle_for_slot(e, 0u, &live), SU_OK);
        CHECK(live == SU_ENTITY_MAKE(0u, GEN_MAX_LIVE));
        CHECK_ST(su_ents_validate(e, SU_ENTITY_MAKE(0u, 1u)), SU_E_STALE_HANDLE);
        CHECK_ST(su_ents_despawn(e, live), SU_OK);
        CHECK(su_ents_retired_count(e) == 1u && su_ents_alive_count(e) == 0u);
        su_ents_destroy(e);
    }
}

static void test_capacity_exhaustion(void) {
    SuEnts *e = make_table(1024u, 1024u, NULL);
    SuEntity got = SU_ENTITY_INVALID;
    size_t n1 = 0;
    size_t n2 = 0;
    uint8_t *b1;
    uint8_t *b2;
    uint32_t i;
    if (e == NULL) {
        return;
    }
    CHECK(su_ents_alive_count(e) == 1024u && su_ents_free_count(e) == 0u);
    b1 = dump_of(e, &n1);
    CHECK_ST(su_ents_alloc(e, 1u, &got), SU_E_RESOURCE_LIMIT);
    CHECK(got == SU_ENTITY_INVALID);
    b2 = dump_of(e, &n2);
    CHECK(n1 == n2 && memcmp(b1, b2, n1) == 0);
    free(b1);
    free(b2);
    for (i = 0; i < 1024u; i++) {
        SuEntity h;
        CHECK_ST(su_ents_handle_for_slot(e, i, &h), SU_OK);
        CHECK_ST(su_ents_despawn(e, h), SU_OK);
    }
    CHECK(su_ents_alive_count(e) == 0u && su_ents_free_count(e) == 1024u);
    CHECK_ST(su_ents_alloc(e, 0u, &got), SU_OK);
    CHECK(SU_ENTITY_SLOT(got) == 0u && SU_ENTITY_GEN(got) == 2u);
    su_ents_destroy(e);
}

/* Churn: >= 100,000 operations against the reference model. Capacity 3500 gives a short last chunk.
 * Full comparison after every operation for the first 20,000, then every 97th. */
static void test_churn_against_model(void) {
    enum { CAP = 3500, INIT = 100, OPS = 120000, GRAVE = 64 };
    SuEnts *e;
    Model m;
    Rng rng = {UINT64_C(0x1234ABCD5678EF01)};
    SuEntity grave[GRAVE];
    uint32_t grave_n = 0;
    uint32_t grave_pos = 0;
    uint32_t op;
    uint32_t checks_done = 0;
    uint32_t spawns_ok = 0;
    uint32_t spawns_full = 0;
    uint32_t despawns = 0;
    uint32_t probes = 0;
    uint32_t i;
    int bad_total = 0;

    e = make_table(CAP, INIT, NULL);
    if (e == NULL) {
        return;
    }
    m = model_new(CAP);
    for (i = 0; i < INIT; i++) {
        m.s[i].alive = 1;
    }
    m.alive_n = INIT;
    /* Fault-inject near the generation limit on two live slots so churn also retires slots. */
    CHECK_ST(su_ents_fault_set_generation(e, 50u, GEN_MAX_LIVE - 2u), SU_OK);
    CHECK_ST(su_ents_fault_set_generation(e, 51u, GEN_MAX_LIVE - 1u), SU_OK);
    m.s[50].gen = GEN_MAX_LIVE - 2u;
    m.s[51].gen = GEN_MAX_LIVE - 1u;
    CHECK(model_compare(&m, e) == 0);

    for (op = 1; op <= OPS; op++) {
        uint32_t r = rng_below(&rng, 100u);
        if (m.alive_n > CAP * 3u / 4u && r < 50u) {
            r = 60u; /* keep pressure on, but not permanently full */
        }
        if (r < 50u) {
            uint32_t ext = rng_below(&rng, 1000u);
            uint32_t slot = 0;
            int exp_ok = model_alloc(&m, ext, &slot);
            SuEntity h = SU_ENTITY_INVALID;
            uint32_t st = su_ents_alloc(e, ext, &h);
            if (exp_ok) {
                CHECK(st == SU_OK);
                CHECK(SU_ENTITY_SLOT(h) == slot);
                CHECK(SU_ENTITY_GEN(h) == m.s[slot].gen);
                spawns_ok++;
            } else {
                CHECK(st == SU_E_RESOURCE_LIMIT);
                CHECK(h == SU_ENTITY_INVALID);
                spawns_full++;
            }
        } else if (r < 95u) {
            uint32_t start = rng_below(&rng, CAP);
            uint32_t slot = CAP;
            uint32_t k;
            for (k = 0; k < CAP; k++) {
                uint32_t c = (start + k) % CAP;
                if (m.s[c].alive) {
                    slot = c;
                    break;
                }
            }
            if (slot != CAP) {
                SuEntity h = SU_ENTITY_INVALID;
                CHECK_ST(su_ents_handle_for_slot(e, slot, &h), SU_OK);
                CHECK_ST(su_ents_despawn(e, h), SU_OK);
                model_despawn(&m, slot);
                if (grave_n < GRAVE) {
                    grave[grave_n++] = h;
                } else {
                    grave[grave_pos] = h;
                    grave_pos = (grave_pos + 1u) % GRAVE;
                }
                despawns++;
            }
        } else {
            /* Stale probes: every previously despawned handle must stay stale. */
            if (grave_n > 0u) {
                uint32_t j = rng_below(&rng, grave_n);
                uint32_t ext = 0;
                CHECK_ST(su_ents_validate(e, grave[j]), SU_E_STALE_HANDLE);
                CHECK_ST(su_ents_despawn(e, grave[j]), SU_E_STALE_HANDLE);
                CHECK_ST(su_ents_get_external(e, grave[j], &ext), SU_E_STALE_HANDLE);
                probes++;
            }
            CHECK_ST(su_ents_validate(e, SU_ENTITY_INVALID), SU_E_STALE_HANDLE);
            CHECK_ST(su_ents_validate(e, SU_ENTITY_MAKE(CAP, 1u)), SU_E_STALE_HANDLE);
        }

        if (op <= 20000u || op % 97u == 0u) {
            int bad = model_compare(&m, e);
            checks_done++;
            if (bad != 0) {
                bad_total += bad;
                fprintf(stderr, "churn mismatch at op %u (%d)\n", op, bad);
                break;
            }
        }
        if (op % 10000u == 0u) {
            check_round_trip(e);
        }
    }
    CHECK(bad_total == 0);
    CHECK(model_compare(&m, e) == 0);
    CHECK(spawns_ok > 10000u && despawns > 10000u && probes > 1000u);
    CHECK(m.retired_n == su_ents_retired_count(e));
    printf("churn: ops=%u checks=%u spawn_ok=%u spawn_full=%u despawn=%u probes=%u retired=%u alive=%u "
           "high_water=%u\n",
           (unsigned)OPS, (unsigned)checks_done, (unsigned)spawns_ok, (unsigned)spawns_full,
           (unsigned)despawns, (unsigned)probes, (unsigned)su_ents_retired_count(e),
           (unsigned)su_ents_alive_count(e), (unsigned)su_ents_high_water(e));
    free(m.s);
    su_ents_destroy(e);
}

static void test_iteration(void) {
    SuEnts *e = make_table(3000u, 0u, NULL); /* chunks: 1024, 1024, 952 (short last chunk) */
    uint32_t i;
    uint32_t cur = 0;
    uint32_t seen = 0;
    uint32_t slot = 0;
    uint32_t expect_seen = 0;
    const uint64_t *words = NULL;
    uint32_t nw = 0;
    if (e == NULL) {
        return;
    }
    CHECK(su_ents_chunk_count(e) == 3u);
    for (i = 0; i < 2500u; i++) {
        SuEntity h;
        CHECK_ST(su_ents_alloc(e, i, &h), SU_OK);
    }
    for (i = 1; i < 2500u; i += 3u) {
        SuEntity h;
        CHECK_ST(su_ents_handle_for_slot(e, i, &h), SU_OK);
        CHECK_ST(su_ents_despawn(e, h), SU_OK);
    }
    /* Alive slots: i < 2500 with i % 3 != 1. */
    for (i = 0; i < 2500u; i++) {
        if (i % 3u != 1u) {
            expect_seen++;
        }
    }
    CHECK(su_ents_alive_count(e) == expect_seen);
    CHECK(su_ents_high_water(e) == 2500u);
    for (;;) {
        uint32_t want;
        if (!su_ents_next_alive(e, cur, &slot)) {
            break;
        }
        for (want = cur; want < 3000u; want++) {
            if (su_ents_slot_alive(e, want)) {
                break;
            }
        }
        CHECK(slot == want);
        seen++;
        cur = slot + 1u;
    }
    CHECK(seen == expect_seen);
    CHECK(su_ents_next_alive(e, 2500u, &slot) == 0);
    CHECK(su_ents_next_alive(e, 3000u, &slot) == 0);
    CHECK(su_ents_next_alive(e, 2499u, &slot) == 1 && slot == 2499u); /* 2499 % 3 == 0 */
    /* Chunk bitmap access: full chunk has 16 words, short last chunk has ceil(952/64) = 15. */
    CHECK_ST(su_ents_chunk_alive_bits(e, 2u, &words, &nw), SU_OK);
    CHECK(nw == 15u);
    CHECK(words != NULL && (words[0] & 1u) == 1u); /* slot 2048: 2048 % 3 == 2, alive */
    CHECK(words != NULL && ((words[0] >> 1) & 1u) == 1u); /* slot 2049: 2049 % 3 == 0, alive */
    CHECK_ST(su_ents_chunk_alive_bits(e, 0u, &words, &nw), SU_OK);
    CHECK(nw == 16u);
    CHECK_ST(su_ents_chunk_alive_bits(e, 3u, &words, &nw), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_chunk_alive_bits(e, 0u, NULL, &nw), SU_E_INVALID_ARGUMENT);
    su_ents_destroy(e);
}

/* Builds the table used by the dump tests: capacity 2048, slots 0..9 allocated (hw 10), slot 3
 * retired, slot 5 freed (gen 2), external ids set. */
static SuEnts *make_dump_fixture(void) {
    SuEnts *e = make_table(2048u, 10u, NULL);
    SuEntity h = SU_ENTITY_INVALID;
    uint32_t i;
    if (e == NULL) {
        return NULL;
    }
    for (i = 0; i < 10u; i++) {
        CHECK_ST(su_ents_handle_for_slot(e, i, &h), SU_OK);
        CHECK_ST(su_ents_set_external(e, h, 100u + i), SU_OK);
    }
    CHECK_ST(su_ents_fault_set_generation(e, 3u, GEN_MAX_LIVE), SU_OK);
    CHECK_ST(su_ents_handle_for_slot(e, 3u, &h), SU_OK);
    CHECK_ST(su_ents_despawn(e, h), SU_OK);
    CHECK_ST(su_ents_handle_for_slot(e, 5u, &h), SU_OK);
    CHECK_ST(su_ents_despawn(e, h), SU_OK);
    CHECK(su_ents_retired_count(e) == 1u && su_ents_alive_count(e) == 8u);
    return e;
}

static void expect_parse(const char *label, const uint8_t *buf, size_t len, uint32_t want) {
    SuEnts *p = NULL;
    uint32_t st = su_ents_canonical_parse(buf, len, &p);
    g_checks++;
    if (st != want || (want == SU_OK) != (p != NULL)) {
        g_failures++;
        fprintf(stderr, "corruption '%s' -> %s (out %s), want %s\n", label, su_status_name(st),
                p != NULL ? "non-null" : "null", su_status_name(want));
    }
    su_ents_destroy(p);
}

static void test_dump_round_trip_and_corruption(void) {
    SuEnts *e = make_dump_fixture();
    size_t n = 0;
    uint8_t *base;
    uint8_t *buf;
    SuEnts *p = NULL;
    if (e == NULL) {
        return;
    }
    check_round_trip(e);
    base = dump_of(e, &n);
    CHECK(n == DUMP_HEADER + DUMP_REC * 10u);

    /* Restored table allocates the same next slot as the original. */
    CHECK_ST(su_ents_canonical_parse(base, n, &p), SU_OK);
    if (p != NULL) {
        SuEntity a1 = SU_ENTITY_INVALID;
        SuEntity a2 = SU_ENTITY_INVALID;
        CHECK_ST(su_ents_alloc(e, 0u, &a1), SU_OK);
        CHECK_ST(su_ents_alloc(p, 0u, &a2), SU_OK);
        CHECK(a1 == a2);
        su_ents_destroy(p);
    }

    /* Zero-size table is a valid dump. */
    {
        SuEnts *z = make_table(0u, 0u, NULL);
        size_t zn = 0;
        uint8_t *zb;
        if (z != NULL) {
            zb = dump_of(z, &zn);
            CHECK(zn == DUMP_HEADER);
            expect_parse("empty table", zb, zn, SU_OK);
            free(zb);
            su_ents_destroy(z);
        }
    }

    buf = malloc(n + 16u);
    if (buf == NULL) {
        free(base);
        su_ents_destroy(e);
        return;
    }

#define CORRUPT(label_, want_, len_, mutate_)          \
    do {                                               \
        memcpy(buf, base, n);                          \
        mutate_;                                       \
        expect_parse(label_, buf, (len_), (want_));    \
    } while (0)

    CORRUPT("truncated", SU_E_PARSE, n - 1u, (void)0);
    CORRUPT("empty", SU_E_PARSE, 0u, (void)0);
    CORRUPT("header only", SU_E_PARSE, DUMP_HEADER - 1u, (void)0);
    CORRUPT("trailing byte", SU_E_PARSE, n + 1u, buf[n] = 0u);
    CORRUPT("bad magic", SU_E_PARSE, n, buf[0] = 'X');
    CORRUPT("bad magic terminator", SU_E_PARSE, n, buf[8] = 1u);
    CORRUPT("bad encoding version", SU_E_PARSE, n, buf[9] = 2u);
    CORRUPT("policy length", SU_E_PARSE, n, buf[13] = 10u);
    CORRUPT("policy name", SU_E_PARSE, n, buf[17] = 'M');
    CORRUPT("capacity below high-water", SU_E_PARSE, n, (memset(buf + OFF_CAP, 0, 8), buf[OFF_CAP] = 5u));
    CORRUPT("capacity oversize (no allocation)", SU_E_RESOURCE_LIMIT, n,
            (buf[OFF_CAP] = 0x41u, buf[OFF_CAP + 1] = 0x42u, buf[OFF_CAP + 2] = 0x0Fu,
             buf[OFF_CAP + 3] = 0u, buf[OFF_CAP + 4] = 0u));
    CORRUPT("high-water above capacity", SU_E_PARSE, n, buf[OFF_HW + 1] = 0x10u);
    CORRUPT("alive_count claim", SU_E_PARSE, n, buf[OFF_ALIVE] = (uint8_t)(buf[OFF_ALIVE] + 1u));
    CORRUPT("retired_count claim", SU_E_PARSE, n, buf[OFF_RET] = 0u);
    CORRUPT("alive byte not 0/1", SU_E_PARSE, n, buf[REC(0) + 8] = 2u);
    CORRUPT("alive slot generation 0", SU_E_PARSE, n, memset(buf + REC(0), 0, 4));
    CORRUPT("alive slot generation 0xFFFFFFFF", SU_E_PARSE, n, memset(buf + REC(0), 0xFF, 4));
    CORRUPT("retired slot made alive", SU_E_PARSE, n, buf[REC(3) + 8] = 1u);
    CORRUPT("free slot generation 0", SU_E_PARSE, n, memset(buf + REC(5), 0, 4));
    CORRUPT("free slot generation 0xFFFFFFFF unclaimed", SU_E_PARSE, n, memset(buf + REC(5), 0xFF, 4));
    CORRUPT("free slot marked alive", SU_E_PARSE, n, buf[REC(5) + 8] = 1u);
    CORRUPT("dead slot nonzero external id", SU_E_PARSE, n, buf[REC(5) + 4] = 7u);
    CORRUPT("retired slot nonzero external id", SU_E_PARSE, n, buf[REC(3) + 4] = 1u);
    CORRUPT("alive slot cleared without count fix", SU_E_PARSE, n, buf[REC(9) + 8] = 0u);
#undef CORRUPT

    /* High-water must be exact: a trailing never-allocated slot (dead, generation 1) is rejected. */
    {
        size_t n2 = DUMP_HEADER + DUMP_REC * 12u;
        uint8_t *big = malloc(n2);
        if (big != NULL) {
            memcpy(big, base, n);
            set_u64_le(big + OFF_HW, 12u);
            memset(big + REC(10), 0, DUMP_REC * 2u);
            set_u32_le(big + REC(10), SU_GEN_FIRST);
            set_u32_le(big + REC(11), SU_GEN_FIRST);
            expect_parse("high-water names never-allocated slot", big, n2, SU_E_PARSE);
            free(big);
        }
    }
    free(buf);
    free(base);
    su_ents_destroy(e);

    /* Null inputs and writer contract. */
    {
        SuEnts *q = NULL;
        CHECK_ST(su_ents_canonical_parse(NULL, 0u, &q), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_ents_canonical_parse(NULL, 0u, NULL), SU_E_INVALID_ARGUMENT);
    }
}

static void test_copy_and_discard(void) {
    SuEnts *committed = make_table(2048u, 20u, NULL);
    SuEnts *working = NULL;
    SuEnts *other = make_table(1024u, 0u, NULL);
    size_t n0 = 0;
    size_t n1 = 0;
    uint8_t *d0;
    uint8_t *d1;
    SuEntity h = SU_ENTITY_INVALID;
    int eq = 0;
    uint32_t i;
    if (committed == NULL || other == NULL) {
        return;
    }
    d0 = dump_of(committed, &n0);
    CHECK_ST(su_ents_clone(committed, &working), SU_OK);
    if (working == NULL) {
        return;
    }
    /* A tick works on the working root: spawn, despawn, retire, set external ids. */
    for (i = 0; i < 40u; i++) {
        CHECK_ST(su_ents_alloc(working, 900u + i, &h), SU_OK);
    }
    CHECK_ST(su_ents_handle_for_slot(working, 4u, &h), SU_OK);
    CHECK_ST(su_ents_despawn(working, h), SU_OK);
    CHECK_ST(su_ents_fault_set_generation(working, 6u, GEN_MAX_LIVE), SU_OK);
    CHECK_ST(su_ents_handle_for_slot(working, 6u, &h), SU_OK);
    CHECK_ST(su_ents_despawn(working, h), SU_OK);
    CHECK(su_ents_retired_count(working) == 1u);

    /* Committed root is untouched by working-root edits (no aliasing). */
    d1 = dump_of(committed, &n1);
    CHECK(n0 == n1 && memcmp(d0, d1, n0) == 0);
    free(d1);

    /* Abort: discard the working copy by copying the committed root back. */
    CHECK_ST(su_ents_copy(working, committed), SU_OK);
    CHECK_ST(su_ents_state_equal(working, committed, &eq), SU_OK);
    CHECK(eq == 1);
    d1 = dump_of(working, &n1);
    CHECK(n0 == n1 && memcmp(d0, d1, n0) == 0);
    free(d1);

    /* Edits on the copy after copy-into do not reach the source. */
    CHECK_ST(su_ents_alloc(working, 1u, &h), SU_OK);
    d1 = dump_of(committed, &n1);
    CHECK(n0 == n1 && memcmp(d0, d1, n0) == 0);
    free(d1);
    CHECK_ST(su_ents_state_equal(working, committed, &eq), SU_OK);
    CHECK(eq == 0);

    /* Capacity mismatch and NULL: rejected, nothing changes. */
    CHECK_ST(su_ents_copy(other, committed), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_copy(NULL, committed), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_copy(working, NULL), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_ents_copy(working, working), SU_OK);
    CHECK_ST(su_ents_clone(NULL, &working), SU_E_INVALID_ARGUMENT);

    su_ents_destroy(working);
    su_ents_destroy(committed);
    su_ents_destroy(other);
    free(d0);
}

/* Same operation sequence -> byte-identical canonical dumps; a different sequence differs. */
static void run_ops(SuEnts *e, uint64_t seed, uint32_t ops, uint8_t **dump, size_t *len) {
    Rng r = {seed};
    uint32_t k;
    for (k = 0; k < ops; k++) {
        if (rng_below(&r, 100u) < 55u) {
            SuEntity h;
            (void)su_ents_alloc(e, rng_below(&r, 16u), &h);
        } else {
            uint32_t start = rng_below(&r, su_ents_capacity(e));
            uint32_t s = 0;
            if (!su_ents_next_alive(e, start, &s)) {
                (void)su_ents_next_alive(e, 0u, &s);
            }
            if (su_ents_slot_alive(e, s)) {
                SuEntity h;
                CHECK_ST(su_ents_handle_for_slot(e, s, &h), SU_OK);
                CHECK_ST(su_ents_despawn(e, h), SU_OK);
            }
        }
    }
    if (dump != NULL && len != NULL) {
        *dump = dump_of(e, len);
    }
}

static void test_determinism(void) {
    SuEnts *a = make_table(4096u, 5u, NULL);
    SuEnts *b = make_table(4096u, 5u, NULL);
    SuEnts *c = make_table(4096u, 5u, NULL);
    uint8_t *da = NULL;
    uint8_t *db = NULL;
    uint8_t *dc = NULL;
    size_t na = 0;
    size_t nb = 0;
    size_t nc = 0;
    if (a == NULL || b == NULL || c == NULL) {
        return;
    }
    run_ops(a, UINT64_C(77), 30000u, &da, &na);
    run_ops(b, UINT64_C(77), 30000u, &db, &nb);
    run_ops(c, UINT64_C(78), 30000u, &dc, &nc);
    CHECK(na == nb && memcmp(da, db, na) == 0);
    CHECK(!(na == nc && memcmp(da, dc, na) == 0)); /* different sequence -> different dump */
    free(da);
    free(db);
    free(dc);
    su_ents_destroy(a);
    su_ents_destroy(b);
    su_ents_destroy(c);
}

int main(void) {
    test_create_rules();
    test_lowest_slot_reuse();
    test_high_water();
    test_stale_handles();
    test_generation_exhaustion();
    test_capacity_exhaustion();
    test_churn_against_model();
    test_iteration();
    test_dump_round_trip_and_corruption();
    test_copy_and_discard();
    test_determinism();
    if (g_failures != 0) {
        fprintf(stderr, "test_entities: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_entities: %d checks passed\n", g_checks);
    return 0;
}
