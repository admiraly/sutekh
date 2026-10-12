/* M3-04a T-HOST-11 (restore of a host entry) and the world.create host branch (load_schedule), run against the
 * real static functions of src/runtime/worker.c. This file includes the registry, the declaration module and
 * worker translation units with SU_TEST_HOST_REGISTRY defined, so the test-only registry hook is available.
 * Usage (repository root): test_host_restore <scratch-dir>
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

#include "host_test_ops.h"

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

static char g_dir[512];
static RevPaths g_paths[SU_SCHED_MAX_SYSTEMS];
static CkptRec g_rec;

static void write_host_capsule(const char *path) {
    char hex[65];
    FILE *fh;
    su_sha256_hex(g_test_vel_bump.constants_hash, hex);
    fh = fopen(path, "wb");
    if (fh == NULL) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fprintf(fh,
            "{\"capsule_format\":\"0.1\",\"system_id\":\"test.host_vel_bump\",\"system_rev\":1,\"capabilities\":[],"
            "\"schema\":{\"id\":\"demo.actor\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"u32_mod_v1\","
            "\"host\":{\"operator\":\"rts_ops_v1\",\"operator_version\":1,\"constants_hash\":\"%s\",\"fields\":"
            "[{\"id\":2,\"name\":\"velocity_x\",\"type\":\"u32\",\"access\":\"read_write\"}]},"
            "\"schedule\":{\"phase\":\"rts\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
            "\"limits\":{\"max_instructions\":1,\"max_rows\":1000000,\"allocation\":\"forbidden\"},"
            "\"tests\":[\"T-HOST-11\"]}",
            hex);
    fclose(fh);
}

/* Parses a capsules array and loads it with the worker's own world.create loader. */
static uint32_t load_world(const char *caps_text, SuSchedule *sched, WorkerFail *f) {
    static const SuJsonLimits lim = {16u * 1024u * 1024u, 32u};
    SuJsonDoc doc;
    SuJsonError err;
    uint32_t st;
    memset(&doc, 0, sizeof doc);
    if (su_json_parse((const uint8_t *)caps_text, strlen(caps_text), &lim, &doc, &err) != SU_OK) {
        return SU_E_PARSE;
    }
    st = load_schedule(doc.root, sched, g_paths, f);
    su_json_doc_free(&doc);
    return st;
}

/* Checkpoint record of an installed schedule (what ckpt_record writes). */
static void build_record(const SuSchedule *sched) {
    uint32_t i;
    memset(&g_rec, 0, sizeof g_rec);
    g_rec.count = sched->count;
    for (i = 0; i < sched->count; i++) {
        CkptSys *c = &g_rec.sys[i];
        snprintf(c->system_id, sizeof c->system_id, "%s", sched->sys[i].system_id);
        snprintf(c->semantic_hash, sizeof c->semantic_hash, "%s", sched->sys[i].semantic_hash);
        snprintf(c->source_hash, sizeof c->source_hash, "%s", sched->sys[i].source_hash);
        snprintf(c->capsule_hash, sizeof c->capsule_hash, "%s", sched->sys[i].capsule_hash);
        c->kind = sched->sys[i].kind;
        c->paths = g_paths[i];
    }
}

/* Rebuilds the schedule from g_rec with a snapshot manifest that lists the same revisions. */
static uint32_t rebuild_from_record(SuSchedule *out, WorkerFail *f, const SuSchedule *sched) {
    static SuSnapshotManifestEntry man[SU_SCHED_MAX_SYSTEMS];
    SuSnapshotData data;
    uint32_t i;
    memset(&data, 0, sizeof data);
    memset(man, 0, sizeof man);
    for (i = 0; i < sched->count; i++) {
        man[i].system_id = sched->sys[i].system_id;
        snprintf(man[i].semantic_hash, sizeof man[i].semantic_hash, "%s", sched->sys[i].semantic_hash);
    }
    data.manifest = man;
    data.manifest_count = sched->count;
    return rebuild_schedule(&g_rec, &data, out, f);
}

int main(int argc, char **argv) {
    static const SuHostOpDesc *const k_list[] = {&g_test_vel_bump, NULL};
    char host_path[600];
    char caps[1024];
    char saved_hash[65];
    SuSchedule sched;
    SuSchedule re;
    WorkerFail f;
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
    test_host_ops_init();
    snprintf(host_path, sizeof host_path, "%s/host_restore.capsule.json", g_dir);
    write_host_capsule(host_path);
    snprintf(caps, sizeof caps,
             "[{\"capsule_path\":\"%s\",\"host_operator\":\"test.host_vel_bump\"},"
             "{\"capsule_path\":\"examples/movement.capsule.json\",\"source_path\":\"examples/movement.lir.json\"}]",
             host_path);

    /* world.create with a host item, the registry holding the test descriptor. */
    su_host_registry_test_set(k_list);
    su_sched_init(&sched);
    memset(&f, 0, sizeof f);
    st = load_world(caps, &sched, &f);
    CHECK(st == SU_OK, "world.create with a host item loads: %s (%s)", su_status_name(st), f.msg);
    CHECK(sched.count == 2 && sched.sys[0].kind == SU_SYS_KIND_HOST && sched.sys[1].kind == SU_SYS_KIND_IR,
          "host entry is entry 0, IR entry 1");
    CHECK(g_paths[0].source[0] == '\0' && g_paths[0].capsule[0] != '\0', "host paths: capsule set, source empty");
    CHECK(sched.count == 2 && sched.sys[0].source_hash[0] != '\0' && sched.sys[0].semantic_hash[0] != '\0' &&
              sched.sys[0].capsule_hash[0] != '\0',
          "host entry hashes are non-empty");

    /* T-HOST-11: restore rebuilds the host entry from the registry and its capsule (paths.source unused). */
    build_record(&sched);
    snprintf(saved_hash, sizeof saved_hash, "%s", g_rec.sys[0].capsule_hash);
    su_sched_init(&re);
    memset(&f, 0, sizeof f);
    st = rebuild_from_record(&re, &f, &sched);
    CHECK(st == SU_OK, "restore rebuilds the host entry: %s (%s)", su_status_name(st), f.msg);
    CHECK(re.count == 2 && re.sys[0].kind == SU_SYS_KIND_HOST, "rebuilt schedule keeps the host entry");
    CHECK(re.count == 2 && strcmp(re.sys[0].semantic_hash, sched.sys[0].semantic_hash) == 0 &&
              strcmp(re.sys[0].capsule_hash, sched.sys[0].capsule_hash) == 0,
          "restored semantic and capsule hashes equal the recorded ones");
    su_sched_free(&re);

    /* The recorded capsule hash must still match the file: a changed capsule does not restore. */
    snprintf(g_rec.sys[0].capsule_hash, sizeof g_rec.sys[0].capsule_hash, "%s", "00");
    su_sched_init(&re);
    memset(&f, 0, sizeof f);
    st = rebuild_from_record(&re, &f, &sched);
    CHECK(st == SU_E_CANDIDATE_FAILED, "restore refuses a host capsule whose bytes differ (got %s)",
          su_status_name(st));
    CHECK(re.count == 0, "failed restore installs nothing");
    su_sched_free(&re);
    snprintf(g_rec.sys[0].capsule_hash, sizeof g_rec.sys[0].capsule_hash, "%s", saved_hash);

    /* A checkpoint whose host descriptor is no longer registered does not restore (r4, no fallback). */
    su_host_registry_test_set(NULL);
    su_sched_init(&re);
    memset(&f, 0, sizeof f);
    st = rebuild_from_record(&re, &f, &sched);
    CHECK(st == SU_E_SCHEMA_MISMATCH, "unregistered host descriptor: SU_E_SCHEMA_MISMATCH (got %s)",
          su_status_name(st));
    su_sched_free(&re);

    /* world.create with a host item and no registered descriptor is refused. */
    su_sched_free(&sched);
    su_sched_init(&sched);
    memset(&f, 0, sizeof f);
    st = load_world(caps, &sched, &f);
    CHECK(st == SU_E_CAPABILITY_DENIED && sched.count == 0,
          "world.create host item without a registered operator is refused (got %s)", su_status_name(st));
    su_sched_free(&sched);

    printf("test_host_restore: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
