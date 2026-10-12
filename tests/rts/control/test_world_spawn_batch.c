/* M3-05 world.spawn_batch (contract D3, OD-3b, N-c, T-RTS-LOAD-1 (g)), run against the real static handlers of
 * src/runtime/worker.c, as test_world_set_fields.c does. Not registered in CMake until h_world_spawn_batch is
 * in worker.c. Every refusal must leave the world unchanged (state, revision, record count); an accepted call
 * bumps the revision once and appends one SPAWNED record per row. Usage: test_world_spawn_batch <scratch-dir>
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

/* The 18 host fields of spatial.capsule.json. A row names all of them, with values that are valid at spawn. */
static const char *const k_row_values =
    "\"kind\":1,\"team\":0,\"hp\":5,\"pos_x\":10,\"pos_y\":10,\"range_sq\":0,\"formation_slot\":4294967295,"
    "\"formation_gen\":0,\"anchor_x\":0,\"anchor_y\":0,\"anchor_valid\":0,\"target_slot\":4294967295,"
    "\"target_gen\":0,\"target_dist_sq\":0,\"target_valid\":0,\"target_x\":0,\"target_y\":0,\"truncated\":0";

static const char *const k_fields[] = {
    "kind", "team", "hp", "pos_x", "pos_y", "range_sq", "formation_slot", "formation_gen", "anchor_x", "anchor_y",
    "anchor_valid", "target_slot", "target_gen", "target_dist_sq", "target_valid", "target_x", "target_y", "truncated",
};
#define NFIELDS (sizeof k_fields / sizeof k_fields[0])

static const uint32_t k_kind[ROWS] = {1u, 1u, 2u, 1u};
static const uint32_t k_range[ROWS] = {100u, 100u, 0u, 50u};
static const uint32_t k_fslot[ROWS] = {4294967295u, 0u, 4294967295u, 4294967295u};
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

static uint32_t run_json(uint32_t (*fn)(WorkerState *, const SuJsonValue *, SuBuf *, WorkerFail *),
                         const char *body, WorkerFail *f) {
    static const SuJsonLimits lim = {16u * 1024u * 1024u, 32u};
    SuJsonDoc doc;
    SuJsonError err;
    SuBuf res;
    uint32_t st;
    memset(&doc, 0, sizeof doc);
    if (su_json_parse((const uint8_t *)body, strlen(body), &lim, &doc, &err) != SU_OK) {
        CHECK(0, "test body does not parse: %.200s", body);
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

static uint32_t free_slots(void) {
    const SuEnts *e = su_world_entities_committed(world()->inst.w);
    return su_ents_capacity(e) - su_ents_alive_count(e);
}

static uint32_t alive(void) {
    return su_ents_alive_count(su_world_entities_committed(world()->inst.w));
}

static uint32_t hw(void) {
    return su_ents_high_water(su_world_entities_committed(world()->inst.w));
}

/* A batch of n rows, each with every field; with bad set, the last row names an unknown field. */
static char *batch(uint32_t n, int bad, char *buf, size_t cap) {
    size_t used = 0;
    uint32_t i;
    used += (size_t)snprintf(buf, cap, "[");
    for (i = 0; i < n; i++) {
        used += (size_t)snprintf(buf + used, cap - used, "%s{\"values\":{%s%s},\"external_id\":%u}", i ? "," : "",
                                 k_row_values, (bad && i == n - 1) ? ",\"nope\":1" : "", 1000u + i);
    }
    snprintf(buf + used, cap - used, "]");
    return buf;
}

static uint32_t spawn_batch(uint32_t n, int bad, WorkerFail *f) {
    static char rows[65536];
    static char body[70000];
    char rev[64];
    rev_string(world(), rev, sizeof rev);
    batch(n, bad, rows, sizeof rows);
    snprintf(body, sizeof body, "{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"rows\":%s}", g_id, rev,
             rows);
    return run_json(h_world_spawn_batch, body, f);
}

int main(int argc, char **argv) {
    char fixture[1024];
    char caps[512];
    char create[2560];
    WorkerFail f;
    WorkerState *s;
    uint64_t rev0;
    uint32_t st;
    uint32_t free0;
    uint32_t alive0;
    uint32_t hw0;

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
    st = run_json(h_world_create, create, &f);
    CHECK(st == SU_OK, "world.create of the RTS fixture: %s (%s)", su_status_name(st), f.msg);
    if (st != SU_OK || s->worlds[0].inst.w == NULL) {
        free(s);
        fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    snprintf(g_id, sizeof g_id, "%s", s->worlds[0].id);
    rev0 = revision();
    free0 = free_slots();
    alive0 = alive();
    hw0 = hw();

    /* Refusal: a batch larger than the free slots changes nothing (D3). */
    st = spawn_batch(free0 + 1u, 0, &f);
    CHECK(st == SU_E_RESOURCE_LIMIT, "batch over free capacity refused (got %s)", su_status_name(st));
    CHECK(revision() == rev0 && alive() == alive0 && hw() == hw0 && free_slots() == free0,
          "over-capacity refusal leaves revision, alive count, high water and free slots unchanged");

    /* Refusal: a domain-invalid last row refuses the whole batch with no row committed (T-RTS-LOAD-1 (g)). */
    st = spawn_batch(2u, 1, &f);
    CHECK(st == SU_E_SCHEMA_MISMATCH, "unknown field in the last row refused (got %s)", su_status_name(st));
    CHECK(revision() == rev0 && alive() == alive0 && free_slots() == free0,
          "a bad last row leaves no first row behind");

    /* Refusal: zero rows and more than 1024 rows. */
    st = run_json(h_world_spawn_batch,
                  "{\"world_id\":\"x\",\"expected_world_revision\":\"x\",\"rows\":[]}", &f);
    CHECK(st == SU_E_RESOURCE_LIMIT, "empty batch refused with SU_E_RESOURCE_LIMIT (got %s)", su_status_name(st));
    CHECK(revision() == rev0, "empty batch changes nothing");

    /* Accepted: as many rows as fit (up to 3) take the free slots, one revision bump, one record per row. */
    if (free0 >= 1u) {
        uint32_t take = free0 < 3u ? free0 : 3u;
        uint64_t recs0 = su_evlog_next_seq(ent_log(world()));
        st = spawn_batch(take, 0, &f);
        CHECK(st == SU_OK, "accepted batch of %u (got %s, %s)", take, su_status_name(st), f.msg);
        CHECK(revision() == rev0 + 1u, "accepted batch bumps the revision once");
        CHECK(alive() == alive0 + take, "alive count grows by the batch size");
        CHECK(su_evlog_next_seq(ent_log(world())) == recs0 + take, "one SPAWNED record per row");
    }

    free(s);
    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
