#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "agent/su_path.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sutekh/status.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

uint32_t su_path_canonical_root(const char *root, char *out, size_t cap) {
    char real[PATH_MAX];
    if (root == NULL || realpath(root, real) == NULL) {
        return SU_E_IO;
    }
    if (strlen(real) >= cap) {
        return SU_E_RESOURCE_LIMIT;
    }
    strcpy(out, real);
    return SU_OK;
}

static int has_dotdot(const char *p) {
    const char *seg = p;
    for (;;) {
        const char *end = strchr(seg, '/');
        size_t n = end != NULL ? (size_t)(end - seg) : strlen(seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            return 1;
        }
        if (end == NULL) {
            return 0;
        }
        seg = end + 1;
    }
}

uint32_t su_path_resolve(const char *root_real, const char *input, char *out, size_t cap) {
    char cand[SU_PATH_BUF];
    char real[PATH_MAX];
    size_t rl;
    int n;

    if (input == NULL || input[0] == '\0') {
        return SU_E_CAPABILITY_DENIED;
    }
    if (strlen(input) >= sizeof cand - 2u) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (has_dotdot(input)) {
        return SU_E_CAPABILITY_DENIED;
    }
    if (input[0] == '/') {
        n = snprintf(cand, sizeof cand, "%s", input);
    } else {
        n = snprintf(cand, sizeof cand, "%s/%s", root_real, input);
    }
    if (n < 0 || (size_t)n >= sizeof cand) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (realpath(cand, real) == NULL) {
        return errno == ENAMETOOLONG ? SU_E_RESOURCE_LIMIT : SU_E_IO;
    }
    rl = strlen(root_real);
    if (!(rl == 1u && root_real[0] == '/')) {
        if (strncmp(real, root_real, rl) != 0 || (real[rl] != '\0' && real[rl] != '/')) {
            return SU_E_CAPABILITY_DENIED;
        }
    }
    if (strlen(real) >= cap) {
        return SU_E_RESOURCE_LIMIT;
    }
    strcpy(out, real);
    return SU_OK;
}

uint32_t su_path_sibling(const char *root_real, const char *resolved_file, const char *name,
                         char *out, size_t cap) {
    char joined[PATH_MAX];
    const char *slash;
    size_t dir_len;
    int n;

    if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return SU_E_CAPABILITY_DENIED;
    }
    slash = strrchr(resolved_file, '/');
    if (slash == NULL) {
        return SU_E_CAPABILITY_DENIED;
    }
    dir_len = (size_t)(slash - resolved_file);
    if (dir_len == 0u) {
        n = snprintf(joined, sizeof joined, "/%s", name);
    } else {
        n = snprintf(joined, sizeof joined, "%.*s/%s", (int)dir_len, resolved_file, name);
    }
    if (n < 0 || (size_t)n >= sizeof joined) {
        return SU_E_RESOURCE_LIMIT;
    }
    return su_path_resolve(root_real, joined, out, cap);
}

uint32_t su_path_read_file(const char *path, size_t max_bytes, uint8_t **data, size_t *len) {
    struct stat st;
    uint8_t *buf;
    size_t want;
    size_t got = 0;
    uint8_t probe;
    int fd;

    *data = NULL;
    *len = 0;
    /* O_NONBLOCK keeps open() from blocking on a FIFO before the S_ISREG check rejects it. */
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return SU_E_IO;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return SU_E_IO;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)max_bytes) {
        close(fd);
        return SU_E_RESOURCE_LIMIT;
    }
    want = (size_t)st.st_size;
    buf = malloc(want + 1u);
    if (buf == NULL) {
        close(fd);
        return SU_E_RESOURCE_LIMIT;
    }
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            close(fd);
            return SU_E_IO;
        }
        got += (size_t)r;
    }
    /* The file may have grown since fstat; refuse rather than truncate. */
    for (;;) {
        ssize_t r = read(fd, &probe, 1);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r != 0) {
            free(buf);
            close(fd);
            return r < 0 ? SU_E_IO : SU_E_RESOURCE_LIMIT;
        }
        break;
    }
    close(fd);
    buf[want] = '\0';
    *data = buf;
    *len = want;
    return SU_OK;
}
