/* Test-only worker executable. It is the real worker entry (su_worker_main_with_hooks)
 * with two fault seams, both driven by files named in environment variables:
 *
 * SU_TEST_FAULT_FILE (M0-05), read at the start of every internal.candidate.evaluate:
 *   hang  -> never answers (exercises the supervisor's wall-clock limit)
 *   kill  -> a helper child SIGKILLs this process 150 ms later, mid-evaluation
 *
 * SU_TEST_SYSTEM_FAULT_FILE (M0-07), read before every system of every tick: the file
 * holds one decimal schedule index. A system at that index fails with
 * SU_E_CANDIDATE_FAILED after earlier systems of the same tick have run, so the tick
 * is discarded. Missing, empty or non-numeric content injects nothing.
 *
 * Requests that are not evaluations or ticks never reach the hooks. */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "runtime/worker.h"

static void fault_hook(void) {
    const char *path = getenv("SU_TEST_FAULT_FILE");
    char mode[16];
    ssize_t n = 0;
    int fd;

    if (path == NULL) {
        return;
    }
    memset(mode, 0, sizeof mode);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    n = read(fd, mode, sizeof mode - 1);
    close(fd);
    if (n <= 0) {
        return;
    }
    if (strncmp(mode, "hang", 4) == 0) {
        for (;;) {
            pause();
        }
    }
    if (strncmp(mode, "kill", 4) == 0) {
        pid_t me = getpid();
        pid_t k = fork();
        if (k == 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 150000000L;
            nanosleep(&ts, NULL);
            kill(me, SIGKILL);
            _exit(0);
        }
        for (;;) {
            pause();
        }
    }
}

static int system_fault(unsigned idx) {
    const char *path = getenv("SU_TEST_SYSTEM_FAULT_FILE");
    char buf[16];
    ssize_t n;
    int fd;

    if (path == NULL) {
        return 0;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    memset(buf, 0, sizeof buf);
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0 || buf[0] < '0' || buf[0] > '9') {
        return 0;
    }
    return strtoul(buf, NULL, 10) == idx;
}

int main(void) {
    SuWorkerHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.before_eval = fault_hook;
    hooks.fail_system = system_fault;
    return su_worker_main_with_hooks(0, 1, &hooks);
}
