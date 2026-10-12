/* M2-01a migration tests (src/world/migration/su_migrate.h; snapshot 0.3 in src/world/snapshot.h).
 *
 * Expectations for the migrated columns, schemas, retired IDs and the spawn row come from tests/migration/model.py
 * (expected JSON, path in argv[2]); the engine never computes them. The 1,000,000-row timing case checks the
 * transform against a closed form written here (the model would be too slow at that size; the formula is the
 * same one the model states).
 *
 * Usage: test_migrate <fixtures dir> <expected.json> [--write-golden]
 *   --write-golden regenerates tests/fixtures/snapshot03 from the engine (run once; the test then compares). */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ir/su_json.h"
#include "ir/su_sha256.h"
#include "sutekh/entities.h"
#include "sutekh/status.h"
#include "world/entities/su_ents.h"
#include "world/migration/su_migrate.h"
#include "world/snapshot.h"
#include "world/su_world.h"

static unsigned long g_checks;
static unsigned long g_fail;
static const char *g_fixtures;
static const char *g_expected_path;
static int g_write_golden;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_fail++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                        \
    } while (0)

#define CHECK_ST(expr, want)                                                     \
    do {                                                                         \
        uint32_t got_ = (expr);                                                  \
        g_checks++;                                                              \
        if (got_ != (uint32_t)(want)) {                                          \
            g_fail++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s returned %u, want %u\n", __FILE__,   \
                    __LINE__, #expr, (unsigned)got_, (unsigned)(want));          \
        }                                                                        \
    } while (0)

/* ---- helpers ------------------------------------------------------------ */

static char *read_file(const char *path, size_t *len) {
    FILE *fh = fopen(path, "rb");
    char *buf;
    long n;
    if (fh == NULL) {
        return NULL;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        return NULL;
    }
    n = ftell(fh);
    if (n < 0 || fseek(fh, 0, SEEK_SET) != 0) {
        fclose(fh);
        return NULL;
    }
    buf = malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(fh);
        return NULL;
    }
    if (fread(buf, 1u, (size_t)n, fh) != (size_t)n) {
        free(buf);
        fclose(fh);
        return NULL;
    }
    fclose(fh);
    buf[n] = '\0';
    *len = (size_t)n;
    return buf;
}

static void fixture(char *out, size_t cap, const char *sub, const char *name) {
    snprintf(out, cap, "%s/%s/%s", g_fixtures, sub, name);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static const SuJsonValue *jm(const SuJsonValue *o, const char *k) {
    return su_json_member(o, k);
}

static uint32_t jn(const SuJsonValue *v) {
    uint64_t x = 0;
    if (!su_json_get_uint(v, &x)) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)x;
}

/* Base world: tests/fixtures/snapshot02/worked_0_2.json, restored without the original fixture (allow). */
static SuWorld *load_base(void) {
    char path[512];
    size_t len = 0;
    char *bytes;
    SuWorld *w = NULL;
    SuSnapshotError err;
    fixture(path, sizeof path, "snapshot02", "worked_0_2.json");
    bytes = read_file(path, &len);
    if (bytes == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        g_fail++;
        return NULL;
    }
    CHECK_ST(su_snapshot_restore_world((const uint8_t *)bytes, len, NULL, 1, &w, NULL, &err), SU_OK);
    free(bytes);
    return w;
}

static int digest_of(const SuWorld *w, uint8_t out[32]) {
    return su_world_recovery_digest(w, out) == SU_OK;
}

static int digests_equal(const SuWorld *a, const SuWorld *b) {
    uint8_t da[32];
    uint8_t db[32];
    if (!digest_of(a, da) || !digest_of(b, db)) {
        return 0;
    }
    return memcmp(da, db, 32u) == 0;
}

/* Reads a whole fixture into a migration input set. */
typedef struct Docs {
    char *mig;
    size_t mig_len;
    char *xf;
    size_t xf_len;
    char *lir;
    size_t lir_len;
} Docs;

static int load_docs(Docs *d, const char *mig_name, int with_transform) {
    char path[512];
    memset(d, 0, sizeof *d);
    fixture(path, sizeof path, "migration", mig_name);
    d->mig = read_file(path, &d->mig_len);
    if (d->mig == NULL) {
        return 0;
    }
    if (with_transform) {
        fixture(path, sizeof path, "migration", "transform.json");
        d->xf = read_file(path, &d->xf_len);
        fixture(path, sizeof path, "migration", "migrate_alpha.lir.json");
        d->lir = read_file(path, &d->lir_len);
        if (d->xf == NULL || d->lir == NULL) {
            return 0;
        }
    }
    return 1;
}

static void docs_free(Docs *d) {
    free(d->mig);
    free(d->xf);
    free(d->lir);
    memset(d, 0, sizeof *d);
}

static SuMigrateInputs inputs_of(const Docs *d) {
    SuMigrateInputs in;
    in.migration = (const uint8_t *)d->mig;
    in.migration_len = d->mig_len;
    in.transform = (const uint8_t *)d->xf;
    in.transform_len = d->xf_len;
    in.lir = (const uint8_t *)d->lir;
    in.lir_len = d->lir_len;
    return in;
}

static SuMigrateBudget big_budget(void) {
    SuMigrateBudget b;
    b.spool_bytes = UINT64_C(512) * 1024u * 1024u; /* the sutekh binary default spool */
    b.memory_bytes = UINT64_C(1024) * 1024u * 1024u;
    return b;
}

/* Checks the destination schema and the columns of w against one model scenario. */
static void check_scenario_state(const SuWorld *w, const SuJsonValue *scn, const char *tag) {
    const SuJsonValue *schema = jm(scn, "schema");
    const SuJsonValue *cols = jm(scn, "columns");
    const SuJsonValue *ret = jm(scn, "retired");
    uint64_t cap = su_world_row_count(w);
    uint32_t i;
    uint32_t n_expect = schema->u.array.count;
    CHECK(su_world_field_count(w) == n_expect);
    for (i = 0; i < n_expect && i < su_world_field_count(w); i++) {
        SuWorldFieldDesc fd;
        uint32_t def = 0;
        const SuJsonValue *e = schema->u.array.items[i];
        CHECK_ST(su_world_field_at(w, i, &fd, &def), SU_OK);
        CHECK(fd.id == jn(jm(e, "id")));
        CHECK(strlen(fd.name) == jm(e, "name")->u.string.len);
        CHECK(memcmp(fd.name, jm(e, "name")->u.string.data, jm(e, "name")->u.string.len) == 0);
        CHECK(def == jn(jm(e, "default")));
    }
    /* Retired IDs. */
    {
        const uint32_t *ids = NULL;
        uint32_t nret = 0;
        uint32_t k;
        CHECK_ST(su_world_retired_ids(w, &ids, &nret), SU_OK);
        CHECK(nret == ret->u.array.count);
        for (k = 0; k < nret && k < ret->u.array.count; k++) {
            CHECK(ids[k] == jn(ret->u.array.items[k]));
        }
    }
    /* Columns, by name, over the whole capacity. */
    for (i = 0; i < cols->u.object.count; i++) {
        const SuJsonMember *m = &cols->u.object.members[i];
        const SuJsonValue *arr = m->value;
        uint32_t f;
        int found = 0;
        for (f = 0; f < su_world_field_count(w) && !found; f++) {
            SuWorldFieldDesc fd;
            uint32_t def;
            const uint32_t *data = NULL;
            uint64_t rows = 0;
            uint64_t r;
            if (su_world_field_at(w, f, &fd, &def) != SU_OK || strlen(fd.name) != m->key_len ||
                memcmp(fd.name, m->key, m->key_len) != 0) {
                continue;
            }
            found = 1;
            CHECK_ST(su_world_read_committed(w, fd.id, &data, &rows), SU_OK);
            CHECK(rows == cap);
            CHECK(arr->u.array.count == cap);
            for (r = 0; r < cap && r < arr->u.array.count; r++) {
                if (data[r] != jn(arr->u.array.items[r])) {
                    g_fail++;
                    fprintf(stderr, "FAIL %s column %s row %llu: got %u want %u\n", tag, fd.name,
                            (unsigned long long)r, (unsigned)data[r],
                            (unsigned)jn(arr->u.array.items[r]));
                    break;
                }
            }
            g_checks++;
        }
        CHECK(found);
    }
}

/* Spawn after migration: the row takes each field's default and lands on the model's slot. */
static void check_spawn(SuWorld *w, const SuJsonValue *scn, const char *tag) {
    const SuJsonValue *sp = jm(scn, "spawn");
    SuEntity h = SU_ENTITY_INVALID;
    uint32_t slot;
    uint32_t i;
    CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_OK);
    slot = SU_ENTITY_SLOT(h);
    CHECK(slot == jn(jm(sp, "slot")));
    for (i = 0; i < su_world_field_count(w); i++) {
        SuWorldFieldDesc fd;
        uint32_t def;
        const uint32_t *data = NULL;
        uint64_t rows = 0;
        const SuJsonValue *want = NULL;
        if (su_world_field_at(w, i, &fd, &def) != SU_OK) {
            g_fail++;
            continue;
        }
        want = jm(jm(sp, "values"), fd.name);
        CHECK(want != NULL);
        if (want == NULL) {
            continue;
        }
        CHECK_ST(su_world_read_committed(w, fd.id, &data, &rows), SU_OK);
        if (data[slot] != jn(want)) {
            g_fail++;
            fprintf(stderr, "FAIL %s spawn field %s: got %u want %u\n", tag, fd.name, (unsigned)data[slot],
                    (unsigned)jn(want));
        }
        g_checks++;
    }
}

/* Plans, prepares and swaps one migration on a fresh base world. */
static uint32_t migrate_once(SuWorld **live, const Docs *d, const SuMigrateSystem *sys, uint32_t nsys,
                             const char *const *drop, uint32_t ndrop, SuMigratePlan *plan) {
    SuMigrateInputs in = inputs_of(d);
    SuMigrateBudget b = big_budget();
    SuWorld *cand = NULL;
    SuWorld *retired = NULL;
    uint32_t st;
    st = su_migrate_plan(*live, &in, sys, nsys, drop, ndrop, &b, plan);
    if (st != SU_OK) {
        return st;
    }
    if (!plan->valid) {
        return SU_E_UNSUPPORTED_MIGRATION;
    }
    st = su_migrate_prepare(*live, plan, NULL, &cand, NULL);
    if (st != SU_OK) {
        return st;
    }
    st = su_migrate_swap(live, plan, cand, &retired);
    if (st != SU_OK) {
        su_world_destroy(cand);
        return st;
    }
    su_world_destroy(retired);
    return SU_OK;
}

/* ---- scenarios with the model ------------------------------------------- */

static void scenario_case(const SuJsonValue *expect, const char *name, const char *mig_file, int with_xf) {
    Docs d;
    SuMigratePlan plan;
    SuWorld *live = load_base();
    const SuJsonValue *scn = jm(jm(expect, "scenarios"), name);
    uint32_t st;
    char tag[64];
    snprintf(tag, sizeof tag, "scenario %s", name);
    CHECK(live != NULL);
    CHECK(scn != NULL);
    if (live == NULL || scn == NULL || !load_docs(&d, mig_file, with_xf)) {
        g_fail++;
        docs_free(&d);
        su_world_destroy(live);
        return;
    }
    memset(&plan, 0, sizeof plan);
    st = migrate_once(&live, &d, NULL, 0, NULL, 0, &plan);
    CHECK_ST(st, SU_OK);
    CHECK(plan.valid == 1);
    CHECK(plan.from_layout_generation == 1 && plan.to_layout_generation == 2);
    CHECK(su_world_layout_generation(live) == 2);
    CHECK(su_world_tick(live) == 1);
    check_scenario_state(live, scn, tag);
    check_spawn(live, scn, tag);
    su_migrate_plan_free(&plan);
    docs_free(&d);
    su_world_destroy(live);
}

/* ---- tests -------------------------------------------------------------- */

static void test_expected_columns(const SuJsonValue *expect) {
    scenario_case(expect, "add", "m_add.json", 0);
    scenario_case(expect, "rename", "m_rename.json", 0);
    scenario_case(expect, "remove", "m_remove.json", 0);
    scenario_case(expect, "mixed", "m_mixed.json", 1);
}

/* Plan details: counts, impact of an installed system, and admission on a removed field. */
static void test_plan_details(void) {
    Docs d;
    SuWorld *base = load_base();
    SuMigratePlan plan;
    SuMigrateSystemImpact *si;
    SuWorldFieldDesc movement[2];
    SuWorldFieldDesc audit[1];
    SuMigrateSystem sys[2];
    const char *drop_audit[1] = {"demo.audit"};
    SuMigrateInputs in;
    SuMigrateBudget bud = big_budget();

    CHECK(base != NULL);
    if (base == NULL) {
        return;
    }
    /* demo.movement uses a (id 10, renamed by m_rename) read/write; demo.audit reads c (id 5, removed by m_remove) */
    movement[0].id = 10;
    movement[0].type = SU_TYPE_U32;
    movement[0].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
    movement[0].name = "a";
    movement[1].id = 20;
    movement[1].type = SU_TYPE_U32;
    movement[1].access = SU_ACCESS_READ;
    movement[1].name = "b";
    audit[0].id = 5;
    audit[0].type = SU_TYPE_U32;
    audit[0].access = SU_ACCESS_READ;
    audit[0].name = "c";
    sys[0].system_id = "demo.movement";
    sys[0].fields = movement;
    sys[0].field_count = 2;
    sys[1].system_id = "demo.audit";
    sys[1].fields = audit;
    sys[1].field_count = 1;

    /* Rename: the system using id 10 needs resubmission; the other is compatible after resubmission. */
    CHECK(load_docs(&d, "m_rename.json", 0));
    in = inputs_of(&d);
    memset(&plan, 0, sizeof plan);
    CHECK_ST(su_migrate_plan(base, &in, sys, 2, NULL, 0, &bud, &plan), SU_OK);
    CHECK(plan.valid == 1);
    CHECK(plan.columns_renamed == 1 && plan.columns_added == 0 && plan.columns_removed == 0);
    CHECK(plan.schema_before_fields == 3 && plan.schema_after_fields == 3);
    CHECK(plan.system_count == 2);
    si = &plan.systems[0];
    CHECK(strcmp(si->system_id, "demo.movement") == 0);
    CHECK(si->impact == SU_MIGRATE_IMPACT_NEEDS_RESUBMISSION);
    CHECK((si->reasons & SU_MIGRATE_REASON_RENAMED) != 0u);
    CHECK((si->reasons & SU_MIGRATE_REASON_LAYOUT) != 0u);
    CHECK(plan.systems[1].impact == SU_MIGRATE_IMPACT_COMPATIBLE);
    CHECK(plan.rows == 6 && plan.capacity == 8);
    CHECK(plan.bytes_new_columns > 0u && plan.bytes_peak >= plan.bytes_new_columns);
    CHECK(plan.pre_migration_bytes > 0u && plan.post_commit_bytes_bound > 0u);
    /* The rename changes the schema digest even though no value changes (contract section 6). */
    {
        uint8_t before[32];
        CHECK_ST(su_world_schema_digest(base, before), SU_OK);
        CHECK(memcmp(before, plan.schema_before_digest, 32u) == 0);
        CHECK(memcmp(plan.schema_before_digest, plan.schema_after_digest, 32u) != 0);
    }
    su_migrate_plan_free(&plan);
    docs_free(&d);

    /* Remove: refused while demo.audit uses id 5; allowed when that system is dropped. */
    CHECK(load_docs(&d, "m_remove.json", 0));
    in = inputs_of(&d);
    memset(&plan, 0, sizeof plan);
    CHECK_ST(su_migrate_plan(base, &in, sys, 2, NULL, 0, &bud, &plan), SU_OK);
    CHECK(plan.valid == 0);
    CHECK(plan.reason == SU_MIGRATE_REFUSED_FIELD_IN_USE);
    CHECK(plan.systems[1].impact == SU_MIGRATE_IMPACT_BROKEN);
    CHECK((plan.systems[1].reasons & SU_MIGRATE_REASON_FIELD_REMOVED) != 0u);
    su_migrate_plan_free(&plan);
    memset(&plan, 0, sizeof plan);
    CHECK_ST(su_migrate_plan(base, &in, sys, 2, drop_audit, 1, &bud, &plan), SU_OK);
    CHECK(plan.valid == 1);
    CHECK(plan.columns_removed == 1);
    su_migrate_plan_free(&plan);
    docs_free(&d);

    /* Admission: a spool budget below the pre-migration plus post-commit estimate refuses the plan. */
    CHECK(load_docs(&d, "m_add.json", 0));
    in = inputs_of(&d);
    {
        SuMigrateBudget small = bud;
        small.spool_bytes = 1000u;
        memset(&plan, 0, sizeof plan);
        CHECK_ST(su_migrate_plan(base, &in, NULL, 0, NULL, 0, &small, &plan), SU_OK);
        CHECK(plan.valid == 0 && plan.reason == SU_MIGRATE_REFUSED_SPOOL_BUDGET);
        su_migrate_plan_free(&plan);
        small = bud;
        small.memory_bytes = 1u;
        memset(&plan, 0, sizeof plan);
        CHECK_ST(su_migrate_plan(base, &in, NULL, 0, NULL, 0, &small, &plan), SU_OK);
        CHECK(plan.valid == 0 && plan.reason == SU_MIGRATE_REFUSED_MEMORY);
        su_migrate_plan_free(&plan);
    }
    docs_free(&d);
    su_world_destroy(base);
}

/* Document checks in the contract's stage order (limits, parse, duplicate keys, shape, semantics). */
static uint32_t plan_text(SuWorld *w, const char *doc, SuMigratePlan *plan) {
    SuMigrateInputs in;
    SuMigrateBudget b = big_budget();
    in.migration = (const uint8_t *)doc;
    in.migration_len = strlen(doc);
    in.transform = NULL;
    in.transform_len = 0;
    in.lir = NULL;
    in.lir_len = 0;
    memset(plan, 0, sizeof *plan);
    return su_migrate_plan(w, &in, NULL, 0, NULL, 0, &b, plan);
}

#define DOC_HEAD "{\"migration_format\":\"0.1\",\"id\":\"t\",\"from_layout_generation\":1,\"to_layout_generation\":2,\"ops\":["
#define DOC_TAIL "]}"

static void test_malformed(void) {
    SuWorld *w = load_base();
    SuMigratePlan plan;
    char *big;
    char *deep;
    size_t i;
    size_t n;
    static const char *ops65_tail = "{\"op\":\"remove_field\",\"id\":5}";
    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }

    /* Stage 1: size (over 256 KiB) before anything else. */
    n = 256u * 1024u + 16u;
    big = malloc(n + 1u);
    CHECK(big != NULL);
    if (big != NULL) {
        memset(big, ' ', n);
        big[n] = '\0';
        CHECK_ST(plan_text(w, big, &plan), SU_E_RESOURCE_LIMIT);
        su_migrate_plan_free(&plan);
        free(big);
    }
    /* Stage 1: depth 18 nested arrays under an extra key (the document shape is otherwise wrong too). */
    {
        size_t depth = 18u;
        size_t cap = 64u + depth * 2u;
        deep = malloc(cap);
        CHECK(deep != NULL);
        if (deep != NULL) {
            size_t k = 0;
            k += (size_t)snprintf(deep + k, cap - k, "{\"x\":");
            for (i = 0; i < depth; i++) {
                deep[k++] = '[';
            }
            for (i = 0; i < depth; i++) {
                deep[k++] = ']';
            }
            deep[k++] = '}';
            deep[k] = '\0';
            CHECK_ST(plan_text(w, deep, &plan), SU_E_RESOURCE_LIMIT);
            su_migrate_plan_free(&plan);
            free(deep);
        }
    }
    /* Stage 2: syntax. */
    CHECK_ST(plan_text(w, "{\"migration_format\":", &plan), SU_E_PARSE);
    su_migrate_plan_free(&plan);
    /* Stage 3 beats stage 4: a duplicate key with wrong types elsewhere is a duplicate-key error. */
    CHECK_ST(plan_text(w, "{\"migration_format\":\"0.1\",\"migration_format\":\"0.1\",\"id\":5,\"from_layout_generation\":\"1\","
                          "\"to_layout_generation\":2,\"ops\":[]}", &plan), SU_E_DUPLICATE_KEY);
    su_migrate_plan_free(&plan);
    /* Stage 4: shape (wrong JSON types, extra and missing keys). */
    CHECK_ST(plan_text(w, "{\"migration_format\":\"0.1\",\"id\":\"t\",\"from_layout_generation\":\"1\","
                          "\"to_layout_generation\":2,\"ops\":[]}", &plan), SU_E_TYPE);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, "{\"migration_format\":\"0.1\",\"id\":\"t\",\"from_layout_generation\":1,"
                          "\"to_layout_generation\":2,\"ops\":[],\"extra\":1}", &plan), SU_E_TYPE);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, "{\"migration_format\":\"0.2\",\"id\":\"t\",\"from_layout_generation\":1,"
                          "\"to_layout_generation\":2,\"ops\":[]}", &plan), SU_E_TYPE);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, "{\"migration_format\":\"0.1\",\"id\":\"bad id!\",\"from_layout_generation\":1,"
                          "\"to_layout_generation\":2,\"ops\":[]}", &plan), SU_E_TYPE);
    su_migrate_plan_free(&plan);
    /* add_field with read/write keys is rejected (contract section 2, B6). */
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"add_field\",\"field\":{\"id\":30,\"name\":\"h\",\"type\":\"u32\","
                          "\"default\":1,\"read\":true}}" DOC_TAIL, &plan), SU_E_TYPE);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"add_field\",\"field\":{\"id\":0,\"name\":\"h\",\"type\":\"u32\","
                          "\"default\":1}}" DOC_TAIL, &plan), SU_E_TYPE);
    su_migrate_plan_free(&plan);
    /* Stage 4 op count: 65 ops is a limit error, 64 is accepted by the count (checked in shape only). */
    {
        size_t cap = 65u * 64u + 256u;
        char *doc = malloc(cap);
        size_t k = 0;
        CHECK(doc != NULL);
        if (doc != NULL) {
            k += (size_t)snprintf(doc + k, cap - k, "%s", DOC_HEAD);
            for (i = 0; i < 65u; i++) {
                k += (size_t)snprintf(doc + k, cap - k, "%s%s", i > 0u ? "," : "", ops65_tail);
            }
            k += (size_t)snprintf(doc + k, cap - k, "%s", DOC_TAIL);
            CHECK_ST(plan_text(w, doc, &plan), SU_E_RESOURCE_LIMIT);
            su_migrate_plan_free(&plan);
            free(doc);
        }
    }
    /* Semantics: generation, unknown op, schema ids and names, retired reuse, transform. */
    CHECK_ST(plan_text(w, "{\"migration_format\":\"0.1\",\"id\":\"t\",\"from_layout_generation\":5,"
                          "\"to_layout_generation\":6,\"ops\":[]}", &plan), SU_E_STALE_REVISION);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, "{\"migration_format\":\"0.1\",\"id\":\"t\",\"from_layout_generation\":1,"
                          "\"to_layout_generation\":3,\"ops\":[]}", &plan), SU_E_TYPE);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"frobnicate\"}" DOC_TAIL, &plan), SU_E_UNSUPPORTED_MIGRATION);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"add_field\",\"field\":{\"id\":10,\"name\":\"z\",\"type\":\"u32\","
                          "\"default\":1}}" DOC_TAIL, &plan), SU_E_SCHEMA_MISMATCH);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"add_field\",\"field\":{\"id\":30,\"name\":\"a\",\"type\":\"u32\","
                          "\"default\":1}}" DOC_TAIL, &plan), SU_E_SCHEMA_MISMATCH);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"rename_field\",\"id\":10,\"new_name\":\"b\"}" DOC_TAIL, &plan),
             SU_E_SCHEMA_MISMATCH);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"rename_field\",\"id\":99,\"new_name\":\"q\"}" DOC_TAIL, &plan),
             SU_E_SCHEMA_MISMATCH);
    su_migrate_plan_free(&plan);
    /* Retired reuse inside one document: remove 5 then add 5. */
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"remove_field\",\"id\":5},{\"op\":\"add_field\",\"field\":{\"id\":5,"
                          "\"name\":\"c2\",\"type\":\"u32\",\"default\":1}}" DOC_TAIL, &plan), SU_E_SCHEMA_MISMATCH);
    su_migrate_plan_free(&plan);
    /* Two transforms and a transform without its document. */
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"transform\",\"document\":\"transform.json\"},"
                          "{\"op\":\"transform\",\"document\":\"transform.json\"}" DOC_TAIL, &plan),
             SU_E_UNSUPPORTED_MIGRATION);
    su_migrate_plan_free(&plan);
    CHECK_ST(plan_text(w, DOC_HEAD "{\"op\":\"transform\",\"document\":\"transform.json\"}" DOC_TAIL, &plan),
             SU_E_TYPE);
    su_migrate_plan_free(&plan);
    su_world_destroy(w);
}

/* Transform documents: writes of a missing ID, undeclared reads, duplicate keys, wrong shape. */
static void test_transform_docs(void) {
    SuWorld *w = load_base();
    SuMigratePlan plan;
    SuMigrateInputs in;
    SuMigrateBudget b = big_budget();
    static const char *mig = DOC_HEAD "{\"op\":\"transform\",\"document\":\"transform.json\"}" DOC_TAIL;
    static const char *lir_ok =
        "{\"ir_version\":\"0.1\",\"system_id\":\"migrate.t\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"worked\","
        "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"a\"},"
        "{\"op\":\"store_u32\",\"field\":\"a\",\"src\":\"x\"}]}";
    static const char *lir_undeclared =
        "{\"ir_version\":\"0.1\",\"system_id\":\"migrate.t\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"worked\","
        "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"nosuch\"},"
        "{\"op\":\"store_u32\",\"field\":\"a\",\"src\":\"x\"}]}";
    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    /* writes of a missing ID: refused (SU_E_SCHEMA_MISMATCH). */
    in.migration = (const uint8_t *)mig;
    in.migration_len = strlen(mig);
    in.transform = (const uint8_t *)"{\"transform_format\":\"0.1\",\"source\":\"t.lir.json\",\"writes\":[99]}";
    in.transform_len = strlen((const char *)in.transform);
    in.lir = (const uint8_t *)lir_ok;
    in.lir_len = strlen(lir_ok);
    memset(&plan, 0, sizeof plan);
    CHECK_ST(su_migrate_plan(w, &in, NULL, 0, NULL, 0, &b, &plan), SU_E_SCHEMA_MISMATCH);
    su_migrate_plan_free(&plan);
    /* Duplicate key in the transform document. */
    in.transform = (const uint8_t *)"{\"transform_format\":\"0.1\",\"transform_format\":\"0.1\",\"source\":\"t\",\"writes\":[10]}";
    in.transform_len = strlen((const char *)in.transform);
    CHECK_ST(su_migrate_plan(w, &in, NULL, 0, NULL, 0, &b, &plan), SU_E_DUPLICATE_KEY);
    su_migrate_plan_free(&plan);
    /* Repeated writes and a wrong type in writes are shape errors. */
    in.transform = (const uint8_t *)"{\"transform_format\":\"0.1\",\"source\":\"t\",\"writes\":[10,10]}";
    in.transform_len = strlen((const char *)in.transform);
    CHECK_ST(su_migrate_plan(w, &in, NULL, 0, NULL, 0, &b, &plan), SU_E_TYPE);
    su_migrate_plan_free(&plan);
    /* The IR reads a field the world does not have: verification fails (SU_E_CANDIDATE_FAILED). */
    in.transform = (const uint8_t *)"{\"transform_format\":\"0.1\",\"source\":\"t\",\"writes\":[10]}";
    in.transform_len = strlen((const char *)in.transform);
    in.lir = (const uint8_t *)lir_undeclared;
    in.lir_len = strlen(lir_undeclared);
    CHECK_ST(su_migrate_plan(w, &in, NULL, 0, NULL, 0, &b, &plan), SU_E_CANDIDATE_FAILED);
    su_migrate_plan_free(&plan);
    /* The IR's query must be the world's schema: a different schema ID is a schema mismatch. */
    in.lir = (const uint8_t *)lir_ok;
    in.lir_len = strlen(lir_ok);
    {
        static const char *lir_other =
            "{\"ir_version\":\"0.1\",\"system_id\":\"migrate.t\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"other\","
            "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"a\"},"
            "{\"op\":\"store_u32\",\"field\":\"a\",\"src\":\"x\"}]}";
        in.lir = (const uint8_t *)lir_other;
        in.lir_len = strlen(lir_other);
        CHECK_ST(su_migrate_plan(w, &in, NULL, 0, NULL, 0, &b, &plan), SU_E_CANDIDATE_FAILED);
        su_migrate_plan_free(&plan);
    }
    su_world_destroy(w);
}

/* Failure injection at every allocation point of prepare (contract section 10): each N fails cleanly, the old
 * world keeps its recovery digest, and the first N past the last point succeeds. */
static void test_failure_injection(void) {
    Docs d;
    SuWorld *base = load_base();
    SuMigratePlan plan;
    SuMigrateInputs in;
    SuMigrateBudget b = big_budget();
    uint8_t d0[32];
    uint64_t n;
    uint64_t total = 0;
    CHECK(base != NULL);
    if (base == NULL) {
        return;
    }
    CHECK(load_docs(&d, "m_mixed.json", 1));
    in = inputs_of(&d);
    CHECK_ST(su_migrate_plan(base, &in, NULL, 0, NULL, 0, &b, &plan), SU_OK);
    CHECK(plan.valid == 1);
    CHECK(digest_of(base, d0));
    for (n = 1; n < 100000u; n++) {
        SuMigrateHooks h;
        SuWorld *cand = NULL;
        uint8_t now[32];
        uint32_t st;
        memset(&h, 0, sizeof h);
        h.fail_at = n;
        st = su_migrate_prepare(base, &plan, &h, &cand, NULL);
        if (st == SU_OK) {
            total = h.points;
            CHECK(cand != NULL);
            CHECK(n > total); /* the success is past the last point */
            su_world_destroy(cand);
            break;
        }
        CHECK(cand == NULL);
        CHECK(st == SU_E_RESOURCE_LIMIT);
        CHECK(h.points >= n);
        CHECK(digest_of(base, now));
        CHECK(memcmp(now, d0, 32u) == 0);
    }
    CHECK(total >= 8u); /* the injection points cover the entity scratch, buffers, transform and columns */
    if (g_checks > 0u) {
        printf("migration failure injection: %llu allocation points in prepare\n", (unsigned long long)total);
    }
    /* A failing transform is reported as a candidate failure and leaves the world unchanged. */
    {
        SuMigrateHooks h;
        SuWorld *cand = NULL;
        uint8_t now[32];
        memset(&h, 0, sizeof h);
        h.fail_transform = 1;
        CHECK_ST(su_migrate_prepare(base, &plan, &h, &cand, NULL), SU_E_CANDIDATE_FAILED);
        CHECK(cand == NULL);
        CHECK(digest_of(base, now));
        CHECK(memcmp(now, d0, 32u) == 0);
    }
    su_migrate_plan_free(&plan);
    docs_free(&d);
    su_world_destroy(base);
}

/* Staleness: a state change after the plan refuses prepare and swap, and the live world is untouched. */
static void test_staleness(void) {
    Docs d;
    SuWorld *live = load_base();
    SuWorld *cand = NULL;
    SuWorld *retired = NULL;
    SuMigratePlan plan;
    SuMigrateInputs in;
    SuMigrateBudget b = big_budget();
    SuEntity h = SU_ENTITY_INVALID;
    uint8_t before[32];
    uint8_t after[32];
    CHECK(live != NULL);
    if (live == NULL) {
        return;
    }
    CHECK(load_docs(&d, "m_add.json", 0));
    in = inputs_of(&d);
    memset(&plan, 0, sizeof plan);
    CHECK_ST(su_migrate_plan(live, &in, NULL, 0, NULL, 0, &b, &plan), SU_OK);
    /* Prepare a candidate, then change the live world: swap must refuse it. */
    CHECK_ST(su_migrate_prepare(live, &plan, NULL, &cand, NULL), SU_OK);
    CHECK_ST(su_world_spawn(live, NULL, 0u, 7u, &h), SU_OK);
    CHECK(digest_of(live, before));
    CHECK_ST(su_migrate_swap(&live, &plan, cand, &retired), SU_E_STALE_REVISION);
    CHECK(retired == NULL);
    CHECK(digest_of(live, after));
    CHECK(memcmp(before, after, 32u) == 0);
    su_world_destroy(cand);
    /* A stale plan cannot prepare either. */
    CHECK_ST(su_migrate_prepare(live, &plan, NULL, &cand, NULL), SU_E_STALE_REVISION);
    CHECK(cand == NULL);
    su_migrate_plan_free(&plan);
    docs_free(&d);
    su_world_destroy(live);
}

/* Zero-row worlds are valid migration targets. */
static void test_zero_rows(void) {
    SuWorldFieldDesc f;
    SuWorldDesc desc;
    SuWorld *w = NULL;
    SuMigratePlan plan;
    SuMigrateInputs in;
    SuMigrateBudget b = big_budget();
    SuWorld *cand = NULL;
    SuWorld *retired = NULL;
    static const char *doc = "{\"migration_format\":\"0.1\",\"id\":\"zero\",\"from_layout_generation\":1,"
                             "\"to_layout_generation\":2,\"ops\":[{\"op\":\"add_field\",\"field\":{\"id\":2,"
                             "\"name\":\"y\",\"type\":\"u32\",\"default\":5}}]}";
    f.id = 1;
    f.type = SU_TYPE_U32;
    f.access = SU_ACCESS_READ;
    f.name = "x";
    memset(&desc, 0, sizeof desc);
    desc.schema_id = "zero";
    desc.schema_version = 1;
    desc.layout_generation = 1;
    desc.row_count = 0;
    desc.entity_ids = NULL;
    desc.fields = &f;
    desc.field_count = 1;
    CHECK_ST(su_world_create(&desc, &w), SU_OK);
    CHECK(w != NULL);
    if (w == NULL) {
        return;
    }
    in.migration = (const uint8_t *)doc;
    in.migration_len = strlen(doc);
    in.transform = NULL;
    in.transform_len = 0;
    in.lir = NULL;
    in.lir_len = 0;
    memset(&plan, 0, sizeof plan);
    CHECK_ST(su_migrate_plan(w, &in, NULL, 0, NULL, 0, &b, &plan), SU_OK);
    CHECK(plan.valid == 1 && plan.rows == 0);
    CHECK_ST(su_migrate_prepare(w, &plan, NULL, &cand, NULL), SU_OK);
    CHECK(su_world_row_count(cand) == 0);
    CHECK(su_world_field_count(cand) == 2);
    CHECK_ST(su_migrate_swap(&w, &plan, cand, &retired), SU_OK);
    su_world_destroy(retired);
    /* Zero-row world: a spawn needs a free slot, and there is none (capacity 0). */
    {
        SuEntity h = SU_ENTITY_INVALID;
        CHECK_ST(su_world_spawn(w, NULL, 0u, 0u, &h), SU_E_RESOURCE_LIMIT);
    }
    su_migrate_plan_free(&plan);
    su_world_destroy(w);
}

/* Entity worlds: generations, retired slots and the entity table survive a migration unchanged. */
static void test_entity_worlds(void) {
    Docs d;
    SuWorld *base = load_base();
    SuWorld *cand = NULL;
    SuWorld *retired = NULL;
    SuMigratePlan plan;
    SuMigrateInputs in;
    SuMigrateBudget b = big_budget();
    int eq = 0;
    uint32_t gen_before = 0;
    uint32_t gen_after = 0;
    CHECK(base != NULL);
    if (base == NULL) {
        return;
    }
    CHECK(load_docs(&d, "m_remove.json", 0));
    in = inputs_of(&d);
    memset(&plan, 0, sizeof plan);
    CHECK_ST(su_migrate_plan(base, &in, NULL, 0, NULL, 0, &b, &plan), SU_OK);
    CHECK_ST(su_migrate_prepare(base, &plan, NULL, &cand, NULL), SU_OK);
    CHECK_ST(su_ents_state_equal(su_world_entities_committed(base), su_world_entities_committed(cand), &eq), SU_OK);
    CHECK(eq == 1);
    CHECK(su_ents_slot_retired(su_world_entities_committed(cand), 2) == 1);
    CHECK(su_ents_alive_count(su_world_entities_committed(cand)) == su_ents_alive_count(su_world_entities_committed(base)));
    CHECK_ST(su_ents_slot_generation(su_world_entities_committed(base), 2, &gen_before), SU_OK);
    CHECK_ST(su_ents_slot_generation(su_world_entities_committed(cand), 2, &gen_after), SU_OK);
    CHECK(gen_before == gen_after && gen_after == 0xFFFFFFFFu);
    CHECK(su_world_tick(cand) == su_world_tick(base));
    CHECK_ST(su_migrate_swap(&base, &plan, cand, &retired), SU_OK);
    su_world_destroy(retired);
    su_world_destroy(base);
    su_migrate_plan_free(&plan);
    docs_free(&d);
}

/* Schema 0.3 round trips: a migrated world captures as 0.3, restores without the fixture, keeps its digests,
 * refuses ID reuse, and a plain restore of a different schema is refused. The golden is regenerated and
 * compared byte for byte. */
static void test_snapshot03(void) {
    Docs d;
    SuWorld *base = load_base();
    SuMigratePlan plan;
    SuMigrateInputs in;
    SuMigrateBudget b = big_budget();
    SuWorld *cand = NULL;
    SuWorld *retired = NULL;
    SuWorld *live;
    SuSnapshot snap;
    SuWorldDesc desc;
    SuWorldFieldDesc fields[SU_LIMIT_FIELDS];
    uint32_t i;
    uint8_t pre[32];
    uint8_t post[32];
    char path[512];
    CHECK(base != NULL);
    if (base == NULL) {
        return;
    }
    /* Pre-migration capture is 0.2 (no lineage, no retired IDs). */
    {
        SuWorldFieldDesc bf[SU_LIMIT_FIELDS];
        SuWorldDesc bd;
        uint32_t bn = su_world_field_count(base);
        for (i = 0; i < bn; i++) {
            uint32_t def = 0;
            CHECK_ST(su_world_field_at(base, i, &bf[i], &def), SU_OK);
        }
        memset(&bd, 0, sizeof bd);
        bd.schema_id = su_world_schema_id(base);
        bd.schema_version = su_world_schema_version(base);
        bd.layout_generation = su_world_layout_generation(base);
        bd.fields = bf;
        bd.field_count = bn;
        CHECK_ST(su_snapshot_capture(base, &bd, "rev-pre", NULL, 0, &snap), SU_OK);
        CHECK(snap.len > 0 && memcmp(snap.bytes, "{\"snapshot_format\":\"0.2\"", 24) == 0);
        /* Rollback proof: the 0.2 bytes restored with allow_schema_change give back the pre-migration digest. */
        {
            SuWorld *rb = NULL;
            CHECK_ST(su_world_recovery_digest(base, pre), SU_OK);
            CHECK_ST(su_snapshot_restore_world(snap.bytes, snap.len, NULL, 1, &rb, NULL, NULL), SU_OK);
            CHECK(rb != NULL && digests_equal(rb, base));
            su_world_destroy(rb);
        }
        su_snapshot_destroy(&snap);
    }
    CHECK(load_docs(&d, "m_mixed.json", 1));
    in = inputs_of(&d);
    memset(&plan, 0, sizeof plan);
    CHECK_ST(su_migrate_plan(base, &in, NULL, 0, NULL, 0, &b, &plan), SU_OK);
    CHECK_ST(su_migrate_prepare(base, &plan, NULL, &cand, NULL), SU_OK);
    CHECK_ST(su_migrate_swap(&base, &plan, cand, &retired), SU_OK);
    su_world_destroy(retired);
    live = base;

    /* Capture the migrated world. */
    {
        uint32_t n = su_world_field_count(live);
        memset(&desc, 0, sizeof desc);
        for (i = 0; i < n; i++) {
            uint32_t def = 0;
            CHECK_ST(su_world_field_at(live, i, &fields[i], &def), SU_OK);
        }
        desc.schema_id = su_world_schema_id(live);
        desc.schema_version = su_world_schema_version(live);
        desc.layout_generation = su_world_layout_generation(live);
        desc.fields = fields;
        desc.field_count = n;
        CHECK_ST(su_snapshot_capture(live, &desc, "rev-3", NULL, 0, &snap), SU_OK);
    }
    CHECK(snap.len > 0 && memcmp(snap.bytes, "{\"snapshot_format\":\"0.3\"", 24) == 0);
    CHECK(strstr((const char *)snap.bytes, "\"migration_lineage\":[\"") != NULL);
    CHECK(strstr((const char *)snap.bytes, "\"retired_field_ids\":[]}") != NULL);

    /* Golden: byte identity with tests/fixtures/snapshot03, and its identity file. */
    fixture(path, sizeof path, "snapshot03", "worked_0_3.json");
    if (g_write_golden) {
        FILE *fh = fopen(path, "wb");
        char shafile[512];
        if (fh != NULL) {
            fwrite(snap.bytes, 1u, snap.len, fh);
            fclose(fh);
        }
        snprintf(shafile, sizeof shafile, "%s/snapshot03/worked_0_3.sha256", g_fixtures);
        fh = fopen(shafile, "wb");
        if (fh != NULL) {
            fprintf(fh, "%s\n", snap.id);
            fclose(fh);
        }
        printf("golden written: %s (%s)\n", path, snap.id);
    } else {
        size_t glen = 0;
        char *golden = read_file(path, &glen);
        char shafile[512];
        char *shatext;
        size_t shalen = 0;
        snprintf(shafile, sizeof shafile, "%s/snapshot03/worked_0_3.sha256", g_fixtures);
        shatext = read_file(shafile, &shalen);
        CHECK(golden != NULL);
        if (golden != NULL) {
            CHECK(glen == snap.len && memcmp(golden, snap.bytes, glen) == 0);
            free(golden);
        }
        CHECK(shatext != NULL);
        if (shatext != NULL) {
            CHECK(strncmp(shatext, snap.id, 64u) == 0);
            free(shatext);
        }
    }

    /* Round trip without the fixture: schema, defaults, lineage, generation and digests come back. */
    {
        SuWorld *rt = NULL;
        SuSnapshotData data;
        SuSnapshotError err;
        CHECK_ST(su_world_recovery_digest(live, post), SU_OK);
        CHECK_ST(su_snapshot_restore_world(snap.bytes, snap.len, NULL, 1, &rt, &data, &err), SU_OK);
        CHECK(rt != NULL);
        if (rt != NULL) {
            CHECK(digests_equal(rt, live));
            CHECK(su_world_layout_generation(rt) == 2);
            {
                const uint8_t (*lin)[32] = NULL;
                uint32_t ln = 0;
                CHECK_ST(su_world_lineage(rt, &lin, &ln), SU_OK);
                CHECK(ln == 1u);
            }
            su_world_destroy(rt);
        }
        su_snapshot_data_free(&data);
    }
    /* A plain restore of a schema-changed snapshot into the pre-migration descriptor is refused. */
    {
        SuWorldFieldDesc old_f[3];
        SuWorldDesc od;
        SuWorld *rt = NULL;
        SuSnapshotError err;
        old_f[0].id = 5;
        old_f[0].type = SU_TYPE_U32;
        old_f[0].access = SU_ACCESS_READ;
        old_f[0].name = "c";
        old_f[1].id = 10;
        old_f[1].type = SU_TYPE_U32;
        old_f[1].access = SU_ACCESS_READ;
        old_f[1].name = "a";
        old_f[2].id = 20;
        old_f[2].type = SU_TYPE_U32;
        old_f[2].access = SU_ACCESS_READ;
        old_f[2].name = "b";
        memset(&od, 0, sizeof od);
        od.schema_id = "worked";
        od.schema_version = 1;
        od.layout_generation = 1;
        od.fields = old_f;
        od.field_count = 3;
        CHECK_ST(su_snapshot_restore_world(snap.bytes, snap.len, &od, 0, &rt, NULL, &err), SU_E_SCHEMA_MISMATCH);
        CHECK(rt == NULL);
    }
    /* Retired-ID reuse is refused after the round trip: a remove followed by a fresh add of the removed ID. */
    {
        Docs rm;
        SuMigratePlan p2;
        SuWorld *rt = NULL;
        SuWorld *rc = NULL;
        SuSnapshotError err;
        SuMigrateInputs in2;
        SuMigrateBudget b2 = big_budget();
        CHECK(load_docs(&rm, "m_remove.json", 0));
        {
            /* Build the removed-ID lineage: remove 5 on the base, snapshot 0.3, restore, then try to re-add 5. */
            SuWorld *base2 = load_base();
            SuMigratePlan p3;
            SuMigrateInputs in3 = inputs_of(&rm);
            SuWorld *c3 = NULL;
            SuWorld *r3 = NULL;
            SuWorld *live3;
            SuSnapshot s3;
            SuWorldFieldDesc f3[SU_LIMIT_FIELDS];
            uint32_t n3;
            CHECK(base2 != NULL);
            memset(&p3, 0, sizeof p3);
            CHECK_ST(su_migrate_plan(base2, &in3, NULL, 0, NULL, 0, &b2, &p3), SU_OK);
            CHECK_ST(su_migrate_prepare(base2, &p3, NULL, &c3, NULL), SU_OK);
            CHECK_ST(su_migrate_swap(&base2, &p3, c3, &r3), SU_OK);
            su_world_destroy(r3);
            su_migrate_plan_free(&p3);
            live3 = base2;
            n3 = su_world_field_count(live3);
            {
                uint32_t q;
                SuWorldDesc dd;
                memset(&dd, 0, sizeof dd);
                for (q = 0; q < n3; q++) {
                    uint32_t def = 0;
                    CHECK_ST(su_world_field_at(live3, q, &f3[q], &def), SU_OK);
                }
                dd.schema_id = su_world_schema_id(live3);
                dd.schema_version = su_world_schema_version(live3);
                dd.layout_generation = su_world_layout_generation(live3);
                dd.fields = f3;
                dd.field_count = n3;
                CHECK_ST(su_snapshot_capture(live3, &dd, "rev-r", NULL, 0, &s3), SU_OK);
            }
            /* Restore into a world built only from the snapshot header, then plan the forbidden re-add of 5. */
            CHECK_ST(su_snapshot_restore_world(s3.bytes, s3.len, NULL, 1, &rt, NULL, &err), SU_OK);
            su_snapshot_destroy(&s3);
            su_world_destroy(live3);
        }
        CHECK(rt != NULL);
        if (rt != NULL) {
            static const char *readd =
                "{\"migration_format\":\"0.1\",\"id\":\"readd\",\"from_layout_generation\":2,\"to_layout_generation\":3,"
                "\"ops\":[{\"op\":\"add_field\",\"field\":{\"id\":5,\"name\":\"c2\",\"type\":\"u32\",\"default\":1}}]}";
            in2.migration = (const uint8_t *)readd;
            in2.migration_len = strlen(readd);
            in2.transform = NULL;
            in2.transform_len = 0;
            in2.lir = NULL;
            in2.lir_len = 0;
            memset(&p2, 0, sizeof p2);
            CHECK_ST(su_migrate_plan(rt, &in2, NULL, 0, NULL, 0, &b2, &p2), SU_E_SCHEMA_MISMATCH);
            su_migrate_plan_free(&p2);
            (void)rc;
            su_world_destroy(rt);
        }
        docs_free(&rm);
    }
    su_snapshot_destroy(&snap);
    su_migrate_plan_free(&plan);
    docs_free(&d);
    su_world_destroy(live);
}

/* Timing observation at 1,000,000 rows (contract section 10). Reported separately; shared VM. The transform is
 * checked against the closed form: alpha = a + b and d = a * b, both mod 2^32, with a = i and b = 2i. */
static void test_million_rows(void) {
    enum { ROWS = 1000000 };
    SuWorldFieldDesc f[3];
    SuWorldDesc desc;
    SuWorld *w = NULL;
    SuWorld *cand = NULL;
    SuWorld *retired = NULL;
    SuMigratePlan plan;
    SuMigrateTimings tm;
    SuMigrateInputs in;
    SuMigrateBudget b = big_budget();
    uint32_t *ids = NULL;
    uint32_t *av = NULL;
    uint32_t *bv = NULL;
    uint32_t *cv = NULL;
    uint64_t t0;
    uint64_t t_plan;
    uint64_t t_prep;
    uint64_t t_swap;
    Docs d;
    uint64_t r;
    f[0].id = 5;
    f[0].type = SU_TYPE_U32;
    f[0].access = SU_ACCESS_READ;
    f[0].name = "c";
    f[1].id = 10;
    f[1].type = SU_TYPE_U32;
    f[1].access = SU_ACCESS_READ;
    f[1].name = "a";
    f[2].id = 20;
    f[2].type = SU_TYPE_U32;
    f[2].access = SU_ACCESS_READ;
    f[2].name = "b";
    ids = malloc(ROWS * sizeof *ids);
    av = malloc(ROWS * sizeof *av);
    bv = malloc(ROWS * sizeof *bv);
    cv = malloc(ROWS * sizeof *cv);
    CHECK(ids != NULL && av != NULL && bv != NULL && cv != NULL);
    if (ids == NULL || av == NULL || bv == NULL || cv == NULL) {
        goto out;
    }
    for (r = 0; r < ROWS; r++) {
        ids[r] = (uint32_t)(r + 1u);
        av[r] = (uint32_t)r;
        bv[r] = (uint32_t)(2u * r);
        cv[r] = (uint32_t)(r % 7u);
    }
    memset(&desc, 0, sizeof desc);
    desc.schema_id = "worked";
    desc.schema_version = 1;
    desc.layout_generation = 1;
    desc.row_count = ROWS;
    desc.entity_ids = ids;
    desc.fields = f;
    desc.field_count = 3;
    CHECK_ST(su_world_create(&desc, &w), SU_OK);
    if (w == NULL) {
        goto out;
    }
    CHECK_ST(su_world_put_committed(w, 10, av, ROWS), SU_OK);
    CHECK_ST(su_world_put_committed(w, 20, bv, ROWS), SU_OK);
    CHECK_ST(su_world_put_committed(w, 5, cv, ROWS), SU_OK);
    CHECK(load_docs(&d, "m_mixed.json", 1));
    if (w == NULL) {
        goto out;
    }
    in = inputs_of(&d);
    memset(&plan, 0, sizeof plan);
    memset(&tm, 0, sizeof tm);
    t0 = now_ns();
    CHECK_ST(su_migrate_plan(w, &in, NULL, 0, NULL, 0, &b, &plan), SU_OK);
    t_plan = now_ns() - t0;
    CHECK(plan.valid == 1);
    t0 = now_ns();
    CHECK_ST(su_migrate_prepare(w, &plan, NULL, &cand, &tm), SU_OK);
    t_prep = now_ns() - t0;
    t0 = now_ns();
    CHECK_ST(su_migrate_swap(&w, &plan, cand, &retired), SU_OK);
    t_swap = now_ns() - t0;
    su_world_destroy(retired);
    {
        const uint32_t *alpha = NULL;
        const uint32_t *dd = NULL;
        uint64_t rows = 0;
        int bad = 0;
        CHECK_ST(su_world_read_committed(w, 10, &alpha, &rows), SU_OK);
        CHECK(rows == ROWS);
        CHECK_ST(su_world_read_committed(w, 30, &dd, &rows), SU_OK);
        for (r = 0; r < ROWS && !bad; r++) {
            uint32_t a = (uint32_t)r;
            uint32_t bb = (uint32_t)(2u * r);
            if (alpha[r] != (uint32_t)(a + bb) || dd[r] != (uint32_t)(a * bb)) {
                bad = 1;
            }
        }
        CHECK(!bad);
    }
    printf("TIMING 1000000 rows (observation, shared VM, this host; not a benchmark): plan_ns=%llu prepare_ns=%llu "
           "transform_ns=%llu swap_ns=%llu\n",
           (unsigned long long)t_plan, (unsigned long long)t_prep, (unsigned long long)tm.transform_ns,
           (unsigned long long)t_swap);
    su_migrate_plan_free(&plan);
    docs_free(&d);
out:
    su_world_destroy(w);
    free(ids);
    free(av);
    free(bv);
    free(cv);
}

/* Recovery digest sensitivity at one generation and one canonical state: a rename alone and a default alone
 * each change the digest (contract section 6), and so does the schema digest. */
static void test_digest_sensitivity(void) {
    SuWorldFieldDesc fx;
    SuWorldFieldDesc fy;
    SuWorldDesc dx;
    SuWorldDesc dy;
    SuWorld *wx = NULL;
    SuWorld *wy = NULL;
    SuWorld *wd = NULL;
    uint32_t dflt[1] = {7u};
    SuWorldSchemaExt ext;
    uint8_t rx[32];
    uint8_t ry[32];
    uint8_t rd[32];
    uint8_t sx[32];
    uint8_t sy[32];
    fx.id = 1;
    fx.type = SU_TYPE_U32;
    fx.access = SU_ACCESS_READ;
    fx.name = "x";
    fy = fx;
    fy.name = "y";
    memset(&dx, 0, sizeof dx);
    dx.schema_id = "dig";
    dx.schema_version = 1;
    dx.layout_generation = 1;
    dx.fields = &fx;
    dx.field_count = 1;
    dy = dx;
    dy.fields = &fy;
    CHECK_ST(su_world_create(&dx, &wx), SU_OK);
    CHECK_ST(su_world_create(&dy, &wy), SU_OK);
    memset(&ext, 0, sizeof ext);
    ext.defaults = dflt;
    CHECK_ST(su_world_create_ext(&dx, 0u, &ext, &wd), SU_OK);
    if (wx == NULL || wy == NULL || wd == NULL) {
        su_world_destroy(wx);
        su_world_destroy(wy);
        su_world_destroy(wd);
        return;
    }
    CHECK_ST(su_world_recovery_digest(wx, rx), SU_OK);
    CHECK_ST(su_world_recovery_digest(wy, ry), SU_OK);
    CHECK_ST(su_world_recovery_digest(wd, rd), SU_OK);
    CHECK(memcmp(rx, ry, 32u) != 0);
    CHECK(memcmp(rx, rd, 32u) != 0);
    CHECK_ST(su_world_schema_digest(wx, sx), SU_OK);
    CHECK_ST(su_world_schema_digest(wy, sy), SU_OK);
    CHECK(memcmp(sx, sy, 32u) != 0);
    su_world_destroy(wx);
    su_world_destroy(wy);
    su_world_destroy(wd);
}

int main(int argc, char **argv) {
    char path[512];
    size_t len = 0;
    char *text;
    SuJsonDoc doc;
    SuJsonLimits lim;
    SuJsonError jerr;
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fixtures dir> <expected.json> [--write-golden]\n", argv[0]);
        return 2;
    }
    g_fixtures = argv[1];
    g_expected_path = argv[2];
    g_write_golden = argc > 3 && strcmp(argv[3], "--write-golden") == 0;
    text = read_file(g_expected_path, &len);
    if (text == NULL) {
        fprintf(stderr, "cannot read expected model output %s\n", g_expected_path);
        return 2;
    }
    lim.max_bytes = len;
    lim.max_depth = 32;
    if (su_json_parse((const uint8_t *)text, len, &lim, &doc, &jerr) != SU_OK) {
        fprintf(stderr, "expected model output is not valid JSON: %s\n", jerr.message);
        free(text);
        return 2;
    }
    (void)path;
    test_expected_columns(doc.root);
    test_plan_details();
    test_malformed();
    test_transform_docs();
    test_failure_injection();
    test_staleness();
    test_zero_rows();
    test_entity_worlds();
    test_snapshot03();
    test_digest_sensitivity();
    test_million_rows();
    printf("test_migrate: %lu checks, %lu failures\n", g_checks, g_fail);
    su_json_doc_free(&doc);
    free(text);
    return g_fail == 0 ? 0 : 1;
}
