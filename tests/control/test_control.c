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
#include <sys/wait.h>
#include <unistd.h>

#include "agent/su_agent.h"
#include "agent/su_proto.h"
#include "ir/su_json.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"

static int g_checks = 0;
static int g_failures = 0;

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
        if (write(fd, text, strlen(text)) < 0) {
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
    resp_free(&r);
}

static void test_path_policy(Session *s) {
    Resp r;
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"p1\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"../../../../etc/passwd\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    resp_free(&r);
    CHECK(call(s, "{\"protocol\":\"0.1\",\"id\":\"p2\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"/etc/hostname\"}}", &r) == 0);
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
    CHECK(symlink(target, link_path) == 0);
    cfg.project_root = scratch;
    s = session_start(&cfg);
    CHECK(call(&s, "{\"protocol\":\"0.1\",\"id\":\"sym1\",\"method\":\"capsule.submit\",\"params\":{\"capsule_path\":\"escape.json\"}}", &r) == 0);
    CHECK_CODE(r, SU_E_CAPABILITY_DENIED);
    resp_free(&r);
    session_stop(&s);
    unlink(link_path);
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
    CHECK(pid_after != 0 && kill((pid_t)pid_after, 0) == 0);
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

int main(int argc, char **argv) {
    static const char *const fallback_root = ".";
    const char *helper;
    const char *root;
    const char *scratch;
    char cwd_buf[4096];
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
    root = argc >= 3 ? argv[2] : fallback_root;
    if (argc < 3 && getcwd(cwd_buf, sizeof cwd_buf) != NULL) {
        root = cwd_buf;
    }
    tmpdir = getenv("TMPDIR");
    scratch = getenv("SU_TEST_SCRATCH");
    if (scratch == NULL) {
        scratch = tmpdir != NULL ? tmpdir : "/tmp";
    }
    snprintf(scratch_dir, sizeof scratch_dir, "%s/sutekh-control-%ld", scratch, (long)getpid());
    if (mkdir(scratch_dir, 0700) != 0 && errno != EEXIST) {
        perror("mkdir scratch");
        return 2;
    }
    snprintf(fault_file, sizeof fault_file, "%s/fault", scratch_dir);
    write_text(fault_file, "");
    setenv("SU_TEST_FAULT_FILE", fault_file, 1);

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
    session_stop(&s);

    test_symlink_escape(&cfg, scratch_dir, root);

    unlink(fault_file);
    rmdir(scratch_dir);
    printf("test_control: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
