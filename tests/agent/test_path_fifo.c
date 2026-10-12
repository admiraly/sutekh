/* Regression: su_path_read_file must not block on a FIFO inside the project root.
 *
 * su_path_resolve accepts a FIFO (its realpath is inside the root). su_path_read_file
 * used to call open(O_RDONLY) before its S_ISREG check, which blocks until a writer
 * appears. The test arms alarm(2) so a regression fails instead of hanging.
 *
 * Build: cc -std=c17 -Wall -Wextra -Werror -pedantic -Iinclude -Isrc
 *        src/agent/su_path.c tests/agent/test_path_fifo.c -o <scratch>/test_path_fifo
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "agent/su_path.h"
#include "sutekh/status.h"

static int failures = 0;

static void on_hang(int sig) {
    (void)sig;
    static const char msg[] = "test_path_fifo: su_path_read_file blocked on a FIFO\n";
    (void)!write(STDERR_FILENO, msg, sizeof msg - 1u);
    _exit(1);
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

int main(void) {
    char tmpl[] = "/tmp/su_path_fifo_XXXXXX";
    char root[PATH_MAX];
    char fifo_path[PATH_MAX + 16];
    char resolved[PATH_MAX];
    uint8_t *data = NULL;
    size_t len = 0;
    uint32_t rc;

    if (mkdtemp(tmpl) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    if (realpath(tmpl, root) == NULL) {
        perror("realpath");
        return 1;
    }
    snprintf(fifo_path, sizeof fifo_path, "%s/cap.fifo", root);
    CHECK(mkfifo(fifo_path, 0600) == 0);

    rc = su_path_resolve(root, "cap.fifo", resolved, sizeof resolved);
    CHECK(rc == SU_OK);
    if (rc == SU_OK) {
        /* With the bug, this open() blocks forever; SIGALRM turns the hang into a failure. */
        signal(SIGALRM, on_hang);
        alarm(5);
        rc = su_path_read_file(resolved, 1024u, &data, &len);
        alarm(0);
        CHECK(rc == SU_E_IO);
        CHECK(data == NULL);
        CHECK(len == 0);
    }

    unlink(fifo_path);
    rmdir(root);
    if (failures != 0) {
        fprintf(stderr, "test_path_fifo: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_path_fifo: all checks passed\n");
    return 0;
}
