/* M1-02c-1 qualification: world.specialize admission runs in a disposable worker (C07).
 * Built only with SUTEKH_NATIVE. Usage: test_specialize_admission <test worker> <repo root> <scratch dir>
 * The worker must be the test build (native_test_worker, SU_TEST_FAULT_HOOKS). Each scenario runs a
 * fresh agent process; the fault environment is set in that agent before it serves, so it reaches the
 * disposable admission children and no other process.
 *
 * Covers: success for scalar, avx2 (when the host has it) and auto; the rejections of the four fault
 * seams (wrong kernel in the corpus, wrong kernel in own test cases only, crash, hang) and of a
 * perturbed recompile in the active worker; each rejection leaves the active worker pid and its start
 * time, the world revision and the supervisor answer unchanged, and leaves no zombie child; the stale
 * revision and unknown backend refusals; a delayed admission (ordering) still installs. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

#include "agent/su_agent.h"
#include "agent/su_proc.h"
#include "agent/su_proto.h"
#include "backends/native/su_native.h"
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
static char g_proj[1100];
static int g_seq;

typedef struct Sess {
    pid_t pid;
    int wfd;
    SuLineReader rd;
} Sess;

typedef struct Resp {
    uint8_t *text;
    size_t len;
    SuJsonDoc doc;
    const SuJsonValue *root;
} Resp;

static void resp_free(Resp *r) {
    su_json_doc_free(&r->doc);
    free(r->text);
    memset(r, 0, sizeof *r);
}

static int resp_ok(const Resp *r) {
    const SuJsonValue *v = r->root != NULL ? su_json_member(r->root, "ok") : NULL;
    return v != NULL && v->type == SU_JSON_BOOL && v->u.boolean;
}

static const SuJsonValue *member(const SuJsonValue *o, const char *key) {
    return o != NULL && o->type == SU_JSON_OBJECT ? su_json_member(o, key) : NULL;
}

static uint64_t uint_of(const SuJsonValue *obj, const char *key) {
    uint64_t v = UINT64_MAX;
    const SuJsonValue *m = member(obj, key);
    if (m == NULL || !su_json_get_uint(m, &v)) {
        return UINT64_MAX;
    }
    return v;
}

static int str_of(const SuJsonValue *obj, const char *key, char *dst, size_t cap) {
    const SuJsonValue *m = member(obj, key);
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

/* Error status number of a failed response, or UINT64_MAX. */
static uint64_t err_status(const Resp *r) {
    const SuJsonValue *e;
    if (r->root == NULL || resp_ok(r)) {
        return UINT64_MAX;
    }
    e = member(r->root, "error");
    return uint_of(e, "status");
}

static int err_code_is(const Resp *r, const char *name) {
    char buf[64];
    const SuJsonValue *e = member(r->root, "error");
    return str_of(e, "code", buf, sizeof buf) && strcmp(buf, name) == 0;
}

/* ---------- agent sessions: one agent process per scenario ---------- */

static Sess sess_start(const char *fault, const char *fault_ms, uint64_t timeout_ms, const char *tag) {
    Sess s;
    int req[2];
    int rsp[2];
    pid_t pid;
    char spool[1200];
    SuAgentConfig cfg;
    static const char *argv[2];

    snprintf(spool, sizeof spool, "%s/spool-%s-%d", g_scratch, tag, g_seq++);
    mkdir(spool, 0700);
    argv[0] = g_worker;
    argv[1] = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.project_root = g_proj;
    cfg.worker_argv = argv;
    cfg.request_timeout_ms = timeout_ms;
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
        if (fault != NULL) {
            setenv("SU_TEST_FAULT", fault, 1);
        } else {
            unsetenv("SU_TEST_FAULT");
        }
        if (fault_ms != NULL) {
            setenv("SU_TEST_FAULT_MS", fault_ms, 1);
        } else {
            unsetenv("SU_TEST_FAULT_MS");
        }
        rc = su_agent_serve(req[0], rsp[1], &cfg);
        _exit(rc == 0 ? 0 : 1);
    }
    close(req[0]);
    close(rsp[1]);
    memset(&s, 0, sizeof s);
    s.pid = pid;
    s.wfd = req[1];
    su_line_reader_init(&s.rd, rsp[0], -1);
    return s;
}

static void sess_stop(Sess *s) {
    int status = 0;
    close(s->wfd);
    su_line_reader_free(&s->rd);
    waitpid(s->pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int call(Sess *s, const char *line, Resp *r, int64_t wait_ms) {
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
    s->rd.deadline_ms = su_now_ms() + wait_ms;
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

/* ---------- project copy and process facts ---------- */

static void copy_example(const char *name) {
    char src[1200];
    char dst[1200];
    FILE *fi;
    FILE *fo;
    char buf[4096];
    size_t n;

    snprintf(src, sizeof src, "%s/examples/%s", g_root, name);
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

/* Start time (clock ticks since boot) of a process, or -1. */
static long long proc_starttime(pid_t pid) {
    char path[64];
    char buf[1024];
    FILE *f;
    char *p;
    long long st = -1;
    int field = 0;

    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    if (fgets(buf, sizeof buf, f) != NULL) {
        p = strrchr(buf, ')');
        if (p != NULL) {
            char *tok = strtok(p + 2, " ");
            /* after ") ": field 3 is the first token; field 22 is the 20th token. */
            while (tok != NULL) {
                field++;
                if (field == 20) {
                    st = strtoll(tok, NULL, 10);
                    break;
                }
                tok = strtok(NULL, " ");
            }
        }
    }
    fclose(f);
    return st;
}

/* Zombie children of ppid (a reaped child leaves nothing behind). */
static int zombie_children(pid_t ppid) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;

    if (d == NULL) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char path[300];
        char buf[1024];
        FILE *f;
        char *p;
        char state = '?';
        int pp = -1;
        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        if (fgets(buf, sizeof buf, f) != NULL) {
            p = strrchr(buf, ')');
            if (p != NULL) {
                (void)sscanf(p + 2, "%c %d", &state, &pp);
            }
        }
        fclose(f);
        if (pp == (int)ppid && state == 'Z') {
            n++;
        }
    }
    closedir(d);
    return n;
}

/* ---------- world helpers ---------- */

static int create_movement(Sess *s, char *wid, size_t wcap, char *rev, size_t rcap) {
    Resp r;
    int ok;
    call(s,
         "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
         "\"world.json\",\"capsule_path\":\"movement.capsule.json\"}}",
         &r, 120000);
    ok = resp_ok(&r) && str_of(member(r.root, "result"), "world_id", wid, wcap) &&
         str_of(member(r.root, "result"), "world_revision", rev, rcap);
    resp_free(&r);
    return ok;
}

static uint64_t active_pid(Sess *s) {
    Resp r;
    uint64_t pid = 0;
    call(s, "{\"protocol\":\"0.1\",\"id\":\"ei\",\"method\":\"engine.info\",\"params\":{}}", &r, 120000);
    if (resp_ok(&r)) {
        pid = uint_of(member(member(r.root, "result"), "active_worker"), "pid");
    }
    resp_free(&r);
    return pid;
}

static int host_avx2(void) {
    return (su_native_host_caps() & SU_NCAP_X86_64_AVX2) != 0u;
}

/* world.inspect result world_revision (copied), or 0 when the call failed. */
static int world_rev(Sess *s, const char *wid, char *rev, size_t cap) {
    Resp r;
    char line[512];
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\"}}",
             wid);
    call(s, line, &r, 120000);
    ok = resp_ok(&r) && str_of(member(r.root, "result"), "world_revision", rev, cap);
    resp_free(&r);
    return ok;
}

static void specialize(Sess *s, const char *wid, const char *rev, const char *backend, Resp *r) {
    char line[768];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"sp\",\"method\":\"world.specialize\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"system_id\":\"demo.movement\",\"backend\":\"%s\"}}",
             wid, rev, backend);
    call(s, line, r, 120000);
}

/* The rejection contract: the active worker pid and start time, the world revision and the supervisor
 * answer are unchanged, and the disposable child left no zombie. */
static void check_unchanged(Sess *s, uint64_t active, long long start, const char *wid, const char *rev,
                            const char *label) {
    char now[65];
    long long st_now;
    CHECK(active != 0u);
    st_now = active != 0u ? proc_starttime((pid_t)active) : -1;
    CHECK(st_now != -1 && st_now == start);
    CHECK(active_pid(s) == active);
    CHECK(world_rev(s, wid, now, sizeof now) && strcmp(now, rev) == 0);
    CHECK(zombie_children(s->pid) == 0);
    if (label != NULL && (st_now == -1 || st_now != start)) {
        fprintf(stderr, "%s: active worker changed\n", label);
    }
}

/* ---------- scenarios ---------- */

static void scenario_success(const char *backend, int expect_avx2_tier) {
    Sess s = sess_start(NULL, NULL, 60000, "ok");
    char wid[64];
    char rev[65];
    char got[65];
    char tier[32];
    Resp r;
    uint64_t active;
    long long start;
    uint64_t cases;
    const SuJsonValue *res;

    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    active = active_pid(&s);
    start = proc_starttime((pid_t)active);
    specialize(&s, wid, rev, backend, &r);
    CHECK(resp_ok(&r));
    res = member(r.root, "result");
    CHECK(str_of(res, "state", got, sizeof got) && strcmp(got, "installed") == 0);
    CHECK(str_of(res, "artifact_hash", got, sizeof got) && strlen(got) == 64u);
    CHECK(str_of(res, "execution_tier", tier, sizeof tier));
    if (expect_avx2_tier) {
        CHECK(strcmp(tier, "native_avx2") == 0);
    } else {
        CHECK(strcmp(tier, "native_scalar") == 0);
    }
    CHECK(str_of(member(res, "source"), "kind", got, sizeof got) && strcmp(got, "disposable_worker") == 0);
    cases = uint_of(member(member(res, "source"), "admission"), "cases");
    /* 42 corpus cases plus the 3 own test cases of examples/movement.test.json. */
    CHECK(cases == 45u);
    CHECK(uint_of(member(member(res, "source"), "admission"), "mismatches") == 0u);
    CHECK(str_of(res, "world_revision", got, sizeof got) && strcmp(got, rev) == 0);
    resp_free(&r);
    check_unchanged(&s, active, start, wid, rev, backend);
    sess_stop(&s);
}

static void scenario_fault(const char *label, const char *fault, const char *ms, uint64_t timeout_ms,
                           uint64_t want_status, const char *want_code) {
    Sess s = sess_start(fault, ms, timeout_ms, label);
    char wid[64];
    char rev[65];
    char got[65];
    Resp r;
    uint64_t active;
    long long start;
    const SuJsonValue *det;

    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    active = active_pid(&s);
    start = proc_starttime((pid_t)active);
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(!resp_ok(&r));
    CHECK(err_status(&r) == want_status);
    CHECK(err_code_is(&r, want_code));
    det = member(member(r.root, "error"), "detail");
    CHECK(det != NULL);
    CHECK(str_of(det, "phase", got, sizeof got) && strcmp(got, "admission") == 0);
    if (want_status == SU_E_WORKER_FAILED) {
        CHECK(str_of(det, "end", got, sizeof got));
        if (strcmp(label, "crash") == 0) {
            CHECK(strcmp(got, "signaled") == 0);
            CHECK(uint_of(det, "signal") == 11u);
        } else {
            CHECK(strcmp(got, "supervisor_killed") == 0);
        }
    } else {
        CHECK(str_of(member(det, "first_mismatch"), "source", got, sizeof got));
        if (strcmp(label, "wrong_world") == 0) {
            CHECK(strcmp(got, "own_case") == 0);
        } else {
            CHECK(strcmp(got, "corpus") == 0);
        }
    }
    resp_free(&r);
    check_unchanged(&s, active, start, wid, rev, label);
    /* The supervisor still answers: a later read succeeds. */
    CHECK(active_pid(&s) == active);
    sess_stop(&s);
}

static void scenario_perturbed_recompile(void) {
    Sess s = sess_start("perturb_recompile", NULL, 60000, "perturb");
    char wid[64];
    char rev[65];
    Resp r;
    uint64_t active;
    long long start;

    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    active = active_pid(&s);
    start = proc_starttime((pid_t)active);
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(!resp_ok(&r));
    CHECK(err_status(&r) == SU_E_CANDIDATE_FAILED);
    {
        char msg[512];
        CHECK(str_of(member(r.root, "error"), "message", msg, sizeof msg) && strstr(msg, "artifact_hash") != NULL);
    }
    resp_free(&r);
    check_unchanged(&s, active, start, wid, rev, "perturb");
    sess_stop(&s);
}

/* Refusals decided before any admission runs. */
static void scenario_refusals(void) {
    Sess s = sess_start(NULL, NULL, 60000, "refuse");
    char wid[64];
    char rev[65];
    Resp r;

    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    specialize(&s, wid, "stale-revision", "scalar", &r);
    CHECK(err_status(&r) == SU_E_STALE_REVISION);
    resp_free(&r);
    specialize(&s, wid, rev, "sse9", &r);
    CHECK(err_status(&r) == SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    sess_stop(&s);
}

/* A delayed admission (the disposable sleeps before each kernel call) still installs in order. */
static void scenario_delay(void) {
    Sess s = sess_start("delay", "150", 60000, "delay");
    char wid[64];
    char rev[65];
    Resp r;
    uint64_t active;
    long long start;

    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    active = active_pid(&s);
    start = proc_starttime((pid_t)active);
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(resp_ok(&r));
    resp_free(&r);
    check_unchanged(&s, active, start, wid, rev, "delay");
    sess_stop(&s);
}

/* ---------- worker-level scenario: the install checks of the active worker, driven directly ----------
 * The supervisor always passes the identity it just read, so a stale semantic hash, a layout generation
 * mismatch and a revision that changed between admission and install are reachable only here. The
 * disposable admission runs in a second worker process, as the supervisor does. */

static int wparse(Resp *r, const uint8_t *out, size_t len) {
    SuJsonLimits lim;
    SuJsonError jerr;
    uint32_t st;

    memset(r, 0, sizeof *r);
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

static int wcall(SuProc *p, const char *line, Resp *r) {
    const uint8_t *out = NULL;
    size_t len = 0;
    uint32_t st = su_proc_request(p, (const uint8_t *)line, strlen(line), su_now_ms() + 120000, &out, &len);
    if (st != SU_OK) {
        memset(r, 0, sizeof *r);
        return -1;
    }
    return wparse(r, out, len);
}

static int wspawn(SuProc *p) {
    static const char *argv[2];
    argv[0] = g_worker;
    argv[1] = NULL;
    return su_proc_spawn(p, argv, NULL) == SU_OK ? 0 : -1;
}

static void wstop(SuProc *p) {
    su_proc_finish(p, 1000);
}

static void scenario_worker_install(void) {
    SuProc act;
    SuProc dis;
    char cap[1200];
    char src[1200];
    char fixture[1200];
    char test[1200];
    char line[8192];
    char wid[64];
    char rev[65];
    char rev2[65];
    char sem[65];
    char srch[65];
    char caph[65];
    char admitted[65];
    char got[65];
    char zeros[65];
    Resp r;
    uint64_t gen = 0;

    memset(zeros, '0', 64);
    zeros[64] = '\0';
    snprintf(cap, sizeof cap, "%s/movement.capsule.json", g_proj);
    snprintf(src, sizeof src, "%s/movement.lir.json", g_proj);
    snprintf(fixture, sizeof fixture, "%s/world.json", g_proj);
    snprintf(test, sizeof test, "%s/movement.test.json", g_proj);
    CHECK(wspawn(&act) == 0);
    CHECK(wspawn(&dis) == 0);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"%s\","
             "\"capsules\":[{\"capsule_path\":\"%s\",\"source_path\":\"%s\"}]}}",
             fixture, cap, src);
    CHECK(wcall(&act, line, &r) == 0 && resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "world_id", wid, sizeof wid));
    CHECK(str_of(member(r.root, "result"), "world_revision", rev, sizeof rev));
    resp_free(&r);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"id\",\"method\":\"internal.specialize.identity\",\"params\":{"
             "\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"system_id\":\"demo.movement\"}}",
             wid, rev);
    CHECK(wcall(&act, line, &r) == 0 && resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "semantic_hash", sem, sizeof sem));
    CHECK(str_of(member(r.root, "result"), "source_hash", srch, sizeof srch));
    CHECK(str_of(member(r.root, "result"), "capsule_hash", caph, sizeof caph));
    gen = uint_of(member(r.root, "result"), "layout_generation");
    CHECK(gen != UINT64_MAX);
    resp_free(&r);

    /* Admission in the disposable process. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ad\",\"method\":\"internal.specialize.admit\",\"params\":{"
             "\"capsule_path\":\"%s\",\"source_path\":\"%s\",\"test_path\":\"%s\","
             "\"expected_source_hash\":\"%s\",\"expected_capsule_hash\":\"%s\",\"expected_semantic_hash\":\"%s\","
             "\"expected_layout_generation\":%llu,\"backend\":\"scalar\"}}",
             cap, src, test, srch, caph, sem, (unsigned long long)gen);
    CHECK(wcall(&dis, line, &r) == 0 && resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "status", got, sizeof got) && strcmp(got, "admitted") == 0);
    CHECK(str_of(member(r.root, "result"), "artifact_hash", admitted, sizeof admitted) && strlen(admitted) == 64u);
    resp_free(&r);

    /* A. stale semantic hash at install. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"in\",\"method\":\"internal.specialize.install\",\"params\":{"
             "\"world_id\":\"%s\",\"system_id\":\"demo.movement\",\"expected_world_revision\":\"%s\","
             "\"expected_semantic_hash\":\"%s\",\"expected_layout_generation\":%llu,\"backend\":\"scalar\","
             "\"artifact_hash\":\"%s\"}}",
             wid, rev, zeros, (unsigned long long)gen, admitted);
    CHECK(wcall(&act, line, &r) == 0 && err_status(&r) == SU_E_STALE_REVISION);
    resp_free(&r);

    /* B. layout generation differs. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"in\",\"method\":\"internal.specialize.install\",\"params\":{"
             "\"world_id\":\"%s\",\"system_id\":\"demo.movement\",\"expected_world_revision\":\"%s\","
             "\"expected_semantic_hash\":\"%s\",\"expected_layout_generation\":%llu,\"backend\":\"scalar\","
             "\"artifact_hash\":\"%s\"}}",
             wid, rev, sem, (unsigned long long)(gen + 1u), admitted);
    CHECK(wcall(&act, line, &r) == 0 && err_status(&r) == SU_E_SCHEMA_MISMATCH);
    resp_free(&r);

    /* C. an artifact_hash other than the admitted one is refused and nothing is installed. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"in\",\"method\":\"internal.specialize.install\",\"params\":{"
             "\"world_id\":\"%s\",\"system_id\":\"demo.movement\",\"expected_world_revision\":\"%s\","
             "\"expected_semantic_hash\":\"%s\",\"expected_layout_generation\":%llu,\"backend\":\"scalar\","
             "\"artifact_hash\":\"%s\"}}",
             wid, rev, sem, (unsigned long long)gen, zeros);
    CHECK(wcall(&act, line, &r) == 0 && err_status(&r) == SU_E_CANDIDATE_FAILED);
    resp_free(&r);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\"}}", wid);
    CHECK(wcall(&act, line, &r) == 0 && resp_ok(&r));
    {
        const SuJsonValue *man = member(member(r.root, "result"), "semantic_manifest");
        const SuJsonValue *m0 = man != NULL && man->type == SU_JSON_ARRAY && man->u.array.count > 0
                                    ? man->u.array.items[0]
                                    : NULL;
        CHECK(str_of(m0, "execution_tier", got, sizeof got) && strcmp(got, "vm") == 0);
    }
    resp_free(&r);

    /* D. the world revision changes between admission and install: the install is stale. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"st\",\"method\":\"world.step\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"ticks\":1}}",
             wid, rev);
    CHECK(wcall(&act, line, &r) == 0 && resp_ok(&r));
    resp_free(&r);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\"}}", wid);
    CHECK(wcall(&act, line, &r) == 0 && resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "world_revision", rev2, sizeof rev2) && strcmp(rev2, rev) != 0);
    resp_free(&r);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"in\",\"method\":\"internal.specialize.install\",\"params\":{"
             "\"world_id\":\"%s\",\"system_id\":\"demo.movement\",\"expected_world_revision\":\"%s\","
             "\"expected_semantic_hash\":\"%s\",\"expected_layout_generation\":%llu,\"backend\":\"scalar\","
             "\"artifact_hash\":\"%s\"}}",
             wid, rev, sem, (unsigned long long)gen, admitted);
    CHECK(wcall(&act, line, &r) == 0 && err_status(&r) == SU_E_STALE_REVISION);
    resp_free(&r);

    /* E. the current revision with the admitted hash installs; the tier is native. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"in\",\"method\":\"internal.specialize.install\",\"params\":{"
             "\"world_id\":\"%s\",\"system_id\":\"demo.movement\",\"expected_world_revision\":\"%s\","
             "\"expected_semantic_hash\":\"%s\",\"expected_layout_generation\":%llu,\"backend\":\"scalar\","
             "\"artifact_hash\":\"%s\"}}",
             wid, rev2, sem, (unsigned long long)gen, admitted);
    CHECK(wcall(&act, line, &r) == 0 && resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "execution_tier", got, sizeof got) &&
          strcmp(got, "native_scalar") == 0);
    CHECK(str_of(member(r.root, "result"), "artifact_hash", got, sizeof got) && strcmp(got, admitted) == 0);
    resp_free(&r);

    wstop(&dis);
    wstop(&act);
}

int main(int argc, char **argv) {
    int avx2;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <test worker> <repo root> <scratch dir>\n", argv[0]);
        return 2;
    }
    g_worker = argv[1];
    g_root = argv[2];
    snprintf(g_scratch, sizeof g_scratch, "%s", argv[3]);
    mkdir(g_scratch, 0700);
    snprintf(g_proj, sizeof g_proj, "%s/project", g_scratch);
    mkdir(g_proj, 0700);
    copy_example("movement.capsule.json");
    copy_example("movement.lir.json");
    copy_example("movement.test.json");
    copy_example("world.json");

    {
        avx2 = host_avx2();
    }

    scenario_success("scalar", 0);
    scenario_success("auto", avx2 == 1);
    if (avx2) {
        scenario_success("avx2", 1);
    }
    scenario_fault("wrong_kernel", "wrong_kernel", NULL, 60000, SU_E_CANDIDATE_FAILED, "SU_E_CANDIDATE_FAILED");
    scenario_fault("wrong_world", "wrong_world", NULL, 60000, SU_E_CANDIDATE_FAILED, "SU_E_CANDIDATE_FAILED");
    scenario_fault("crash", "crash", NULL, 60000, SU_E_WORKER_FAILED, "SU_E_WORKER_FAILED");
    scenario_fault("hang", "hang", NULL, 3000, SU_E_WORKER_FAILED, "SU_E_WORKER_FAILED");
    scenario_perturbed_recompile();
    scenario_refusals();
    scenario_delay();
    scenario_worker_install();

    printf("test_specialize_admission: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
