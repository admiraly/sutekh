/* C07 containment tests (M1 closure C): what a disposable child and the active worker actually
 * enforce. Each check reads a fact the CHILD reports about itself (its own getrlimit read-back and
 * its own /proc/self/status and /proc/self/limits, and the result of socket/write/open calls it
 * makes), not a value the supervisor assumed.
 *
 * Usage: test_containment <recovery_worker> <scratch_dir>
 *   recovery_worker  tests/recovery/recovery_worker built with -DSU_TEST_FAULT_HOOKS (the containment
 *                    probe `internal.test.containment` exists only there)
 *   scratch_dir      writable directory for the probe's file
 *
 * Case A: a child with the disposable limits (RLIMIT_FSIZE 1 MiB, RLIMIT_NOFILE 64, network denial).
 * Case B: the same binary with no limits and no network denial. Case B is the control: it proves the
 * probe really creates sockets, writes past 1 MiB and opens more than 64 descriptors when nothing
 * stops it, so a passing case A cannot come from a probe that always reports success. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>

#include "agent/su_proc.h"
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

#define MIB (1024ull * 1024ull)
#define GIB (1024ull * 1024ull * 1024ull)

static const char g_probe_req[] =
    "{\"protocol\":\"" SU_PROTOCOL_VERSION "\",\"id\":\"c1\","
    "\"method\":\"internal.test.containment\",\"params\":{}}";

/* Spawns the worker with the given limits, sends the probe, returns the child's answer in doc. */
typedef struct Probe {
    SuProc p;
    uint8_t *resp_copy;
    SuJsonDoc doc;
    const SuJsonValue *result;
    int answered;
} Probe;

static void probe_run(Probe *pr, const char *worker, const SuProcLimits *lim) {
    const char *argv[] = {worker, "worker", NULL};
    const uint8_t *resp = NULL;
    size_t len = 0;
    SuJsonLimits jl;
    SuJsonError err;
    uint32_t st;

    memset(pr, 0, sizeof *pr);
    st = su_proc_spawn(&pr->p, argv, lim);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        return;
    }
    st = su_proc_request(&pr->p, (const uint8_t *)g_probe_req, sizeof g_probe_req - 1u,
                         su_now_ms() + 10000, &resp, &len);
    if (st != SU_OK || resp == NULL) {
        /* A child killed by a signal (for example SIGXFSZ) or a refused syscall shows here. */
        CHECK(0);
        su_proc_kill(&pr->p);
        return;
    }
    pr->resp_copy = malloc(len + 1u);
    if (pr->resp_copy == NULL) {
        CHECK(0);
        su_proc_kill(&pr->p);
        return;
    }
    memcpy(pr->resp_copy, resp, len);
    jl.max_bytes = len;
    jl.max_depth = 8;
    st = su_json_parse(pr->resp_copy, len, &jl, &pr->doc, &err);
    CHECK(st == SU_OK);
    if (st == SU_OK && pr->doc.root != NULL) {
        pr->result = su_json_member(pr->doc.root, "result");
        pr->answered = pr->result != NULL && pr->result->type == SU_JSON_OBJECT;
    }
    CHECK(pr->answered);
    su_proc_finish(&pr->p, 1000);
}

static void probe_free(Probe *pr) {
    su_json_doc_free(&pr->doc);
    free(pr->resp_copy);
}

static uint64_t u64_of(const SuJsonValue *r, const char *key, uint64_t fallback) {
    uint64_t v = fallback;
    const SuJsonValue *m = r != NULL ? su_json_member(r, key) : NULL;
    if (m == NULL || !su_json_get_uint(m, &v)) {
        return fallback;
    }
    return v;
}

/* The string value of key, copied into dst (NUL-terminated). Returns 0 when absent or not a string. */
static int str_of(const SuJsonValue *r, const char *key, char *dst, size_t cap) {
    const SuJsonValue *m = r != NULL ? su_json_member(r, key) : NULL;
    if (m == NULL || !su_json_is_string(m)) {
        dst[0] = '\0';
        return 0;
    }
    {
        size_t n = m->u.string.len < cap - 1u ? m->u.string.len : cap - 1u;
        memcpy(dst, m->u.string.data, n);
        dst[n] = '\0';
    }
    return 1;
}

static void probe_free_all(Probe *pr) {
    probe_free(pr);
}

/* Soft limit of one /proc/self/limits row (the value after the key, before the hard limit). Returns
 * UINT64_MAX for "unlimited" and UINT64_MAX - 1 when the row is missing or unreadable. */
static uint64_t limit_soft(const char *text, const char *key) {
    const char *p = strstr(text, key);
    char *end = NULL;
    unsigned long long v;
    if (p == NULL) {
        return UINT64_MAX - 1u;
    }
    p += strlen(key);
    while (*p == ' ') {
        p++;
    }
    if (strncmp(p, "unlimited", 9) == 0) {
        return UINT64_MAX;
    }
    v = strtoull(p, &end, 10);
    if (end == p) {
        return UINT64_MAX - 1u;
    }
    return (uint64_t)v;
}

/* Case A: the disposable limits. */
static void case_contained(const char *worker) {
    Probe pr;
    SuProcLimits lim;
    char seccomp[64];
    char limits_text[8192];
    const SuJsonValue *r;
    /* Under AddressSanitizer the engine runs its workers with SUTEKH_WORKER_AS_BYTES=0 (no address-space
     * bound: the sanitizer reserves a large range). The probe then checks that no bound was set. */
    const char *asenv = getenv("SUTEKH_WORKER_AS_BYTES");
    int as_unbounded = asenv != NULL && strcmp(asenv, "0") == 0;
    uint64_t want_as = as_unbounded ? SU_PROC_UNLIMITED : 4u * GIB;

    memset(&lim, 0, sizeof lim);
    lim.address_space_bytes = as_unbounded ? 0u : 4u * GIB;
    lim.cpu_seconds = 12u;
    lim.file_bytes = MIB;
    lim.nofile = 64u;
    lim.deny_network = 1;
    probe_run(&pr, worker, &lim);
    CHECK(pr.p.containment.reported == 1);
    CHECK(pr.p.containment.rlimit_as == want_as);
    CHECK(pr.p.containment.rlimit_cpu == 12u);
    CHECK(pr.p.containment.rlimit_fsize == MIB);
    CHECK(pr.p.containment.rlimit_nofile == 64u);
#if defined(__linux__) && defined(__x86_64__)
    CHECK(pr.p.containment.net_mode == SU_PROC_NET_SECCOMP);
#endif
    CHECK(pr.p.containment.reason[0] != '\0');
    if (pr.answered) {
        r = pr.result;
        /* socket(AF_INET, SOCK_STREAM) must fail with EPERM (errno 1) and create nothing. */
        CHECK(u64_of(r, "socket_errno", 0) == 1u);
        /* writing past RLIMIT_FSIZE fails with EFBIG (27) after exactly 1 MiB. */
        CHECK(u64_of(r, "bytes_written", 0) == MIB);
        CHECK(u64_of(r, "write_errno", 0) == 27u);
        /* descriptors stop at RLIMIT_NOFILE with EMFILE (24). */
        CHECK(u64_of(r, "open_errno", 0) == 24u);
        CHECK(u64_of(r, "fds_opened", 0) + 3u <= 64u);
        CHECK(u64_of(r, "fds_opened", 0) + 3u >= 60u);
        /* the child's own status: Seccomp mode 2 (filter). */
        CHECK(str_of(r, "seccomp_line", seccomp, sizeof seccomp) == 1);
        CHECK(strcmp(seccomp, "Seccomp:\t2") == 0);
        /* the child's own limits: the file size and open files limits are the ones set. */
        CHECK(str_of(r, "limits", limits_text, sizeof limits_text) == 1);
        CHECK(limit_soft(limits_text, "Max file size") == MIB);
        CHECK(limit_soft(limits_text, "Max open files") == 64u);
        CHECK(limit_soft(limits_text, "Max core file size") == 0u);
        CHECK(limit_soft(limits_text, "Max address space") == want_as);
    }
    probe_free_all(&pr);
}

/* Case B: the control. Nothing is limited, so the probe must see the unlimited behaviour. */
static void case_control(const char *worker) {
    Probe pr;
    SuProcLimits lim;
    char seccomp[64];
    const SuJsonValue *r;

    memset(&lim, 0, sizeof lim);
    lim.deny_network = 0;
    probe_run(&pr, worker, &lim);
    CHECK(pr.p.containment.reported == 1);
    CHECK(pr.p.containment.net_mode == SU_PROC_NET_NONE);
    if (pr.answered) {
        r = pr.result;
        CHECK(u64_of(r, "socket_errno", 99) == 0u);
        CHECK(str_of(r, "seccomp_line", seccomp, sizeof seccomp) == 1);
        CHECK(strcmp(seccomp, "Seccomp:\t0") == 0);
        CHECK(u64_of(r, "bytes_written", 0) == 128u * 64u * 1024u);
        CHECK(u64_of(r, "write_errno", 99) == 0u);
        CHECK(u64_of(r, "fds_opened", 0) == 256u);
    }
    probe_free_all(&pr);
}

int main(int argc, char **argv) {
    const char *worker;
    const char *scratch;
    if (argc != 3) {
        fprintf(stderr, "usage: %s <recovery_worker> <scratch_dir>\n", argv[0]);
        return 2;
    }
    worker = argv[1];
    scratch = argv[2];
    if (setenv("SU_TEST_CONTAINMENT_DIR", scratch, 1) != 0) {
        fprintf(stderr, "cannot set SU_TEST_CONTAINMENT_DIR\n");
        return 1;
    }
    case_contained(worker);
    case_control(worker);
    printf("test_containment: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
