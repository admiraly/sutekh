/* M1-02c-3 qualification: in-flight ordering of world.specialize, and world.close during admission.
 * Built only with SUTEKH_NATIVE. Usage: test_specialize_inflight <test worker> <repo root> <scratch dir>
 *
 * The supervisor is su_agent_test: the agent library compiled with SU_TEST_FAULT_HOOKS, which adds one
 * seam (test_interleave in src/agent/su_agent.c). The environment variable SU_TEST_INTERLEAVE, read by
 * the agent process, makes the supervisor itself issue one request between a specialization's admission
 * and its install, through its own request router: "candidate" = candidate.install of a passed candidate
 * (a new semantic revision), "close" = world.close. The seam fires once per agent process.
 * The production sutekh binary is built from su_agent without this macro (check_release_seams).
 *
 * Covers: a stale-revision install is discarded, the old artifact is not installed, the VM tier serves
 * the new revision, and a specialization for the new revision installs; world.close during admission
 * gives a clean error for the pending install and leaves the supervisor and its worker serving. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include "../../common/su_test_os.h"
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
    SuTestServe serve; /* supervisor under test (tests/common/su_test_os.h) */
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


/* ---------- agent sessions: one agent process per scenario ---------- */

/* Sets (value != NULL) or removes an environment variable before a session starts. */
static void fault_env(const char *name, const char *value) {
    if (value != NULL) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

/* Seam variables are set before the supervisor starts. POSIX: the forked agent keeps its copy. Windows: the agent
 * is a thread of this process and reads this environment, so su_test_unset_after_start removes them only when the
 * session ends. */
static Sess sess_start(const char *fault, const char *fault_ms, uint64_t timeout_ms, const char *tag) {
    Sess s;
    int wfd;
    int rfd;
    static char spool[1200]; /* static: on Windows the supervisor thread reads cfg strings after this returns */
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
    fault_env("SU_TEST_FAULT", fault);
    fault_env("SU_TEST_FAULT_MS", fault_ms);
    memset(&s, 0, sizeof s);
    su_test_serve_start(&s.serve, &cfg, &wfd, &rfd);
    if (fault != NULL) {
        su_test_unset_after_start("SU_TEST_FAULT");
    }
    if (fault_ms != NULL) {
        su_test_unset_after_start("SU_TEST_FAULT_MS");
    }
    s.wfd = wfd;
    su_line_reader_init(&s.rd, rfd, -1);
    return s;
}

static void sess_stop(Sess *s) {
    close(s->wfd);
    su_line_reader_free(&s->rd);
    CHECK(su_test_serve_wait(&s->serve));
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

#if defined(_WIN32)
/* Creation time of a process (GetProcessTimes, 100 ns units since 1601), or -1. Like the /proc start time it
 * identifies the process instance (a replacement reusing the pid differs); unlike it, it cannot be read once the
 * process is gone. */
static long long proc_starttime(pid_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    FILETIME created, ended, kern, user;
    long long st = -1;
    if (h == NULL) {
        return -1;
    }
    if (GetProcessTimes(h, &created, &ended, &kern, &user)) {
        st = (long long)(((unsigned long long)created.dwHighDateTime << 32) | created.dwLowDateTime);
    }
    CloseHandle(h);
    return st;
}

/* Windows has no zombies (an ended child leaves nothing to reap). The counterpart of "no zombie child" is: no
 * child of the agent (this test process: the supervisor is a thread) created after it is still running except the
 * active worker. Toolhelp snapshot; parent ids survive their parent and pids are reused, hence the time filter. */
static int leftover_children(uint64_t agent, uint64_t keep) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    long long born = proc_starttime((pid_t)agent);
    int n = 0;
    if (snap == INVALID_HANDLE_VALUE || born < 0) {
        if (snap != INVALID_HANDLE_VALUE) {
            CloseHandle(snap);
        }
        return -1;
    }
    memset(&pe, 0, sizeof pe);
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            uint64_t id = (uint64_t)pe.th32ProcessID;
            if ((uint64_t)pe.th32ParentProcessID == agent && id != keep && su_test_pid_alive(id) &&
                proc_starttime((pid_t)id) >= born) {
                n++;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

/* Zombie children (POSIX) or leftover running children other than the active worker (Windows) of the agent. */
static int zombies_of(const Sess *s, uint64_t active) {
    (void)s;
    return leftover_children((uint64_t)GetCurrentProcessId(), active);
}
#else
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

static int zombies_of(const Sess *s, uint64_t active) {
    (void)active;
    return zombie_children(s->serve.pid);
}
#endif

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


/* ---------- fixtures from tests/fixtures/edit ---------- */

static void copy_fixture(const char *name) {
    char src[1200];
    char dst[1200];
    FILE *fi;
    FILE *fo;
    char buf[4096];
    size_t n;

    snprintf(src, sizeof src, "%s/tests/fixtures/edit/%s", g_root, name);
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

/* capsule.submit then candidate.evaluate (must pass). cid receives the candidate id. */
static int submit_and_eval(Sess *s, const char *capsule, const char *test, char *cid, size_t cap) {
    Resp r;
    char line[512];
    char st[16];
    int ok;

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"cs\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"%s\"}}",
             capsule);
    call(s, line, &r, 120000);
    ok = resp_ok(&r) && str_of(member(r.root, "result"), "candidate_id", cid, cap);
    resp_free(&r);
    if (!ok) {
        return 0;
    }
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ce\",\"method\":\"candidate.evaluate\",\"params\":{\"candidate_id\":\"%s\","
             "\"test_path\":\"%s\"}}",
             cid, test);
    call(s, line, &r, 120000);
    ok = resp_ok(&r) && str_of(member(r.root, "result"), "status", st, sizeof st) && strcmp(st, "passed") == 0;
    resp_free(&r);
    return ok;
}

static int world_exists(Sess *s, const char *wid) {
    Resp r;
    int ok;
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\"}}",
             wid);
    call(s, line, &r, 120000);
    ok = resp_ok(&r);
    resp_free(&r);
    return ok;
}

/* world.inspect of wid (caller frees r). */
static void inspect(Sess *s, const char *wid, Resp *r) {
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\"}}",
             wid);
    call(s, line, r, 120000);
}

/* The execution tier of the system (semantic_manifest[0]) in a world.inspect answer. */
static int tier_of(const Resp *r, char *out, size_t cap) {
    const SuJsonValue *man = member(member(r->root, "result"), "semantic_manifest");
    if (man == NULL || man->type != SU_JSON_ARRAY || man->u.array.count == 0) {
        return 0;
    }
    return str_of(man->u.array.items[0], "execution_tier", out, cap);
}

/* M2-01 fixes3: the native registry of the worker, read through the test supervisor's internal.stats (the kernels live
 * in the worker process). registry_live and registry_released are process totals; world_artifacts counts the artifacts
 * the used worlds' schedules reference (seq and registry state of the first four). */
#define NAT_INSTALLED 3u /* SU_NV_STATE_INSTALLED (runtime/native_versions.h) */

typedef struct NatStat {
    uint64_t live;
    uint64_t released;
    uint64_t world_artifacts;
    uint64_t n;
    uint64_t seq[4];
    uint64_t state[4];
} NatStat;

static int native_stat(Sess *s, NatStat *out);

/* ---------- in-flight scenarios (the supervisor issues the interleaved request once) ---------- */

/* A new semantic revision (candidate.install of movement_v2) lands between admission and install of a
 * specialization made for the old revision. The stale install must be refused, the world must run the VM
 * tier at the new revision, the old specialization must not be installed, and a specialization made for
 * the new revision must install. */
static void scenario_interleave_candidate(void) {
    Sess s;
    char wid[64];
    char rev0[65];
    char rev1[65];
    char cid[32];
    char tier[32];
    Resp r;
    uint64_t active;
    long long start;

    setenv("SU_TEST_INTERLEAVE", "candidate", 1);
    s = sess_start(NULL, NULL, 120000, "ilc");
    su_test_unset_after_start("SU_TEST_INTERLEAVE");
    CHECK(create_movement(&s, wid, sizeof wid, rev0, sizeof rev0));
    active = active_pid(&s);
    start = proc_starttime((pid_t)active);
    copy_fixture("movement_v2.capsule.json");
    copy_fixture("movement_v2.lir.json");
    copy_fixture("movement_v2.test.json");
    CHECK(submit_and_eval(&s, "movement_v2.capsule.json", "movement_v2.test.json", cid, sizeof cid));

    /* The armed request fires between this specialization's admission and its install. */
    specialize(&s, wid, rev0, "scalar", &r);
    CHECK(!resp_ok(&r));
    CHECK(member(r.root, "error") != NULL);
    CHECK(err_status(&r) != UINT64_MAX);
    resp_free(&r);
    CHECK(world_rev(&s, wid, rev1, sizeof rev1));
    CHECK(strcmp(rev1, rev0) != 0);
    /* The new revision runs the VM tier: nothing native for rev0 survives. */
    {
        Resp ir;
        inspect(&s, wid, &ir);
        CHECK(resp_ok(&ir));
        CHECK(tier_of(&ir, tier, sizeof tier) && strcmp(tier, "vm") == 0);
        resp_free(&ir);
    }
    CHECK(active_pid(&s) == active);
    CHECK(zombies_of(&s, active) == 0);

    /* The next specialization, made for the new revision, installs (the seam is one-shot). */
    specialize(&s, wid, rev1, "scalar", &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "execution_tier", tier, sizeof tier) &&
          strcmp(tier, "native_scalar") == 0);
    resp_free(&r);
    CHECK(active_pid(&s) == active);
    CHECK(proc_starttime((pid_t)active) == start);
    CHECK(zombies_of(&s, active) == 0);
    sess_stop(&s);
}

/* world.close between admission and install: the pending install gets a clean error, nothing is left
 * installed, and the supervisor and its active worker keep serving. */
static void scenario_interleave_close(void) {
    Sess s;
    char wid[64];
    char rev[65];
    Resp r;
    uint64_t active;
    const SuJsonValue *err;
    char msg[256];

    setenv("SU_TEST_INTERLEAVE", "close", 1);
    s = sess_start(NULL, NULL, 120000, "ilcl");
    su_test_unset_after_start("SU_TEST_INTERLEAVE");
    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    active = active_pid(&s);
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(!resp_ok(&r));
    err = member(r.root, "error");
    CHECK(err != NULL);
    CHECK(err_status(&r) != UINT64_MAX);
    CHECK(str_of(err, "message", msg, sizeof msg));
    if (!resp_ok(&r)) {
        fprintf(stderr, "close interleave: %s\n", msg);
    }
    resp_free(&r);
    CHECK(!world_exists(&s, wid));
    CHECK(active_pid(&s) == active);
    CHECK(zombies_of(&s, active) == 0);
    sess_stop(&s);
}

/* ---------- layout generation in flight (M2-01 fixes2, BLK-3 (iii)) ---------- */

/* The raw text of member key (an object or an array) of a reply text, or 0 when absent or too long. */
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

/* The migration: a field no system uses (id 3, "bonus") and layout generation 1 -> 2. */
static void write_migration(void) {
    char path[1200];
    FILE *f;
    snprintf(path, sizeof path, "%s/m_gen2.json", g_proj);
    f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fputs("{\"migration_format\":\"0.1\",\"id\":\"m-gen2-bonus\",\"from_layout_generation\":1,"
          "\"to_layout_generation\":2,\"ops\":[{\"op\":\"add_field\",\"field\":{\"id\":3,\"name\":\"bonus\","
          "\"type\":\"u32\",\"default\":5}}]}",
          f);
    fclose(f);
}

/* world.migrate_plan of m_gen2.json; pid receives the plan id. Succeeds when the plan is valid. */
static int plan_gen2(Sess *s, const char *wid, const char *rev, char *pid, size_t cap) {
    Resp r;
    char line[512];
    const SuJsonValue *pl;
    int ok;

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"mp\",\"method\":\"world.migrate_plan\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"migration_path\":\"m_gen2.json\"}}",
             wid, rev);
    call(s, line, &r, 120000);
    ok = resp_ok(&r) && str_of(member(r.root, "result"), "plan_id", pid, cap);
    pl = member(member(r.root, "result"), "plan");
    ok = ok && pl != NULL && member(pl, "valid") != NULL && member(pl, "valid")->type == SU_JSON_BOOL &&
         member(pl, "valid")->u.boolean == 1;
    resp_free(&r);
    return ok;
}

/* world.migrate_commit of plan pid at rev, resubmitting candidate cid for demo.movement. r is the caller's reply. */
static int commit_gen2(Sess *s, const char *wid, const char *pid, const char *rev, const char *cid, Resp *r) {
    char line[1024];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"mc\",\"method\":\"world.migrate_commit\",\"params\":{\"world_id\":\"%s\","
             "\"plan_id\":\"%s\",\"expected_world_revision\":\"%s\",\"resubmit\":[{\"system_id\":\"demo.movement\","
             "\"candidate_id\":\"%s\"}]}}",
             wid, pid, rev, cid);
    call(s, line, r, 120000);
    return resp_ok(r);
}

/* world.step of ticks at rev; rev receives the new revision. */
static int step_n(Sess *s, const char *wid, char *rev, size_t cap, uint64_t ticks) {
    Resp r;
    char line[512];
    int ok;

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"st\",\"method\":\"world.step\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"ticks\":%llu}}",
             wid, rev, (unsigned long long)ticks);
    call(s, line, &r, 120000);
    ok = resp_ok(&r) && str_of(member(r.root, "result"), "world_revision", rev, cap);
    resp_free(&r);
    return ok;
}

/* The stored values of the unaffected fields (position_x, velocity_x) and the tick of wid (world.inspect). */
static int columns_of(Sess *s, const char *wid, char *cols, size_t cap, uint64_t *tick) {
    Resp r;
    char line[512];
    int ok;

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wi\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\","
             "\"fields\":[\"position_x\",\"velocity_x\"],\"limit\":16}}",
             wid);
    call(s, line, &r, 120000);
    ok = resp_ok(&r) && raw_member((const char *)r.text, "columns", cols, cap);
    if (ok && tick != NULL) {
        *tick = uint_of(member(r.root, "result"), "tick");
    }
    resp_free(&r);
    return ok;
}

/* A layout generation lands between the admission of a native specialization (made at generation 1) and its install.
 * The supervisor commits the migration in the seam (add a field no system uses; demo.movement resubmitted at
 * generation 2). Expected: the install is refused (the revision moved); the world runs the VM tier at generation 2
 * (the kernel of the replaced schedule is gone); a specialization made at generation 2 installs; and after the same
 * ticks the unaffected columns and the tick equal those of a VM-only control world that received the same migration.
 * The release of the old kernel is observed through the tier (the test worker is a separate process; su_nv_state is
 * not visible from here). */
static void scenario_interleave_migrate(void) {
    Sess s;
    char wid[64];
    char wid2[64];
    char rev0[65];
    char rev1[65];
    char rev2[65];
    char cid[32];
    char pid[32];
    char pid2[32];
    char tier[32];
    char cols_a[4096];
    char cols_b[4096];
    Resp r;
    uint64_t active;
    uint64_t ta = 0;
    uint64_t tb = 0;
    long long start;

    setenv("SU_TEST_INTERLEAVE", "migrate", 1);
    s = sess_start(NULL, NULL, 120000, "ilm");
    su_test_unset_after_start("SU_TEST_INTERLEAVE");
    CHECK(create_movement(&s, wid, sizeof wid, rev0, sizeof rev0));
    active = active_pid(&s);
    start = proc_starttime((pid_t)active);
    copy_fixture("movement_gen2.capsule.json");
    copy_fixture("movement_gen2.test.json");
    write_migration();
    CHECK(submit_and_eval(&s, "movement_gen2.capsule.json", "movement_gen2.test.json", cid, sizeof cid));
    CHECK(plan_gen2(&s, wid, rev0, pid, sizeof pid));

    /* Admission at generation 1; the seam commits the migration before the install, which is refused as stale. */
    specialize(&s, wid, rev0, "scalar", &r);
    CHECK(!resp_ok(&r));
    CHECK(member(r.root, "error") != NULL);
    CHECK(err_status(&r) == SU_E_STALE_REVISION);
    resp_free(&r);
    CHECK(world_rev(&s, wid, rev1, sizeof rev1));
    CHECK(strcmp(rev1, rev0) != 0);
    {
        Resp ir;
        inspect(&s, wid, &ir);
        CHECK(resp_ok(&ir));
        CHECK(tier_of(&ir, tier, sizeof tier) && strcmp(tier, "vm") == 0);
        CHECK(uint_of(member(ir.root, "result"), "layout_generation") == 2u);
        resp_free(&ir);
    }
    CHECK(active_pid(&s) == active);
    CHECK(zombies_of(&s, active) == 0);

    /* The released kernel is not brought back: a worker death while the world is idle recovers it at generation 2
     * with the VM tier (the supervisor's records of the replaced schedule were cleared by the commit). */
    CHECK(su_test_kill_pid(active) == 0);
    {
        Resp ir;
        /* The first request after the death reports SU_E_WORKER_FAILED (retry: true) and starts the replacement. */
        inspect(&s, wid, &ir);
        if (!resp_ok(&ir)) {
            resp_free(&ir);
            inspect(&s, wid, &ir);
        }
        CHECK(resp_ok(&ir));
        CHECK(tier_of(&ir, tier, sizeof tier) && strcmp(tier, "vm") == 0);
        CHECK(uint_of(member(ir.root, "result"), "layout_generation") == 2u);
        resp_free(&ir);
    }
    active = active_pid(&s);
    CHECK(active != 0u);
    start = proc_starttime((pid_t)active);

    /* A specialization made at generation 2 installs. */
    specialize(&s, wid, rev1, "scalar", &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "execution_tier", tier, sizeof tier) && strcmp(tier, "native_scalar") == 0);
    resp_free(&r);
    {
        /* Bindings regenerated: the world's only artifact is the one compiled for layout generation 2 (installed). */
        NatStat ng;
        CHECK(native_stat(&s, &ng));
        CHECK(ng.world_artifacts == 1u && ng.n == 1u && ng.state[0] == NAT_INSTALLED);
    }
    CHECK(active_pid(&s) == active);
    CHECK(proc_starttime((pid_t)active) == start);

    /* Control: a second world, VM only, with the same migration (the seam is one-shot, so this commit is not seamed). */
    CHECK(create_movement(&s, wid2, sizeof wid2, rev2, sizeof rev2));
    CHECK(plan_gen2(&s, wid2, rev2, pid2, sizeof pid2));
    CHECK(commit_gen2(&s, wid2, pid2, rev2, cid, &r));
    CHECK(str_of(member(r.root, "result"), "world_revision", rev2, sizeof rev2));
    resp_free(&r);

    CHECK(step_n(&s, wid, rev1, sizeof rev1, 10));
    CHECK(step_n(&s, wid2, rev2, sizeof rev2, 10));
    CHECK(columns_of(&s, wid, cols_a, sizeof cols_a, &ta));
    CHECK(columns_of(&s, wid2, cols_b, sizeof cols_b, &tb));
    CHECK(ta == 10u && tb == 10u);
    CHECK(strcmp(cols_a, cols_b) == 0);
    {
        Resp ir;
        inspect(&s, wid, &ir);
        CHECK(resp_ok(&ir));
        CHECK(tier_of(&ir, tier, sizeof tier) && strcmp(tier, "native_scalar") == 0);
        resp_free(&ir);
    }
    CHECK(active_pid(&s) == active);
    CHECK(zombies_of(&s, active) == 0);
    sess_stop(&s);
}

/* Reads NatStat (see above) through the test supervisor's internal.stats. Returns 0 when the answer has no native object. */
static int native_stat(Sess *s, NatStat *out) {
    Resp r;
    const SuJsonValue *nat = NULL;
    const SuJsonValue *arr = NULL;
    uint64_t i;
    int ok;

    memset(out, 0, sizeof *out);
    call(s, "{\"protocol\":\"0.1\",\"id\":\"ns\",\"method\":\"internal.stats\",\"params\":{}}", &r, 120000);
    ok = resp_ok(&r);
    if (ok) {
        nat = member(member(r.root, "result"), "native");
    }
    ok = nat != NULL;
    if (ok) {
        out->live = uint_of(nat, "registry_live");
        out->released = uint_of(nat, "registry_released");
        out->world_artifacts = uint_of(nat, "world_artifacts");
        arr = member(nat, "artifacts");
        if (arr != NULL && arr->type == SU_JSON_ARRAY) {
            out->n = arr->u.array.count;
            for (i = 0; i < arr->u.array.count && i < 4u; i++) {
                out->seq[i] = uint_of(arr->u.array.items[i], "seq");
                out->state[i] = uint_of(arr->u.array.items[i], "state");
            }
        }
    }
    resp_free(&r);
    return ok;
}

/* A generation-1 kernel is installed and the migration commits (the swap releases it). A generation-2 kernel is installed
 * for the committed layout, and the migration is rolled back: the restore releases that kernel and clears the
 * supervisor's native records, so the worker death that follows (SUTEKH_TEST_KILL_AT_TICK seam, once) has no native
 * member in its recovery object. Records that survived the rollback would be attributed to the death (MIG-X10). */
static void scenario_rollback_native(void) {
    Sess s;
    char wid[64];
    char rev[65];
    char cid[32];
    char pid[32];
    char tok[1100];
    char line[512];
    char tier[32];
    Resp r;
    const SuJsonValue *rec;
    NatStat pre;
    NatStat post;
    NatStat g2;

    snprintf(tok, sizeof tok, "%s/token-rollback-native-kill", g_scratch);
    {
        FILE *f = fopen(tok, "wb");
        if (f == NULL) {
            perror(tok);
            exit(2);
        }
        fputs("armed\n", f);
        fclose(f);
    }
    setenv("SUTEKH_TEST_KILL_AT_TICK", "1", 1);
    setenv("SUTEKH_TEST_KILL_TOKEN", tok, 1);
    s = sess_start(NULL, NULL, 120000, "rbn");
    su_test_unset_after_start("SUTEKH_TEST_KILL_AT_TICK");
    su_test_unset_after_start("SUTEKH_TEST_KILL_TOKEN");
    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    copy_fixture("movement_gen2.capsule.json");
    copy_fixture("movement_gen2.test.json");
    write_migration();
    CHECK(submit_and_eval(&s, "movement_gen2.capsule.json", "movement_gen2.test.json", cid, sizeof cid));

    specialize(&s, wid, rev, "scalar", &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "execution_tier", tier, sizeof tier) && strcmp(tier, "native_scalar") == 0);
    resp_free(&r);
    CHECK(world_rev(&s, wid, rev, sizeof rev));
    CHECK(native_stat(&s, &pre));
    CHECK(pre.world_artifacts == 1u && pre.n == 1u && pre.state[0] == NAT_INSTALLED);

    /* Commit (the gen-1 kernel is released), then a gen-2 kernel for the committed layout is installed. */
    CHECK(plan_gen2(&s, wid, rev, pid, sizeof pid));
    CHECK(commit_gen2(&s, wid, pid, rev, cid, &r));
    CHECK(str_of(member(r.root, "result"), "world_revision", rev, sizeof rev));
    resp_free(&r);
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "execution_tier", tier, sizeof tier) && strcmp(tier, "native_scalar") == 0);
    resp_free(&r);
    CHECK(world_rev(&s, wid, rev, sizeof rev));
    CHECK(native_stat(&s, &g2));
    CHECK(g2.world_artifacts == 1u && g2.n == 1u && g2.state[0] == NAT_INSTALLED);
    CHECK(g2.seq[0] != pre.seq[0]); /* bindings regenerated: a new artifact for the new layout */

    /* Rollback releases the gen-2 kernel of the schedule it replaces. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rb\",\"method\":\"world.migrate_rollback\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             wid, rev);
    call(&s, line, &r, 120000);
    CHECK(resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "world_revision", rev, sizeof rev));
    resp_free(&r);
    CHECK(native_stat(&s, &post));
    CHECK(post.world_artifacts == 0u && post.n == 0u);
    CHECK(post.live + g2.world_artifacts == g2.live);
    CHECK(post.released == g2.released + g2.world_artifacts);
    {
        Resp ir;
        inspect(&s, wid, &ir);
        CHECK(resp_ok(&ir));
        CHECK(tier_of(&ir, tier, sizeof tier) && strcmp(tier, "vm") == 0);
        CHECK(uint_of(member(ir.root, "result"), "layout_generation") == 1u);
        resp_free(&ir);
    }

    /* The worker dies at tick 1 after the rollback: the recovery object has no native member. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"st\",\"method\":\"world.step\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"ticks\":1}}",
             wid, rev);
    call(&s, line, &r, 120000);
    CHECK(!resp_ok(&r));
    CHECK(err_status(&r) == 15u);
    rec = member(r.root, "recovery");
    CHECK(rec != NULL);
    CHECK(member(rec, "native") == NULL);
    resp_free(&r);
    CHECK(step_n(&s, wid, rev, sizeof rev, 1));
    sess_stop(&s);
}

/* A generation-1 kernel is installed, the migration commits (the swap releases the kernel), and the worker then dies
 * while the next step computes tick 1 (SUTEKH_TEST_KILL_AT_TICK seam, once). The recovery must not attribute the death
 * to a kernel: the commit cleared the supervisor's records of the replaced schedule, so nothing is quarantined and the
 * recovery object has no native member. The retry then steps the world. */
static void scenario_migrate_step_death(void) {
    Sess s;
    char wid[64];
    char rev[65];
    char cid[32];
    char pid[32];
    char tok[1100];
    char line[512];
    char tier[32];
    Resp r;
    const SuJsonValue *rec;
    const SuJsonValue *quar;
    uint64_t pre_live = 0;
    uint64_t pre_released = 0;
    uint64_t pre_world = 0;

    snprintf(tok, sizeof tok, "%s/token-migrate-step-kill", g_scratch);
    {
        FILE *f = fopen(tok, "wb");
        if (f == NULL) {
            perror(tok);
            exit(2);
        }
        fputs("armed\n", f);
        fclose(f);
    }
    setenv("SUTEKH_TEST_KILL_AT_TICK", "1", 1);
    setenv("SUTEKH_TEST_KILL_TOKEN", tok, 1);
    s = sess_start(NULL, NULL, 120000, "ilmd");
    su_test_unset_after_start("SUTEKH_TEST_KILL_AT_TICK");
    su_test_unset_after_start("SUTEKH_TEST_KILL_TOKEN");
    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    copy_fixture("movement_gen2.capsule.json");
    copy_fixture("movement_gen2.test.json");
    write_migration();
    CHECK(submit_and_eval(&s, "movement_gen2.capsule.json", "movement_gen2.test.json", cid, sizeof cid));

    specialize(&s, wid, rev, "scalar", &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "execution_tier", tier, sizeof tier) && strcmp(tier, "native_scalar") == 0);
    resp_free(&r);
    CHECK(world_rev(&s, wid, rev, sizeof rev));
    {
        /* M2-01 fixes3: the generation-1 kernel is installed before the commit (old-artifact release, kill X14). */
        NatStat nb;
        CHECK(native_stat(&s, &nb));
        CHECK(nb.world_artifacts == 1u && nb.n == 1u && nb.state[0] == NAT_INSTALLED);
        pre_live = nb.live;
        pre_released = nb.released;
        pre_world = nb.world_artifacts;
    }

    CHECK(plan_gen2(&s, wid, rev, pid, sizeof pid));
    CHECK(commit_gen2(&s, wid, pid, rev, cid, &r));
    CHECK(str_of(member(r.root, "result"), "world_revision", rev, sizeof rev));
    resp_free(&r);
    {
        /* The swap released the old schedule's kernel: the registry lost it, the world holds none, and the release is
         * counted. Read before the seam kills the worker, so the answer comes from the worker that did the swap. */
        NatStat na;
        CHECK(native_stat(&s, &na));
        CHECK(na.world_artifacts == 0u && na.n == 0u);
        CHECK(na.live + pre_world == pre_live);
        CHECK(na.released == pre_released + pre_world);
    }

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"st\",\"method\":\"world.step\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\",\"ticks\":1}}",
             wid, rev);
    call(&s, line, &r, 120000);
    CHECK(!resp_ok(&r));
    CHECK(err_status(&r) == 15u);
    rec = member(r.root, "recovery");
    CHECK(rec != NULL);
    CHECK(member(rec, "native") == NULL);
    quar = member(rec, "quarantined");
    CHECK(quar != NULL && quar->type == SU_JSON_ARRAY && quar->u.array.count == 0u);
    resp_free(&r);

    CHECK(step_n(&s, wid, rev, sizeof rev, 1));
    sess_stop(&s);
}

int main(int argc, char **argv) {
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

    scenario_interleave_candidate();
    scenario_interleave_close();
    scenario_interleave_migrate();
    scenario_migrate_step_death();
    scenario_rollback_native();

    printf("test_specialize_inflight: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
