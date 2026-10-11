/* M1-02c-2 qualification: a native kernel that kills the active worker is quarantined; recovery returns
 * the world to the VM tier with canonical state equal to an uninterrupted VM-only run; kernels that
 * were not attributed to the crash are respecialized between requests, never during recovery and never
 * before a tick.
 *
 * Usage: test_native_fault <native_test_worker> <repo root> <scratch dir>
 *   native_test_worker  the native test build (SU_TEST_FAULT_HOOKS, native_versions.c linked)
 *   repo root           absolute path of the repository (fixtures are copied from examples/)
 *   scratch dir         writable directory for a project copy and spool directories
 *
 * Seam: SU_TEST_FAULT_NATIVE_TICK=5 (and SU_TEST_FAULT_NATIVE_BACKEND=avx2|scalar, or unset for any
 * backend) makes the worker die with SIGSEGV inside an installed kernel while it computes tick 5.
 * Replacement workers inherit the environment; after recovery no kernel is installed, so the seam can
 * only fire again when a kernel is installed by a later world.specialize.
 *
 * Every scenario compares the canonical digest (sha256 of the snapshot bytes, which include the world
 * revision) at each tick with a control session that steps one tick at a time on the VM only. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
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

#include "agent/su_agent.h"
#include "agent/su_proc.h"
#include "agent/su_proto.h"
#include "backends/native/su_native.h"
#include "ir/su_json.h"
#include "sutekh/status.h"

static long g_checks = 0;
static long g_fail = 0;
static long g_skipped = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CRASH_TICK 5u
#define CTL_TICKS 60u

static const char *g_worker;
static const char *g_root;
static char g_scratch[1024];
static char g_proj[1100];
static int g_seq;
static char g_ctl[CTL_TICKS + 1u][65];

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

static const SuJsonValue *member(const SuJsonValue *o, const char *key) {
    return o != NULL && o->type == SU_JSON_OBJECT ? su_json_member(o, key) : NULL;
}

static int resp_ok(const Resp *r) {
    const SuJsonValue *v = member(r->root, "ok");
    return v != NULL && v->type == SU_JSON_BOOL && v->u.boolean;
}

static const SuJsonValue *resp_result(const Resp *r) {
    return member(r->root, "result");
}

static int bool_of(const SuJsonValue *obj, const char *key, int *out) {
    const SuJsonValue *m = member(obj, key);
    if (m == NULL || m->type != SU_JSON_BOOL) {
        return 0;
    }
    *out = m->u.boolean ? 1 : 0;
    return 1;
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

static uint64_t err_status(const Resp *r) {
    if (r->root == NULL || resp_ok(r)) {
        return UINT64_MAX;
    }
    return uint_of(member(r->root, "error"), "status");
}

static int err_code_is(const Resp *r, const char *name) {
    char buf[64];
    return str_of(member(r->root, "error"), "code", buf, sizeof buf) && strcmp(buf, name) == 0;
}

/* ---------- agent sessions: one agent process per session; the env is set before it serves ---------- */

static Sess sess_start(const char *native_tick, const char *native_backend, const char *tag) {
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
    cfg.request_timeout_ms = 60000;
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
        unsetenv("SU_TEST_FAULT");
        unsetenv("SU_TEST_FAULT_MS");
        if (native_tick != NULL) {
            setenv("SU_TEST_FAULT_NATIVE_TICK", native_tick, 1);
        } else {
            unsetenv("SU_TEST_FAULT_NATIVE_TICK");
        }
        if (native_backend != NULL) {
            setenv("SU_TEST_FAULT_NATIVE_BACKEND", native_backend, 1);
        } else {
            unsetenv("SU_TEST_FAULT_NATIVE_BACKEND");
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

static int call(Sess *s, const char *line, Resp *r) {
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

/* State letter and parent pid from /proc/<pid>/stat. 0 when the process is gone. */
static int proc_stat(pid_t pid, char *state, int *ppid) {
    char path[64];
    char buf[1024];
    FILE *f;
    char *p;
    int ok = 0;
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    if (fgets(buf, sizeof buf, f) != NULL) {
        p = strrchr(buf, ')');
        if (p != NULL && sscanf(p + 2, "%c %d", state, ppid) == 2) {
            ok = 1;
        }
    }
    fclose(f);
    return ok;
}

/* A live child of the agent: exists, is not a zombie, and its parent is the agent. */
static int live_child_of(pid_t pid, pid_t agent) {
    char state = '?';
    int ppid = -1;
    if (pid <= 0 || !proc_stat(pid, &state, &ppid)) {
        return 0;
    }
    return state != 'Z' && ppid == (int)agent;
}

/* Zombie children of ppid: a reaped child leaves nothing behind. */
static int zombie_children(pid_t ppid) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;
    if (d == NULL) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char state = '?';
        int pp = -1;
        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        if (proc_stat((pid_t)atol(e->d_name), &state, &pp) && pp == (int)ppid && state == 'Z') {
            n++;
        }
    }
    closedir(d);
    return n;
}

/* ---------- world helpers ---------- */

static int host_avx2(void) {
    return (su_native_host_caps() & SU_NCAP_X86_64_AVX2) != 0u;
}

static int create_movement(Sess *s, char *wid, size_t wcap, char *rev, size_t rcap) {
    Resp r;
    int ok;
    call(s,
         "{\"protocol\":\"0.1\",\"id\":\"cw\",\"method\":\"world.create\",\"params\":{\"fixture_path\":"
         "\"world.json\",\"capsule_path\":\"movement.capsule.json\"}}",
         &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_id", wid, wcap) &&
         str_of(resp_result(&r), "world_revision", rev, rcap);
    resp_free(&r);
    return ok;
}

static uint64_t active_pid(Sess *s) {
    Resp r;
    uint64_t pid = 0;
    call(s, "{\"protocol\":\"0.1\",\"id\":\"ei\",\"method\":\"engine.info\",\"params\":{}}", &r);
    if (resp_ok(&r)) {
        pid = uint_of(member(resp_result(&r), "active_worker"), "pid");
    }
    resp_free(&r);
    return pid;
}

/* world.inspect: the world revision, and the state of the answer in r (caller frees r). */
static void inspect(Sess *s, const char *wid, Resp *r) {
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\"}}",
             wid);
    call(s, line, r);
}

static int rev_of_inspect(Sess *s, const char *wid, char *rev, size_t cap) {
    Resp r;
    int ok;
    inspect(s, wid, &r);
    ok = resp_ok(&r) && str_of(resp_result(&r), "world_revision", rev, cap);
    resp_free(&r);
    return ok;
}

/* The execution tier of the system (semantic_manifest[0]). */
static int tier_of(const Resp *r, char *out, size_t cap) {
    const SuJsonValue *man = member(resp_result(r), "semantic_manifest");
    if (man == NULL || man->type != SU_JSON_ARRAY || man->u.array.count == 0) {
        return 0;
    }
    return str_of(man->u.array.items[0], "execution_tier", out, cap);
}

/* The "specialization" member of a world.inspect answer. */
static const SuJsonValue *spec_of(const Resp *r) {
    return member(r->root, "specialization");
}

static void specialize(Sess *s, const char *wid, const char *rev, const char *backend, Resp *r) {
    char line[768];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"sp\",\"method\":\"world.specialize\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"system_id\":\"demo.movement\",\"backend\":\"%s\"}}",
             wid, rev, backend);
    call(s, line, r);
}

/* One world.step of one tick. rev is updated on success. r holds the answer (caller frees). */
static int step1(Sess *s, const char *wid, char *rev, size_t rcap, Resp *r) {
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"st\",\"method\":\"world.step\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"ticks\":1}}",
             wid, rev);
    call(s, line, r);
    if (resp_ok(r)) {
        str_of(resp_result(r), "world_revision", rev, rcap);
        return 1;
    }
    return 0;
}

/* Canonical digest at rev (sha256 of the snapshot bytes) and the tick it was taken at. */
static int digest(Sess *s, const char *wid, const char *rev, char sha[65], uint64_t *tick) {
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

/* A worker-death answer: SU_E_WORKER_FAILED with a recovery object that reports the replaced pid. When
 * want_hash is set, the recovery object names that artifact as quarantined. new_pid receives the
 * replacement pid. */
static void expect_recovery(const Resp *r, uint64_t old_pid, const char *want_hash, uint64_t *new_pid) {
    const SuJsonValue *rec = member(r->root, "recovery");
    const SuJsonValue *nat = member(rec, "native");
    char got[65];
    int flag = 0;
    CHECK(!resp_ok(r));
    CHECK(err_status(r) == SU_E_WORKER_FAILED);
    CHECK(rec != NULL);
    if (!(bool_of(rec, "recovered", &flag) && flag == 1)) {
        fprintf(stderr, "diagnostic: recovery answer: %s\n", r->text != NULL ? (const char *)r->text : "(none)");
    }
    CHECK(bool_of(rec, "recovered", &flag) && flag == 1);
    CHECK(uint_of(rec, "replaced_pid_old") == old_pid);
    *new_pid = uint_of(rec, "replaced_pid_new");
    CHECK(*new_pid != 0u && *new_pid != old_pid);
    if (want_hash != NULL) {
        CHECK(nat != NULL);
        CHECK(str_of(nat, "artifact_hash", got, sizeof got) && strcmp(got, want_hash) == 0);
        CHECK(bool_of(nat, "quarantined", &flag) && flag == 1);
    } else {
        CHECK(nat == NULL);
    }
}

/* Steps one tick at a time up to `to` and compares the digest at each tick with the control run. */
static void run_to(Sess *s, const char *label, const char *wid, char *rev, size_t rcap, uint64_t to) {
    for (;;) {
        char sha[65];
        uint64_t tick = UINT64_MAX;
        Resp r;
        if (!digest(s, wid, rev, sha, &tick)) {
            fprintf(stderr, "%s: digest failed\n", label);
            CHECK(0);
            return;
        }
        if (tick > CTL_TICKS || strcmp(sha, g_ctl[tick]) != 0) {
            fprintf(stderr, "%s: tick %llu differs from the VM control run\n", label,
                    (unsigned long long)tick);
            CHECK(0);
            return;
        }
        if (tick >= to) {
            break;
        }
        if (!step1(s, wid, rev, rcap, &r)) {
            fprintf(stderr, "%s: step to tick %llu failed\n", label, (unsigned long long)(tick + 1u));
            CHECK(0);
            resp_free(&r);
            return;
        }
        resp_free(&r);
    }
}

/* ---------- control run: VM only, single-tick steps ---------- */

static void control_run(void) {
    Sess s = sess_start(NULL, NULL, "control");
    char wid[64];
    char rev[65];
    uint32_t t;
    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    CHECK(digest(&s, wid, rev, g_ctl[0], NULL));
    for (t = 1; t <= CTL_TICKS; t++) {
        Resp r;
        CHECK(step1(&s, wid, rev, sizeof rev, &r));
        resp_free(&r);
        CHECK(digest(&s, wid, rev, g_ctl[t], NULL));
    }
    sess_stop(&s);
}

/* ---------- scenarios ---------- */

/* avx2 kernel dies while computing tick 5. Recovery quarantines it: the failing request answers
 * SU_E_WORKER_FAILED with native.quarantined, the world is on the VM tier, the retry and 50 further
 * ticks equal the control run, the same artifact is refused, and scalar is admitted and bit-identical. */
static void scenario_avx2_crash(void) {
    Sess s;
    char wid[64];
    char rev[65];
    char hash[65];
    char scalar_hash[65];
    char got[65];
    char tier[32];
    Resp r;
    uint64_t old_pid;
    uint64_t new_pid = 0;
    int flag = 0;

    if (!host_avx2()) {
        g_skipped++;
        return;
    }
    s = sess_start("5", "avx2", "avx2crash");
    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    specialize(&s, wid, rev, "avx2", &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(resp_result(&r), "artifact_hash", hash, sizeof hash) && strlen(hash) == 64u);
    CHECK(str_of(resp_result(&r), "execution_tier", tier, sizeof tier) && strcmp(tier, "native_avx2") == 0);
    resp_free(&r);
    run_to(&s, "avx2crash before the fault", wid, rev, sizeof rev, CRASH_TICK - 1u);

    old_pid = active_pid(&s);
    CHECK(old_pid != 0u);
    CHECK(!step1(&s, wid, rev, sizeof rev, &r));
    expect_recovery(&r, old_pid, hash, &new_pid);
    CHECK(live_child_of((pid_t)new_pid, s.pid));
    CHECK(zombie_children(s.pid) == 0);
    resp_free(&r);

    /* VM tier after recovery; the artifact is quarantined; nothing is respecialized. */
    inspect(&s, wid, &r);
    CHECK(resp_ok(&r));
    CHECK(tier_of(&r, tier, sizeof tier) && strcmp(tier, "vm") == 0);
    CHECK(spec_of(&r) != NULL && bool_of(spec_of(&r), "pending_respecialization", &flag) && flag == 0);
    CHECK(member(spec_of(&r), "quarantined") != NULL && member(spec_of(&r), "quarantined")->u.array.count == 1);
    if (member(spec_of(&r), "quarantined") != NULL && member(spec_of(&r), "quarantined")->u.array.count > 0) {
        CHECK(str_of(member(spec_of(&r), "quarantined")->u.array.items[0], "artifact_hash", got, sizeof got) &&
              strcmp(got, hash) == 0);
    } else {
        CHECK(0);
    }
    CHECK(str_of(spec_of(&r), "respecialization", got, sizeof got) && strcmp(got, "none") == 0);
    CHECK(rev_of_inspect(&s, wid, rev, sizeof rev));
    resp_free(&r);

    /* the failed tick is retried on the VM; 50 further ticks equal the control run */
    run_to(&s, "avx2crash after recovery", wid, rev, sizeof rev, CRASH_TICK + 50u);

    /* the same artifact is refused, with the quarantine reason */
    specialize(&s, wid, rev, "avx2", &r);
    CHECK(!resp_ok(&r));
    CHECK(err_status(&r) == SU_E_CANDIDATE_FAILED);
    CHECK(err_code_is(&r, "SU_E_CANDIDATE_FAILED"));
    CHECK(str_of(member(member(r.root, "error"), "detail"), "reason", got, sizeof got) &&
          strcmp(got, "artifact_quarantined") == 0);
    CHECK(str_of(member(member(r.root, "error"), "detail"), "artifact_hash", got, sizeof got) &&
          strcmp(got, hash) == 0);
    resp_free(&r);

    /* scalar is a different artifact: admitted, installed, and bit-identical afterwards */
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(resp_result(&r), "artifact_hash", scalar_hash, sizeof scalar_hash) &&
          strcmp(scalar_hash, hash) != 0);
    CHECK(str_of(resp_result(&r), "execution_tier", tier, sizeof tier) && strcmp(tier, "native_scalar") == 0);
    resp_free(&r);
    run_to(&s, "avx2crash scalar", wid, rev, sizeof rev, CTL_TICKS);
    sess_stop(&s);
}

/* scalar and avx2 both die: each kernel is quarantined in turn; with both refused the VM keeps running
 * and matches the control run. */
static void scenario_double_crash(void) {
    Sess s;
    char wid[64];
    char rev[65];
    char avx_hash[65];
    char sc_hash[65];
    char got[65];
    Resp r;
    uint64_t pid1;
    uint64_t pid2;
    uint64_t pid3;
    int n;

    if (!host_avx2()) {
        g_skipped++;
        return;
    }
    s = sess_start("5", NULL, "double");
    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    specialize(&s, wid, rev, "avx2", &r);
    CHECK(resp_ok(&r) && str_of(resp_result(&r), "artifact_hash", avx_hash, sizeof avx_hash));
    resp_free(&r);
    run_to(&s, "double before the first fault", wid, rev, sizeof rev, CRASH_TICK - 1u);

    pid1 = active_pid(&s);
    CHECK(!step1(&s, wid, rev, sizeof rev, &r));
    expect_recovery(&r, pid1, avx_hash, &pid2);
    CHECK(live_child_of((pid_t)pid2, s.pid));
    resp_free(&r);

    specialize(&s, wid, rev, "scalar", &r);
    CHECK(resp_ok(&r) && str_of(resp_result(&r), "artifact_hash", sc_hash, sizeof sc_hash));
    resp_free(&r);
    /* the scalar kernel now computes tick 5 and dies as well */
    CHECK(!step1(&s, wid, rev, sizeof rev, &r));
    expect_recovery(&r, pid2, sc_hash, &pid3);
    CHECK(live_child_of((pid_t)pid3, s.pid));
    CHECK(pid3 != pid1 && pid3 != pid2);
    resp_free(&r);

    /* both artifacts are quarantined; the VM keeps running with no native tier */
    inspect(&s, wid, &r);
    if (!resp_ok(&r)) {
        fprintf(stderr, "diagnostic: inspect after two faults: %s\n", r.text != NULL ? (const char *)r.text : "(none)");
    }
    CHECK(resp_ok(&r));
    n = spec_of(&r) != NULL && member(spec_of(&r), "quarantined") != NULL
            ? (int)member(spec_of(&r), "quarantined")->u.array.count
            : -1;
    CHECK(n == 2);
    CHECK(tier_of(&r, got, sizeof got) && strcmp(got, "vm") == 0);
    CHECK(rev_of_inspect(&s, wid, rev, sizeof rev));
    resp_free(&r);

    specialize(&s, wid, rev, "auto", &r);
    CHECK(!resp_ok(&r) && err_status(&r) == SU_E_CANDIDATE_FAILED);
    CHECK(str_of(member(member(r.root, "error"), "detail"), "reason", got, sizeof got) &&
          strcmp(got, "artifact_quarantined") == 0);
    resp_free(&r);
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(!resp_ok(&r) && err_status(&r) == SU_E_CANDIDATE_FAILED);
    resp_free(&r);

    run_to(&s, "double after both faults", wid, rev, sizeof rev, CTL_TICKS);
    sess_stop(&s);
}

/* The worker is killed from outside while scalar is installed (no kernel fault, so nothing is
 * quarantined). The recovery queues the kernel for respecialization. Right after recovery the world
 * is on the VM tier and pending; a world.step does not service the queue; the next non-tick reply
 * after that shows the respecialized tier; the state equals the control run throughout. */
static void scenario_respecialize(void) {
    Sess s;
    char wid[64];
    char rev[65];
    char got[65];
    char tier[32];
    Resp r;
    uint64_t old_pid;
    uint64_t new_pid = 0;
    int flag = 0;
    int i;

    s = sess_start(NULL, NULL, "respec");
    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(resp_ok(&r));
    resp_free(&r);
    for (i = 0; i < 3; i++) {
        CHECK(step1(&s, wid, rev, sizeof rev, &r));
        resp_free(&r);
    }
    run_to(&s, "respec before the kill", wid, rev, sizeof rev, 3u);

    old_pid = active_pid(&s);
    CHECK(old_pid != 0u);
    CHECK(kill((pid_t)old_pid, SIGKILL) == 0);
    /* the first request after the death finds the dead worker: recovery, not executed */
    inspect(&s, wid, &r);
    expect_recovery(&r, old_pid, NULL, &new_pid);
    CHECK(live_child_of((pid_t)new_pid, s.pid));
    resp_free(&r);

    /* A tick directly after the recovery answer runs on the VM; respecialization is not run for it. */
    CHECK(step1(&s, wid, rev, sizeof rev, &r));
    CHECK(resp_ok(&r));
    resp_free(&r);

    /* The first non-tick answer after the recovery shows the pending request on the VM tier. Its
     * reply is sent before the request is serviced. */
    inspect(&s, wid, &r);
    CHECK(resp_ok(&r));
    CHECK(tier_of(&r, tier, sizeof tier) && strcmp(tier, "vm") == 0);
    CHECK(spec_of(&r) != NULL && bool_of(spec_of(&r), "pending_respecialization", &flag) && flag == 1);
    CHECK(str_of(spec_of(&r), "respecialization", got, sizeof got) && strcmp(got, "pending") == 0);
    CHECK(member(spec_of(&r), "quarantined") != NULL && member(spec_of(&r), "quarantined")->u.array.count == 0);
    CHECK(str_of(resp_result(&r), "world_revision", rev, sizeof rev));
    resp_free(&r);

    /* Serviced after that reply: the next answer shows the restored tier. */
    inspect(&s, wid, &r);
    CHECK(resp_ok(&r));
    CHECK(tier_of(&r, tier, sizeof tier) && strcmp(tier, "native_scalar") == 0);
    CHECK(spec_of(&r) != NULL && bool_of(spec_of(&r), "pending_respecialization", &flag) && flag == 0);
    CHECK(str_of(spec_of(&r), "respecialization", got, sizeof got) && strcmp(got, "respecialized") == 0);
    CHECK(str_of(resp_result(&r), "world_revision", rev, sizeof rev));
    resp_free(&r);
    run_to(&s, "respec after respecialization", wid, rev, sizeof rev, CTL_TICKS);
    sess_stop(&s);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <native test worker> <repo root> <scratch dir>\n", argv[0]);
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

    control_run();
    scenario_avx2_crash();
    scenario_double_crash();
    scenario_respecialize();

    printf("test_native_fault: %ld checks, %ld failures, %ld scenarios skipped\n", g_checks, g_fail, g_skipped);
    return g_fail == 0 ? 0 : 1;
}
