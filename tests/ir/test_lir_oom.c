/* Regression: su_lir_verify must not dereference a NULL document root when the JSON
 * parser reports an allocation failure (SU_E_INTERNAL "out of memory").
 *
 * Built with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc so the test can fail the
 * Nth allocation made by su_json.c and su_lir.c. For every N, the verifier must return
 * a status (SU_OK or an SU_E_* code) and must leave *out zeroed on failure. Before the
 * fix, the INTERNAL parse result fell through to check_capsule(NULL), which crashed.
 *
 * Run from the repository root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/status.h"
#include "ir/su_lir.h"

static size_t g_alloc_count = 0;
static size_t g_fail_at = 0;

#ifdef SU_LIR_OOM_SEAM
/* Windows: the IR objects are compiled with tests/ir/lir_oom_seam.h instead of linked with --wrap; this
 * file is not, so its calls below are the real allocator. */
static void *__real_malloc(size_t n) {
    return malloc(n);
}
static void *__real_calloc(size_t n, size_t m) {
    return calloc(n, m);
}
static void *__real_realloc(void *p, size_t n) {
    return realloc(p, n);
}
#else
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
#endif

static int should_fail(void) {
    g_alloc_count++;
    return g_fail_at != 0 && g_alloc_count == g_fail_at;
}

void *__wrap_malloc(size_t n) {
    if (should_fail()) {
        return NULL;
    }
    return __real_malloc(n);
}

void *__wrap_calloc(size_t n, size_t m) {
    if (should_fail()) {
        return NULL;
    }
    return __real_calloc(n, m);
}

void *__wrap_realloc(void *p, size_t n) {
    if (should_fail()) {
        return NULL;
    }
    return __real_realloc(p, n);
}

static int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                             \
        }                                                                           \
    } while (0)

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *buf;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = __real_malloc((size_t)sz + 1u);
    if (buf == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

int main(void) {
    uint8_t *cap, *lir;
    size_t cap_len = 0, lir_len = 0;
    size_t n;
    int reached_success = 0;

    cap = read_all("examples/movement.capsule.json", &cap_len);
    lir = read_all("examples/movement.lir.json", &lir_len);
    if (cap == NULL || lir == NULL) {
        fprintf(stderr, "test_lir_oom: cannot read fixtures\n");
        return 1;
    }

    /* Sweep the Nth allocation failure until a run completes without any failure fired. */
    for (n = 1; n < 100000u; n++) {
        SuLirModule m;
        SuDiag d;
        uint32_t rc;
        int fired;

        memset(&m, 0, sizeof m);
        su_diag_init(&d);
        g_alloc_count = 0;
        g_fail_at = n;
        rc = su_lir_verify(cap, cap_len, lir, lir_len, &m, &d);
        g_fail_at = 0;
        fired = g_alloc_count >= n;

        if (rc == SU_OK) {
            su_lir_module_free(&m);
            if (!fired) {
                reached_success = 1;
                break;
            }
        } else {
            CHECK(rc == SU_E_INTERNAL || rc == SU_E_RESOURCE_LIMIT || rc == SU_E_PARSE ||
                  rc == SU_E_TYPE || rc == SU_E_DUPLICATE_KEY);
            CHECK(m.system_id == NULL && m.fields == NULL && m.field_count == 0u);
        }
    }
    CHECK(reached_success);

    free(cap);
    free(lir);
    if (failures != 0) {
        fprintf(stderr, "test_lir_oom: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_lir_oom: all checks passed\n");
    return 0;
}
