/* M1-02b qualification: native artifact registry lifetime (src/runtime/native_versions.h).
 * Built only with SUTEKH_NATIVE. Usage: test_native_versions <repo root>
 *
 * Covers: state transitions; admission against the VM; deferred retirement while a call is in
 * flight; double release; no handle resolves after release; the bound on live artifacts; 1,000
 * prepare/admit/install/release cycles with a bounded live count and no growth of executable
 * mappings; no rwx mapping at any point. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backends/native/su_native.h"
#include "ir/su_lir.h"
#include "runtime/native_versions.h"
#include "sutekh/status.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)

typedef struct Buf {
    unsigned char *p;
    size_t n;
} Buf;

static int read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    long sz;
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return 0;
    }
    rewind(f);
    b->p = malloc((size_t)sz + 1u);
    if (b->p == NULL) {
        fclose(f);
        return 0;
    }
    if (fread(b->p, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b->p);
        b->p = NULL;
        return 0;
    }
    fclose(f);
    b->n = (size_t)sz;
    return 1;
}

/* Executable mappings of this process: count of lines with an x permission, and rwx lines. */
static void maps_scan(size_t *exec_lines, size_t *rwx_lines) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    *exec_lines = 0;
    *rwx_lines = 0;
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        /* perms field: after the first space-separated address range */
        char *sp = strchr(line, ' ');
        if (sp != NULL && sp[1] != '\0') {
            char *perm = sp + 1;
            if (perm[0] == 'r' && perm[1] == 'w' && perm[2] == 'x') {
                (*rwx_lines)++;
            }
            if (perm[2] == 'x') {
                (*exec_lines)++;
            }
        }
    }
    fclose(f);
}

static int load_movement(const char *root, SuLirModule *m) {
    char cap_path[1024];
    char lir_path[1024];
    Buf cap = {NULL, 0};
    Buf lir = {NULL, 0};
    SuDiag d;
    int ok = 0;
    snprintf(cap_path, sizeof cap_path, "%s/examples/movement.capsule.json", root);
    snprintf(lir_path, sizeof lir_path, "%s/examples/movement.lir.json", root);
    su_diag_init(&d);
    if (read_file(cap_path, &cap) && read_file(lir_path, &lir)) {
        ok = su_lir_verify(cap.p, cap.n, lir.p, lir.n, m, &d) == SU_OK;
        if (!ok) {
            fprintf(stderr, "verify failed: %s\n", d.message);
        }
    }
    free(cap.p);
    free(lir.p);
    return ok;
}

static void test_transitions(const SuLirModule *m) {
    uint64_t seq = 0;
    uint64_t seq2 = 0;
    char msg[256];
    char h1[65];
    char h2[65];
    uint32_t cases = 0;
    uint64_t rows = 0;
    uint64_t bad = 0;
    uint32_t st;
    uint32_t backend = 0;

    st = su_nv_prepare(m, 1u, SU_NCAP_X86_64_SCALAR, &seq, msg, sizeof msg);
    CHECK(st == SU_OK && seq != 0u, "prepare scalar: %s %s", su_status_name(st), msg);
    CHECK(su_nv_state(seq) == SU_NV_STATE_PREPARED, "state after prepare");
    CHECK(su_nv_install(seq) == SU_E_INTERNAL, "install before admission must be refused");
    st = su_nv_admit(seq, 7u, &cases, &rows, &bad);
    CHECK(st == SU_OK && bad == 0u, "admission: %s mismatches=%llu", su_status_name(st), (unsigned long long)bad);
    CHECK(cases >= 21u && rows >= 2000u, "corpus size cases=%u rows=%llu", (unsigned)cases, (unsigned long long)rows);
    CHECK(su_nv_state(seq) == SU_NV_STATE_ADMITTED, "state after admission");
    CHECK(su_nv_install(seq) == SU_OK, "install after admission");
    CHECK(su_nv_state(seq) == SU_NV_STATE_INSTALLED, "state after install");
    CHECK(su_nv_artifact_hash(seq, h1) == SU_OK && strlen(h1) == 64u, "artifact hash");
    CHECK(su_nv_backend_id(seq, &backend) == SU_OK && backend == SU_NATIVE_BACKEND_ASMJIT_SCALAR, "backend id");

    /* Deferred retirement: a call in flight keeps the code mapped until it returns. */
    su_nv_test_enter(seq);
    CHECK(su_nv_inflight(seq) == 1u, "in-flight count");
    su_nv_release(seq);
    CHECK(su_nv_state(seq) == SU_NV_STATE_RETIRING, "release with a call in flight defers (state RETIRING)");
    CHECK(su_nv_artifact_hash(seq, h2) == SU_OK, "a retiring artifact is not yet freed");
    su_nv_test_leave(seq);
    CHECK(su_nv_state(seq) == SU_NV_STATE_NONE, "last call returned: freed, handle resolves to nothing");
    CHECK(su_nv_artifact_hash(seq, h2) == SU_E_INTERNAL, "retired handle yields no artifact");

    /* Double release is a no-op; a stale handle cannot install or run. */
    su_nv_release(seq);
    CHECK(su_nv_install(seq) == SU_E_INTERNAL, "stale handle cannot install");
    CHECK(su_nv_run_system(seq, NULL) == SU_E_INTERNAL, "stale handle cannot run");

    /* Release before install (failed publication path) frees at once. */
    st = su_nv_prepare(m, 1u, SU_NCAP_X86_64_SCALAR, &seq2, msg, sizeof msg);
    CHECK(st == SU_OK, "prepare again");
    su_nv_release(seq2);
    CHECK(su_nv_state(seq2) == SU_NV_STATE_NONE, "prepared artifact released immediately");
}

static void test_bound(const SuLirModule *m) {
    uint64_t seqs[SU_NV_MAX_ARTIFACTS + 1u];
    char msg[256];
    uint32_t i;
    uint32_t st;
    uint32_t base = su_nv_live_count();
    uint32_t made = 0;

    for (i = 0; i < SU_NV_MAX_ARTIFACTS + 1u; i++) {
        seqs[i] = 0u;
        st = su_nv_prepare(m, 1u, SU_NCAP_X86_64_SCALAR, &seqs[i], msg, sizeof msg);
        if (st != SU_OK) {
            break;
        }
        made++;
    }
    CHECK(made == SU_NV_MAX_ARTIFACTS - base, "bound: made %u of %u with %u already live", (unsigned)made,
          (unsigned)SU_NV_MAX_ARTIFACTS, (unsigned)base);
    CHECK(st == SU_E_RESOURCE_LIMIT, "the artifact after the bound is refused with SU_E_RESOURCE_LIMIT");
    CHECK(su_nv_live_count() <= SU_NV_MAX_ARTIFACTS, "live count never exceeds the bound");
    for (i = 0; i < made; i++) {
        su_nv_release(seqs[i]);
    }
    CHECK(su_nv_live_count() == base, "all bound-test artifacts released");
}

static void test_cycles(const SuLirModule *m, uint32_t caps, const char *label, uint32_t n) {
    uint32_t i;
    size_t exec0;
    size_t rwx0;
    size_t exec1;
    size_t rwx1;
    size_t maxexec = 0;
    uint32_t max_live = 0;
    uint64_t released0 = su_nv_released_count();
    uint64_t code_first = 0;

    maps_scan(&exec0, &rwx0);
    for (i = 0; i < n; i++) {
        uint64_t seq = 0;
        uint32_t cases = 0;
        uint64_t rows = 0;
        uint64_t bad = 0;
        char msg[256];
        uint32_t st = su_nv_prepare(m, 1u, caps, &seq, msg, sizeof msg);
        CHECK(st == SU_OK, "%s cycle %u prepare: %s %s", label, (unsigned)i, su_status_name(st), msg);
        if (st != SU_OK) {
            return;
        }
        if (i == 0u) {
            (void)su_nv_code_bytes(seq, &code_first);
        }
        st = su_nv_admit(seq, 1000u + i, &cases, &rows, &bad);
        CHECK(st == SU_OK && bad == 0u, "%s cycle %u admission mismatches=%llu", label, (unsigned)i,
              (unsigned long long)bad);
        CHECK(su_nv_install(seq) == SU_OK, "%s cycle %u install", label, (unsigned)i);
        if (su_nv_live_count() > max_live) {
            max_live = su_nv_live_count();
        }
        {
            /* While the kernel is installed its mapping must exist and must not be rwx. */
            size_t e;
            size_t r;
            maps_scan(&e, &r);
            CHECK(r == 0u, "%s: rwx mapping while installed (cycle %u)", label, (unsigned)i);
            CHECK(e > 0u, "%s: installed kernel has no executable mapping (cycle %u)", label, (unsigned)i);
            if (e > maxexec) {
                maxexec = e;
            }
        }
        if (i % 2u == 0u) {
            /* Replacement-style: release the installed kernel directly. */
            su_nv_release(seq);
        } else {
            su_nv_test_enter(seq);
            su_nv_release(seq);
            su_nv_test_leave(seq);
        }
        CHECK(su_nv_state(seq) == SU_NV_STATE_NONE, "%s cycle %u handle gone", label, (unsigned)i);
        if (i % 100u == 99u) {
            size_t e;
            size_t r;
            maps_scan(&e, &r);
            if (e > maxexec) {
                maxexec = e;
            }
            CHECK(r == 0u, "%s: rwx mapping seen after %u cycles", label, (unsigned)(i + 1u));
        }
    }
    maps_scan(&exec1, &rwx1);
    CHECK(rwx1 == 0u && rwx0 == 0u, "%s: no rwx mapping (before %zu, after %zu)", label, rwx0, rwx1);
    CHECK(exec1 <= exec0 + 2u, "%s: executable mappings grew from %zu to %zu over %u cycles", label, exec0, exec1,
          (unsigned)n);
    CHECK(max_live <= SU_NV_MAX_ARTIFACTS, "%s: live count %u exceeds bound", label, (unsigned)max_live);
    CHECK(su_nv_live_count() == 0u, "%s: %u artifacts still live after the cycles", label,
          (unsigned)su_nv_live_count());
    CHECK(su_nv_released_count() - released0 == n, "%s: released %llu of %u", label,
          (unsigned long long)(su_nv_released_count() - released0), (unsigned)n);
    printf("%s: %u cycles, live<=%u, executable mappings %zu -> %zu (max %zu), code bytes %llu\n", label,
           (unsigned)n, (unsigned)max_live, exec0, exec1, maxexec, (unsigned long long)code_first);
}

int main(int argc, char **argv) {
    SuLirModule m;
    uint32_t host;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <repo root>\n", argv[0]);
        return 2;
    }
    memset(&m, 0, sizeof m);
    if (!load_movement(argv[1], &m)) {
        fprintf(stderr, "cannot verify examples/movement\n");
        return 1;
    }
    host = su_native_host_caps();
    test_transitions(&m);
    test_bound(&m);
    test_cycles(&m, SU_NCAP_X86_64_SCALAR, "scalar", 1000u);
    if ((host & SU_NCAP_X86_64_AVX2) != 0u) {
        test_cycles(&m, SU_NCAP_X86_64_AVX2, "avx2", 1000u);
    } else {
        printf("avx2 cycles skipped: host does not report AVX2\n");
    }
    su_lir_module_free(&m);
    printf("test_native_versions: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
