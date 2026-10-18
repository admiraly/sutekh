/* M1-02c-3 qualification: repeated native swaps on one agent process. Built only with SUTEKH_NATIVE.
 * Usage: test_specialize_churn <test worker> <repo root> <scratch dir>
 *
 * 1000 world.specialize cycles alternate scalar and avx2 at one revision (every install replaces the
 * previous artifact), then six revision swaps (candidate.install, alternately movement_v2 and movement),
 * each followed by specializations for the new revision. Checked on the active worker: no writable and
 * executable mapping, mapping count and RSS bounded, the old artifact never installed again for a new
 * revision, the VM tier after each swap, and world.close releasing the world (mappings do not grow).
 * The candidate slot limit (16 per agent process) bounds the number of revision swaps: see the handoff. */
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


#define CYCLES 1000
#define REVSWAPS 6

static int host_avx2(void) {
    return (su_native_host_caps() & SU_NCAP_X86_64_AVX2) != 0u;
}

#if defined(_WIN32)
/* Mappings of pid: committed regions of its address space (VirtualQueryEx), and how many are writable and
 * executable at once (must be zero). The Windows counterpart of /proc/<pid>/maps lines: region counts are not
 * comparable with Linux counts, only with themselves (growth, and a drop when kernels are released). */
static uint64_t maps_of(pid_t pid, uint64_t *wx) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *a = NULL;
    uint64_t n = 0;
    *wx = 0;
    if (h == NULL) {
        return UINT64_MAX;
    }
    while (VirtualQueryEx(h, a, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT) {
            DWORD p = mbi.Protect & 0xFFu;
            n++;
            if (p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY) {
                (*wx)++;
            }
        }
        a = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
    CloseHandle(h);
    return n;
}

/* Working set of pid in kB (su_test_rss_kib), or UINT64_MAX. */
static uint64_t rss_kb(pid_t pid) {
    long v = su_test_rss_kib((uint64_t)pid);
    return v < 0 ? UINT64_MAX : (uint64_t)v;
}
#else
/* Mappings of pid: count, and how many are writable and executable at once (must be zero). */
static uint64_t maps_of(pid_t pid, uint64_t *wx) {
    char path[64];
    char line[1024];
    FILE *f;
    uint64_t n = 0;

    *wx = 0;
    snprintf(path, sizeof path, "/proc/%ld/maps", (long)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return UINT64_MAX;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        /* perms follow the address range: " rwxp" is a writable and executable mapping. */
        const char *sp = strchr(line, ' ');
        n++;
        if (sp != NULL && strlen(sp) > 3 && sp[1] == 'r' && sp[2] == 'w' && sp[3] == 'x') {
            (*wx)++;
        }
    }
    fclose(f);
    return n;
}

/* VmRSS of pid in kB, or UINT64_MAX. */
static uint64_t rss_kb(pid_t pid) {
    char path[64];
    char line[256];
    FILE *f;
    uint64_t v = UINT64_MAX;

    snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return UINT64_MAX;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            v = strtoull(line + 6, NULL, 10);
        }
    }
    fclose(f);
    return v;
}

#endif

/* The art-hash and tier of a successful specialize answer. */
static int spec_answer(const Resp *r, char *hash, size_t hcap, char *tier, size_t tcap) {
    const SuJsonValue *res = member(r->root, "result");
    return str_of(res, "artifact_hash", hash, hcap) && str_of(res, "execution_tier", tier, tcap);
}

static void expect_tier(Sess *s, const char *wid, const char *want, const char *label) {
    Resp ir;
    char tier[32];
    inspect(s, wid, &ir);
    CHECK(resp_ok(&ir));
    CHECK(tier_of(&ir, tier, sizeof tier) && strcmp(tier, want) == 0);
    if (strcmp(tier, want) != 0) {
        fprintf(stderr, "%s: tier %s, expected %s\n", label, tier, want);
    }
    resp_free(&ir);
}

/* CYCLES specializations alternate scalar and avx2 at one revision; every installed artifact replaces the
 * previous one. Then REVSWAPS revision swaps (candidate.install of v2 and of v1 alternately), each followed by
 * a specialization for the new revision. Mapping, RSS and the retirement of the old artifacts are checked
 * on the active worker; world.close releases the world. */
int main(int argc, char **argv) {
    Sess s;
    char wid[64];
    char rev[65];
    char cid[32];
    char hash[65];
    char tier[32];
    char prev_hash[65];
    char want_tier[32];
    uint64_t base_maps = 0;
    uint64_t base_wx = 0;
    uint64_t max_maps = 0;
    uint64_t wx = 0;
    uint64_t base_rss = 0;
    uint64_t rss = 0;
    uint64_t maps_pre_close = 0;
    uint64_t maps_post_close = 0;
    uint64_t active = 0;
    long long start = -1;
    int avx2;
    int i;
    int swap;
    Resp r;
    char line[512];

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
    copy_fixture("movement_v2.capsule.json");
    copy_fixture("movement_v2.lir.json");
    copy_fixture("movement_v2.test.json");
    avx2 = host_avx2();

    s = sess_start(NULL, NULL, 120000, "churn");
    CHECK(create_movement(&s, wid, sizeof wid, rev, sizeof rev));
    active = active_pid(&s);
    start = proc_starttime((pid_t)active);
    CHECK(active != 0u);

    /* Warm-up: both backends installed once, so the mapping count is measured in steady state. */
    specialize(&s, wid, rev, "scalar", &r);
    CHECK(resp_ok(&r));
    resp_free(&r);
    if (avx2) {
        specialize(&s, wid, rev, "avx2", &r);
        CHECK(resp_ok(&r));
        resp_free(&r);
    }
    base_maps = maps_of((pid_t)active, &base_wx);
    base_rss = rss_kb((pid_t)active);
    max_maps = base_maps;
    CHECK(base_wx == 0u);

    for (i = 0; i < CYCLES; i++) {
        int use_avx2 = avx2 && (i % 2) == 1;
        strcpy(want_tier, use_avx2 ? "native_avx2" : "native_scalar");
        specialize(&s, wid, rev, use_avx2 ? "avx2" : "scalar", &r);
        if (!resp_ok(&r) || !spec_answer(&r, hash, sizeof hash, tier, sizeof tier) ||
            strcmp(tier, want_tier) != 0 || strlen(hash) != 64u) {
            CHECK(0);
            fprintf(stderr, "cycle %d failed\n", i);
            resp_free(&r);
            break;
        }
        resp_free(&r);
        if (i % 50 == 0 || i == CYCLES - 1) {
            {
                uint64_t n = maps_of((pid_t)active, &wx);
                if (n > max_maps && n != UINT64_MAX) {
                    max_maps = n;
                }
                CHECK(wx == 0u);
                CHECK(n != UINT64_MAX && n <= base_maps + 4u);
            }
            CHECK(active_pid(&s) == active);
            CHECK(zombies_of(&s, active) == 0);
        }
    }
    CHECK(proc_starttime((pid_t)active) == start);
    rss = rss_kb((pid_t)active);
    CHECK(rss != UINT64_MAX && base_rss != UINT64_MAX && rss <= base_rss + 16384u);
    printf("churn: %d specialize cycles, maps base %llu max %llu, rss base %llukB end %llukB\n", CYCLES,
           (unsigned long long)base_maps, (unsigned long long)max_maps, (unsigned long long)base_rss,
           (unsigned long long)rss);

    /* Revision swaps: each candidate.install moves the world to a new semantic revision. */
    strcpy(prev_hash, "");
    for (swap = 0; swap < REVSWAPS; swap++) {
        const char *cap = (swap % 2 == 0) ? "movement_v2.capsule.json" : "movement.capsule.json";
        const char *tst = (swap % 2 == 0) ? "movement_v2.test.json" : "movement.test.json";
        char prev_rev[65];
        strcpy(prev_rev, rev);
        CHECK(submit_and_eval(&s, cap, tst, cid, sizeof cid));
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"ci\",\"method\":\"candidate.install\",\"params\":{\"candidate_id\":\"%s\","
                 "\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"allowed_effective_tick\":0}}",
                 cid, wid, rev);
        call(&s, line, &r, 120000);
        CHECK(resp_ok(&r));
        resp_free(&r);
        CHECK(world_rev(&s, wid, rev, sizeof rev));
        CHECK(strcmp(rev, prev_rev) != 0);
        expect_tier(&s, wid, "vm", "after revision swap");
        /* A specialization still asking for the old revision is refused: no artifact for it is installed. */
        specialize(&s, wid, prev_rev, "scalar", &r);
        CHECK(!resp_ok(&r));
        CHECK(err_status(&r) != UINT64_MAX);
        resp_free(&r);
        expect_tier(&s, wid, "vm", "after stale specialize");
        specialize(&s, wid, rev, "scalar", &r);
        CHECK(resp_ok(&r) && spec_answer(&r, hash, sizeof hash, tier, sizeof tier));
        CHECK(strcmp(tier, "native_scalar") == 0);
        /* The artifact for the new revision is not the one installed for the old revision. */
        CHECK(prev_hash[0] == '\0' || strcmp(hash, prev_hash) != 0);
        strcpy(prev_hash, hash);
        resp_free(&r);
        if (avx2) {
            specialize(&s, wid, rev, "avx2", &r);
            CHECK(resp_ok(&r) && spec_answer(&r, hash, sizeof hash, tier, sizeof tier));
            CHECK(strcmp(tier, "native_avx2") == 0);
            resp_free(&r);
        }
        CHECK(active_pid(&s) == active);
        CHECK(zombies_of(&s, active) == 0);
    }
    maps_pre_close = maps_of((pid_t)active, &wx);
    CHECK(wx == 0u);

    /* world.close releases the world: no native kernel survives, and the worker's mappings shrink. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wc\",\"method\":\"world.close\",\"params\":{\"world_id\":\"%s\","
             "\"expected_world_revision\":\"%s\"}}",
             wid, rev);
    call(&s, line, &r, 120000);
    CHECK(resp_ok(&r));
    resp_free(&r);
    CHECK(!world_exists(&s, wid));
    maps_post_close = maps_of((pid_t)active, &wx);
    CHECK(wx == 0u);
    /* The closed world's native kernel is released: its mapping is gone, so the count shrinks. */
    CHECK(maps_post_close < maps_pre_close);
    printf("churn: %d revision swaps; maps before close %llu, after close %llu\n", REVSWAPS,
           (unsigned long long)maps_pre_close, (unsigned long long)maps_post_close);
    CHECK(active_pid(&s) == active);
    sess_stop(&s);

    printf("test_specialize_churn: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
