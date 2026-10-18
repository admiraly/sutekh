/* M0-08 control-plane tests: world.snapshot and world.restore through the agent, served by
 * the active worker with its bounded checkpoint store.
 *
 * Covers T08 (round trip through the control plane: exact columns, tick and manifest;
 * restore then continued stepping equals an uninterrupted run; restore across an
 * installed code swap restores the old manifest and behaviour), stale revisions (no work
 * done), unknown and evicted snapshot ids (SU_E_INVALID_ARGUMENT), a snapshot of another
 * world, and a failed restore (changed revision bytes -> SU_E_SCHEMA_MISMATCH) that leaves
 * the world bit-identical: the canonical snapshot digest before and after is the same.
 *
 * Expected states come from the C reference model below (the same arithmetic as the
 * fixtures in tests/reload), never from the VM. The worker process must not change.
 *
 * Usage: test_snapshot_cp <worker_helper_path> <repo_root>
 *   repo_root  absolute repository path. Its tests/reload JSON files are copied into a
 *              scratch project root that is removed at the end.
 * Environment:
 *   SU_TEST_SCRATCH  base directory for the scratch root (default $TMPDIR, then /tmp). */
#define _POSIX_C_SOURCE 200809L

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

static int g_checks = 0;
static int g_failures = 0;
static uint64_t g_pid = 0;
static char g_root[4096];

#if defined(__SANITIZE_ADDRESS__)
#define TEST_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_UNDER_ASAN 1
#endif
#endif

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_CODE(resp, want)                                                        \
    do {                                                                              \
        g_checks++;                                                                   \
        if (resp_error_status(&(resp)) != (uint64_t)(want)) {                         \
            g_failures++;                                                             \
            fprintf(stderr, "%s:%d: expected %s, got text=%.*s\n", __FILE__, __LINE__, \
                    su_status_name(want), (int)(resp).len, (const char *)(resp).text); \
        }                                                                             \
    } while (0)

#define CHECK_OK(resp)                                                                \
    do {                                                                              \
        g_checks++;                                                                   \
        if (!resp_ok(&(resp))) {                                                      \
            g_failures++;                                                             \
            fprintf(stderr, "%s:%d: expected ok: %.*s\n", __FILE__, __LINE__,         \
                    (int)(resp).len, (const char *)(resp).text);                      \
        }                                                                             \
    } while (0)

#define REF_MAX_ROWS 16u
#define MAX_SYS 8u
#define HEX64 "0000000000000000000000000000000000000000000000000000000000000000"

/* ---------- session: supervisor in a forked child ---------- */

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

static Session session_start(const SuAgentConfig *cfg) {
    Session s;
    int req[2];
    int rsp[2];

    su_test_serve_start(&s.serve, cfg, &req[1], &rsp[0]);
    s.wfd = req[1];
    su_line_reader_init(&s.rd, rsp[0], -1);
    return s;
}

static void session_stop(Session *s) {
    close(s->wfd);
    su_line_reader_free(&s->rd);
    CHECK(su_test_serve_wait(&s->serve));
}

/* Sends one request line and reads exactly one response line. Returns 0 on success. */
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
    lim.max_depth = 32;
    st = su_json_parse(r->text, len, &lim, &r->doc, &jerr);
    r->root = st == SU_OK && r->doc.root != NULL ? r->doc.root : NULL;
    if (st != SU_OK) {
        su_json_doc_free(&r->doc);
    }
    return 0;
}

static void rpc(Session *s, const char *line, Resp *r) {
    if (call(s, line, r) != 0) {
        fprintf(stderr, "%s:%d: no response to %s\n", __FILE__, __LINE__, line);
        exit(2);
    }
}

/* ---------- JSON accessors ---------- */

static uint64_t jnum(const SuJsonValue *o, const char *k) {
    uint64_t v = 0;
    su_json_get_uint(su_json_member(o, k), &v);
    return v;
}

static int jstr(const SuJsonValue *o, const char *k, char *dst, size_t cap) {
    const SuJsonValue *v = su_json_member(o, k);
    dst[0] = '\0';
    if (v == NULL || v->type != SU_JSON_STRING) {
        return 0;
    }
    snprintf(dst, cap, "%.*s", (int)v->u.string.len, v->u.string.data);
    return 1;
}

static int u32_array(const SuJsonValue *arr, uint64_t n, uint32_t *dst) {
    uint64_t i;
    if (arr == NULL || arr->type != SU_JSON_ARRAY || arr->u.array.count != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint64_t v = 0;
        if (!su_json_get_uint(arr->u.array.items[i], &v) || v > UINT32_MAX) {
            return 0;
        }
        dst[i] = (uint32_t)v;
    }
    return 1;
}

static int resp_message_has(const Resp *r, const char *needle) {
    const SuJsonValue *e = r->root != NULL ? su_json_member(r->root, "error") : NULL;
    const SuJsonValue *m = e != NULL ? su_json_member(e, "message") : NULL;
    size_t n = strlen(needle);
    size_t i;
    if (m == NULL || m->type != SU_JSON_STRING || m->u.string.len < n) {
        return 0;
    }
    for (i = 0; i + n <= m->u.string.len; i++) {
        if (memcmp(m->u.string.data + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------- filesystem (names are relative to g_root) ---------- */

static void root_path(char *dst, size_t cap, const char *name) {
    snprintf(dst, cap, "%s/%s", g_root, name);
}

static uint8_t *read_whole(const char *path, size_t *len) {
    FILE *fh = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (fh == NULL) {
        perror(path);
        exit(2);
    }
    fseek(fh, 0, SEEK_END);
    n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    buf = malloc((size_t)n + 1u);
    if (buf == NULL || fread(buf, 1, (size_t)n, fh) != (size_t)n) {
        perror(path);
        exit(2);
    }
    buf[n] = '\0';
    fclose(fh);
    *len = (size_t)n;
    return buf;
}

static void write_bytes(const char *path, const char *data, size_t len) {
    FILE *fh = fopen(path, "wb");
    if (fh == NULL || (len > 0 && fwrite(data, 1, len, fh) != len)) {
        perror(path);
        exit(2);
    }
    fclose(fh);
}

/* Copies every .json file of repo/tests/reload into the scratch root. */
static void stage_fixtures(const char *repo) {
    char dir[4096];
    char src[8192];
    char dst[8192];
    SuTestDir d;
    const char *de_name;

    snprintf(dir, sizeof dir, "%s/tests/reload", repo);
    if (su_test_dir_open(&d, dir) != 0) {
        perror(dir);
        exit(2);
    }
    while ((de_name = su_test_dir_next(&d)) != NULL) {
        size_t n = strlen(de_name);
        size_t len = 0;
        uint8_t *data;
        if (n < 6 || strcmp(de_name + n - 5, ".json") != 0) {
            continue;
        }
        snprintf(src, sizeof src, "%s/%s", dir, de_name);
        root_path(dst, sizeof dst, de_name);
        data = read_whole(src, &len);
        write_bytes(dst, (const char *)data, len);
        free(data);
    }
    su_test_dir_close(&d);
}

/* ---------- independent reference model (world4: 4 rows) ---------- */

typedef struct Ref {
    uint64_t rows;
    uint32_t ids[REF_MAX_ROWS];
    uint32_t pos[REF_MAX_ROWS];
    uint32_t vel[REF_MAX_ROWS];
} Ref;

enum { A_V1 = 1, A_V2 = 2 };

/* One tick: system A (version a) then system B (velocity + 1). */
static void ref_tick(Ref *r, int a) {
    uint64_t i;
    for (i = 0; i < r->rows; i++) {
        if (a == A_V1) {
            r->pos[i] = r->pos[i] + r->vel[i];
        } else {
            r->pos[i] = r->pos[i] + 2u * r->vel[i];
        }
    }
    for (i = 0; i < r->rows; i++) {
        r->vel[i] = r->vel[i] + 1u;
    }
}

static void ref_init4(Ref *r) {
    static const uint32_t pos[4] = {0u, 1u, 4294967295u, 7u};
    static const uint32_t vel[4] = {1u, 2u, 1u, 0u};
    uint32_t i;
    memset(r, 0, sizeof *r);
    r->rows = 4;
    for (i = 0; i < 4; i++) {
        r->ids[i] = i + 1u;
        r->pos[i] = pos[i];
        r->vel[i] = vel[i];
    }
}

static int ref_equal(const Ref *a, const Ref *b) {
    size_t bytes;
    if (a->rows != b->rows) {
        return 0;
    }
    bytes = (size_t)a->rows * sizeof(uint32_t);
    return memcmp(a->ids, b->ids, bytes) == 0 && memcmp(a->pos, b->pos, bytes) == 0 &&
           memcmp(a->vel, b->vel, bytes) == 0;
}

/* ---------- world views ---------- */

typedef struct View {
    Ref cols;
    uint64_t tick;
    char rev[64];
    uint64_t pid;
    uint32_t n;
    char sys[MAX_SYS][64];
    char hash[MAX_SYS][65];
} View;

/* Committed state, tick, revision, PID and manifest of one world, via world.inspect. */
static int read_view(Session *s, const char *wid, View *v) {
    char line[512];
    Resp r;
    const SuJsonValue *res;
    const SuJsonValue *cols;
    const SuJsonValue *man;
    uint64_t rows;
    uint32_t i;
    int ok;

    memset(v, 0, sizeof *v);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"iv\",\"method\":\"world.inspect\",\"params\":"
             "{\"world_id\":\"%s\",\"limit\":16}}",
             wid);
    rpc(s, line, &r);
    if (!resp_ok(&r)) {
        resp_free(&r);
        return 0;
    }
    res = resp_result(&r);
    cols = su_json_member(res, "columns");
    rows = jnum(res, "row_count");
    ok = rows > 0 && rows <= REF_MAX_ROWS;
    if (ok) {
        v->cols.rows = rows;
        ok = u32_array(su_json_member(res, "entity_ids"), rows, v->cols.ids) &&
             u32_array(su_json_member(cols, "position_x"), rows, v->cols.pos) &&
             u32_array(su_json_member(cols, "velocity_x"), rows, v->cols.vel);
    }
    v->tick = jnum(res, "tick");
    v->pid = jnum(res, "worker_pid");
    jstr(res, "world_revision", v->rev, sizeof v->rev);
    man = su_json_member(res, "semantic_manifest");
    if (man != NULL && man->type == SU_JSON_ARRAY && man->u.array.count <= MAX_SYS) {
        v->n = (uint32_t)man->u.array.count;
        for (i = 0; i < v->n; i++) {
            jstr(man->u.array.items[i], "system_id", v->sys[i], sizeof v->sys[i]);
            jstr(man->u.array.items[i], "semantic_hash", v->hash[i], sizeof v->hash[i]);
        }
    } else {
        ok = 0;
    }
    resp_free(&r);
    return ok;
}

/* Two views describe the same committed state, tick and manifest (revision excluded). */
static int same_state(const View *a, const View *b) {
    uint32_t i;
    if (!ref_equal(&a->cols, &b->cols) || a->tick != b->tick || a->n != b->n) {
        return 0;
    }
    for (i = 0; i < a->n; i++) {
        if (strcmp(a->sys[i], b->sys[i]) != 0 || strcmp(a->hash[i], b->hash[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---------- request helpers ---------- */

static void submit(Session *s, const char *cap, char *cid, size_t cidn, char *sem, size_t semn) {
    char line[512];
    Resp r;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"sub\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"%s\"}}",
             cap);
    rpc(s, line, &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "candidate_id", cid, cidn);
    jstr(resp_result(&r), "semantic_hash", sem, semn);
    resp_free(&r);
}

static void evaluate_ok(Session *s, const char *cid, const char *test) {
    char line[512];
    Resp r;
    char status[16];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ev\",\"method\":\"candidate.evaluate\",\"params\":"
             "{\"candidate_id\":\"%s\",\"test_path\":\"%s\"}}",
             cid, test);
    rpc(s, line, &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "status", status, sizeof status);
    CHECK(strcmp(status, "passed") == 0);
    resp_free(&r);
}

static void install(Session *s, const char *cid, const char *wid, const char *expect, uint64_t tick, Resp *r) {
    char line[1024];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ins\",\"method\":\"candidate.install\",\"params\":"
             "{\"candidate_id\":\"%s\",\"world_id\":\"%s\",\"expected_world_revision\":\"%s\","
             "\"allowed_effective_tick\":%llu}}",
             cid, wid, expect, (unsigned long long)tick);
    rpc(s, line, r);
}

static void step(Session *s, const char *wid, const char *expect, uint64_t ticks, Resp *r) {
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"stp\",\"method\":\"world.step\",\"params\":"
             "{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"ticks\":%llu}}",
             wid, expect, (unsigned long long)ticks);
    rpc(s, line, r);
}

static void create_two(Session *s, const char *fixture, const char *a, const char *b, Resp *r) {
    char line[768];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wc\",\"method\":\"world.create\",\"params\":"
             "{\"fixture_path\":\"%s\",\"capsule_paths\":[\"%s\",\"%s\"]}}",
             fixture, a, b);
    rpc(s, line, r);
}

/* world.snapshot; the snapshot id is copied to id (65 bytes) when the call succeeds. */
static void snapshot(Session *s, const char *wid, const char *expect, Resp *r) {
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"snp\",\"method\":\"world.snapshot\",\"params\":"
             "{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\"}}",
             wid, expect);
    rpc(s, line, r);
}

static void restore(Session *s, const char *wid, const char *snap, const char *expect, Resp *r) {
    char line[768];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"rst\",\"method\":\"world.restore\",\"params\":"
             "{\"world_id\":\"%s\",\"snapshot_id\":\"%s\",\"expected_world_revision\":\"%s\"}}",
             wid, snap, expect);
    rpc(s, line, r);
}

/* Reads the world revision string out of a successful result. */
static void result_rev(const Resp *r, char *rev, size_t cap) {
    jstr(resp_result(r), "world_revision", rev, cap);
}

static uint64_t active_pid(Session *s) {
    Resp r;
    uint64_t pid = 0;
    rpc(s, "{\"protocol\":\"0.1\",\"id\":\"ei\",\"method\":\"engine.info\",\"params\":{}}", &r);
    CHECK_OK(r);
    if (resp_ok(&r)) {
        pid = jnum(su_json_member(resp_result(&r), "active_worker"), "pid");
    }
    resp_free(&r);
    return pid;
}

/* ---------- test sequence ---------- */

typedef struct Ids {
    char cand_a[32], sem_a[65];
    char cand_b[32], sem_b[65];
    char cand_v2[32], sem_v2[65];
    char world0[32], world1[32], world2[32];
} Ids;

static void test_engine_info(Session *s) {
    Resp r;
    const SuJsonValue *caps;
    uint32_t i;
    int has_snapshot = 0;
    int has_restore = 0;
    int has_install = 0;

    rpc(s, "{\"protocol\":\"0.1\",\"id\":\"ei\",\"method\":\"engine.info\",\"params\":{}}", &r);
    CHECK_OK(r);
    caps = resp_result(&r) != NULL ? su_json_member(resp_result(&r), "capabilities") : NULL;
    for (i = 0; caps != NULL && caps->type == SU_JSON_ARRAY && i < caps->u.array.count; i++) {
        const SuJsonValue *c = caps->u.array.items[i];
        if (c->type != SU_JSON_STRING) {
            continue;
        }
        if (c->u.string.len == 14 && memcmp(c->u.string.data, "world.snapshot", 14) == 0) {
            has_snapshot = 1;
        }
        if (c->u.string.len == 13 && memcmp(c->u.string.data, "world.restore", 13) == 0) {
            has_restore = 1;
        }
        if (c->u.string.len == 17 && memcmp(c->u.string.data, "candidate.install", 17) == 0) {
            has_install = 1;
        }
    }
    CHECK(has_snapshot);
    CHECK(has_restore);
    CHECK(has_install);
    CHECK(jnum(su_json_member(resp_result(&r), "active_worker"), "pid") == 0);
    resp_free(&r);
}

/* T08: round trip through the control plane, then continued stepping. */
static void test_round_trip(Session *s, Ids *id) {
    Resp r;
    Ref e3;
    Ref e6;
    View v;
    View pre;
    char rev[64];
    char rev_snap[64];
    char snap[65];
    char sha[65];
    char man_hash_a[65];
    char man_hash_b[65];
    char rev_restored[64];
    uint64_t bytes = 0;

    create_two(s, "world4.json", "sys_a.capsule.json", "sys_b.capsule.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "world_id", id->world0, sizeof id->world0);
    g_pid = jnum(resp_result(&r), "worker_pid");
    CHECK(g_pid > 0);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);
    CHECK(read_view(s, id->world0, &pre));
    CHECK(pre.n == 2);
    CHECK(strcmp(pre.sys[0], "demo.movement") == 0);
    snprintf(id->sem_a, sizeof id->sem_a, "%s", pre.hash[0]);
    snprintf(id->sem_b, sizeof id->sem_b, "%s", pre.hash[1]);
    CHECK(strcmp(id->sem_a, id->sem_b) != 0);

    /* Three ticks, then the snapshot at tick 3. */
    ref_init4(&e3);
    step(s, id->world0, rev, 3, &r);
    CHECK_OK(r);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);
    ref_tick(&e3, A_V1);
    ref_tick(&e3, A_V1);
    ref_tick(&e3, A_V1);
    CHECK(read_view(s, id->world0, &v));
    CHECK(ref_equal(&v.cols, &e3));
    CHECK(v.tick == 3);
    CHECK(v.pid == g_pid);

    snapshot(s, id->world0, rev, &r);
    CHECK_OK(r);
    if (resp_ok(&r)) {
        const SuJsonValue *res = resp_result(&r);
        jstr(res, "snapshot_id", snap, sizeof snap);
        jstr(res, "sha256", sha, sizeof sha);
        jstr(res, "world_revision", rev_snap, sizeof rev_snap);
        bytes = jnum(res, "bytes");
        CHECK(strlen(snap) == 64);
        CHECK(strcmp(snap, sha) == 0);
        CHECK(jnum(res, "tick") == 3);
        CHECK(bytes > 0);
        CHECK(strcmp(rev_snap, rev) == 0);
        CHECK(su_json_member(res, "capture_ns") != NULL);
    }
    resp_free(&r);

    /* Keep going to tick 6; this is the uninterrupted reference. */
    step(s, id->world0, rev, 3, &r);
    CHECK_OK(r);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);
    e6 = e3;
    ref_tick(&e6, A_V1);
    ref_tick(&e6, A_V1);
    ref_tick(&e6, A_V1);
    CHECK(read_view(s, id->world0, &v));
    CHECK(ref_equal(&v.cols, &e6));
    CHECK(v.tick == 6);

    /* Restore tick 3: a NEW revision, restored_from, the same worker, the same manifest. */
    restore(s, id->world0, snap, rev, &r);
    CHECK_OK(r);
    if (resp_ok(&r)) {
        const SuJsonValue *res = resp_result(&r);
        char from[65];
        jstr(res, "restored_from", from, sizeof from);
        CHECK(strcmp(from, snap) == 0);
        CHECK(jnum(res, "tick") == 3);
        CHECK(jnum(res, "worker_pid") == g_pid);
        result_rev(&r, rev_restored, sizeof rev_restored);
        CHECK(strcmp(rev_restored, rev) != 0);
    }
    resp_free(&r);
    CHECK(read_view(s, id->world0, &v));
    CHECK(ref_equal(&v.cols, &e3));
    CHECK(v.tick == 3);
    CHECK(v.pid == g_pid);
    CHECK(strcmp(v.rev, rev_restored) == 0);
    CHECK(v.n == 2);
    snprintf(man_hash_a, sizeof man_hash_a, "%s", v.hash[0]);
    snprintf(man_hash_b, sizeof man_hash_b, "%s", v.hash[1]);
    CHECK(strcmp(man_hash_a, id->sem_a) == 0);
    CHECK(strcmp(man_hash_b, id->sem_b) == 0);

    /* Continued stepping equals the uninterrupted run. */
    step(s, id->world0, rev_restored, 3, &r);
    CHECK_OK(r);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);
    CHECK(read_view(s, id->world0, &v));
    CHECK(ref_equal(&v.cols, &e6));
    CHECK(v.tick == 6);
    CHECK(v.pid == g_pid);
    CHECK(strcmp(v.rev, rev) == 0);
    CHECK(bytes > 0);
}

/* Stale revisions, unknown and evicted snapshots. None of them changes the world. */
static void test_stale_and_unknown(Session *s, Ids *id) {
    Resp r;
    View before;
    View after;
    char rev[64];
    char first_snap[65];
    char last_snap[65];
    char old_rev[64];
    int i;

    CHECK(read_view(s, id->world0, &before));
    snprintf(rev, sizeof rev, "%s", before.rev);
    snprintf(old_rev, sizeof old_rev, "%s.r1", id->world0);

    snapshot(s, id->world0, old_rev, &r);
    CHECK_CODE(r, SU_E_STALE_REVISION);
    resp_free(&r);

    restore(s, id->world0, HEX64, old_rev, &r);
    CHECK_CODE(r, SU_E_STALE_REVISION);
    resp_free(&r);

    restore(s, id->world0, HEX64, rev, &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    CHECK(resp_message_has(&r, "unknown or evicted"));
    resp_free(&r);

    restore(s, id->world0, "not-a-snapshot", rev, &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);

    CHECK(read_view(s, id->world0, &after));
    CHECK(same_state(&before, &after));
    CHECK(strcmp(after.rev, before.rev) == 0);

    /* Eviction: the store retains 8. The snapshot taken at tick 3 is the oldest once eight
     * more are taken, each after its own tick, so it must no longer restore. */
    snapshot(s, id->world0, rev, &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "snapshot_id", first_snap, sizeof first_snap);
    resp_free(&r);
    snprintf(rev, sizeof rev, "%s", before.rev);
    for (i = 0; i < 8; i++) {
        char nrev[64];
        step(s, id->world0, rev, 1, &r);
        CHECK_OK(r);
        result_rev(&r, nrev, sizeof nrev);
        resp_free(&r);
        snprintf(rev, sizeof rev, "%s", nrev);
        snapshot(s, id->world0, rev, &r);
        CHECK_OK(r);
        jstr(resp_result(&r), "snapshot_id", last_snap, sizeof last_snap);
        resp_free(&r);
    }
    restore(s, id->world0, first_snap, rev, &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    CHECK(resp_message_has(&r, "evicted"));
    resp_free(&r);

    /* The newest snapshot is still retained and restores. */
    restore(s, id->world0, last_snap, rev, &r);
    CHECK_OK(r);
    resp_free(&r);
}

/* Restore across an installed code swap: the old manifest and behaviour come back. */
static void test_swap_restore(Session *s, Ids *id) {
    Resp r;
    Ref e1;
    View v;
    char rev[64];
    char snap[65];
    char rev_inst[64];
    char rev_restored[64];

    create_two(s, "world4.json", "sys_a.capsule.json", "sys_b.capsule.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "world_id", id->world1, sizeof id->world1);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);

    ref_init4(&e1);
    step(s, id->world1, rev, 1, &r);
    CHECK_OK(r);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);
    ref_tick(&e1, A_V1);

    snapshot(s, id->world1, rev, &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "snapshot_id", snap, sizeof snap);
    resp_free(&r);

    install(s, id->cand_v2, id->world1, rev, 1, &r);
    CHECK_OK(r);
    result_rev(&r, rev_inst, sizeof rev_inst);
    resp_free(&r);

    step(s, id->world1, rev_inst, 1, &r);
    CHECK_OK(r);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);
    {
        Ref v2 = e1;
        ref_tick(&v2, A_V2);
        CHECK(read_view(s, id->world1, &v));
        CHECK(ref_equal(&v.cols, &v2));
        CHECK(strcmp(v.hash[0], id->sem_v2) == 0);
    }

    restore(s, id->world1, snap, rev, &r);
    CHECK_OK(r);
    result_rev(&r, rev_restored, sizeof rev_restored);
    resp_free(&r);
    CHECK(read_view(s, id->world1, &v));
    CHECK(ref_equal(&v.cols, &e1));
    CHECK(v.tick == 1);
    CHECK(v.pid == g_pid);
    CHECK(v.n == 2);
    CHECK(strcmp(v.hash[0], id->sem_a) == 0);
    CHECK(strcmp(v.hash[1], id->sem_b) == 0);
    CHECK(strcmp(v.rev, rev_restored) == 0);

    /* The restored code runs: one more tick is v1 behaviour, not v2. */
    step(s, id->world1, rev_restored, 1, &r);
    CHECK_OK(r);
    resp_free(&r);
    ref_tick(&e1, A_V1);
    CHECK(read_view(s, id->world1, &v));
    CHECK(ref_equal(&v.cols, &e1));
    CHECK(v.tick == 2);
}

/* A failed restore leaves the world bit-identical, and a snapshot of another world is refused. */
static void test_failed_restore(Session *s, Ids *id, const char *lir_path) {
    Resp r;
    Ref e2;
    View before;
    View after;
    char rev[64];
    char snap[65];
    char sha_before[65];
    char sha_after[65];
    char path[8192];
    size_t len = 0;
    uint8_t *data;
    FILE *fh;

    create_two(s, "world4.json", "sys_a.capsule.json", "sys_b.capsule.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "world_id", id->world2, sizeof id->world2);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);

    ref_init4(&e2);
    step(s, id->world2, rev, 1, &r);
    CHECK_OK(r);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);
    ref_tick(&e2, A_V1);
    snapshot(s, id->world2, rev, &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "snapshot_id", snap, sizeof snap);
    resp_free(&r);

    step(s, id->world2, rev, 1, &r);
    CHECK_OK(r);
    result_rev(&r, rev, sizeof rev);
    resp_free(&r);
    ref_tick(&e2, A_V1);

    /* The snapshot belongs to world2; world0 must not restore it (with world0's own
     * current revision, so the refusal is about the snapshot). */
    CHECK(read_view(s, id->world0, &before));
    restore(s, id->world0, snap, before.rev, &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);

    /* Change the bytes of sys_a's source after the checkpoint: its revision can no longer be
     * reloaded with the recorded hashes, so restore must refuse and change nothing. */
    root_path(path, sizeof path, lir_path);
    data = read_whole(path, &len);
    fh = fopen(path, "wb");
    if (fh == NULL || fwrite(data, 1, len, fh) != len || fwrite("\n", 1, 1, fh) != 1) {
        perror(path);
        exit(2);
    }
    fclose(fh);
    free(data);

    snapshot(s, id->world2, rev, &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "sha256", sha_before, sizeof sha_before);
    resp_free(&r);
    CHECK(read_view(s, id->world2, &before));

    restore(s, id->world2, snap, rev, &r);
    CHECK_CODE(r, SU_E_SCHEMA_MISMATCH);
    CHECK(resp_message_has(&r, "cannot be reloaded"));
    resp_free(&r);

    snapshot(s, id->world2, rev, &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "sha256", sha_after, sizeof sha_after);
    resp_free(&r);
    CHECK(strcmp(sha_before, sha_after) == 0);
    CHECK(read_view(s, id->world2, &after));
    CHECK(same_state(&before, &after));
    CHECK(strcmp(after.rev, rev) == 0);
    CHECK(ref_equal(&after.cols, &e2));
    CHECK(after.tick == 2);

    /* The world still steps from the state it had. */
    step(s, id->world2, rev, 1, &r);
    CHECK_OK(r);
    resp_free(&r);
    ref_tick(&e2, A_V1);
    CHECK(read_view(s, id->world2, &after));
    CHECK(ref_equal(&after.cols, &e2));
}

static void remove_root(void) {
    SuTestDir d;
    const char *de_name;
    if (su_test_dir_open(&d, g_root) != 0) {
        return;
    }
    while ((de_name = su_test_dir_next(&d)) != NULL) {
        char p[8192];
        if (strcmp(de_name, ".") == 0 || strcmp(de_name, "..") == 0) {
            continue;
        }
        root_path(p, sizeof p, de_name);
        unlink(p);
    }
    su_test_dir_close(&d);
    rmdir(g_root);
}

int main(int argc, char **argv) {
    static const char *helper_argv[2];
    char base[4000];
    SuAgentConfig cfg;
    Session s;
    Ids ids;
    const char *tmp;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <worker_helper_path> <repo_root>\n", argv[0]);
        return 2;
    }
    tmp = su_test_scratch_base(); /* SU_TEST_SCRATCH, then TMPDIR, then the platform temporary directory */
    snprintf(base, sizeof base, "%s/su_snapshot_cp_XXXXXX", tmp);
    if (su_platform_mkdtemp(base) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(g_root, sizeof g_root, "%s", base);
    stage_fixtures(argv[2]);

    helper_argv[0] = argv[1];
    helper_argv[1] = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.project_root = g_root;
    cfg.worker_argv = helper_argv;
    cfg.request_timeout_ms = 10000;
#if defined(TEST_UNDER_ASAN)
    cfg.worker_address_space_bytes = 0;
#else
    cfg.worker_address_space_bytes = (uint64_t)4u * 1024u * 1024u * 1024u;
#endif

    memset(&ids, 0, sizeof ids);
    s = session_start(&cfg);
    test_engine_info(&s);
    CHECK(active_pid(&s) == 0);
    submit(&s, "sys_a.capsule.json", ids.cand_a, sizeof ids.cand_a, ids.sem_a, sizeof ids.sem_a);
    submit(&s, "sys_b.capsule.json", ids.cand_b, sizeof ids.cand_b, ids.sem_b, sizeof ids.sem_b);
    submit(&s, "sys_a_v2.capsule.json", ids.cand_v2, sizeof ids.cand_v2, ids.sem_v2, sizeof ids.sem_v2);
    evaluate_ok(&s, ids.cand_v2, "sys_a_v2.test.json");

    test_round_trip(&s, &ids);
    test_stale_and_unknown(&s, &ids);
    test_swap_restore(&s, &ids);
    test_failed_restore(&s, &ids, "sys_a.lir.json");
    CHECK(active_pid(&s) == g_pid);
    session_stop(&s);

    remove_root();
    printf("test_snapshot_cp: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
