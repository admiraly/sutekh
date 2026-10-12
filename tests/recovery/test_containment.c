/* C07 containment tests (M1 closure D): what the children actually enforce. Each check reads a fact the
 * CHILD reports or a result the kernel gives the child (its own getrlimit read-back, /proc/self/status and
 * /proc/self/limits, socket, io_uring, write and open results, and how it ended). No value is assumed.
 *
 * Usage: test_containment <recovery_worker> <scratch_dir> <project_root>
 *   recovery_worker  tests/recovery/recovery_worker built with -DSU_TEST_FAULT_HOOKS (the probes
 *                    `internal.test.*` exist only there)
 *   scratch_dir      writable directory for the probes' files
 *   project_root     a directory that must NOT be writable by a disposable child
 *
 * The same binary also runs as a child (`test_containment --child <kind>`): a process with only the
 * network filter (no worker self-sandbox), so the arch and x32 checks prove the network filter alone.
 *
 * Every contained case has a control: the same probe with nothing denied must show the unrestricted
 * behaviour (socket created, io_uring set up, i386 and x32 calls returning, execve reaching the kernel),
 * so a passing contained case cannot come from a probe that always reports success. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agent/su_proc.h"
#include "agent/su_proto.h"
#include "agent/su_sandbox.h"
#include "ir/su_json.h"
#include "sutekh/status.h"

static long g_checks = 0;
static long g_fail = 0;
static long g_unavailable = 0;
static char g_exe[4096];

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
#define EXIT_CALL_SUCCEEDED 100

/* ---- child mode: one raw call per process, so the filter's verdict is the process's fate ---------- */

#if defined(__x86_64__)
static long raw_i386_socket(void) {
    long ret = -1;
    /* int 0x80 is the i386 ABI: eax 359 is socket(2) on i386, arguments in ebx, ecx, edx. */
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(359L), "b"(2L), "c"(1L), "d"(0L) : "memory");
    return ret;
}
static long raw_x32_socket(void) {
    /* The x32 ABI number of socket: the x32 bit (0x40000000) or'd into 41. */
    return syscall(0x40000000L | 41L, 2L, 1L, 0L);
}
#endif

/* Child: performs one call and exits with EXIT_CALL_SUCCEEDED when it succeeded, or with the errno
 * (1..99) it failed with. A killed child shows the signal instead. */
static int child_main(const char *kind) {
    long r = -1000;
    int err = 0;
    if (strcmp(kind, "socket") == 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            close(fd);
            r = 0;
        } else {
            err = errno;
        }
    } else if (strcmp(kind, "socketpair") == 0) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            close(sv[0]);
            close(sv[1]);
            r = 0;
        } else {
            err = errno;
        }
#if defined(__x86_64__)
    } else if (strcmp(kind, "i386") == 0) {
        r = raw_i386_socket();
        if (r >= 0) {
            close((int)r);
            r = 0;
        } else {
            err = (int)-r;
        }
    } else if (strcmp(kind, "x32") == 0) {
        r = raw_x32_socket();
        if (r >= 0) {
            close((int)r);
            r = 0;
        } else {
            err = errno;
        }
#endif
    } else {
        return 2;
    }
    if (r == 0) {
        return EXIT_CALL_SUCCEEDED;
    }
    return err > 0 && err < 100 ? err : 99;
}

/* ---- parent helpers ------------------------------------------------------------------------------- */

/* Runs `test_containment --child kind` under lim and waits for it. Returns 1 when it was started. */
static int run_child(const char *kind, const SuProcLimits *lim, int *wstatus) {
    SuProc p;
    const char *argv[] = {g_exe, "--child", kind, NULL};
    uint32_t st;
    pid_t r;
    int status = 0;
    *wstatus = 0;
    st = su_proc_spawn(&p, argv, lim);
    if (st != SU_OK) {
        return 0;
    }
    do {
        r = waitpid(p.pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    p.pid = 0;
    close(p.in_fd);
    close(p.out_fd);
    su_line_reader_free(&p.reader);
    *wstatus = status;
    return r > 0;
}

static const char *const g_probe_fmt =
    "{\"protocol\":\"" SU_PROTOCOL_VERSION "\",\"id\":\"c1\",\"method\":\"%s\",\"params\":{}}";

typedef struct Probe {
    SuProc p;
    uint8_t *resp_copy;
    SuJsonDoc doc;
    const SuJsonValue *result;
    int answered;
} Probe;

/* Spawns the worker with lim, sends one probe method, copies the answer into pr. */
static void probe_run(Probe *pr, const char *worker, const SuProcLimits *lim, const char *method) {
    const char *argv[] = {worker, "worker", NULL};
    const uint8_t *resp = NULL;
    size_t len = 0;
    SuJsonLimits jl;
    SuJsonError err;
    uint32_t st;
    char req[256];

    memset(pr, 0, sizeof *pr);
    st = su_proc_spawn(&pr->p, argv, lim);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        return;
    }
    snprintf(req, sizeof req, g_probe_fmt, method);
    st = su_proc_request(&pr->p, (const uint8_t *)req, strlen(req), su_now_ms() + 10000, &resp, &len);
    if (st != SU_OK || resp == NULL) {
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

/* The string value of key (of object r), copied into dst (NUL-terminated). Returns 0 when absent or not a string. */
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

/* Soft limit of one /proc/self/limits row. UINT64_MAX means unlimited; UINT64_MAX - 1 means missing. */
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

static int as_unbounded(void) {
    const char *asenv = getenv("SUTEKH_WORKER_AS_BYTES");
    return asenv != NULL && strcmp(asenv, "0") == 0;
}

/* The disposable limits used by the engine, plus the test's writable scratch directory. */
static void disposable_limits(SuProcLimits *lim, const char *scratch) {
    memset(lim, 0, sizeof *lim);
    lim->address_space_bytes = as_unbounded() ? 0u : 4u * GIB;
    lim->cpu_seconds = 12u;
    lim->file_bytes = MIB;
    lim->nofile = 64u;
    lim->deny_network = 1;
    lim->deny_fs_writes = 1;
    lim->writable_dir = scratch;
}

/* ---- cases ---------------------------------------------------------------------------------------- */

/* Case A: a disposable worker. Network (socket, socketpair, io_uring), limits, Seccomp mode and filter
 * count, file creation outside the scratch directory. */
static void case_contained(const char *worker, const char *scratch, const char *project) {
    Probe pr;
    SuProcLimits lim;
    char seccomp[64];
    char filters[64];
    char limits_text[8192];
    const SuJsonValue *r;
    uint64_t want_as = as_unbounded() ? UINT64_MAX : 4u * GIB;
    char path[600];

    disposable_limits(&lim, scratch);
    probe_run(&pr, worker, &lim, "internal.test.containment");
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
        /* socket(AF_INET, SOCK_STREAM) and socketpair(AF_UNIX) must fail with EPERM (1). */
        CHECK(u64_of(r, "socket_errno", 0) == 1u);
        CHECK(u64_of(r, "socketpair_errno", 0) == 1u);
        /* io_uring_setup must fail with EPERM: io_uring is the route to IORING_OP_SOCKET (C07 bypass). */
        CHECK(u64_of(r, "uring_setup_errno", 0) == 1u);
        /* writing past RLIMIT_FSIZE fails with EFBIG (27) after exactly 1 MiB, in the writable scratch dir. */
        CHECK(u64_of(r, "bytes_written", 0) == MIB);
        CHECK(u64_of(r, "write_errno", 0) == 27u);
        /* descriptors stop at RLIMIT_NOFILE with EMFILE (24). */
        CHECK(u64_of(r, "open_errno", 0) == 24u);
        CHECK(u64_of(r, "fds_opened", 0) + 3u <= 64u);
        CHECK(u64_of(r, "fds_opened", 0) + 3u >= 60u);
        /* seccomp: mode 2 (filter) and two filters (network, then the worker self-sandbox). */
        CHECK(str_of(r, "seccomp_line", seccomp, sizeof seccomp) == 1);
        CHECK(strcmp(seccomp, "Seccomp:\t2") == 0);
        CHECK(str_of(r, "seccomp_filters_line", filters, sizeof filters) == 1);
        CHECK(strcmp(filters, "Seccomp_filters:\t2") == 0);
        CHECK(str_of(r, "limits", limits_text, sizeof limits_text) == 1);
        CHECK(limit_soft(limits_text, "Max file size") == MIB);
        CHECK(limit_soft(limits_text, "Max open files") == 64u);
        CHECK(limit_soft(limits_text, "Max core file size") == 0u);
        CHECK(limit_soft(limits_text, "Max address space") == want_as);
    }
    probe_free(&pr);

    /* Landlock: file creation outside the scratch directory is denied with EACCES (13), inside it allowed. */
    if (pr.p.containment.fs_mode == SU_PROC_FS_LANDLOCK) {
        probe_run(&pr, worker, &lim, "internal.test.fs");
        if (pr.answered) {
            CHECK(u64_of(pr.result, "project_errno", 0) == 13u);
            CHECK(u64_of(pr.result, "scratch_errno", 99) == 0u);
        }
        probe_free(&pr);
    } else {
        printf("test_containment: landlock NOT enforced here: %s\n", pr.p.containment.fs_reason);
        g_unavailable++;
    }
    (void)path;
    (void)project;
}

/* Case B: the control. Nothing is limited. The worker still runs its own self-sandbox (process creation
 * is denied in every worker), but no network filter and no Landlock: socket, socketpair and io_uring work,
 * and project-root creation succeeds. */
static void case_control(const char *worker, const char *scratch, const char *project) {
    Probe pr;
    SuProcLimits lim;
    char seccomp[64];
    char filters[64];
    const SuJsonValue *r;

    memset(&lim, 0, sizeof lim);
    lim.deny_network = 0;
    probe_run(&pr, worker, &lim, "internal.test.containment");
    CHECK(pr.p.containment.net_mode == SU_PROC_NET_NONE);
    if (pr.answered) {
        r = pr.result;
        CHECK(u64_of(r, "socket_errno", 99) == 0u);
        CHECK(u64_of(r, "socketpair_errno", 99) == 0u);
        /* positive control for io_uring: set-up succeeds and IORING_OP_SOCKET opens a socket. */
        CHECK(u64_of(r, "uring_setup_errno", 99) == 0u);
        /* IORING_OP_SOCKET completed with a socket descriptor (a small non-negative fd), not an error. */
        CHECK(u64_of(r, "uring_socket_result", 999) != 999u);
        CHECK(u64_of(r, "uring_socket_result", 999) < 4096u);
        CHECK(str_of(r, "seccomp_filters_line", filters, sizeof filters) == 1);
        CHECK(strcmp(filters, "Seccomp_filters:\t1") == 0);
        CHECK(str_of(r, "seccomp_line", seccomp, sizeof seccomp) == 1);
        CHECK(strcmp(seccomp, "Seccomp:\t2") == 0);
        CHECK(u64_of(r, "bytes_written", 0) == 128u * 64u * 1024u);
        CHECK(u64_of(r, "write_errno", 99) == 0u);
        CHECK(u64_of(r, "fds_opened", 0) == 256u);
    }
    probe_free(&pr);

    /* file creation in the project root and in the scratch directory works without Landlock. */
    if (project != NULL) {
        SuProcLimits none;
        Probe pf;
        memset(&none, 0, sizeof none);
        none.deny_network = 0;
        probe_run(&pf, worker, &none, "internal.test.fs");
        if (pf.answered) {
            CHECK(u64_of(pf.result, "project_errno", 99) == 0u);
            CHECK(u64_of(pf.result, "tmp_errno", 99) == 0u);
        }
        probe_free(&pf);
    }
    (void)scratch;
}

/* Case C: the worker self-sandbox. Under the worker's own filter, execve and fork return EPERM; the state is
 * reported by internal.stats. With SUTEKH_WORKER_SANDBOX=0 (sanitizer builds only) the state is opted_out
 * and the probe is skipped: execve of /bin/true would replace the worker. */
static void case_self_sandbox(const char *worker) {
    Probe pr;
    SuProcLimits none;
    const char *env = getenv("SUTEKH_WORKER_SANDBOX");
    int opted_out = env != NULL && strcmp(env, "0") == 0;
    char state[32];
    memset(&none, 0, sizeof none);
    none.deny_network = 0;
    probe_run(&pr, worker, &none, "internal.stats");
    if (pr.answered) {
        const SuJsonValue *sb = su_json_member(pr.result, "self_sandbox");
        CHECK(sb != NULL);
        if (sb != NULL) {
            CHECK(str_of(sb, "state", state, sizeof state) == 1);
            CHECK(strcmp(state, opted_out ? "opted_out" : "on") == 0);
        }
    }
    probe_free(&pr);
    if (opted_out) {
        printf("test_containment: self-sandbox opted out (sanitizer build); execve and fork probe skipped\n");
        g_unavailable++;
        return;
    }
    probe_run(&pr, worker, &none, "internal.test.sandbox");
    if (pr.answered) {
        CHECK(u64_of(pr.result, "execve_errno", 0) == 1u);
        CHECK(u64_of(pr.result, "fork_errno", 0) == 1u);
    }
    probe_free(&pr);
}

/* Case D: the network filter alone, through the arch and x32 checks and socketpair, each in its own child. */
static void case_filter_alone(void) {
    SuProcLimits lim;
    int ws = 0;
    memset(&lim, 0, sizeof lim);
    lim.deny_network = 1;
    /* foreign arch (i386 int 0x80) is killed (SIGSYS) by the filter */
    CHECK(run_child("i386", &lim, &ws) == 1);
    CHECK(WIFSIGNALED(ws) && WTERMSIG(ws) == SIGSYS);
    /* x32 socket is killed (SIGSYS) by the filter */
    CHECK(run_child("x32", &lim, &ws) == 1);
    CHECK(WIFSIGNALED(ws) && WTERMSIG(ws) == SIGSYS);
    /* socketpair is denied with EPERM */
    CHECK(run_child("socketpair", &lim, &ws) == 1);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 1);
    /* socket is denied with EPERM (control of the same mechanism) */
    CHECK(run_child("socket", &lim, &ws) == 1);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 1);
}

/* Case E: positive controls in unfiltered children. The same calls must succeed without the filter, so
 * the contained results above are the filter's doing. A host without the i386 or x32 ABI reports
 * unavailable for that call instead of passing. */
static void case_controls_unfiltered(void) {
    SuProcLimits none;
    int ws = 0;
    memset(&none, 0, sizeof none);
    CHECK(run_child("socketpair", &none, &ws) == 1);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == EXIT_CALL_SUCCEEDED);
    CHECK(run_child("socket", &none, &ws) == 1);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == EXIT_CALL_SUCCEEDED);
    if (run_child("i386", &none, &ws) == 1 && WIFEXITED(ws) && WEXITSTATUS(ws) == EXIT_CALL_SUCCEEDED) {
        /* i386 call ran: the arch kill above is the filter's doing */
    } else {
        printf("test_containment: i386 ABI call not available on this host; arch kill not contrasted\n");
        g_unavailable++;
    }
    if (run_child("x32", &none, &ws) == 1 && WIFEXITED(ws) && WEXITSTATUS(ws) == EXIT_CALL_SUCCEEDED) {
        /* x32 call ran */
    } else {
        printf("test_containment: x32 ABI call not available on this host; x32 kill not contrasted\n");
        g_unavailable++;
    }
    /* the test process itself: execve of a missing path reaches the kernel (ENOENT, not EPERM) and fork works */
    {
        char *argv[] = {NULL};
        char *envp[] = {NULL};
        pid_t pid;
        errno = 0;
        (void)execve("/nonexistent/sutekh-control-probe", argv, envp);
        CHECK(errno == ENOENT);
        pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            _exit(0);
        }
        if (pid > 0) {
            int st = 0;
            CHECK(waitpid(pid, &st, 0) == pid);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        }
    }
}

int main(int argc, char **argv) {
    const char *worker;
    const char *scratch;
    const char *project;
    ssize_t n;
    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        return child_main(argv[2]);
    }
    if (argc != 4) {
        fprintf(stderr, "usage: %s <recovery_worker> <scratch_dir> <project_root>\n", argv[0]);
        return 2;
    }
    worker = argv[1];
    scratch = argv[2];
    project = argv[3];
    n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1u);
    if (n <= 0) {
        fprintf(stderr, "cannot resolve /proc/self/exe\n");
        return 1;
    }
    g_exe[n] = '\0';
    if (setenv("SU_TEST_CONTAINMENT_DIR", scratch, 1) != 0 || setenv("SU_TEST_FS_PROJECT", project, 1) != 0 ||
        setenv("SU_TEST_FS_TMP", "/tmp", 1) != 0) {
        fprintf(stderr, "cannot set the probe environment\n");
        return 1;
    }
    case_contained(worker, scratch, project);
    case_control(worker, scratch, project);
    case_self_sandbox(worker);
    case_filter_alone();
    case_controls_unfiltered();
    printf("test_containment: %ld checks, %ld failures, %ld unavailable (not enforced or not on this host)\n",
           g_checks, g_fail, g_unavailable);
    return g_fail == 0 ? 0 : 1;
}
