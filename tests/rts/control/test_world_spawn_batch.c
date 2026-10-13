/* M3-05 world.spawn_batch and the RTS spawn rules of world.spawn (contract 2.2 D1, D3, OD-3b), run against the
 * real static handlers of src/runtime/worker.c. The world is an RTS world built by the worker's own world.create
 * from capsules/rts/host/spatial.capsule.json and an eight-row fixture written by this test; four rows are then
 * despawned so the batch has free slots. Every refusal must leave the revision, the alive count, the high water
 * and the event log unchanged; an accepted batch bumps the revision once and appends one record per row.
 * Usage (repository root): test_world_spawn_batch <scratch-dir>
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

#define ROWS 8u
#define NONE_U32 4294967295u
#define GEN1 4294967296ull

/* The 18 host fields of spatial.capsule.json, the schema the fixture must match. */
static const char *const k_fields[] = {
    "kind", "team", "hp", "pos_x", "pos_y", "range_sq", "formation_slot", "formation_gen", "anchor_x", "anchor_y",
    "anchor_valid", "target_slot", "target_gen", "target_dist_sq", "target_valid", "target_x", "target_y", "truncated",
};
#define NFIELDS (sizeof k_fields / sizeof k_fields[0])

static char g_dir[512];
static WorkerState *g_s;
static char g_id[WORKER_ID_CAP];

/* Every fixture row is a unit with hp 10, no formation and no target. */
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
            if (strcmp(k_fields[k], "kind") == 0) v = 1u;
            if (strcmp(k_fields[k], "hp") == 0) v = 10u;
            if (strcmp(k_fields[k], "formation_slot") == 0 || strcmp(k_fields[k], "target_slot") == 0) v = NONE_U32;
            fprintf(fh, "%s%u", r ? "," : "", v);
        }
        fprintf(fh, "]");
    }
    fprintf(fh, "}}");
    fclose(fh);
}

/* Parses a request body and runs one handler; returns its status, the failure in f and the reply in out. */
static uint32_t run_json(uint32_t (*fn)(WorkerState *, const SuJsonValue *, SuBuf *, WorkerFail *),
                         const char *body, WorkerFail *f, char *out, size_t out_cap) {
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
    if (out != NULL && out_cap > 0u) {
        size_t n = res.len < out_cap - 1u ? res.len : out_cap - 1u;
        if (n > 0u) {
            memcpy(out, res.data, n);
        }
        out[n] = '\0';
    }
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

static const SuEnts *ents(void) {
    return su_world_entities_committed(world()->inst.w);
}

static uint64_t next_seq(void) {
    return su_evlog_next_seq(ent_log(world()));
}

/* The committed value of one field at one row. */
static uint32_t committed(const char *name, uint32_t row) {
    const uint32_t *data = NULL;
    uint64_t rows = 0;
    uint32_t id = field_id_by_name(world(), name, strlen(name));
    if (su_world_read_committed(world()->inst.w, id, &data, &rows) != SU_OK || row >= rows) {
        return 0xdeadbeefu;
    }
    return data[row];
}

/* Runs world.spawn_batch with the current revision unless expect is given. */
static uint32_t spawn_batch(const char *rows, const char *expect, WorkerFail *f, char *out, size_t out_cap) {
    char rev[64];
    char body[4096];
    const char *use = expect;
    if (use == NULL) {
        rev_string(world(), rev, sizeof rev);
        use = rev;
    }
    snprintf(body, sizeof body, "{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"rows\":%s}", g_id, use,
             rows);
    return run_json(h_world_spawn_batch, body, f, out, out_cap);
}

static uint32_t spawn_one(const char *values, WorkerFail *f) {
    char rev[64];
    char body[2048];
    rev_string(world(), rev, sizeof rev);
    snprintf(body, sizeof body, "{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"values\":%s}", g_id, rev,
             values);
    return run_json(h_world_spawn, body, f, NULL, 0u);
}

static uint32_t despawn(unsigned long long handle, WorkerFail *f) {
    char rev[64];
    char body[512];
    rev_string(world(), rev, sizeof rev);
    snprintf(body, sizeof body, "{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"entity\":%llu}", g_id,
             rev, handle);
    return run_json(h_world_despawn, body, f, NULL, 0u);
}

/* State a refused call must leave as it found. */
typedef struct Mark {
    uint64_t revision;
    uint32_t alive;
    uint32_t high_water;
    uint64_t seq;
} Mark;

static Mark mark(void) {
    Mark m;
    m.revision = revision();
    m.alive = su_ents_alive_count(ents());
    m.high_water = su_ents_high_water(ents());
    m.seq = next_seq();
    return m;
}

static int unchanged(Mark m) {
    Mark now = mark();
    return now.revision == m.revision && now.alive == m.alive && now.high_water == m.high_water && now.seq == m.seq;
}

/* A refused batch: expected status and no state change. */
#define REFUSED(rows, want, what)                                                                            \
    do {                                                                                                     \
        Mark m_ = mark();                                                                                    \
        uint32_t st_ = spawn_batch(rows, NULL, &f, NULL, 0u);                                                \
        CHECK(st_ == (want), "%s: want %s, got %s (%s)", what, su_status_name(want), su_status_name(st_), f.msg); \
        CHECK(unchanged(m_), "%s: refusal changes nothing", what);                                           \
    } while (0)

/* One valid unit row with the given extra members (a leading comma, or ""). */
#define UNIT(extra) "{\"values\":{\"kind\":1,\"hp\":5,\"pos_x\":7" extra "}}"

int main(int argc, char **argv) {
    char fixture[1024];
    char caps[512];
    char create[2560];
    char out[4096];
    WorkerFail f;
    WorkerState *s;
    Mark before;
    uint32_t st;
    uint32_t r;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s\n", g_dir);
        return 2;
    }
    snprintf(fixture, sizeof fixture, "%s/rts_spawn_batch_world.json", g_dir);
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
    st = run_json(h_world_create, create, &f, NULL, 0u);
    CHECK(st == SU_OK, "world.create of the RTS fixture: %s (%s)", su_status_name(st), f.msg);
    if (st != SU_OK || s->worlds[0].inst.w == NULL) {
        free(s);
        fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    snprintf(g_id, sizeof g_id, "%s", s->worlds[0].id);
    CHECK(su_ents_alive_count(ents()) == ROWS && su_ents_high_water(ents()) == ROWS, "fixture rows are all alive");

    /* A full world refuses one more row for capacity; the row is checked first. */
    st = spawn_one("{\"kind\":1,\"hp\":5}", &f);
    CHECK(st == SU_E_RESOURCE_LIMIT, "spawn into a full world: SU_E_RESOURCE_LIMIT (got %s)", su_status_name(st));
    st = spawn_one("{\"hp\":5}", &f);
    CHECK(st == SU_E_TYPE, "world.spawn without kind: SU_E_TYPE (got %s, %s)", su_status_name(st), f.msg);

    /* Free slots 4..7. */
    for (r = 4; r < ROWS; r++) {
        st = despawn(GEN1 + r, &f);
        CHECK(st == SU_OK, "despawn slot %u: %s (%s)", r, su_status_name(st), f.msg);
    }
    CHECK(su_ents_alive_count(ents()) == 4u, "four slots free");

    /* Shape and size refusals. */
    REFUSED("[]", SU_E_RESOURCE_LIMIT, "empty rows");
    REFUSED("{}", SU_E_TYPE, "rows not an array");
    REFUSED("[{\"external_id\":1}]", SU_E_TYPE, "row without values");
    REFUSED("[" UNIT("") ",{\"values\":{\"kind\":1,\"hp\":5},\"external_id\":4294967296}]", SU_E_TYPE,
            "external_id beyond u32");
    REFUSED("[" UNIT(",\"no_such_field\":1") "]", SU_E_SCHEMA_MISMATCH, "unknown field");

    /* D3 (T-RTS-LOAD-1 g): the last row over free capacity refuses the whole batch. */
    REFUSED("[" UNIT("") "," UNIT("") "," UNIT("") "," UNIT("") "," UNIT("") "]", SU_E_RESOURCE_LIMIT,
            "five rows into four free slots");
    /* D3: a domain-invalid last row refuses the whole batch. */
    REFUSED("[" UNIT("") "," UNIT("") "," UNIT("") ",{\"values\":{\"kind\":1,\"hp\":5,\"team\":2}}]", SU_E_TYPE,
            "team 2 in the last row");

    /* 2.2 domains and D1 rules at spawn. */
    REFUSED("[{\"values\":{\"hp\":5}}]", SU_E_TYPE, "kind is required");
    REFUSED("[{\"values\":{\"kind\":3}}]", SU_E_TYPE, "kind 3");
    REFUSED("[{\"values\":{\"kind\":1}}]", SU_E_TYPE, "kind 1 with hp 0");
    REFUSED("[{\"values\":{\"kind\":1,\"hp\":65536}}]", SU_E_TYPE, "hp above 65535");
    REFUSED("[{\"values\":{\"kind\":2,\"hp\":1}}]", SU_E_TYPE, "kind 2 with hp");
    REFUSED("[{\"values\":{\"kind\":2,\"range_sq\":1}}]", SU_E_TYPE, "kind 2 with range_sq");
    REFUSED("[" UNIT(",\"pos_y\":32768") "]", SU_E_TYPE, "pos_y above 32767");
    REFUSED("[" UNIT(",\"range_sq\":262145") "]", SU_E_TYPE, "range_sq above 262144");
    REFUSED("[" UNIT(",\"target_gen\":1") "]", SU_E_TYPE, "system-owned field not idle");
    REFUSED("[" UNIT(",\"target_slot\":0") "]", SU_E_TYPE, "target_slot 0 is a real slot, not idle");
    REFUSED("[" UNIT(",\"formation_gen\":1") "]", SU_E_TYPE, "formation_gen without a slot");
    REFUSED("[" UNIT(",\"formation_slot\":1") "]", SU_E_TYPE, "formation_slot without formation_gen");
    REFUSED("[" UNIT(",\"formation_slot\":1,\"formation_gen\":0") "]", SU_E_TYPE, "formation_gen 0 with a slot");
    REFUSED("[" UNIT(",\"formation_slot\":8,\"formation_gen\":1") "]", SU_E_TYPE, "formation_slot beyond capacity");

    /* Expected revision must match. */
    before = mark();
    st = spawn_batch("[" UNIT("") "]", "not-a-revision", &f, NULL, 0u);
    CHECK(st == SU_E_STALE_REVISION, "stale expected revision refused (got %s)", su_status_name(st));
    CHECK(unchanged(before), "stale revision refusal changes nothing");

    /* Accepted batch: lowest free slots in the given order, one bump, one record per row. */
    before = mark();
    st = spawn_batch("[{\"values\":{\"kind\":1,\"hp\":5,\"pos_x\":11},\"external_id\":901},"
                     "{\"values\":{\"kind\":2,\"team\":1,\"pos_x\":12}},"
                     "{\"values\":{\"kind\":1,\"hp\":65535,\"pos_x\":13,\"formation_slot\":5,\"formation_gen\":3}},"
                     "{\"values\":{\"kind\":1,\"hp\":1,\"pos_x\":14,\"target_slot\":4294967295}}]",
                     NULL, &f, out, sizeof out);
    CHECK(st == SU_OK, "valid batch accepted: %s (%s)", su_status_name(st), f.msg);
    CHECK(revision() == before.revision + 1u, "accepted batch bumps the revision once");
    CHECK(su_ents_alive_count(ents()) == ROWS, "four rows spawned");
    CHECK(next_seq() == before.seq + 4u, "one SU_EVENT_SPAWNED per row (%llu -> %llu)",
          (unsigned long long)before.seq, (unsigned long long)next_seq());
    for (r = 4; r < ROWS; r++) {
        CHECK(committed("pos_x", r) == 11u + (r - 4u), "row %u is in slot %u", r - 4u, r);
        CHECK(committed("target_slot", r) == NONE_U32, "slot %u target_slot defaults to NONE", r);
    }
    CHECK(committed("formation_slot", 4) == NONE_U32 && committed("formation_gen", 4) == 0u,
          "omitted formation_slot is NONE (D1)");
    CHECK(committed("formation_slot", 6) == 5u && committed("formation_gen", 6) == 3u, "named formation kept");
    CHECK(committed("kind", 5) == 2u && committed("team", 5) == 1u && committed("hp", 5) == 0u, "anchor row");
    CHECK(strstr(out, "\"spawned\":4") != NULL, "reply counts the rows: %s", out);
    CHECK(strstr(out, "\"entities\":[8589934596,8589934597,8589934598,8589934599]") != NULL,
          "reply lists the generation-2 handles in row order: %s", out);
    {
        SuEntity h = 0;
        uint32_t x = 0;
        CHECK(su_ents_handle_for_slot(ents(), 4u, &h) == SU_OK && su_ents_get_external(ents(), h, &x) == SU_OK &&
                  x == 901u,
              "external_id stored on slot 4");
    }

    /* Full again: a one-row batch refuses for capacity. */
    REFUSED("[" UNIT("") "]", SU_E_RESOURCE_LIMIT, "batch into a full world");

    free(s);
    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
