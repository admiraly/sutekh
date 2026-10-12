/* M3-05 world.set_fields (contract 2.2, OD-3), run against the real static handlers of src/runtime/worker.c.
 * The world is an RTS world built by the worker's own world.create from capsules/rts/host/spatial.capsule.json
 * and a four-row fixture written by this test. Every refusal must leave the world unchanged; an accepted call
 * bumps the revision once. Usage (repository root): test_world_set_fields <scratch-dir>
 */
#define SU_TEST_HOST_REGISTRY 1
#include "../../../src/runtime/rts/host_registry.c"
#include "../../../src/runtime/rts/host_decl.c"
#include "../../../src/runtime/worker.c"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_checks++;                                       \
        if (!(cond)) {                                    \
            g_failures++;                                 \
            fprintf(stderr, "FAIL line %d: ", __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                 \
            fprintf(stderr, "\n");                        \
        }                                                 \
    } while (0)

#define ROWS 4u
#define NONE_U32 4294967295u

/* The 18 host fields of spatial.capsule.json, the schema the fixture must match. */
static const char *const k_fields[] = {
    "kind", "team", "hp", "pos_x", "pos_y", "range_sq", "formation_slot", "formation_gen", "anchor_x", "anchor_y",
    "anchor_valid", "target_slot", "target_gen", "target_dist_sq", "target_valid", "target_x", "target_y", "truncated",
};
#define NFIELDS (sizeof k_fields / sizeof k_fields[0])

/* Fixture row r is slot r with generation 1, so its live handle is 4294967296 + r. Rows 0..3: unit, unit in a
 * formation, anchor (kind 2), unit. */
static const uint32_t k_kind[ROWS] = {1u, 1u, 2u, 1u};
static const uint32_t k_range[ROWS] = {100u, 100u, 0u, 50u};
static const uint32_t k_fslot[ROWS] = {NONE_U32, 0u, NONE_U32, NONE_U32};
static const uint32_t k_fgen[ROWS] = {0u, 1u, 0u, 0u};

static char g_dir[512];
static WorkerState *g_s;
static char g_id[WORKER_ID_CAP];

static void write_fixture(const char *path) {
    FILE *fh = fopen(path, "wb");
    uint32_t r;
    size_t k;
    if (fh == NULL) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fprintf(fh, "{\"fixture_format\":\"0.1\",\"schema_id\":\"rts\",\"schema_version\":1,\"tick\":0,\"entity_ids\":[");
    for (r = 0; r < ROWS; r++) {
        fprintf(fh, "%s%u", r ? "," : "", r + 1u);
    }
    fprintf(fh, "],\"columns\":{");
    for (k = 0; k < NFIELDS; k++) {
        fprintf(fh, "%s\"%s\":[", k ? "," : "", k_fields[k]);
        for (r = 0; r < ROWS; r++) {
            uint32_t v = 0;
            if (strcmp(k_fields[k], "kind") == 0) v = k_kind[r];
            if (strcmp(k_fields[k], "range_sq") == 0) v = k_range[r];
            if (strcmp(k_fields[k], "formation_slot") == 0) v = k_fslot[r];
            if (strcmp(k_fields[k], "formation_gen") == 0) v = k_fgen[r];
            fprintf(fh, "%s%u", r ? "," : "", v);
        }
        fprintf(fh, "]");
    }
    fprintf(fh, "}}");
    fclose(fh);
}

/* Parses a request body and runs one handler; returns its status and the failure detail in f. */
static uint32_t run_json(uint32_t (*fn)(WorkerState *, const SuJsonValue *, SuBuf *, WorkerFail *),
                         const char *body, WorkerFail *f) {
    static const SuJsonLimits lim = {16u * 1024u * 1024u, 32u};
    SuJsonDoc doc;
    SuJsonError err;
    SuBuf res;
    uint32_t st;
    memset(&doc, 0, sizeof doc);
    if (su_json_parse((const uint8_t *)body, strlen(body), &lim, &doc, &err) != SU_OK) {
        CHECK(0, "test body does not parse: %s", body);
        return SU_E_PARSE;
    }
    su_buf_init(&res);
    memset(f, 0, sizeof *f);
    st = fn(g_s, doc.root, &res, f);
    su_buf_free(&res);
    su_json_doc_free(&doc);
    return st;
}

static WorkerWorld *world(void) {
    return find_world(g_s, g_id);
}

static uint64_t revision(void) {
    return world()->revision;
}

/* The committed value of one field at one row; the caller checks the status. */
static uint32_t committed(const char *name, uint32_t row) {
    const uint32_t *data = NULL;
    uint64_t rows = 0;
    uint32_t id = field_id_by_name(world(), name, strlen(name));
    if (su_world_read_committed(world()->inst.w, id, &data, &rows) != SU_OK || row >= rows) {
        return 0xdeadbeefu;
    }
    return data[row];
}

/* Runs world.set_fields with the current revision unless expect is given. */
static uint32_t set_fields(const char *entities, const char *expect, WorkerFail *f) {
    char rev[64];
    char body[2048];
    const char *use = expect;
    if (use == NULL) {
        rev_string(world(), rev, sizeof rev);
        use = rev;
    }
    snprintf(body, sizeof body, "{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"entities\":%s}", g_id, use,
             entities);
    return run_json(h_world_set_fields, body, f);
}

int main(int argc, char **argv) {
    char fixture[1024];
    char caps[512];
    char create[2560];
    WorkerFail f;
    WorkerState *s;
    uint64_t rev0;
    uint32_t st;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s\n", g_dir);
        return 2;
    }
    snprintf(fixture, sizeof fixture, "%s/rts_control_world.json", g_dir);
    write_fixture(fixture);

    s = calloc(1, sizeof *s);
    if (s == NULL) {
        fprintf(stderr, "cannot allocate worker state\n");
        return 2;
    }
    g_s = s;
    snprintf(caps, sizeof caps, "[{\"capsule_path\":\"capsules/rts/host/spatial.capsule.json\","
                                "\"host_operator\":\"rts.spatial\"}]");
    snprintf(create, sizeof create, "{\"fixture_path\":\"%s\",\"capsules\":%s}", fixture, caps);
    st = run_json(h_world_create, create, &f);
    CHECK(st == SU_OK, "world.create of the RTS fixture: %s (%s)", su_status_name(st), f.msg);
    if (st != SU_OK || s->worlds[0].inst.w == NULL) {
        free(s);
        fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    snprintf(g_id, sizeof g_id, "%s", s->worlds[0].id);
    rev0 = revision();
    CHECK(committed("range_sq", 0) == 100u && committed("kind", 2) == 2u, "fixture rows load as written");

    /* Valid call: two rows, one bump of the revision, committed values change. */
    st = set_fields("[{\"entity\":4294967296,\"values\":{\"range_sq\":200}},"
                    "{\"entity\":4294967297,\"values\":{\"formation_slot\":3,\"formation_gen\":5}}]",
                    NULL, &f);
    CHECK(st == SU_OK, "valid set_fields accepted: %s (%s)", su_status_name(st), f.msg);
    CHECK(revision() == rev0 + 1u, "accepted call bumps the revision once (%llu -> %llu)",
          (unsigned long long)rev0, (unsigned long long)revision());
    CHECK(committed("range_sq", 0) == 200u, "range_sq written on slot 1");
    CHECK(committed("formation_slot", 1) == 3u && committed("formation_gen", 1) == 5u,
          "formation fields written on slot 2");
    rev0 = revision();

    /* Refusals change nothing and do not bump the revision. */
    st = set_fields("[{\"entity\":4294967296,\"values\":{\"kind\":2}}]", NULL, &f);
    CHECK(st == SU_E_TYPE, "kind is not writable: SU_E_TYPE (got %s)", su_status_name(st));
    CHECK(committed("kind", 0) == 1u && revision() == rev0, "refused write leaves kind and revision unchanged");

    st = set_fields("[{\"entity\":4294967296,\"values\":{\"range_sq\":4294967295}}]", NULL, &f);
    CHECK(st == SU_E_TYPE, "out-of-domain range_sq refused (got %s)", su_status_name(st));
    CHECK(committed("range_sq", 0) == 200u && revision() == rev0, "out-of-domain refusal changes nothing");

    st = set_fields("[{\"entity\":4294967296,\"values\":{\"range_sq\":10}},{\"entity\":4294967296,\"values\":{\"range_sq\":20}}]", NULL,
                    &f);
    CHECK(st == SU_E_INVALID_ARGUMENT, "duplicate entity refused (got %s)", su_status_name(st));
    CHECK(committed("range_sq", 0) == 200u && revision() == rev0, "duplicate refusal changes nothing");

    /* Generation 2 of slot 0 is not live: the fixture rows are generation 1 (SU_GEN_FIRST). */
    st = set_fields("[{\"entity\":8589934592,\"values\":{\"range_sq\":10}}]", NULL, &f);
    CHECK(st == SU_E_STALE_HANDLE, "stale handle refused (got %s)", su_status_name(st));
    CHECK(revision() == rev0, "stale refusal changes nothing");

    /* Resulting-row rule: an anchor (kind 2) keeps range_sq at 0. */
    st = set_fields("[{\"entity\":4294967298,\"values\":{\"range_sq\":50}}]", NULL, &f);
    CHECK(st == SU_E_TYPE, "kind-2 row with nonzero range_sq refused (got %s)", su_status_name(st));
    CHECK(committed("range_sq", 2) == 0u && revision() == rev0, "kind-2 refusal changes nothing");

    /* Resulting-row rule: formation_gen is 0 exactly when formation_slot is NONE. */
    st = set_fields("[{\"entity\":4294967296,\"values\":{\"formation_gen\":7}}]", NULL, &f);
    CHECK(st == SU_E_TYPE, "NONE slot with nonzero formation_gen refused (got %s)", su_status_name(st));
    CHECK(revision() == rev0, "formation_gen refusal changes nothing");

    /* formation_slot must be below the world's capacity (the four rows here). */
    st = set_fields("[{\"entity\":4294967296,\"values\":{\"formation_slot\":4,\"formation_gen\":1}}]", NULL, &f);
    CHECK(st == SU_E_TYPE, "formation_slot beyond capacity refused (got %s)", su_status_name(st));
    CHECK(committed("formation_slot", 0) == NONE_U32 && revision() == rev0, "capacity refusal changes nothing");

    /* Expected revision must match the active world. */
    st = set_fields("[{\"entity\":4294967296,\"values\":{\"range_sq\":10}}]", "not-a-revision", &f);
    CHECK(st == SU_E_STALE_REVISION, "stale expected revision refused (got %s)", su_status_name(st));

    /* A legal formation write on a unit (slot 4) after the refusals. */
    st = set_fields("[{\"entity\":4294967299,\"values\":{\"formation_slot\":2,\"formation_gen\":9}}]", NULL, &f);
    CHECK(st == SU_OK, "formation write on a unit accepted (got %s, %s)", su_status_name(st), f.msg);
    CHECK(committed("formation_slot", 3) == 2u && committed("formation_gen", 3) == 9u,
          "formation fields written on slot 4");

    free(s);
    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
