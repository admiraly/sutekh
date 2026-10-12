/* M0-07 reload tests: version registry, prepared schedule, safe-point installation.
 *
 * Covers T05 (code-only swap at a recorded tick; PID and world state preserved), T06
 * (invalid, tampered and failed candidates leave the active semantic hash and committed
 * state unchanged), T07 (stale revision, stale effective tick, layout generation change),
 * T09 (a failing second system leaves the published tick root unchanged), T11 (1,000
 * successful, failed and reverted installs with bounded registry retention) and paging of
 * a 1,024-row world (TEST_SPEC T02: every row readable through world.inspect).
 *
 * Expected states come from a C reference model in this file, and the fixture values
 * come from make_fixtures.py (an independent Python model), never from the VM.
 *
 * Usage: test_reload <worker_helper_path> <repo_root>
 *   repo_root  absolute path of the repository. Its tests/reload JSON files and
 *              tests/fixtures/t02/world_1024.json are copied into a scratch project root.
 * Environment:
 *   SU_TEST_SCRATCH  base directory for the scratch project root. Defaults to $TMPDIR,
 *                    then /tmp. A fresh su_reload_* subdirectory is created inside it and
 *                    removed at the end. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agent/su_agent.h"
#include "agent/su_proto.h"
#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "runtime/reload.h"
#include "sutekh/status.h"

static int g_checks = 0;
static int g_failures = 0;
static uint64_t g_pid = 0; /* active worker PID; must not change across installs */
static char g_root[4096];  /* scratch project root (absolute) */

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

#define REF_MAX_ROWS 1024u
#define PAGE 256u
#define MAX_MANIFEST 8u

/* ---------- session: supervisor in a forked child, as in test_control ---------- */

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

/* True when the error message contains needle. */
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

static Session session_start(const SuAgentConfig *cfg) {
    Session s;
    int req[2];
    int rsp[2];
    pid_t pid;

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
        rc = su_agent_serve(req[0], rsp[1], cfg);
        _exit(rc == 0 ? 0 : 1);
    }
    close(req[0]);
    close(rsp[1]);
    s.pid = pid;
    s.wfd = req[1];
    su_line_reader_init(&s.rd, rsp[0], -1);
    return s;
}

static void session_stop(Session *s) {
    int status = 0;
    close(s->wfd);
    su_line_reader_free(&s->rd);
    waitpid(s->pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
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

/* Copies a JSON array of n u32 values into dst. Returns 0 on any mismatch. */
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

/* ---------- filesystem (all names are relative to g_root) ---------- */

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

/* Copies one scratch file to another, replacing every `from` by `to` (from may be NULL). */
static void cp(const char *src_name, const char *dst_name, const char *from, const char *to) {
    char src[8192];
    char dst[8192];
    size_t len = 0;
    uint8_t *data;
    root_path(src, sizeof src, src_name);
    root_path(dst, sizeof dst, dst_name);
    data = read_whole(src, &len);
    if (from == NULL) {
        write_bytes(dst, (const char *)data, len);
    } else {
        size_t flen = strlen(from);
        size_t tlen = strlen(to);
        char *out = malloc(len * 4u + 64u);
        size_t o = 0;
        size_t i = 0;
        if (out == NULL) {
            exit(2);
        }
        while (i < len) {
            if (i + flen <= len && memcmp(data + i, from, flen) == 0) {
                memcpy(out + o, to, tlen);
                o += tlen;
                i += flen;
            } else {
                out[o++] = (char)data[i++];
            }
        }
        write_bytes(dst, out, o);
        free(out);
    }
    free(data);
}

/* Copies every .json file of repo/tests/reload, and the 1,024-row fixture, into the root. */
static void stage_fixtures(const char *repo) {
    char dir[4096];
    char src[8192];
    char dst[8192];
    DIR *d;
    struct dirent *de;

    snprintf(dir, sizeof dir, "%s/tests/reload", repo);
    d = opendir(dir);
    if (d == NULL) {
        perror(dir);
        exit(2);
    }
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        size_t len = 0;
        uint8_t *data;
        if (n < 6 || strcmp(de->d_name + n - 5, ".json") != 0) {
            continue;
        }
        snprintf(src, sizeof src, "%s/%s", dir, de->d_name);
        root_path(dst, sizeof dst, de->d_name);
        data = read_whole(src, &len);
        write_bytes(dst, (const char *)data, len);
        free(data);
    }
    closedir(d);
    snprintf(src, sizeof src, "%s/tests/fixtures/t02/world_1024.json", repo);
    root_path(dst, sizeof dst, "world_1024.json");
    {
        size_t len = 0;
        uint8_t *data = read_whole(src, &len);
        write_bytes(dst, (const char *)data, len);
        free(data);
    }
}

/* ---------- independent reference model ---------- */

typedef struct Ref {
    uint64_t rows;
    uint32_t ids[REF_MAX_ROWS];
    uint32_t pos[REF_MAX_ROWS];
    uint32_t vel[REF_MAX_ROWS];
} Ref;

enum { A_NONE = 0, A_V1 = 1, A_V2 = 2, A_BAD = 3 };

/* One tick: system A (position update, version a) then, if b, system B (velocity + 1). */
static void ref_tick(Ref *r, int a, int b) {
    uint64_t i;
    for (i = 0; i < r->rows; i++) {
        if (a == A_V1) {
            r->pos[i] = r->pos[i] + r->vel[i];
        } else if (a == A_V2) {
            r->pos[i] = r->pos[i] + 2u * r->vel[i];
        } else if (a == A_BAD) {
            r->pos[i] = r->pos[i] - r->vel[i];
        }
    }
    if (b) {
        for (i = 0; i < r->rows; i++) {
            r->vel[i] = r->vel[i] + 1u;
        }
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

/* Loads the 1,024-row fixture (tick 0) from the scratch root. */
static void ref_load_fixture(const char *name, Ref *r) {
    char path[8192];
    size_t len = 0;
    uint8_t *data;
    SuJsonLimits lim;
    SuJsonDoc doc;
    SuJsonError err;
    const SuJsonValue *root;
    const SuJsonValue *ids;
    uint64_t n;

    root_path(path, sizeof path, name);
    data = read_whole(path, &len);
    lim.max_bytes = len;
    lim.max_depth = 16;
    memset(&doc, 0, sizeof doc);
    if (su_json_parse(data, len, &lim, &doc, &err) != SU_OK) {
        fprintf(stderr, "fixture %s does not parse: %s\n", name, err.message);
        exit(2);
    }
    root = doc.root;
    ids = su_json_member(root, "entity_ids");
    n = ids != NULL && ids->type == SU_JSON_ARRAY ? ids->u.array.count : 0;
    if (n == 0 || n > REF_MAX_ROWS) {
        fprintf(stderr, "fixture %s has %llu rows\n", name, (unsigned long long)n);
        exit(2);
    }
    memset(r, 0, sizeof *r);
    r->rows = n;
    if (!u32_array(ids, n, r->ids) ||
        !u32_array(su_json_member(su_json_member(root, "columns"), "position_x"), n, r->pos) ||
        !u32_array(su_json_member(su_json_member(root, "columns"), "velocity_x"), n, r->vel)) {
        fprintf(stderr, "fixture %s is malformed\n", name);
        exit(2);
    }
    su_json_doc_free(&doc);
    free(data);
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

/* Committed state of one world, read through paged world.inspect calls. */
typedef struct View {
    Ref cols;
    uint64_t tick;
    char rev[64];
    uint64_t pid;
} View;

typedef struct Manifest {
    uint32_t n;
    char sys[MAX_MANIFEST][128];
    char hash[MAX_MANIFEST][65];
    uint64_t txid[MAX_MANIFEST];
} Manifest;

static int view_equal(const View *a, const View *b) {
    return ref_equal(&a->cols, &b->cols) && a->tick == b->tick && strcmp(a->rev, b->rev) == 0 &&
           a->pid == b->pid;
}

/* Reads all rows of a world, PAGE rows per call. Returns 0 on any failed page. */
static int read_view(Session *s, const char *wid, uint64_t rows, View *v) {
    uint64_t off;
    memset(v, 0, sizeof *v);
    v->cols.rows = rows;
    for (off = 0; off < rows; off += PAGE) {
        char line[512];
        Resp r;
        uint64_t n = rows - off < PAGE ? rows - off : PAGE;
        const SuJsonValue *res;
        const SuJsonValue *cols;
        int good;
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"v\",\"method\":\"world.inspect\",\"params\":"
                 "{\"world_id\":\"%s\",\"fields\":[\"position_x\",\"velocity_x\"],\"limit\":%u,\"offset\":%llu}}",
                 wid, (unsigned)PAGE, (unsigned long long)off);
        rpc(s, line, &r);
        CHECK_OK(r);
        if (!resp_ok(&r)) {
            resp_free(&r);
            return 0;
        }
        res = resp_result(&r);
        cols = su_json_member(res, "columns");
        CHECK(jnum(res, "offset") == off);
        CHECK(jnum(res, "returned_rows") == n);
        CHECK(jnum(res, "row_count") == rows);
        good = u32_array(su_json_member(res, "entity_ids"), n, &v->cols.ids[off]) &&
               u32_array(su_json_member(cols, "position_x"), n, &v->cols.pos[off]) &&
               u32_array(su_json_member(cols, "velocity_x"), n, &v->cols.vel[off]);
        CHECK(good);
        if (off == 0) {
            jstr(res, "world_revision", v->rev, sizeof v->rev);
            v->tick = jnum(res, "tick");
            v->pid = jnum(res, "worker_pid");
        }
        resp_free(&r);
        if (!good) {
            return 0;
        }
    }
    return 1;
}

static void read_manifest(Session *s, const char *wid, Manifest *m) {
    char line[256];
    Resp r;
    const SuJsonValue *arr;
    uint32_t i;
    memset(m, 0, sizeof *m);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"m\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\",\"limit\":1}}",
             wid);
    rpc(s, line, &r);
    CHECK_OK(r);
    arr = resp_result(&r) != NULL ? su_json_member(resp_result(&r), "semantic_manifest") : NULL;
    if (arr != NULL && arr->type == SU_JSON_ARRAY) {
        m->n = (uint32_t)arr->u.array.count;
        for (i = 0; i < m->n && i < MAX_MANIFEST; i++) {
            const SuJsonValue *e = arr->u.array.items[i];
            jstr(e, "system_id", m->sys[i], sizeof m->sys[i]);
            jstr(e, "semantic_hash", m->hash[i], sizeof m->hash[i]);
            m->txid[i] = jnum(e, "txid");
        }
    }
    resp_free(&r);
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

static void evaluate(Session *s, const char *cid, const char *test, Resp *r) {
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"ev\",\"method\":\"candidate.evaluate\",\"params\":"
             "{\"candidate_id\":\"%s\",\"test_path\":\"%s\"}}",
             cid, test);
    rpc(s, line, r);
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

static void create_one(Session *s, const char *fixture, const char *cap, Resp *r) {
    char line[512];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wc\",\"method\":\"world.create\",\"params\":"
             "{\"fixture_path\":\"%s\",\"capsule_path\":\"%s\"}}",
             fixture, cap);
    rpc(s, line, r);
}

static void create_many(Session *s, const char *fixture, const char *a, const char *b, Resp *r) {
    char line[768];
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"wc\",\"method\":\"world.create\",\"params\":"
             "{\"fixture_path\":\"%s\",\"capsule_paths\":[\"%s\",\"%s\"]}}",
             fixture, a, b);
    rpc(s, line, r);
}

/* Entry i of the semantic_manifest of a world.create response, or NULL. */
static const SuJsonValue *manifest_item(const Resp *r, uint32_t i) {
    const SuJsonValue *man = resp_result(r) != NULL ? su_json_member(resp_result(r), "semantic_manifest") : NULL;
    if (man == NULL || man->type != SU_JSON_ARRAY || i >= man->u.array.count) {
        return NULL;
    }
    return man->u.array.items[i];
}

/* The declared_system_rev of manifest entry i, or 0 when absent. */
static uint64_t manifest_declared_rev(const Resp *r, uint32_t i) {
    const SuJsonValue *item = manifest_item(r, i);
    return item != NULL ? jnum(item, "declared_system_rev") : 0;
}

/* error.detail of an error response, or NULL. */
static const SuJsonValue *resp_detail(const Resp *r) {
    const SuJsonValue *e = r->root != NULL ? su_json_member(r->root, "error") : NULL;
    return e != NULL ? su_json_member(e, "detail") : NULL;
}

/* Audit item 6: the detail of a verification error names the source file, its SHA-256 (over the bytes on
 * disk, computed here independently) and the JSON pointer; expected, actual and hint are present. */
static void check_verify_detail(const Resp *r, const char *lir_name, const char *source_path) {
    const SuJsonValue *d = resp_detail(r);
    char got[80];
    char name[128];
    char path[8192];
    char hex[65];
    uint8_t *bytes;
    size_t n = 0;
    SuSha256 ctx;
    uint8_t dg[32];

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    CHECK(jstr(d, "json_pointer", name, sizeof name));
    CHECK(jstr(d, "source_path", name, sizeof name) && strcmp(name, source_path) == 0);
    CHECK(jstr(d, "source_sha256", got, sizeof got) && strlen(got) == 64);
    root_path(path, sizeof path, lir_name);
    bytes = read_whole(path, &n);
    CHECK(bytes != NULL);
    if (bytes != NULL) {
        su_sha256_init(&ctx);
        su_sha256_update(&ctx, bytes, n);
        su_sha256_final(&ctx, dg);
        su_sha256_hex(dg, hex);
        CHECK(strcmp(got, hex) == 0);
        free(bytes);
    }
    CHECK(jstr(d, "capsule_sha256", got, sizeof got) && strlen(got) == 64);
    CHECK(jstr(d, "hint", got, sizeof got) && got[0] != '\0');
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
    char cand_a[32], sem_a[65];   /* sys_a (v1) */
    char cand_v2[32], sem_v2[65]; /* sys_a_v2 */
    char cand_bad[32], sem_bad[65]; /* sys_a_bad: evaluation fails */
    char cand_g2[32], sem_g2[65]; /* sys_a_gen2: layout generation 2 */
    char cand_b[32], sem_b[65];   /* sys_b */
    char cand_mut[32], sem_mut[65];
    char world0[32], world1[32], world2[32], world3[32];
} Ids;

static void test_engine_info(Session *s) {
    Resp r;
    const SuJsonValue *caps;
    uint32_t i;
    int has_install = 0;
    int has_snapshot = 0;

    rpc(s, "{\"protocol\":\"0.1\",\"id\":\"ei\",\"method\":\"engine.info\",\"params\":{}}", &r);
    CHECK_OK(r);
    caps = resp_result(&r) != NULL ? su_json_member(resp_result(&r), "capabilities") : NULL;
    for (i = 0; caps != NULL && caps->type == SU_JSON_ARRAY && i < caps->u.array.count; i++) {
        const SuJsonValue *c = caps->u.array.items[i];
        if (c->type != SU_JSON_STRING) {
            continue;
        }
        if (c->u.string.len == 17 && memcmp(c->u.string.data, "candidate.install", 17) == 0) {
            has_install = 1;
        }
        if (c->u.string.len == 14 && memcmp(c->u.string.data, "world.snapshot", 14) == 0) {
            has_snapshot = 1;
        }
    }
    CHECK(has_install);
    CHECK(has_snapshot); /* world.snapshot is implemented since M0-08b */
    CHECK(jnum(su_json_member(resp_result(&r), "active_worker"), "pid") == 0);
    resp_free(&r);

    rpc(s, "{\"protocol\":\"0.1\",\"id\":\"sn\",\"method\":\"world.teleport\",\"params\":{}}", &r);
    CHECK_CODE(r, SU_E_UNSUPPORTED_METHOD);
    resp_free(&r);
}

static void test_submit_and_evaluate(Session *s, Ids *id) {
    Resp r;
    char status[16];
    char line[512];

    submit(s, "sys_a.capsule.json", id->cand_a, sizeof id->cand_a, id->sem_a, sizeof id->sem_a);
    submit(s, "sys_a_v2.capsule.json", id->cand_v2, sizeof id->cand_v2, id->sem_v2, sizeof id->sem_v2);
    submit(s, "sys_a_bad.capsule.json", id->cand_bad, sizeof id->cand_bad, id->sem_bad, sizeof id->sem_bad);
    submit(s, "sys_a_gen2.capsule.json", id->cand_g2, sizeof id->cand_g2, id->sem_g2, sizeof id->sem_g2);
    submit(s, "sys_b.capsule.json", id->cand_b, sizeof id->cand_b, id->sem_b, sizeof id->sem_b);
    CHECK(strcmp(id->sem_a, id->sem_v2) != 0);
    CHECK(strcmp(id->sem_b, id->sem_a) != 0);
    /* The semantic hash excludes layout_generation: gen2 has sys_a's program. */
    CHECK(strcmp(id->sem_g2, id->sem_a) == 0);

    /* An invalid source (unknown opcode) is rejected at submission. */
    cp("sys_a.lir.json", "broken.lir.json", "load_u32", "frobnicate_u32");
    cp("sys_a.capsule.json", "broken.capsule.json", "sys_a.lir.json", "broken.lir.json");
    rpc(s,
        "{\"protocol\":\"0.1\",\"id\":\"bk\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"broken.capsule.json\"}}",
        &r);
    CHECK_CODE(r, SU_E_UNKNOWN_OPCODE);
    check_verify_detail(&r, "broken.lir.json", "broken.lir.json");
    resp_free(&r);

    /* An unknown capability is refused at submission (audit item 4), with the expected and actual values. */
    rpc(s,
        "{\"protocol\":\"0.1\",\"id\":\"bc\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"sys_a_badcap.capsule.json\"}}",
        &r);
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    check_verify_detail(&r, "sys_a_badcap.lir.json", "sys_a_badcap.lir.json");
    {
        char ex[96];
        char ac[96];
        CHECK(resp_detail(&r) != NULL);
        CHECK(jstr(resp_detail(&r), "expected", ex, sizeof ex) && strcmp(ex, "despawn_self or emit_event") == 0);
        CHECK(jstr(resp_detail(&r), "actual", ac, sizeof ac) && strcmp(ac, "unknown name") == 0);
        CHECK(jstr(resp_detail(&r), "system_id", ex, sizeof ex) == 0);
    }
    resp_free(&r);

    evaluate(s, id->cand_a, "sys_a.test.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "status", status, sizeof status);
    CHECK(strcmp(status, "passed") == 0);
    resp_free(&r);
    evaluate(s, id->cand_v2, "sys_a_v2.test.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "status", status, sizeof status);
    CHECK(strcmp(status, "passed") == 0);
    resp_free(&r);
    evaluate(s, id->cand_bad, "sys_a_bad.test.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "status", status, sizeof status);
    CHECK(strcmp(status, "failed") == 0);
    resp_free(&r);
    evaluate(s, id->cand_g2, "sys_a_gen2.test.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "status", status, sizeof status);
    CHECK(strcmp(status, "passed") == 0);
    resp_free(&r);
    evaluate(s, id->cand_b, "sys_b.test.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "status", status, sizeof status);
    CHECK(strcmp(status, "passed") == 0);
    resp_free(&r);

    /* The failing candidate records its evaluating worker and has not been installed. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"cs\",\"method\":\"candidate.status\",\"params\":{\"candidate_id\":\"%s\"}}",
             id->cand_bad);
    rpc(s, line, &r);
    CHECK_OK(r);
    CHECK(jnum(resp_result(&r), "worker_pid") > 0);
    {
        const SuJsonValue *ev = su_json_member(resp_result(&r), "evaluation");
        const SuJsonValue *inst = su_json_member(resp_result(&r), "install");
        char st[16];
        jstr(ev, "status", st, sizeof st);
        CHECK(strcmp(st, "failed") == 0);
        jstr(inst, "state", st, sizeof st);
        CHECK(strcmp(st, "not_attempted") == 0);
    }
    resp_free(&r);

    /* A copy of sys_a_v2 that is tampered with after its evaluation passed. */
    cp("sys_a_v2.lir.json", "mut.lir.json", NULL, NULL);
    cp("sys_a_v2.capsule.json", "mut.capsule.json", "sys_a_v2.lir.json", "mut.lir.json");
    cp("sys_a_v2.test.json", "mut.test.json", "sys_a_v2.capsule.json", "mut.capsule.json");
    submit(s, "mut.capsule.json", id->cand_mut, sizeof id->cand_mut, id->sem_mut, sizeof id->sem_mut);
    evaluate(s, id->cand_mut, "mut.test.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "status", status, sizeof status);
    CHECK(strcmp(status, "passed") == 0);
    resp_free(&r);
}

/* SHA-256 hex of a file under the scratch project root, computed here independently of the engine. */
static void file_sha256(const char *name, char out[65]) {
    char path[8192];
    uint8_t *bytes;
    size_t n = 0;
    SuSha256 ctx;
    uint8_t dg[32];

    root_path(path, sizeof path, name);
    bytes = read_whole(path, &n);
    su_sha256_init(&ctx);
    su_sha256_update(&ctx, bytes != NULL ? bytes : (const uint8_t *)"", bytes != NULL ? n : 0u);
    su_sha256_final(&ctx, dg);
    su_sha256_hex(dg, out);
    free(bytes);
}

/* Audit item 7: tests.run runs the suite now and keeps a completed job; job.status returns that record with
 * the exact input fingerprints. A failing suite completes with status failed. Unknown jobs are refused. */
static void test_jobs(Session *s, Ids *id) {
    Resp r;
    char line[512];
    char job[32];
    char job_bad[32];
    char st[16];
    char sha[65];
    char want[65];
    const SuJsonValue *inputs;
    const SuJsonValue *res;

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"tr\",\"method\":\"tests.run\",\"params\":"
             "{\"candidate_id\":\"%s\",\"suite\":\"sys_a.test.json\"}}",
             id->cand_a);
    rpc(s, line, &r);
    CHECK_OK(r);
    CHECK(jstr(resp_result(&r), "job_id", job, sizeof job));
    CHECK(jstr(resp_result(&r), "state", st, sizeof st) && strcmp(st, "completed") == 0);
    res = su_json_member(resp_result(&r), "result");
    CHECK(res != NULL && jstr(res, "status", st, sizeof st) && strcmp(st, "passed") == 0);
    resp_free(&r);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"js\",\"method\":\"job.status\",\"params\":{\"job_id\":\"%s\"}}",
             job);
    rpc(s, line, &r);
    CHECK_OK(r);
    CHECK(jstr(resp_result(&r), "job_id", st, sizeof st) && strcmp(st, job) == 0);
    CHECK(jstr(resp_result(&r), "state", st, sizeof st) && strcmp(st, "completed") == 0);
    CHECK(jstr(resp_result(&r), "candidate_id", st, sizeof st) && strcmp(st, id->cand_a) == 0);
    inputs = su_json_member(resp_result(&r), "inputs");
    file_sha256("sys_a.test.json", want);
    CHECK(jstr(inputs, "suite_sha256", sha, sizeof sha) && strcmp(sha, want) == 0);
    file_sha256("sys_a.capsule.json", want);
    CHECK(jstr(inputs, "capsule_sha256", sha, sizeof sha) && strcmp(sha, want) == 0);
    file_sha256("sys_a.lir.json", want);
    CHECK(jstr(inputs, "source_sha256", sha, sizeof sha) && strcmp(sha, want) == 0);
    res = su_json_member(resp_result(&r), "result");
    CHECK(res != NULL && jstr(res, "status", st, sizeof st) && strcmp(st, "passed") == 0);
    resp_free(&r);

    /* A failing candidate: the job completes and its result is failed. */
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"tb\",\"method\":\"tests.run\",\"params\":"
             "{\"candidate_id\":\"%s\",\"suite\":\"sys_a_bad.test.json\"}}",
             id->cand_bad);
    rpc(s, line, &r);
    CHECK_OK(r);
    CHECK(jstr(resp_result(&r), "job_id", job_bad, sizeof job_bad));
    res = su_json_member(resp_result(&r), "result");
    CHECK(res != NULL && jstr(res, "status", st, sizeof st) && strcmp(st, "failed") == 0);
    resp_free(&r);

    /* Unknown job; a suite that does not name the candidate's capsule creates no job. */
    rpc(s,
        "{\"protocol\":\"0.1\",\"id\":\"jx\",\"method\":\"job.status\",\"params\":{\"job_id\":\"job-999999\"}}",
        &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"tm\",\"method\":\"tests.run\",\"params\":"
             "{\"candidate_id\":\"%s\",\"suite\":\"sys_b.test.json\"}}",
             id->cand_a);
    rpc(s, line, &r);
    CHECK_CODE(r, SU_E_SCHEMA_MISMATCH);
    resp_free(&r);
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"js2\",\"method\":\"job.status\",\"params\":{\"job_id\":\"%s\"}}",
             job_bad);
    rpc(s, line, &r);
    CHECK_OK(r);
    CHECK(jstr(resp_result(&r), "job_id", st, sizeof st) && strcmp(st, job_bad) == 0);
    res = su_json_member(resp_result(&r), "result");
    CHECK(res != NULL && jstr(res, "status", st, sizeof st) && strcmp(st, "failed") == 0);
    resp_free(&r);
}

static void test_world_create(Session *s, Ids *id) {
    Resp r;
    Manifest m;
    char rev[64];
    char want[80];

    /* Backward-compatible single capsule_path. The capsule declares system_rev 7 (audit item 4): the
     * manifest shows it. A capsule without the key shows no declared_system_rev (world1 below). */
    create_one(s, "world4.json", "sys_a_rev.capsule.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "world_id", id->world0, sizeof id->world0);
    jstr(resp_result(&r), "world_revision", rev, sizeof rev);
    snprintf(want, sizeof want, "%s.r1", id->world0);
    CHECK(strcmp(rev, want) == 0);
    CHECK(jnum(resp_result(&r), "tick") == 0);
    CHECK(manifest_declared_rev(&r, 0) == 7);
    resp_free(&r);
    read_manifest(s, id->world0, &m);
    CHECK(m.n == 1);
    CHECK(strcmp(m.sys[0], "demo.movement") == 0);
    CHECK(strcmp(m.hash[0], id->sem_a) == 0);
    g_pid = active_pid(s);
    CHECK(g_pid > 0);

    /* Two systems, serial, in array order. */
    create_many(s, "world4.json", "sys_a.capsule.json", "sys_b.capsule.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "world_id", id->world1, sizeof id->world1);
    CHECK(jnum(resp_result(&r), "row_count") == 4);
    CHECK(jnum(resp_result(&r), "worker_pid") == g_pid);
    CHECK(manifest_item(&r, 0) != NULL && su_json_member(manifest_item(&r, 0), "declared_system_rev") == NULL);
    resp_free(&r);
    read_manifest(s, id->world1, &m);
    CHECK(m.n == 2);
    CHECK(strcmp(m.sys[0], "demo.movement") == 0);
    CHECK(strcmp(m.sys[1], "demo.drift") == 0);
    CHECK(strcmp(m.hash[1], id->sem_b) == 0);

    /* Exactly one of capsule_path or capsule_paths; a non-empty array. */
    rpc(s,
        "{\"protocol\":\"0.1\",\"id\":\"w1\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"world4.json\","
        "\"capsule_path\":\"sys_a.capsule.json\",\"capsule_paths\":[\"sys_b.capsule.json\"]}}",
        &r);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);
    rpc(s, "{\"protocol\":\"0.1\",\"id\":\"w2\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"world4.json\"}}", &r);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);
    rpc(s,
        "{\"protocol\":\"0.1\",\"id\":\"w3\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"world4.json\","
        "\"capsule_paths\":[]}}",
        &r);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);

    /* The 1,024-row world (paging test) and a second two-system world. */
    create_one(s, "world_1024.json", "sys_a.capsule.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "world_id", id->world2, sizeof id->world2);
    CHECK(jnum(resp_result(&r), "row_count") == 1024);
    resp_free(&r);

    create_many(s, "world4.json", "sys_a.capsule.json", "sys_b.capsule.json", &r);
    CHECK_OK(r);
    jstr(resp_result(&r), "world_id", id->world3, sizeof id->world3);
    resp_free(&r);

    /* The worker holds at most four worlds. */
    create_one(s, "world4.json", "sys_a.capsule.json", &r);
    CHECK_CODE(r, SU_E_RESOURCE_LIMIT);
    resp_free(&r);
}

/* TEST_SPEC T02-style: all 1,024 rows read through paged world.inspect, before and after a tick. */
static void test_paging(Session *s, Ids *id) {
    Ref expect;
    View v;
    Resp r;
    char line[512];
    char rev[80];

    ref_load_fixture("world_1024.json", &expect);
    CHECK(read_view(s, id->world2, 1024, &v));
    CHECK(ref_equal(&v.cols, &expect));
    CHECK(v.tick == 0);
    CHECK(v.pid == g_pid);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"pg\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\",\"limit\":16,\"offset\":1024}}",
             id->world2);
    rpc(s, line, &r);
    CHECK_OK(r);
    CHECK(jnum(resp_result(&r), "returned_rows") == 0);
    resp_free(&r);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"pg\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\",\"offset\":1025}}",
             id->world2);
    rpc(s, line, &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);

    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"pg\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\",\"limit\":257}}",
             id->world2);
    rpc(s, line, &r);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);

    /* One tick of sys_a: position += velocity on every row. */
    snprintf(rev, sizeof rev, "%s.r1", id->world2);
    step(s, id->world2, rev, 1, &r);
    CHECK_OK(r);
    resp_free(&r);
    ref_tick(&expect, A_V1, 0);
    CHECK(read_view(s, id->world2, 1024, &v));
    CHECK(ref_equal(&v.cols, &expect));
    CHECK(v.tick == 1);
}

/* Audit item 2: a code-only install is analysed against the whole schedule before it is prepared.
 * Each candidate below is refused with the status the effect analysis gives (conflict and declared !=
 * derived: SU_E_SCHEMA_MISMATCH; ordering cycle: SU_E_INVALID_ARGUMENT), and the world is unchanged:
 * revision, tick, PID and both installed semantic hashes. Runs on world3 (sys_a, sys_b at r1, tick 0). */
static void test_install_effects(Session *s, Ids *id) {
    char cc[32];
    char cy[32];
    char ov[32];
    char sem[80];
    char rev[80];
    Resp r;
    View v;
    Manifest m;

    submit(s, "sys_b_conflict.capsule.json", cc, sizeof cc, sem, sizeof sem);
    submit(s, "sys_a_cycle.capsule.json", cy, sizeof cy, sem, sizeof sem);
    submit(s, "sys_b_overdecl.capsule.json", ov, sizeof ov, sem, sizeof sem);
    snprintf(rev, sizeof rev, "%s.r1", id->world3);

    install(s, cc, id->world3, rev, 0, &r);
    CHECK_CODE(r, SU_E_SCHEMA_MISMATCH);
    CHECK(resp_message_has(&r, "conflict on field 1 (write/write)"));
    resp_free(&r);

    install(s, cy, id->world3, rev, 0, &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    CHECK(resp_message_has(&r, "ordering cycle"));
    resp_free(&r);

    install(s, ov, id->world3, rev, 0, &r);
    CHECK_CODE(r, SU_E_SCHEMA_MISMATCH);
    CHECK(resp_message_has(&r, "field position_x declared read=1 write=0"));
    resp_free(&r);

    CHECK(read_view(s, id->world3, 4, &v));
    CHECK(strcmp(v.rev, rev) == 0);
    CHECK(v.tick == 0);
    CHECK(v.pid == g_pid);
    read_manifest(s, id->world3, &m);
    CHECK(m.n == 2);
    CHECK(strcmp(m.sys[0], "demo.movement") == 0);
    CHECK(strcmp(m.hash[0], id->sem_a) == 0);
    CHECK(strcmp(m.sys[1], "demo.drift") == 0);
    CHECK(strcmp(m.hash[1], id->sem_b) == 0);
}

static void test_swap(Session *s, Ids *id) {
    Ref expect;
    View before;
    View after;
    Resp r;
    Manifest m;
    char rev[80];
    char want[80];
    uint64_t txid = 0;
    uint64_t pre_pid;

    ref_init4(&expect);
    CHECK(read_view(s, id->world1, 4, &before));
    CHECK(ref_equal(&before.cols, &expect));
    pre_pid = before.pid;
    CHECK(pre_pid == g_pid);

    /* One tick on v1 + drift, then swap in v2 at tick 1. */
    snprintf(rev, sizeof rev, "%s.r1", id->world1);
    step(s, id->world1, rev, 1, &r);
    CHECK_OK(r);
    resp_free(&r);
    ref_tick(&expect, A_V1, 1);

    snprintf(rev, sizeof rev, "%s.r2", id->world1);
    install(s, id->cand_v2, id->world1, rev, 1, &r);
    CHECK_OK(r);
    if (resp_ok(&r)) {
        const SuJsonValue *res = resp_result(&r);
        char h[65];
        txid = jnum(res, "txid");
        CHECK(txid > 0);
        CHECK(jnum(res, "effective_tick") == 1);
        CHECK(jnum(res, "system_revision") == 2);
        CHECK(jnum(res, "worker_pid") == pre_pid);
        jstr(res, "previous_semantic_hash", h, sizeof h);
        CHECK(strcmp(h, id->sem_a) == 0);
        jstr(res, "semantic_hash", h, sizeof h);
        CHECK(strcmp(h, id->sem_v2) == 0);
    }
    resp_free(&r);

    /* World state, PID and the other system are preserved; the manifest changed. */
    CHECK(read_view(s, id->world1, 4, &after));
    CHECK(ref_equal(&after.cols, &expect));
    CHECK(after.tick == 1);
    CHECK(after.pid == pre_pid);
    snprintf(want, sizeof want, "%s.r3", id->world1);
    CHECK(strcmp(after.rev, want) == 0);
    read_manifest(s, id->world1, &m);
    CHECK(m.n == 2);
    CHECK(strcmp(m.hash[0], id->sem_v2) == 0);
    CHECK(strcmp(m.hash[1], id->sem_b) == 0);
    CHECK(txid > 0 && m.txid[0] == txid);

    /* Two more ticks run the new behaviour. */
    step(s, id->world1, after.rev, 2, &r);
    CHECK_OK(r);
    resp_free(&r);
    ref_tick(&expect, A_V2, 1);
    ref_tick(&expect, A_V2, 1);
    CHECK(read_view(s, id->world1, 4, &after));
    CHECK(ref_equal(&after.cols, &expect));
    CHECK(after.tick == 3);
    CHECK(after.pid == pre_pid);

    /* The candidate record reports the install, its effective tick and the worker PID. */
    {
        char line[512];
        char st[16];
        const SuJsonValue *inst;
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"cs\",\"method\":\"candidate.status\",\"params\":{\"candidate_id\":\"%s\"}}",
                 id->cand_v2);
        rpc(s, line, &r);
        CHECK_OK(r);
        inst = su_json_member(resp_result(&r), "install");
        jstr(inst, "state", st, sizeof st);
        CHECK(strcmp(st, "installed") == 0);
        CHECK(jnum(inst, "txid") == txid);
        CHECK(jnum(inst, "effective_tick") == 1);
        CHECK(jnum(resp_result(&r), "worker_pid") == pre_pid);
        resp_free(&r);
    }
}

static void test_failed_and_stale(Session *s, Ids *id) {
    View before;
    View after;
    Manifest m0;
    Manifest m1;
    Resp r;
    char rev[80];

    CHECK(read_view(s, id->world1, 4, &before));
    read_manifest(s, id->world1, &m0);

    /* T06: a candidate whose evaluation failed is rejected by its evidence gate. */
    install(s, id->cand_bad, id->world1, before.rev, before.tick, &r);
    CHECK_CODE(r, SU_E_CANDIDATE_FAILED);
    CHECK(resp_message_has(&r, "state=rejected"));
    resp_free(&r);
    CHECK(read_view(s, id->world1, 4, &after));
    CHECK(view_equal(&before, &after));
    read_manifest(s, id->world1, &m1);
    CHECK(m1.n == m0.n && strcmp(m1.hash[0], m0.hash[0]) == 0 && strcmp(m1.hash[1], m0.hash[1]) == 0);

    /* T06: bytes changed after evaluation are detected by the worker, not installed. */
    cp("sys_a_bad.lir.json", "mut.lir.json", NULL, NULL);
    install(s, id->cand_mut, id->world1, before.rev, before.tick, &r);
    CHECK_CODE(r, SU_E_CANDIDATE_FAILED);
    CHECK(resp_message_has(&r, "bytes changed"));
    resp_free(&r);
    CHECK(read_view(s, id->world1, 4, &after));
    CHECK(view_equal(&before, &after));

    /* T07: a layout generation change is rejected as a schema mismatch. */
    install(s, id->cand_g2, id->world1, before.rev, before.tick, &r);
    CHECK_CODE(r, SU_E_SCHEMA_MISMATCH);
    resp_free(&r);
    CHECK(read_view(s, id->world1, 4, &after));
    CHECK(view_equal(&before, &after));

    /* T07: stale expected revision, then stale effective tick. */
    snprintf(rev, sizeof rev, "%s.r0", id->world1);
    install(s, id->cand_v2, id->world1, rev, before.tick, &r);
    CHECK_CODE(r, SU_E_STALE_REVISION);
    resp_free(&r);
    /* The stale expected revision is refused at entry, before the candidate is read or evaluated: a candidate
     * that would fail evaluation (cand_bad) reports the stale revision, not CANDIDATE_FAILED. */
    install(s, id->cand_bad, id->world1, rev, before.tick, &r);
    CHECK_CODE(r, SU_E_STALE_REVISION);
    CHECK(resp_message_has(&r, "expected_world_revision"));
    resp_free(&r);
    install(s, id->cand_v2, id->world1, before.rev, 0, &r);
    CHECK_CODE(r, SU_E_STALE_REVISION);
    CHECK(resp_message_has(&r, "allowed_effective_tick"));
    resp_free(&r);
    CHECK(read_view(s, id->world1, 4, &after));
    CHECK(view_equal(&before, &after));

    /* Unknown identifiers, and a system that world0 does not run. */
    install(s, id->cand_v2, "world-999", before.rev, before.tick, &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    install(s, "candidate-999", id->world1, before.rev, before.tick, &r);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    {
        View w0;
        CHECK(read_view(s, id->world0, 4, &w0));
        install(s, id->cand_b, id->world0, w0.rev, w0.tick, &r);
        CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
        CHECK(resp_message_has(&r, "not in this world's schedule"));
        resp_free(&r);
    }
    rpc(s,
        "{\"protocol\":\"0.1\",\"id\":\"in\",\"method\":\"candidate.install\",\"params\":{\"candidate_id\":\"x\"}}",
        &r);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);
}

/* T09: the second system fails after the first has written; the published root holds. */
static void test_second_system_failure(Session *s, Ids *id, const char *fault_path) {
    Ref expect;
    View before;
    View after;
    Resp r;
    char rev[80];

    ref_init4(&expect);
    CHECK(read_view(s, id->world3, 4, &before));
    CHECK(before.tick == 0);

    write_bytes(fault_path, "1", 1);
    snprintf(rev, sizeof rev, "%s.r1", id->world3);
    step(s, id->world3, rev, 1, &r);
    CHECK_CODE(r, SU_E_CANDIDATE_FAILED);
    CHECK(resp_message_has(&r, "system 1 (demo.drift) failed"));
    CHECK(resp_message_has(&r, "completed 0 of 1 ticks"));
    resp_free(&r);
    CHECK(read_view(s, id->world3, 4, &after));
    CHECK(view_equal(&before, &after));

    /* A good tick, then a failing two-tick request: it rolls back to the last published root. */
    write_bytes(fault_path, "none", 4);
    step(s, id->world3, before.rev, 1, &r);
    CHECK_OK(r);
    resp_free(&r);
    ref_tick(&expect, A_V1, 1);
    CHECK(read_view(s, id->world3, 4, &before));
    CHECK(ref_equal(&before.cols, &expect));
    CHECK(before.tick == 1);

    write_bytes(fault_path, "1", 1);
    step(s, id->world3, before.rev, 2, &r);
    CHECK_CODE(r, SU_E_CANDIDATE_FAILED);
    CHECK(resp_message_has(&r, "completed 0 of 2 ticks"));
    resp_free(&r);
    CHECK(read_view(s, id->world3, 4, &after));
    CHECK(view_equal(&before, &after));

    write_bytes(fault_path, "none", 4);
}

/* T11: 1,000 installs (successful with reversion, failed, stale) with ticks between.
 * Registry retention must stay bounded, and the live manifest must name the last install. */
static void test_repeated_reload(Session *s, Ids *id) {
    enum { ITERATIONS = 1000 };
    Ref expect;
    View v;
    Resp r;
    char cur_rev[80];
    char stale[80];
    uint64_t cur_tick;
    int cur_a = A_V1;
    uint64_t last_txid = 0;
    uint64_t successes = 0;
    uint32_t i;
    Manifest m;

    ref_init4(&expect);
    CHECK(read_view(s, id->world0, 4, &v));
    CHECK(ref_equal(&v.cols, &expect));
    snprintf(cur_rev, sizeof cur_rev, "%s", v.rev);
    cur_tick = v.tick;
    snprintf(stale, sizeof stale, "%s.r0", id->world0);

    for (i = 0; i < ITERATIONS; i++) {
        uint32_t kind = i % 4u;
        if (kind == 0u || kind == 1u) {
            /* Successful installs alternate between v2 and v1 (a reversion each time). */
            const char *target = (successes % 2u == 0u) ? id->cand_v2 : id->cand_a;
            int target_a = (successes % 2u == 0u) ? A_V2 : A_V1;
            uint64_t tick_at_install = cur_tick;
            install(s, target, id->world0, cur_rev, cur_tick, &r);
            CHECK_OK(r);
            if (resp_ok(&r)) {
                char next[80];
                uint64_t txid = jnum(resp_result(&r), "txid");
                CHECK(txid > last_txid);
                CHECK(jnum(resp_result(&r), "effective_tick") == tick_at_install);
                jstr(resp_result(&r), "world_revision", next, sizeof next);
                snprintf(cur_rev, sizeof cur_rev, "%s", next);
                last_txid = txid;
                cur_a = target_a;
                successes++;
            }
            resp_free(&r);
            /* One tick of the newly installed behaviour. */
            step(s, id->world0, cur_rev, 1, &r);
            CHECK_OK(r);
            if (resp_ok(&r)) {
                char next[80];
                jstr(resp_result(&r), "world_revision", next, sizeof next);
                snprintf(cur_rev, sizeof cur_rev, "%s", next);
                cur_tick++;
                ref_tick(&expect, cur_a, 0);
            }
            resp_free(&r);
        } else if (kind == 2u) {
            install(s, id->cand_bad, id->world0, cur_rev, cur_tick, &r);
            CHECK_CODE(r, SU_E_CANDIDATE_FAILED);
            resp_free(&r);
        } else {
            install(s, id->cand_v2, id->world0, stale, cur_tick, &r);
            CHECK_CODE(r, SU_E_STALE_REVISION);
            resp_free(&r);
        }
    }

    CHECK(successes == 500);
    CHECK(cur_tick == 500);
    CHECK(read_view(s, id->world0, 4, &v));
    CHECK(ref_equal(&v.cols, &expect));
    CHECK(v.tick == cur_tick);
    CHECK(strcmp(v.rev, cur_rev) == 0);
    CHECK(v.pid == g_pid);

    /* The live manifest names the last installed transaction; the registry stayed bounded. */
    read_manifest(s, id->world0, &m);
    CHECK(m.n == 1 && m.txid[0] == last_txid);
    {
        char line[256];
        const SuJsonValue *reg;
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"rg\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"%s\",\"limit\":1}}",
                 id->world0);
        rpc(s, line, &r);
        CHECK_OK(r);
        reg = resp_result(&r) != NULL ? su_json_member(resp_result(&r), "registry") : NULL;
        CHECK(jnum(reg, "capacity") == 128);
        CHECK(jnum(reg, "in_use") <= 128);
        CHECK(jnum(reg, "evictions") > 0);
        /* Live schedules: W0 (1 system), W1 (2), W2 (1), W3 (2) are INSTALLED; all else is
         * terminal and evictable. */
        CHECK(jnum(reg, "installed") == 6);
        resp_free(&r);
    }
}

/* ---------- main ---------- */

static void remove_root(void) {
    DIR *d = opendir(g_root);
    struct dirent *de;
    if (d == NULL) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char p[8192];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        root_path(p, sizeof p, de->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(g_root);
}

/* Verifier location for a worker-side failure (audit item 6, worker remainder): su_reload_load_diag fills
 * the first error's JSON pointer and leaves the plain su_reload_load contract unchanged. */
static void test_verify_location(const char *repo) {
    char cap[4096];
    char lir[4096];
    char msg[256];
    SuLirModule *m = NULL;
    SuDiag d;
    uint32_t sv = 0;
    uint32_t gen = 0;
    uint32_t st;

    snprintf(cap, sizeof cap, "%s/tests/fixtures/ir02/cap_out_of_range.capsule.json", repo);
    snprintf(lir, sizeof lir, "%s/tests/fixtures/ir02/cap_out_of_range.lir.json", repo);
    st = su_reload_load_diag(cap, lir, NULL, NULL, &m, &sv, &gen, msg, sizeof msg, &d);
    CHECK(st != SU_OK);
    CHECK(d.code == st);
    CHECK(d.json_pointer[0] != '\0');
    CHECK(strncmp(msg, "verification failed", 19) == 0);
    su_reload_module_free(m);
}

int main(int argc, char **argv) {
    static const char *helper_argv[2];
    char base[4000];
    char fault_path[8192];
    SuAgentConfig cfg;
    Session s;
    Ids ids;
    const char *tmp;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <worker_helper_path> <repo_root>\n", argv[0]);
        return 2;
    }
    tmp = getenv("SU_TEST_SCRATCH");
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = getenv("TMPDIR");
    }
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    snprintf(base, sizeof base, "%s/su_reload_XXXXXX", tmp);
    if (mkdtemp(base) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(g_root, sizeof g_root, "%s", base);
    stage_fixtures(argv[2]);
    root_path(fault_path, sizeof fault_path, "system_fault.txt");
    write_bytes(fault_path, "none", 4);
    setenv("SU_TEST_SYSTEM_FAULT_FILE", fault_path, 1);

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

    test_verify_location(argv[2]);
    memset(&ids, 0, sizeof ids);
    s = session_start(&cfg);
    test_engine_info(&s);
    test_submit_and_evaluate(&s, &ids);
    test_jobs(&s, &ids);
    test_world_create(&s, &ids);
    test_paging(&s, &ids);
    test_install_effects(&s, &ids);
    test_swap(&s, &ids);
    test_failed_and_stale(&s, &ids);
    test_second_system_failure(&s, &ids, fault_path);
    test_repeated_reload(&s, &ids);
    CHECK(active_pid(&s) == g_pid);
    session_stop(&s);

    remove_root();
    printf("test_reload: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
