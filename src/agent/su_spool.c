#define _POSIX_C_SOURCE 200809L

#include "agent/su_spool.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ir/su_sha256.h"

typedef struct SpoolEntry {
    int used;
    char sha[65];
    char ext[SU_SPOOL_EXT_MAX + 1u];
    uint64_t bytes;
} SpoolEntry;

struct SuSpool {
    char dir[4096];
    int owns_dir;
    uint64_t max_bytes;
    uint64_t bytes;
    uint32_t count;
    SpoolEntry entries[SU_SPOOL_MAX_ENTRIES];
};

static int valid_ext(const char *ext) {
    size_t i;
    size_t n = ext != NULL ? strlen(ext) : 0;
    if (n == 0 || n > SU_SPOOL_EXT_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (ext[i] < 'a' || ext[i] > 'z') {
            return 0;
        }
    }
    return 1;
}

static int valid_sha(const char *sha) {
    size_t i;
    if (sha == NULL || strlen(sha) != 64) {
        return 0;
    }
    for (i = 0; i < 64; i++) {
        char c = sha[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static SpoolEntry *find_entry(SuSpool *sp, const char *sha, const char *ext) {
    uint32_t i;
    for (i = 0; i < SU_SPOOL_MAX_ENTRIES; i++) {
        SpoolEntry *e = &sp->entries[i];
        if (e->used && strcmp(e->sha, sha) == 0 && strcmp(e->ext, ext) == 0) {
            return e;
        }
    }
    return NULL;
}

static uint32_t entry_path(const SuSpool *sp, const char *sha, const char *ext, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s.%s", sp->dir, sha, ext);
    return n > 0 && (size_t)n < cap ? SU_OK : SU_E_RESOURCE_LIMIT;
}

uint32_t su_spool_open(const char *dir, uint64_t max_bytes, SuSpool **out) {
    SuSpool *sp;
    const char *tmp;
    char tmpl[4096];
    int n;

    *out = NULL;
    if (max_bytes == 0) {
        return SU_E_INVALID_ARGUMENT;
    }
    sp = calloc(1, sizeof *sp);
    if (sp == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    sp->max_bytes = max_bytes;
    if (dir != NULL) {
        struct stat st;
        if (strlen(dir) >= sizeof sp->dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            free(sp);
            return SU_E_IO;
        }
        /* The spool holds captured capsules and checkpoints: it must be ours and not group/world
         * accessible (the private mkdtemp directory below is created 0700). */
        if (st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
            free(sp);
            return SU_E_CAPABILITY_DENIED;
        }
        snprintf(sp->dir, sizeof sp->dir, "%s", dir);
        sp->owns_dir = 0;
    } else {
        tmp = getenv("TMPDIR");
        if (tmp == NULL || tmp[0] == '\0') {
            tmp = "/tmp";
        }
        n = snprintf(tmpl, sizeof tmpl, "%s/sutekh-spool-XXXXXX", tmp);
        if (n <= 0 || (size_t)n >= sizeof tmpl || mkdtemp(tmpl) == NULL) {
            free(sp);
            return SU_E_IO;
        }
        snprintf(sp->dir, sizeof sp->dir, "%s", tmpl);
        sp->owns_dir = 1;
    }
    *out = sp;
    return SU_OK;
}

void su_spool_close(SuSpool *sp) {
    uint32_t i;
    if (sp == NULL) {
        return;
    }
    for (i = 0; i < SU_SPOOL_MAX_ENTRIES; i++) {
        if (sp->entries[i].used) {
            char path[4200];
            if (entry_path(sp, sp->entries[i].sha, sp->entries[i].ext, path, sizeof path) == SU_OK) {
                unlink(path);
            }
        }
    }
    if (sp->owns_dir) {
        rmdir(sp->dir);
    }
    free(sp);
}

static SpoolEntry *new_entry(SuSpool *sp) {
    uint32_t i;
    for (i = 0; i < SU_SPOOL_MAX_ENTRIES; i++) {
        if (!sp->entries[i].used) {
            memset(&sp->entries[i], 0, sizeof sp->entries[i]);
            sp->entries[i].used = 1;
            sp->count++;
            return &sp->entries[i];
        }
    }
    return NULL;
}

uint32_t su_spool_put(SuSpool *sp, const char *ext, const uint8_t *data, size_t len, char sha_out[65]) {
    SuSha256 ctx;
    uint8_t dig[32];
    char sha[65];
    char path[4200];
    size_t off = 0;
    SpoolEntry *e;
    int fd;

    if (sp == NULL || !valid_ext(ext) || (data == NULL && len > 0)) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_sha256_init(&ctx);
    su_sha256_update(&ctx, data, len);
    su_sha256_final(&ctx, dig);
    su_sha256_hex(dig, sha);
    snprintf(sha_out, 65, "%s", sha);
    e = find_entry(sp, sha, ext);
    if (e != NULL) {
        return SU_OK;
    }
    if ((uint64_t)len > sp->max_bytes - sp->bytes) {
        return SU_E_RESOURCE_LIMIT;
    }
    e = new_entry(sp);
    if (e == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (entry_path(sp, sha, ext, path, sizeof path) != SU_OK) {
        sp->count--;
        e->used = 0;
        return SU_E_RESOURCE_LIMIT;
    }
    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        sp->count--;
        e->used = 0;
        return SU_E_IO;
    }
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            close(fd);
            unlink(path);
            sp->count--;
            e->used = 0;
            return SU_E_IO;
        }
        off += (size_t)w;
    }
    if (close(fd) != 0) {
        unlink(path);
        sp->count--;
        e->used = 0;
        return SU_E_IO;
    }
    snprintf(e->sha, sizeof e->sha, "%s", sha);
    snprintf(e->ext, sizeof e->ext, "%s", ext);
    e->bytes = (uint64_t)len;
    sp->bytes += (uint64_t)len;
    return SU_OK;
}

uint32_t su_spool_adopt(SuSpool *sp, const char *ext, const char *src, const char *sha, uint64_t bytes) {
    char path[4200];
    struct stat st;
    SpoolEntry *e;

    if (sp == NULL || !valid_ext(ext) || !valid_sha(sha) || src == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (entry_path(sp, sha, ext, path, sizeof path) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (find_entry(sp, sha, ext) != NULL) {
        unlink(src);
        return SU_OK;
    }
    if (stat(src, &st) != 0 || (uint64_t)st.st_size != bytes) {
        unlink(src);
        return SU_E_IO;
    }
    if (bytes > sp->max_bytes - sp->bytes) {
        unlink(src);
        return SU_E_RESOURCE_LIMIT;
    }
    e = new_entry(sp);
    if (e == NULL) {
        unlink(src);
        return SU_E_RESOURCE_LIMIT;
    }
    unlink(path);
    if (rename(src, path) != 0) {
        unlink(src);
        sp->count--;
        e->used = 0;
        return SU_E_IO;
    }
    snprintf(e->sha, sizeof e->sha, "%s", sha);
    snprintf(e->ext, sizeof e->ext, "%s", ext);
    e->bytes = bytes;
    sp->bytes += bytes;
    return SU_OK;
}

uint32_t su_spool_path(const SuSpool *sp, const char *sha, const char *ext, char *out, size_t cap) {
    if (sp == NULL || !valid_sha(sha) || !valid_ext(ext)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (find_entry((SuSpool *)sp, sha, ext) == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return entry_path(sp, sha, ext, out, cap);
}

int su_spool_has(const SuSpool *sp, const char *sha, const char *ext) {
    if (sp == NULL || !valid_sha(sha) || !valid_ext(ext)) {
        return 0;
    }
    return find_entry((SuSpool *)sp, sha, ext) != NULL;
}

uint32_t su_spool_drop(SuSpool *sp, const char *sha, const char *ext) {
    char path[4200];
    SpoolEntry *e;
    if (sp == NULL || !valid_sha(sha) || !valid_ext(ext)) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = find_entry(sp, sha, ext);
    if (e == NULL) {
        return SU_OK;
    }
    if (entry_path(sp, sha, ext, path, sizeof path) == SU_OK) {
        unlink(path);
    }
    sp->bytes -= e->bytes;
    sp->count--;
    e->used = 0;
    return SU_OK;
}

uint64_t su_spool_bytes(const SuSpool *sp) {
    return sp != NULL ? sp->bytes : 0;
}

uint64_t su_spool_max_bytes(const SuSpool *sp) {
    return sp != NULL ? sp->max_bytes : 0;
}

uint32_t su_spool_count(const SuSpool *sp) {
    return sp != NULL ? sp->count : 0;
}

const char *su_spool_entry(const SuSpool *sp, uint32_t i, const char **ext, uint64_t *bytes) {
    uint32_t seen = 0;
    uint32_t k;
    if (sp == NULL) {
        return NULL;
    }
    for (k = 0; k < SU_SPOOL_MAX_ENTRIES; k++) {
        if (sp->entries[k].used) {
            if (seen == i) {
                if (ext != NULL) {
                    *ext = sp->entries[k].ext;
                }
                if (bytes != NULL) {
                    *bytes = sp->entries[k].bytes;
                }
                return sp->entries[k].sha;
            }
            seen++;
        }
    }
    return NULL;
}

uint32_t su_spool_scratch_path(const SuSpool *sp, const char *tag, char *out, size_t cap) {
    int n;
    if (sp == NULL || tag == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    n = snprintf(out, cap, "%s/scratch-%s-%ld.tmp", sp->dir, tag, (long)getpid());
    return n > 0 && (size_t)n < cap ? SU_OK : SU_E_RESOURCE_LIMIT;
}
