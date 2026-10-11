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
    unsetenv("SU_TEST_INTERLEAVE");
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
    CHECK(zombie_children(s.pid) == 0);

    /* The next specialization, made for the new revision, installs (the seam is one-shot). */
    specialize(&s, wid, rev1, "scalar", &r);
    CHECK(resp_ok(&r));
    CHECK(str_of(member(r.root, "result"), "execution_tier", tier, sizeof tier) &&
          strcmp(tier, "native_scalar") == 0);
    resp_free(&r);
    CHECK(active_pid(&s) == active);
    CHECK(proc_starttime((pid_t)active) == start);
    CHECK(zombie_children(s.pid) == 0);
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
    unsetenv("SU_TEST_INTERLEAVE");
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
    CHECK(zombie_children(s.pid) == 0);
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

    printf("test_specialize_inflight: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
