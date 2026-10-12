/* M3-08a: exact per-root accounting (su_world_memory, su_ents_memory_bytes, su_cmdq_memory,
 * su_evlog_memory_bytes).
 *
 * The world, entity-table and command-queue sources are compiled with tests/world/acct_seam.h
 * force-included (see tests/CMakeLists or the root CMakeLists target test_world_acct), so every
 * malloc, calloc and free they perform goes through the counting seam below. The seam's live-byte
 * count is the real heap usage of those sources. Reported categories must match sizes computed from
 * field and row counts, and the reported total must equal the seam's live delta exactly. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/entities.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "world/su_world.h"
#include "world/entities/su_cmdq.h"
#include "world/entities/su_ents.h"

/* ---- counting seam (the definitions behind acct_seam.h) ----------------- */

#define SEAM_HDR 16u /* header keeps the returned block aligned and records the requested size */

static uint64_t g_live_bytes;
static uint64_t g_live_blocks;
static uint64_t g_total_allocs;

static void *seam_wrap(void *raw, size_t n) {
    if (raw == NULL) {
        return NULL;
    }
    *(size_t *)raw = n;
    g_live_bytes += n;
    g_live_blocks++;
    g_total_allocs++;
    return (unsigned char *)raw + SEAM_HDR;
}

void *su_seam_malloc(size_t n) {
    return seam_wrap(malloc(n + SEAM_HDR), n);
}

void *su_seam_calloc(size_t n, size_t size) {
    size_t total;
    if (size != 0u && n > (SIZE_MAX - SEAM_HDR) / size) {
        return NULL;
    }
    total = n * size;
    return seam_wrap(calloc(1, total + SEAM_HDR), total);
}

void su_seam_free(void *p) {
    unsigned char *raw;
    if (p == NULL) {
        return;
    }
    raw = (unsigned char *)p - SEAM_HDR;
    g_live_bytes -= *(size_t *)raw;
    g_live_blocks--;
    free(raw);
}

/* ---- harness ------------------------------------------------------------ */

static int g_fail;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_fail++;                                                            \
        }                                                                        \
    } while (0)

#define CHECK_EQ_U64(a, b)                                                       \
    do {                                                                         \
        uint64_t va_ = (uint64_t)(a);                                            \
        uint64_t vb_ = (uint64_t)(b);                                            \
        if (va_ != vb_) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__,   \
                    __LINE__, #a, #b, (unsigned long long)va_,                   \
                    (unsigned long long)vb_);                                    \
            g_fail++;                                                            \
        }                                                                        \
    } while (0)

/* ---- world fixtures ----------------------------------------------------- */

typedef struct Spec {
    const char *schema;
    uint64_t rows;          /* initial alive rows */
    uint64_t max_entities;  /* requested capacity (0 means rows) */
    uint32_t n_fields;
    uint32_t access[8];     /* SU_ACCESS_READ, or READ | WRITE */
} Spec;

static uint64_t spec_capacity(const Spec *s) {
    return s->max_entities > s->rows ? s->max_entities : s->rows;
}

static uint32_t spec_writable(const Spec *s) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < s->n_fields; i++) {
        if ((s->access[i] & SU_ACCESS_WRITE) != 0u) {
            n++;
        }
    }
    return n;
}

/* Builds a world from the spec (with optional schema extension). Returns the create status and the
 * seam's live-byte delta across the call. */
static uint32_t make_world(const Spec *s, const SuWorldSchemaExt *ext, SuWorld **out,
                           uint64_t *delta_out) {
    SuWorldFieldDesc fields[8];
    static char names[8][32];
    uint32_t ids[64];
    SuWorldDesc d;
    uint64_t before = g_live_bytes;
    uint32_t st;
    uint32_t i;
    for (i = 0; i < s->n_fields; i++) {
        snprintf(names[i], sizeof names[i], "field_%u_name", (unsigned)i);
        fields[i].id = 10u + i;
        fields[i].type = SU_TYPE_U32;
        fields[i].access = s->access[i];
        fields[i].name = names[i];
    }
    for (i = 0; i < s->rows; i++) {
        ids[i] = i + 1u;
    }
    memset(&d, 0, sizeof d);
    d.schema_id = s->schema;
    d.schema_version = 1;
    d.layout_generation = 1;
    d.row_count = s->rows;
    d.entity_ids = s->rows > 0u ? ids : NULL;
    d.fields = fields;
    d.field_count = s->n_fields;
    *out = NULL;
    st = su_world_create_ext(&d, s->max_entities, ext, out);
    *delta_out = g_live_bytes - before;
    return st;
}

static const Spec SPECS[] = {
    {"w.empty", 0, 0, 0, {0}},
    {"w.one", 1, 0, 1, {SU_ACCESS_READ}},
    {"w.mixed63", 5, 63, 3,
     {SU_ACCESS_READ, SU_ACCESS_READ | SU_ACCESS_WRITE, SU_ACCESS_WRITE}},
    {"w.full64", 64, 64, 4,
     {SU_ACCESS_READ, SU_ACCESS_READ | SU_ACCESS_WRITE, SU_ACCESS_WRITE, SU_ACCESS_READ}},
    {"w.two_chunks", 3, 1025, 6,
     {SU_ACCESS_READ, SU_ACCESS_READ | SU_ACCESS_WRITE, SU_ACCESS_WRITE, SU_ACCESS_READ,
      SU_ACCESS_READ | SU_ACCESS_WRITE, SU_ACCESS_READ}},
    {"w.exact1024", 2, 1024, 2, {SU_ACCESS_READ | SU_ACCESS_WRITE, SU_ACCESS_READ}},
    {"w.noread_rows", 0, 65, 2, {SU_ACCESS_WRITE, SU_ACCESS_READ | SU_ACCESS_WRITE}},
    /* zero capacity with fields: columns still hold one element each */
    {"w.zero_cap", 0, 0, 2, {SU_ACCESS_READ, SU_ACCESS_READ | SU_ACCESS_WRITE}},
};

#define N_SPECS ((int)(sizeof SPECS / sizeof SPECS[0]))

/* Entity table bytes for one table of the given capacity, from the seam and from the report. */
static void ents_seam(uint64_t cap, uint64_t *seam, uint64_t *reported) {
    SuEntsDesc d;
    SuEnts *e = NULL;
    uint64_t before = g_live_bytes;
    uint32_t st;
    memset(&d, 0, sizeof d);
    d.max_entities = cap;
    d.initial_alive = 0;
    st = su_ents_create(&d, &e);
    CHECK(st == SU_OK);
    *seam = g_live_bytes - before;
    *reported = su_ents_memory_bytes(e);
    su_ents_destroy(e);
    CHECK_EQ_U64(g_live_bytes, before);
}

static void test_ents(void) {
    static const uint64_t caps[] = {0, 1, 63, 64, 65, 1023, 1024, 1025, 4097, 100000};
    size_t i;
    for (i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        uint64_t seam = 0;
        uint64_t rep = 0;
        ents_seam(caps[i], &seam, &rep);
        CHECK_EQ_U64(rep, seam);
    }
    CHECK_EQ_U64(su_ents_memory_bytes(NULL), 0u);
}

static void test_world_configs(void) {
    int k;
    for (k = 0; k < N_SPECS; k++) {
        const Spec *s = &SPECS[k];
        uint64_t cap = spec_capacity(s);
        uint64_t rows = cap > 0u ? cap : 1u; /* column length, restated from the row count */
        uint64_t delta = 0;
        uint64_t ents_bytes = 0;
        uint64_t ents_rep = 0;
        SuWorld *w = NULL;
        SuWorldMemory m;
        SuWorldMemory m2;
        uint64_t before;
        uint64_t blocks_before;
        uint32_t wr[8];
        uint32_t n = 0;
        uint32_t i;
        SuEntity h;
        SuWorldSpawnValue v;

        if (make_world(s, NULL, &w, &delta) != SU_OK) {
            fprintf(stderr, "FAIL: create %s\n", s->schema);
            g_fail++;
            continue;
        }
        CHECK_EQ_U64(su_world_row_count(w), cap);
        CHECK_EQ_U64(su_world_memory(w, &m), SU_OK);

        /* Categories from field and row counts. */
        CHECK_EQ_U64(m.columns_committed, (uint64_t)s->n_fields * rows * 4u);
        CHECK_EQ_U64(m.columns_working, (uint64_t)s->n_fields * rows * 4u);
        CHECK_EQ_U64(m.columns_staging, (uint64_t)spec_writable(s) * rows * 4u);
        CHECK_EQ_U64(m.total, m.columns_committed + m.columns_working + m.columns_staging +
                                  m.entity_table + m.metadata);
        /* Every byte the world asked the allocator for and still holds is reported. */
        CHECK_EQ_U64(m.total, delta);

        /* Entity table: committed and working tables share capacity and chunk shape. */
        ents_seam(cap, &ents_bytes, &ents_rep);
        CHECK_EQ_U64(ents_rep, ents_bytes);
        CHECK_EQ_U64(m.entity_table, 2u * ents_rep);

        /* Tick operations and control-plane spawn allocate nothing and leave the report unchanged. */
        for (i = 0; i < s->n_fields; i++) {
            if ((s->access[i] & SU_ACCESS_WRITE) != 0u) {
                wr[n++] = 10u + i;
            }
        }
        before = g_live_bytes;
        blocks_before = g_live_blocks;
        if (s->n_fields > 0u && s->rows < cap) {
            v.field_id = 10u;
            v.value = 7u;
            CHECK_EQ_U64(su_world_spawn(w, &v, 1u, 0u, &h), SU_OK);
        }
        CHECK_EQ_U64(su_world_begin_tick(w), SU_OK);
        CHECK_EQ_U64(su_world_commit_staging(w, wr, n), SU_OK);
        CHECK_EQ_U64(su_world_publish(w), SU_OK);
        CHECK_EQ_U64(su_world_memory(w, &m2), SU_OK);
        CHECK_EQ_U64(m2.total, m.total);
        CHECK_EQ_U64(m2.entity_table, m.entity_table);
        CHECK_EQ_U64(g_live_bytes, before);
        CHECK_EQ_U64(g_live_blocks, blocks_before);

        su_world_destroy(w);
    }
    CHECK_EQ_U64(su_world_memory(NULL, NULL), SU_E_INVALID_ARGUMENT);
}

/* Name bytes and extension bytes are exact: metadata moves by exactly what the schema adds. */
static void test_metadata_exact(void) {
    Spec a = {"w.meta", 2, 0, 2, {SU_ACCESS_READ, SU_ACCESS_READ | SU_ACCESS_WRITE}};
    Spec b = {"w.meta_longer_id", 2, 0, 2, {SU_ACCESS_READ, SU_ACCESS_READ | SU_ACCESS_WRITE}};
    uint32_t retired[3] = {100u, 101u, 102u};
    uint8_t lineage[2][32];
    SuWorldSchemaExt ext;
    SuWorld *wa = NULL;
    SuWorld *wb = NULL;
    SuWorld *we = NULL;
    uint64_t da = 0;
    uint64_t db = 0;
    uint64_t de = 0;
    SuWorldMemory ma;
    SuWorldMemory mb;
    SuWorldMemory me;

    memset(lineage, 7, sizeof lineage);
    memset(&ext, 0, sizeof ext);
    ext.retired_ids = retired;
    ext.retired_count = 3u;
    ext.lineage = (const uint8_t (*)[32])lineage;
    ext.lineage_count = 2u;

    CHECK_EQ_U64(make_world(&a, NULL, &wa, &da), SU_OK);
    CHECK_EQ_U64(make_world(&b, NULL, &wb, &db), SU_OK);
    CHECK_EQ_U64(make_world(&a, &ext, &we, &de), SU_OK);
    CHECK_EQ_U64(su_world_memory(wa, &ma), SU_OK);
    CHECK_EQ_U64(su_world_memory(wb, &mb), SU_OK);
    CHECK_EQ_U64(su_world_memory(we, &me), SU_OK);
    /* Schema ID is 9 bytes longer (name bytes plus NUL are counted); nothing else changes. */
    CHECK_EQ_U64(mb.metadata - ma.metadata, strlen(b.schema) - strlen(a.schema));
    CHECK_EQ_U64(mb.total - ma.total, mb.metadata - ma.metadata);
    /* Retired IDs: 3 x 4 bytes; lineage: 2 x 32 bytes. */
    CHECK_EQ_U64(me.metadata - ma.metadata, 3u * 4u + 2u * 32u);
    CHECK_EQ_U64(me.total, de);
    CHECK_EQ_U64(mb.total, db);
    CHECK_EQ_U64(ma.total, da);
    su_world_destroy(wa);
    su_world_destroy(wb);
    su_world_destroy(we);
}

static void test_successor_counts(void) {
    Spec s = {"w.succ", 4, 0, 3, {SU_ACCESS_READ, SU_ACCESS_WRITE, SU_ACCESS_READ | SU_ACCESS_WRITE}};
    SuWorldFieldDesc fields[3];
    SuWorldDesc d;
    SuWorld *w = NULL;
    SuWorld *succ = NULL;
    SuWorldMemory m;
    SuWorldMemory ms;
    uint64_t delta = 0;
    uint64_t before;
    uint32_t i;

    CHECK_EQ_U64(make_world(&s, NULL, &w, &delta), SU_OK);
    for (i = 0; i < 3u; i++) {
        fields[i].id = 10u + i;
        fields[i].type = SU_TYPE_U32;
        fields[i].access = s.access[i];
        fields[i].name = i == 0u ? "f0" : (i == 1u ? "f1" : "f2");
    }
    memset(&d, 0, sizeof d);
    d.schema_id = "w.succ";
    d.schema_version = 1;
    d.layout_generation = 2;
    d.row_count = 0;
    d.fields = fields;
    d.field_count = 3;
    before = g_live_bytes;
    CHECK_EQ_U64(su_world_create_successor(w, &d, NULL, &succ), SU_OK);
    CHECK_EQ_U64(su_world_memory(succ, &ms), SU_OK);
    /* The successor's columns are zero-length to the caller but still one element each. */
    CHECK_EQ_U64(ms.total, g_live_bytes - before);
    CHECK_EQ_U64(su_world_memory(w, &m), SU_OK);
    CHECK_EQ_U64(m.total, delta);
    su_world_destroy(succ);
    su_world_destroy(w);
}

static void test_cmdq(void) {
    SuCmdQ *q = NULL;
    SuCmdQMemory m;
    uint64_t before = g_live_bytes;
    uint32_t st;

    /* 3 systems, 2500 slots in chunks of 1024 (1024, 1024, 452), per-row structural 1 and event 2,
     * per-tick caps 2500 (structural) and 5000 (event). */
    st = su_cmdq_create(3u, 2500u, 1024u, 1u, 2u, 2500u, 5000u, &q);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        return;
    }
    CHECK_EQ_U64(su_cmdq_memory(q, &m), SU_OK);
    /* Structural: per system min(chunk*1, 2500) over chunks sums to 2500 (three systems). */
    CHECK_EQ_U64(m.structural, 3u * 2500u * sizeof(SuStructCmd));
    /* Events: per chunk min(chunk*2, 5000) = 2048, 2048, 904, which sums to 5000 per system. */
    CHECK_EQ_U64(m.events, 3u * 5000u * sizeof(SuEvent));
    /* Scratch: m_cmd, t_cmd (2500 each), rec (2500), m_evt, t_evt, fin (5000 each). */
    CHECK_EQ_U64(m.scratch, 2u * 2500u * sizeof(SuStructCmd) + 2500u * sizeof(SuEvent) +
                                3u * 5000u * sizeof(SuEvent));
    CHECK_EQ_U64(m.total, m.structural + m.events + m.scratch + m.bookkeeping);
    /* Bookkeeping (BufMeta array, offsets, cursors, heap, struct) is checked through the total. */
    CHECK_EQ_U64(m.total, g_live_bytes - before);
    su_cmdq_destroy(q);
    CHECK_EQ_U64(g_live_bytes, before);
    CHECK_EQ_U64(su_cmdq_memory(NULL, &m), SU_E_INVALID_ARGUMENT);
}

static void test_evlog(void) {
    SuEventLog *a = NULL;
    SuEventLog *b = NULL;
    uint64_t before = g_live_bytes;
    uint64_t ba;
    uint64_t bb;
    CHECK_EQ_U64(su_evlog_create(100u, 0u, &a), SU_OK);
    ba = su_evlog_memory_bytes(a);
    CHECK_EQ_U64(ba, g_live_bytes - before);
    CHECK_EQ_U64(su_evlog_create(200u, 0u, &b), SU_OK);
    bb = su_evlog_memory_bytes(b);
    /* The ring grows by exactly 100 records. */
    CHECK_EQ_U64(bb - ba, 100u * sizeof(SuEvent));
    su_evlog_destroy(a);
    su_evlog_destroy(b);
    CHECK_EQ_U64(su_evlog_memory_bytes(NULL), 0u);
    CHECK_EQ_U64(g_live_bytes, before);
}

int main(void) {
    test_ents();
    test_world_configs();
    test_metadata_exact();
    test_successor_counts();
    test_cmdq();
    test_evlog();
    if (g_live_blocks != 0u) {
        fprintf(stderr, "FAIL: %llu seam blocks still live\n", (unsigned long long)g_live_blocks);
        g_fail++;
    }
    if (g_fail != 0) {
        fprintf(stderr, "test_world_acct: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_world_acct: ok (%llu seam allocations)\n", (unsigned long long)g_total_allocs);
    return 0;
}
