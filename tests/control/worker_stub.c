/* Test-only worker executable. It is the real worker entry (su_worker_main_with_hook)
 * with a fault hook that runs at the start of every internal.candidate.evaluate
 * request. The hook reads one word from the file named by the environment
 * variable SU_TEST_FAULT_FILE (unset or empty file means no fault):
 *   hang  -> never answers (exercises the supervisor's wall-clock limit)
 *   kill  -> a helper child SIGKILLs this process 150 ms later, mid-evaluation
 * Requests that are not evaluations (world.*) never reach the hook. */
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

int main(void) {
    return su_worker_main_with_hook(0, 1, fault_hook);
}
