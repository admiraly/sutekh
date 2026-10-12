/* RTS host-operator record check (M3-04; contract 10.7 T-HOST-05: the M3-04 production descriptors equal the
 * M3-04a host records). The records are loaded with su_host_load, the same check world.create applies.
 *
 * Two checks per record:
 *   - "except constants_hash": the record's constants_hash is replaced by the descriptor's hash in a scratch copy;
 *     every other field (system_id, operator_version, fields, schema, schedule, numeric profile) must agree.
 *   - "committed": the record as committed. The two committed constants_hash values were replaced with the hashes of the
 *     constants texts this tree implements (owner-accepted resolution of M3-04 blocker B1; the earlier M3-04a
 *     values 0d634621... and c1ad001c... had no recorded constants text).
 *
 * Usage: test_rts_records <repo-root> <scratch-dir>   (CTest runs it from the repository root)
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "profiles/rts/native/rts_ops.h"
#include "runtime/rts/host_decl.h"
#include "sutekh/status.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_failures++;                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
        }                                                       \
    } while (0)

static int read_file(const char *path, char **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    b = malloc((size_t)n + 1u);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return 0;
    }
    b[n] = '\0';
    fclose(f);
    *out = b;
    *len = (size_t)n;
    return 1;
}

static void hex32(const uint8_t h[32], char out[65]) {
    static const char d[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[2 * i] = d[h[i] >> 4];
        out[2 * i + 1] = d[h[i] & 15];
    }
    out[64] = '\0';
}

/* Loads the record at path against desc (su_host_load). When override_hash is set, the record's constants_hash is
 * replaced by the descriptor's hash in a scratch copy first. */
static uint32_t load_record(const char *path, const SuHostOpDesc *desc, const char *scratch, int override_hash,
                            char *msg, size_t msg_cap) {
    char *text = NULL;
    size_t len = 0;
    SuLirModule *m = NULL;
    uint32_t sv = 0;
    uint32_t gen = 0;
    uint32_t st;
    const char *use = path;
    char new_hash[65];
    char outpath[512];
    if (!read_file(path, &text, &len)) {
        snprintf(msg, msg_cap, "cannot read %s", path);
        return SU_E_INVALID_ARGUMENT;
    }
    hex32(desc->constants_hash, new_hash);
    if (override_hash) {
        const char *key = "\"constants_hash\": \"";
        char *p = strstr(text, key);
        FILE *o;
        if (p == NULL) {
            free(text);
            return SU_E_INVALID_ARGUMENT;
        }
        p += strlen(key);
        memcpy(p, new_hash, 64);
        snprintf(outpath, sizeof outpath, "%s/override_%s.capsule.json", scratch, desc->system_id);
        o = fopen(outpath, "wb");
        if (o == NULL) {
            free(text);
            return SU_E_INVALID_ARGUMENT;
        }
        fwrite(text, 1, len, o);
        fclose(o);
        use = outpath;
    }
    st = su_host_load(use, NULL, desc, &m, &sv, &gen, msg, msg_cap);
    if (st == SU_OK) {
        su_host_decl_free(m);
    }
    free(text);
    return st;
}

static void test_records(const char *root, const char *scratch) {
    char msg[256];
    char path[512];
    uint32_t st;
    snprintf(path, sizeof path, "%s/capsules/rts/host/spatial.capsule.json", root);
    st = load_record(path, &g_rts_spatial_desc, scratch, 1, msg, sizeof msg);
    CHECK(st == SU_OK, "spatial record equals the descriptor except constants_hash (%u: %s)", st, msg);
    snprintf(path, sizeof path, "%s/capsules/rts/host/scatter.capsule.json", root);
    st = load_record(path, &g_rts_scatter_desc, scratch, 1, msg, sizeof msg);
    CHECK(st == SU_OK, "scatter record equals the descriptor except constants_hash (%u: %s)", st, msg);
    /* The records as committed (BLOCKER, see the M3-04 handoff). */
    snprintf(path, sizeof path, "%s/capsules/rts/host/spatial.capsule.json", root);
    st = load_record(path, &g_rts_spatial_desc, scratch, 0, msg, sizeof msg);
    CHECK(st == SU_OK, "committed spatial record loads against the descriptor (%u: %s)", st, msg);
    snprintf(path, sizeof path, "%s/capsules/rts/host/scatter.capsule.json", root);
    st = load_record(path, &g_rts_scatter_desc, scratch, 0, msg, sizeof msg);
    CHECK(st == SU_OK, "committed scatter record loads against the descriptor (%u: %s)", st, msg);
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    const char *scratch = argc > 2 ? argv[2] : ".";
    test_records(root, scratch);
    printf("test_rts_records: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
