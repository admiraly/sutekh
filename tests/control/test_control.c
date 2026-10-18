/* M0-05 control-plane tests (T15 protocol and path policy, T10 worker isolation).
 *
 * Usage: test_control <worker_helper_path> [project_root]
 *   worker_helper_path  the worker executable (tests/control/worker_stub, built from
 *                       worker_stub.c; or the real `sutekh worker` once wired).
 *   project_root        defaults to the current directory, which must be the
 *                       repository root (fixtures are named relative to it).
 * Environment:
 *   SU_TEST_SCRATCH     writable directory for the fault file and a symlink test.
 *                       Defaults to $TMPDIR, then /tmp.
 * The supervisor runs in a forked child that owns the pipe pair, so the test can
 * change the fault file between requests while the active worker stays alive. */
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

#ifdef SUTEKH_NATIVE_BUILD
#define SU_MODE_NAME "dev_native"
#else
#define SU_MODE_NAME "dev_checked"
#endif

#include "../common/su_test_os.h"
#include "agent/su_agent.h"
#include "agent/su_path.h"
#include "agent/su_proc.h"
#include "agent/su_proto.h"
#include "ir/su_json.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"

static int g_checks = 0;
static int g_failures = 0;
static int g_not_run = 0; /* checks this platform cannot perform; reported, never counted as passed */

#if defined(__SANITIZE_ADDRESS__)
#define TEST_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_UNDER_ASAN 1
#endif
#endif

#define PATH_MAX_TEST 4000
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
            fprintf(stderr, "%s:%d: expected %s, got ok=%d status=%llu text=%.*s\n",  \
                    __FILE__, __LINE__, su_status_name(want), resp_ok(&(resp)),       \
                    (unsigned long long)resp_error_status(&(resp)),                   \
                    (int)(resp).len, (const char *)(resp).text);                      \
        }                                                                             \
    } while (0)

#define CHECK_OK(resp)                                                                \
    do {                                                                              \
        g_checks++;                                                                   \
        if (!resp_ok(&(resp))) {                                                      \
            g_failures++;                                                             \
            fprintf(stderr, "%s:%d: expected ok response: %.*s\n", __FILE__, __LINE__, \
                    (int)(resp).len, (const char *)(resp).text);                      \
        }                                                                             \
    } while (0)

/* ---------- a supervisor session in a forked child ---------- */

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

/* Sends raw bytes followed by a newline, then reads exactly one response line. */
static int call_bytes(Session *s, const uint8_t *p, size_t n, Resp *r) {
    const uint8_t *line = NULL;
    size_t len = 0;
    int overflow = 0;
    int rc;
    SuJsonLimits lim;
    SuJsonError jerr;
    uint32_t st;

    memset(r, 0, sizeof *r);
    if (su_write_all(s->wfd, p, n, -1) != 0 || su_write_all(s->wfd, (const uint8_t *)"\n", 1, -1) != 0) {
        return -1;
    }
    s->rd.deadline_ms = su_now_ms() + 120000;
    rc = su_line_next(&s->rd, &line, &len, &overflow);
    if (rc != 1) {
        return -1;
    }
    r->text = malloc(len + 1u);
    if (r->text == NULL) {
        return -1;
    }
    memcpy(r->text, line, len);
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

static int call(Session *s, const char *line, Resp *r) {
    return call_bytes(s, (const uint8_t *)line, strlen(line), r);
}

/* ---------- filesystem helpers ---------- */

static void write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror(path);
        exit(2);
    }
    if (text[0] != '\0') {
        if (write(fd, text, SU_IO_COUNT(strlen(text))) < 0) {
            perror(path);
            exit(2);
        }
    }
    close(fd);
}

/* ---------- tests ---------- */

static void test_protocol_errors(Session *s) {
    Resp r;

    CHECK(call(s, "{", &r) == 0);
    CHECK_CODE(r, SU_E_PARSE);
    resp_free(&r);

    {
        static const uint8_t bad_utf8[] = "{\"protocol\":\"0.1\",\"id\":\"x\xff\",\"method\":\"engine.info\",\"params\":{}}";
        CHECK(call_bytes(s, bad_utf8, sizeof bad_utf8 - 1u, &r) == 0);
        CHECK_CODE(r, SU_E_PARSE);
        resp_free(&r);
    }
    CHECK(call(s, "{\"protocol\":\"0.1\",\"protocol\":\"0.1\",\"id\":\"d1\",\"method\":\"engine.info\",\"params\":{}}", &r) == 0);
    CHECK_CODE(r, SU_E_DUPLICATE_KEY);
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.2\",\"id\":\"w1\",\"method\":\"engine.info\",\"params\":{}}", &r) == 0);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"e1\",\"method\":\"engine.info\",\"params\":{},\"extra\":1}", &r) == 0);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);

    CHECK(call(s, "[1,2]", &r) == 0);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"u1\",\"method\":\"world.teleport\",\"params\":{}}", &r) == 0);
    CHECK_CODE(r, SU_E_UNSUPPORTED_METHOD);
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"u2\",\"method\":\"world.run\",\"params\":{\"world_id\":\"world-1\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_UNSUPPORTED_METHOD);
    resp_free(&r);

    /* Oversized line: more than 1 MiB before the newline. The id cannot be read,
     * so the response carries a null id. */
    {
        const char *head = "{\"protocol\":\"0.1\",\"id\":\"big\",\"method\":\"engine.info\",\"params\":{\"pad\":\"";
        const char *tail = "\"}}";
        size_t pad = SU_LIMIT_PROTOCOL_MESSAGE + 64u;
        size_t hl = strlen(head);
        size_t tl = strlen(tail);
        uint8_t *big = malloc(hl + pad + tl);
        CHECK(big != NULL);
        if (big != NULL) {
            memcpy(big, head, hl);
            memset(big + hl, 'a', pad);
            memcpy(big + hl + pad, tail, tl);
            CHECK(call_bytes(s, big, hl + pad + tl, &r) == 0);
            CHECK_CODE(r, SU_E_RESOURCE_LIMIT);
            resp_free(&r);
            free(big);
        }
    }

    /* Still serving after every rejection. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"info-after\",\"method\":\"engine.info\",\"params\":{}}", &r) == 0);
    CHECK_OK(r);
    /* runtime_mode names the build (ARCHITECTURE section 7): dev_native for SUTEKH_NATIVE, else dev_checked. */
    {
        const char *want = "\"runtime_mode\":\"" SU_MODE_NAME "\"";
        size_t wl = strlen(want);
        int found = 0;
        for (size_t i = 0; r.len >= wl && i + wl <= r.len; i++) {
            if (memcmp(r.text + i, want, wl) == 0) {
                found = 1;
                break;
            }
        }
        CHECK(found);
    }
    resp_free(&r);
}

/* An existing file outside the project root, named by an absolute path: /etc/hostname on POSIX; on Windows
 * the fault file in the scratch directory (set in main). */
static char g_outside[PATH_MAX_TEST + 32] = "/etc/hostname";

static void test_path_policy(Session *s) {
    Resp r;
    char line[PATH_MAX_TEST + 256];
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"p1\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"../../../../etc/passwd\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    resp_free(&r);
    snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"p2\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"%s\"}}", g_outside);
    CHECK(call(s, line, &r) == 0);
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"p3\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"examples/../../etc/passwd\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    resp_free(&r);
}

static void test_symlink_escape(const SuAgentConfig *base, const char *scratch, const char *root) {
    char link_path[1024];
    char target[1024];
    Session s;
    Resp r;
    SuAgentConfig cfg = *base;

    CHECK(snprintf(link_path, sizeof link_path, "%s/escape.json", scratch) < (int)sizeof link_path);
    CHECK(snprintf(target, sizeof target, "%s/examples/movement.capsule.json", root) < (int)sizeof target);
    unlink(link_path);
    {
        const char *why = NULL;
        if (su_test_symlink(target, link_path, &why) != 0) {
#if defined(_WIN32)
            fprintf(stderr, "test_control: symlink escape check NOT RUN: %s\n", why);
            g_not_run++;
            return;
#else
            CHECK(0);
#endif
        }
    }
    cfg.project_root = scratch;
    s = session_start(&cfg);
    CHECK(call(&s, "{\"protocol\":\"0.1\",\"id\":\"sym1\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"escape.json\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    resp_free(&r);
    session_stop(&s);
    unlink(link_path);
}

/* capsule.submit source_path override (AGENT_RPC: replaces the capsule's own source binding). */
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    FILE *out;
    char buf[4096];
    size_t n;
    int ok = 1;

    if (in == NULL) {
        return 0;
    }
    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
        }
    }
    ok = ok && ferror(in) == 0;
    fclose(in);
    return fclose(out) == 0 && ok;
}

static void test_source_path(Session *s, const SuAgentConfig *base, const char *scratch, const char *root) {
    char rdir[1024];
    char path[1200];
    char target[1200];
    Session t;
    Resp r;
    SuAgentConfig cfg = *base;
    int have_link = 1;

    /* Main project root: valid sibling accepted; traversal, absolute path, wrong type, extra key rejected. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"sp-bad-type\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"examples/movement.capsule.json\",\"source_path\":5}}", &r) == 0);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"sp-extra\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"examples/movement.capsule.json\",\"bogus\":1}}", &r) == 0);
    CHECK_CODE(r, SU_E_TYPE);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"sp-trav\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"examples/movement.capsule.json\",\"source_path\":\"../../../../etc/passwd\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    resp_free(&r);
    {
        char line[PATH_MAX_TEST + 256];
        snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"sp-abs\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"examples/movement.capsule.json\",\"source_path\":\"%s\"}}", g_outside);
        CHECK(call(s, line, &r) == 0);
    }
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    resp_free(&r);

    /* Scratch project root: a regular sibling with a different name is accepted; a symlink inside the
     * root that points outside it is rejected. The capsule and its own source are copies in the root. */
    CHECK(snprintf(rdir, sizeof rdir, "%s/sp-root", scratch) < (int)sizeof rdir);
    if (mkdir(rdir, 0700) != 0 && errno != EEXIST) {
        perror("mkdir sp-root");
        exit(2);
    }
    CHECK(snprintf(path, sizeof path, "%s/movement.capsule.json", rdir) < (int)sizeof path);
    CHECK(copy_file("examples/movement.capsule.json", path));
    CHECK(snprintf(path, sizeof path, "%s/movement.lir.json", rdir) < (int)sizeof path);
    CHECK(copy_file("examples/movement.lir.json", path));
    CHECK(snprintf(path, sizeof path, "%s/alt.lir.json", rdir) < (int)sizeof path);
    CHECK(copy_file("examples/movement.lir.json", path));
    CHECK(snprintf(path, sizeof path, "%s/esc.lir.json", rdir) < (int)sizeof path);
    CHECK(snprintf(target, sizeof target, "%s/examples/movement.lir.json", root) < (int)sizeof target);
    unlink(path);
    {
        const char *why = NULL;
        if (su_test_symlink(target, path, &why) != 0) {
#if defined(_WIN32)
            fprintf(stderr, "test_control: source_path symlink check NOT RUN: %s\n", why);
            g_not_run++;
            have_link = 0;
#else
            CHECK(0);
#endif
        }
    }

    cfg.project_root = rdir;
    t = session_start(&cfg);
    CHECK(call(&t, "{\"protocol\":\"0.1\",\"id\":\"sp-alt\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"movement.capsule.json\",\"source_path\":\"alt.lir.json\"}}", &r) == 0);
    CHECK_OK(r);
    resp_free(&r);
    if (have_link) {
        CHECK(call(&t, "{\"protocol\":\"0.1\",\"id\":\"sp-esc\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"movement.capsule.json\",\"source_path\":\"esc.lir.json\"}}", &r) == 0);
        CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
        resp_free(&r);
    }
    session_stop(&t);
    unlink(path);
    CHECK(snprintf(path, sizeof path, "%s/movement.capsule.json", rdir) < (int)sizeof path);
    unlink(path);
    CHECK(snprintf(path, sizeof path, "%s/movement.lir.json", rdir) < (int)sizeof path);
    unlink(path);
    CHECK(snprintf(path, sizeof path, "%s/alt.lir.json", rdir) < (int)sizeof path);
    unlink(path);
    rmdir(rdir);
}

static void test_world_sequence(Session *s, const char *fault_file) {
    Resp r;
    const SuJsonValue *res;
    const SuJsonValue *cols;
    const SuJsonValue *px;
    uint64_t v = 0;
    uint64_t pid_before = 0;
    uint64_t pid_after = 0;
    int64_t t0;

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"e0\",\"method\":\"engine.info\",\"params\":{}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    CHECK(res != NULL && su_json_get_uint(su_json_member(su_json_member(res, "active_worker"), "pid"), &pid_before) &&
          pid_before == 0);
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"c1\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"examples/movement.capsule.json\"}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    CHECK(res != NULL && su_json_is_string(su_json_member(res, "candidate_id")) &&
          memcmp(su_json_member(res, "candidate_id")->u.string.data, "candidate-1", 11) == 0);
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"w1\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"examples/world.json\",\"capsule_path\":\"examples/movement.capsule.json\"}}", &r) == 0);
    CHECK_OK(r);
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"i0\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"world-1\",\"fields\":[\"position_x\"],\"limit\":4}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    cols = res != NULL ? su_json_member(res, "columns") : NULL;
    px = cols != NULL ? su_json_member(cols, "position_x") : NULL;
    CHECK(px != NULL && px->type == SU_JSON_ARRAY && px->u.array.count == 4u);
    if (px != NULL && px->type == SU_JSON_ARRAY && px->u.array.count == 4u) {
        CHECK(su_json_get_uint(px->u.array.items[0], &v) && v == 0);
        CHECK(su_json_get_uint(px->u.array.items[2], &v) && v == 4294967295ULL);
    }
    resp_free(&r);

    /* One tick: expected [1,3,0,7]. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"s1\",\"method\":\"world.step\",\"params\":{\"world_id\":\"world-1\",\"expected_world_revision\":\"world-1.r1\",\"ticks\":1}}", &r) == 0);
    CHECK_OK(r);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"i1\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"world-1\",\"fields\":[\"position_x\"],\"limit\":4}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    cols = res != NULL ? su_json_member(res, "columns") : NULL;
    px = cols != NULL ? su_json_member(cols, "position_x") : NULL;
    if (px != NULL && px->type == SU_JSON_ARRAY && px->u.array.count == 4u) {
        uint64_t want[4] = {1, 3, 0, 7};
        size_t k;
        for (k = 0; k < 4; k++) {
            CHECK(su_json_get_uint(px->u.array.items[k], &v) && v == want[k]);
        }
    } else {
        CHECK(0);
    }
    resp_free(&r);

    /* A stale revision is refused and changes nothing. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"s-stale\",\"method\":\"world.step\",\"params\":{\"world_id\":\"world-1\",\"expected_world_revision\":\"world-1.r1\",\"ticks\":1}}", &r) == 0);
    CHECK_CODE(r, SU_E_STALE_REVISION);
    resp_free(&r);

    /* Two more ticks: total 3, expected [3,7,2,7]. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"s2\",\"method\":\"world.step\",\"params\":{\"world_id\":\"world-1\",\"expected_world_revision\":\"world-1.r2\",\"ticks\":2}}", &r) == 0);
    CHECK_OK(r);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"i3\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"world-1\",\"fields\":[\"position_x\"],\"limit\":4}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    cols = res != NULL ? su_json_member(res, "columns") : NULL;
    px = cols != NULL ? su_json_member(cols, "position_x") : NULL;
    if (px != NULL && px->type == SU_JSON_ARRAY && px->u.array.count == 4u) {
        uint64_t want[4] = {3, 7, 2, 7};
        size_t k;
        for (k = 0; k < 4; k++) {
            CHECK(su_json_get_uint(px->u.array.items[k], &v) && v == want[k]);
        }
    } else {
        CHECK(0);
    }
    resp_free(&r);

    /* Evaluate the candidate's fixture test in a disposable worker. */
    write_text(fault_file, "");
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"ev1\",\"method\":\"candidate.evaluate\",\"params\":{\"candidate_id\":\"candidate-1\",\"test_path\":\"examples/movement.test.json\"}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    CHECK(res != NULL && su_json_get_uint(su_json_member(res, "cases_total"), &v) && v == 3u);
    CHECK(res != NULL && su_json_get_uint(su_json_member(res, "cases_passed"), &v) && v == 3u);
    resp_free(&r);

    /* T10: a candidate worker killed mid-evaluation. */
    pid_before = 0;
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"e1\",\"method\":\"engine.info\",\"params\":{}}", &r) == 0);
    res = resp_result(&r);
    CHECK(res != NULL && su_json_get_uint(su_json_member(su_json_member(res, "active_worker"), "pid"), &pid_before));
    resp_free(&r);

    write_text(fault_file, "kill");
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"ev-kill\",\"method\":\"candidate.evaluate\",\"params\":{\"candidate_id\":\"candidate-1\",\"test_path\":\"examples/movement.test.json\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_WORKER_FAILED);
    {
        const SuJsonValue *e = r.root != NULL ? su_json_member(r.root, "error") : NULL;
        const SuJsonValue *rt = e != NULL ? su_json_member(e, "retry") : NULL;
        CHECK(rt != NULL && rt->type == SU_JSON_BOOL && rt->u.boolean == 1);
    }
    resp_free(&r);

    /* T10: a hung candidate worker is stopped by the time limit. */
    write_text(fault_file, "hang");
    t0 = su_now_ms();
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"ev-hang\",\"method\":\"candidate.evaluate\",\"params\":{\"candidate_id\":\"candidate-1\",\"test_path\":\"examples/movement.test.json\",\"timeout_ms\":300}}", &r) == 0);
    CHECK_CODE(r, SU_E_WORKER_FAILED);
    CHECK(su_now_ms() - t0 >= 250 && su_now_ms() - t0 < 30000);
    resp_free(&r);

    /* Supervisor and active worker survive both failures. */
    write_text(fault_file, "");
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"e2\",\"method\":\"engine.info\",\"params\":{}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    CHECK(res != NULL && su_json_get_uint(su_json_member(su_json_member(res, "active_worker"), "pid"), &pid_after));
    CHECK(pid_after != 0 && pid_after == pid_before);
    CHECK(pid_after != 0 && su_test_pid_alive(pid_after));
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"i4\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"world-1\",\"fields\":[\"position_x\"],\"limit\":4}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    {
        const SuJsonValue *rev = res != NULL ? su_json_member(res, "world_revision") : NULL;
        CHECK(su_json_is_string(rev) && rev->u.string.len == 10u && memcmp(rev->u.string.data, "world-1.r4", 10) == 0);
    }
    cols = res != NULL ? su_json_member(res, "columns") : NULL;
    px = cols != NULL ? su_json_member(cols, "position_x") : NULL;
    if (px != NULL && px->type == SU_JSON_ARRAY && px->u.array.count == 4u) {
        uint64_t want[4] = {3, 7, 2, 7};
        size_t k;
        for (k = 0; k < 4; k++) {
            CHECK(su_json_get_uint(px->u.array.items[k], &v) && v == want[k]);
        }
    } else {
        CHECK(0);
    }
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"cs\",\"method\":\"candidate.status\",\"params\":{\"candidate_id\":\"candidate-1\"}}", &r) == 0);
    CHECK_OK(r);
    resp_free(&r);
}

/* ---------- world.close (M1 prerequisite: a worker holds at most 4 worlds) ---------- */

static int str_is(const SuJsonValue *v, const char *want) {
    return v != NULL && su_json_is_string(v) && strlen(want) == v->u.string.len &&
           memcmp(v->u.string.data, want, v->u.string.len) == 0;
}

/* Copies a string member of res into out. Returns 1 when the member is a string that fits. */
static int str_copy(const SuJsonValue *v, char *out, size_t cap) {
    if (v == NULL || !su_json_is_string(v) || v->u.string.len + 1u > cap) {
        return 0;
    }
    memcpy(out, v->u.string.data, v->u.string.len);
    out[v->u.string.len] = '\0';
    return 1;
}

static uint64_t uint_of(const SuJsonValue *res, const char *key) {
    uint64_t v = UINT64_MAX;
    if (res == NULL || !su_json_get_uint(su_json_member(res, key), &v)) {
        return UINT64_MAX;
    }
    return v;
}

/* Supervisor path (agent): creation, the 4-world cap, stale and unknown closes, use after
 * close, id monotonicity, slot reuse, and snapshots of a closed world. world-1 already
 * exists (test_world_sequence). */
static void test_world_close_supervisor(Session *s) {
    Resp r;
    char snap3[80];
    char tmp[512];
    const SuJsonValue *res;

    /* Four worlds live (world-1 .. world-4); the fifth is refused. */
    for (int k = 2; k <= 4; k++) {
        snprintf(tmp, sizeof tmp,
                 "{\"protocol\":\"0.1\",\"id\":\"wc%d\",\"method\":\"world.create\",\"params\":"
                 "{\"fixture_path\":\"examples/world.json\",\"capsule_path\":\"examples/movement.capsule.json\"}}",
                 k);
        CHECK(call(s, tmp, &r) == 0);
        CHECK_OK(r);
        resp_free(&r);
    }
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"wc5\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"examples/world.json\",\"capsule_path\":\"examples/movement.capsule.json\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_RESOURCE_LIMIT);
    resp_free(&r);

    /* A snapshot of world-3 for the closed-world restore check below. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"sn3\",\"method\":\"world.snapshot\",\"params\":{\"world_id\":\"world-3\",\"expected_world_revision\":\"world-3.r1\"}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    snap3[0] = '\0';
    CHECK(str_copy(res != NULL ? su_json_member(res, "snapshot_id") : NULL, snap3, sizeof snap3));
    resp_free(&r);

    /* Stale revision: refused, and world-2 still works afterwards. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"cs-stale\",\"method\":\"world.close\",\"params\":{\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r9\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_STALE_REVISION);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"i-w2\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"world-2\",\"fields\":[\"position_x\"],\"limit\":1}}", &r) == 0);
    CHECK_OK(r);
    resp_free(&r);

    /* Close world-2: released counts; the id then fails as unknown everywhere. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"cl2\",\"method\":\"world.close\",\"params\":{\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r1\"}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    CHECK(res != NULL && str_is(su_json_member(res, "world_id"), "world-2"));
    {
        const SuJsonValue *closed = res != NULL ? su_json_member(res, "closed") : NULL;
        CHECK(closed != NULL && closed->type == SU_JSON_BOOL && closed->u.boolean == 1);
        CHECK(uint_of(su_json_member(res, "released") , "systems") == 1u);
        CHECK(uint_of(su_json_member(res, "released"), "snapshots") == 0u);
    }
    resp_free(&r);

    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"i-closed\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"world-2\",\"fields\":[\"position_x\"],\"limit\":1}}", &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"s-closed\",\"method\":\"world.step\",\"params\":{\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r1\",\"ticks\":1}}", &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"sn-closed\",\"method\":\"world.snapshot\",\"params\":{\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r1\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"rs-closed\",\"method\":\"world.restore\",\"params\":{\"world_id\":\"world-2\",\"snapshot_id\":\"0000\",\"expected_world_revision\":\"world-2.r1\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"in-closed\",\"method\":\"candidate.install\",\"params\":{\"candidate_id\":\"candidate-1\",\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r1\",\"allowed_effective_tick\":0}}", &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"cl2b\",\"method\":\"world.close\",\"params\":{\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r1\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);

    /* The slot is reusable, and the new id is not the closed one. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"wc-new\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"examples/world.json\",\"capsule_path\":\"examples/movement.capsule.json\"}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    CHECK(res != NULL && str_is(su_json_member(res, "world_id"), "world-5"));
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"wc-full\",\"method\":\"world.create\",\"params\":{\"fixture_path\":\"examples/world.json\",\"capsule_path\":\"examples/movement.capsule.json\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_RESOURCE_LIMIT);
    resp_free(&r);

    /* world-3 closes with its one snapshot released. Restoring that snapshot into world-4 fails. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"cl3\",\"method\":\"world.close\",\"params\":{\"world_id\":\"world-3\",\"expected_world_revision\":\"world-3.r1\"}}", &r) == 0);
    CHECK_OK(r);
    res = resp_result(&r);
    CHECK(uint_of(su_json_member(res, "released"), "snapshots") == 1u);
    resp_free(&r);
    snprintf(tmp, sizeof tmp,
             "{\"protocol\":\"0.1\",\"id\":\"rs-cross\",\"method\":\"world.restore\",\"params\":{\"world_id\":\"world-4\",\"snapshot_id\":\"%s\",\"expected_world_revision\":\"world-4.r1\"}}",
             snap3);
    CHECK(call(s, tmp, &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);

    /* The remaining worlds keep stepping. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"s-w5\",\"method\":\"world.step\",\"params\":{\"world_id\":\"world-5\",\"expected_world_revision\":\"world-5.r1\",\"ticks\":2}}", &r) == 0);
    CHECK_OK(r);
    resp_free(&r);

    /* Capabilities advertise world.close. */
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"caps\",\"method\":\"engine.info\",\"params\":{}}", &r) == 0);
    CHECK_OK(r);
    {
        const SuJsonValue *caps = su_json_member(resp_result(&r), "capabilities");
        int found = 0;
        size_t k;
        if (caps != NULL && caps->type == SU_JSON_ARRAY) {
            for (k = 0; k < caps->u.array.count; k++) {
                found = found || str_is(caps->u.array.items[k], "world.close");
            }
        }
        CHECK(found);
    }
    resp_free(&r);
}

/* Resident set (KiB) of a process: VmRSS from /proc on Linux, the working set on Windows. -1 when unavailable. */
static long rss_kib_of(pid_t pid) {
    return su_test_rss_kib((uint64_t)pid);
}

/* Sends one line to a bare worker process and parses the reply into r. */
static int wire_call(SuProc *p, const char *line, Resp *r) {
    const uint8_t *out = NULL;
    size_t len = 0;
    SuJsonLimits lim;
    SuJsonError jerr;
    uint32_t st;

    memset(r, 0, sizeof *r);
    st = su_proc_request(p, (const uint8_t *)line, strlen(line), su_now_ms() + 60000, &out, &len);
    if (st != SU_OK) {
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

/* internal.stats field, or UINT64_MAX when the call fails. */
static uint64_t worker_stat(SuProc *p, const char *key) {
    Resp r;
    uint64_t v = UINT64_MAX;
    if (wire_call(p, "{\"protocol\":\"0.1\",\"id\":\"st\",\"method\":\"internal.stats\",\"params\":{}}", &r) != 0) {
        resp_free(&r);
        return UINT64_MAX;
    }
    CHECK_OK(r);
    if (resp_ok(&r)) {
        v = uint_of(resp_result(&r), key);
    }
    resp_free(&r);
    return v;
}

/* Creates a world on a bare worker from absolute paths. Copies its id and revision. */
static int worker_create(SuProc *p, const char *id, const char *fixture, const char *capsule,
                         const char *source, char *wid, size_t wcap, char *rev, size_t rcap) {
    char line[2048];
    Resp r;
    int ok;
    snprintf(line, sizeof line,
             "{\"protocol\":\"0.1\",\"id\":\"%s\",\"method\":\"world.create\",\"params\":"
             "{\"fixture_path\":\"%s\",\"capsules\":[{\"capsule_path\":\"%s\",\"source_path\":\"%s\"}]}}",
             id, fixture, capsule, source);
    if (wire_call(p, line, &r) != 0) {
        resp_free(&r);
        return 0;
    }
    ok = resp_ok(&r) && str_copy(su_json_member(resp_result(&r), "world_id"), wid, wcap) &&
         str_copy(su_json_member(resp_result(&r), "world_revision"), rev, rcap);
    resp_free(&r);
    return ok;
}

/* Worker path: the world table and the checkpoint store and registry are checked through
 * internal.stats, so a close that skips a release fails here, not only in a reply. Ends with
 * the worker's own exit status: a leak reported by LeakSanitizer exits nonzero. */
static void test_world_close_worker(const char *helper, const char *root) {
    SuProc p;
    SuProcLimits lim;
    const char *argv[2];
    char fx[PATH_MAX_TEST + 32];
    char cap[PATH_MAX_TEST + 32];
    char src[PATH_MAX_TEST + 32];
    char wid[32];
    char rev[64];
    char snap_a[80];
    char snap_b[80];
    char line[2048];
    Resp r;
    long rss_warm = -1;
    long rss_end = -1;
    int i;

    snprintf(fx, sizeof fx, "%s/examples/world.json", root);
    snprintf(cap, sizeof cap, "%s/examples/movement.capsule.json", root);
    snprintf(src, sizeof src, "%s/examples/movement.lir.json", root);
    argv[0] = helper;
    argv[1] = NULL;
    memset(&lim, 0, sizeof lim);
#if defined(TEST_UNDER_ASAN)
    lim.address_space_bytes = 0;
#else
    lim.address_space_bytes = (uint64_t)4u * 1024u * 1024u * 1024u;
#endif
    memset(&p, 0, sizeof p);
    if (su_proc_spawn(&p, argv, &lim) != SU_OK) {
        CHECK(0);
        return;
    }

    CHECK(worker_stat(&p, "worlds") == 0u);
    CHECK(worker_create(&p, "k1", fx, cap, src, wid, sizeof wid, rev, sizeof rev));
    CHECK(strcmp(wid, "world-1") == 0);
    /* Four live worlds; the fifth is refused and consumes no id. */
    for (i = 2; i <= 4; i++) {
        char id[16];
        snprintf(id, sizeof id, "k%d", i);
        CHECK(worker_create(&p, id, fx, cap, src, wid, sizeof wid, rev, sizeof rev));
    }
    CHECK(worker_stat(&p, "worlds") == 4u);
    CHECK(worker_stat(&p, "registry_in_use") == 4u);
    CHECK(!worker_create(&p, "k5", fx, cap, src, wid, sizeof wid, rev, sizeof rev));
    CHECK(worker_stat(&p, "world_seq") == 4u);

    /* Snapshots of world-1 (at r1) and world-2 (after one tick). */
    snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"sa\",\"method\":\"world.snapshot\",\"params\":{\"world_id\":\"world-1\",\"expected_world_revision\":\"world-1.r1\"}}");
    CHECK(wire_call(&p, line, &r) == 0);
    CHECK_OK(r);
    CHECK(str_copy(su_json_member(resp_result(&r), "snapshot_id"), snap_a, sizeof snap_a));
    /* R4: capture_ns is an integer (monotonic nanoseconds of the capture), not null, as in the baseline. */
    CHECK(uint_of(resp_result(&r), "capture_ns") != UINT64_MAX);
    resp_free(&r);
    snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"st2\",\"method\":\"world.step\",\"params\":{\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r1\",\"ticks\":1}}");
    CHECK(wire_call(&p, line, &r) == 0);
    CHECK_OK(r);
    resp_free(&r);
    snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"sb\",\"method\":\"world.snapshot\",\"params\":{\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r2\"}}");
    CHECK(wire_call(&p, line, &r) == 0);
    CHECK_OK(r);
    CHECK(str_copy(su_json_member(resp_result(&r), "snapshot_id"), snap_b, sizeof snap_b));
    resp_free(&r);
    CHECK(worker_stat(&p, "checkpoints") == 2u);
    CHECK(worker_stat(&p, "checkpoint_records") == 2u);

    /* Stale close: refused, nothing released. */
    CHECK(wire_call(&p, "{\"protocol\":\"0.1\",\"id\":\"cs\",\"method\":\"world.close\",\"params\":{\"world_id\":\"world-1\",\"expected_world_revision\":\"world-1.r7\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_STALE_REVISION);
    resp_free(&r);
    CHECK(worker_stat(&p, "worlds") == 4u);
    CHECK(worker_stat(&p, "checkpoints") == 2u);

    /* Close world-1: its snapshot, its schedule entry and its registry record are released. */
    CHECK(wire_call(&p, "{\"protocol\":\"0.1\",\"id\":\"c1\",\"method\":\"world.close\",\"params\":{\"world_id\":\"world-1\",\"expected_world_revision\":\"world-1.r1\"}}", &r) == 0);
    CHECK_OK(r);
    CHECK(uint_of(su_json_member(resp_result(&r), "released"), "snapshots") == 1u);
    CHECK(uint_of(su_json_member(resp_result(&r), "released"), "systems") == 1u);
    resp_free(&r);
    CHECK(worker_stat(&p, "worlds") == 3u);
    CHECK(worker_stat(&p, "checkpoints") == 1u);
    CHECK(worker_stat(&p, "checkpoint_records") == 1u);
    CHECK(worker_stat(&p, "registry_in_use") == 3u);

    /* A snapshot of the closed world cannot be restored into a live one. */
    snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"rx\",\"method\":\"world.restore\",\"params\":{\"world_id\":\"world-3\",\"snapshot_id\":\"%s\",\"expected_world_revision\":\"world-3.r1\"}}", snap_a);
    CHECK(wire_call(&p, line, &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    /* The closed world's id is unknown to every method. */
    CHECK(wire_call(&p, "{\"protocol\":\"0.1\",\"id\":\"ri\",\"method\":\"world.inspect\",\"params\":{\"world_id\":\"world-1\",\"limit\":1}}", &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);
    CHECK(wire_call(&p, "{\"protocol\":\"0.1\",\"id\":\"rc\",\"method\":\"world.close\",\"params\":{\"world_id\":\"world-1\",\"expected_world_revision\":\"world-1.r1\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_INVALID_ARGUMENT);
    resp_free(&r);

    /* Close world-2 with its snapshot; the new world takes the next id, never a closed one. */
    CHECK(wire_call(&p, "{\"protocol\":\"0.1\",\"id\":\"c2\",\"method\":\"world.close\",\"params\":{\"world_id\":\"world-2\",\"expected_world_revision\":\"world-2.r2\"}}", &r) == 0);
    CHECK_OK(r);
    CHECK(uint_of(su_json_member(resp_result(&r), "released"), "snapshots") == 1u);
    resp_free(&r);
    CHECK(worker_stat(&p, "checkpoints") == 0u);
    CHECK(worker_create(&p, "k6", fx, cap, src, wid, sizeof wid, rev, sizeof rev));
    CHECK(strcmp(wid, "world-5") == 0);
    CHECK(worker_stat(&p, "registry_in_use") == 3u);

    /* 1,000 create, step, snapshot, close cycles. The worker's RSS must not grow with the
     * number of cycles, and every counter returns to the same value. */
    for (i = 0; i < 1000; i++) {
        char id[32];
        char w2[32];
        char r2[64];
        char snap[80];
        snprintf(id, sizeof id, "cyc%d", i);
        if (!worker_create(&p, id, fx, cap, src, w2, sizeof w2, r2, sizeof r2)) {
            CHECK(0);
            break;
        }
        snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"cs%d\",\"method\":\"world.step\",\"params\":{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\",\"ticks\":1}}", i, w2, r2);
        CHECK(wire_call(&p, line, &r) == 0);
        CHECK_OK(r);
        if (resp_ok(&r)) {
            if (!str_copy(su_json_member(resp_result(&r), "world_revision"), r2, sizeof r2)) {
                CHECK(0);
            }
        }
        resp_free(&r);
        snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"cn%d\",\"method\":\"world.snapshot\",\"params\":{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\"}}", i, w2, r2);
        CHECK(wire_call(&p, line, &r) == 0);
        CHECK_OK(r);
        if (!str_copy(su_json_member(resp_result(&r), "snapshot_id"), snap, sizeof snap)) {
            CHECK(0);
        }
        resp_free(&r);
        snprintf(line, sizeof line, "{\"protocol\":\"0.1\",\"id\":\"cc%d\",\"method\":\"world.close\",\"params\":{\"world_id\":\"%s\",\"expected_world_revision\":\"%s\"}}", i, w2, r2);
        CHECK(wire_call(&p, line, &r) == 0);
        CHECK_OK(r);
        CHECK(uint_of(su_json_member(resp_result(&r), "released"), "snapshots") == 1u);
        resp_free(&r);
        if (i == 99) {
            rss_warm = rss_kib_of(p.pid);
        }
    }
    rss_end = rss_kib_of(p.pid);
    CHECK(worker_stat(&p, "worlds") == 3u);
    CHECK(worker_stat(&p, "checkpoints") == 0u);
    CHECK(worker_stat(&p, "checkpoint_records") == 0u);
    CHECK(worker_stat(&p, "registry_in_use") == 3u);
    CHECK(worker_stat(&p, "world_seq") == 1005u); /* 5 creations, then 1,000 cycles: ids never repeat */
    CHECK(worker_stat(&p, "world_seq") != UINT64_MAX);
    if (rss_warm < 0 || rss_end < 0) {
        fprintf(stderr, "test_control: RSS not observable; RSS check NOT RUN\n");
        g_not_run++;
    } else {
        long growth = rss_end - rss_warm;
        fprintf(stderr, "test_control: worker RSS after 100 cycles %ld KiB, after 1000 cycles %ld KiB\n",
                rss_warm, rss_end);
        CHECK(growth <= 4096L);
    }

    /* Clean shutdown: EOF on stdin; the worker must exit 0 (LeakSanitizer exits nonzero). */
    close(p.in_fd);
    p.in_fd = -1;
    CHECK(su_test_proc_clean_exit(&p));
}

/* M2-00 audit MX21: entity worlds are released by world.close. 1,000 create/spawn/close cycles of an entity
 * world on one worker: after every close the worker holds no world, no entity runtime, no checkpoint and no
 * registry record, and the counters at the end equal the counters before the first cycle (flat). */
#define ENT_CLOSE_CYCLES 1000
static void test_entity_close_cycles(const char *helper, const char *root) {
    SuProc p;
    SuProcLimits lim;
    const char *argv[2];
    char fx[PATH_MAX_TEST + 128];
    char cap[PATH_MAX_TEST + 128];
    char src[PATH_MAX_TEST + 128];
    char wid[32];
    char rev[64];
    char id[24];
    char line[2048];
    Resp r;
    uint64_t base_worlds;
    uint64_t base_ent;
    uint64_t base_ck;
    uint64_t base_reg;
    uint64_t base_bytes;
    int i;

    snprintf(fx, sizeof fx, "%s/tests/fixtures/entities/actors_3.json", root);
    snprintf(cap, sizeof cap, "%s/tests/fixtures/entities/despawn_move.capsule.json", root);
    snprintf(src, sizeof src, "%s/tests/fixtures/entities/despawn_move.lir.json", root);
    argv[0] = helper;
    argv[1] = NULL;
    memset(&lim, 0, sizeof lim);
#if defined(TEST_UNDER_ASAN)
    lim.address_space_bytes = 0;
#else
    lim.address_space_bytes = (uint64_t)4u * 1024u * 1024u * 1024u;
#endif
    memset(&p, 0, sizeof p);
    if (su_proc_spawn(&p, argv, &lim) != SU_OK) {
        CHECK(0);
        return;
    }
    base_worlds = worker_stat(&p, "worlds");
    base_ent = worker_stat(&p, "entity_runtimes");
    base_ck = worker_stat(&p, "checkpoints");
    base_reg = worker_stat(&p, "registry_in_use");
    base_bytes = worker_stat(&p, "checkpoint_bytes");
    CHECK(base_worlds == 0u && base_ent == 0u);

    for (i = 0; i < ENT_CLOSE_CYCLES; i++) {
        uint64_t got_ent;
        snprintf(id, sizeof id, "ec%d", i);
        if (!worker_create(&p, id, fx, cap, src, wid, sizeof wid, rev, sizeof rev)) {
            CHECK(0);
            break;
        }
        if (i == 0) {
            CHECK(worker_stat(&p, "entity_runtimes") == 1u);
        }
        /* One despawn of fixture row 0 (handle slot 0, generation 1), then the close at the revision it
         * returned. The fixture has no free slot, so spawning is refused by design. */
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"es\",\"method\":\"world.despawn\",\"params\":{\"world_id\":\"%s\","
                 "\"expected_world_revision\":\"%s\",\"entity\":4294967296}}",
                 wid, rev);
        CHECK(wire_call(&p, line, &r) == 0);
        CHECK_OK(r);
        if (resp_ok(&r)) {
            CHECK(str_copy(su_json_member(resp_result(&r), "world_revision"), rev, sizeof rev));
        }
        resp_free(&r);
        snprintf(line, sizeof line,
                 "{\"protocol\":\"0.1\",\"id\":\"ecl\",\"method\":\"world.close\",\"params\":{\"world_id\":\"%s\","
                 "\"expected_world_revision\":\"%s\"}}",
                 wid, rev);
        CHECK(wire_call(&p, line, &r) == 0);
        CHECK_OK(r);
        CHECK(uint_of(su_json_member(resp_result(&r), "released"), "systems") == 1u);
        resp_free(&r);
        got_ent = worker_stat(&p, "entity_runtimes");
        CHECK(got_ent == base_ent);
        CHECK(worker_stat(&p, "worlds") == base_worlds);
        if (i == 0 || i == ENT_CLOSE_CYCLES - 1) {
            CHECK(worker_stat(&p, "checkpoints") == base_ck);
            CHECK(worker_stat(&p, "registry_in_use") == base_reg);
        }
    }
    /* Flat at the end: the same counters as before the first cycle; the worker minted one id per cycle. */
    CHECK(worker_stat(&p, "worlds") == base_worlds);
    CHECK(worker_stat(&p, "entity_runtimes") == base_ent);
    CHECK(worker_stat(&p, "checkpoints") == base_ck);
    CHECK(worker_stat(&p, "checkpoint_bytes") == base_bytes);
    CHECK(worker_stat(&p, "registry_in_use") == base_reg);
    CHECK(worker_stat(&p, "world_seq") == (uint64_t)ENT_CLOSE_CYCLES);

    /* Clean shutdown: EOF on stdin; the worker exits 0 (LeakSanitizer exits nonzero). */
    close(p.in_fd);
    p.in_fd = -1;
    CHECK(su_test_proc_clean_exit(&p));
}

int main(int argc, char **argv) {
    static const char *const fallback_root = ".";
    const char *helper;
    const char *root;
    const char *scratch;
    char cwd_buf[4096];
    char root_abs[PATH_MAX_TEST];
    char fault_file[PATH_MAX_TEST + 32];
    char scratch_dir[PATH_MAX_TEST];
    const char *tmpdir;
    SuAgentConfig cfg;
    const char *helper_argv[2];
    Session s;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <worker_helper_path> [project_root]\n", argv[0]);
        return 2;
    }
    helper = argv[1];
#if defined(TEST_UNDER_ASAN)
    /* The worker processes are ASan builds too. A 256 MB allocator quarantine would hide RSS
     * behaviour, so the quarantine is capped for the children (set only when unset). */
    setenv("ASAN_OPTIONS", "quarantine_size_mb=1", 0);
#endif
    root = argc >= 3 ? argv[2] : fallback_root;
    if (argc < 3 && getcwd(cwd_buf, sizeof cwd_buf) != NULL) {
        root = cwd_buf;
    }
    /* Symlink targets are built from root, so root must be absolute (a relative root made them dangle). */
    if (su_path_canonical_root(root, root_abs, sizeof root_abs) != SU_OK) {
        fprintf(stderr, "project root %s could not be canonicalized\n", root);
        return 2;
    }
    root = root_abs;
    tmpdir = getenv("TMPDIR");
    scratch = getenv("SU_TEST_SCRATCH");
    if (scratch == NULL) {
        scratch = tmpdir != NULL ? tmpdir : su_test_scratch_base();
    }
    snprintf(scratch_dir, sizeof scratch_dir, "%s/sutekh-control-%ld", scratch, (long)getpid());
    if (mkdir(scratch_dir, 0700) != 0 && errno != EEXIST) {
        perror("mkdir scratch");
        return 2;
    }
    snprintf(fault_file, sizeof fault_file, "%s/fault", scratch_dir);
    write_text(fault_file, "");
    setenv("SU_TEST_FAULT_FILE", fault_file, 1);
#if defined(_WIN32)
    snprintf(g_outside, sizeof g_outside, "%s", fault_file);
#endif

    helper_argv[0] = helper;
    helper_argv[1] = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.project_root = root;
    cfg.worker_argv = helper_argv;
    cfg.request_timeout_ms = 10000;
#if defined(TEST_UNDER_ASAN)
    cfg.worker_address_space_bytes = 0; /* ASan reserves a large address range */
#else
    cfg.worker_address_space_bytes = (uint64_t)4u * 1024u * 1024u * 1024u;
#endif

    s = session_start(&cfg);
    test_protocol_errors(&s);
    test_path_policy(&s);
    test_world_sequence(&s, fault_file);
    test_world_close_supervisor(&s);
    test_source_path(&s, &cfg, scratch_dir, root);
    session_stop(&s);
    test_world_close_worker(helper, root);
    test_entity_close_cycles(helper, root);

    test_symlink_escape(&cfg, scratch_dir, root);

    unlink(fault_file);
    rmdir(scratch_dir);
    printf("test_control: %d checks, %d failures, %d not run\n", g_checks, g_failures, g_not_run);
    return g_failures == 0 ? 0 : 1;
}
