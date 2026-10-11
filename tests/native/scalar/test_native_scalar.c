/* M1-01 tests: scalar x86-64 native backend against the reference VM.
 *
 * Sections:
 *  1. hand-computed known answers (literal tables, independent of both implementations)
 *  2. fixture differential: t02, t03, m0_neg valid, edit movement_v2, examples/movement
 *  3. seeded random corpus from gen/gen_corpus.py (argv[1] = corpus directory)
 *  4. wrapper rejections: native returns the VM's status and touches no column
 *  5. ABI: callee-saved clobber trampoline (SysV x86-64 only)
 *  6. W^X: /proc/self/maps permissions, no rwx mapping, a write to the code page faults
 *  7. failure injection: mmap/mprotect failure returns SU_E_RESOURCE_LIMIT, no mapping leak
 *  8. source map invariants, artifact hash sensitivity, disassembly
 *  9. observation: 1M-row timing, VM vs native (printed, not asserted)
 *
 * Columns are mmap'd between PROT_NONE guard pages. Each column ends exactly at a guard page, so
 * an overrun store or load crashes the test. A column starts at element offset 1 of a buffer one
 * element longer than the rows, so it is not 8-byte aligned for odd row counts. Underrun is caught
 * only at page granularity.
 *
 * Run from the repository root: test_native_scalar <corpus-dir>. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "backends/native/su_native.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/native/abi.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
#include "world/su_world.h"

static int g_checks = 0;
static int g_failures = 0;

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

static const uint64_t k_row_counts[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 1024, 1025};
#define N_ROW_COUNTS (sizeof k_row_counts / sizeof k_row_counts[0])
#define MAX_FIELDS_TEST 256u

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

/* Verifies a capsule/LIR pair through the real M0 verifier. On success *m must be freed with
 * su_lir_module_free. */
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

/* ---------- guard-page columns ---------- */

typedef struct Col {
    uint8_t *map;
    size_t map_bytes;
    uint32_t *data;
} Col;

static size_t page_size(void) {
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096u;
}

static int col_alloc(Col *c, uint64_t rows) {
    size_t pg = page_size();
    size_t bytes = (size_t)rows * 4u;
    size_t nd = (bytes + 4u + pg - 1u) / pg;
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
    base = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return 0;
    }
    if (mprotect(base + pg, nd * pg, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, total);
        return 0;
    }
    boundary = base + pg + nd * pg;
    c->map = base;
    c->map_bytes = total;
    c->data = (uint32_t *)(void *)(boundary - bytes);
    return 1;
}

static void col_free(Col *c) {
    if (c->map != NULL) {
        munmap(c->map, c->map_bytes);
    }
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
    static const uint32_t edges[] = {0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
    uint64_t r = sm64(s);
    if ((r & 3u) == 0u) {
        return edges[(r >> 8) % 6u];
    }
    return (uint32_t)(r >> 16);
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

/* Single-input invocation: field 1 readable. */
static void make_inv(SuInvocation *inv, SuColumnBinding *in, uint64_t rows, uint32_t *in_data) {
    bind_one(in, 1u, SU_ACCESS_READ, rows, in_data);
    inv->struct_size = (uint32_t)sizeof *inv;
    inv->abi_version = SU_ABI_VERSION;
    inv->layout_generation = 1u;
    inv->binding_count = 1u;
    inv->row_count = rows;
    inv->bindings = in;
}

/* Single-output staging: field 2 writable. */
static void make_outs(SuOutputs *outs, SuColumnBinding *ob, uint64_t rows, uint32_t *out_data) {
    bind_one(ob, 2u, SU_ACCESS_WRITE, rows, out_data);
    outs->struct_size = (uint32_t)sizeof *outs;
    outs->binding_count = 1u;
    outs->failed_insn = UINT32_MAX;
    outs->reserved = 0u;
    outs->bindings = ob;
}

/* Heap-free module: out[r] = in[r] (LOAD field 0 -> r0; STORE field 1 <- r0). Do not pass it to
 * su_lir_module_free. */
static void make_copy_module(SuLirModule *m, SuLirField f[2], SuInsn ins[2]) {
    memset(m, 0, sizeof *m);
    memset(f, 0, 2u * sizeof f[0]);
    f[0].id = 1u;
    f[0].type = SU_TYPE_U32;
    f[0].access = SU_ACCESS_READ;
    f[1].id = 2u;
    f[1].type = SU_TYPE_U32;
    f[1].access = SU_ACCESS_WRITE;
    ins[0].op = SU_OP_LOAD_U32;
    ins[0].dst = 0u;
    ins[0].a = 0u;
    ins[0].b = 0u;
    ins[0].c = 0u;
    ins[1].op = SU_OP_STORE_U32;
    ins[1].dst = SU_OPERAND_NONE;
    ins[1].a = 1u;
    ins[1].b = 0u;
    ins[1].c = 0u;
    m->artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    m->field_count = 2u;
    m->fields = f;
    m->register_count = 1u;
    m->insn_count = 2u;
    m->insns = ins;
    m->max_rows = 0u;
}

/* ---------- differential run ---------- */

/* Runs VM and native on identical inputs (fresh random columns from seed). Returns 1 when both
 * return SU_OK and every staging column matches byte for byte. */
static int diff_run(const SuLirModule *m, SuExecContext *vm, SuNativeArtifact *art, uint64_t rows,
                    uint64_t seed, const char *tag) {
    static SuColumnBinding in_b[MAX_FIELDS_TEST];
    static SuColumnBinding vo_b[MAX_FIELDS_TEST];
    static SuColumnBinding no_b[MAX_FIELDS_TEST];
    static Col in_c[MAX_FIELDS_TEST];
    static Col vo_c[MAX_FIELDS_TEST];
    static Col no_c[MAX_FIELDS_TEST];
    uint32_t ni = 0;
    uint32_t nw = 0;
    uint32_t i;
    uint32_t j;
    uint64_t s = seed;
    uint64_t r;
    SuInvocation inv;
    SuOutputs vo;
    SuOutputs no;
    uint32_t st_vm;
    uint32_t st_nat;
    int ok = 1;

    memset(in_c, 0, sizeof in_c);
    memset(vo_c, 0, sizeof vo_c);
    memset(no_c, 0, sizeof no_c);
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
            if (!col_alloc(&vo_c[nw], rows) || !col_alloc(&no_c[nw], rows)) {
                ok = 0;
                goto cleanup;
            }
            for (r = 0; r < rows; r++) {
                vo_c[nw].data[r] = 0xA5A5A5A5u;
                no_c[nw].data[r] = 0x5A5A5A5Au;
            }
            bind_one(&vo_b[nw], m->fields[i].id, SU_ACCESS_WRITE, rows, vo_c[nw].data);
            bind_one(&no_b[nw], m->fields[i].id, SU_ACCESS_WRITE, rows, no_c[nw].data);
            nw++;
        }
    }

    inv.struct_size = (uint32_t)sizeof(SuInvocation);
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = ni;
    inv.row_count = rows;
    inv.bindings = in_b;

    vo.struct_size = (uint32_t)sizeof(SuOutputs);
    vo.binding_count = nw;
    vo.failed_insn = UINT32_MAX;
    vo.reserved = 0u;
    vo.bindings = vo_b;
    no = vo;
    no.bindings = no_b;

    st_vm = su_vm_invoke_v1(vm, &inv, &vo);
    st_nat = su_native_run_checked(art, &inv, &no);
    if (st_vm != SU_OK || st_nat != SU_OK) {
        fprintf(stderr, "%s rows=%llu: vm=%s native=%s\n", tag, (unsigned long long)rows,
                su_status_name(st_vm), su_status_name(st_nat));
        ok = 0;
    } else {
        for (j = 0; j < nw; j++) {
            if (memcmp(vo_c[j].data, no_c[j].data, (size_t)rows * 4u) != 0) {
                fprintf(stderr, "%s rows=%llu: staging column %u differs\n", tag,
                        (unsigned long long)rows, j);
                ok = 0;
                break;
            }
        }
    }

cleanup:
    for (i = 0; i < ni; i++) {
        col_free(&in_c[i]);
    }
    for (i = 0; i < nw; i++) {
        col_free(&vo_c[i]);
        col_free(&no_c[i]);
    }
    return ok;
}

/* Compiles m once and runs every row count. */
static int diff_module(const SuLirModule *m, uint64_t seed_base, const char *tag) {
    SuExecContext *vm = NULL;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    size_t k;
    int ok = 1;
    uint32_t st;

    st = su_vm_context_create(m, 1u, &vm);
    if (st != SU_OK) {
        fprintf(stderr, "%s: vm context: %s\n", tag, su_status_name(st));
        return 0;
    }
    st = su_native_compile(m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d);
    if (st != SU_OK) {
        fprintf(stderr, "%s: native compile: %s (%s)\n", tag, su_status_name(st), d.message);
        su_vm_context_destroy(vm);
        return 0;
    }
    for (k = 0; k < N_ROW_COUNTS; k++) {
        if (!diff_run(m, vm, art, k_row_counts[k], seed_base + k, tag)) {
            ok = 0;
            break;
        }
    }
    su_native_artifact_free(art);
    su_vm_context_destroy(vm);
    return ok;
}

/* ---------- 1. known answers ---------- */

typedef struct Kat {
    const char *name;
    const SuInsn *insns;
    uint32_t n_insns;
    uint32_t regs;
    uint32_t n_rows;
    uint32_t rows[8][3]; /* a, b, expected */
} Kat;

/* Field table: 0 = a (id 1, read), 1 = b (id 2, read), 2 = out (id 3, write). */
static uint32_t run_kat(const Kat *k) {
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

    st = su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d);
    CHECK_ST(st, SU_OK);
    if (st != SU_OK) {
        return 1u;
    }
    if (!col_alloc(&ca, k->n_rows) || !col_alloc(&cb, k->n_rows) || !col_alloc(&co, k->n_rows)) {
        su_native_artifact_free(art);
        return 1u;
    }
    for (r = 0; r < k->n_rows; r++) {
        ca.data[r] = k->rows[r][0];
        cb.data[r] = k->rows[r][1];
        co.data[r] = 0xDEADBEEFu;
    }
    bind_one(&in[0], 1u, SU_ACCESS_READ, k->n_rows, ca.data);
    bind_one(&in[1], 2u, SU_ACCESS_READ, k->n_rows, cb.data);
    bind_one(&out[0], 3u, SU_ACCESS_WRITE, k->n_rows, co.data);
    inv.struct_size = sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = 2u;
    inv.row_count = k->n_rows;
    inv.bindings = in;
    outs.struct_size = sizeof outs;
    outs.binding_count = 1u;
    outs.failed_insn = UINT32_MAX;
    outs.reserved = 0u;
    outs.bindings = out;
    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_OK);
    for (r = 0; r < k->n_rows; r++) {
        if (co.data[r] != k->rows[r][2]) {
            fprintf(stderr, "KAT %s row %u: a=%08x b=%08x got %08x want %08x\n", k->name, r,
                    k->rows[r][0], k->rows[r][1], co.data[r], k->rows[r][2]);
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

/* r0 = a, r1 = b. */
static const SuInsn k_sub[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0},
    {SU_OP_SUB_U32, 2, 0, 1, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 2, 0}};
static const SuInsn k_add[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0},
    {SU_OP_ADD_U32, 2, 0, 1, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 2, 0}};
static const SuInsn k_mul[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0},
    {SU_OP_MUL_U32, 2, 0, 1, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 2, 0}};
/* lt(a,b) ? a : b (unsigned minimum) */
static const SuInsn k_min[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0}, {SU_OP_LT_U32, 2, 0, 1, 0},
    {SU_OP_SELECT_U32, 3, 2, 0, 1}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 3, 0}};
/* eq(a,b) ? 7 : 9 */
static const SuInsn k_eq79[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0}, {SU_OP_EQ_U32, 2, 0, 1, 0},
    {SU_OP_CONST_U32, 3, 7, 0, 0}, {SU_OP_CONST_U32, 4, 9, 0, 0},
    {SU_OP_SELECT_U32, 5, 2, 3, 4}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 5, 0}};
/* lt(a,b) ? b : a */
static const SuInsn k_swap[] = {
    {SU_OP_LOAD_U32, 0, 0, 0, 0}, {SU_OP_LOAD_U32, 1, 1, 0, 0}, {SU_OP_LT_U32, 2, 0, 1, 0},
    {SU_OP_SELECT_U32, 3, 2, 1, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 3, 0}};
static const SuInsn k_const[] = {
    {SU_OP_CONST_U32, 0, 0xFFFFFFFFu, 0, 0}, {SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 0, 0}};

static int run_known_answers(void) {
    uint32_t bad = 0;
    Kat k;

    memset(&k, 0, sizeof k);
    k.name = "sub"; k.insns = k_sub; k.n_insns = 4; k.regs = 3; k.n_rows = 6;
    { static const uint32_t rows[6][3] = {{0, 1, 0xFFFFFFFFu}, {1, 2, 0xFFFFFFFFu}, {0xFFFFFFFFu, 0xFFFFFFFFu, 0},
                                           {0x80000000u, 0x80000000u, 0}, {5, 3, 2}, {0, 0xFFFFFFFFu, 1}};
      memcpy(k.rows, rows, sizeof rows); }
    bad += run_kat(&k);

    memset(&k, 0, sizeof k);
    k.name = "add"; k.insns = k_add; k.n_insns = 4; k.regs = 3; k.n_rows = 4;
    { static const uint32_t rows[4][3] = {{0xFFFFFFFFu, 1, 0}, {0x7FFFFFFFu, 1, 0x80000000u},
                                           {0x80000000u, 0x80000000u, 0}, {0xFFFFFFFEu, 3, 1}};
      memcpy(k.rows, rows, sizeof rows); }
    bad += run_kat(&k);

    memset(&k, 0, sizeof k);
    k.name = "mul"; k.insns = k_mul; k.n_insns = 4; k.regs = 3; k.n_rows = 5;
    { static const uint32_t rows[5][3] = {{0x80000000u, 2, 0}, {0xFFFFFFFFu, 0xFFFFFFFFu, 1}, {65536, 65536, 0},
                                           {3, 7, 21}, {0x10001u, 0x10001u, 0x20001u}};
      memcpy(k.rows, rows, sizeof rows); }
    bad += run_kat(&k);

    memset(&k, 0, sizeof k);
    k.name = "min_unsigned"; k.insns = k_min; k.n_insns = 5; k.regs = 4; k.n_rows = 4;
    { static const uint32_t rows[4][3] = {{0xFFFFFFFFu, 1, 1}, {0, 0xFFFFFFFFu, 0}, {5, 5, 5},
                                           {0x80000000u, 0x7FFFFFFFu, 0x7FFFFFFFu}};
      memcpy(k.rows, rows, sizeof rows); }
    bad += run_kat(&k);

    memset(&k, 0, sizeof k);
    k.name = "eq_select"; k.insns = k_eq79; k.n_insns = 7; k.regs = 6; k.n_rows = 4;
    { static const uint32_t rows[4][3] = {{5, 5, 7}, {5, 6, 9}, {0xFFFFFFFFu, 0xFFFFFFFFu, 7},
                                           {0, 0x80000000u, 9}};
      memcpy(k.rows, rows, sizeof rows); }
    bad += run_kat(&k);

    memset(&k, 0, sizeof k);
    k.name = "select_swapped"; k.insns = k_swap; k.n_insns = 5; k.regs = 4; k.n_rows = 4;
    { static const uint32_t rows[4][3] = {{1, 2, 2}, {9, 3, 9}, {3, 9, 9}, {4, 4, 4}};
      memcpy(k.rows, rows, sizeof rows); }
    bad += run_kat(&k);

    memset(&k, 0, sizeof k);
    k.name = "const"; k.insns = k_const; k.n_insns = 2; k.regs = 1; k.n_rows = 3;
    { static const uint32_t rows[3][3] = {{0, 0, 0xFFFFFFFFu}, {1, 2, 0xFFFFFFFFu}, {7, 7, 0xFFFFFFFFu}};
      memcpy(k.rows, rows, sizeof rows); }
    bad += run_kat(&k);

    printf("known answers (7 programs): %s\n", bad == 0u ? "ok" : "FAILED");
    return bad == 0u;
}

/* ---------- 2. fixtures ---------- */

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
        if (!diff_module(&m, 1000u * (uint64_t)i, k_fixture_pairs[i][1])) {
            ok = 0;
        }
        su_lir_module_free(&m);
    }
    printf("fixtures (%zu verified pairs, %zu row counts each): %s\n", n, N_ROW_COUNTS, ok ? "ok" : "FAILED");
    return ok;
}

/* ---------- 3. corpus ---------- */

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
    printf("corpus: %u programs, %u verified, %u failed differential\n", n, verified, failed);
    CHECK(n >= 5000u);
    CHECK(verified == n);
    CHECK(failed == 0u);
    return failed == 0u && n >= 5000u && verified == n;
}

/* ---------- 4. wrapper rejections ---------- */

static void run_wrapper_rejections(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuExecContext *vm = NULL;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    Col ca;
    Col co;
    SuColumnBinding in[1];
    SuColumnBinding out[1];
    SuInvocation inv;
    SuOutputs outs;
    uint32_t vst;
    uint32_t nst;
    uint32_t r;

    make_copy_module(&m, f, ins);
    CHECK_ST(su_vm_context_create(&m, 1u, &vm), SU_OK);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_OK);
    if (vm == NULL || art == NULL || !col_alloc(&ca, 17) || !col_alloc(&co, 17)) {
        CHECK(0);
        su_native_artifact_free(art);
        su_vm_context_destroy(vm);
        return;
    }
    for (r = 0; r < 17; r++) {
        ca.data[r] = r * 3u + 1u;
        co.data[r] = 0xC0FFEE00u + r;
    }
    make_inv(&inv, in, 17, ca.data);
    make_outs(&outs, out, 17, co.data);

    inv.layout_generation = 2u;
    vst = su_vm_invoke_v1(vm, &inv, &outs);
    nst = su_native_run_checked(art, &inv, &outs);
    CHECK_ST(vst, SU_E_SCHEMA_MISMATCH);
    CHECK_ST(nst, SU_E_SCHEMA_MISMATCH);
    inv.layout_generation = 1u;

    inv.struct_size = 8u;
    vst = su_vm_invoke_v1(vm, &inv, &outs);
    nst = su_native_run_checked(art, &inv, &outs);
    CHECK_ST(vst, SU_E_INVALID_ARGUMENT);
    CHECK_ST(nst, SU_E_INVALID_ARGUMENT);
    inv.struct_size = (uint32_t)sizeof inv;

    inv.binding_count = 0u;
    vst = su_vm_invoke_v1(vm, &inv, &outs);
    nst = su_native_run_checked(art, &inv, &outs);
    CHECK_ST(vst, SU_E_INVALID_ARGUMENT);
    CHECK_ST(nst, SU_E_INVALID_ARGUMENT);
    inv.binding_count = 1u;

    in[0].row_count = 16u;
    vst = su_vm_invoke_v1(vm, &inv, &outs);
    nst = su_native_run_checked(art, &inv, &outs);
    CHECK_ST(vst, SU_E_SCHEMA_MISMATCH);
    CHECK_ST(nst, SU_E_SCHEMA_MISMATCH);
    in[0].row_count = 17u;

    /* staging aliases the input: rejected before any store */
    out[0].data = ca.data;
    vst = su_vm_invoke_v1(vm, &inv, &outs);
    nst = su_native_run_checked(art, &inv, &outs);
    CHECK_ST(vst, SU_E_INVALID_ARGUMENT);
    CHECK_ST(nst, SU_E_INVALID_ARGUMENT);
    for (r = 0; r < 17; r++) {
        CHECK(ca.data[r] == r * 3u + 1u);
    }
    out[0].data = co.data;

    /* row limit below the invocation: nothing is written */
    CHECK_ST(su_vm_context_set_row_limit(vm, 10u), SU_OK);
    CHECK_ST(su_native_set_row_limit(art, 10u), SU_OK);
    vst = su_vm_invoke_v1(vm, &inv, &outs);
    nst = su_native_run_checked(art, &inv, &outs);
    CHECK_ST(vst, SU_E_RESOURCE_LIMIT);
    CHECK_ST(nst, SU_E_RESOURCE_LIMIT);
    CHECK(co.data[0] == 0xC0FFEE00u && co.data[16] == 0xC0FFEE10u);
    CHECK_ST(su_native_set_row_limit(art, SU_LIMIT_ROWS), SU_OK);
    CHECK_ST(su_vm_context_set_row_limit(vm, SU_LIMIT_ROWS), SU_OK);

    /* a valid invocation afterwards: both succeed and agree */
    CHECK_ST(su_vm_invoke_v1(vm, &inv, &outs), SU_OK);
    for (r = 0; r < 17; r++) {
        CHECK(co.data[r] == ca.data[r]);
    }
    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_OK);
    for (r = 0; r < 17; r++) {
        CHECK(co.data[r] == ca.data[r]);
    }

    col_free(&ca);
    col_free(&co);
    su_native_artifact_free(art);
    su_vm_context_destroy(vm);
}

/* ---------- 5. ABI clobber trampoline ---------- */

#if defined(__x86_64__) && !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
/* uint32_t su_test_clobber_call(SuNativeKernelFn fn, const SuNativeArgs *args, uint64_t regs[7])
 * Poisons rbx rbp r12-r15, calls fn(args) with rsp 16-byte aligned at the call, then stores the
 * six callee-saved values and the status into regs[0..6]. */
__asm__(
    ".text\n"
    ".globl su_test_clobber_call\n"
    ".type su_test_clobber_call,@function\n"
    "su_test_clobber_call:\n"
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
    ".size su_test_clobber_call, .-su_test_clobber_call\n");

uint32_t su_test_clobber_call(SuNativeKernelFn fn, const SuNativeArgs *args, uint64_t regs[7]);

static int run_abi_clobber(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    SuNativeKernelFn fn;
    Col ca;
    Col co;
    SuNativeArgs args;
    const uint32_t *in_cols[1];
    uint32_t *out_cols[1];
    uint64_t regs[7];
    uint32_t st;
    uint32_t r;
    int ok = 1;
    static const uint64_t want[6] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull,
                                     0x4444444444444444ull, 0x5555555555555555ull, 0x6666666666666666ull};

    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_OK);
    fn = su_native_entry(art);
    if (fn == NULL || !col_alloc(&ca, 33) || !col_alloc(&co, 33)) {
        CHECK(0);
        su_native_artifact_free(art);
        return 0;
    }
    for (r = 0; r < 33; r++) {
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
    memset(regs, 0, sizeof regs);
    st = su_test_clobber_call(fn, &args, regs);
    CHECK(st == SU_OK);
    for (r = 0; r < 6; r++) {
        if (regs[r] != want[r]) {
            fprintf(stderr, "callee-saved register %u clobbered: %016llx\n", r, (unsigned long long)regs[r]);
            ok = 0;
        }
    }
    CHECK((uint32_t)regs[6] == SU_OK);
    for (r = 0; r < 33; r++) {
        if (co.data[r] != ca.data[r]) {
            fprintf(stderr, "clobber call: row %u not copied\n", r);
            ok = 0;
            break;
        }
    }
    col_free(&ca);
    col_free(&co);
    su_native_artifact_free(art);
    printf("ABI clobber trampoline (SysV x86-64, rbx rbp r12-r15 poisoned and checked): %s\n", ok ? "ok" : "FAILED");
    return ok;
}
#else
static int run_abi_clobber(void) {
    printf("ABI clobber trampoline: not run (needs SysV x86-64 with GNU-compatible asm)\n");
    return 1;
}
#endif

/* ---------- 6. W^X ---------- */

/* Counts /proc/self/maps lines, or -1. */
static int count_maps(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int n = 0;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        n++;
    }
    fclose(f);
    return n;
}

/* Permission string of the mapping containing addr; sets *any_rwx when any mapping is rwx. */
static int perms_at(uintptr_t addr, char perms[8], int *any_rwx) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    *any_rwx = 0;
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long lo = 0;
        unsigned long hi = 0;
        char p[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, p) == 3) {
            if (p[0] == 'r' && p[1] == 'w' && p[2] == 'x') {
                *any_rwx = 1;
            }
            if (!found && addr >= (uintptr_t)lo && addr < (uintptr_t)hi) {
                memcpy(perms, p, sizeof p);
                found = 1;
            }
        }
    }
    fclose(f);
    return found;
}

static int run_wx(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    SuNativeKernelFn fn;
    void *fn_addr;
    char perms[8] = {0};
    int any_rwx = 0;
    int ok = 1;
    const uint8_t *code = NULL;
    size_t size = 0;
    pid_t pid;
    int status = 0;

    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_OK);
    fn = su_native_entry(art);
    if (fn == NULL || su_native_code(art, &code, &size) != SU_OK) {
        su_native_artifact_free(art);
        return 0;
    }
    fn_addr = NULL;
    memcpy(&fn_addr, &fn, sizeof fn);
    if (!perms_at((uintptr_t)fn_addr, perms, &any_rwx)) {
        fprintf(stderr, "W^X: code mapping not found in /proc/self/maps\n");
        ok = 0;
    } else if (strcmp(perms, "r-xp") != 0) {
        fprintf(stderr, "W^X: code mapping permissions %s, want r-xp\n", perms);
        ok = 0;
    }
    if (any_rwx) {
        fprintf(stderr, "W^X: an rwx mapping exists in this process\n");
        ok = 0;
    }
    /* A store into the code page must fault. The child is killed by the fault or exits nonzero
     * under a sanitizer runtime; a clean exit means the store succeeded. */
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        volatile uint8_t *p = (volatile uint8_t *)(void *)code;
        *p = 0x90u;
        _exit(0);
    }
    if (pid < 0) {
        ok = 0;
    } else {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            fprintf(stderr, "W^X: store into the code page did not fault\n");
            ok = 0;
        }
    }
    CHECK(ok);
    su_native_artifact_free(art);
    printf("W^X (code mapping r-x, no rwx mapping, store into code faults): %s\n", ok ? "ok" : "FAILED");
    return ok;
}

/* ---------- 7. failure injection ---------- */

static int run_failure_injection(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuNativeArtifact *keep = NULL;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    int before;
    int after;
    uint32_t mask;
    uint32_t r;
    int ok = 1;
    Col ca;
    Col co;
    SuColumnBinding in[1];
    SuColumnBinding out[1];
    SuInvocation inv;
    SuOutputs outs;

    make_copy_module(&m, f, ins);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &keep, &d), SU_OK);
    if (keep == NULL) {
        return 0;
    }
    for (mask = SU_NATIVE_FAIL_MMAP; mask <= SU_NATIVE_FAIL_MPROTECT; mask <<= 1) {
        before = count_maps();
        su_native_test_inject_failure(mask);
        art = NULL;
        memset(&d, 0, sizeof d);
        CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_E_RESOURCE_LIMIT);
        su_native_test_inject_failure(0u);
        if (art != NULL) {
            fprintf(stderr, "failure injection: artifact returned on failure\n");
            ok = 0;
        }
        after = count_maps();
        if (before != after || before < 0) {
            fprintf(stderr, "failure injection mask %u: mappings %d -> %d\n", mask, before, after);
            ok = 0;
        }
    }

    /* The previously installed implementation is still active and correct. */
    if (!col_alloc(&ca, 5) || !col_alloc(&co, 5)) {
        su_native_artifact_free(keep);
        return 0;
    }
    for (r = 0; r < 5; r++) {
        ca.data[r] = r + 10u;
        co.data[r] = 0u;
    }
    make_inv(&inv, in, 5, ca.data);
    make_outs(&outs, out, 5, co.data);
    CHECK_ST(su_native_run_checked(keep, &inv, &outs), SU_OK);
    for (r = 0; r < 5; r++) {
        CHECK(co.data[r] == r + 10u);
    }
    col_free(&ca);
    col_free(&co);
    su_native_artifact_free(keep);
    printf("failure injection (mmap, mprotect: SU_E_RESOURCE_LIMIT, no mapping leak, old artifact intact): %s\n",
           ok ? "ok" : "FAILED");
    return ok;
}

/* ---------- 8. source map, hash, disassembly ---------- */

static int run_metadata(void) {
    SuLirModule m;
    SuNativeArtifact *a1 = NULL;
    SuNativeArtifact *a2 = NULL;
    SuNativeArtifact *a3 = NULL;
    SuNativeArtifact *a4 = NULL;
    SuNativeDiag d;
    SuNativeArtifactInfo i1;
    SuNativeArtifactInfo i3;
    const SuNativeSourceRange *rg = NULL;
    uint32_t nr = 0;
    uint32_t k;
    const uint8_t *c1 = NULL;
    const uint8_t *c2 = NULL;
    const uint8_t *c3 = NULL;
    size_t s1 = 0;
    size_t s2 = 0;
    size_t s3 = 0;
    static char text[65536];
    size_t tlen = 0;
    uint32_t nret = 0;
    uint32_t nimul = 0;
    uint32_t nmul_insn = 0;
    SuLirModule m2;
    const char *p;
    int ok_before = g_failures;

    memset(&m, 0, sizeof m);
    if (!verify_pair("tests/fixtures/t03/mul_overflow/capsule.json", "tests/fixtures/t03/mul_overflow/program.lir.json",
                     &m)) {
        return 0;
    }
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &a1, &d), SU_OK);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &a2, &d), SU_OK);
    if (a1 == NULL || a2 == NULL) {
        su_lir_module_free(&m);
        return 0;
    }

    CHECK_ST(su_native_source_map(a1, &rg, &nr), SU_OK);
    CHECK(nr == m.insn_count);
    CHECK_ST(su_native_code(a1, &c1, &s1), SU_OK);
    for (k = 0; k < nr; k++) {
        CHECK(rg[k].insn == k);
        CHECK(rg[k].begin < rg[k].end);
        CHECK(rg[k].end <= s1);
        CHECK(rg[k].reserved == 0u);
        if (k > 0u) {
            CHECK(rg[k].begin >= rg[k - 1u].end);
        }
    }

    CHECK_ST(su_native_artifact_info(a1, &i1), SU_OK);
    CHECK_ST(su_native_artifact_info(a2, &i3), SU_OK);
    CHECK(memcmp(i1.artifact_hash, i3.artifact_hash, SU_HASH_BYTES) == 0);
    CHECK_ST(su_native_code(a2, &c2, &s2), SU_OK);
    CHECK(s1 == s2 && memcmp(c1, c2, s1) == 0);
    CHECK(i1.struct_size == sizeof i1 && i1.abi_version == SU_NATIVE_ABI_VERSION);
    CHECK(i1.artifact_format == SU_ARTIFACT_FORMAT_VERSION);
    CHECK(i1.backend_id == SU_NATIVE_BACKEND_ASMJIT_SCALAR && i1.reserved0 == 0u && i1.reserved1 == 0u);
    CHECK(i1.required_caps == SU_NCAP_X86_64_SCALAR);
    CHECK(i1.insn_count == m.insn_count && i1.code_size == s1);
    CHECK(memcmp(i1.semantic_hash, m.semantic_hash, SU_HASH_BYTES) == 0);

    /* layout generation changes the artifact hash, not the code */
    CHECK_ST(su_native_compile(&m, 2u, SU_NCAP_X86_64_SCALAR, &a3, &d), SU_OK);
    if (a3 != NULL) {
        SuNativeArtifactInfo i2;
        CHECK_ST(su_native_artifact_info(a3, &i2), SU_OK);
        CHECK(memcmp(i1.artifact_hash, i2.artifact_hash, SU_HASH_BYTES) != 0);
        CHECK(memcmp(i1.semantic_hash, i2.semantic_hash, SU_HASH_BYTES) == 0);
        CHECK_ST(su_native_code(a3, &c3, &s3), SU_OK);
        CHECK(s1 == s3 && memcmp(c1, c3, s1) == 0);
    }

    /* semantic hash changes the artifact hash, not the code */
    m2 = m;
    m2.semantic_hash[0] ^= 0x01u;
    CHECK_ST(su_native_compile(&m2, 1u, SU_NCAP_X86_64_SCALAR, &a4, &d), SU_OK);
    if (a4 != NULL) {
        CHECK_ST(su_native_artifact_info(a4, &i3), SU_OK);
        CHECK(memcmp(i1.artifact_hash, i3.artifact_hash, SU_HASH_BYTES) != 0);
    }

    /* caps word is hashed (the hash function is the compile path's hash) */
    {
        uint8_t h1[SU_HASH_BYTES];
        uint8_t h2[SU_HASH_BYTES];
        su_native_test_hash(m.semantic_hash, 1u, SU_NCAP_X86_64_SCALAR, h1);
        su_native_test_hash(m.semantic_hash, 1u, SU_NCAP_X86_64_SCALAR | SU_NCAP_X86_64_AVX2, h2);
        CHECK(memcmp(h1, h2, SU_HASH_BYTES) != 0);
        CHECK(memcmp(h1, i1.artifact_hash, SU_HASH_BYTES) == 0);
    }
    {
        SuNativeArtifact *bad = NULL;
        CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &bad, &d), SU_E_CAPABILITY_DENIED);
        CHECK(bad == NULL);
    }

    /* disassembly: RET count is the two exits (row loop, zero-row path); IMUL count equals the
     * module's MUL count. */
    CHECK_ST(su_native_disassemble(a1, text, sizeof text, &tlen), SU_OK);
    CHECK(tlen > 0u);
    for (k = 0; k < m.insn_count; k++) {
        if (m.insns[k].op == SU_OP_MUL_U32) {
            nmul_insn++;
        }
    }
    p = text;
    while (*p != '\0') {
        const char *line_end = strchr(p, '\n');
        const char *s = p;
        char mnem[16];
        size_t q = 0;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        while (*s != '\0' && *s != ' ' && *s != '\t' && *s != '\n' && q + 1 < sizeof mnem) {
            mnem[q++] = *s++;
        }
        mnem[q] = '\0';
        if (strcmp(mnem, "ret") == 0) {
            nret++;
        }
        if (strcmp(mnem, "imul") == 0) {
            nimul++;
        }
        if (line_end == NULL) {
            break;
        }
        p = line_end + 1;
    }
    CHECK(nret == 2u);
    CHECK(nimul == nmul_insn);
    CHECK(nimul > 0u);
    {
        size_t need = 0;
        CHECK_ST(su_native_disassemble(a1, text, tlen, &need), SU_E_RESOURCE_LIMIT);
        CHECK(need == tlen);
    }
    printf("source map, hash and disassembly (%zu bytes text, %u ret, %u imul): %s\n", tlen, nret, nimul,
           g_failures == ok_before ? "ok" : "FAILED");

    su_native_artifact_free(a1);
    su_native_artifact_free(a2);
    su_native_artifact_free(a3);
    su_native_artifact_free(a4);
    su_lir_module_free(&m);
    return g_failures == ok_before;
}

/* ---------- 8b. large frame: 4000 registers (16000-byte frame, stack probing path) ---------- */

static int run_large_frame(void) {
    enum { kRegs = 4000 };
    static SuInsn big[kRegs + 2];
    SuLirField f[2];
    SuLirModule m;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    SuExecContext *vm = NULL;
    Col ca;
    Col co;
    SuColumnBinding in[1];
    SuColumnBinding out[1];
    SuInvocation inv;
    SuOutputs outs;
    uint32_t n = 0;
    uint32_t r;
    uint32_t i;
    int ok = 1;

    make_copy_module(&m, f, big);
    big[n].op = SU_OP_LOAD_U32; big[n].dst = 0u; big[n].a = 0u; big[n].b = 0u; big[n].c = 0u; n++;
    for (i = 1; i < kRegs - 1u; i++) {
        big[n].op = SU_OP_CONST_U32; big[n].dst = i; big[n].a = i; big[n].b = 0u; big[n].c = 0u; n++;
    }
    /* r(kRegs-1) = r0 + r(kRegs-2) = in + (kRegs-2) */
    big[n].op = SU_OP_ADD_U32; big[n].dst = kRegs - 1u; big[n].a = 0u; big[n].b = kRegs - 2u; big[n].c = 0u; n++;
    big[n].op = SU_OP_STORE_U32; big[n].dst = SU_OPERAND_NONE; big[n].a = 1u; big[n].b = kRegs - 1u; big[n].c = 0u; n++;
    m.register_count = kRegs;
    m.insn_count = n;
    m.insns = big;

    CHECK_ST(su_vm_context_create(&m, 1u, &vm), SU_OK);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_OK);
    if (vm == NULL || art == NULL || !col_alloc(&ca, 17) || !col_alloc(&co, 17)) {
        CHECK(0);
        su_native_artifact_free(art);
        su_vm_context_destroy(vm);
        return 0;
    }
    for (r = 0; r < 17; r++) {
        ca.data[r] = 0xFFFFFFF0u + r; /* wraps past 2^32 */
        co.data[r] = 0u;
    }
    make_inv(&inv, in, 17, ca.data);
    make_outs(&outs, out, 17, co.data);
    CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_OK);
    for (r = 0; r < 17; r++) {
        uint32_t want = (0xFFFFFFF0u + r) + (uint32_t)(kRegs - 2);
        if (co.data[r] != want) {
            fprintf(stderr, "large frame row %u: got %08x want %08x\n", r, co.data[r], want);
            ok = 0;
        }
    }
    CHECK_ST(su_vm_invoke_v1(vm, &inv, &outs), SU_OK);
    for (r = 0; r < 17; r++) {
        if (co.data[r] != (uint32_t)((0xFFFFFFF0u + r) + (uint32_t)(kRegs - 2))) {
            ok = 0;
        }
    }
    col_free(&ca);
    col_free(&co);
    su_native_artifact_free(art);
    su_vm_context_destroy(vm);
    printf("large frame (4000 registers, 16000-byte frame with page probes): %s\n", ok ? "ok" : "FAILED");
    return ok;
}

/* ---------- 9. observation ---------- */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t elapsed_ns(const struct timespec *t0, const struct timespec *t1) {
    return (uint64_t)((t1->tv_sec - t0->tv_sec) * 1000000000LL + (t1->tv_nsec - t0->tv_nsec));
}

static void run_timing(void) {
    SuLirField f[2];
    SuInsn ins[2];
    SuLirModule m;
    SuExecContext *vm = NULL;
    SuNativeArtifact *art = NULL;
    SuNativeDiag d;
    Col ca;
    Col co;
    SuColumnBinding in[1];
    SuColumnBinding out[1];
    SuInvocation inv;
    SuOutputs outs;
    uint64_t tv[5];
    uint64_t tn[5];
    const uint64_t rows = 1000000u;
    uint32_t rep;
    uint64_t s = 42u;
    uint64_t r;
    struct timespec t0;
    struct timespec t1;

    make_copy_module(&m, f, ins);
    CHECK_ST(su_vm_context_create(&m, 1u, &vm), SU_OK);
    CHECK_ST(su_vm_context_set_row_limit(vm, rows), SU_OK);
    CHECK_ST(su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &art, &d), SU_OK);
    CHECK_ST(su_native_set_row_limit(art, rows), SU_OK);
    if (vm == NULL || art == NULL || !col_alloc(&ca, rows) || !col_alloc(&co, rows)) {
        CHECK(0);
        su_native_artifact_free(art);
        su_vm_context_destroy(vm);
        return;
    }
    for (r = 0; r < rows; r++) {
        ca.data[r] = edge_or_random(&s);
        co.data[r] = 0u;
    }
    make_inv(&inv, in, rows, ca.data);
    make_outs(&outs, out, rows, co.data);
    for (rep = 0; rep < 5; rep++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK_ST(su_vm_invoke_v1(vm, &inv, &outs), SU_OK);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        tv[rep] = elapsed_ns(&t0, &t1);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK_ST(su_native_run_checked(art, &inv, &outs), SU_OK);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        tn[rep] = elapsed_ns(&t0, &t1);
    }
    qsort(tv, 5, sizeof tv[0], cmp_u64);
    qsort(tn, 5, sizeof tn[0], cmp_u64);
    printf("observation, copy kernel, 1M rows, n=5 each, includes wrapper checks (not a benchmark):\n");
    printf("  reference VM  min %.2f ms  median %.2f ms  max %.2f ms\n", tv[0] / 1e6, tv[2] / 1e6, tv[4] / 1e6);
    printf("  native scalar min %.2f ms  median %.2f ms  max %.2f ms\n", tn[0] / 1e6, tn[2] / 1e6, tn[4] / 1e6);
    col_free(&ca);
    col_free(&co);
    su_native_artifact_free(art);
    su_vm_context_destroy(vm);
}

int main(int argc, char **argv) {
    const char *corpus = argc > 1 ? argv[1] : NULL;
    int ok = 1;
    if (corpus == NULL) {
        fprintf(stderr, "usage: test_native_scalar <corpus-dir>\n");
        return 2;
    }
    ok &= run_known_answers();
    ok &= run_fixtures();
    ok &= run_corpus(corpus);
    run_wrapper_rejections();
    ok &= run_abi_clobber();
    ok &= run_wx();
    ok &= run_failure_injection();
    ok &= run_metadata();
    ok &= run_large_frame();
    run_timing();
    if (g_failures != 0 || !ok) {
        fprintf(stderr, "test_native_scalar: FAILED (%d of %d checks failed)\n", g_failures, g_checks);
        return 1;
    }
    printf("test_native_scalar: all %d checks passed\n", g_checks);
    return 0;
}
