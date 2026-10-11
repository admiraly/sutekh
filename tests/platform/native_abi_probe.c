/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * M1-05 platform probe: runs the M1-01 scalar native kernels through the public native API on the
 * host ABI (SysV on Linux, Win64 on Windows) and compares every output with a C reference written
 * here, not with the VM. It is not a replacement for tests/native/scalar (which uses POSIX mmap and
 * fork and is not portable yet); it is the smallest portable Win64 execution evidence.
 *
 * Checks:
 *  - seven kernels (sub, add, mul, unsigned min, eq-select, max, const) plus a 3000-register chain
 *    whose frame spans several pages (the page-probe path), at row counts 0..1000 including tails;
 *  - unaligned (4-byte-aligned, not 8) column pointers, and guard words on both sides of each
 *    staging column so an out-of-range write is caught;
 *  - host ABI reported by the artifact is the one this binary was compiled for;
 *  - Win64 only: a callee-saved and xmm6-15 clobber check through an assembly trampoline that
 *    follows the Win64 convention, plus a negative control (a deliberately bad kernel that clobbers
 *    rsi) that the same trampoline must detect. SysV clobber coverage stays in tests/native/scalar.
 *
 * Output: one line per failed check, then a summary line. Exit 0 only if every check passed.
 * Win64 clobber code is compiled only with _WIN64 and uses GNU inline assembly (gcc/clang). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backends/native/su_native.h"
#include "sutekh/native/abi.h"

#define GUARD_WORD UINT32_C(0xA5A5A5A5)
#define CHAIN_REGS 3000u

enum kind { K_SUB, K_ADD, K_MUL, K_MIN, K_EQ79, K_MAX, K_CONST, K_CHAIN, K_COUNT };

static const char *const kind_name[K_COUNT] = {"sub", "add", "mul", "min_unsigned", "eq_select",
                                               "max_unsigned", "const", "chain3000"};

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("  FAIL line %d: ", __LINE__);                               \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

/* Field table: 0 = a (id 1, read), 1 = b (id 2, read), 2 = out (id 3, write). */
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

/* Static: the chain program is about 36 KB and must not sit on the stack of each kernel. */
static SuInsn g_chain_storage[CHAIN_REGS + 1];

typedef struct Kern {
    enum kind kind;
    uint32_t caps;        /* SU_NCAP_X86_64_SCALAR or SU_NCAP_X86_64_AVX2 */
    SuInsn *insns;
    uint32_t n_insns;
    uint32_t regs;
    /* The artifact borrows the module and its field table (su_native.h): they must outlive it. */
    SuLirField fields[3];
    SuLirModule module;
    SuNativeArtifact *art;
} Kern;

static uint32_t ref_value(enum kind kind, uint32_t a, uint32_t b) {
    switch (kind) {
    case K_SUB: return a - b;
    case K_ADD: return a + b;
    case K_MUL: return a * b;
    case K_MIN: return a < b ? a : b;
    case K_EQ79: return a == b ? 7u : 9u;
    case K_MAX: return a < b ? b : a;
    case K_CONST: return 0xFFFFFFFFu;
    case K_CHAIN: return a * CHAIN_REGS; /* r_i = (i+1)*a, last register = CHAIN_REGS*a */
    default: return 0u;
    }
}

static void build_kernel(Kern *k, enum kind kind) {
    memset(k, 0, sizeof *k);
    k->kind = kind;
    switch (kind) {
    case K_SUB: k->insns = (SuInsn *)(void *)k_sub; k->n_insns = 4; k->regs = 3; break;
    case K_ADD: k->insns = (SuInsn *)(void *)k_add; k->n_insns = 4; k->regs = 3; break;
    case K_MUL: k->insns = (SuInsn *)(void *)k_mul; k->n_insns = 4; k->regs = 3; break;
    case K_MIN: k->insns = (SuInsn *)(void *)k_min; k->n_insns = 5; k->regs = 4; break;
    case K_EQ79: k->insns = (SuInsn *)(void *)k_eq79; k->n_insns = 7; k->regs = 6; break;
    case K_MAX: k->insns = (SuInsn *)(void *)k_max; k->n_insns = 5; k->regs = 4; break;
    case K_CONST: k->insns = (SuInsn *)(void *)k_const; k->n_insns = 2; k->regs = 1; break;
    case K_CHAIN: {
        /* r0 = a; r_i = r_{i-1} + r0 for i = 1..CHAIN_REGS-1; out = r_{CHAIN_REGS-1}. */
        uint32_t i;
        k->insns = g_chain_storage;
        k->insns[0] = (SuInsn){SU_OP_LOAD_U32, 0, 0, 0, 0};
        for (i = 1; i < CHAIN_REGS; i++) {
            k->insns[i] = (SuInsn){SU_OP_ADD_U32, i, i - 1, 0, 0};
        }
        k->insns[CHAIN_REGS] = (SuInsn){SU_OP_STORE_U32, SU_OPERAND_NONE, 2, CHAIN_REGS - 1, 0};
        k->n_insns = CHAIN_REGS + 1;
        k->regs = CHAIN_REGS;
        break;
    }
    default: break;
    }
}

static uint32_t compile_kernel(Kern *k) {
    SuLirModule *m = &k->module;
    SuNativeDiag d;
    uint32_t st;
    memset(m, 0, sizeof *m);
    memset(k->fields, 0, sizeof k->fields);
    k->fields[0].id = 1u; k->fields[0].type = SU_TYPE_U32; k->fields[0].access = SU_ACCESS_READ;
    k->fields[1].id = 2u; k->fields[1].type = SU_TYPE_U32; k->fields[1].access = SU_ACCESS_READ;
    k->fields[2].id = 3u; k->fields[2].type = SU_TYPE_U32; k->fields[2].access = SU_ACCESS_WRITE;
    m->artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    m->field_count = 3u;
    m->fields = k->fields;
    m->register_count = k->regs;
    m->insn_count = k->n_insns;
    m->insns = k->insns;
    st = su_native_compile(m, 1u, k->caps, &k->art, &d);
    if (st != SU_OK) {
        printf("  compile %s caps=%u: status %u diag %u '%s'\n", kind_name[k->kind], (unsigned)k->caps,
               (unsigned)st, (unsigned)d.code, d.message);
    }
    return st;
}

static uint64_t rng_state;

static uint32_t rng_next(void) {
    uint64_t z;
    rng_state += UINT64_C(0x9E3779B97F4A7C15);
    z = rng_state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    z ^= z >> 31;
    return (uint32_t)z;
}

static uint32_t gen_value(void) {
    static const uint32_t edge[10] = {0u, 1u, 2u, 3u, 7u, 9u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu};
    uint32_t r = rng_next();
    if ((r & 3u) == 0u) {
        return edge[(r >> 8) % 10u];
    }
    return rng_next();
}

/* Runs one kernel over n rows. Columns start at element offset 1 of a guarded buffer, so they
 * are 4-byte aligned but not 8-byte aligned, and one guard word sits on each side. */
static void run_rows(Kern *k, uint32_t n) {
    uint32_t *ba = malloc(((size_t)n + 2u) * 4u);
    uint32_t *bb = malloc(((size_t)n + 2u) * 4u);
    uint32_t *bo = malloc(((size_t)n + 2u) * 4u);
    uint32_t *a;
    uint32_t *b;
    uint32_t *o;
    uint32_t i;
    unsigned bad_values = 0;
    unsigned bad_guard = 0;
    uint32_t st;
    SuColumnBinding in[2];
    SuColumnBinding out[1];
    SuInvocation inv;
    SuOutputs outs;
    if (ba == NULL || bb == NULL || bo == NULL) {
        printf("  FAIL alloc rows=%u\n", (unsigned)n);
        g_failures++;
        g_checks++;
        free(ba);
        free(bb);
        free(bo);
        return;
    }
    a = ba + 1;
    b = bb + 1;
    o = bo + 1;
    ba[0] = ba[n + 1u] = GUARD_WORD;
    bb[0] = bb[n + 1u] = GUARD_WORD;
    bo[0] = bo[n + 1u] = GUARD_WORD;
    for (i = 0; i < n; i++) {
        a[i] = gen_value();
        b[i] = (rng_next() & 1u) ? a[i] : gen_value(); /* equal pairs exercise eq_select */
        o[i] = GUARD_WORD;
    }

    memset(in, 0, sizeof in);
    in[0].struct_size = sizeof in[0]; in[0].field_id = 1u; in[0].elem_type = SU_TYPE_U32;
    in[0].access = SU_ACCESS_READ; in[0].layout_generation = 1u; in[0].stride_bytes = 4u;
    in[0].row_count = n; in[0].data = a;
    in[1].struct_size = sizeof in[1]; in[1].field_id = 2u; in[1].elem_type = SU_TYPE_U32;
    in[1].access = SU_ACCESS_READ; in[1].layout_generation = 1u; in[1].stride_bytes = 4u;
    in[1].row_count = n; in[1].data = b;
    memset(out, 0, sizeof out);
    out[0].struct_size = sizeof out[0]; out[0].field_id = 3u; out[0].elem_type = SU_TYPE_U32;
    out[0].access = SU_ACCESS_WRITE; out[0].layout_generation = 1u; out[0].stride_bytes = 4u;
    out[0].row_count = n; out[0].data = o;

    inv.struct_size = sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = 2u;
    inv.row_count = n;
    inv.bindings = in;
    outs.struct_size = sizeof outs;
    outs.binding_count = 1u;
    outs.failed_insn = UINT32_MAX;
    outs.reserved = 0u;
    outs.bindings = out;

    st = su_native_run_checked(k->art, &inv, &outs);
    CHECK(st == SU_OK, "%s rows=%u: run status %u", kind_name[k->kind], (unsigned)n, (unsigned)st);
    for (i = 0; st == SU_OK && i < n; i++) {
        if (o[i] != ref_value(k->kind, a[i], b[i])) {
            if (bad_values < 3u) {
                printf("  mismatch %s rows=%u row=%u a=%08x b=%08x got=%08x want=%08x\n",
                       kind_name[k->kind], (unsigned)n, (unsigned)i, a[i], b[i], o[i],
                       ref_value(k->kind, a[i], b[i]));
            }
            bad_values++;
        }
    }
    if (ba[0] != GUARD_WORD || ba[n + 1u] != GUARD_WORD || bb[0] != GUARD_WORD ||
        bb[n + 1u] != GUARD_WORD || bo[0] != GUARD_WORD || bo[n + 1u] != GUARD_WORD) {
        bad_guard++;
    }
    CHECK(bad_values == 0u, "%s rows=%u: %u wrong output rows", kind_name[k->kind], (unsigned)n,
          bad_values);
    CHECK(bad_guard == 0u, "%s rows=%u: guard word overwritten", kind_name[k->kind], (unsigned)n);
    free(ba);
    free(bb);
    free(bo);
}

static const uint32_t k_row_counts[] = {0u, 1u, 2u, 3u, 7u, 8u, 9u, 15u, 16u, 17u, 33u, 1000u};

#if defined(_WIN64) && (defined(__GNUC__) || defined(__clang__))
/* uint32_t su_probe_win64_call(fn, args, gpr_out[8], xmm_out[160]) with Win64 arguments in
 * rcx, rdx, r8, r9. Saves rbp rbx rsi rdi r12-r15 and xmm6-15, poisons them, calls fn(args) with a
 * 16-byte aligned stack and 32 bytes of shadow space, then stores the callee-saved GPRs and
 * xmm6-15 low halves seen after the call. The original values are restored before returning. */
__asm__(
    ".text\n"
    ".globl su_probe_win64_call\n"
    "su_probe_win64_call:\n"
    "    push %rbp\n"
    "    push %rbx\n"
    "    push %rsi\n"
    "    push %rdi\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    sub $216, %rsp\n"
    "    mov %r8, 192(%rsp)\n"
    "    mov %r9, 200(%rsp)\n"
    "    movdqu %xmm6, 32(%rsp)\n"
    "    movdqu %xmm7, 48(%rsp)\n"
    "    movdqu %xmm8, 64(%rsp)\n"
    "    movdqu %xmm9, 80(%rsp)\n"
    "    movdqu %xmm10, 96(%rsp)\n"
    "    movdqu %xmm11, 112(%rsp)\n"
    "    movdqu %xmm12, 128(%rsp)\n"
    "    movdqu %xmm13, 144(%rsp)\n"
    "    movdqu %xmm14, 160(%rsp)\n"
    "    movdqu %xmm15, 176(%rsp)\n"
    "    movabs $0x1111111111111111, %rbx\n"
    "    movabs $0x2222222222222222, %rbp\n"
    "    movabs $0x3333333333333333, %rsi\n"
    "    movabs $0x4444444444444444, %rdi\n"
    "    movabs $0x5555555555555555, %r12\n"
    "    movabs $0x6666666666666666, %r13\n"
    "    movabs $0x7777777777777777, %r14\n"
    "    movabs $0x8888888888888888, %r15\n"
    "    movabs $0xA1A1A1A1A1A1A1A1, %rax\n    movq %rax, %xmm6\n"
    "    movabs $0xA2A2A2A2A2A2A2A2, %rax\n    movq %rax, %xmm7\n"
    "    movabs $0xA3A3A3A3A3A3A3A3, %rax\n    movq %rax, %xmm8\n"
    "    movabs $0xA4A4A4A4A4A4A4A4, %rax\n    movq %rax, %xmm9\n"
    "    movabs $0xA5A5A5A5A5A5A5A5, %rax\n    movq %rax, %xmm10\n"
    "    movabs $0xA6A6A6A6A6A6A6A6, %rax\n    movq %rax, %xmm11\n"
    "    movabs $0xA7A7A7A7A7A7A7A7, %rax\n    movq %rax, %xmm12\n"
    "    movabs $0xA8A8A8A8A8A8A8A8, %rax\n    movq %rax, %xmm13\n"
    "    movabs $0xA9A9A9A9A9A9A9A9, %rax\n    movq %rax, %xmm14\n"
    "    movabs $0xAAAAAAAAAAAAAAAA, %rax\n    movq %rax, %xmm15\n"
    "    mov %rcx, %rax\n"
    "    mov %rdx, %rcx\n"
    "    call *%rax\n"
    "    mov 192(%rsp), %r10\n"
    "    mov %rbx, 0(%r10)\n"
    "    mov %rbp, 8(%r10)\n"
    "    mov %rsi, 16(%r10)\n"
    "    mov %rdi, 24(%r10)\n"
    "    mov %r12, 32(%r10)\n"
    "    mov %r13, 40(%r10)\n"
    "    mov %r14, 48(%r10)\n"
    "    mov %r15, 56(%r10)\n"
    "    mov 200(%rsp), %r10\n"
    "    movdqu %xmm6, 0(%r10)\n"
    "    movdqu %xmm7, 16(%r10)\n"
    "    movdqu %xmm8, 32(%r10)\n"
    "    movdqu %xmm9, 48(%r10)\n"
    "    movdqu %xmm10, 64(%r10)\n"
    "    movdqu %xmm11, 80(%r10)\n"
    "    movdqu %xmm12, 96(%r10)\n"
    "    movdqu %xmm13, 112(%r10)\n"
    "    movdqu %xmm14, 128(%r10)\n"
    "    movdqu %xmm15, 144(%r10)\n"
    "    movdqu 32(%rsp), %xmm6\n"
    "    movdqu 48(%rsp), %xmm7\n"
    "    movdqu 64(%rsp), %xmm8\n"
    "    movdqu 80(%rsp), %xmm9\n"
    "    movdqu 96(%rsp), %xmm10\n"
    "    movdqu 112(%rsp), %xmm11\n"
    "    movdqu 128(%rsp), %xmm12\n"
    "    movdqu 144(%rsp), %xmm13\n"
    "    movdqu 160(%rsp), %xmm14\n"
    "    movdqu 176(%rsp), %xmm15\n"
    "    add $216, %rsp\n"
    "    pop %r15\n"
    "    pop %r14\n"
    "    pop %r13\n"
    "    pop %r12\n"
    "    pop %rdi\n"
    "    pop %rsi\n"
    "    pop %rbx\n"
    "    pop %rbp\n"
    "    ret\n"
    /* Negative control: a kernel that breaks the Win64 contract by clobbering rsi (callee-saved on
     * Win64, volatile on SysV). The trampoline must observe the change. */
    ".globl su_probe_bad_clobber\n"
    "su_probe_bad_clobber:\n"
    "    mov $5, %rsi\n"
    "    xor %eax, %eax\n"
    "    ret\n");

uint32_t su_probe_win64_call(SuNativeKernelFn fn, const SuNativeArgs *args, uint64_t gpr_out[8],
                             uint8_t xmm_out[160]);
uint32_t su_probe_bad_clobber(const SuNativeArgs *args);

static const uint64_t k_poison_gpr[8] = {
    UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222), UINT64_C(0x3333333333333333),
    UINT64_C(0x4444444444444444), UINT64_C(0x5555555555555555), UINT64_C(0x6666666666666666),
    UINT64_C(0x7777777777777777), UINT64_C(0x8888888888888888)};

static void check_win64_clobber(Kern *sub, const char *label) {
    uint32_t n = 5u;
    uint32_t a[5] = {9u, 1u, 0xFFFFFFFFu, 4u, 0u};
    uint32_t b[5] = {4u, 2u, 1u, 4u, 0xFFFFFFFFu};
    uint32_t o[5] = {0u, 0u, 0u, 0u, 0u};
    SuNativeArgs args;
    const uint32_t *in_cols[2] = {a, b};
    uint32_t *out_cols[1] = {o};
    uint64_t gpr[8];
    uint8_t xmm[160];
    uint32_t st;
    unsigned i;
    unsigned gpr_bad = 0;
    unsigned xmm_bad = 0;
    unsigned sub_bad = 0;
    SuNativeKernelFn fn = su_native_entry(sub->art);
    SuNativeKernelFn bad;
    static const uint8_t xmm_poison_low[10] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA};

    args.struct_size = sizeof args;
    args.abi_version = SU_NATIVE_ABI_VERSION;
    args.row_count = n;
    args.in_cols = in_cols;
    args.out_cols = out_cols;

    memset(gpr, 0, sizeof gpr);
    memset(xmm, 0, sizeof xmm);
    st = su_probe_win64_call(fn, &args, gpr, xmm);
    CHECK(st == SU_OK, "win64 clobber: kernel status %u", (unsigned)st);
    for (i = 0; i < 8u; i++) {
        if (gpr[i] != k_poison_gpr[i]) {
            gpr_bad++;
        }
    }
    for (i = 0; i < 10u; i++) {
        uint64_t low;
        memcpy(&low, xmm + 16u * i, sizeof low);
        if (low != (UINT64_C(0x0101010101010101) * xmm_poison_low[i])) {
            xmm_bad++;
        }
    }
    CHECK(gpr_bad == 0u, "%s win64 clobber: %u callee-saved GPR(s) changed (rbx rbp rsi rdi r12-r15)",
          label, gpr_bad);
    CHECK(xmm_bad == 0u, "%s win64 clobber: %u xmm6-15 register(s) changed", label, xmm_bad);
    for (i = 0; i < n; i++) {
        if (o[i] != ref_value(K_SUB, a[i], b[i])) {
            sub_bad++;
        }
    }
    CHECK(sub_bad == 0u, "%s win64 clobber: %u subtraction row(s) wrong under the trampoline", label,
          sub_bad);

    /* Negative control: the trampoline must detect the deliberate rsi clobber. */
    memset(gpr, 0, sizeof gpr);
    memset(xmm, 0, sizeof xmm);
    bad = su_probe_bad_clobber;
    st = su_probe_win64_call(bad, &args, gpr, xmm);
    CHECK(st == SU_OK && gpr[2] == 5u, "negative control: trampoline did not observe rsi=5 (got %llx)",
          (unsigned long long)gpr[2]);
    (void)label;
}
#endif

/* Publication faults (SU_NATIVE_FAIL_MMAP / _MPROTECT inject failures into the exec mapping step).
 * On Win64 the second one runs the VirtualFree cleanup after a failed VirtualProtect. The artifact
 * must not be retained, and compile must succeed again once the injection is cleared. */
static void check_publication_faults(void) {
    static const uint32_t masks[2] = {SU_NATIVE_FAIL_MMAP, SU_NATIVE_FAIL_MPROTECT};
    static const char *const names[2] = {"mmap/VirtualAlloc", "mprotect/VirtualProtect"};
    static Kern k;
    uint32_t i;
    for (i = 0; i < 2u; i++) {
        uint32_t st;
        build_kernel(&k, K_CONST);
        k.caps = SU_NCAP_X86_64_SCALAR;
        k.art = NULL;
        su_native_test_inject_failure(masks[i]);
        st = compile_kernel(&k);
        su_native_test_inject_failure(0u);
        CHECK(st == SU_E_RESOURCE_LIMIT && k.art == NULL, "publication fault %s: status %u, artifact %s",
              names[i], (unsigned)st, k.art == NULL ? "not retained" : "retained");
        k.art = NULL;
        st = compile_kernel(&k);
        CHECK(st == SU_OK && k.art != NULL, "publication after %s fault: status %u", names[i], (unsigned)st);
        if (k.art != NULL) {
            su_native_artifact_free(k.art);
            k.art = NULL;
        }
    }
}

static const char *backend_name(uint32_t caps) {
    return caps == SU_NCAP_X86_64_AVX2 ? "avx2" : "scalar";
}

int main(void) {
    static const enum kind kinds[K_COUNT] = {K_SUB, K_ADD, K_MUL, K_MIN, K_EQ79, K_MAX, K_CONST, K_CHAIN};
    /* Index 0: scalar backend. Index 1: AVX2 backend, only when the host reports it. */
    static Kern kern[2][K_COUNT];
    uint32_t host_abi_expected;
    uint32_t host_caps;
    uint32_t bi;
    uint32_t ki;
    uint32_t ri;
    unsigned nbackends = 1u;
    uint32_t backend_caps[2] = {SU_NCAP_X86_64_SCALAR, SU_NCAP_X86_64_AVX2};
    SuNativeArtifactInfo info;

#if defined(_WIN32)
    host_abi_expected = SU_NATIVE_HOST_ABI_WIN64;
#else
    host_abi_expected = SU_NATIVE_HOST_ABI_SYSV;
#endif
    host_caps = su_native_host_caps();
    if ((host_caps & SU_NCAP_X86_64_AVX2) != 0u) {
        nbackends = 2u;
    }
    rng_state = UINT64_C(0x5EED5EED);
    printf("native_abi_probe: host_abi=%s host_caps=0x%x backends=%u\n",
           host_abi_expected == SU_NATIVE_HOST_ABI_WIN64 ? "win64" : "sysv", (unsigned)host_caps,
           (unsigned)nbackends);

    for (bi = 0; bi < nbackends; bi++) {
        for (ki = 0; ki < K_COUNT; ki++) {
            Kern *k = &kern[bi][ki];
            uint32_t st;
            build_kernel(k, kinds[ki]);
            k->caps = backend_caps[bi];
            st = compile_kernel(k);
            if (bi == 1u && kinds[ki] == K_CHAIN) {
                /* The AVX2 frame bound must refuse the 3000-register frame (su_native.h). */
                CHECK(st == SU_E_RESOURCE_LIMIT, "avx2 %s: frame bound returned %u, expected RESOURCE_LIMIT",
                      kind_name[kinds[ki]], (unsigned)st);
                continue;
            }
            CHECK(st == SU_OK, "%s %s: compile status %u", backend_name(k->caps), kind_name[kinds[ki]],
                  (unsigned)st);
            if (st == SU_OK && su_native_artifact_info(k->art, &info) == SU_OK) {
                CHECK(info.host_abi == host_abi_expected, "%s %s: artifact host_abi %u, expected %u",
                      backend_name(k->caps), kind_name[kinds[ki]], (unsigned)info.host_abi,
                      (unsigned)host_abi_expected);
                CHECK(info.required_caps == k->caps, "%s %s: artifact required_caps %u",
                      backend_name(k->caps), kind_name[kinds[ki]], (unsigned)info.required_caps);
            }
        }
    }

    for (bi = 0; bi < nbackends; bi++) {
        for (ki = 0; ki < K_COUNT; ki++) {
            Kern *k = &kern[bi][ki];
            if (k->art == NULL) {
                continue;
            }
            for (ri = 0; ri < sizeof k_row_counts / sizeof k_row_counts[0]; ri++) {
                if (kinds[ki] == K_CHAIN && k_row_counts[ri] > 33u) {
                    continue; /* chain3000 is a frame-size check; a few row counts are enough */
                }
                run_rows(k, k_row_counts[ri]);
            }
        }
    }

    check_publication_faults();

#if defined(_WIN64) && (defined(__GNUC__) || defined(__clang__))
    for (bi = 0; bi < nbackends; bi++) {
        if (kern[bi][K_SUB].art != NULL) {
            check_win64_clobber(&kern[bi][K_SUB], backend_name(backend_caps[bi]));
        }
    }
#else
    printf("  win64 clobber check: not built for this host (SysV clobber is in tests/native/scalar)\n");
#endif

    for (bi = 0; bi < nbackends; bi++) {
        for (ki = 0; ki < K_COUNT; ki++) {
            if (kern[bi][ki].art != NULL) {
                su_native_artifact_free(kern[bi][ki].art);
            }
        }
    }
    printf("native_abi_probe: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u && g_checks > 0u) ? 0 : 1;
}
