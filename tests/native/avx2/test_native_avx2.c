/* M1-03 AVX2 native backend tests (C17, own CHECK macros).
 *
 * Usage: test_native_avx2 <corpus_dir> [--bench]
 *   corpus_dir: output of tests/native/scalar/gen/gen_corpus.py --seed 20261012 --count 5000
 *   --bench: measures 1M-row throughput (observation only, never a gate)
 *
 * Every differential check compares three engines on identical inputs: the reference VM
 * (su_vm_invoke_v1), the scalar native kernel (SU_NCAP_X86_64_SCALAR) and the AVX2 native kernel
 * (SU_NCAP_X86_64_AVX2). When the host reports no AVX2 the AVX2 parts print "not run" and do not
 * count as passed. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "backends/native/su_native.h"
#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/native/abi.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"

#include "tests/native/native_test_mem.h" /* OS helpers: mappings, faults, AVX2 oracle, clocks */

static int g_checks = 0;
static int g_failures = 0;
static int g_avx_runs = 0;      /* differential runs that included the AVX2 engine */
static int g_avx_host = 0;      /* host reports AVX2 usable */

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_ST(expr, want)                                                        \
    do {                                                                            \
        uint32_t got_ = (expr);                                                     \
        g_checks++;                                                                 \
        if (got_ != (want)) {                                                       \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: %s -> %s, want %s\n", __FILE__, __LINE__, #expr, \
                    su_status_name(got_), su_status_name(want));                    \
        }                                                                           \
    } while (0)

/* Row counts of every differential run: 0..40, then the vector-width and page boundaries. */
static const uint64_t k_row_counts[] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    63, 64, 65, 127, 128, 129, 1024, 1025};
#define N_ROW_COUNTS (sizeof k_row_counts / sizeof k_row_counts[0])
#define MAXF 256u

static const uint32_t k_edges[] = {0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};

/* ---------- memory: guard-page columns ---------- */

/* Column storage: [guard][data pages][guard]. The data ends exactly at the after-guard page, so a
 * vector store or load past the last row faults. The start is not page aligned in general: a
 * column of rows r has data mod 32 == (-4*r) mod 32 (see run_unaligned_offsets). */
typedef struct Col {
    uint8_t *map;
    size_t map_bytes;
    uint32_t *data;
} Col;

static size_t page_size(void) {
    return su_nt_page_size();
}

static int col_alloc(Col *c, uint64_t rows) {
    size_t pg = page_size();
    size_t bytes = (size_t)rows * 4u;
    size_t nd = (bytes + pg - 1u) / pg;
    size_t total;
    uint8_t *base;
    uint8_t *boundary;
    c->map = NULL;
    c->map_bytes = 0;
    c->data = NULL;
    if (nd == 0u) {
        nd = 1u;
    }
    total = (nd + 2u) * pg;
    base = su_nt_guarded_map(total, 1u, nd);
    if (base == NULL) {
        return 0;
    }
    boundary = base + pg + nd * pg;
    c->map = base;
    c->map_bytes = total;
    c->data = (uint32_t *)(void *)(boundary - bytes);
    return 1;
}

static void col_free(Col *c) {
    su_nt_unmap(c->map, c->map_bytes);
    c->map = NULL;
    c->data = NULL;
}

static uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint32_t edge_or_random(uint64_t *s) {
    uint64_t r = sm64(s);
    if ((r & 3u) == 0u) {
        return k_edges[(r >> 8) % 6u];
    }
    return (uint32_t)(r >> 16);
}

/* ---------- file and verify helpers ---------- */

typedef struct Buf {
    uint8_t *p;
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

static int verify_pair(const char *cap_path, const char *lir_path, SuLirModule *m) {
    Buf cap = {0, 0};
    Buf lir = {0, 0};
    SuDiag d;
    uint32_t st;
    int ok = 0;
    su_diag_init(&d);
    if (read_file(cap_path, &cap) && read_file(lir_path, &lir)) {
        st = su_lir_verify(cap.p, cap.n, lir.p, lir.n, m, &d);
        if (st == SU_OK) {
            ok = 1;
        } else {
            fprintf(stderr, "verify %s + %s: %s (%s)\n", cap_path, lir_path, su_status_name(st), d.message);
        }
    } else {
        fprintf(stderr, "cannot read %s or %s\n", cap_path, lir_path);
    }
    free(cap.p);
    free(lir.p);
    return ok;
}

/* ---------- binding helpers ---------- */

static void bind_one(SuColumnBinding *b, uint32_t field_id, uint32_t access, uint64_t rows, uint32_t *data) {
    memset(b, 0, sizeof *b);
    b->struct_size = (uint32_t)sizeof(SuColumnBinding);
    b->field_id = field_id;
    b->elem_type = SU_TYPE_U32;
    b->access = access;
    b->layout_generation = 1u;
    b->stride_bytes = 4u;
    b->row_count = rows;
    b->data = data;
}

/* Heap-free module: out[r] = in[r]. Do not pass it to su_lir_module_free. */
static void make_copy_module(SuLirModule *m, SuLirField f[2], SuInsn ins[2]) {
    memset(m, 0, sizeof *m);
    memset(f, 0, 2u * sizeof f[0]);
    f[0].id = 1u;
    f[0].type = SU_TYPE_U32;
    f[0].access = SU_ACCESS_READ;
    f[1].id = 2u;
    f[1].type = SU_TYPE_U32;
    f[1].access = SU_ACCESS_WRITE;
    memset(ins, 0, 2u * sizeof ins[0]);
    ins[0].op = SU_OP_LOAD_U32;
    ins[0].dst = 0u;
    ins[0].a = 0u;
    ins[1].op = SU_OP_STORE_U32;
    ins[1].dst = SU_OPERAND_NONE;
    ins[1].a = 1u;
    ins[1].b = 0u;
    m->artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    m->field_count = 2u;
    m->fields = f;
    m->register_count = 1u;
    m->insn_count = 2u;
    m->insns = ins;
}

/* ---------- three-engine differential run ---------- */

/* Engines: 0 = reference VM, 1 = scalar native, 2 = AVX2 native. Every engine gets its own output
 * columns, filled with a different sentinel. Returns 1 when all engines return SU_OK, every staging
 * column matches byte for byte, and (when an AVX2 artifact is given) the AVX2 engine agreed. */
static int diff_run(const SuLirModule *m, SuExecContext *vm, SuNativeArtifact *sa, SuNativeArtifact *va,
                    uint64_t rows, uint64_t seed, const char *tag) {
    static SuColumnBinding in_b[MAXF];
    static Col in_c[MAXF];
    static SuColumnBinding ob[3][MAXF];
    static Col oc[3][MAXF];
    static SuOutputs outs[3];
    uint32_t ni = 0;
    uint32_t nw = 0;
    uint32_t i;
    uint32_t j;
    uint32_t e;
    uint64_t s = seed;
    uint64_t r;
    SuInvocation inv;
    uint32_t st[3];
    int ok = 1;
    int n_engines = va != NULL ? 3 : 2;

    memset(in_c, 0, sizeof in_c);
    memset(oc, 0, sizeof oc);
    for (i = 0; i < m->field_count; i++) {
        if ((m->fields[i].access & SU_ACCESS_READ) != 0u) {
            if (!col_alloc(&in_c[ni], rows)) {
                ok = 0;
                goto cleanup;
            }
            for (r = 0; r < rows; r++) {
                in_c[ni].data[r] = edge_or_random(&s);
            }
            bind_one(&in_b[ni], m->fields[i].id, SU_ACCESS_READ, rows, in_c[ni].data);
            ni++;
        }
    }
    for (i = 0; i < m->field_count; i++) {
        if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
            for (e = 0; e < 3u; e++) {
                uint32_t sentinel = e == 0u ? 0xA5A5A5A5u : (e == 1u ? 0x5A5A5A5Au : 0xC3C3C3C3u);
                if (!col_alloc(&oc[e][nw], rows)) {
                    ok = 0;
                    goto cleanup;
                }
                for (r = 0; r < rows; r++) {
                    oc[e][nw].data[r] = sentinel;
                }
                bind_one(&ob[e][nw], m->fields[i].id, SU_ACCESS_WRITE, rows, oc[e][nw].data);
            }
            nw++;
        }
    }

    inv.struct_size = (uint32_t)sizeof(SuInvocation);
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = ni;
    inv.row_count = rows;
    inv.bindings = in_b;
    for (e = 0; e < 3u; e++) {
        memset(&outs[e], 0, sizeof outs[e]);
        outs[e].struct_size = (uint32_t)sizeof(SuOutputs);
        outs[e].binding_count = nw;
        outs[e].failed_insn = UINT32_MAX;
        outs[e].reserved = 0u;
        outs[e].bindings = ob[e];
    }

    st[0] = su_vm_invoke_v1(vm, &inv, &outs[0]);
    st[1] = su_native_run_checked(sa, &inv, &outs[1]);
    st[2] = va != NULL ? su_native_run_checked(va, &inv, &outs[2]) : SU_OK;
    for (e = 0; e < (uint32_t)n_engines; e++) {
        if (st[e] != SU_OK) {
            fprintf(stderr, "%s rows=%llu: engine %u status %s\n", tag, (unsigned long long)rows, e,
                    su_status_name(st[e]));
            ok = 0;
        }
    }
    if (ok) {
        for (j = 0; j < nw; j++) {
            for (e = 1; e < (uint32_t)n_engines; e++) {
                if (memcmp(oc[0][j].data, oc[e][j].data, (size_t)rows * 4u) != 0) {
                    fprintf(stderr, "%s rows=%llu: staging column %u differs for engine %u\n", tag,
                            (unsigned long long)rows, j, e);
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                break;
            }
        }
    }
    if (va != NULL && ok) {
        g_avx_runs++;
    }

cleanup:
    for (i = 0; i < ni; i++) {
        col_free(&in_c[i]);
    }
    for (e = 0; e < 3u; e++) {
        for (j = 0; j < nw; j++) {
            col_free(&oc[e][j]);
        }
    }
    return ok;
}

/* Compiles m for the scalar engine and, when the host has AVX2, for the AVX2 engine. Runs every
 * row count of k_row_counts. */
static int diff_module(const SuLirModule *m, uint64_t seed_base, const char *tag) {
    SuExecContext *vm = NULL;
    SuNativeArtifact *sa = NULL;
    SuNativeArtifact *va = NULL;
    SuNativeDiag d;
    size_t k;
    int ok = 1;
    uint32_t st;

    st = su_vm_context_create(m, 1u, &vm);
    if (st != SU_OK) {
        fprintf(stderr, "%s: vm context: %s\n", tag, su_status_name(st));
        return 0;
    }
    st = su_native_compile(m, 1u, SU_NCAP_X86_64_SCALAR, &sa, &d);
    if (st != SU_OK) {
        fprintf(stderr, "%s: scalar compile: %s (%s)\n", tag, su_status_name(st), d.message);
        su_vm_context_destroy(vm);
        return 0;
    }
    if (g_avx_host) {
        st = su_native_compile(m, 1u, SU_NCAP_X86_64_AVX2, &va, &d);
        if (st != SU_OK) {
            fprintf(stderr, "%s: avx2 compile: %s (%s)\n", tag, su_status_name(st), d.message);
            ok = 0;
        }
    }
    for (k = 0; ok && k < N_ROW_COUNTS; k++) {
        if (!diff_run(m, vm, sa, va, k_row_counts[k], seed_base + k, tag)) {
            ok = 0;
        }
    }
    su_native_artifact_free(va);
    su_native_artifact_free(sa);
    su_vm_context_destroy(vm);
    return ok;
}

/* ---------- 1. capabilities and fallback ---------- */

/* Independent oracle: the OS's view (/proc/cpuinfo flags on Linux, IsProcessorFeaturePresent on Windows).
 * Returns -1 when unknown. */
static int cpuinfo_has_avx2(void) {
    return su_nt_os_avx2();
}

static void run_capabilities(void) {
    int oracle = cpuinfo_has_avx2();
    uint32_t caps = su_native_host_caps();
    uint32_t best = su_native_best_backend();
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;

    printf("host caps: 0x%x (AVX2 %s; the OS says %s)\n", caps,
           (caps & SU_NCAP_X86_64_AVX2) ? "usable" : "not usable",
           oracle < 0 ? "unknown" : (oracle ? "avx2" : "no avx2"));
    CHECK(caps & SU_NCAP_X86_64_SCALAR);
    if (oracle >= 0) {
        CHECK(((caps & SU_NCAP_X86_64_AVX2) != 0u) == (oracle == 1));
    }
    g_avx_host = (caps & SU_NCAP_X86_64_AVX2) != 0u;
    CHECK(best == (g_avx_host ? SU_NCAP_X86_64_AVX2 : SU_NCAP_X86_64_SCALAR));

    /* Unknown capability sets are refused. */
    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, 0u, &art, &d), SU_E_CAPABILITY_DENIED);
    CHECK(art == NULL);
    CHECK_ST(su_native_compile(&m, 1u, 0x4u, &art, &d), SU_E_CAPABILITY_DENIED);
    CHECK_ST(su_native_compile(&m, 1u, 0x10u | SU_NCAP_X86_64_SCALAR, &art, &d), SU_E_CAPABILITY_DENIED);

    /* Forced disable: AVX2 is not reported and an AVX2 request is denied; scalar still works. */
    su_native_test_disable_caps(SU_NCAP_X86_64_AVX2);
    CHECK((su_native_host_caps() & SU_NCAP_X86_64_AVX2) == 0u);
    CHECK((su_native_host_caps() & SU_NCAP_X86_64_SCALAR) != 0u);
    CHECK(su_native_best_backend() == SU_NCAP_X86_64_SCALAR);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &art, &d), SU_E_CAPABILITY_DENIED);
    CHECK(art == NULL);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2 | SU_NCAP_X86_64_SCALAR, &art, &d),
             SU_E_CAPABILITY_DENIED);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_OK);
    su_native_artifact_free(art);
    art = NULL;
    su_native_test_disable_caps(0u);
    CHECK((su_native_host_caps() & SU_NCAP_X86_64_AVX2) == (uint32_t)(g_avx_host ? SU_NCAP_X86_64_AVX2 : 0u));
}

/* Frame bound: a module of R registers needs frame = round32(32R + 8*(in+out)). With one input
 * and one output column the bound 65536 is reached at R = 2048 (frame 65568); R = 2047 fits. The
 * AVX2 lowering must return SU_E_RESOURCE_LIMIT at 2048, and the scalar lowering (4-byte slots)
 * still compiles and copies correctly. */
static SuInsn g_big_insns[2050];
static SuLirField g_big_fields[2];

static void make_big_module(SuLirModule *m, uint32_t regs) {
    uint32_t i;
    memset(m, 0, sizeof *m);
    memset(g_big_fields, 0, sizeof g_big_fields);
    g_big_fields[0].id = 1u;
    g_big_fields[0].type = SU_TYPE_U32;
    g_big_fields[0].access = SU_ACCESS_READ;
    g_big_fields[1].id = 2u;
    g_big_fields[1].type = SU_TYPE_U32;
    g_big_fields[1].access = SU_ACCESS_WRITE;
    memset(g_big_insns, 0, sizeof g_big_insns);
    g_big_insns[0].op = SU_OP_LOAD_U32;
    g_big_insns[0].dst = 0u;
    g_big_insns[0].a = 0u;
    for (i = 1u; i < regs; i++) {
        g_big_insns[i].op = SU_OP_CONST_U32;
        g_big_insns[i].dst = i;
        g_big_insns[i].a = i * 3u + 1u;
    }
    g_big_insns[regs].op = SU_OP_STORE_U32;
    g_big_insns[regs].dst = SU_OPERAND_NONE;
    g_big_insns[regs].a = 1u;
    g_big_insns[regs].b = 0u;
    m->artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    m->field_count = 2u;
    m->fields = g_big_fields;
    m->register_count = regs;
    m->insn_count = regs + 1u;
    m->insns = g_big_insns;
}

static void run_frame_bound(void) {
    SuLirModule m;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    Col ca;
    Col co;
    SuColumnBinding in[1];
    SuColumnBinding out[1];
    SuInvocation inv;
    SuOutputs outs;
    uint32_t r;
    int ok = 1;

    make_big_module(&m, 2048u);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_OK);
    su_native_artifact_free(art);
    art = NULL;
    if (g_avx_host) {
        CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &art, &d), SU_E_RESOURCE_LIMIT);
        CHECK(art == NULL);
        CHECK(strstr(d.message, "frame") != NULL);
    }

    make_big_module(&m, 2047u);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_OK);
    su_native_artifact_free(art);
    art = NULL;
    if (!g_avx_host) {
        printf("frame bound: AVX2 part not run (no AVX2 on host)\n");
        return;
    }
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &art, &d), SU_OK);
    if (art == NULL || !col_alloc(&ca, 37) || !col_alloc(&co, 37)) {
        CHECK(0);
        su_native_artifact_free(art);
        return;
    }
    for (r = 0; r < 37u; r++) {
        ca.data[r] = r * 11u + 5u;
        co.data[r] = 0u;
    }
    bind_one(&in[0], 1u, SU_ACCESS_READ, 37u, ca.data);
    bind_one(&out[0], 2u, SU_ACCESS_WRITE, 37u, co.data);
    memset(&inv, 0, sizeof inv);
    inv.struct_size = sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = 1u;
    inv.row_count = 37u;
    inv.bindings = in;
    memset(&outs, 0, sizeof outs);
    outs.struct_size = sizeof outs;
    outs.binding_count = 1u;
    outs.failed_insn = UINT32_MAX;
    outs.bindings = out;
    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_OK);
    for (r = 0; r < 37u; r++) {
        /* The program stores r0 (the loaded input). */
        if (co.data[r] != ca.data[r]) {
            ok = 0;
        }
    }
    CHECK(ok);
    col_free(&ca);
    col_free(&co);
    su_native_artifact_free(art);
    printf("frame bound (R=2048 -> RESOURCE_LIMIT, R=2047 AVX2 copy): %s\n", ok ? "ok" : "FAILED");
}

/* ---------- 2. known answers (hand-written, independent of VM and scalar) ---------- */

typedef struct Tri {
    uint32_t a;
    uint32_t b;
    uint32_t want;
} Tri;

typedef struct Kat {
    const char *name;
    const SuInsn *insns;
    uint32_t n_insns;
    uint32_t regs;
    Tri tri[8];
} Kat;

/* Field table: 0 = a (id 1, read), 1 = b (id 2, read), 2 = out (id 3, write). Every KAT runs 43
 * rows (five full vectors plus a three-row tail); row r uses tri[r % 8]. */
#define KAT_ROWS 43u

static const SuInsn k_sub[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0},
    {SU_OP_SUB_U32, 2, 0, 1, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 2, 0}};
static const SuInsn k_add[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0},
    {SU_OP_ADD_U32, 2, 0, 1, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 2, 0}};
static const SuInsn k_mul[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0},
    {SU_OP_MUL_U32, 2, 0, 1, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 2, 0}};
static const SuInsn k_min[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0}, {SU_OP_LT_U32, 2, 0, 1, 0},
    {SU_OP_SELECT_U32, 3, 2, 0, 1}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 3, 0}};
static const SuInsn k_eq79[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0}, {SU_OP_EQ_U32, 2, 0, 1, 0},
    {SU_OP_CONST_U32, 3, 7, 0, 0}, {SU_OP_CONST_U32, 4, 9, 0, 0},
    {SU_OP_SELECT_U32, 5, 2, 3, 4}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 5, 0}};
static const SuInsn k_max[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0}, {SU_OP_LT_U32, 2, 0, 1, 0},
    {SU_OP_SELECT_U32, 3, 2, 1, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 3, 0}};
static const SuInsn k_const[] = {
    {SU_OP_CONST_U32, 0, 0xFFFFFFFFu, 0, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 0, 0}};

/* Expected values were worked out by hand (not produced by either implementation). */
static const Kat k_kats[] = {
    {"sub", k_sub, 4u, 3u,
     {{0u, 1u, 0xFFFFFFFFu}, {0x80000000u, 1u, 0x7FFFFFFFu}, {0u, 0u, 0u},
      {0xFFFFFFFFu, 0xFFFFFFFFu, 0u}, {5u, 7u, 0xFFFFFFFEu}, {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu},
      {1u, 0xFFFFFFFFu, 2u}, {0xFFFFFFFFu, 0u, 0xFFFFFFFFu}}},
    {"add", k_add, 4u, 3u,
     {{0xFFFFFFFFu, 1u, 0u}, {0x80000000u, 0x80000000u, 0u}, {0x7FFFFFFFu, 1u, 0x80000000u},
      {3u, 4u, 7u}, {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFEu}, {0u, 0u, 0u},
      {0x12345678u, 0x87654321u, 0x99999999u}, {1u, 0xFFFFFFFEu, 0xFFFFFFFFu}}},
    {"mul", k_mul, 4u, 3u,
     {{0x80000000u, 2u, 0u}, {0xFFFFFFFFu, 0xFFFFFFFFu, 1u}, {0x10000u, 0x10000u, 0u},
      {3u, 5u, 15u}, {0x7FFFFFFFu, 2u, 0xFFFFFFFEu}, {0u, 0xFFFFFFFFu, 0u},
      {0x12345678u, 9u, 0xA3D70A38u}, {0x80000001u, 3u, 0x80000003u}}},
    {"unsigned_min", k_min, 5u, 4u,
     {{0x80000000u, 0x7FFFFFFFu, 0x7FFFFFFFu}, {0xFFFFFFFFu, 0u, 0u}, {0u, 0xFFFFFFFFu, 0u},
      {5u, 5u, 5u}, {0x7FFFFFFFu, 0x80000000u, 0x7FFFFFFFu}, {1u, 0x80000000u, 1u},
      {0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFEu}, {0x80000000u, 0x80000000u, 0x80000000u}}},
    {"eq_select", k_eq79, 7u, 6u,
     {{0u, 0u, 7u}, {0xFFFFFFFFu, 0xFFFFFFFFu, 7u}, {0u, 1u, 9u}, {0x80000000u, 0x7FFFFFFFu, 9u},
      {0x80000000u, 0x80000000u, 7u}, {1u, 2u, 9u}, {7u, 7u, 7u}, {0xFFFFFFFEu, 0xFFFFFFFFu, 9u}}},
    {"unsigned_max", k_max, 5u, 4u,
     {{0x80000000u, 0x7FFFFFFFu, 0x80000000u}, {0u, 0xFFFFFFFFu, 0xFFFFFFFFu},
      {1u, 0x80000000u, 0x80000000u}, {0xFFFFFFFFu, 0u, 0xFFFFFFFFu}, {5u, 5u, 5u},
      {0x7FFFFFFFu, 0x80000000u, 0x80000000u}, {0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu},
      {0u, 0u, 0u}}},
    {"const", k_const, 2u, 1u,
     {{0u, 0u, 0xFFFFFFFFu}, {1u, 2u, 0xFFFFFFFFu}, {3u, 4u, 0xFFFFFFFFu}, {5u, 6u, 0xFFFFFFFFu},
      {7u, 8u, 0xFFFFFFFFu}, {9u, 10u, 0xFFFFFFFFu}, {11u, 12u, 0xFFFFFFFFu},
      {13u, 14u, 0xFFFFFFFFu}}},
};

/* Runs one KAT on one engine (caps). Returns the number of wrong rows. */
static uint32_t run_kat(const Kat *k, uint32_t caps) {
    SuLirField fields[3];
    SuLirModule m;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    Col ca;
    Col cb;
    Col co;
    SuColumnBinding in[2];
    SuColumnBinding out[1];
    SuInvocation inv;
    SuOutputs outs;
    uint32_t st;
    uint32_t r;
    uint32_t bad = 0;

    memset(&m, 0, sizeof m);
    memset(fields, 0, sizeof fields);
    fields[0].id = 1u; fields[0].type = SU_TYPE_U32; fields[0].access = SU_ACCESS_READ;
    fields[1].id = 2u; fields[1].type = SU_TYPE_U32; fields[1].access = SU_ACCESS_READ;
    fields[2].id = 3u; fields[2].type = SU_TYPE_U32; fields[2].access = SU_ACCESS_WRITE;
    m.artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    m.field_count = 3u;
    m.fields = fields;
    m.register_count = k->regs;
    m.insn_count = k->n_insns;
    m.insns = (SuInsn *)(void *)k->insns;

    st = su_native_compile(&m, 1u, caps, &art, &d);
    CHECK_ST(st, SU_OK);
    if (st != SU_OK) {
        return 1u;
    }
    if (!col_alloc(&ca, KAT_ROWS) || !col_alloc(&cb, KAT_ROWS) || !col_alloc(&co, KAT_ROWS)) {
        su_native_artifact_free(art);
        return 1u;
    }
    for (r = 0; r < KAT_ROWS; r++) {
        ca.data[r] = k->tri[r % 8u].a;
        cb.data[r] = k->tri[r % 8u].b;
        co.data[r] = 0xDEADBEEFu;
    }
    bind_one(&in[0], 1u, SU_ACCESS_READ, KAT_ROWS, ca.data);
    bind_one(&in[1], 2u, SU_ACCESS_READ, KAT_ROWS, cb.data);
    bind_one(&out[0], 3u, SU_ACCESS_WRITE, KAT_ROWS, co.data);
    memset(&inv, 0, sizeof inv);
    inv.struct_size = sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = 2u;
    inv.row_count = KAT_ROWS;
    inv.bindings = in;
    memset(&outs, 0, sizeof outs);
    outs.struct_size = sizeof outs;
    outs.binding_count = 1u;
    outs.failed_insn = UINT32_MAX;
    outs.bindings = out;
    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_OK);
    for (r = 0; r < KAT_ROWS; r++) {
        const Tri *t = &k->tri[r % 8u];
        if (co.data[r] != t->want) {
            if (bad < 4u) {
                fprintf(stderr, "KAT %s caps=0x%x row %u: a=%08x b=%08x got %08x want %08x\n", k->name,
                        caps, r, t->a, t->b, co.data[r], t->want);
            }
            bad++;
        }
    }
    CHECK(bad == 0u);
    col_free(&ca);
    col_free(&cb);
    col_free(&co);
    su_native_artifact_free(art);
    return bad;
}

static int run_known_answers(void) {
    size_t i;
    size_t n = sizeof k_kats / sizeof k_kats[0];
    uint32_t bad = 0;
    for (i = 0; i < n; i++) {
        bad += run_kat(&k_kats[i], SU_NCAP_X86_64_SCALAR);
        if (g_avx_host) {
            bad += run_kat(&k_kats[i], SU_NCAP_X86_64_AVX2);
        }
    }
    printf("known answers (%zu programs, 43 rows each, scalar%s): %s\n", n,
           g_avx_host ? " and avx2" : " (avx2 not run)", bad == 0u ? "ok" : "FAILED");
    return bad == 0u;
}

/* ---------- 3. fixtures ---------- */

static const char *k_fixture_pairs[][2] = {
    {"tests/fixtures/t02/movement.capsule.json", "tests/fixtures/t02/movement.lir.json"},
    {"tests/fixtures/t03/eq_edges/capsule.json", "tests/fixtures/t03/eq_edges/program.lir.json"},
    {"tests/fixtures/t03/lt_edges/capsule.json", "tests/fixtures/t03/lt_edges/program.lir.json"},
    {"tests/fixtures/t03/mul_overflow/capsule.json", "tests/fixtures/t03/mul_overflow/program.lir.json"},
    {"tests/fixtures/t03/select_min/capsule.json", "tests/fixtures/t03/select_min/program.lir.json"},
    {"tests/fixtures/t03/sub_underflow/capsule.json", "tests/fixtures/t03/sub_underflow/program.lir.json"},
    {"tests/fixtures/t03/wrap_add/capsule.json", "tests/fixtures/t03/wrap_add/program.lir.json"},
    {"tests/fixtures/m0_neg/valid.capsule.json", "tests/fixtures/m0_neg/valid.lir.json"},
    {"tests/fixtures/edit/movement_v2.capsule.json", "tests/fixtures/edit/movement_v2.lir.json"},
    {"examples/movement.capsule.json", "examples/movement.lir.json"},
};

static int run_fixtures(void) {
    size_t i;
    size_t n = sizeof k_fixture_pairs / sizeof k_fixture_pairs[0];
    int ok = 1;
    for (i = 0; i < n; i++) {
        SuLirModule m;
        memset(&m, 0, sizeof m);
        if (!verify_pair(k_fixture_pairs[i][0], k_fixture_pairs[i][1], &m)) {
            ok = 0;
            continue;
        }
        if (!diff_module(&m, 7000u * (uint64_t)i, k_fixture_pairs[i][1])) {
            ok = 0;
        }
        su_lir_module_free(&m);
    }
    printf("fixtures (%zu verified pairs, %zu row counts each, 3 engines%s): %s\n", n, N_ROW_COUNTS,
           g_avx_host ? "" : " (avx2 not run)", ok ? "ok" : "FAILED");
    CHECK(ok);
    return ok;
}

/* ---------- 4. seeded corpus ---------- */

static int run_corpus(const char *dir) {
    char path[1024];
    char line[256];
    FILE *idx;
    uint32_t n = 0;
    uint32_t verified = 0;
    uint32_t failed = 0;

    snprintf(path, sizeof path, "%s/index.txt", dir);
    idx = fopen(path, "r");
    if (idx == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    while (fgets(line, sizeof line, idx) != NULL) {
        char cap[1024];
        char lir[1024];
        SuLirModule m;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        n++;
        snprintf(cap, sizeof cap, "%s/%s.capsule.json", dir, line);
        snprintf(lir, sizeof lir, "%s/%s.lir.json", dir, line);
        memset(&m, 0, sizeof m);
        if (!verify_pair(cap, lir, &m)) {
            failed++;
            continue;
        }
        verified++;
        if (!diff_module(&m, 0x5EED0000u + (uint64_t)n * 977u, line)) {
            failed++;
        }
        su_lir_module_free(&m);
    }
    fclose(idx);
    printf("corpus: %u programs, %u verified, %u failed 3-engine differential%s\n", n, verified, failed,
           g_avx_host ? "" : " (avx2 not run)");
    CHECK(n >= 5000u);
    CHECK(verified == n);
    CHECK(failed == 0u);
    return failed == 0u && n >= 5000u && verified == n;
}

/* ---------- 5. unaligned columns with exact end guard ---------- */

/* A column of r rows ends at a page boundary, so data mod 32 == (-4r) mod 32. Rows 4096 + (8-k)
 * give data mod 32 == 4k for k = 1..7 (element offsets 1..7 from a 32-byte boundary). The VM and
 * the native engines run these with the end guard in place. */
static int run_unaligned_offsets(void) {
    int ok = 1;
    uint32_t k;
    for (k = 1u; k <= 7u; k++) {
        SuLirField f[2];
        SuInsn ins[2];
        SuLirModule m;
        uint64_t rows = 4096u + (8u - k);
        Col probe;
        make_copy_module(&m, f, ins);
        if (!col_alloc(&probe, rows)) {
            ok = 0;
            continue;
        }
        if (((uintptr_t)probe.data % 32u) != 4u * k) {
            fprintf(stderr, "offset %u: data mod 32 = %u, want %u\n", k,
                    (unsigned)((uintptr_t)probe.data % 32u), 4u * k);
            ok = 0;
        }
        col_free(&probe);
        if (!ok) {
            continue;
        }
        {
            SuExecContext *vm = NULL;
            SuNativeArtifact *sa = NULL;
            SuNativeArtifact *va = NULL;
            SuNativeDiag d;
            CHECK_ST(su_vm_context_create(&m, 1u, &vm), SU_OK);
            CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &sa, &d), SU_OK);
            if (g_avx_host) {
                CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &va, &d), SU_OK);
            }
            if (!diff_run(&m, vm, sa, va, rows, 0xA11CEu + k, "unaligned")) {
                ok = 0;
            }
            su_native_artifact_free(va);
            su_native_artifact_free(sa);
            su_vm_context_destroy(vm);
        }
    }
    printf("unaligned columns (element offsets 1..7, exact end guard, rows 4097..4103): %s%s\n",
           ok ? "ok" : "FAILED", g_avx_host ? "" : " (avx2 not run)");
    return ok;
}

/* ---------- 6. wrapper rejections on the AVX2 artifact ---------- */

static void run_wrapper_avx2(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    Col ca;
    Col co;
    SuColumnBinding in[1];
    SuColumnBinding out[1];
    SuInvocation inv;
    SuOutputs outs;
    uint32_t r;

    if (!g_avx_host) {
        printf("wrapper rejections (AVX2): not run (no AVX2 on host)\n");
        return;
    }
    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &art, &d), SU_OK);
    if (art == NULL || !col_alloc(&ca, 33) || !col_alloc(&co, 33)) {
        CHECK(0);
        su_native_artifact_free(art);
        return;
    }
    for (r = 0; r < 33u; r++) {
        ca.data[r] = r + 1u;
        co.data[r] = 0x77777777u;
    }
    memset(&inv, 0, sizeof inv);
    inv.struct_size = sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = 1u;
    inv.row_count = 33u;
    inv.bindings = in;
    bind_one(&in[0], 1u, SU_ACCESS_READ, 33u, ca.data);
    memset(&outs, 0, sizeof outs);
    outs.struct_size = sizeof outs;
    outs.binding_count = 1u;
    outs.failed_insn = UINT32_MAX;
    outs.bindings = out;

    /* Aliasing: the output column is the input column. Must be rejected, nothing written. */
    bind_one(&out[0], 2u, SU_ACCESS_WRITE, 33u, ca.data);
    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_E_INVALID_ARGUMENT);
    CHECK(ca.data[0] == 1u && ca.data[32] == 33u);

    /* Row limit below the row count. */
    bind_one(&out[0], 2u, SU_ACCESS_WRITE, 33u, co.data);
    CHECK_ST(su_native_set_row_limit(art, 32u), SU_OK);
    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_E_RESOURCE_LIMIT);
    for (r = 0; r < 33u; r++) {
        if (co.data[r] != 0x77777777u) {
            CHECK(0);
            break;
        }
    }
    CHECK_ST(su_native_set_row_limit(art, SU_LIMIT_ROWS), SU_OK);

    /* Generation mismatch. */
    inv.layout_generation = 2u;
    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_E_SCHEMA_MISMATCH);
    inv.layout_generation = 1u;

    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_OK);
    for (r = 0; r < 33u; r++) {
        if (co.data[r] != ca.data[r]) {
            CHECK(0);
            break;
        }
    }
    col_free(&ca);
    col_free(&co);
    su_native_artifact_free(art);
    printf("wrapper rejections on AVX2 (alias, row limit, generation): ok\n");
}

/* ---------- 7. ABI: callee-saved registers and upper YMM state ---------- */

#if defined(__x86_64__) && !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
/* uint32_t su_test_avx2_clobber_call(SuNativeKernelFn fn, const SuNativeArgs *args, uint64_t regs[7])
 * Same contract as the scalar trampoline: poisons rbx rbp r12-r15, calls fn, stores the six
 * values and the status into regs[0..6]. */
__asm__(
    ".text\n"
    ".globl su_test_avx2_clobber_call\n"
    ".type su_test_avx2_clobber_call,@function\n"
    "su_test_avx2_clobber_call:\n"
    "    push %rbp\n"
    "    push %rbx\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    sub $24, %rsp\n"
    "    mov %rdx, (%rsp)\n"
    "    mov %rdi, %rax\n"
    "    mov %rsi, %rdi\n"
    "    movabs $0x1111111111111111, %rbx\n"
    "    movabs $0x2222222222222222, %rbp\n"
    "    movabs $0x3333333333333333, %r12\n"
    "    movabs $0x4444444444444444, %r13\n"
    "    movabs $0x5555555555555555, %r14\n"
    "    movabs $0x6666666666666666, %r15\n"
    "    call *%rax\n"
    "    mov (%rsp), %rcx\n"
    "    mov %rbx, 0(%rcx)\n"
    "    mov %rbp, 8(%rcx)\n"
    "    mov %r12, 16(%rcx)\n"
    "    mov %r13, 24(%rcx)\n"
    "    mov %r14, 32(%rcx)\n"
    "    mov %r15, 40(%rcx)\n"
    "    mov %rax, 48(%rcx)\n"
    "    add $24, %rsp\n"
    "    pop %r15\n"
    "    pop %r14\n"
    "    pop %r13\n"
    "    pop %r12\n"
    "    pop %rbx\n"
    "    pop %rbp\n"
    "    ret\n"
    ".size su_test_avx2_clobber_call, .-su_test_avx2_clobber_call\n");

uint32_t su_test_avx2_clobber_call(SuNativeKernelFn fn, const SuNativeArgs *args, uint64_t regs[7]);

/* XSAVE the x87/SSE/AVX state and return XSTATE_BV (header bytes 512..519). Bit 2 (YMM_Hi128) is
 * set when the upper halves of ymm0-15 are not in their initial (zero) state. */
/* XSAVE areas, zeroed BEFORE the upper YMM state is dirtied: a library memset between dirtying and XSAVE may
 * itself end with vzeroupper (glibc's AVX2 memset does) and clear the state the probe is looking for
 * (2026-10-18: that made the probe invalid on CI runners). */
static uint8_t g_xarea[2][4096] __attribute__((aligned(64)));

static void xstate_prepare(void) {
    memset(g_xarea, 0, sizeof g_xarea);
}

/* XSTATE_BV after XSAVE into area k (no call between the caller's last vector instruction and the XSAVE). */
static uint64_t xstate_bv(unsigned k) {
    uint64_t bv;
    __asm__ volatile("xsave (%0)" : : "r"(g_xarea[k]), "a"(7u), "d"(0u) : "memory");
    memcpy(&bv, g_xarea[k] + 512, sizeof bv);
    return bv;
}

/* Sets the upper 128 bits of ymm15 to ones (VEX write), leaving the kernel's registers alone. */
static void dirty_upper_ymm(void) {
    __asm__ volatile("vpcmpeqd %%ymm15, %%ymm15, %%ymm15" : : : "xmm15");
}

static int run_abi_clobber(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *sart = NULL;
    SuNativeArtifact *vart = NULL;
    SuNativeDiag d;
    Col ca;
    Col co;
    SuNativeArgs args;
    const uint32_t *in_cols[1];
    uint32_t *out_cols[1];
    uint64_t regs[7];
    uint64_t want[6] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull,
                        0x4444444444444444ull, 0x5555555555555555ull, 0x6666666666666666ull};
    uint32_t r;
    int ok = 1;
    int feasible;
    uint32_t engine;

    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &sart, &d), SU_OK);
    if (g_avx_host) {
        CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &vart, &d), SU_OK);
    }
    if (!col_alloc(&ca, 33) || !col_alloc(&co, 33)) {
        CHECK(0);
        return 0;
    }
    for (r = 0; r < 33u; r++) {
        ca.data[r] = r * 7u + 3u;
        co.data[r] = 0u;
    }
    in_cols[0] = ca.data;
    out_cols[0] = co.data;
    args.struct_size = (uint32_t)sizeof args;
    args.abi_version = SU_NATIVE_ABI_VERSION;
    args.row_count = 33u;
    args.in_cols = in_cols;
    args.out_cols = out_cols;

    for (engine = 0; engine < 2u; engine++) {
        SuNativeArtifact *art = engine == 0u ? sart : vart;
        SuNativeKernelFn fn;
        uint64_t bv_before;
        uint64_t bv_after;
        if (art == NULL) {
            continue;
        }
        fn = su_native_entry(art);
        memset(regs, 0, sizeof regs);
        memset(co.data, 0, 33u * sizeof co.data[0]);
        xstate_prepare();
        dirty_upper_ymm();
        bv_before = xstate_bv(0u);
        CHECK_ST(su_test_avx2_clobber_call(fn, &args, regs), SU_OK);
        bv_after = xstate_bv(1u);
        for (r = 0; r < 6u; r++) {
            if (regs[r] != want[r]) {
                fprintf(stderr, "%s: callee-saved register %u clobbered: %016llx\n",
                        engine == 0u ? "scalar" : "avx2", r, (unsigned long long)regs[r]);
                ok = 0;
            }
        }
        for (r = 0; r < 33u; r++) {
            if (co.data[r] != ca.data[r]) {
                fprintf(stderr, "%s clobber call: row %u not copied\n", engine == 0u ? "scalar" : "avx2", r);
                ok = 0;
                break;
            }
        }
        if (engine == 1u) {
            /* Upper YMM state: dirty before (bit 2 set), clean after (vzeroupper). */
            feasible = (bv_before & 4u) != 0u;
            printf("upper YMM state: XSTATE_BV bit2 before call=%d after call=%d (%s)\n",
                   (int)((bv_before >> 2) & 1u), (int)((bv_after >> 2) & 1u),
                   feasible ? "probe valid" : "probe NOT valid: dirty state not observed");
            CHECK(feasible);
            CHECK((bv_after & 4u) == 0u);
            if ((bv_after & 4u) != 0u) {
                ok = 0;
            }
        }
    }
    col_free(&ca);
    col_free(&co);
    su_native_artifact_free(vart);
    su_native_artifact_free(sart);
    printf("ABI clobber trampoline (SysV, rbx rbp r12-r15 poisoned and checked, scalar%s): %s\n",
           g_avx_host ? " and avx2" : "", ok ? "ok" : "FAILED");
    return ok;
}
#else
static int run_abi_clobber(void) {
    printf("ABI clobber trampoline: not run (needs SysV x86-64 with GNU-compatible asm)\n");
    return 1;
}
#endif

/* ---------- 8. disassembly, source map, metadata, hash ---------- */

/* Every ret must be preceded (since the previous ret, or the start) by a vzeroupper. */
static int check_vzeroupper_before_ret(const char *text) {
    int seen_vzu = 0;
    int rets = 0;
    int ok = 1;
    const char *p = text;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        char buf[256];
        size_t n = len < sizeof buf - 1u ? len : sizeof buf - 1u;
        memcpy(buf, p, n);
        buf[n] = '\0';
        if (strstr(buf, "vzeroupper") != NULL) {
            seen_vzu = 1;
        } else if (strstr(buf, "ret") != NULL && strstr(buf, "ret") == buf + strspn(buf, " \t")) {
            rets++;
            if (!seen_vzu) {
                ok = 0;
            }
            seen_vzu = 0;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return ok && rets >= 2;
}

static void run_metadata(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *sa = NULL;
    SuNativeArtifact *va = NULL;
    SuNativeDiag d;
    SuNativeArtifactInfo si;
    SuNativeArtifactInfo vi;
    const SuNativeSourceRange *ranges = NULL;
    uint32_t count = 0;
    const uint8_t *code = NULL;
    size_t code_size = 0;
    uint8_t h1[SU_HASH_BYTES];
    uint8_t h2[SU_HASH_BYTES];
    uint8_t h3[SU_HASH_BYTES];
    char *text = NULL;
    size_t len = 0;
    uint32_t k;
    int ok = 1;
    SuLirField kf[3];

    if (!g_avx_host) {
        printf("metadata/source map/disassembly/hash: AVX2 part not run (no AVX2 on host)\n");
        return;
    }
    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &sa, &d), SU_OK);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &va, &d), SU_OK);
    if (sa == NULL || va == NULL) {
        su_native_artifact_free(sa);
        su_native_artifact_free(va);
        return;
    }
    CHECK_ST(su_native_artifact_info(va, &vi), SU_OK);
    CHECK_ST(su_native_artifact_info(sa, &si), SU_OK);
    CHECK(vi.backend_id == SU_NATIVE_BACKEND_ASMJIT_AVX2);
    CHECK(si.backend_id == SU_NATIVE_BACKEND_ASMJIT_SCALAR);
    CHECK(vi.required_caps == SU_NCAP_X86_64_AVX2);
    CHECK(memcmp(vi.semantic_hash, si.semantic_hash, SU_HASH_BYTES) == 0);
    CHECK(memcmp(vi.artifact_hash, si.artifact_hash, SU_HASH_BYTES) != 0);

    /* Hash sensitivity: each input changes the hash, equal inputs do not. */
    su_native_test_hash(m.semantic_hash, 1u, SU_NCAP_X86_64_AVX2, h1);
    su_native_test_hash(m.semantic_hash, 1u, SU_NCAP_X86_64_AVX2, h2);
    CHECK(memcmp(h1, h2, SU_HASH_BYTES) == 0);
    su_native_test_hash(m.semantic_hash, 2u, SU_NCAP_X86_64_AVX2, h2);
    CHECK(memcmp(h1, h2, SU_HASH_BYTES) != 0);
    su_native_test_hash(m.semantic_hash, 1u, SU_NCAP_X86_64_SCALAR, h3);
    CHECK(memcmp(h1, h3, SU_HASH_BYTES) != 0);
    {
        uint8_t other[SU_HASH_BYTES];
        memcpy(other, m.semantic_hash, SU_HASH_BYTES);
        other[0] ^= 1u;
        su_native_test_hash(other, 1u, SU_NCAP_X86_64_AVX2, h2);
        CHECK(memcmp(h1, h2, SU_HASH_BYTES) != 0);
    }
    CHECK(memcmp(h1, vi.artifact_hash, SU_HASH_BYTES) == 0);

    /* Source map: one range per instruction, ascending, inside the code. */
    CHECK_ST(su_native_source_map(va, &ranges, &count), SU_OK);
    CHECK(count == 2u);
    CHECK_ST(su_native_code(va, &code, &code_size), SU_OK);
    for (k = 0; k < count && ranges != NULL; k++) {
        if (ranges[k].insn != k || ranges[k].begin >= ranges[k].end || ranges[k].end > code_size) {
            ok = 0;
        }
        if (k > 0 && ranges[k].begin < ranges[k - 1].end) {
            ok = 0;
        }
    }
    CHECK(ok);

    su_native_artifact_free(va);
    su_native_artifact_free(sa);

    /* Disassembly from the unsigned-min program: it contains the compare and the select. */
    memset(kf, 0, sizeof kf);
    kf[0].id = 1u; kf[0].type = SU_TYPE_U32; kf[0].access = SU_ACCESS_READ;
    kf[1].id = 2u; kf[1].type = SU_TYPE_U32; kf[1].access = SU_ACCESS_READ;
    kf[2].id = 3u; kf[2].type = SU_TYPE_U32; kf[2].access = SU_ACCESS_WRITE;
    memset(&m, 0, sizeof m);
    m.artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    m.field_count = 3u;
    m.fields = kf;
    m.register_count = k_kats[3].regs;
    m.insn_count = k_kats[3].n_insns;
    m.insns = (SuInsn *)(void *)k_kats[3].insns;
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &va, &d), SU_OK);
    if (va == NULL) {
        return;
    }
    text = malloc(65536u);
    if (text != NULL && su_native_disassemble(va, text, 65536u, &len) == SU_OK) {
        CHECK(check_vzeroupper_before_ret(text));
        CHECK(strstr(text, "vpblendvb") != NULL);
        CHECK(strstr(text, "vpcmpgtd") != NULL);
        CHECK(strstr(text, "vmovdqa") == NULL);
        CHECK(strstr(text, "ymmword ptr") != NULL);
        CHECK(su_native_code(va, &code, &code_size) == SU_OK);
        printf("disassembly (unsigned-min): %zu bytes of text, %zu bytes of code, ret/vzeroupper rule checked\n",
               len, code_size);
    } else {
        CHECK(0);
    }
    free(text);
    su_native_artifact_free(va);
}

/* ---------- 9. W^X of the AVX2 mapping ---------- */

/* 1 when any mapping is writable and executable, 0 when none. */
static int count_rwx_lines(void) {
    char p[8];
    int any = 0;
    (void)su_nt_perms_at(0u, p, &any);
    return any;
}

/* Permissions of the mapping containing addr, as "r-xp" style text. Returns 0 when not found. */
static int perms_at(uintptr_t addr, char perms[8]) {
    return su_nt_perms_at(addr, perms, NULL);
}

static void run_wx(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *va = NULL;
    SuNativeDiag d;
    const uint8_t *code = NULL;
    size_t code_size = 0;
    char perms[8] = {0};

    if (!g_avx_host) {
        printf("W^X (AVX2): not run (no AVX2 on host)\n");
        return;
    }
    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &va, &d), SU_OK);
    if (va == NULL) {
        return;
    }
    CHECK_ST(su_native_code(va, &code, &code_size), SU_OK);
    CHECK(perms_at((uintptr_t)code, perms) == 1);
    CHECK(strcmp(perms, "r-xp") == 0);
    CHECK(count_rwx_lines() == 0);
    /* Writing into the code mapping must fault (a forked child on POSIX, SEH on Windows). */
    CHECK(su_nt_store_faults(code, 0xCCu) == 1);
    su_native_artifact_free(va);
    printf("W^X (AVX2 code mapping %s, no rwx mapping, store into code faults): ok\n", perms);
}

/* ---------- 10. failure injection on the AVX2 path ---------- */

static void run_failure_injection(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *base = NULL;
    SuNativeArtifact *va = NULL;
    SuNativeDiag d;
    int before;
    int after;

    if (!g_avx_host) {
        printf("failure injection (AVX2): not run (no AVX2 on host)\n");
        return;
    }
    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &base, &d), SU_OK);
    before = su_nt_count_maps();
    su_native_test_inject_failure(SU_NATIVE_FAIL_MMAP);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &va, &d), SU_E_RESOURCE_LIMIT);
    CHECK(va == NULL);
    su_native_test_inject_failure(SU_NATIVE_FAIL_MPROTECT);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &va, &d), SU_E_RESOURCE_LIMIT);
    CHECK(va == NULL);
    su_native_test_inject_failure(0u);
    after = su_nt_count_maps();
    CHECK(before == after && before >= 0);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &va, &d), SU_OK);
    su_native_artifact_free(va);
    su_native_artifact_free(base);
    printf("failure injection (AVX2 mmap and mprotect -> RESOURCE_LIMIT, mapping count unchanged): ok\n");
}

/* ---------- 11. throughput (observation only) ---------- */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t elapsed_ns(const struct timespec *t0, const struct timespec *t1) {
    return (uint64_t)(t1->tv_sec - t0->tv_sec) * 1000000000u + (uint64_t)(t1->tv_nsec - t0->tv_nsec);
}

static void report_stats(const char *label, uint64_t *ns, size_t n, uint64_t rows) {
    qsort(ns, n, sizeof ns[0], cmp_u64);
    printf("%-44s min %9.3f  median %9.3f  p95 %9.3f  max %9.3f ms  (%.3f ns/row, n=%zu)\n", label,
           ns[0] / 1e6, ns[n / 2] / 1e6, ns[(size_t)((n - 1) * 95u / 100u)] / 1e6, ns[n - 1] / 1e6,
           (double)ns[n / 2] / (double)rows, n);
}

static void run_bench_module(const SuLirModule *m, const char *label, uint64_t rows) {
    enum { SAMPLES = 21 };
    uint64_t t_vm[SAMPLES];
    uint64_t t_sc_e2e[SAMPLES];
    uint64_t t_av_e2e[SAMPLES];
    uint64_t t_sc_iso[SAMPLES];
    uint64_t t_av_iso[SAMPLES];
    SuExecContext *vm = NULL;
    SuNativeArtifact *sa = NULL;
    SuNativeArtifact *va = NULL;
    SuNativeDiag d;
    Col cols_in[MAXF];
    Col cols_out[MAXF];
    SuColumnBinding in_b[MAXF];
    SuColumnBinding out_b[MAXF];
    const uint32_t *in_p[MAXF];
    uint32_t *out_p[MAXF];
    uint32_t ni = 0;
    uint32_t nw = 0;
    uint32_t i;
    size_t s;
    uint64_t r;
    uint64_t sd = 99u;
    SuInvocation inv;
    SuOutputs outs;
    SuNativeArgs args;
    struct timespec t0;
    struct timespec t1;

    if (su_vm_context_create(m, 1u, &vm) != SU_OK || su_native_compile(m, 1u, SU_NCAP_X86_64_SCALAR, &sa, &d) != SU_OK ||
        su_native_compile(m, 1u, SU_NCAP_X86_64_AVX2, &va, &d) != SU_OK) {
        fprintf(stderr, "bench: setup failed\n");
        return;
    }
    for (i = 0; i < m->field_count; i++) {
        if ((m->fields[i].access & SU_ACCESS_READ) != 0u) {
            col_alloc(&cols_in[ni], rows);
            for (r = 0; r < rows; r++) {
                cols_in[ni].data[r] = edge_or_random(&sd);
            }
            bind_one(&in_b[ni], m->fields[i].id, SU_ACCESS_READ, rows, cols_in[ni].data);
            in_p[ni] = cols_in[ni].data;
            ni++;
        }
    }
    for (i = 0; i < m->field_count; i++) {
        if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
            col_alloc(&cols_out[nw], rows);
            bind_one(&out_b[nw], m->fields[i].id, SU_ACCESS_WRITE, rows, cols_out[nw].data);
            out_p[nw] = cols_out[nw].data;
            nw++;
        }
    }
    memset(&inv, 0, sizeof inv);
    inv.struct_size = sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = ni;
    inv.row_count = rows;
    inv.bindings = in_b;
    memset(&outs, 0, sizeof outs);
    outs.struct_size = sizeof outs;
    outs.binding_count = nw;
    outs.failed_insn = UINT32_MAX;
    outs.bindings = out_b;
    args.struct_size = sizeof args;
    args.abi_version = SU_NATIVE_ABI_VERSION;
    args.row_count = rows;
    args.in_cols = in_p;
    args.out_cols = out_p;

    for (s = 0; s < SAMPLES; s++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        (void)su_vm_invoke_v1(vm, &inv, &outs);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        t_vm[s] = elapsed_ns(&t0, &t1);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        (void)su_native_run_checked(sa, &inv, &outs);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        t_sc_e2e[s] = elapsed_ns(&t0, &t1);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        (void)su_native_run_checked(va, &inv, &outs);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        t_av_e2e[s] = elapsed_ns(&t0, &t1);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        (void)su_native_entry(sa)(&args);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        t_sc_iso[s] = elapsed_ns(&t0, &t1);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        (void)su_native_entry(va)(&args);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        t_av_iso[s] = elapsed_ns(&t0, &t1);
    }
    printf("%s, %llu rows, %d samples (observation on a shared VM; not a gate)\n", label,
           (unsigned long long)rows, SAMPLES);
    report_stats("reference VM, end-to-end (wrapper incl.)", t_vm, SAMPLES, rows);
    report_stats("scalar native, end-to-end (wrapper incl.)", t_sc_e2e, SAMPLES, rows);
    report_stats("avx2 native, end-to-end (wrapper incl.)", t_av_e2e, SAMPLES, rows);
    report_stats("scalar native, kernel only", t_sc_iso, SAMPLES, rows);
    report_stats("avx2 native, kernel only", t_av_iso, SAMPLES, rows);

    for (i = 0; i < ni; i++) {
        col_free(&cols_in[i]);
    }
    for (i = 0; i < nw; i++) {
        col_free(&cols_out[i]);
    }
    su_native_artifact_free(va);
    su_native_artifact_free(sa);
    su_vm_context_destroy(vm);
}

static void run_bench(void) {
    SuLirModule mv;
    SuLirModule ar;
    SuLirField f[3];
    SuInsn ins[24];
    uint32_t n = 0;

    if (!g_avx_host) {
        printf("bench: not run (no AVX2 on host)\n");
        return;
    }
    if (verify_pair("examples/movement.capsule.json", "examples/movement.lir.json", &mv)) {
        run_bench_module(&mv, "movement-like (examples/movement.lir.json)", 1000000u);
        su_lir_module_free(&mv);
    }

    /* 17 arithmetic/compare/select operations on two inputs, plus two loads and one store. */
    memset(f, 0, sizeof f);
    f[0].id = 1u; f[0].type = SU_TYPE_U32; f[0].access = SU_ACCESS_READ;
    f[1].id = 2u; f[1].type = SU_TYPE_U32; f[1].access = SU_ACCESS_READ;
    f[2].id = 3u; f[2].type = SU_TYPE_U32; f[2].access = SU_ACCESS_WRITE;
    memset(ins, 0, sizeof ins);
    ins[n++] = (SuInsn){SU_OP_LOAD_U32, 0, 0, 0, 0};
    ins[n++] = (SuInsn){SU_OP_LOAD_U32, 1, 1, 0, 0};
    ins[n++] = (SuInsn){SU_OP_ADD_U32, 2, 0, 1, 0};
    ins[n++] = (SuInsn){SU_OP_MUL_U32, 3, 2, 0, 0};
    ins[n++] = (SuInsn){SU_OP_SUB_U32, 4, 3, 1, 0};
    ins[n++] = (SuInsn){SU_OP_LT_U32, 5, 4, 0, 0};
    ins[n++] = (SuInsn){SU_OP_SELECT_U32, 6, 5, 4, 2};
    ins[n++] = (SuInsn){SU_OP_ADD_U32, 7, 6, 3, 0};
    ins[n++] = (SuInsn){SU_OP_MUL_U32, 8, 7, 7, 0};
    ins[n++] = (SuInsn){SU_OP_EQ_U32, 9, 8, 6, 0};
    ins[n++] = (SuInsn){SU_OP_SELECT_U32, 10, 9, 8, 7};
    ins[n++] = (SuInsn){SU_OP_SUB_U32, 11, 10, 0, 0};
    ins[n++] = (SuInsn){SU_OP_ADD_U32, 12, 11, 1, 0};
    ins[n++] = (SuInsn){SU_OP_LT_U32, 13, 12, 10, 0};
    ins[n++] = (SuInsn){SU_OP_SELECT_U32, 14, 13, 12, 11};
    ins[n++] = (SuInsn){SU_OP_MUL_U32, 15, 14, 1, 0};
    ins[n++] = (SuInsn){SU_OP_ADD_U32, 16, 15, 0, 0};
    ins[n++] = (SuInsn){SU_OP_SUB_U32, 17, 16, 12, 0};
    ins[n++] = (SuInsn){SU_OP_MUL_U32, 18, 17, 14, 0};
    ins[n++] = (SuInsn){SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 18, 0};
    memset(&ar, 0, sizeof ar);
    ar.artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    ar.field_count = 3u;
    ar.fields = f;
    ar.register_count = 19u;
    ar.insn_count = n;
    ar.insns = ins;
    run_bench_module(&ar, "arithmetic (17 ops + 2 loads + 1 store, 19 registers)", 1000000u);
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    int bench = 0;
    int ok = 1;
    const char *corpus = NULL;
    int a;

    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--bench") == 0) {
            bench = 1;
        } else {
            corpus = argv[a];
        }
    }
    if (corpus == NULL) {
        fprintf(stderr, "usage: %s <corpus_dir> [--bench]\n", argv[0]);
        return 2;
    }

    run_capabilities();
    ok &= run_known_answers() ? 1 : 0;
    run_frame_bound();
    ok &= run_fixtures() ? 1 : 0;
    ok &= run_corpus(corpus) ? 1 : 0;
    ok &= run_unaligned_offsets() ? 1 : 0;
    run_wrapper_avx2();
    ok &= run_abi_clobber() ? 1 : 0;
    run_metadata();
    run_wx();
    run_failure_injection();
    if (bench) {
        run_bench();
    }

    printf("avx2 differential runs with the AVX2 engine: %d; host AVX2 %s; checks %d, failures %d\n",
           g_avx_runs, g_avx_host ? "usable" : "NOT usable (AVX2 parts not run)", g_checks, g_failures);
    ok = ok && g_failures == 0 && (g_avx_host ? 1 : 0) == (g_avx_runs > 0 ? 1 : 0);
    if (!g_avx_host) {
        /* Without AVX2 nothing AVX2 ran: report it as a failure of the claim, never a pass. */
        printf("result: NOT PASSED (host lacks AVX2; AVX2 validation not run)\n");
        return 1;
    }
    printf("result: %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
