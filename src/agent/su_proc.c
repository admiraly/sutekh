#define _POSIX_C_SOURCE 200809L

#include "agent/su_proc.h"

#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sutekh/status.h"

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void reap(pid_t pid) {
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

/* Non-blocking check for a child that has exited (killed, crashed or returned). An exited child
 * is reaped and its pipes are closed, as su_proc_kill does. Returns 1 when the child is gone. */
int su_proc_exited(SuProc *p) {
    int status;
    pid_t r;
    if (!p->alive || p->pid <= 0) {
        return 1;
    }
    do {
        r = waitpid(p->pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == 0) {
        return 0;
    }
    /* Already reaped by waitpid: close the pipes only, never signal a pid that may be reused. */
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    su_line_reader_free(&p->reader);
    p->pid = 0;
    p->alive = 0;
    return 1;
}


/* Children (workers, disposable admission workers) get a scrubbed environment: only test seams
 * (SU_TEST_*), engine settings (SUTEKH_*) and sanitizer options are passed on. Everything else,
 * including credentials or tokens in the supervisor's environment, is dropped (constitution C07). */
extern char **environ;
static int env_allowed(const char *kv) {
    static const char *const prefixes[] = {"SU_TEST_", "SUTEKH_", "ASAN_OPTIONS=", "UBSAN_OPTIONS=",
                                           "LSAN_OPTIONS=", "MSAN_OPTIONS=", "TSAN_OPTIONS=", NULL};
    for (size_t i = 0; prefixes[i] != NULL; i++) {
        if (strncmp(kv, prefixes[i], strlen(prefixes[i])) == 0) {
            return 1;
        }
    }
    return 0;
}
static char **scrubbed_env(void) {
    size_t n = 0;
    char **out;
    size_t k = 0;
    if (environ != NULL) {
        while (environ[n] != NULL) {
            n++;
        }
    }
    out = calloc(n + 1u, sizeof *out);
    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (env_allowed(environ[i])) {
            out[k++] = environ[i];
        }
    }
    out[k] = NULL;
    return out;
}

/* Records how a failed child ended. A child that is already gone (crash, exit) is reaped here and
 * its status kept; a child still running at grace_until_ms is left for su_proc_kill, and is
 * recorded as killed by the supervisor. */
static void end_record(SuProc *p, int64_t grace_until_ms) {
    p->end_kind = SU_PROC_END_SUPERVISOR;
    p->exit_code = 0;
    p->term_signal = 0;
    while (p->pid > 0) {
        int status = 0;
        pid_t r = waitpid(p->pid, &status, WNOHANG);
        if (r == p->pid) {
            if (WIFSIGNALED(status)) {
                p->end_kind = SU_PROC_END_SIGNALED;
                p->term_signal = WTERMSIG(status);
            } else if (WIFEXITED(status)) {
                p->end_kind = SU_PROC_END_EXITED;
                p->exit_code = WEXITSTATUS(status);
            }
            p->pid = 0;
            return;
        }
        if (r < 0 && errno != EINTR) {
            return;
        }
        if (su_now_ms() >= grace_until_ms) {
            return;
        }
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 2000000L;
            nanosleep(&ts, NULL);
        }
    }
}

static void end_fail(SuProc *p, int64_t deadline_ms) {
    int64_t now = su_now_ms();
    /* A pipe that closed before the deadline means the child is exiting: give it a second to be
     * reaped so that its status is known. At the deadline there is no grace. */
    end_record(p, now < deadline_ms ? now + 1000 : now);
    su_proc_kill(p);
}

void su_proc_kill(SuProc *p) {
    if (p->pid > 0) {
        kill(p->pid, SIGKILL);
        reap(p->pid);
    }
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    su_line_reader_free(&p->reader);
    p->pid = 0;
    p->alive = 0;
}

uint32_t su_proc_spawn(SuProc *p, const char *const *argv, const SuProcLimits *limits) {
    int pin[2] = {-1, -1};
    int pout[2] = {-1, -1};
    size_t argc = 0;
    pid_t pid;

    p->pid = 0;
    p->in_fd = -1;
    p->out_fd = -1;
    p->alive = 0;
    p->end_kind = SU_PROC_END_NONE;
    p->exit_code = 0;
    p->term_signal = 0;
    memset(&p->reader, 0, sizeof p->reader);

    if (argv == NULL || argv[0] == NULL || strchr(argv[0], '/') == NULL) {
        return SU_E_WORKER_FAILED;
    }
    while (argv[argc] != NULL) {
        argc++;
        if (argc > SU_PROC_MAX_ARGS) {
            return SU_E_WORKER_FAILED;
        }
    }
    if (argc == 0 || access(argv[0], X_OK) != 0) {
        return SU_E_WORKER_FAILED;
    }
    if (pipe(pin) != 0) {
        return SU_E_WORKER_FAILED;
    }
    if (pipe(pout) != 0) {
        close(pin[0]);
        close(pin[1]);
        return SU_E_WORKER_FAILED;
    }
    /* Parent-side ends and every pipe end are close-on-exec, so a later child
     * does not inherit another worker's pipes. dup2 in the child clears it. */
    fcntl(pin[0], F_SETFD, FD_CLOEXEC);
    fcntl(pin[1], F_SETFD, FD_CLOEXEC);
    fcntl(pout[0], F_SETFD, FD_CLOEXEC);
    fcntl(pout[1], F_SETFD, FD_CLOEXEC);

    char **child_env = scrubbed_env();
    if (child_env == NULL) {
        close(pin[0]);
        close(pin[1]);
        close(pout[0]);
        close(pout[1]);
        return SU_E_RESOURCE_LIMIT;
    }
    pid = fork();
    if (pid < 0) {
        free(child_env);
        close(pin[0]);
        close(pin[1]);
        close(pout[0]);
        close(pout[1]);
        return SU_E_WORKER_FAILED;
    }
    if (pid == 0) {
        /* Child: only async-signal-safe calls until exec. */
        int a = fcntl(pin[0], F_DUPFD, 10);
        int b = fcntl(pout[1], F_DUPFD, 10);
        if (a < 0 || b < 0) {
            _exit(127);
        }
        if (dup2(a, 0) < 0 || dup2(b, 1) < 0) {
            _exit(127);
        }
        /* The worker must hold only stdin/stdout: close every other pipe end so it never keeps
         * its own stdin writer or the supervisor's read end alive (EOF would never arrive). */
        close(a);
        close(b);
        if (pin[0] > 2) close(pin[0]);
        if (pin[1] > 2) close(pin[1]);
        if (pout[0] > 2) close(pout[0]);
        if (pout[1] > 2) close(pout[1]);
        if (limits != NULL && limits->address_space_bytes > 0) {
            struct rlimit rl;
            rl.rlim_cur = (rlim_t)limits->address_space_bytes;
            rl.rlim_max = (rlim_t)limits->address_space_bytes;
            setrlimit(RLIMIT_AS, &rl);
        }
        if (limits != NULL && limits->cpu_seconds > 0) {
            struct rlimit rl;
            rl.rlim_cur = (rlim_t)limits->cpu_seconds;
            rl.rlim_max = (rlim_t)limits->cpu_seconds + 1u;
            setrlimit(RLIMIT_CPU, &rl);
        }
        execve(argv[0], (char *const *)argv, child_env);
        _exit(127);
    }
    free(child_env);

    close(pin[0]);
    close(pout[1]);
    p->pid = pid;
    p->in_fd = pin[1];
    p->out_fd = pout[0];
    p->alive = 1;
    su_line_reader_init(&p->reader, p->out_fd, -1);
    return SU_OK;
}

uint32_t su_proc_request(SuProc *p, const uint8_t *req, size_t req_len, int64_t deadline_ms,
                         const uint8_t **resp, size_t *resp_len) {
    const uint8_t *line = NULL;
    size_t len = 0;
    int overflow = 0;
    int rc;
    uint8_t *framed;

    *resp = NULL;
    *resp_len = 0;
    if (!p->alive) {
        return SU_E_WORKER_FAILED;
    }
    if (req_len == SIZE_MAX) {
        su_proc_kill(p);
        return SU_E_RESOURCE_LIMIT;
    }
    framed = malloc(req_len + 1u);
    if (framed == NULL) {
        su_proc_kill(p);
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(framed, req, req_len);
    framed[req_len] = '\n';
    rc = su_write_all(p->in_fd, framed, req_len + 1u, deadline_ms);
    free(framed);
    if (rc != 0) {
        end_fail(p, deadline_ms);
        return SU_E_WORKER_FAILED;
    }

    p->reader.deadline_ms = deadline_ms;
    rc = su_line_next(&p->reader, &line, &len, &overflow);
    if (rc != 1) {
        end_fail(p, deadline_ms);
        return SU_E_WORKER_FAILED;
    }
    if (overflow) {
        p->end_kind = SU_PROC_END_SUPERVISOR;
        su_proc_kill(p);
        return SU_E_RESOURCE_LIMIT;
    }
    *resp = line;
    *resp_len = len;
    return SU_OK;
}

void su_proc_finish(SuProc *p, int64_t grace_ms) {
    int64_t until;
    if (p->pid <= 0) {
        return;
    }
    close_fd(&p->in_fd);
    until = su_now_ms() + grace_ms;
    for (;;) {
        int status;
        pid_t r = waitpid(p->pid, &status, WNOHANG);
        if (r == p->pid) {
            p->pid = 0;
            break;
        }
        if (r < 0 && errno != EINTR) {
            p->pid = 0;
            break;
        }
        if (su_now_ms() >= until) {
            break;
        }
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 5000000L;
            nanosleep(&ts, NULL);
        }
    }
    su_proc_kill(p);
}
