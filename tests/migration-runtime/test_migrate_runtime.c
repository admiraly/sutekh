/* M2-01b schema migration runtime tests: world.migrate_plan / migrate_commit / migrate_rollback through the
 * supervisor, with worker crashes at each commit stage (contract M2_MIGRATION_CONTRACT.md sections 4-6).
 *
 * Usage: test_migrate_runtime <recovery_worker> <repo_root> <scratch_dir>
 *   recovery_worker  built with -DSU_TEST_FAULT_HOOKS (the migration stage seam lives there)
 *   repo_root        absolute repository path (fixtures are copied from tests/reload)
 *   scratch_dir      writable directory: a project copy and per-session spool directories
 *
 * Every crash scenario is compared with an uninterrupted control run of the same operations: the canonical
 * digest (sha256 of the snapshot bytes, which include the world revision) after each step must be equal. A worker
 * is killed by the stage seam SUTEKH_TEST_KILL_STAGE (once, through a token file). */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "agent/su_agent.h"
#include "agent/su_proto.h"
#include "ir/su_json.h"
#include "sutekh/status.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static const char *g_worker;
static const char *g_root;
static char g_scratch[1024];
static char g_proj[2200];

/* ---------- sessions ---------- */

typedef struct Session {
    pid_t pid;
    int wfd;
    SuLineReader rd;
} Session;

typedef struct Resp {
    uint8_t *text;
    size_t len;
    SuJsonDoc doc;
    const SuJsonValue *root;
} Resp;

static int g_hold[64];
static int g_hold_n = 0;

static void resp_free(Resp *r) {
    free(r->text);
    su_json_doc_free(&r->doc);
    memset(r, 0, sizeof *r);
}

static int resp_ok(const Resp *r) {
    const SuJsonValue *ok = r->root != NULL ? su_json_member(r->root, "ok") : NULL;
    return ok != NULL && ok->type == SU_JSON_BOOL && ok->u.boolean == 1;
}

static const SuJsonValue *resp_result(const Resp *r) {
    return r->root != NULL ? su_json_member(r->root, "result") : NULL;
}

static uint64_t resp_error_status(const Resp *r) {
    const SuJsonValue *e = r->root != NULL ? su_json_member(r->root, "error") : NULL;
    uint64_t v = 0;
    if (e == NULL || !su_json_get_uint(su_json_member(e, "status"), &v)) {
        return 0;
    }
    return v;
}

static int resp_has_recovery(const Resp *r) {
    return r->root != NULL && su_json_member(r->root, "recovery") != NULL;
}

static uint64_t uint_of(const SuJsonValue *obj, const char *key) {
    uint64_t v = 0;
    if (obj == NULL || !su_json_get_uint(su_json_member(obj, key), &v)) {
        return 0;
    }
    return v;
}

static int str_of(const SuJsonValue *obj, const char *key, char *dst, size_t cap) {
    const SuJsonValue *v = obj != NULL ? su_json_member(obj, key) : NULL;
    if (v == NULL || v->type != SU_JSON_STRING || v->u.string.len >= cap) {
        dst[0] = '\0';
        return 0;
    }
    memcpy(dst, v->u.string.data, v->u.string.len);
    dst[v->u.string.len] = '\0';
    return 1;
}

static Session session_start(const char *spool_name) {
    Session s;
    int req[2];
    int rsp[2];
    pid_t pid;
    char spool[2200];
    SuAgentConfig cfg;
    static const char *argv[2];
    int k;

    snprintf(spool, sizeof spool, "%s/%s", g_scratch, spool_name);
    mkdir(spool, 0700);
    argv[0] = g_worker;
    argv[1] = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.project_root = g_proj;
    cfg.worker_argv = argv;
    cfg.request_timeout_ms = 30000;
    cfg.checkpoint_interval_ticks = 64;
    cfg.checkpoint_retain = 4;
    cfg.log_max_entries = 256;
    cfg.quarantine_after = 2;
    cfg.spool_bytes_max = UINT64_C(64) << 20;
    cfg.spool_dir = spool;
    if (pipe(req) != 0 || pipe(rsp) != 0) {
        perror("pipe");
        exit(2);
    }
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        int rc;
        close(req[1]);
        close(rsp[0]);
        for (k = 0; k < g_hold_n; k++) {
            close(g_hold[k]);
        }
        rc = su_agent_serve(req[0], rsp[1], &cfg);
        _exit(rc == 0 ? 0 : 1);
    }
    close(req[0]);
    close(rsp[1]);
    if (g_hold_n + 2 <= (int)(sizeof g_hold / sizeof g_hold[0])) {
        g_hold[g_hold_n++] = req[1];
        g_hold[g_hold_n++] = rsp[0];
    }
    memset(&s, 0, sizeof s);
    s.pid = pid;
    s.wfd = req[1];
    su_line_reader_init(&s.rd, rsp[0], -1);
    return s;
}

static void session_stop(Session *s) {
    int status = 0;
    int k;
    close(s->wfd);
    for (k = 0; k < g_hold_n; k++) {
        if (g_hold[k] == s->wfd || g_hold[k] == s->rd.fd) {
            g_hold[k] = -1;
        }
    }
    su_line_reader_free(&s->rd);
    waitpid(s->pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int call(Session *s, const char *line, Resp *r) {
    const uint8_t *out = NULL;
    size_t len = 0;
    int overflow = 0;
    SuJsonLimits lim;
    SuJsonError jerr;
    uint32_t st;

    memset(r, 0, sizeof *r);
    if (su_write_all(s->wfd, (const uint8_t *)line, strlen(line), -1) != 0 ||
        su_write_all(s->wfd, (const uint8_t *)"\n", 1, -1) != 0) {
        return -1;
    }
    s->rd.deadline_ms = su_now_ms() + 120000;
    if (su_line_next(&s->rd, &out, &len, &overflow) != 1) {
        return -1;
    }
    r->text = malloc(len + 1u);
    if (r->text == NULL) {
        return -1;
    }
    memcpy(r->text, out, len);
    r->text[len] = '\0';
    r->len = len;
    lim.max_bytes = len;
    lim.max_depth = 40;
    st = su_json_parse(r->text, len, &lim, &r->doc, &jerr);
    r->root = st == SU_OK && r->doc.root != NULL ? r->doc.root : NULL;
    if (st != SU_OK) {
        su_json_doc_free(&r->doc);
    }
    return 0;
}

/* ---------- project copy ---------- */

static void copy_from(const char *dir, const char *name) {
    char src[4400];
    char dst[4400];
    FILE *fi;
    FILE *fo;
    char buf[4096];
    size_t n;

    snprintf(src, sizeof src, "%s/%s/%s", g_root, dir, name);
    snprintf(dst, sizeof dst, "%s/%s", g_proj, name);
    fi = fopen(src, "rb");
    fo = fopen(dst, "wb");
    if (fi == NULL || fo == NULL) {
        perror(name);
        exit(2);
    }
    while ((n = fread(buf, 1, sizeof buf, fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) {
            perror(dst);
            exit(2);
        }
    }
    fclose(fi);
    fclose(fo);
}

static void overwrite_file(const char *name, const char *bytes) {
    char dst[4400];
    FILE *fo;
    snprintf(dst, sizeof dst, "%s/%s", g_proj, name);
    fo = fopen(dst, "wb");
    if (fo == NULL) {
        perror(dst);
        exit(2);
    }
    fputs(bytes, fo);
    fclose(fo);
}

/* A token file for the stage seam: the worker unlinks it, so the kill happens once. */
static void arm_stage(const char *stage) {
    char tok[2200];
    snprintf(tok, sizeof tok, "%s/token-%s", g_scratch, stage);
    {
        FILE *f = fopen(tok, "wb");
        if (f == NULL) {
            perror(tok);
            exit(2);
        }
        fputs("armed\n", f);
        fclose(f);
    }
    setenv("SUTEKH_TEST_KILL_STAGE", stage, 1);
    setenv("SUTEKH_TEST_KILL_TOKEN", tok, 1);
}

static void disarm_stage(void) {
    unsetenv("SUTEKH_TEST_KILL_STAGE");
    unsetenv("SUTEKH_TEST_KILL_TOKEN");
}

/* ---------- request helpers ---------- */

static int create_world(Session *s, char *wid, size_t wcap, char *rev, size_t rcap) {
    Resp r;
    int ok;
    call(s,
         "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
         "\"world4.json\",\"capsule_path\":\"sys_a.capsule.json\"}}",
         &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_id", wid, wcap) &&
         str_of(resp_result(&r), "world_revision", rev, rcap);
    resp_free(&r);
    return ok;
}

static int step(Session *s, const char *wid, char *rev, size_t rcap, uint64_t ticks, Resp *r) {
    char line[512];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"st\",\"method\":\"world.step\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"ticks\":%llu}}",
             wid, rev, (unsigned long long)ticks);
    call(s, line, r);
    ok = resp_ok(r);
    if (ok) {
        str_of(resp_result(r), "world_revision", rev, rcap);
    }
    return ok;
}

static int digest(Session *s, const char *wid, const char *rev, char sha[65], uint64_t *tick) {
    Resp r;
    char line[512];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"sn\",\"method\":\"world.snapshot\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             wid, rev);
    call(s, line, &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "sha256", sha, 65);
    if (ok && tick != NULL) {
        *tick = uint_of(resp_result(&r), "tick");
    }
    resp_free(&r);
    return ok;
}

static int submit(Session *s, const char *capsule, char *cand, size_t ccap) {
    Resp r;
    char line[512];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"sub\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"%s\"}}",
             capsule);
    call(s, line, &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "candidate_id", cand, ccap);
    resp_free(&r);
    return ok;
}

static int evaluate(Session *s, const char *cand, const char *test_file) {
    Resp r;
    char line[512];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ev\",\"method\":\"candidate.evaluate\",\"params\":{\"candidate_id\":"
             "\"%s\",\"test_path\":\"%s\"}}",
             cand, test_file);
    call(s, line, &r);
    ok = resp_ok(&r);
    if (ok) {
        char st[16];
        ok = str_of(resp_result(&r), "status", st, sizeof st) && strcmp(st, "passed") == 0;
    }
    resp_free(&r);
    return ok;
}

static int plan(Session *s, const char *wid, const char *rev, const char *migration, char *pid, size_t pcap,
                char *phash, size_t hcap, int *valid, Resp *r) {
    char line[768];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"mp\",\"method\":\"world.migrate_plan\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"migration_path\":\"%s\"}}",
             wid, rev, migration);
    call(s, line, r);
    if (!resp_ok(r)) {
        return 0;
    }
    str_of(resp_result(r), "plan_id", pid, pcap);
    {
        const SuJsonValue *pl = su_json_member(resp_result(r), "plan");
        str_of(pl, "plan_hash", phash, hcap);
        *valid = pl != NULL && su_json_member(pl, "valid") != NULL && su_json_member(pl, "valid")->type == SU_JSON_BOOL &&
                 su_json_member(pl, "valid")->u.boolean == 1;
    }
    return 1;
}

static int commit(Session *s, const char *wid, const char *pid, const char *rev, const char *cand, Resp *r) {
    char line[1024];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"mc\",\"method\":\"world.migrate_commit\",\"params\":{\"world_id\":\"%s\","
             "\"plan_id\":\"%s\",\"expected_world_revision\":\"%s\",\"resubmit\":[{\"system_id\":\"demo.movement\","
             "\"candidate_id\":\"%s\"}]}}",
             wid, pid, rev, cand);
    call(s, line, r);
    return resp_ok(r);
}

/* Shared control/crash fixture: a world with one gen-1 system, two ticks, a gen-2 candidate evaluated. */
typedef struct Setup {
    char wid[24];
    char rev[48];
    char cand[32];
    char pid[32];
    char phash[65];
    int valid;
} Setup;

static int setup_world(Session *s, Setup *st) {
    Resp r;
    int ok;
    memset(st, 0, sizeof *st);
    if (!create_world(s, st->wid, sizeof st->wid, st->rev, sizeof st->rev)) {
        return 0;
    }
    ok = step(s, st->wid, st->rev, sizeof st->rev, 2, &r);
    resp_free(&r);
    if (!ok) {
        return 0;
    }
    ok = submit(s, "sys_a_gen2.capsule.json", st->cand, sizeof st->cand) &&
         evaluate(s, st->cand, "sys_a_gen2.test.json");
    return ok;
}

/* ---------- scenarios ---------- */

typedef struct Outcome {
    char pre_digest[65];
    char final_digest[65];
    char commit_code[64];   /* "ok" or the error code of the commit */
    int commit_recovery;    /* the commit response reports a recovery */
    int step_recovery;      /* the post-commit step reports a recovery */
    int event_seen;         /* SU_EVENT_MIGRATED is in the event log after the commit */
    uint64_t final_tick;
} Outcome;

/* Runs the standard sequence: plan, commit, two more ticks. With rollback != 0, rolls back after the commit and
 * records the digest of the restored state. */
static void run_happy(const char *tag, const char *stage, Outcome *o, int crash_step) {
    Session s;
    Setup st;
    Resp r;
    char spool[64];
    char rev[48];
    int valid = 0;
    char pid[32];
    char phash[65];

    memset(o, 0, sizeof *o);
    snprintf(spool, sizeof spool, "spool-%s", tag);
    /* The stage seam is read from the environment the supervisor inherits at fork, so it is armed first. */
    if (stage != NULL) {
        arm_stage(stage);
    }
    s = session_start(spool);
    disarm_stage();
    if (!setup_world(&s, &st)) {
        CHECK(0);
        session_stop(&s);
        return;
    }
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(digest(&s, st.wid, rev, o->pre_digest, NULL));
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(valid == 1);
    resp_free(&r);

    commit(&s, st.wid, pid, rev, st.cand, &r);
    if (resp_ok(&r)) {
        snprintf(o->commit_code, sizeof o->commit_code, "ok");
        o->commit_recovery = resp_has_recovery(&r);
        str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    } else {
        const SuJsonValue *e = su_json_member(r.root, "error");
        str_of(e, "code", o->commit_code, sizeof o->commit_code);
        if (getenv("MIGRATE_TRACE") != NULL) {
            fprintf(stderr, "[%s] commit: %s\n", tag, r.text);
        }
        resp_free(&r);
        if (strcmp(o->commit_code, "SU_E_WORKER_FAILED") == 0) {
            /* Not acknowledged: the world is the pre-migration state; the retry (the stage fired once) commits. */
            CHECK(digest(&s, st.wid, rev, o->final_digest, NULL));
            CHECK(strcmp(o->final_digest, o->pre_digest) == 0);
            commit(&s, st.wid, pid, rev, st.cand, &r);
            CHECK(resp_ok(&r));
            o->commit_recovery = resp_has_recovery(&r);
            str_of(resp_result(&r), "world_revision", rev, sizeof rev);
        }
    }
    resp_free(&r);

    if (crash_step && strcmp(o->commit_code, "ok") == 0) {
        /* The stage seam (armed at fork) kills the worker on the first step after the post-migration checkpoint. */
        if (!step(&s, st.wid, rev, sizeof rev, 2, &r)) {
            o->step_recovery = resp_has_recovery(&r);
            resp_free(&r);
            if (getenv("MIGRATE_TRACE") != NULL) {
                fprintf(stderr, "[%s] first step after commit: %s\n", tag, r.text);
            }
            CHECK(step(&s, st.wid, rev, sizeof rev, 2, &r));
            if (!resp_ok(&r) && getenv("MIGRATE_TRACE") != NULL) {
                fprintf(stderr, "[%s] retry step: %s\n", tag, r.text);
            }
            resp_free(&r);
        } else {
            resp_free(&r);
        }
        disarm_stage();
    } else {
        CHECK(step(&s, st.wid, rev, sizeof rev, 2, &r));
        resp_free(&r);
    }
    CHECK(digest(&s, st.wid, rev, o->final_digest, &o->final_tick));
    if (stage == NULL && !crash_step) {
        /* SU_EVENT_MIGRATED (0xFFFFFFFC) is in the world's event log after the commit. */
        char line[256];
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"ev\",\"method\":\"events.read\",\"params\":{\"world_id\":\"%s\","
                 "\"cursor\":0,\"max\":256}}",
                 st.wid);
        call(&s, line, &r);
        o->event_seen = resp_ok(&r) && strstr((const char *)r.text, "4294967292") != NULL;
        resp_free(&r);
    }
    session_stop(&s);
}

/* Crash during a rollback (stage migrate.rollback, after the restore and before the reply): the rollback is not
 * acknowledged, recovery restores the post-migration checkpoint (the commit is durable), and the retry rolls back. */
static void run_rollback_crash(void) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    int valid = 0;
    char line[512];
    int ok_rb = 0;

    arm_stage("migrate.rollback");
    s = session_start("spool-rollback-crash");
    disarm_stage();
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    resp_free(&r);
    CHECK(commit(&s, st.wid, pid, rev, st.cand, &r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             st.wid, rev);
    call(&s, line, &r);
    CHECK(!resp_ok(&r));
    {
        char code[64];
        str_of(su_json_member(r.root, "error"), "code", code, sizeof code);
        CHECK(strcmp(code, "SU_E_WORKER_FAILED") == 0);
    }
    resp_free(&r);

    /* After recovery the world is the committed (post-migration) state at the same revision; the retry rolls back. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rb2\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             st.wid, rev);
    call(&s, line, &r);
    ok_rb = resp_ok(&r);
    CHECK(ok_rb);
    CHECK(ok_rb && su_json_member(resp_result(&r), "changes_lost") != NULL);
    resp_free(&r);
    session_stop(&s);
}

/* Refusals: an invalid plan (remove a used field) and a stale plan. */
static void run_refusals(void) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    int valid = 1;

    overwrite_file("m_remove.json",
                   "{\"migration_format\":\"0.1\",\"id\":\"m-remove\",\"from_layout_generation\":1,"
                   "\"to_layout_generation\":2,\"ops\":[{\"op\":\"remove_field\",\"id\":2}]}");
    s = session_start("spool-refuse");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(plan(&s, st.wid, rev, "m_remove.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(valid == 0);
    resp_free(&r);
    CHECK(!commit(&s, st.wid, pid, rev, st.cand, &r));
    CHECK(resp_error_status(&r) != 0);
    resp_free(&r);

    /* A valid plan made now, then a step: the commit must be refused as stale. */
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    resp_free(&r);
    CHECK(step(&s, st.wid, rev, sizeof rev, 1, &r));
    resp_free(&r);
    CHECK(!commit(&s, st.wid, pid, rev, st.cand, &r));
    {
        char code[64];
        str_of(su_json_member(r.root, "error"), "code", code, sizeof code);
        CHECK(strcmp(code, "SU_E_STALE_REVISION") == 0);
    }
    resp_free(&r);
    session_stop(&s);
}

/* Rollback: restores the pinned pre-migration checkpoint as a new revision; the columns return to the pin's and
 * the changes after the pin are reported as lost. */
static int inspect_state(Session *s, const char *wid, char *schema, size_t scap, char *cols, size_t ccap);

static void run_rollback(void) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    int valid = 0;
    char pre_sha[65];
    char post_sha[65];
    char back_sha[65];
    char pre_schema[4096];
    char pre_cols[4096];
    char back_schema[4096];
    char back_cols[4096];

    s = session_start("spool-rollback");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(digest(&s, st.wid, rev, pre_sha, NULL));
    CHECK(inspect_state(&s, st.wid, pre_schema, sizeof pre_schema, pre_cols, sizeof pre_cols));
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    resp_free(&r);
    CHECK(commit(&s, st.wid, pid, rev, st.cand, &r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);
    CHECK(digest(&s, st.wid, rev, post_sha, NULL));
    CHECK(strcmp(pre_sha, post_sha) != 0);

    {
        char line[512];
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
                 "\"expected_world_revision\":\"%s\"}}",
                 st.wid, rev);
        call(&s, line, &r);
        CHECK(resp_ok(&r));
        CHECK(su_json_member(resp_result(&r), "changes_lost") != NULL);
        str_of(resp_result(&r), "world_revision", rev, sizeof rev);
        resp_free(&r);
    }
    /* The rollback restores the pinned schema: the world has the pre-migration field table again. The
     * canonical bytes differ from the pre-migration digest by the revision and the retired-ID record. */
    CHECK(digest(&s, st.wid, rev, back_sha, NULL));
    CHECK(strcmp(back_sha, post_sha) != 0);
    /* M2-01 fixes2: the restored schema and the stored values equal the pre-migration ones (the state, not only
     * the digest, which moves with the revision). */
    CHECK(inspect_state(&s, st.wid, back_schema, sizeof back_schema, back_cols, sizeof back_cols));
    CHECK(strcmp(back_schema, pre_schema) == 0);
    CHECK(strcmp(back_cols, pre_cols) == 0);
    /* A second rollback with no pin change is available again (the pin is retained). */
    session_stop(&s);
}

/* ---------- M2-01 fixes: undone commits, retired field IDs ---------- */

/* The active worker's pid, from engine.info, or 0. */
static uint64_t active_worker_pid(Session *s) {
    Resp r;
    uint64_t pid = 0;
    call(s, "{\"protocol\":\"0.1\",\"id\":\"ei\",\"method\":\"engine.info\",\"params\":{}}", &r);
    if (resp_ok(&r)) {
        pid = uint_of(su_json_member(resp_result(&r), "active_worker"), "pid");
    }
    resp_free(&r);
    return pid;
}

/* Kills the active worker from outside (SIGKILL). The next request finds the dead worker, recovers it and is
 * answered with SU_E_WORKER_FAILED; the request after that runs on the replacement. */
static void kill_active_worker(Session *s, const char *wid, const char *rev) {
    uint64_t pid = active_worker_pid(s);
    char sha[65];
    CHECK(pid > 0);
    if (pid > 0) {
        kill((pid_t)pid, SIGKILL);
    }
    (void)digest(s, wid, rev, sha, NULL);
    CHECK(active_worker_pid(s) != pid);
}

/* Writes a stage kill token: the seam kills the worker count times (see mig_stage in worker.c). The env is
 * inherited by the supervisor at fork, so it is set before session_start. */
static void arm_kill_count(const char *stage, const char *count) {
    char tok[2200];
    FILE *f;
    snprintf(tok, sizeof tok, "%s/token-%s-%s", g_scratch, stage, count);
    f = fopen(tok, "wb");
    if (f == NULL) {
        perror(tok);
        exit(2);
    }
    fputs(count, f);
    fputc('\n', f);
    fclose(f);
    setenv("SUTEKH_TEST_KILL_STAGE", stage, 1);
    setenv("SUTEKH_TEST_KILL_TOKEN", tok, 1);
}

/* Arms the supervisor's no-room seam (one post-commit checkpoint). Read by the supervisor at fork. */
static void arm_post_noroom(void) {
    char tok[2200];
    FILE *f;
    snprintf(tok, sizeof tok, "%s/token-post-noroom", g_scratch);
    f = fopen(tok, "wb");
    if (f == NULL) {
        perror(tok);
        exit(2);
    }
    fputs("armed\n", f);
    fclose(f);
    setenv("SUTEKH_TEST_POST_CK_NOROOM_TOKEN", tok, 1);
}

static int error_code(const Resp *r, char *code, size_t cap) {
    return str_of(su_json_member(r->root, "error"), "code", code, cap);
}

/* Post-commit failure, undone (contract section 1, B9). mode "double": the post-migration checkpoint dies in the
 * worker twice (the commit's capture, then the capture of the replacement worker). mode "noroom": the post-commit
 * checkpoint reports no room (supervisor test seam). In both cases the failed commit must leave the world at the
 * pre-commit digest, a kill and recovery must rebuild that state, and the retry must reach the control digest. */
static void run_undo(const char *mode, const Outcome *ctl) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    char code[64];
    char sha[65];
    char spool[64];
    int valid = 0;
    int double_fault = strcmp(mode, "double") == 0;

    snprintf(spool, sizeof spool, "spool-undo-%s", mode);
    if (double_fault) {
        arm_kill_count("migrate.capture", "2");
    } else {
        arm_post_noroom();
    }
    s = session_start(spool);
    disarm_stage();
    unsetenv("SUTEKH_TEST_POST_CK_NOROOM_TOKEN");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(valid == 1);
    resp_free(&r);

    /* The first commit attempt: the post-commit checkpoint fails and the migration is undone. */
    CHECK(!commit(&s, st.wid, pid, rev, st.cand, &r));
    CHECK(error_code(&r, code, sizeof code));
    if (double_fault) {
        CHECK(strcmp(code, "SU_E_WORKER_FAILED") == 0);
        CHECK(strstr((const char *)r.text, "post_commit_checkpoint_failed") != NULL);
        CHECK(resp_has_recovery(&r));
    } else {
        CHECK(strcmp(code, "SU_E_RESOURCE_LIMIT") == 0);
        CHECK(strstr((const char *)r.text, "\"reason\":\"post_commit_checkpoint\"") != NULL);
    }
    CHECK(strstr((const char *)r.text, "rolled_back_to") != NULL);
    resp_free(&r);

    /* The world is the old world at the old revision: the digest equals the control's pre-migration digest. */
    CHECK(digest(&s, st.wid, rev, sha, NULL));
    CHECK(strcmp(sha, ctl->pre_digest) == 0);

    /* Kill and recovery: the replacement rebuilds the same state from the pin and the replay log. */
    kill_active_worker(&s, st.wid, rev);
    CHECK(digest(&s, st.wid, rev, sha, NULL));
    CHECK(strcmp(sha, ctl->pre_digest) == 0);

    /* The retry commits (the kill count is spent) and the run reaches the control digest after two ticks. */
    CHECK(commit(&s, st.wid, pid, rev, st.cand, &r));
    CHECK(resp_ok(&r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);
    CHECK(step(&s, st.wid, rev, sizeof rev, 2, &r));
    resp_free(&r);
    CHECK(digest(&s, st.wid, rev, sha, NULL));
    CHECK(strcmp(sha, ctl->final_digest) == 0);
    session_stop(&s);
}

/* Field IDs of a rolled-back lineage stay retired (contract B5, section 5): after a commit that adds id 3 and its
 * rollback, the id is refused by a new plan, also after a kill and recovery, and an unused id is still accepted. */
static void run_id_retirement(void) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    char code[64];
    int valid = 0;
    char line[512];

    overwrite_file("m_add_shield.json",
                   "{\"migration_format\":\"0.1\",\"id\":\"m-add-shield\",\"from_layout_generation\":1,"
                   "\"to_layout_generation\":2,\"ops\":[{\"op\":\"add_field\",\"field\":{\"id\":4,\"name\":\"shield\","
                   "\"type\":\"u32\",\"default\":7}}]}");
    s = session_start("spool-id-retire");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    resp_free(&r);
    CHECK(commit(&s, st.wid, pid, rev, st.cand, &r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             st.wid, rev);
    call(&s, line, &r);
    CHECK(resp_ok(&r));
    {
        char lin[16];
        CHECK(str_of(resp_result(&r), "lineage", lin, sizeof lin) && strcmp(lin, "retained") == 0);
    }
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);

    /* id 3 was used by the rolled-back lineage: a new add_field is refused as retired. */
    CHECK(!plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_SCHEMA_MISMATCH") == 0);
    resp_free(&r);
    /* An id never used is still accepted. */
    CHECK(plan(&s, st.wid, rev, "m_add_shield.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(valid == 1);
    resp_free(&r);

    kill_active_worker(&s, st.wid, rev);
    CHECK(!plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_SCHEMA_MISMATCH") == 0);
    resp_free(&r);
    session_stop(&s);
}

/* A checkpoint taken after the rollback (64 ticks, the test interval) must carry the retired IDs: recovery from it
 * keeps the refusal. This is what the migrated flag after a rollback protects (contract section 6). */
static void run_id_retirement_after_checkpoint(void) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    char code[64];
    char line[512];
    int valid = 0;

    s = session_start("spool-id-retire-ck");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    resp_free(&r);
    CHECK(commit(&s, st.wid, pid, rev, st.cand, &r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             st.wid, rev);
    call(&s, line, &r);
    CHECK(resp_ok(&r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);
    /* 64 ticks: the interval checkpoint is taken after the rollback. */
    CHECK(step(&s, st.wid, rev, sizeof rev, 64, &r));
    resp_free(&r);
    kill_active_worker(&s, st.wid, rev);
    CHECK(!plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_SCHEMA_MISMATCH") == 0);
    resp_free(&r);
    session_stop(&s);
}

/* Commit admission of candidates (contract section 4): a candidate that has not passed evaluation is refused,
 * and so is a candidate of the old generation. The world is unchanged in both cases. */
static void run_candidate_refusals(void) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    char code[64];
    char cand[32];
    char pre_sha[65];
    char post_sha[65];
    int valid = 0;

    s = session_start("spool-cand-refuse");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(digest(&s, st.wid, rev, pre_sha, NULL));
    /* A missing document is an I/O refusal naming the file, not a path-policy refusal. */
    CHECK(!plan(&s, st.wid, rev, "no_such_migration.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_IO") == 0);
    CHECK(strstr((const char *)r.text, "does not exist") != NULL);
    resp_free(&r);
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    resp_free(&r);

    /* Submitted, not evaluated. */
    CHECK(submit(&s, "sys_a_gen2.capsule.json", cand, sizeof cand));
    CHECK(!commit(&s, st.wid, pid, rev, cand, &r));
    CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_UNSUPPORTED_MIGRATION") == 0);
    resp_free(&r);

    /* Evaluated, but generation 1: it cannot replace the system of a generation-2 world. */
    CHECK(submit(&s, "sys_a.capsule.json", cand, sizeof cand));
    CHECK(evaluate(&s, cand, "sys_a.test.json"));
    CHECK(!commit(&s, st.wid, pid, rev, cand, &r));
    CHECK(error_code(&r, code, sizeof code));
    CHECK(strcmp(code, "SU_E_SCHEMA_MISMATCH") == 0 || strcmp(code, "SU_E_UNSUPPORTED_MIGRATION") == 0);
    resp_free(&r);

    CHECK(digest(&s, st.wid, rev, post_sha, NULL));
    CHECK(strcmp(pre_sha, post_sha) == 0);
    session_stop(&s);
}

/* Two committed migrations (1 -> 2 adds health, 2 -> 3 adds shield), then a rollback to the pin taken before the
 * second one. The restored schema differs from the fixture, so recovery after the rollback must rebuild from the
 * checkpoint's schema (contract section 6): a checkpoint taken after the rollback, a kill, and recovery must
 * reproduce the same digest, keep health live, and keep the ID of the undone migration (4) retired. */
static void run_rollback_two_migrations(void) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    char cand3[32];
    char code[64];
    char sha_back[65];
    char sha_ck[65];
    char line[512];
    int valid = 0;

    s = session_start("spool-two-mig");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    resp_free(&r);
    CHECK(commit(&s, st.wid, pid, rev, st.cand, &r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);

    CHECK(submit(&s, "sys_a_gen3.capsule.json", cand3, sizeof cand3));
    CHECK(evaluate(&s, cand3, "sys_a_gen3.test.json"));
    CHECK(plan(&s, st.wid, rev, "m_shield3.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(valid == 1);
    resp_free(&r);
    CHECK(commit(&s, st.wid, pid, rev, cand3, &r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);

    /* Rolls back to the pin of the second commit: the schema with health, layout generation 2. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             st.wid, rev);
    call(&s, line, &r);
    CHECK(resp_ok(&r));
    {
        char lin[16];
        CHECK(str_of(resp_result(&r), "lineage", lin, sizeof lin) && strcmp(lin, "retained") == 0);
    }
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);
    CHECK(digest(&s, st.wid, rev, sha_back, NULL));

    /* A checkpoint after the rollback (64 ticks), then a kill and recovery from it: the same digest. */
    CHECK(step(&s, st.wid, rev, sizeof rev, 64, &r));
    resp_free(&r);
    CHECK(digest(&s, st.wid, rev, sha_ck, NULL));
    kill_active_worker(&s, st.wid, rev);
    CHECK(digest(&s, st.wid, rev, sha_back, NULL));
    CHECK(strcmp(sha_back, sha_ck) == 0);

    /* The restored schema still has health (3) and not shield (4); 4 is retired, so the second migration is refused. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\","
             "\"fields\":[\"position_x\"],\"limit\":1}}",
             st.wid);
    call(&s, line, &r);
    CHECK(resp_ok(&r) && strstr((const char *)r.text, "\"name\":\"health\"") != NULL);
    CHECK(resp_ok(&r) && strstr((const char *)r.text, "\"name\":\"shield\"") == NULL);
    resp_free(&r);
    CHECK(!plan(&s, st.wid, rev, "m_shield3.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_SCHEMA_MISMATCH") == 0);
    resp_free(&r);
    session_stop(&s);
}

/* ---------- M2-01 fixes2: N1 (pin of a failed commit), N2 (allocation points), C1, rollback comparison ---------- */

/* Writes a token file (the worker or supervisor test seam reads and removes it). */
static void put_token(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static int token_gone(const char *path) {
    return access(path, F_OK) != 0;
}

/* The raw text of member key (an object or an array) of a JSON reply text, or 0 when absent or too long. */
static int raw_member(const char *text, const char *key, char *out, size_t cap) {
    char pat[96];
    const char *p;
    const char *q;
    int depth = 0;
    size_t n;

    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(text, pat);
    if (p == NULL) {
        return 0;
    }
    p += strlen(pat);
    if (*p != '[' && *p != '{') {
        return 0;
    }
    for (q = p; *q != '\0'; q++) {
        if (*q == '[' || *q == '{') {
            depth++;
        } else if (*q == ']' || *q == '}') {
            depth--;
            if (depth == 0) {
                q++;
                break;
            }
        }
    }
    n = (size_t)(q - p);
    if (n + 1u > cap) {
        return 0;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* The schema and the stored values of field position_x (world.inspect, at the world's current state). */
static int inspect_state(Session *s, const char *wid, char *schema, size_t scap, char *cols, size_t ccap) {
    Resp r;
    char line[512];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\","
             "\"fields\":[\"position_x\"],\"limit\":256}}",
             wid);
    call(s, line, &r);
    ok = resp_ok(&r) && raw_member((const char *)r.text, "schema", schema, scap) &&
         raw_member((const char *)r.text, "columns", cols, ccap);
    resp_free(&r);
    return ok;
}

/* Rollback at tick 63, below the checkpoint interval (64). The checkpoints are: the initial one, one at tick 2 (taken by
 * the digest below: world.snapshot checkpoints the state), and the commit's pin at tick 63. The checkpoint before the
 * pin is therefore the tick-2 state, a different state, so a rollback to it is detected (the tick and the columns
 * differ). The state at tick 63 is read with world.inspect, which does not checkpoint, so the pin stays at tick 63. */
static void run_rollback_deep(void) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    char pre_sha[65];
    char back_sha[65];
    char pre_schema[4096];
    char pre_cols[4096];
    char back_schema[4096];
    char back_cols[4096];
    char line[512];
    uint64_t pre_tick = 0;
    uint64_t back_tick = 0;
    int valid = 0;

    s = session_start("spool-rollback-deep");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(digest(&s, st.wid, rev, pre_sha, NULL));
    CHECK(step(&s, st.wid, rev, sizeof rev, 61, &r));
    pre_tick = uint_of(resp_result(&r), "tick");
    resp_free(&r);
    CHECK(pre_tick == 63u);
    CHECK(inspect_state(&s, st.wid, pre_schema, sizeof pre_schema, pre_cols, sizeof pre_cols));
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(valid == 1);
    resp_free(&r);
    CHECK(commit(&s, st.wid, pid, rev, st.cand, &r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             st.wid, rev);
    call(&s, line, &r);
    CHECK(resp_ok(&r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);
    CHECK(digest(&s, st.wid, rev, back_sha, &back_tick));
    CHECK(back_tick == pre_tick);
    CHECK(inspect_state(&s, st.wid, back_schema, sizeof back_schema, back_cols, sizeof back_cols));
    CHECK(strcmp(back_schema, pre_schema) == 0);
    CHECK(strcmp(back_cols, pre_cols) == 0);
    session_stop(&s);
}

/* N1: a failed second commit leaves the first commit's pin in place (the rollback target). The second commit fails
 * either in its prepare (mode "prepare": the allocation seam refuses the prepare request's first point) or in its
 * post-commit checkpoint (mode "noroom": the supervisor no-room seam, armed after the first commit). Afterwards the
 * world is the state after the first commit, and rollback restores the state before it: the schema has no shield,
 * no health, and the restored_from names the first plan. */
static void run_failed_second_commit(const char *mode) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    char phash1[65];
    char cand3[32];
    char code[64];
    char sha_after1[65];
    char sha[65];
    char tok[2200];
    char restored[65];
    char line[512];
    char schema[4096];
    char cols[4096];
    int valid = 0;
    int prep = strcmp(mode, "prepare") == 0;
    const char *env_name = prep ? "SUTEKH_TEST_MIG_ALLOC_TOKEN" : "SUTEKH_TEST_POST_CK_NOROOM_TOKEN";

    snprintf(tok, sizeof tok, "%s/token-n1-%s", g_scratch, mode);
    unlink(tok);
    setenv(env_name, tok, 1);
    s = session_start(prep ? "spool-n1-prepare" : "spool-n1-noroom");
    unsetenv(env_name);
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash1, sizeof phash1, &valid, &r));
    CHECK(valid == 1);
    resp_free(&r);
    CHECK(commit(&s, st.wid, pid, rev, st.cand, &r));
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);
    CHECK(digest(&s, st.wid, rev, sha_after1, NULL));

    CHECK(submit(&s, "sys_a_gen3.capsule.json", cand3, sizeof cand3));
    CHECK(evaluate(&s, cand3, "sys_a_gen3.test.json"));
    CHECK(plan(&s, st.wid, rev, "m_shield3.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(valid == 1);
    resp_free(&r);
    if (prep) {
        put_token(tok, "1\n");
    } else {
        put_token(tok, "armed\n");
    }
    CHECK(!commit(&s, st.wid, pid, rev, cand3, &r));
    CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_RESOURCE_LIMIT") == 0);
    resp_free(&r);
    CHECK(digest(&s, st.wid, rev, sha, NULL) && strcmp(sha, sha_after1) == 0);

    /* Rollback after the failed attempt: the target is the first commit's pin. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             st.wid, rev);
    call(&s, line, &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(resp_result(&r), "restored_from", restored, sizeof restored) && strcmp(restored, phash1) == 0);
    str_of(resp_result(&r), "world_revision", rev, sizeof rev);
    resp_free(&r);
    CHECK(inspect_state(&s, st.wid, schema, sizeof schema, cols, sizeof cols));
    CHECK(strstr(schema, "\"name\":\"health\"") == NULL);
    CHECK(strstr(schema, "\"name\":\"shield\"") == NULL);
    session_stop(&s);
}

/* N2: an allocation failure at each injection point of world.migrate_plan and of the commit's migrate.prepare request
 * (the replan and the successor world) leaves the world bit-identical at its revision and active. Points are armed one
 * at a time from 1 upward through the SUTEKH_TEST_MIG_ALLOC_TOKEN file; the first request without a failure succeeds.
 * After the failed requests a kill and recovery still give the pre-migration digest, and the successful commit after
 * two ticks reaches the control digest. */
static void run_alloc_faults(const Outcome *ctl) {
    Session s;
    Setup st;
    Resp r;
    char rev[48];
    char pid[32];
    char phash[65];
    char code[64];
    char sha[65];
    char tok[2200];
    char line[512];
    int valid = 0;
    uint64_t n;
    uint64_t k_plan = 0;
    uint64_t k_prep = 0;

    snprintf(tok, sizeof tok, "%s/token-mig-alloc", g_scratch);
    unlink(tok);
    setenv("SUTEKH_TEST_MIG_ALLOC_TOKEN", tok, 1);
    s = session_start("spool-alloc");
    unsetenv("SUTEKH_TEST_MIG_ALLOC_TOKEN");
    CHECK(setup_world(&s, &st));
    snprintf(rev, sizeof rev, "%s", st.rev);
    CHECK(digest(&s, st.wid, rev, sha, NULL) && strcmp(sha, ctl->pre_digest) == 0);

    for (n = 1; n <= 400u; n++) {
        {
            char num[32];
            snprintf(num, sizeof num, "%llu\n", (unsigned long long)n);
            put_token(tok, num);
        }
        if (plan(&s, st.wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r)) {
            CHECK(valid == 1);
            resp_free(&r);
            k_plan = n - 1u;
            break;
        }
        CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_RESOURCE_LIMIT") == 0);
        CHECK(token_gone(tok));
        CHECK(digest(&s, st.wid, rev, sha, NULL) && strcmp(sha, ctl->pre_digest) == 0);
        resp_free(&r);
    }
    CHECK(k_plan >= 3u);

    kill_active_worker(&s, st.wid, rev);
    CHECK(digest(&s, st.wid, rev, sha, NULL) && strcmp(sha, ctl->pre_digest) == 0);

    for (n = 1; n <= 400u; n++) {
        char num[32];
        snprintf(num, sizeof num, "%llu\n", (unsigned long long)n);
        put_token(tok, num);
        if (commit(&s, st.wid, pid, rev, st.cand, &r)) {
            str_of(resp_result(&r), "world_revision", rev, sizeof rev);
            resp_free(&r);
            k_prep = n - 1u;
            break;
        }
        CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_RESOURCE_LIMIT") == 0);
        CHECK(token_gone(tok));
        CHECK(digest(&s, st.wid, rev, sha, NULL) && strcmp(sha, ctl->pre_digest) == 0);
        if (n == 1u) {
            /* No commit was acknowledged, so there is no rollback target. The failed commit's reply is still held
             * in r; release it before call() overwrites r, or its text and parsed document leak. */
            resp_free(&r);
            snprintf(line, sizeof line,
                     "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":"
                     "\"%s\",\"expected_world_revision\":\"%s\"}}",
                     st.wid, rev);
            call(&s, line, &r);
            CHECK(!resp_ok(&r) && error_code(&r, code, sizeof code) && strcmp(code, "SU_E_STALE_REVISION") == 0);
        }
        resp_free(&r);
    }
    CHECK(k_prep >= 3u);

    CHECK(step(&s, st.wid, rev, sizeof rev, 2, &r));
    resp_free(&r);
    CHECK(digest(&s, st.wid, rev, sha, NULL) && strcmp(sha, ctl->final_digest) == 0);
    session_stop(&s);
}

/* C1: a commit that leaves an installed system neither resubmitted nor dropped is refused (systems_unresolved) and
 * changes nothing; the same migration with that system dropped commits. */
static void run_two_system_refusal(void) {
    Session s;
    Resp r;
    char wid[24];
    char rev[48];
    char cand[32];
    char pid[32];
    char phash[65];
    char code[64];
    char sha0[65];
    char sha[65];
    char line[1024];
    int valid = 0;

    s = session_start("spool-c1");
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"world4.json\","
             "\"capsule_paths\":[\"sys_a.capsule.json\",\"sys_b.capsule.json\"]}}");
    call(&s, line, &r);
    CHECK(resp_ok(&r) && str_of(resp_result(&r), "world_id", wid, sizeof wid) &&
          str_of(resp_result(&r), "world_revision", rev, sizeof rev));
    resp_free(&r);
    CHECK(step(&s, wid, rev, sizeof rev, 2, &r));
    resp_free(&r);
    CHECK(digest(&s, wid, rev, sha0, NULL));
    CHECK(submit(&s, "sys_a_gen2.capsule.json", cand, sizeof cand) && evaluate(&s, cand, "sys_a_gen2.test.json"));

    CHECK(plan(&s, wid, rev, "m_add.json", pid, sizeof pid, phash, sizeof phash, &valid, &r));
    CHECK(valid == 1);
    resp_free(&r);
    CHECK(!commit(&s, wid, pid, rev, cand, &r));
    CHECK(error_code(&r, code, sizeof code) && strcmp(code, "SU_E_UNSUPPORTED_MIGRATION") == 0);
    CHECK(strstr((const char *)r.text, "systems_unresolved") != NULL);
    resp_free(&r);
    CHECK(digest(&s, wid, rev, sha, NULL) && strcmp(sha, sha0) == 0);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"mp\",\"method\":\"world.migrate_plan\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"migration_path\":\"m_add.json\",\"drop_systems\":[\"demo.drift\"]}}",
             wid, rev);
    call(&s, line, &r);
    CHECK(resp_ok(&r) && str_of(resp_result(&r), "plan_id", pid, sizeof pid));
    resp_free(&r);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"mc\",\"method\":\"world.migrate_commit\",\"params\":{\"world_id\":\"%s\","
             "\"plan_id\":\"%s\",\"expected_world_revision\":\"%s\",\"resubmit\":[{\"system_id\":\"demo.movement\","
             "\"candidate_id\":\"%s\"}],\"drop_systems\":[\"demo.drift\"]}}",
             wid, pid, rev, cand);
    call(&s, line, &r);
    CHECK(resp_ok(&r));
    resp_free(&r);
    session_stop(&s);
}

int main(int argc, char **argv) {
    Outcome ctl;
    Outcome o1;
    Outcome o2;
    Outcome o3;
    Outcome o4;
    if (argc != 4) {
        fprintf(stderr, "usage: %s <recovery_worker> <repo_root> <scratch_dir>\n", argv[0]);
        return 2;
    }
    g_worker = argv[1];
    g_root = argv[2];
    snprintf(g_scratch, sizeof g_scratch, "%s", argv[3]);
    snprintf(g_proj, sizeof g_proj, "%s/project", g_scratch);
    mkdir(g_scratch, 0700);
    mkdir(g_proj, 0700);
    copy_from("tests/reload", "sys_a.capsule.json");
    copy_from("tests/reload", "sys_a.lir.json");
    copy_from("tests/reload", "sys_a.test.json");
    copy_from("tests/reload", "sys_a_gen2.capsule.json");
    copy_from("tests/reload", "sys_a_gen2.lir.json");
    copy_from("tests/reload", "sys_a_gen2.test.json");
    copy_from("tests/reload", "world4.json");
    copy_from("tests/reload", "sys_a_gen3.capsule.json");
    copy_from("tests/reload", "sys_a_gen3.test.json");
    copy_from("tests/reload", "sys_b.capsule.json");
    copy_from("tests/reload", "sys_b.lir.json");
    copy_from("tests/reload", "sys_b.test.json");
    overwrite_file("m_add.json",
                   "{\"migration_format\":\"0.1\",\"id\":\"m-add-health\",\"from_layout_generation\":1,"
                   "\"to_layout_generation\":2,\"ops\":[{\"op\":\"add_field\",\"field\":{\"id\":3,\"name\":\"health\","
                   "\"type\":\"u32\",\"default\":100}}]}");

    run_happy("control", NULL, &ctl, 0);
    CHECK(strcmp(ctl.commit_code, "ok") == 0);
    CHECK(strcmp(ctl.pre_digest, ctl.final_digest) != 0);
    CHECK(ctl.event_seen == 1);

    /* Crash before the swap: the commit is not acknowledged, the world is the pre-migration state. */
    run_happy("prepare", "migrate.prepare", &o1, 0);
    CHECK(strcmp(o1.commit_code, "SU_E_WORKER_FAILED") == 0);
    CHECK(strcmp(o1.final_digest, ctl.final_digest) == 0);

    run_happy("swap", "migrate.swap", &o2, 0);
    CHECK(strcmp(o2.commit_code, "SU_E_WORKER_FAILED") == 0);

    /* Crash during the post-commit checkpoint: recovery replays the logged migration; the commit completes. */
    run_happy("capture", "migrate.capture", &o3, 0);
    CHECK(strcmp(o3.commit_code, "ok") == 0);
    CHECK(o3.commit_recovery == 1);
    CHECK(strcmp(o3.final_digest, ctl.final_digest) == 0);

    /* Crash after the checkpoint: the first step after the commit recovers from the post-migration checkpoint. */
    run_happy("step", "migrate.step", &o4, 1);
    CHECK(strcmp(o4.commit_code, "ok") == 0);
    CHECK(o4.step_recovery == 1);
    CHECK(strcmp(o4.final_digest, ctl.final_digest) == 0);

    run_refusals();
    run_rollback();
    run_rollback_crash();
    run_rollback_deep();
    overwrite_file("m_shield3.json",
                   "{\"migration_format\":\"0.1\",\"id\":\"m-shield3\",\"from_layout_generation\":2,"
                   "\"to_layout_generation\":3,\"ops\":[{\"op\":\"add_field\",\"field\":{\"id\":4,\"name\":\"shield\","
                   "\"type\":\"u32\",\"default\":7}}]}");
    run_undo("double", &ctl);
    run_undo("noroom", &ctl);
    run_id_retirement();
    run_id_retirement_after_checkpoint();
    run_rollback_two_migrations();
    run_candidate_refusals();
    run_alloc_faults(&ctl);
    run_failed_second_commit("prepare");
    run_failed_second_commit("noroom");
    run_two_system_refusal();

    printf("migrate_runtime: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
