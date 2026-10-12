/* M0-03 tests for the fixed-row SoA u32 world. Self-contained assert harness. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "world/su_world.h"

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

/* Fields are declared out of id order on purpose (table must sort them). */
static const SuWorldFieldDesc k_fields[] = {
    {10u, SU_TYPE_U32, SU_ACCESS_READ, "a"},
    {20u, SU_TYPE_U32, SU_ACCESS_WRITE, "b"},
    {5u, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "c"},
};
#define K_FIELD_COUNT 3u
#define ID_A 10u
#define ID_B 20u
#define ID_C 5u

/* Entity IDs 1..rows, heap allocated (never NULL, so rows==0 is valid too). */
static uint32_t *make_ids(uint64_t rows) {
    uint64_t i;
    uint32_t *ids = malloc((size_t)(rows > 0 ? rows : 1u) * sizeof *ids);
    if (ids == NULL) {
        abort();
    }
    for (i = 0; i < rows; i++) {
        ids[i] = (uint32_t)(i + 1u);
    }
    return ids;
}

static uint32_t make_world(uint64_t rows, const SuWorldFieldDesc *fields, uint32_t n,
                           uint32_t *ids, SuWorld **out) {
    SuWorldDesc d;
    memset(&d, 0, sizeof d);
    d.schema_id = "test.schema";
    d.schema_version = 1u;
    d.layout_generation = 7u;
    d.row_count = rows;
    d.entity_ids = ids;
    d.fields = fields;
    d.field_count = n;
    return su_world_create(&d, out);
}

static const SuColumnBinding *find_binding(const SuColumnBinding *b, uint32_t n, uint32_t id) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (b[i].field_id == id) {
            return &b[i];
        }
    }
    return NULL;
}

/* Byte-range overlap for two u32 columns of the given row counts. */
static int ranges_overlap(const void *p, uint64_t np, const void *q, uint64_t nq) {
    uintptr_t pa = (uintptr_t)p;
    uintptr_t qa = (uintptr_t)q;
    uintptr_t pe = pa + (uintptr_t)np * 4u;
    uintptr_t qe = qa + (uintptr_t)nq * 4u;
    if (np == 0u || nq == 0u) {
        return 0;
    }
    return pa < qe && qa < pe;
}

/* Fills the committed root with a deterministic pattern. */
static void seed(SuWorld *w, uint64_t rows) {
    uint32_t *a = malloc((size_t)(rows > 0 ? rows : 1u) * 4u);
    uint32_t *c = malloc((size_t)(rows > 0 ? rows : 1u) * 4u);
    uint32_t *b = calloc((size_t)(rows > 0 ? rows : 1u), 4u);
    uint64_t r;
    for (r = 0; r < rows; r++) {
        a[r] = (uint32_t)(r + 1u);
        c[r] = (uint32_t)(100u + r);
    }
    CHECK_ST(su_world_init_committed(w, ID_A, a, rows), SU_OK);
    CHECK_ST(su_world_init_committed(w, ID_B, b, rows), SU_OK);
    CHECK_ST(su_world_init_committed(w, ID_C, c, rows), SU_OK);
    free(a);
    free(b);
    free(c);
}

static int committed_equals(const SuWorld *w, uint32_t id, const uint32_t *expect, uint64_t rows) {
    const uint32_t *data = NULL;
    uint64_t n = 0;
    if (su_world_read_committed(w, id, &data, &n) != SU_OK || n != rows) {
        return 0;
    }
    return rows == 0u || memcmp(data, expect, (size_t)rows * 4u) == 0;
}

/* ---- tests ------------------------------------------------------------ */

static void test_row_counts(void) {
    static const uint64_t counts[] = {0u, 1u, 7u, 8u, 9u, 1024u};
    size_t k;
    for (k = 0; k < sizeof counts / sizeof counts[0]; k++) {
        uint64_t rows = counts[k];
        uint32_t *ids = make_ids(rows);
        SuWorld *w = NULL;
        SuInvocation inv;
        SuOutputs out;
        uint32_t *expect_b = calloc((size_t)(rows > 0 ? rows : 1u), 4u);
        uint32_t *expect_c = calloc((size_t)(rows > 0 ? rows : 1u), 4u);
        uint32_t *expect_a = calloc((size_t)(rows > 0 ? rows : 1u), 4u);
        uint64_t r;
        uint32_t ids_b[2] = {ID_B, ID_C};
        const SuColumnBinding *ib;
        const SuColumnBinding *ob;

        CHECK_ST(make_world(rows, k_fields, K_FIELD_COUNT, ids, &w), SU_OK);
        CHECK(w != NULL);
        if (w == NULL) {
            free(ids);
            free(expect_a);
            free(expect_b);
            free(expect_c);
            continue;
        }
        CHECK(su_world_row_count(w) == rows);
        CHECK(su_world_field_count(w) == K_FIELD_COUNT);
        CHECK(su_world_layout_generation(w) == 7u);
        seed(w, rows);

        CHECK_ST(su_world_begin_tick(w), SU_OK);
        CHECK(su_world_in_tick(w));
        CHECK_ST(su_world_build_invocation(w, &inv), SU_OK);
        CHECK_ST(su_world_build_outputs(w, &out), SU_OK);
        /* Reads: a and c, ascending id -> 5 then 10. Writes: c and b -> 5 then 20. */
        CHECK(inv.binding_count == 2u);
        CHECK(inv.row_count == rows);
        CHECK(inv.layout_generation == 7u);
        CHECK(inv.bindings[0].field_id == ID_C);
        CHECK(inv.bindings[1].field_id == ID_A);
        CHECK(out.binding_count == 2u);
        CHECK(out.bindings[0].field_id == ID_C);
        CHECK(out.bindings[1].field_id == ID_B);
        CHECK(out.failed_insn == UINT32_MAX);

        ib = inv.bindings;
        ob = out.bindings;
        for (r = 0; r < rows; r++) {
            uint32_t a = ((uint32_t *)ib[1].data)[r];
            uint32_t c = ((uint32_t *)ib[0].data)[r];
            ((uint32_t *)ob[0].data)[r] = c + 1u;       /* c staging */
            ((uint32_t *)ob[1].data)[r] = a + c;        /* b staging */
            expect_a[r] = a;
            expect_b[r] = a + c;
            expect_c[r] = c + 1u;
        }
        CHECK_ST(su_world_commit_staging(w, ids_b, 2u), SU_OK);
        CHECK_ST(su_world_publish(w), SU_OK);
        CHECK(su_world_tick(w) == 1u);
        CHECK(!su_world_in_tick(w));
        CHECK(committed_equals(w, ID_A, expect_a, rows));
        CHECK(committed_equals(w, ID_B, expect_b, rows));
        CHECK(committed_equals(w, ID_C, expect_c, rows));

        su_world_destroy(w);
        free(ids);
        free(expect_a);
        free(expect_b);
        free(expect_c);
    }
}

static void test_row_limits(void) {
    uint32_t *ids = NULL;
    uint32_t *ids_many = make_ids(1u);
    SuWorld *w = NULL;
    SuWorldFieldDesc one[1] = {{1u, SU_TYPE_U32, SU_ACCESS_READ, "x"}};
    SuWorldFieldDesc *many = NULL;
    uint32_t i;

    /* Over the cap, and absurd values, fail before any allocation. */
    CHECK_ST(make_world(SU_LIMIT_ROWS + 1u, one, 1u, NULL, &w), SU_E_RESOURCE_LIMIT);
    CHECK(w == NULL);
    CHECK_ST(make_world(UINT64_MAX, one, 1u, NULL, &w), SU_E_RESOURCE_LIMIT);
    CHECK(w == NULL);
    CHECK_ST(make_world(UINT64_MAX / 4u + 1u, one, 1u, NULL, &w), SU_E_RESOURCE_LIMIT);
    CHECK(w == NULL);
    CHECK_ST(make_world(UINT64_C(1) << 40, one, 1u, NULL, &w), SU_E_RESOURCE_LIMIT);
    CHECK(w == NULL);

    /* Field count over the cap. */
    many = calloc(SU_LIMIT_FIELDS + 1u, sizeof *many);
    CHECK(many != NULL);
    if (many != NULL) {
        for (i = 0; i <= SU_LIMIT_FIELDS; i++) {
            many[i].id = i + 1u;
            many[i].type = SU_TYPE_U32;
            many[i].access = SU_ACCESS_READ;
            many[i].name = "f";
        }
        CHECK_ST(make_world(1u, many, SU_LIMIT_FIELDS + 1u, ids_many, &w), SU_E_RESOURCE_LIMIT);
        CHECK(w == NULL);
        free(many);
    }
    free(ids_many);

    /* Exactly at the row cap is accepted (one read field, 4 MB per column). */
    ids = make_ids(SU_LIMIT_ROWS);
    CHECK_ST(make_world(SU_LIMIT_ROWS, one, 1u, ids, &w), SU_OK);
    CHECK(w != NULL);
    if (w != NULL) {
        CHECK(su_world_row_count(w) == SU_LIMIT_ROWS);
        su_world_destroy(w);
        w = NULL;
    }
    free(ids);
}

static void test_invalid_descriptions(void) {
    SuWorld *w = NULL;
    uint32_t *ids = make_ids(3u);
    SuWorldFieldDesc dup_id[2] = {{4u, SU_TYPE_U32, SU_ACCESS_READ, "p"},
                                  {4u, SU_TYPE_U32, SU_ACCESS_READ, "q"}};
    SuWorldFieldDesc dup_name[2] = {{4u, SU_TYPE_U32, SU_ACCESS_READ, "p"},
                                    {5u, SU_TYPE_U32, SU_ACCESS_READ, "p"}};
    SuWorldFieldDesc bad_type[1] = {{4u, SU_TYPE_BOOL, SU_ACCESS_READ, "p"}};
    SuWorldFieldDesc no_access[1] = {{4u, SU_TYPE_U32, 0u, "p"}};
    SuWorldFieldDesc bad_access[1] = {{4u, SU_TYPE_U32, 4u, "p"}};
    SuWorldFieldDesc empty_name[1] = {{4u, SU_TYPE_U32, SU_ACCESS_READ, ""}};
    SuWorldFieldDesc ok_field[1] = {{4u, SU_TYPE_U32, SU_ACCESS_READ, "p"}};
    char long_name[SU_WORLD_NAME_MAX + 2u];
    SuWorldFieldDesc too_long[1] = {{4u, SU_TYPE_U32, SU_ACCESS_READ, long_name}};
    uint32_t zero_ids[3] = {1u, 0u, 3u};
    uint32_t dup_ids[3] = {1u, 2u, 1u};

    memset(long_name, 'n', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';

    CHECK_ST(su_world_create(NULL, &w), SU_E_INVALID_ARGUMENT);
    {
        SuWorldDesc d;
        memset(&d, 0, sizeof d);
        CHECK_ST(su_world_create(&d, NULL), SU_E_INVALID_ARGUMENT);
        d.schema_id = NULL;
        CHECK_ST(su_world_create(&d, &w), SU_E_INVALID_ARGUMENT);
        CHECK(w == NULL);
        d.schema_id = "s";
        d.field_count = 1u;
        d.fields = NULL;
        CHECK_ST(su_world_create(&d, &w), SU_E_INVALID_ARGUMENT);
        d.fields = ok_field;
        d.field_count = 0u;
        d.row_count = 2u;
        d.entity_ids = NULL;
        CHECK_ST(su_world_create(&d, &w), SU_E_INVALID_ARGUMENT);
        d.row_count = 0u;
        d.field_count = 1u;
        d.fields = ok_field;
        CHECK_ST(su_world_create(&d, &w), SU_OK); /* zero rows with NULL IDs is valid */
        CHECK(w != NULL);
        su_world_destroy(w);
        w = NULL;
    }

    CHECK_ST(make_world(3u, dup_id, 2u, ids, &w), SU_E_SCHEMA_MISMATCH);
    CHECK(w == NULL);
    CHECK_ST(make_world(3u, dup_name, 2u, ids, &w), SU_E_SCHEMA_MISMATCH);
    CHECK(w == NULL);
    CHECK_ST(make_world(3u, bad_type, 1u, ids, &w), SU_E_TYPE);
    CHECK(w == NULL);
    CHECK_ST(make_world(3u, no_access, 1u, ids, &w), SU_E_INVALID_ARGUMENT);
    CHECK_ST(make_world(3u, bad_access, 1u, ids, &w), SU_E_INVALID_ARGUMENT);
    CHECK_ST(make_world(3u, empty_name, 1u, ids, &w), SU_E_INVALID_ARGUMENT);
    CHECK_ST(make_world(3u, too_long, 1u, ids, &w), SU_E_RESOURCE_LIMIT);
    CHECK(w == NULL);
    CHECK_ST(make_world(3u, ok_field, 1u, NULL, &w), SU_E_INVALID_ARGUMENT);
    CHECK_ST(make_world(3u, ok_field, 1u, zero_ids, &w), SU_E_INVALID_ARGUMENT);
    CHECK(w == NULL);
    CHECK_ST(make_world(3u, ok_field, 1u, dup_ids, &w), SU_E_INVALID_ARGUMENT);
    CHECK(w == NULL);

    free(ids);
}

static void test_abort_restores_committed(void) {
    SuWorld *w = NULL;
    uint32_t *ids = make_ids(9u);
    uint32_t b_only[1] = {ID_B};
    uint32_t *before = NULL;
    size_t n_before = 0;
    size_t n_after = 0;
    uint8_t *snap_before = NULL;
    uint8_t *snap_after = NULL;
    SuOutputs out;
    uint32_t i;
    const uint32_t *data = NULL;
    uint64_t rows = 0;

    (void)before;
    CHECK_ST(make_world(9u, k_fields, K_FIELD_COUNT, ids, &w), SU_OK);
    seed(w, 9u);
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK_ST(su_world_publish(w), SU_OK); /* tick 1 with the seeded state */
    CHECK(su_world_tick(w) == 1u);

    /* Snapshot the committed root bit-for-bit. */
    CHECK_ST(su_world_canonical_size(w, &n_before), SU_OK);
    snap_before = malloc(n_before);
    CHECK(snap_before != NULL);
    if (snap_before == NULL) {
        su_world_destroy(w);
        free(ids);
        return;
    }
    CHECK_ST(su_world_canonical_write(w, snap_before, n_before, &n_before), SU_OK);

    /* Partial tick: stage and commit field b (out.bindings[1]) with new values,
     * then fail before field c is staged. */
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK_ST(su_world_build_outputs(w, &out), SU_OK);
    for (i = 0; i < 9u; i++) {
        ((uint32_t *)out.bindings[1].data)[i] = 0xDEADBEEFu;
    }
    CHECK_ST(su_world_commit_staging(w, b_only, 1u), SU_OK);
    CHECK_ST(su_world_abort(w), SU_OK);
    CHECK(!su_world_in_tick(w));
    CHECK(su_world_tick(w) == 1u);

    CHECK_ST(su_world_canonical_size(w, &n_after), SU_OK);
    CHECK(n_after == n_before);
    snap_after = malloc(n_after);
    CHECK(snap_after != NULL);
    if (snap_after != NULL) {
        CHECK_ST(su_world_canonical_write(w, snap_after, n_after, &n_after), SU_OK);
        CHECK(memcmp(snap_before, snap_after, n_before) == 0);
        free(snap_after);
    }
    CHECK_ST(su_world_read_committed(w, ID_B, &data, &rows), SU_OK);
    for (i = 0; i < 9u; i++) {
        CHECK(data[i] == 0u);
    }

    /* The next tick must start from committed, not from the discarded working
     * root: field b has to read as zero again after begin. */
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK_ST(su_world_build_outputs(w, &out), SU_OK);
    CHECK_ST(su_world_abort(w), SU_OK);
    CHECK_ST(su_world_abort(w), SU_OK); /* no-op outside a tick */
    CHECK(su_world_tick(w) == 1u);

    free(snap_before);
    su_world_destroy(w);
    free(ids);
}

static void test_publish_increments_tick(void) {
    SuWorld *w = NULL;
    uint32_t *ids = make_ids(8u);
    uint32_t a_val[8];
    uint32_t c_val[8];
    uint32_t c_unchanged[8];
    uint32_t b_list[1] = {ID_B};
    uint32_t i;
    SuOutputs out;
    SuInvocation inv;
    uint32_t tick;

    for (i = 0; i < 8u; i++) {
        a_val[i] = i + 1u;
        c_val[i] = 50u + i;
        c_unchanged[i] = 50u + i;
    }
    CHECK_ST(make_world(8u, k_fields, K_FIELD_COUNT, ids, &w), SU_OK);
    CHECK_ST(su_world_init_committed(w, ID_A, a_val, 8u), SU_OK);
    CHECK_ST(su_world_init_committed(w, ID_C, c_val, 8u), SU_OK);
    CHECK(su_world_tick(w) == 0u);

    for (tick = 1u; tick <= 3u; tick++) {
        uint32_t b_expect[8];
        CHECK_ST(su_world_begin_tick(w), SU_OK);
        CHECK_ST(su_world_build_invocation(w, &inv), SU_OK);
        CHECK_ST(su_world_build_outputs(w, &out), SU_OK);
        for (i = 0; i < 8u; i++) {
            /* Stage b = a + tick; stage c only in tick 1 so later ticks leave it
             * alone and that carry-over is checked too. */
            uint32_t a = ((uint32_t *)find_binding(inv.bindings, inv.binding_count, ID_A)->data)[i];
            ((uint32_t *)out.bindings[1].data)[i] = a + tick;
            b_expect[i] = a + tick;
        }
        CHECK_ST(su_world_commit_staging(w, b_list, 1u), SU_OK);
        /* Staged-but-uncommitted c must not leak into committed. */
        ((uint32_t *)out.bindings[0].data)[0] = 0xFFFFFFFFu;
        CHECK_ST(su_world_publish(w), SU_OK);
        CHECK(su_world_tick(w) == tick);
        CHECK(committed_equals(w, ID_B, b_expect, 8u));
        CHECK(committed_equals(w, ID_C, c_unchanged, 8u));
    }
    CHECK(su_world_tick(w) == 3u);

    su_world_destroy(w);
    free(ids);
}

static void test_bindings_alias_free(void) {
    SuWorld *w = NULL;
    uint32_t *ids = make_ids(9u);
    SuInvocation inv;
    SuOutputs out;
    uint32_t i;
    uint32_t j;
    const void *ptrs[4];
    uint64_t lens[4];
    uint32_t n = 0;

    CHECK_ST(make_world(9u, k_fields, K_FIELD_COUNT, ids, &w), SU_OK);
    seed(w, 9u);
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK_ST(su_world_build_invocation(w, &inv), SU_OK);
    CHECK_ST(su_world_build_outputs(w, &out), SU_OK);

    CHECK(inv.struct_size == sizeof(SuInvocation));
    CHECK(inv.abi_version == SU_ABI_VERSION);
    CHECK(out.struct_size == sizeof(SuOutputs));
    CHECK(out.reserved == 0u);

    /* Collect every binding's byte range: reads, then staging. */
    for (i = 0; i < inv.binding_count; i++) {
        CHECK(inv.bindings[i].struct_size == sizeof(SuColumnBinding));
        CHECK(inv.bindings[i].access == SU_ACCESS_READ);
        CHECK(inv.bindings[i].stride_bytes == 4u);
        CHECK(inv.bindings[i].elem_type == SU_TYPE_U32);
        CHECK(inv.bindings[i].layout_generation == 7u);
        CHECK(inv.bindings[i].row_count == 9u);
        if (i > 0) {
            CHECK(inv.bindings[i - 1u].field_id < inv.bindings[i].field_id);
        }
    }
    for (i = 0; i < out.binding_count; i++) {
        CHECK(out.bindings[i].access == SU_ACCESS_WRITE);
        CHECK(out.bindings[i].layout_generation == 7u);
        CHECK(out.bindings[i].row_count == 9u);
        if (i > 0) {
            CHECK(out.bindings[i - 1u].field_id < out.bindings[i].field_id);
        }
    }

    /* Pairwise: no two of the four column ranges (read a, read c, write b,
     * write c) may share bytes. */
    n = 0;
    for (i = 0; i < inv.binding_count; i++) {
        ptrs[n] = inv.bindings[i].data;
        lens[n] = inv.bindings[i].row_count;
        n++;
    }
    for (i = 0; i < out.binding_count; i++) {
        ptrs[n] = out.bindings[i].data;
        lens[n] = out.bindings[i].row_count;
        n++;
    }
    CHECK(n == 4u);
    for (i = 0; i < n; i++) {
        for (j = i + 1u; j < n; j++) {
            CHECK(!ranges_overlap(ptrs[i], lens[i], ptrs[j], lens[j]));
        }
    }
    /* Read and staging for the READ|WRITE field c must be distinct storage. */
    CHECK(find_binding(inv.bindings, inv.binding_count, ID_C)->data !=
          find_binding(out.bindings, out.binding_count, ID_C)->data);

    CHECK_ST(su_world_abort(w), SU_OK);
    su_world_destroy(w);
    free(ids);
}

static void test_invalid_arguments(void) {
    SuWorld *w = NULL;
    uint32_t *ids = make_ids(4u);
    SuInvocation inv;
    SuOutputs out;
    uint32_t ro[1] = {ID_A};
    uint32_t unknown[1] = {999u};
    uint32_t dup[2] = {ID_B, ID_B};
    uint32_t too_many[K_FIELD_COUNT + 1u] = {ID_B, ID_C, ID_B, ID_C};
    uint32_t vals[4] = {1u, 2u, 3u, 4u};
    const uint32_t *data = NULL;
    uint64_t rows = 0;
    size_t size = 0;
    uint8_t small[4];
    int eq = -1;

    /* NULL world / output pointers. */
    su_world_destroy(NULL);
    CHECK(su_world_row_count(NULL) == 0u);
    CHECK(!su_world_in_tick(NULL));
    CHECK_ST(su_world_begin_tick(NULL), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_publish(NULL), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_abort(NULL), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_canonical_size(NULL, &size), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_state_equal(NULL, NULL, &eq), SU_E_INVALID_ARGUMENT);

    CHECK_ST(make_world(4u, k_fields, K_FIELD_COUNT, ids, &w), SU_OK);
    CHECK(w != NULL);
    CHECK_ST(su_world_read_committed(w, ID_A, NULL, &rows), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_read_committed(w, ID_A, &data, NULL), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_read_committed(NULL, ID_A, &data, &rows), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_read_committed(w, 999u, &data, &rows), SU_E_SCHEMA_MISMATCH);
    CHECK_ST(su_world_read_committed_by_name(w, "nope", &data, &rows), SU_E_SCHEMA_MISMATCH);
    CHECK_ST(su_world_read_committed_by_name(w, NULL, &data, &rows), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_read_committed_by_name(w, "b", &data, &rows), SU_OK);
    CHECK(rows == 4u);

    /* Phase errors: outside a tick nothing may build views or publish. */
    CHECK_ST(su_world_build_invocation(w, &inv), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_build_outputs(w, &out), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_publish(w), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_commit_staging(w, ro, 1u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_init_committed(w, ID_A, vals, 3u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_init_committed(w, ID_A, NULL, 4u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_init_committed(w, 999u, vals, 4u), SU_E_SCHEMA_MISMATCH);
    CHECK(su_world_tick(w) == 0u);

    CHECK_ST(su_world_init_committed(w, ID_A, vals, 4u), SU_OK);

    CHECK_ST(su_world_begin_tick(w), SU_OK);
    CHECK_ST(su_world_begin_tick(w), SU_E_INVALID_ARGUMENT); /* already open */
    CHECK_ST(su_world_init_committed(w, ID_A, vals, 4u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_build_invocation(w, NULL), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_build_outputs(w, NULL), SU_E_INVALID_ARGUMENT);

    /* Commit-list validation: each bad list is rejected with no copy. */
    CHECK_ST(su_world_commit_staging(w, ro, 1u), SU_E_UNDECLARED_WRITE);
    CHECK_ST(su_world_commit_staging(w, unknown, 1u), SU_E_SCHEMA_MISMATCH);
    CHECK_ST(su_world_commit_staging(w, dup, 2u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_commit_staging(w, too_many, K_FIELD_COUNT + 1u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_commit_staging(w, NULL, 1u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_commit_staging(w, NULL, 0u), SU_OK);
    CHECK_ST(su_world_commit_staging(NULL, ro, 1u), SU_E_INVALID_ARGUMENT);

    CHECK_ST(su_world_abort(w), SU_OK);
    CHECK(!su_world_in_tick(w));

    /* Canonical encoding and comparison argument checks. */
    CHECK_ST(su_world_canonical_size(w, &size), SU_OK);
    CHECK_ST(su_world_canonical_write(w, small, sizeof small, &size), SU_E_INVALID_ARGUMENT);
    CHECK(size > sizeof small);
    CHECK_ST(su_world_canonical_write(w, NULL, 0u, &size), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_world_state_equal(w, w, NULL), SU_E_INVALID_ARGUMENT);

    su_world_destroy(w);
    free(ids);
}

static void test_canonical_state(void) {
    SuWorld *w1 = NULL;
    SuWorld *w2 = NULL;
    uint32_t *ids = make_ids(3u);
    uint32_t *ids2 = make_ids(3u);
    size_t size = 0;
    uint8_t *buf = NULL;
    size_t written = 0;
    int eq = -1;
    uint32_t b_list[1] = {ID_B};
    SuOutputs out;
    SuWorldFieldDesc renamed[K_FIELD_COUNT];
    uint32_t i;

    /* Layout v2: magic(9) + version(4) + tick(8) + field_count(4) + entity table (60 + 9 * high_water)
     *         + 3 * (id 4 + type 4 + values high_water * 4), with high_water = 3 for a fixture world. */
    CHECK_ST(make_world(3u, k_fields, K_FIELD_COUNT, ids, &w1), SU_OK);
    seed(w1, 3u);
    CHECK_ST(su_world_canonical_size(w1, &size), SU_OK);
    CHECK(size == 25u + 60u + 9u * 3u + 3u * (8u + 12u));
    buf = malloc(size);
    CHECK(buf != NULL);
    if (buf != NULL) {
        CHECK_ST(su_world_canonical_write(w1, buf, size, &written), SU_OK);
        CHECK(written == size);
        CHECK(memcmp(buf, "SUWSTATE\0", 9u) == 0);
        CHECK(buf[9] == 2u && buf[10] == 0u && buf[11] == 0u && buf[12] == 0u);
        /* Fields ascending after the entity table: first record is id 5 (field c), then 10, then 20. */
        CHECK(buf[25 + 60 + 27] == 5u);
        CHECK(buf[25 + 60 + 27 + 20] == 10u);
        CHECK(buf[25 + 60 + 27 + 40] == 20u);
        free(buf);
    }

    /* Same content, names and declaration order differ: identical state. */
    for (i = 0; i < K_FIELD_COUNT; i++) {
        renamed[i] = k_fields[K_FIELD_COUNT - 1u - i];
        renamed[i].name = (renamed[i].id == ID_A) ? "alpha" : (renamed[i].id == ID_B) ? "beta" : "gamma";
    }
    CHECK_ST(make_world(3u, renamed, K_FIELD_COUNT, ids2, &w2), SU_OK);
    seed(w2, 3u);
    CHECK_ST(su_world_state_equal(w1, w2, &eq), SU_OK);
    CHECK(eq == 1);

    /* A publish that changes committed values makes the states differ. */
    CHECK_ST(su_world_begin_tick(w2), SU_OK);
    CHECK_ST(su_world_build_outputs(w2, &out), SU_OK);
    ((uint32_t *)out.bindings[1].data)[1] = 77u;
    CHECK_ST(su_world_commit_staging(w2, b_list, 1u), SU_OK);
    CHECK_ST(su_world_publish(w2), SU_OK);
    CHECK_ST(su_world_state_equal(w1, w2, &eq), SU_OK);
    CHECK(eq == 0);

    /* Tick is part of the state: a no-change tick still differs from w1. */
    CHECK_ST(su_world_begin_tick(w1), SU_OK);
    CHECK_ST(su_world_publish(w1), SU_OK);
    CHECK(su_world_tick(w1) == 1u);
    CHECK_ST(su_world_state_equal(w1, w2, &eq), SU_OK);
    CHECK(eq == 0);

    su_world_destroy(w1);
    su_world_destroy(w2);
    free(ids);
    free(ids2);
}

static void test_entity_order_preserved(void) {
    /* Entity IDs and rows stay in creation order; the dump reflects both. */
    SuWorld *w = NULL;
    uint32_t ids[3] = {42u, 7u, 99u};
    uint32_t a_vals[3] = {1u, 2u, 3u};
    SuWorldFieldDesc f[1] = {{1u, SU_TYPE_U32, SU_ACCESS_READ, "a"}};
    const uint32_t *data = NULL;
    uint64_t rows = 0;

    CHECK_ST(make_world(3u, f, 1u, ids, &w), SU_OK);
    CHECK_ST(su_world_init_committed(w, 1u, a_vals, 3u), SU_OK);
    CHECK_ST(su_world_read_committed_by_name(w, "a", &data, &rows), SU_OK);
    CHECK(rows == 3u && data[0] == 1u && data[2] == 3u);
    CHECK(w != NULL && su_world_field_count(w) == 1u);
    su_world_destroy(w);
}

int main(void) {
    test_row_counts();
    test_row_limits();
    test_invalid_descriptions();
    test_abort_restores_committed();
    test_publish_increments_tick();
    test_bindings_alias_free();
    test_invalid_arguments();
    test_canonical_state();
    test_entity_order_preserved();

    printf("test_world: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
