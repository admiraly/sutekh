/* M1-02 worker-crash recovery tests (supervisor-held checkpoints and replay log).
 *
 * Usage: test_recovery <recovery_worker> <repo_root> <scratch_dir>
 *   recovery_worker  tests/recovery/recovery_worker, built with -DSU_TEST_FAULT_HOOKS
 *   repo_root        absolute path of the repository (fixtures are copied from tests/reload)
 *   scratch_dir      writable directory: a project copy (so tests may change candidate source
 *                    on disk), per-session spool directories, and token files
 *
 * Every scenario runs an uninterrupted control session and a recovered session side by side.
 * After each step the canonical digest (sha256 of the snapshot bytes, which include the world
 * revision) must be identical. A worker is killed from outside with SIGKILL, or dies inside a
 * tick through the test-only crash seam. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../common/su_test_os.h"
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
    SuTestServe serve; /* supervisor under test (tests/common/su_test_os.h) */
    int wfd;
    SuLineReader rd;
} Session;

typedef struct Resp {
    uint8_t *text;
    size_t len;
    SuJsonDoc doc;
    const SuJsonValue *root;
} Resp;

typedef struct Opts {
    uint32_t interval;
    uint32_t retain;
    uint32_t log_max;
    uint32_t quarantine;
    int no_install_ck;
    uint64_t spool_max;
} Opts;

static Opts default_opts(void) {
    Opts o;
    memset(&o, 0, sizeof o);
    return o;
}

static void resp_free(Resp *r) {
    su_json_doc_free(&r->doc);
    free(r->text);
    memset(r, 0, sizeof *r);
}

static int resp_ok(const Resp *r) {
    const SuJsonValue *v = r->root != NULL ? su_json_member(r->root, "ok") : NULL;
    return v != NULL && v->type == SU_JSON_BOOL && v->u.boolean;
}

static const SuJsonValue *resp_result(const Resp *r) {
    return r->root != NULL ? su_json_member(r->root, "result") : NULL;
}

static const SuJsonValue *resp_member(const Resp *r, const char *key) {
    return r->root != NULL ? su_json_member(r->root, key) : NULL;
}

static uint64_t resp_error_status(const Resp *r) {
    const SuJsonValue *e;
    uint64_t v = 0;
    if (r->root == NULL || resp_ok(r)) {
        return UINT64_MAX;
    }
    e = su_json_member(r->root, "error");
    if (e == NULL || !su_json_get_uint(su_json_member(e, "status"), &v)) {
        return UINT64_MAX;
    }
    return v;
}

static uint64_t uint_of(const SuJsonValue *obj, const char *key) {
    uint64_t v = UINT64_MAX;
    const SuJsonValue *m = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    if (m == NULL || !su_json_get_uint(m, &v)) {
        return UINT64_MAX;
    }
    return v;
}

static int str_of(const SuJsonValue *obj, const char *key, char *dst, size_t cap) {
    const SuJsonValue *m = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    if (m == NULL || !su_json_is_string(m) || m->u.string.len >= cap) {
        if (cap > 0) {
            dst[0] = '\0';
        }
        return 0;
    }
    memcpy(dst, m->u.string.data, m->u.string.len);
    dst[m->u.string.len] = '\0';
    return 1;
}

/* Parent-side pipe ends of every session started so far. A later fork inherits them, so the
 * child closes them: otherwise closing one session's stdin would not reach EOF. */
static int g_hold[64];
static int g_hold_n = 0;

static Session session_start(const Opts *o, const char *spool_name) {
    Session s;
    int req[2];
    int rsp[2];
    static char spool[2200]; /* static: the supervisor may read cfg strings after this returns */
    SuAgentConfig cfg;
    static const char *argv[2];

    snprintf(spool, sizeof spool, "%s/%s", g_scratch, spool_name);
    mkdir(spool, 0700);
    argv[0] = g_worker;
    argv[1] = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.project_root = g_proj;
    cfg.worker_argv = argv;
    cfg.request_timeout_ms = 30000;
    cfg.checkpoint_interval_ticks = o->interval;
    cfg.checkpoint_retain = o->retain;
    cfg.log_max_entries = o->log_max;
    cfg.quarantine_after = o->quarantine;
    cfg.spool_bytes_max = o->spool_max;
    cfg.spool_dir = spool;
    cfg.no_install_checkpoint = o->no_install_ck;
    memset(&s, 0, sizeof s);
    su_test_serve_start_ex(&s.serve, &cfg, &req[1], &rsp[0], g_hold, g_hold_n);
    if (g_hold_n + 2 <= (int)(sizeof g_hold / sizeof g_hold[0])) {
        g_hold[g_hold_n++] = req[1];
        g_hold[g_hold_n++] = rsp[0];
    }
    s.wfd = req[1];
    su_line_reader_init(&s.rd, rsp[0], -1);
    return s;
}

static void session_stop(Session *s) {
    int k;
    close(s->wfd);
    for (k = 0; k < g_hold_n; k++) {
        if (g_hold[k] == s->wfd || g_hold[k] == s->rd.fd) {
            g_hold[k] = -1;
        }
    }
    su_line_reader_free(&s->rd);
    CHECK(su_test_serve_wait(&s->serve));
}

static int call(Session *s, const char *line, Resp *r) {
    const uint8_t *line_out = NULL;
    size_t len = 0;
    int overflow = 0;
    int rc;
    SuJsonLimits lim;
    SuJsonError jerr;
    uint32_t st;

    memset(r, 0, sizeof *r);
    if (su_write_all(s->wfd, (const uint8_t *)line, strlen(line), -1) != 0 ||
        su_write_all(s->wfd, (const uint8_t *)"\n", 1, -1) != 0) {
        return -1;
    }
    s->rd.deadline_ms = su_now_ms() + 120000;
    rc = su_line_next(&s->rd, &line_out, &len, &overflow);
    if (rc != 1) {
        return -1;
    }
    r->text = malloc(len + 1u);
    if (r->text == NULL) {
        return -1;
    }
    memcpy(r->text, line_out, len);
    r->text[len] = '\0';
    r->len = len;
    lim.max_bytes = len;
    lim.max_depth = 40;
    st = su_json_parse(r->text, len, &lim, &r->doc, &jerr);
    r->root = st == SU_OK && r->doc.root != NULL ? r->doc.root : NULL;
    if (getenv("RECOVERY_TRACE") != NULL) {
        fprintf(stderr, "> %s\n< %s\n", line, r->text);
    }
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

/* ---------- world and candidate helpers ---------- */

static uint64_t worker_pid_of(Session *s) {
    Resp r;
    uint64_t pid = 0;
    if (call(s, "{\"protocol\":\"0.1\",\"id\":\"ei\",\"method\":\"engine.info\",\"params\":{}}", &r) != 0) {
        resp_free(&r);
        return 0;
    }
    if (resp_ok(&r)) {
        const SuJsonValue *aw = su_json_member(resp_result(&r), "active_worker");
        pid = uint_of(aw, "pid");
    }
    resp_free(&r);
    return pid;
}

static int create_world(Session *s, int two, char *wid, size_t wcap, char *rev, size_t rcap) {
    Resp r;
    char line[1024];
    int ok;
    if (two) {
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
                 "\"world4.json\",\"capsule_paths\":[\"sys_a.capsule.json\",\"sys_b.capsule.json\"]}}");
    } else {
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
                 "\"world4.json\",\"capsule_path\":\"sys_a.capsule.json\"}}");
    }
    call(s, line, &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_id", wid, wcap) &&
         str_of(resp_result(&r), "world_revision", rev, rcap);
    resp_free(&r);
    return ok;
}

static int create_world_sys_b(Session *s, char *wid, size_t wcap, char *rev, size_t rcap) {
    Resp r;
    int ok;
    call(s,
         "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
         "\"world4.json\",\"capsule_path\":\"sys_b.capsule.json\"}}",
         &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_id", wid, wcap) &&
         str_of(resp_result(&r), "world_revision", rev, rcap);
    resp_free(&r);
    return ok;
}

/* Steps a world. Returns 1 when the worker accepted the step. rev is updated on success. */
static void copy_file(const char *name) {
    copy_from("tests/reload", name);
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

/* Canonical digest of the world at rev: the sha256 of its snapshot bytes. */
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

/* Installs a candidate at the world's current tick. Returns 1 on an installed result. */
static int install(Session *s, const char *cand, const char *wid, const char *rev, uint64_t tick, Resp *r) {
    char line[640];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ins\",\"method\":\"candidate.install\",\"params\":{\"candidate_id\":"
             "\"%s\",\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"allowed_effective_tick\":%llu}}",
             cand, wid, rev, (unsigned long long)tick);
    call(s, line, r);
    ok = resp_ok(r);
    return ok;
}

/* ---------- a control/recovered pair ---------- */

typedef struct Pair {
    Session ctl;
    Session rec;
    char wid[24];
    char crev[48];
    char rrev[48];
} Pair;

static void pair_open(Pair *p, const Opts *o, const char *tag) {
    char name[64];
    snprintf(name, sizeof name, "spool-%s-ctl", tag);
    p->ctl = session_start(o, name);
    snprintf(name, sizeof name, "spool-%s-rec", tag);
    p->rec = session_start(o, name);
    memset(p->wid, 0, sizeof p->wid);
}

static void pair_close(Pair *p) {
    session_stop(&p->ctl);
    session_stop(&p->rec);
}

/* Creates the same world in both sessions. */
static void pair_create(Pair *p, int two) {
    char w2[24];
    CHECK(create_world(&p->ctl, two, p->wid, sizeof p->wid, p->crev, sizeof p->crev));
    CHECK(create_world(&p->rec, two, w2, sizeof w2, p->rrev, sizeof p->rrev));
    CHECK(strcmp(p->wid, w2) == 0);
    CHECK(strcmp(p->crev, p->rrev) == 0);
}

/* Steps both sessions (the recovered one must accept the step) and checks the digests agree. */
static void pair_step(Pair *p, uint64_t ticks) {
    Resp r;
    CHECK(step(&p->ctl, p->wid, p->crev, sizeof p->crev, ticks, &r));
    resp_free(&r);
    CHECK(step(&p->rec, p->wid, p->rrev, sizeof p->rrev, ticks, &r));
    resp_free(&r);
    CHECK(strcmp(p->crev, p->rrev) == 0);
}

/* The canonical digests of both worlds are equal at the current revision. */
static void pair_check(Pair *p) {
    char a[65];
    char b[65];
    uint64_t ta = 0;
    uint64_t tb = 0;
    CHECK(digest(&p->ctl, p->wid, p->crev, a, &ta));
    CHECK(digest(&p->rec, p->wid, p->rrev, b, &tb));
    CHECK(strcmp(a, b) == 0);
    CHECK(ta == tb);
    CHECK(strcmp(p->crev, p->rrev) == 0);
}

/* Kills the recovered session's active worker from outside and returns its pid. */
static uint64_t pair_kill(Pair *p) {
    uint64_t pid = worker_pid_of(&p->rec);
    CHECK(pid > 0);
    if (pid > 0) {
        (void)su_test_kill_pid(pid);
    }
    return pid;
}

/* Checks the recovery object of a failed request and the worker replacement. */
static void expect_recovery(const Resp *r, uint64_t old_pid, int want_recovered) {
    const SuJsonValue *rec = resp_member(r, "recovery");
    const SuJsonValue *ok;
    uint64_t newpid;
    CHECK(rec != NULL);
    if (rec == NULL) {
        return;
    }
    ok = su_json_member(rec, "recovered");
    CHECK(ok != NULL && ok->type == SU_JSON_BOOL && ok->u.boolean == want_recovered);
    CHECK(uint_of(rec, "replaced_pid_old") == old_pid);
    newpid = uint_of(rec, "replaced_pid_new");
    CHECK(newpid != 0 && newpid != old_pid && newpid != UINT64_MAX);
    CHECK(uint_of(rec, "worlds_recovered") != UINT64_MAX);
    CHECK(uint_of(rec, "duration_ms") != UINT64_MAX);
    CHECK(uint_of(rec, "worker_generation") != UINT64_MAX);
    if (newpid != 0 && newpid != UINT64_MAX) {
        CHECK(su_test_pid_alive(newpid));
        /* The replacement is a child of the supervisor process: it has a readable parent. */
        CHECK(su_test_parent_pid(newpid) > 0);
    }
}

/* A request sent after a worker death: must fail with SU_E_WORKER_FAILED and a recovery object. */
static void expect_worker_failed(Resp *r) {
    CHECK(!resp_ok(r));
    CHECK(resp_error_status(r) == SU_E_WORKER_FAILED);
}

/* ---------- scenarios ---------- */

/* SIGKILL the active worker after accepted steps; the next step reports the recovery and then
 * both sessions advance one tick at a time with equal digests. Two worlds with different schedules
 * are recovered together. */
static void scenario_after_step(void) {
    Opts o = default_opts();
    Pair p;
    Pair q;
    Resp r;
    uint64_t old_pid;
    int i;

    pair_open(&p, &o, "step");
    pair_create(&p, 1);
    pair_step(&p, 3);
    pair_check(&p);
    old_pid = pair_kill(&p);

    /* The request that finds the dead worker is not executed; it reports the recovery. */
    step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 2, &r);
    expect_worker_failed(&r);
    expect_recovery(&r, old_pid, 1);
    /* The digests taken before the kill are checkpoints (world.snapshot): the newest is the base. */
    CHECK(uint_of(resp_member(&r, "recovery"), "from_checkpoint_tick") == 3u);
    resp_free(&r);
    CHECK(worker_pid_of(&p.rec) != old_pid);
    pair_step(&p, 2);
    pair_check(&p);
    for (i = 0; i < 4; i++) {
        pair_step(&p, 1);
        pair_check(&p);
    }
    pair_close(&p);

    /* Two worlds with different schedules: the first step after the kill reports one recovery. */
    pair_open(&q, &o, "two");
    pair_create(&q, 1);
    {
        char w2c[24];
        char w2cr[48];
        char w2rr[48];
        CHECK(create_world_sys_b(&q.ctl, w2c, sizeof w2c, w2cr, sizeof w2cr));
        CHECK(create_world_sys_b(&q.rec, w2c, sizeof w2c, w2rr, sizeof w2rr));
        CHECK(strcmp(w2cr, w2rr) == 0);
        pair_step(&q, 2);
        CHECK(step(&q.ctl, w2c, w2cr, sizeof w2cr, 4, &r));
        resp_free(&r);
        CHECK(step(&q.rec, w2c, w2rr, sizeof w2rr, 4, &r));
        resp_free(&r);
        pair_check(&q);
        old_pid = pair_kill(&q);
        CHECK(step(&q.rec, q.wid, q.rrev, sizeof q.rrev, 1, &r) == 0);
        expect_worker_failed(&r);
        expect_recovery(&r, old_pid, 1);
        CHECK(uint_of(resp_member(&r, "recovery"), "worlds_recovered") == 2u);
        resp_free(&r);
        pair_step(&q, 1);
        pair_check(&q);
        {
            char a[65];
            char b[65];
            CHECK(digest(&q.ctl, w2c, w2cr, a, NULL));
            CHECK(digest(&q.rec, w2c, w2rr, b, NULL));
            CHECK(strcmp(a, b) == 0);
        }
    }
    pair_close(&q);
}

/* SIGKILL after a candidate was installed: the replacement rebuilds the swapped revision from the
 * supervisor's spool, and the next steps follow the swapped semantics. */
static void scenario_after_install(void) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    char cand_c[32];
    char cand_r[32];
    uint64_t old_pid;

    pair_open(&p, &o, "inst");
    pair_create(&p, 1);
    pair_step(&p, 2);
    CHECK(submit(&p.ctl, "sys_a_v2.capsule.json", cand_c, sizeof cand_c));
    CHECK(submit(&p.rec, "sys_a_v2.capsule.json", cand_r, sizeof cand_r));
    CHECK(evaluate(&p.ctl, cand_c, "sys_a_v2.test.json"));
    CHECK(evaluate(&p.rec, cand_r, "sys_a_v2.test.json"));
    CHECK(install(&p.ctl, cand_c, p.wid, p.crev, 2, &r));
    CHECK(str_of(resp_result(&r), "world_revision", p.crev, sizeof p.crev));
    resp_free(&r);
    CHECK(install(&p.rec, cand_r, p.wid, p.rrev, 2, &r));
    CHECK(str_of(resp_result(&r), "world_revision", p.rrev, sizeof p.rrev));
    resp_free(&r);
    CHECK(strcmp(p.crev, p.rrev) == 0);
    pair_step(&p, 2);
    pair_check(&p);
    old_pid = pair_kill(&p);
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 2, &r) == 0);
    expect_worker_failed(&r);
    expect_recovery(&r, old_pid, 1);
    resp_free(&r);
    pair_step(&p, 2);
    pair_check(&p);
    pair_close(&p);
}

/* Replay of an installed code swap from the creation checkpoint: with the post-install checkpoint
 * disabled, the install is in the replay log and must be re-applied at its recorded tick. */
static void scenario_replay_install(int modify_disk) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    char cand_c[32];
    char cand_r[32];
    uint64_t old_pid;

    o.no_install_ck = 1;
    pair_open(&p, &o, modify_disk ? "repl-disk" : "repl");
    pair_create(&p, 1);
    pair_step(&p, 2);
    CHECK(submit(&p.ctl, "sys_a_v2.capsule.json", cand_c, sizeof cand_c));
    CHECK(submit(&p.rec, "sys_a_v2.capsule.json", cand_r, sizeof cand_r));
    CHECK(evaluate(&p.ctl, cand_c, "sys_a_v2.test.json"));
    CHECK(evaluate(&p.rec, cand_r, "sys_a_v2.test.json"));
    CHECK(install(&p.ctl, cand_c, p.wid, p.crev, 2, &r));
    CHECK(str_of(resp_result(&r), "world_revision", p.crev, sizeof p.crev));
    resp_free(&r);
    CHECK(install(&p.rec, cand_r, p.wid, p.rrev, 2, &r));
    CHECK(str_of(resp_result(&r), "world_revision", p.rrev, sizeof p.rrev));
    resp_free(&r);
    pair_step(&p, 3); /* no digest here: a checkpoint would hide the replay under test */
    if (modify_disk) {
        /* The installed bytes are the supervisor's copy: changing the file on disk afterwards must
         * not change the replay. (The control session never reads it again.) */
        overwrite_file("sys_a_v2.lir.json", "{\"not\":\"the installed source\"}\n");
    }
    old_pid = pair_kill(&p);
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 1, &r) == 0);
    expect_worker_failed(&r);
    expect_recovery(&r, old_pid, 1);
    /* Base: the creation checkpoint (tick 0). Replayed: step 2, the install, step 3. */
    CHECK(uint_of(resp_member(&r, "recovery"), "from_checkpoint_tick") == 0u);
    CHECK(uint_of(resp_member(&r, "recovery"), "ops_replayed") == 3u);
    resp_free(&r);
    /* The recovered world runs the swapped semantics from the recorded tick: the digest at each tick
     * (checked in pair_check) must match the control's. */
    pair_step(&p, 1);
    pair_check(&p);
    pair_step(&p, 2);
    pair_check(&p);
    pair_close(&p);
    if (modify_disk) {
        copy_file("sys_a_v2.lir.json"); /* later scenarios use the original fixture */
    }
}

/* SIGKILL after a snapshot and a restore: the restored state, its revision and the schedule are
 * recovered. */
static void scenario_after_restore(void) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    char snap_c[65];
    char snap_r[65];
    char rev_before[48];
    uint64_t old_pid;

    pair_open(&p, &o, "rest");
    pair_create(&p, 1);
    pair_step(&p, 2);
    CHECK(digest(&p.ctl, p.wid, p.crev, snap_c, NULL));
    CHECK(digest(&p.rec, p.wid, p.rrev, snap_r, NULL));
    CHECK(strcmp(snap_c, snap_r) == 0);
    {
        char line[512];
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"sn\",\"method\":\"world.snapshot\",\"params\":{\"world_id\":\"%s\","
                 "\"expected_world_revision\":\"%s\"}}",
                 p.wid, p.rrev);
        call(&p.rec, line, &r);
        CHECK(resp_ok(&r));
        CHECK(str_of(resp_result(&r), "snapshot_id", snap_r, sizeof snap_r));
        resp_free(&r);
    }
    strcpy(rev_before, p.rrev);
    pair_step(&p, 3);
    {
        char line[512];
        Resp rc;
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"rs\",\"method\":\"world.restore\",\"params\":{\"world_id\":\"%s\","
                 "\"snapshot_id\":\"%s\",\"expected_world_revision\":\"%s\"}}",
                 p.wid, snap_r, p.rrev);
        call(&p.rec, line, &r);
        CHECK(resp_ok(&r));
        str_of(resp_result(&r), "world_revision", p.rrev, sizeof p.rrev);
        resp_free(&r);
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"rs\",\"method\":\"world.restore\",\"params\":{\"world_id\":\"%s\","
                 "\"snapshot_id\":\"%s\",\"expected_world_revision\":\"%s\"}}",
                 p.wid, snap_c, p.crev);
        call(&p.ctl, line, &rc);
        CHECK(resp_ok(&rc));
        str_of(resp_result(&rc), "world_revision", p.crev, sizeof p.crev);
        resp_free(&rc);
    }
    CHECK(strcmp(p.crev, p.rrev) == 0);
    pair_check(&p);
    old_pid = pair_kill(&p);
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 1, &r) == 0);
    expect_worker_failed(&r);
    expect_recovery(&r, old_pid, 1);
    resp_free(&r);
    pair_step(&p, 1);
    pair_check(&p);
    pair_close(&p);
}

/* The worker dies inside a tick (test-only crash seam, one shot): the failed step is not
 * acknowledged, and the recovered state is the last acknowledged one. */
static void scenario_mid_step(void) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    char token[4200];
    FILE *tf;
    uint64_t old_pid;

    /* The control run comes first and without the seam. */
    pair_open(&p, &o, "mid");
    pair_create(&p, 1);
    pair_step(&p, 3);
    pair_check(&p);
    pair_close(&p);

    snprintf(token, sizeof token, "%s/mid.token", g_scratch);
    tf = fopen(token, "w");
    CHECK(tf != NULL);
    if (tf != NULL) {
        fputs("once\n", tf);
        fclose(tf);
    }
    {
        char tick_s[16];
        snprintf(tick_s, sizeof tick_s, "%d", 5);
        setenv("SUTEKH_TEST_KILL_AT_TICK", tick_s, 1);
        setenv("SUTEKH_TEST_KILL_TOKEN", token, 1);
    }
    pair_open(&p, &o, "mid2");
    pair_create(&p, 1);
    pair_step(&p, 3);
    /* This step would compute tick 5 (the crash seam fires once); the request fails. */
    old_pid = worker_pid_of(&p.rec);
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 4, &r) == 0);
    expect_worker_failed(&r);
    expect_recovery(&r, old_pid, 1);
    CHECK(uint_of(resp_member(&r, "recovery"), "from_checkpoint_tick") == 0u);
    resp_free(&r);
    unsetenv("SUTEKH_TEST_KILL_AT_TICK");
    unsetenv("SUTEKH_TEST_KILL_TOKEN");
    /* The retry is safe: the revision is the acknowledged one (3 ticks), and the seam is spent. */
    pair_step(&p, 4);
    CHECK(strcmp(p.rrev, p.crev) == 0);
    pair_check(&p);
    pair_close(&p);
}

/* ---------- control batches on an RTS world (M3-05, OD-3, OD-3b) ---------- */

static int ent_despawn(Session *s, const char *wid, char *rev, size_t rcap, uint64_t handle);
static void ent_events_text(Session *s, const char *wid, char *out, size_t cap);

#define RTS_ROWS 8u
#define RTS_GEN1 4294967296ull

/* The 18 host fields of capsules/rts/host/spatial.capsule.json. */
static const char *const k_rts_fields[] = {
    "kind", "team", "hp", "pos_x", "pos_y", "range_sq", "formation_slot", "formation_gen", "anchor_x", "anchor_y",
    "anchor_valid", "target_slot", "target_gen", "target_dist_sq", "target_valid", "target_x", "target_y", "truncated",
};

/* rts_world.json: eight units with hp 10, no formation and no target, in the project copy. */
static void write_rts_fixture(void) {
    char path[4400];
    FILE *fh;
    uint32_t r;
    size_t k;
    snprintf(path, sizeof path, "%s/rts_world.json", g_proj);
    fh = fopen(path, "wb");
    if (fh == NULL) {
        perror(path);
        exit(2);
    }
    fprintf(fh, "{\"fixture_format\":\"0.1\",\"schema_id\":\"rts\",\"schema_version\":1,\"tick\":0,\"entity_ids\":[");
    for (r = 0; r < RTS_ROWS; r++) {
        fprintf(fh, "%s%u", r ? "," : "", r + 1u);
    }
    fprintf(fh, "],\"columns\":{");
    for (k = 0; k < sizeof k_rts_fields / sizeof k_rts_fields[0]; k++) {
        fprintf(fh, "%s\"%s\":[", k ? "," : "", k_rts_fields[k]);
        for (r = 0; r < RTS_ROWS; r++) {
            uint32_t v = 0;
            if (strcmp(k_rts_fields[k], "kind") == 0) v = 1u;
            if (strcmp(k_rts_fields[k], "hp") == 0) v = 10u;
            if (strcmp(k_rts_fields[k], "pos_x") == 0) v = 100u * r;
            if (strcmp(k_rts_fields[k], "range_sq") == 0) v = 400u;
            if (strcmp(k_rts_fields[k], "formation_slot") == 0 || strcmp(k_rts_fields[k], "target_slot") == 0) {
                v = 4294967295u;
            }
            fprintf(fh, "%s%u", r ? "," : "", v);
        }
        fprintf(fh, "]");
    }
    fprintf(fh, "}}");
    fclose(fh);
}

static int create_rts_world(Session *s, char *wid, size_t wcap, char *rev, size_t rcap) {
    Resp r;
    int ok;
    call(s,
         "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
         "\"rts_world.json\",\"capsule_paths\":[{\"capsule_path\":\"spatial.capsule.json\",\"host_operator\":"
         "\"rts.spatial\"}]}}",
         &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_id", wid, wcap) &&
         str_of(resp_result(&r), "world_revision", rev, rcap);
    resp_free(&r);
    return ok;
}

/* world.set_fields or world.spawn_batch with key (entities or rows) = arr. Returns the error status (0 when
 * accepted, then rev is updated). */
static uint64_t ctl_batch(Session *s, const char *method, const char *key, const char *wid, char *rev,
                          size_t rcap, const char *arr) {
    Resp r;
    char line[4096];
    uint64_t st = 0;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"cb\",\"method\":\"%s\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"%s\":%s}}",
             method, wid, rev, key, arr);
    call(s, line, &r);
    if (resp_ok(&r)) {
        CHECK(str_of(resp_result(&r), "world_revision", rev, rcap));
    } else {
        st = resp_error_status(&r);
    }
    resp_free(&r);
    return st;
}

/* Applies the same batch to both sessions; both must answer want and agree on the revision. */
static void pair_batch(Pair *p, const char *method, const char *key, const char *arr, uint64_t want) {
    uint64_t a = ctl_batch(&p->ctl, method, key, p->wid, p->crev, sizeof p->crev, arr);
    uint64_t b = ctl_batch(&p->rec, method, key, p->wid, p->rrev, sizeof p->rrev, arr);
    CHECK(a == want);
    CHECK(b == want);
    if (a != want || b != want) {
        fprintf(stderr, "%s %s: want %llu, got %llu / %llu\n", method, arr, (unsigned long long)want,
                (unsigned long long)a, (unsigned long long)b);
    }
    CHECK(strcmp(p->crev, p->rrev) == 0);
}

/* An RTS world (spatial host system) created through the supervisor; slots 4..7 despawned, refilled by one
 * world.spawn_batch, two rows changed by one world.set_fields, refusals in between. The worker is killed and
 * the replacement must reach the same digest and event log. interval 0 keeps every operation in the replay
 * log (the creation capture is the base); interval 1 checkpoints after one step, so the host entry is also
 * restored from a checkpoint record. */
static void scenario_control_batches(uint32_t interval, const char *tag) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    char ev_c[16384];
    char ev_r[16384];
    char wid_r[24];
    char line[512];
    uint64_t old_pid;
    uint32_t k;

    o.interval = interval == 0u ? 1000000u : interval;
    pair_open(&p, &o, tag);
    CHECK(create_rts_world(&p.ctl, p.wid, sizeof p.wid, p.crev, sizeof p.crev));
    CHECK(create_rts_world(&p.rec, wid_r, sizeof wid_r, p.rrev, sizeof p.rrev));
    CHECK(strcmp(p.wid, wid_r) == 0 && strcmp(p.crev, p.rrev) == 0);
    if (interval != 0u) {
        pair_step(&p, 1);
    }
    for (k = 4; k < RTS_ROWS; k++) {
        uint64_t h = RTS_GEN1 + k;
        CHECK(ent_despawn(&p.ctl, p.wid, p.crev, sizeof p.crev, h));
        CHECK(ent_despawn(&p.rec, p.wid, p.rrev, sizeof p.rrev, h));
    }
    /* Refused: five rows into four free slots; a domain-invalid row; an unknown item key. */
    pair_batch(&p, "world.spawn_batch", "rows",
               "[{\"values\":{\"kind\":1,\"hp\":1}},{\"values\":{\"kind\":1,\"hp\":1}},{\"values\":{\"kind\":1,\"hp\":1}},"
               "{\"values\":{\"kind\":1,\"hp\":1}},{\"values\":{\"kind\":1,\"hp\":1}}]",
               SU_E_RESOURCE_LIMIT);
    pair_batch(&p, "world.spawn_batch", "rows", "[{\"values\":{\"kind\":1,\"hp\":0}}]", SU_E_TYPE);
    pair_batch(&p, "world.spawn_batch", "rows", "[{\"values\":{\"kind\":1,\"hp\":1},\"extra\":1}]", SU_E_TYPE);
    /* Accepted. */
    pair_batch(&p, "world.spawn_batch", "rows",
               "[{\"values\":{\"kind\":1,\"hp\":7,\"pos_x\":40,\"range_sq\":900},\"external_id\":77},"
               "{\"values\":{\"kind\":2,\"team\":1,\"pos_x\":50}},"
               "{\"values\":{\"kind\":1,\"hp\":9,\"pos_x\":60,\"formation_slot\":2,\"formation_gen\":2}},"
               "{\"values\":{\"kind\":1,\"hp\":3,\"team\":1,\"pos_x\":70}}]",
               0u);
    pair_batch(&p, "world.set_fields", "entities", "[{\"entity\":4294967296,\"values\":{\"kind\":2}}]", SU_E_TYPE);
    pair_batch(&p, "world.set_fields", "entities",
               "[{\"entity\":4294967296,\"values\":{\"pos_x\":5,\"pos_y\":6}},"
               "{\"entity\":4294967297,\"values\":{\"formation_slot\":5,\"formation_gen\":2,\"range_sq\":1}}]",
               0u);
    /* No digest here: world.snapshot would take a checkpoint, and the batches must come from the replay log. */

    old_pid = pair_kill(&p);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"in\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\","
             "\"limit\":1}}",
             p.wid);
    call(&p.rec, line, &r);
    expect_worker_failed(&r);
    expect_recovery(&r, old_pid, 1);
    resp_free(&r);
    pair_check(&p);
    ent_events_text(&p.ctl, p.wid, ev_c, sizeof ev_c);
    ent_events_text(&p.rec, p.wid, ev_r, sizeof ev_r);
    CHECK(strlen(ev_c) > 0 && strcmp(ev_c, ev_r) == 0);

    /* Both sessions continue identically. */
    pair_batch(&p, "world.set_fields", "entities", "[{\"entity\":4294967298,\"values\":{\"pos_y\":3}}]", 0u);
    pair_step(&p, 2);
    pair_check(&p);
    pair_close(&p);
}

/* ---------- host-operator fault seams on the full RTS world (M3 T-RTS-REPLAY-3) ---------- */

/* The 39 fields of contract M3 section 2. */
static const char *const k_rts_full[] = {
    "kind", "team", "hp", "pos_x", "pos_y", "speed", "range_sq", "reload", "cooldown", "damage", "flight",
    "order_mode", "order_x", "order_y", "formation_slot", "formation_gen", "formation_ox", "formation_oy", "goal_x",
    "goal_y", "anchor_x", "anchor_y", "anchor_valid", "target_slot", "target_gen", "target_dist_sq", "target_valid",
    "target_x", "target_y", "truncated", "proj_active", "proj_left", "proj_target", "proj_target_gen", "proj_damage",
    "dmg_out", "dmg_target", "dmg_target_gen", "incoming",
};

/* rts_full_empty.json (39 fields, no rows) and the six RTS capsules, in the project copy. */
static void write_rts_full_project(void) {
    static const char *const ir[] = {"goal", "combat", "move", "damage"};
    char path[4400];
    char name[64];
    FILE *fh;
    size_t k;
    for (k = 0; k < sizeof ir / sizeof ir[0]; k++) {
        snprintf(name, sizeof name, "%s.capsule.json", ir[k]);
        copy_from("capsules/rts", name);
        snprintf(name, sizeof name, "%s.lir.json", ir[k]);
        copy_from("capsules/rts", name);
    }
    copy_from("capsules/rts/host", "scatter.capsule.json");
    snprintf(path, sizeof path, "%s/rts_full_empty.json", g_proj);
    fh = fopen(path, "wb");
    if (fh == NULL) {
        perror(path);
        exit(2);
    }
    fprintf(fh, "{\"fixture_format\":\"0.1\",\"schema_id\":\"rts\",\"schema_version\":1,\"tick\":0,\"entity_ids\":[],"
                "\"columns\":{");
    for (k = 0; k < sizeof k_rts_full / sizeof k_rts_full[0]; k++) {
        fprintf(fh, "%s\"%s\":[]", k ? "," : "", k_rts_full[k]);
    }
    fprintf(fh, "}}");
    fclose(fh);
}

/* The full six-system RTS world of contract 3.2 with capacity 64 and per-tick caps 64 (9.4). */
static int create_rts_full(Session *s, char *wid, size_t wcap, char *rev, size_t rcap) {
    Resp r;
    int ok;
    call(s,
         "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
         "\"rts_full_empty.json\",\"capsule_paths\":[{\"capsule_path\":\"spatial.capsule.json\",\"host_operator\":"
         "\"rts.spatial\"},\"goal.capsule.json\",\"combat.capsule.json\",\"move.capsule.json\",{\"capsule_path\":"
         "\"scatter.capsule.json\",\"host_operator\":\"rts.scatter\"},\"damage.capsule.json\"],\"max_entities\":64,"
         "\"max_struct_per_tick\":64,\"max_events_per_tick\":64}}",
         &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_id", wid, wcap) &&
         str_of(resp_result(&r), "world_revision", rev, rcap);
    if (!ok && r.text != NULL) {
        fprintf(stderr, "create_rts_full: %s\n", (const char *)r.text);
    }
    resp_free(&r);
    return ok;
}

/* Two duelling pairs (contract M3 7.4 weapons are synthetic): opposite teams on one spot, in range, so combat fires
 * and scatter delivers damage every few ticks. */
static const char *const k_duels =
    "[{\"values\":{\"kind\":1,\"team\":0,\"hp\":40,\"pos_x\":100,\"pos_y\":100,\"range_sq\":400,\"reload\":2,"
    "\"damage\":3,\"flight\":2}},{\"values\":{\"kind\":1,\"team\":1,\"hp\":40,\"pos_x\":100,\"pos_y\":100,"
    "\"range_sq\":400,\"reload\":3,\"damage\":2,\"flight\":1}},{\"values\":{\"kind\":1,\"team\":0,\"hp\":30,"
    "\"pos_x\":5000,\"pos_y\":100,\"range_sq\":400,\"reload\":1,\"damage\":1,\"flight\":3}},{\"values\":{\"kind\":1,"
    "\"team\":1,\"hp\":30,\"pos_x\":5000,\"pos_y\":100,\"range_sq\":400,\"reload\":2,\"damage\":2,\"flight\":2}}]";

/* A host operator fails with SU_E_INTERNAL inside the tick that would compute tick 8 (one shot). The supervisor
 * treats it as an in-process fault: the worker is discarded and the world is rebuilt in a replacement from the
 * checkpoint at tick 4 plus the log; the retried step and the rest of the run equal a control session. */
static void scenario_host_fault(const char *system_id, const char *tag) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    char token[4200];
    char wid_r[24];
    char ev_c[16384];
    char ev_r[16384];
    FILE *tf;
    uint64_t old_pid;
    const SuJsonValue *rec;

    o.interval = 4;
    snprintf(token, sizeof token, "%s/host-%s.token", g_scratch, tag);
    tf = fopen(token, "w");
    CHECK(tf != NULL);
    if (tf != NULL) {
        fputs("once\n", tf);
        fclose(tf);
    }
    setenv("SUTEKH_TEST_HOST_FAULT_SYSTEM", system_id, 1);
    setenv("SUTEKH_TEST_HOST_FAULT_TICK", "8", 1);
    setenv("SUTEKH_TEST_HOST_FAULT_TOKEN", token, 1);
    pair_open(&p, &o, tag);
    CHECK(create_rts_full(&p.ctl, p.wid, sizeof p.wid, p.crev, sizeof p.crev));
    CHECK(create_rts_full(&p.rec, wid_r, sizeof wid_r, p.rrev, sizeof p.rrev));
    CHECK(strcmp(p.wid, wid_r) == 0 && strcmp(p.crev, p.rrev) == 0);
    pair_batch(&p, "world.spawn_batch", "rows", k_duels, 0u);
    pair_step(&p, 4); /* checkpoint at tick 4 (interval) */
    pair_step(&p, 2); /* ticks 5-6: one logged step */

    /* The recovered session reaches tick 8 first, so it takes the one-shot seam. */
    old_pid = worker_pid_of(&p.rec);
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 4, &r) == 0);
    expect_worker_failed(&r);
    expect_recovery(&r, old_pid, 1);
    rec = resp_member(&r, "recovery");
    CHECK(uint_of(rec, "from_checkpoint_tick") == 4u);
    CHECK(uint_of(rec, "ops_replayed") >= 1u);
    resp_free(&r);
    CHECK(access(token, F_OK) != 0); /* the seam fired */
    unsetenv("SUTEKH_TEST_HOST_FAULT_SYSTEM");
    unsetenv("SUTEKH_TEST_HOST_FAULT_TICK");
    unsetenv("SUTEKH_TEST_HOST_FAULT_TOKEN");

    /* Retry: the revision is the acknowledged one (tick 6). */
    pair_step(&p, 4);
    pair_check(&p);
    ent_events_text(&p.ctl, p.wid, ev_c, sizeof ev_c);
    ent_events_text(&p.rec, p.wid, ev_r, sizeof ev_r);
    CHECK(strlen(ev_c) > 0 && strcmp(ev_c, ev_r) == 0);
    pair_step(&p, 6);
    pair_check(&p);
    pair_close(&p);
}

/* Repeated crashes at the same tick: the first recovery retries; the second, from the same failing
 * operation, quarantines the installed revision, reverts the world to the newest checkpoint that
 * predates it, and reports the quarantine on the candidate. The crash seam only fires while the
 * quarantined source is installed. */
/* ---------- entity despawn history (M2-00 audit MX18) ---------- */

/* world.spawn between ticks; on success fills the handle, slot and generation and updates rev. */
static int ent_spawn(Session *s, const char *wid, char *rev, size_t rcap, const char *values, uint64_t *handle,
                     uint64_t *slot, uint64_t *gen) {
    Resp r;
    char line[768];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"es\",\"method\":\"world.spawn\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"values\":%s}}",
             wid, rev, values);
    call(s, line, &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_revision", rev, rcap);
    if (ok) {
        *handle = uint_of(resp_result(&r), "entity");
        *slot = uint_of(resp_result(&r), "slot");
        *gen = uint_of(resp_result(&r), "generation");
    }
    resp_free(&r);
    return ok;
}

/* world.despawn of a handle between ticks: returns 1 on success and updates rev. */
static int ent_despawn(Session *s, const char *wid, char *rev, size_t rcap, uint64_t handle) {
    Resp r;
    char line[512];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ed\",\"method\":\"world.despawn\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"entity\":%llu}}",
             wid, rev, (unsigned long long)handle);
    call(s, line, &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_revision", rev, rcap);
    resp_free(&r);
    return ok;
}

/* The whole events.read reply line (no worker pid in it), for an exact comparison of two sessions. */
static void ent_events_text(Session *s, const char *wid, char *out, size_t cap) {
    Resp r;
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ev\",\"method\":\"events.read\",\"params\":{\"world_id\":\"%s\","
             "\"cursor\":0,\"max\":256}}",
             wid);
    call(s, line, &r);
    snprintf(out, cap, "%s", r.text != NULL ? (const char *)r.text : "");
    resp_free(&r);
}

/* Entity creation for both sessions (same fixture and capsule as the harness TX7 world). */
static int create_entity_world(Session *s, char *wid, size_t wcap, char *rev, size_t rcap) {
    Resp r;
    int ok;
    call(s,
         "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
         "\"actors_3.json\",\"capsule_path\":\"despawn_move.capsule.json\",\"max_entities\":8}}",
         &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_id", wid, wcap) &&
         str_of(resp_result(&r), "world_revision", rev, rcap);
    resp_free(&r);
    return ok;
}

/* Despawn history with no checkpoint after it. Checkpoints are disabled (interval far above the ticks
 * used), so the creation capture is the only checkpoint and every control-plane spawn and despawn lives
 * in the replay log. Slot 3 is spawned, despawned, reused (generation 2) and despawned again; slot 4 is
 * spawned. The worker is killed after the last despawn: the replacement must rebuild handles, generations,
 * the event log and the canonical digest exactly as the uninterrupted control session. */
static void scenario_despawn_replay(void) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    char ev_c[16384];
    char ev_r[16384];
    char dig_c[65];
    char dig_r[65];
    char line[512];
    uint64_t hA = 0;
    uint64_t hB = 0;
    uint64_t hC = 0;
    uint64_t sA = 0;
    uint64_t sB = 0;
    uint64_t gA = 0;
    uint64_t gB = 0;
    uint64_t gC = 0;
    uint64_t sC = 0;
    uint64_t t_c = 0;
    uint64_t t_r = 0;
    uint64_t old_pid;
    char wid_r[24];
    char rev_r[48];

    o.interval = 1000000u;
    pair_open(&p, &o, "despawn");
    CHECK(create_entity_world(&p.ctl, p.wid, sizeof p.wid, p.crev, sizeof p.crev));
    CHECK(create_entity_world(&p.rec, wid_r, sizeof wid_r, rev_r, sizeof rev_r));
    CHECK(strcmp(p.wid, wid_r) == 0 && strcmp(p.crev, rev_r) == 0);
    snprintf(p.rrev, sizeof p.rrev, "%s", rev_r);

    /* Slot 3, generation 1; despawned (control plane). */
    CHECK(ent_spawn(&p.ctl, p.wid, p.crev, sizeof p.crev, "{\"position_x\":4,\"velocity_x\":0}", &hA, &sA, &gA));
    CHECK(ent_spawn(&p.rec, p.wid, p.rrev, sizeof p.rrev, "{\"position_x\":4,\"velocity_x\":0}", &hB, &sB, &gB));
    CHECK(hA == hB && sA == 3u && gA == 1u && sB == sA && gB == gA);
    CHECK(ent_despawn(&p.ctl, p.wid, p.crev, sizeof p.crev, hA));
    CHECK(ent_despawn(&p.rec, p.wid, p.rrev, sizeof p.rrev, hB));
    CHECK(strcmp(p.crev, p.rrev) == 0);
    /* Slot 3 reused with generation 2, slot 4 new, then the generation-2 handle is despawned. */
    CHECK(ent_spawn(&p.ctl, p.wid, p.crev, sizeof p.crev, "{\"position_x\":1}", &hA, &sA, &gA));
    CHECK(ent_spawn(&p.rec, p.wid, p.rrev, sizeof p.rrev, "{\"position_x\":1}", &hB, &sB, &gB));
    CHECK(hA == hB && sA == 3u && gA == 2u && sB == sA && gB == gA);
    CHECK(ent_spawn(&p.ctl, p.wid, p.crev, sizeof p.crev, "{}", &hC, &sC, &gC));
    CHECK(ent_spawn(&p.rec, p.wid, p.rrev, sizeof p.rrev, "{}", &hB, &sB, &gB));
    CHECK(hC == hB && sC == 4u && gC == 1u && sB == sC && gB == gC);
    CHECK(ent_despawn(&p.ctl, p.wid, p.crev, sizeof p.crev, hA));
    CHECK(ent_despawn(&p.rec, p.wid, p.rrev, sizeof p.rrev, hA));
    CHECK(strcmp(p.crev, p.rrev) == 0);

    /* Kill the recovered session's worker after the last despawn; the next request reports the recovery. */
    old_pid = pair_kill(&p);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"in\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\","
             "\"limit\":1}}",
             p.wid);
    call(&p.rec, line, &r);
    expect_worker_failed(&r);
    expect_recovery(&r, old_pid, 1);
    resp_free(&r);
    CHECK(worker_pid_of(&p.rec) != old_pid);

    /* Same canonical digest and tick as the control; same event log; the generation-1 handle of slot 3
     * and the live handle of slot 4 are judged the same by both sessions. */
    CHECK(digest(&p.ctl, p.wid, p.crev, dig_c, &t_c));
    CHECK(digest(&p.rec, p.wid, p.rrev, dig_r, &t_r));
    CHECK(strcmp(dig_c, dig_r) == 0 && t_c == t_r);
    ent_events_text(&p.ctl, p.wid, ev_c, sizeof ev_c);
    ent_events_text(&p.rec, p.wid, ev_r, sizeof ev_r);
    CHECK(strlen(ev_c) > 0 && strcmp(ev_c, ev_r) == 0);
    CHECK(ent_despawn(&p.ctl, p.wid, p.crev, sizeof p.crev, hA) == 0);
    CHECK(ent_despawn(&p.rec, p.wid, p.rrev, sizeof p.rrev, hA) == 0);
    CHECK(strcmp(p.crev, p.rrev) == 0);
    pair_step(&p, 2);
    pair_check(&p);
    pair_close(&p);
}

static void scenario_quarantine(void) {
    Opts o = default_opts();
    Resp r;
    char token[4200];
    char cand[32];
    char wid[24];
    char rev[48];
    char status_line[256];
    char src_sha[65];
    FILE *tf;
    uint64_t old_pid;
    int installed_src = 0;
    (void)installed_src;

    /* Source hash of the candidate, from the submission, gates the seam. */
    {
        Session s = session_start(&o, "spool-quar-probe");
        Resp sr;
        CHECK(submit(&s, "sys_a_v2.capsule.json", cand, sizeof cand));
        call(&s,
             "{\"protocol\":\"0.1\",\"id\":\"cs\",\"method\":\"candidate.status\",\"params\":{\"candidate_id\":\"candidate-1\"}}",
             &sr);
        CHECK(resp_ok(&sr));
        str_of(resp_result(&sr), "source_hash", src_sha, sizeof src_sha);
        resp_free(&sr);
        session_stop(&s);
    }
    snprintf(token, sizeof token, "%s/quar.never", g_scratch);
    setenv("SUTEKH_TEST_KILL_AT_TICK", "4", 1);
    setenv("SUTEKH_TEST_KILL_SOURCE_SHA", src_sha, 1);
    (void)tf;

    {
        Pair p;
        pair_open(&p, &o, "quar");
        pair_create(&p, 1);
        CHECK(submit(&p.rec, "sys_a_v2.capsule.json", cand, sizeof cand));
        CHECK(evaluate(&p.rec, cand, "sys_a_v2.test.json"));
        CHECK(install(&p.rec, cand, p.wid, p.rrev, 0, &r));
        CHECK(str_of(resp_result(&r), "world_revision", p.rrev, sizeof p.rrev));
        resp_free(&r);
        snprintf(wid, sizeof wid, "%s", p.wid);
        snprintf(rev, sizeof rev, "%s", p.rrev);

        /* First crash: the step that computes tick 4 kills the worker; the install is in force. */
        old_pid = worker_pid_of(&p.rec);
        CHECK(step(&p.rec, wid, rev, sizeof rev, 5, &r) == 0);
        expect_worker_failed(&r);
        expect_recovery(&r, old_pid, 1);
        resp_free(&r);

        /* Second crash from the same operation: quarantine. */
        old_pid = worker_pid_of(&p.rec);
        CHECK(step(&p.rec, wid, rev, sizeof rev, 5, &r) == 0);
        expect_worker_failed(&r);
        expect_recovery(&r, old_pid, 1);
        {
            const SuJsonValue *q = su_json_member(resp_member(&r, "recovery"), "quarantined");
            CHECK(q != NULL && q->type == SU_JSON_ARRAY && q->u.array.count == 1u);
        }
        resp_free(&r);
        unsetenv("SUTEKH_TEST_KILL_AT_TICK");
        unsetenv("SUTEKH_TEST_KILL_SOURCE_SHA");

        snprintf(status_line, sizeof status_line,
                 "{\"protocol\":\"0.1\",\"id\":\"cs\",\"method\":\"candidate.status\",\"params\":{\"candidate_id\":\"%s\"}}",
                 cand);
        call(&p.rec, status_line, &r);
        CHECK(resp_ok(&r));
        {
            char state[24];
            str_of(su_json_member(resp_result(&r), "install"), "state", state, sizeof state);
            CHECK(strcmp(state, "quarantined") == 0);
            CHECK(su_json_member(su_json_member(resp_result(&r), "install"), "reason") != NULL);
        }
        resp_free(&r);

        /* The world now runs the previous revision and keeps stepping. The rollback advanced the
         * revision, so the client reads it back first. */
        call(&p.rec, "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"world-1\",\"limit\":1}}", &r);
        CHECK(resp_ok(&r));
        str_of(resp_result(&r), "world_revision", rev, sizeof rev);
        CHECK(uint_of(resp_result(&r), "tick") == 0u);
        resp_free(&r);
        CHECK(step(&p.rec, wid, rev, sizeof rev, 5, &r));
        CHECK(uint_of(resp_result(&r), "tick") == 5u);
        resp_free(&r);
        pair_close(&p);
    }
}

/* A rebuilt world whose state differs from the recorded checkpoint digest is refused: recovery
 * reports it unrecoverable and the world is not continued. (The worker is made to skew the exact
 * restore; the environment variable is read only by the test-built worker.) */
static void scenario_digest_check(void) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    uint64_t old_pid;

    /* Set before the sessions start: a supervisor passes its environment on when it forks, and the
     * replacement workers it starts inherit that environment. */
    setenv("SUTEKH_TEST_SKEW_EXACT", "1", 1);
    pair_open(&p, &o, "skew");
    pair_create(&p, 1);
    pair_step(&p, 2);
    pair_check(&p);
    old_pid = worker_pid_of(&p.rec);
    if (old_pid > 0) {
        (void)su_test_kill_pid(old_pid);
    }
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 1, &r) == 0);
    unsetenv("SUTEKH_TEST_SKEW_EXACT");
    expect_worker_failed(&r);
    {
        const SuJsonValue *rec = resp_member(&r, "recovery");
        const SuJsonValue *ok = rec != NULL ? su_json_member(rec, "recovered") : NULL;
        const SuJsonValue *un = rec != NULL ? su_json_member(rec, "unrecoverable") : NULL;
        CHECK(ok != NULL && ok->type == SU_JSON_BOOL && !ok->u.boolean);
        CHECK(un != NULL && un->type == SU_JSON_ARRAY && un->u.array.count == 1u);
    }
    resp_free(&r);
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 1, &r) == 0);
    CHECK(resp_error_status(&r) == SU_E_INTERNAL);
    resp_free(&r);
    pair_close(&p);
}

/* Long run: 100,000 ticks. The supervisor's log and checkpoint bounds hold at every operation. */
static void scenario_bounds(void) {
    Opts o = default_opts();
    Session s;
    Resp r;
    char wid[24];
    char rev[48];
    int i;
    uint64_t max_log = 0;
    uint64_t max_ck = 0;
    uint64_t max_spool = 0;
    uint64_t ticks_done = 0;

    /* The interval is far away, so the log cap (16) drives the checkpoints: every 16 accepted
     * operations. The retained checkpoints and the log stay within their bounds throughout. */
    o.interval = 1000000000u;
    o.retain = 4;
    o.log_max = 16;
    s = session_start(&o, "spool-bounds");
    CHECK(create_world(&s, 1, wid, sizeof wid, rev, sizeof rev));
    for (i = 0; i < 1000; i++) {
        Resp er;
        const SuJsonValue *arr;
        const SuJsonValue *w0;
        CHECK(step(&s, wid, rev, sizeof rev, 100, &r));
        resp_free(&r);
        ticks_done += 100;
        call(&s, "{\"protocol\":\"0.1\",\"id\":\"ei\",\"method\":\"engine.info\",\"params\":{}}", &er);
        arr = su_json_member(resp_result(&er), "recovery");
        arr = su_json_member(arr, "worlds");
        w0 = arr != NULL && arr->type == SU_JSON_ARRAY && arr->u.array.count == 1 ? arr->u.array.items[0] : NULL;
        CHECK(w0 != NULL);
        if (w0 != NULL) {
            uint64_t lg = uint_of(w0, "log_entries");
            uint64_t ck = uint_of(w0, "checkpoints");
            if (lg > max_log) {
                max_log = lg;
            }
            if (ck > max_ck) {
                max_ck = ck;
            }
        }
        {
            uint64_t sp = uint_of(su_json_member(resp_result(&er), "recovery"), "spool_bytes");
            uint64_t lim = uint_of(su_json_member(su_json_member(resp_result(&er), "recovery"), "limits"),
                                   "spool_bytes_max");
            if (sp > max_spool) {
                max_spool = sp;
            }
            CHECK(sp <= lim);
        }
        resp_free(&er);
    }
    CHECK(ticks_done == 100000u);
    CHECK(max_log <= o.log_max);
    CHECK(max_ck <= o.retain);
    CHECK(max_log > 0);
    CHECK(max_spool > 0);
    /* Recovery at the end of the run still reproduces the state (kill, then step). */
    {
        uint64_t old_pid = worker_pid_of(&s);
        CHECK(old_pid > 0);
        if (old_pid > 0) {
            (void)su_test_kill_pid(old_pid);
        }
        CHECK(step(&s, wid, rev, sizeof rev, 1, &r) == 0);
        expect_worker_failed(&r);
        expect_recovery(&r, old_pid, 1);
        resp_free(&r);
    }
    session_stop(&s);
}

/* Corrupt checkpoint injected on disk: recovery must report the world as unrecoverable and must not
 * continue it. */
static void scenario_unrecoverable(void) {
    Opts o = default_opts();
    Pair p;
    Resp r;
    char snap[65];
    char path[4200];
    FILE *f;
    uint64_t old_pid;

    pair_open(&p, &o, "corrupt");
    pair_create(&p, 1);
    pair_step(&p, 2);
    {
        char line[512];
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"sn\",\"method\":\"world.snapshot\",\"params\":{\"world_id\":\"%s\","
                 "\"expected_world_revision\":\"%s\"}}",
                 p.wid, p.rrev);
        call(&p.rec, line, &r);
        CHECK(resp_ok(&r));
        str_of(resp_result(&r), "snapshot_id", snap, sizeof snap);
        resp_free(&r);
    }
    snprintf(path, sizeof path, "%s/spool-corrupt-rec/%s.snap", g_scratch, snap);
    f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f != NULL) {
        int c = fgetc(f);
        fseek(f, 0, SEEK_SET);
        fputc(c == '{' ? '[' : '{', f);
        fclose(f);
    }
    old_pid = pair_kill(&p);
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 1, &r) == 0);
    expect_worker_failed(&r);
    {
        const SuJsonValue *rec = resp_member(&r, "recovery");
        const SuJsonValue *ok = rec != NULL ? su_json_member(rec, "recovered") : NULL;
        const SuJsonValue *un = rec != NULL ? su_json_member(rec, "unrecoverable") : NULL;
        CHECK(ok != NULL && ok->type == SU_JSON_BOOL && !ok->u.boolean);
        CHECK(un != NULL && un->type == SU_JSON_ARRAY && un->u.array.count == 1u);
        CHECK(uint_of(rec, "replaced_pid_old") == old_pid);
    }
    resp_free(&r);
    /* The world is not continued: later steps are refused as unrecoverable. */
    CHECK(step(&p.rec, p.wid, p.rrev, sizeof p.rrev, 1, &r) == 0);
    CHECK(resp_error_status(&r) == SU_E_INTERNAL);
    resp_free(&r);
    pair_close(&p);
}

/* ---------- main ---------- */

/* Returns 1 when needle occurs in the file's bytes (the binary's string table included), 0 when not,
 * -1 when the file cannot be read. */
static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    long size;
    size_t n = strlen(needle);
    unsigned char *buf;
    size_t i;
    size_t j;
    int found = 0;
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return -1;
    }
    for (i = 0; n > 0 && i + n <= (size_t)size && !found; i++) {
        for (j = 0; j < n && buf[i + j] == (unsigned char)needle[j]; j++) {
        }
        found = j == n;
    }
    free(buf);
    fclose(f);
    return found;
}

int main(int argc, char **argv) {
#if !defined(_WIN32)
    struct sigaction ign;
#endif
    const char *files[] = {"world4.json", "sys_a.capsule.json", "sys_a.lir.json", "sys_b.capsule.json",
                           "sys_b.lir.json", "sys_a_v2.capsule.json", "sys_a_v2.lir.json",
                           "sys_a_v2.test.json", "sys_a.test.json"};
    size_t i;

    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s <recovery_worker> <repo_root> <scratch_dir> [release_sutekh]\n", argv[0]);
        return 2;
    }
    g_worker = argv[1];
    g_root = argv[2];
    snprintf(g_scratch, sizeof g_scratch, "%s", argv[3]);
    snprintf(g_proj, sizeof g_proj, "%s/proj", g_scratch);
    mkdir(g_proj, 0700);
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        copy_file(files[i]);
    }

#if !defined(_WIN32)
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, NULL);
#endif

    copy_from("tests/fixtures/entities", "actors_3.json");
    copy_from("tests/fixtures/entities", "despawn_move.capsule.json");
    copy_from("tests/fixtures/entities", "despawn_move.lir.json");
    copy_from("capsules/rts/host", "spatial.capsule.json");
    write_rts_fixture();
    write_rts_full_project();

    scenario_despawn_replay();
    scenario_control_batches(0u, "ctl-log");
    scenario_control_batches(1u, "ctl-ck");
    scenario_host_fault("rts.spatial", "hf-spatial");
    scenario_host_fault("rts.scatter", "hf-scatter");
    scenario_after_step();
    scenario_after_install();
    scenario_replay_install(0);
    scenario_replay_install(1);
    scenario_after_restore();
    scenario_mid_step();
    scenario_quarantine();
    scenario_unrecoverable();
    scenario_digest_check();
    scenario_bounds();

    if (argc == 5) {
        /* The production binary (built without SU_TEST_FAULT_HOOKS) carries no crash seam: none of
         * its strings may be present. The seam's environment names are the probe. */
        const char *probes[] = {"SUTEKH_TEST_KILL_AT_TICK", "SUTEKH_TEST_KILL_SOURCE_SHA",
                                "SUTEKH_TEST_KILL_TOKEN", "SUTEKH_TEST_KILL_MODE",
                                "SUTEKH_TEST_HOST_FAULT_SYSTEM", "SUTEKH_TEST_HOST_FAULT_TICK",
                                "SUTEKH_TEST_HOST_FAULT_TOKEN"};
        size_t k;
        for (k = 0; k < sizeof probes / sizeof probes[0]; k++) {
            int hit = file_contains(argv[4], probes[k]);
            CHECK(hit == 0);
            if (hit != 0) {
                fprintf(stderr, "release binary contains %s\n", probes[k]);
            }
        }
        CHECK(file_contains(argv[4], "worker") == 1);
    }
    printf("test_recovery: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
