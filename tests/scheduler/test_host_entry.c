/* M3-04a host-entry tests (contract M3_RTS_CONTRACT section 10.7, T-HOST-01..10).
 *
 * Runs in the repository root (CTest WORKING_DIRECTORY) with the fixture paths below. Host operators are
 * the test-only descriptors of tests/rts/host (never registered in the product build). Expected values are
 * computed here from the row semantics: movement adds velocity to position, the host operator adds 1 to
 * velocity, damp subtracts the moved position from velocity, dead rows stay zero.
 *
 * Usage: test_host_entry <scratch-dir>   (created if missing; receives generated capsule variants)
 */
#define _POSIX_C_SOURCE 200809L

#include "platform/su_posix.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "runtime/jobs/su_effects.h"
#include "runtime/reload.h"
#include "runtime/rts/host_decl.h"
#include "runtime/schedule.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"
#include "world/su_world.h"

#include "../rts/host/host_test_ops.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_checks++;                                       \
        if (!(cond)) {                                    \
            g_failures++;                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                 \
            fprintf(stderr, "\n");                        \
        }                                                 \
    } while (0)

#define ROWS 3u   /* alive rows */
#define CAP 5u    /* capacity: rows 3 and 4 start dead */
#define TICKS 4u

static const uint32_t k_pos0[CAP] = {5u, 4294967295u, 10u, 0u, 0u};
static const uint32_t k_vel0[CAP] = {7u, 1u, 20u, 0u, 0u};

/* ---------- world and reference ---------- */

static SuWorld *make_world(uint64_t rows, uint64_t cap) {
    SuWorldFieldDesc fd[2];
    SuWorldDesc d;
    SuWorld *w = NULL;
    uint32_t ids[CAP];
    uint64_t i;
    fd[0].id = 1u;
    fd[0].type = SU_TYPE_U32;
    fd[0].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
    fd[0].name = "position_x";
    fd[1].id = 2u;
    fd[1].type = SU_TYPE_U32;
    fd[1].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
    fd[1].name = "velocity_x";
    for (i = 0; i < rows; i++) {
        ids[i] = (uint32_t)(i + 1u);
    }
    d.schema_id = "demo.actor";
    d.schema_version = 1u;
    d.layout_generation = 1u;
    d.row_count = rows;
    d.entity_ids = ids;
    d.fields = fd;
    d.field_count = 2u;
    if (su_world_create_ex(&d, cap, &w) != SU_OK) {
        return NULL;
    }
    if (su_world_init_committed(w, 1u, k_pos0, rows) != SU_OK || su_world_init_committed(w, 2u, k_vel0, rows) != SU_OK) {
        su_world_destroy(w);
        return NULL;
    }
    return w;
}

/* Reference after n ticks: alive rows only; dead rows stay zero (canonical zeroing). */
static void reference(uint32_t n_ticks, uint32_t pos[CAP], uint32_t vel[CAP]) {
    uint32_t t;
    uint32_t r;
    for (r = 0; r < CAP; r++) {
        pos[r] = r < ROWS ? k_pos0[r] : 0u;
        vel[r] = r < ROWS ? k_vel0[r] : 0u;
    }
    for (t = 0; t < n_ticks; t++) {
        for (r = 0; r < ROWS; r++) {
            uint32_t p_next = pos[r] + vel[r];        /* movement */
            uint32_t v1 = vel[r] + 1u;                /* host: test.host_vel_bump */
            vel[r] = v1 - p_next;                     /* damp: v - p, with p after movement */
            pos[r] = p_next;
        }
    }
}

static int committed_matches(const SuWorld *w, uint32_t n_ticks) {
    uint32_t pos[CAP];
    uint32_t vel[CAP];
    const uint32_t *pc = NULL;
    const uint32_t *vc = NULL;
    uint64_t rows = 0;
    uint32_t r;
    reference(n_ticks, pos, vel);
    if (su_world_read_committed(w, 1u, &pc, &rows) != SU_OK || su_world_read_committed(w, 2u, &vc, &rows) != SU_OK) {
        return 0;
    }
    if (rows != CAP) {
        return 0;
    }
    for (r = 0; r < CAP; r++) {
        if (pc[r] != pos[r] || vc[r] != vel[r]) {
            return 0;
        }
    }
    return 1;
}

/* ---------- schedule construction ---------- */

static uint32_t load_ir(const char *cap, const char *src, SuLirModule **mod, uint32_t *sv, uint32_t *gen) {
    char msg[320];
    return su_reload_load(cap, src, NULL, NULL, mod, sv, gen, msg, sizeof msg);
}

static SuLirModule *host_decl_of(const SuHostOpDesc *d) {
    SuLirModule *m = calloc(1u, sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    if (su_host_decl_build(d, m) != SU_OK) {
        free(m);
        return NULL;
    }
    return m;
}

/* movement (IR, pos += vel), test.host_vel_bump (host), pipe.damp (IR, vel = vel - pos). */
static uint32_t build_std(SuSchedule *s, TestHostCtx *ctx) {
    SuLirModule *m1 = NULL;
    SuLirModule *m3 = NULL;
    SuLirModule *dh = NULL;
    uint32_t sv = 0;
    uint32_t gen = 0;
    uint32_t st;
    su_sched_init(s);
    g_test_vel_bump.host_ctx = ctx;
    st = load_ir("examples/movement.capsule.json", "examples/movement.lir.json", &m1, &sv, &gen);
    if (st != SU_OK) {
        return st;
    }
    st = su_sched_add(s, m1, sv, gen, 0, 1, 0);
    if (st != SU_OK) {
        su_reload_module_free(m1);
        return st;
    }
    dh = host_decl_of(&g_test_vel_bump);
    if (dh == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_sched_add_host(s, &g_test_vel_bump, dh, 1u, 1u, 0, 1, 0);
    if (st != SU_OK) {
        su_host_decl_free(dh);
        return st;
    }
    st = load_ir("tests/scheduler/fixtures/pipe/damp.capsule.json", "tests/scheduler/fixtures/pipe/damp.lir.json",
                 &m3, &sv, &gen);
    if (st != SU_OK) {
        return st;
    }
    st = su_sched_add(s, m3, sv, gen, 0, 1, 0);
    if (st != SU_OK) {
        su_reload_module_free(m3);
        return st;
    }
    return su_sched_finalize(s);
}

/* ---------- T-HOST-01, 02, 10 ---------- */

static void test_01_ir_unchanged(void) {
    SuLirModule *m = NULL;
    uint32_t sv = 0;
    uint32_t gen = 0;
    SuSchedule s;
    SuSysEntry *e;
    CHECK(load_ir("examples/movement.capsule.json", "examples/movement.lir.json", &m, &sv, &gen) == SU_OK,
          "movement loads");
    su_sched_init(&s);
    CHECK(su_sched_add(&s, m, sv, gen, 0, 1, 0) == SU_OK, "IR add");
    e = &s.sys[0];
    CHECK(e->kind == SU_SYS_KIND_IR && e->host == NULL && e->ctx != NULL, "IR entry keeps kind IR and a VM context");
    su_sched_free(&s);
}

static void test_02_dispatch_commit(void) {
    SuWorld *w = make_world(ROWS, CAP);
    SuSchedule s;
    TestHostCtx ctx;
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;
    memset(&ctx, 0, sizeof ctx);
    ctx.fail_at = UINT32_MAX;
    CHECK(w != NULL, "world");
    if (w == NULL) {
        return;
    }
    st = build_std(&s, &ctx);
    CHECK(st == SU_OK, "build [IR, host, IR]: %s", su_status_name(st));
    CHECK(s.count == 3 && s.sys[1].kind == SU_SYS_KIND_HOST, "entry 1 is the host entry");
    st = su_sched_run_ticks(w, &s, TICKS, NULL, NULL, &done, &fi);
    CHECK(st == SU_OK && done == TICKS, "run %u ticks: %s", (unsigned)TICKS, su_status_name(st));
    CHECK(ctx.calls == TICKS, "host ran once per tick (calls %u)", (unsigned)ctx.calls);
    CHECK(ctx.bad_views == 0, "host views are exactly its declared bindings (bad %u)", (unsigned)ctx.bad_views);
    CHECK(committed_matches(w, TICKS), "committed state equals the reference (only declared writes, dead rows zero)");
    su_sched_free(&s);
    su_world_destroy(w);
}

static void test_10_workers(void) {
    static const uint32_t k_workers[2] = {1u, 2u};
    uint32_t k;
    for (k = 0; k < 2; k++) {
        SuWorld *w = make_world(ROWS, CAP);
        SuSchedule s;
        TestHostCtx ctx;
        SuSchedTickOpts o;
        uint64_t done = 0;
        uint32_t fi = 0;
        uint32_t st;
        memset(&ctx, 0, sizeof ctx);
        ctx.fail_at = UINT32_MAX;
        memset(&o, 0, sizeof o);
        o.workers = k_workers[k];
        o.chunk_min_rows = 1u;
        if (w == NULL) {
            CHECK(0, "world for workers test");
            continue;
        }
        st = build_std(&s, &ctx);
        CHECK(st == SU_OK, "build for workers=%u", (unsigned)k_workers[k]);
        st = su_sched_run_ticks_opt(w, &s, TICKS, NULL, NULL, &o, &done, &fi);
        CHECK(st == SU_OK && done == TICKS, "workers=%u run: %s", (unsigned)k_workers[k], su_status_name(st));
        CHECK(committed_matches(w, TICKS), "workers=%u digest equals the serial reference", (unsigned)k_workers[k]);
        CHECK(ctx.calls == TICKS, "workers=%u host serial, once per tick", (unsigned)k_workers[k]);
        su_sched_free(&s);
        su_world_destroy(w);
    }
}

/* ---------- T-HOST-03 failure ---------- */

static void test_03_failure(void) {
    SuWorld *w = make_world(ROWS, CAP);
    SuSchedule s;
    TestHostCtx ctx;
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;
    memset(&ctx, 0, sizeof ctx);
    ctx.fail_at = 1u;
    ctx.fail_status = SU_E_INTERNAL;
    if (w == NULL) {
        CHECK(0, "world for failure test");
        return;
    }
    CHECK(build_std(&s, &ctx) == SU_OK, "build failure schedule");
    st = su_sched_run_ticks(w, &s, TICKS, NULL, NULL, &done, &fi);
    CHECK(st == SU_E_INTERNAL, "host failure aborts the tick with SU_E_INTERNAL (got %s)", su_status_name(st));
    CHECK(fi == 1u, "failed_index is the host index (got %u)", (unsigned)fi);
    CHECK(done == 1u, "one tick published before the failure (got %u)", (unsigned)done);
    CHECK(committed_matches(w, 1u), "committed root is the state after tick 0 (failed tick not committed)");
    su_sched_free(&s);
    su_world_destroy(w);
}

/* ---------- T-HOST-04 declared effects ---------- */

static void test_04_effects(void) {
    SuLirModule *mv = NULL;
    SuLirModule *mpos = NULL;
    SuLirModule *mbad = NULL;
    uint32_t sv = 0;
    uint32_t gen = 0;
    SuEffectInput in[2];
    SuEffectGraph *g = NULL;
    char err[256];
    const char *after[1] = {"demo.movement"};
    uint32_t st;

    CHECK(load_ir("examples/movement.capsule.json", "examples/movement.lir.json", &mv, &sv, &gen) == SU_OK, "movement");
    mpos = host_decl_of(&g_test_pos_writer);
    CHECK(mpos != NULL, "pos writer decl");
    if (mv == NULL || mpos == NULL) {
        return;
    }
    memset(in, 0, sizeof in);
    in[0].system_id = "demo.movement";
    in[0].mod = mv;
    in[0].phase = "rts";
    in[0].declared_parallel = 1u;
    in[1].system_id = TEST_HOST_POS_ID;
    in[1].mod = mpos;
    in[1].phase = "rts";
    in[1].declared_parallel = 1u;
    in[1].host_declared = 1u;

    /* Host writes position_x, movement writes it too, no ordering edge: refused. */
    st = su_effects_build(in, 2, &g, err, sizeof err);
    CHECK(st == SU_E_SCHEMA_MISMATCH, "host write conflicts with unordered IR write (got %s)", su_status_name(st));
    CHECK(g == NULL, "no graph on refusal");

    /* With the edge, accepted. */
    in[1].after = after;
    in[1].after_count = 1u;
    st = su_effects_build(in, 2, &g, err, sizeof err);
    CHECK(st == SU_OK, "ordered host write accepted (got %s: %s)", su_status_name(st), err);
    su_effects_free(g);
    g = NULL;

    /* An IR entry with a declared/derived mismatch is still refused when host_declared is 0. */
    if (load_ir("examples/movement.capsule.json", "examples/movement.lir.json", &mbad, &sv, &gen) == SU_OK) {
        SuEffectInput one;
        mbad->fields[1].access |= SU_ACCESS_WRITE; /* declares a write that the IR never stores */
        memset(&one, 0, sizeof one);
        one.system_id = "demo.movement";
        one.mod = mbad;
        one.phase = "rts";
        st = su_effects_build(&one, 1, &g, err, sizeof err);
        CHECK(st == SU_E_SCHEMA_MISMATCH, "IR declared/derived mismatch still refused (got %s)", su_status_name(st));
        su_effects_free(g);
        g = NULL;
        su_reload_module_free(mbad);
    }
    su_reload_module_free(mv);
    su_host_decl_free(mpos);
}

/* ---------- T-HOST-05 load checks ---------- */

static char g_dir[512];

static void write_text(const char *name, const char *text) {
    char path[600];
    FILE *fh;
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    fh = fopen(path, "wb");
    if (fh == NULL) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fwrite(text, 1, strlen(text), fh);
    fclose(fh);
}

static void path_of(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", g_dir, name);
}

static void hash_hex_of(const SuHostOpDesc *d, char out[65]) {
    su_sha256_hex(d->constants_hash, out);
}

#define CAPSULE_TEMPLATE \
    "{\"capsule_format\":\"0.1\",\"system_id\":\"%s\",\"system_rev\":1,\"capabilities\":[]," \
    "\"schema\":{\"id\":\"demo.actor\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"%s\"," \
    "%s\"host\":{\"operator\":\"%s\",\"operator_version\":%u,\"constants_hash\":\"%s\",\"fields\":%s}," \
    "\"limits\":{\"max_instructions\":1,\"max_rows\":1000000,\"allocation\":\"forbidden\"}," \
    "\"tests\":[\"T-HOST-05\"]%s}"

static const char k_sched[] = "\"schedule\":{\"phase\":\"rts\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},";
static const char k_fields_ok[] = "[{\"id\":2,\"name\":\"velocity_x\",\"type\":\"u32\",\"access\":\"read_write\"}]";

struct Variant {
    const char *file;
    const char *system_id;
    const char *profile;
    const char *sched;
    const char *operator_name;
    unsigned op_version;
    const char *hash;       /* NULL: the descriptor's hash */
    const char *fields;
    const char *extra;
    const char *pin;        /* NULL: no pin */
    uint32_t expect;
    const char *label;
};

static void test_05_load(void) {
    static const char *const kBadHash = "0000000000000000000000000000000000000000000000000000000000000000";
    char good_hash[65];
    char path[600];
    char text[4096];
    SuHostOpDesc *d = &g_test_vel_bump;
    SuSchedule s;
    SuLirModule *out = NULL;
    uint32_t sv = 0;
    uint32_t gen = 0;
    char msg[320];
    uint32_t st;
    size_t i;
    struct Variant v[] = {
        {"ok.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL, k_fields_ok, "", NULL, SU_OK, "valid"},
        {"two_fields.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL,
         "[{\"id\":1,\"name\":\"position_x\",\"type\":\"u32\",\"access\":\"write\"},{\"id\":2,\"name\":\"velocity_x\",\"type\":\"u32\",\"access\":\"read_write\"}]",
         "", NULL, SU_E_SCHEMA_MISMATCH, "extra write field"},
        {"trailing.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL,
         "[{\"id\":2,\"name\":\"velocity_x\",\"type\":\"u32\",\"access\":\"read_write\"},{\"id\":3,\"name\":\"extra\",\"type\":\"u32\",\"access\":\"write\"}]",
         "", NULL, SU_E_SCHEMA_MISMATCH, "matching prefix followed by an extra trailing field"},
        {"access_read.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL,
         "[{\"id\":2,\"name\":\"velocity_x\",\"type\":\"u32\",\"access\":\"read\"}]", "", NULL, SU_E_SCHEMA_MISMATCH,
         "access differs (no write)"},
        {"name.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL,
         "[{\"id\":2,\"name\":\"velocity_y\",\"type\":\"u32\",\"access\":\"read_write\"}]", "", NULL, SU_E_SCHEMA_MISMATCH,
         "name differs"},
        {"hash.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, kBadHash, k_fields_ok, "", NULL,
         SU_E_SCHEMA_MISMATCH, "constants_hash differs"},
        {"version.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 2u, NULL, k_fields_ok, "", NULL,
         SU_E_SCHEMA_MISMATCH, "operator_version differs"},
        {"sysid.capsule.json", "test.other", "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL, k_fields_ok, "", NULL,
         SU_E_INVALID_ARGUMENT, "system_id differs"},
        {"nosched.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", "", "rts_ops_v1", 1u, NULL, k_fields_ok, "", NULL,
         SU_E_SCHEMA_MISMATCH, "no schedule block"},
        {"source.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL, k_fields_ok,
         ",\"source\":\"x.lir.json\"", NULL, SU_E_SCHEMA_MISMATCH, "IR-only key source"},
        {"profile.capsule.json", TEST_HOST_VEL_ID, "rts_u32_v1", k_sched, "rts_ops_v1", 1u, NULL, k_fields_ok, "", NULL,
         SU_E_TYPE, "numeric_profile not supported by the engine"},
        {"operator.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "other_ops", 1u, NULL, k_fields_ok, "", NULL,
         SU_E_SCHEMA_MISMATCH, "operator name"},
        {"dupkey.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL, k_fields_ok,
         ",\"system_id\":\"dup\"", NULL, SU_E_DUPLICATE_KEY, "duplicate key"},
        {"ok.capsule.json", TEST_HOST_VEL_ID, "u32_mod_v1", k_sched, "rts_ops_v1", 1u, NULL, k_fields_ok, "", "00",
         SU_E_CANDIDATE_FAILED, "capsule_sha256 pin mismatch"},
    };

    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) {
        CHECK(0, "cannot create scratch dir %s", g_dir);
        return;
    }
    hash_hex_of(d, good_hash);
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        const char *hash = v[i].hash != NULL ? v[i].hash : good_hash;
        snprintf(text, sizeof text, CAPSULE_TEMPLATE, v[i].system_id, v[i].profile, v[i].sched, v[i].operator_name,
                 v[i].op_version, hash, v[i].fields, v[i].extra);
        write_text(v[i].file, text);
        path_of(v[i].file, path, sizeof path);
        su_sched_init(&s);
        out = NULL;
        st = su_host_load(path, v[i].pin, d, &out, &sv, &gen, msg, sizeof msg);
        CHECK(st == v[i].expect, "%s: expected %s, got %s (%s)", v[i].label, su_status_name(v[i].expect),
              su_status_name(st), msg);
        if (st == SU_OK) {
            CHECK(out != NULL && out->insn_count == 0u, "%s: declaration has no instructions", v[i].label);
            CHECK(out != NULL && out->semantic_hash_hex[0] != '\0' && out->capsule_hash_hex[0] != '\0',
                  "%s: semantic and capsule hashes are non-empty", v[i].label);
            CHECK(sv == 1u && gen == 1u, "%s: schema version and generation", v[i].label);
            CHECK(su_sched_add_host(&s, d, out, sv, gen, 0, 1, 0) == SU_OK, "%s: host entry joins the schedule",
                  v[i].label);
            CHECK(s.count == 1 && s.sys[0].kind == SU_SYS_KIND_HOST && s.sys[0].ctx == NULL,
                  "%s: host entry has no VM context", v[i].label);
            su_sched_free(&s);
        } else {
            CHECK(out == NULL, "%s: no declaration on refusal", v[i].label);
            CHECK(s.count == 0, "%s: world schedule untouched", v[i].label);
        }
    }

    /* A declaration that does not match the descriptor cannot join the schedule. */
    {
        SuLirModule *wrong = host_decl_of(&g_test_pos_writer);
        su_sched_init(&s);
        CHECK(wrong != NULL && su_sched_add_host(&s, d, wrong, 1u, 1u, 0, 1, 0) == SU_E_SCHEMA_MISMATCH,
              "su_sched_add_host refuses a declaration that differs from the descriptor");
        if (wrong != NULL) {
            su_host_decl_free(wrong);
        }
        CHECK(s.count == 0, "refused host add leaves the schedule empty");
        su_sched_free(&s);
    }
}

/* ---------- T-HOST-06 candidate refusal ---------- */

static void test_06_candidate(void) {
    SuWorld *w = make_world(ROWS, CAP);
    SuSchedule s;
    TestHostCtx ctx;
    SuLirModule *cand = NULL;
    uint32_t sv = 0;
    uint32_t gen = 0;
    char why[256];
    uint32_t st;
    uint64_t done = 0;
    uint32_t fi = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.fail_at = UINT32_MAX;
    if (w == NULL) {
        CHECK(0, "world for candidate test");
        return;
    }
    CHECK(build_std(&s, &ctx) == SU_OK, "build candidate schedule");
    CHECK(load_ir("examples/movement.capsule.json", "examples/movement.lir.json", &cand, &sv, &gen) == SU_OK,
          "candidate module");
    if (cand != NULL) {
        char *nid = malloc(strlen(TEST_HOST_VEL_ID) + 1u);
        if (nid != NULL) {
            memcpy(nid, TEST_HOST_VEL_ID, strlen(TEST_HOST_VEL_ID) + 1u);
            free(cand->system_id);
            cand->system_id = nid;
        }
        st = su_sched_check_candidate(&s, cand, sv, gen, why, sizeof why);
        CHECK(st == SU_E_CAPABILITY_DENIED, "candidate for a host entry refused with SU_E_CAPABILITY_DENIED (got %s: %s)",
              su_status_name(st), why);
        su_reload_module_free(cand);
    }
    st = su_sched_run_ticks(w, &s, 2u, NULL, NULL, &done, &fi);
    CHECK(st == SU_OK && committed_matches(w, 2u), "old host entry still runs after the refusal");
    su_sched_free(&s);
    su_world_destroy(w);
}

/* ---------- T-HOST-07 native refusal (the worker guard is in worker.c; see the handoff) ---------- */

/* ---------- T-HOST-08 no VM context, clean free ---------- */

static void test_08_no_context(void) {
    SuSchedule s;
    TestHostCtx ctx;
    SuLirModule *dh;
    uint32_t sv = 0;
    uint32_t gen = 0;
    SuLirModule *m = NULL;
    memset(&ctx, 0, sizeof ctx);
    ctx.fail_at = UINT32_MAX;
    CHECK(load_ir("examples/movement.capsule.json", "examples/movement.lir.json", &m, &sv, &gen) == SU_OK,
          "movement");
    su_sched_init(&s);
    g_test_vel_bump.host_ctx = &ctx;
    dh = host_decl_of(&g_test_vel_bump);
    CHECK(dh != NULL, "host decl");
    if (dh != NULL && m != NULL) {
        CHECK(su_sched_add(&s, m, sv, gen, 0, 1, 0) == SU_OK, "IR add");
        m = NULL;
        CHECK(su_sched_add_host(&s, &g_test_vel_bump, dh, sv, gen, 0, 1, 0) == SU_OK, "host add");
        CHECK(s.sys[1].ctx == NULL && s.sys[1].mod->insn_count == 0u && s.sys[1].mod->uses_entity_ops == 0u,
              "host entry: no context, no instructions");
        dh = NULL;
    }
    if (m != NULL) {
        su_reload_module_free(m);
    }
    if (dh != NULL) {
        su_host_decl_free(dh);
    }
    su_sched_free(&s); /* frees the declaration once (ASan build checks for double free) */
}

/* ---------- T-HOST-09 entity-op schedule ---------- */

static void test_09_entity_schedule(void) {
    SuWorld *w = make_world(ROWS, CAP);
    SuSchedule s;
    TestHostCtx ctx;
    SuLirModule *ent = NULL;
    SuLirModule *dh = NULL;
    uint32_t sv = 0;
    uint32_t gen = 0;
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;
    memset(&ctx, 0, sizeof ctx);
    ctx.fail_at = UINT32_MAX;
    if (w == NULL) {
        CHECK(0, "world for entity test");
        return;
    }
    su_sched_init(&s);
    g_test_vel_bump.host_ctx = &ctx;
    st = load_ir("tests/fixtures/entities/despawn_move.capsule.json", "tests/fixtures/entities/despawn_move.lir.json",
                 &ent, &sv, &gen);
    CHECK(st == SU_OK, "IR 0.2 entity system loads: %s", su_status_name(st));
    if (st == SU_OK) {
        CHECK(su_sched_add(&s, ent, sv, gen, 0, 1, 0) == SU_OK, "entity IR add");
        ent = NULL;
    }
    dh = host_decl_of(&g_test_vel_bump);
    if (dh != NULL) {
        CHECK(su_sched_add_host(&s, &g_test_vel_bump, dh, 1u, 1u, 0, 1, 0) == SU_OK, "host add");
        dh = NULL;
    }
    CHECK(su_sched_finalize(&s) == SU_OK, "finalize entity schedule");
    CHECK(su_sched_enable_entities(&s, 1000u, 1000u, 1000u) == SU_OK, "enable entities");
    st = su_sched_run_ticks(w, &s, 2u, NULL, NULL, &done, &fi);
    CHECK(st == SU_OK && done == 2u, "entity schedule with a host entry runs 2 ticks: %s", su_status_name(st));
    CHECK(ctx.calls == 2u, "host ran once per tick under su_ent_run_ticks (calls %u)", (unsigned)ctx.calls);
    CHECK(s.sys[1].kind == SU_SYS_KIND_HOST && s.sys[1].ctx == NULL, "host entry has no entity context");
    su_sched_free(&s);
    su_world_destroy(w);
}

/* ---------- T-HOST-02b dead rows zeroed between host entries; T-HOST-13 staging and views ---------- */

static void test_02b_dead_rows(void) {
    SuWorld *w = make_world(ROWS, CAP);
    SuSchedule s;
    TestHostCtx vc;
    TestHostCtx pc;
    SuLirModule *dv;
    SuLirModule *dp;
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;
    memset(&vc, 0, sizeof vc);
    memset(&pc, 0, sizeof pc);
    vc.fail_at = UINT32_MAX;
    pc.fail_at = UINT32_MAX;
    if (w == NULL) {
        CHECK(0, "world for dead-row test");
        return;
    }
    su_sched_init(&s);
    g_test_vel_bump.host_ctx = &vc;
    g_test_probe.host_ctx = &pc;
    dv = host_decl_of(&g_test_vel_bump);
    dp = host_decl_of(&g_test_probe);
    CHECK(dv != NULL && dp != NULL, "declarations");
    if (dv != NULL) {
        CHECK(su_sched_add_host(&s, &g_test_vel_bump, dv, 1u, 1u, 0, 1, 0) == SU_OK, "vel add");
        dv = NULL;
    }
    if (dp != NULL) {
        CHECK(su_sched_add_host(&s, &g_test_probe, dp, 1u, 1u, 0, 1, 0) == SU_OK, "probe add");
        dp = NULL;
    }
    if (dv != NULL) {
        su_host_decl_free(dv);
    }
    if (dp != NULL) {
        su_host_decl_free(dp);
    }
    st = su_sched_run_ticks(w, &s, 2u, NULL, NULL, &done, &fi);
    CHECK(st == SU_OK && done == 2u, "dead-row schedule runs: %s", su_status_name(st));
    CHECK(pc.dead_nonzero == 0u, "a dead row written by a host entry is zero when the next entry runs (saw %u)",
          (unsigned)pc.dead_nonzero);
    CHECK(pc.calls == 2u, "probe ran once per tick");
    su_sched_free(&s);
    su_world_destroy(w);
}

static void test_13_staging(void) {
    SuWorld *w = make_world(ROWS, CAP);
    SuSchedule s;
    TestHostCtx ctx;
    SuLirModule *d;
    uint64_t done = 0;
    uint32_t fi = 0;
    uint32_t st;
    const uint32_t *vc = NULL;
    uint64_t rows = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.fail_at = UINT32_MAX;
    if (w == NULL) {
        CHECK(0, "world for staging test");
        return;
    }
    su_sched_init(&s);
    g_test_target_writer.host_ctx = &ctx;
    d = host_decl_of(&g_test_target_writer);
    CHECK(d != NULL && su_sched_add_host(&s, &g_test_target_writer, d, 1u, 1u, 0, 1, 0) == SU_OK,
          "target writer add");
    if (d == NULL) {
        su_world_destroy(w);
        return;
    }
    st = su_sched_run_ticks(w, &s, 1u, NULL, NULL, &done, &fi);
    CHECK(st == SU_OK && done == 1u, "target writer tick: %s", su_status_name(st));
    CHECK(su_world_read_committed(w, 2u, &vc, &rows) == SU_OK, "read velocity");
    if (vc != NULL) {
        /* (a) only the target row changed; the non-target alive rows keep their working value. */
        CHECK(vc[0] == k_vel0[0] + 100u, "target row stored (got %u)", (unsigned)vc[0]);
        CHECK(vc[1] == k_vel0[1] && vc[2] == k_vel0[2],
              "non-target rows keep their working value through the commit (got %u, %u)", (unsigned)vc[1],
              (unsigned)vc[2]);
        CHECK(vc[3] == 0u && vc[4] == 0u, "dead rows zero");
    }
    CHECK(ctx.bad_views == 0u && ctx.calls == 1u, "views are the declared set");
    su_sched_free(&s);
    su_world_destroy(w);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    test_host_ops_init();
    test_01_ir_unchanged();
    test_02_dispatch_commit();
    test_02b_dead_rows();
    test_13_staging();
    test_03_failure();
    test_04_effects();
    test_05_load();
    test_06_candidate();
    test_08_no_context();
    test_09_entity_schedule();
    test_10_workers();
    printf("test_host_entry: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
