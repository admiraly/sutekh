/* M1 isolated native throughput driver (observation only; never a qualification gate, C10).
 *
 * For each program x alignment x row count it first VERIFIES every engine against the reference
 * VM (exact output-column equality, all statuses SU_OK) and only then times it. Timed slots:
 *   vm_wrapper      su_vm_invoke_v1 (reference VM, checked wrapper)
 *   scalar_wrapper  su_native_run_checked, SU_NCAP_X86_64_SCALAR
 *   scalar_kernel   su_native_entry(...) call only (no wrapper checks)
 *   avx2_wrapper    su_native_run_checked, SU_NCAP_X86_64_AVX2 (only when the host reports AVX2)
 *   avx2_kernel     su_native_entry(...) call only
 * The VM has no separate kernel, so no vm_kernel slot exists.
 *
 * Slots run interleaved with a rotating start slot per sample. Each sample is the total time of K
 * back-to-back invocations divided by K by the Python driver; K is calibrated during warm-up so a
 * sample lasts at least about 2 ms (K = 1 for 1,000,000 rows).
 *
 * Output (stdout, tab separated, parsed by bench/native/throughput.py):
 *   HOST      avx2 <0|1>
 *   ASMJIT    <commit>
 *   CONFIG    <program> <align> <rows> <bytes_per_row> <K:slot>...
 *   VERIFY    <program> <align> <rows> <slot> pass
 *   SAMPLE    <program> <align> <rows> <slot> <K> <index> <total_ns>
 * Exit status 0 only if every verification passed and every sample was taken.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backends/native/su_native.h"
#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/native/abi.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"

#define SAMPLES 21u
#define WARMUP 3u
#define MIN_SAMPLE_NS 2000000u
#define MAX_K 1000000u
#define MAX_FIELDS 3u
#define MAX_INSNS 24u
#define SLOTS 5u

enum { SLOT_VM = 0, SLOT_SC_WRAP = 1, SLOT_SC_KERN = 2, SLOT_AV_WRAP = 3, SLOT_AV_KERN = 4 };
static const char *const k_slot_name[SLOTS] = {"vm_wrapper", "scalar_wrapper", "scalar_kernel",
                                               "avx2_wrapper", "avx2_kernel"};

typedef struct Prog {
    const char *name;
    uint32_t field_count;
    SuLirField field[MAX_FIELDS];
    uint32_t insn_count;
    SuInsn insn[MAX_INSNS];
    uint32_t register_count;
    uint32_t bytes_per_row; /* sum over bound columns of the element size (all u32 = 4) */
} Prog;

/* Column storage. The aligned variant starts on a 64-byte boundary; the misaligned variant starts
 * one u32 element (4 bytes) after that boundary. Both have room for rows + 1 elements. */
typedef struct Buf {
    void *base;
    uint32_t *data;
} Buf;

static int buf_alloc(Buf *b, uint64_t rows, int misaligned) {
    size_t bytes = (size_t)(rows + 2u) * 4u;
    size_t rounded = (bytes + 63u) & ~(size_t)63u;
    b->base = aligned_alloc(64u, rounded);
    if (b->base == NULL) {
        return 0;
    }
    b->data = (uint32_t *)b->base + (misaligned ? 1 : 0);
    return 1;
}

static void buf_free(Buf *b) {
    free(b->base);
    b->base = NULL;
    b->data = NULL;
}

static uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static void bind(SuColumnBinding *b, uint32_t field_id, uint32_t access, uint64_t rows, uint32_t *data) {
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

/* Program 0: movement-like, the shape of examples/movement.lir.json (2 loads, 1 add, 1 store). */
static void make_movement(Prog *p) {
    memset(p, 0, sizeof *p);
    p->name = "movement";
    p->field_count = 2u;
    p->field[0] = (SuLirField){1u, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, NULL};
    p->field[1] = (SuLirField){2u, SU_TYPE_U32, SU_ACCESS_READ, NULL};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_LOAD_U32, 0, 0, 0, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_LOAD_U32, 1, 1, 0, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_ADD_U32, 2, 0, 1, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_STORE_U32, SU_OPERAND_NONE, 0, 2, 0};
    p->register_count = 3u;
    p->bytes_per_row = 3u * 4u; /* read position, read velocity, write position */
}

/* Program 1: 20 instructions (2 loads, 17 arithmetic/compare/select ops, 1 store), 19 registers. */
static void make_arith(Prog *p) {
    memset(p, 0, sizeof *p);
    p->name = "arith20";
    p->field_count = 3u;
    p->field[0] = (SuLirField){1u, SU_TYPE_U32, SU_ACCESS_READ, NULL};
    p->field[1] = (SuLirField){2u, SU_TYPE_U32, SU_ACCESS_READ, NULL};
    p->field[2] = (SuLirField){3u, SU_TYPE_U32, SU_ACCESS_WRITE, NULL};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_LOAD_U32, 0, 0, 0, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_LOAD_U32, 1, 1, 0, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_ADD_U32, 2, 0, 1, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_MUL_U32, 3, 2, 0, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_SUB_U32, 4, 3, 1, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_LT_U32, 5, 4, 0, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_SELECT_U32, 6, 5, 4, 2};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_ADD_U32, 7, 6, 3, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_MUL_U32, 8, 7, 7, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_EQ_U32, 9, 8, 6, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_SELECT_U32, 10, 9, 8, 7};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_SUB_U32, 11, 10, 0, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_ADD_U32, 12, 11, 1, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_LT_U32, 13, 12, 10, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_SELECT_U32, 14, 13, 12, 11};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_MUL_U32, 15, 14, 1, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_ADD_U32, 16, 15, 0, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_SUB_U32, 17, 16, 12, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_MUL_U32, 18, 17, 14, 0};
    p->insn[p->insn_count++] = (SuInsn){SU_OP_STORE_U32, SU_OPERAND_NONE, 2, 18, 0};
    p->register_count = 19u;
    p->bytes_per_row = 3u * 4u; /* two read fields and one write field */
}

static void make_module(const Prog *p, SuLirModule *m) {
    memset(m, 0, sizeof *m);
    m->artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    m->field_count = p->field_count;
    m->fields = (SuLirField *)p->field; /* borrowed; the Prog outlives the module */
    m->register_count = p->register_count;
    m->insn_count = p->insn_count;
    m->insns = (SuInsn *)p->insn;
    m->max_rows = 0; /* unlimited up to SU_LIMIT_ROWS (1,000,000) */
}

/* One configuration: bindings, the invocation, outputs and the kernel argument block. */
typedef struct Cfg {
    uint64_t rows;
    uint32_t in_n;
    uint32_t out_n;
    Buf in[MAX_FIELDS];
    Buf out[MAX_FIELDS];
    const uint32_t *in_p[MAX_FIELDS];
    uint32_t *out_p[MAX_FIELDS];
    SuColumnBinding in_b[MAX_FIELDS];
    SuColumnBinding out_b[MAX_FIELDS];
    SuInvocation inv;
    SuOutputs outs;
    SuNativeArgs args;
    uint32_t *ref;     /* reference VM output, one column per written field */
    uint32_t *ref_col[MAX_FIELDS];
} Cfg;

static int cfg_setup(Cfg *c, const Prog *p, uint64_t rows, int misaligned, uint64_t seed) {
    uint32_t i;
    uint32_t r;
    uint64_t s = seed;
    uint32_t ni = 0;
    uint32_t nw = 0;

    memset(c, 0, sizeof *c);
    c->rows = rows;
    for (i = 0; i < p->field_count; i++) {
        if ((p->field[i].access & SU_ACCESS_READ) != 0u) {
            if (!buf_alloc(&c->in[ni], rows, misaligned)) {
                return 0;
            }
            for (r = 0; r < rows; r++) {
                c->in[ni].data[r] = (uint32_t)sm64(&s);
            }
            bind(&c->in_b[ni], p->field[i].id, SU_ACCESS_READ, rows, c->in[ni].data);
            c->in_p[ni] = c->in[ni].data;
            ni++;
        }
    }
    for (i = 0; i < p->field_count; i++) {
        if ((p->field[i].access & SU_ACCESS_WRITE) != 0u) {
            if (!buf_alloc(&c->out[nw], rows, misaligned)) {
                return 0;
            }
            for (r = 0; r < rows; r++) {
                c->out[nw].data[r] = 0xA5A5A5A5u;
            }
            c->ref_col[nw] = (uint32_t *)malloc((size_t)rows * 4u + 4u);
            if (c->ref_col[nw] == NULL) {
                return 0;
            }
            bind(&c->out_b[nw], p->field[i].id, SU_ACCESS_WRITE, rows, c->out[nw].data);
            c->out_p[nw] = c->out[nw].data;
            nw++;
        }
    }
    c->in_n = ni;
    c->out_n = nw;

    memset(&c->inv, 0, sizeof c->inv);
    c->inv.struct_size = (uint32_t)sizeof c->inv;
    c->inv.abi_version = SU_ABI_VERSION;
    c->inv.layout_generation = 1u;
    c->inv.binding_count = ni;
    c->inv.row_count = rows;
    c->inv.bindings = c->in_b;

    memset(&c->outs, 0, sizeof c->outs);
    c->outs.struct_size = (uint32_t)sizeof c->outs;
    c->outs.binding_count = nw;
    c->outs.failed_insn = UINT32_MAX;
    c->outs.bindings = c->out_b;

    memset(&c->args, 0, sizeof c->args);
    c->args.struct_size = (uint32_t)sizeof c->args;
    c->args.abi_version = SU_NATIVE_ABI_VERSION;
    c->args.row_count = rows;
    c->args.in_cols = c->in_p;
    c->args.out_cols = c->out_p;
    return 1;
}

static void cfg_free(Cfg *c) {
    uint32_t i;
    for (i = 0; i < MAX_FIELDS; i++) {
        buf_free(&c->in[i]);
        buf_free(&c->out[i]);
        free(c->ref_col[i]);
        c->ref_col[i] = NULL;
    }
}

static void out_reset(Cfg *c) {
    uint32_t i;
    uint64_t r;
    for (i = 0; i < c->out_n; i++) {
        for (r = 0; r < c->rows; r++) {
            c->out[i].data[r] = 0xA5A5A5A5u;
        }
    }
}

static int out_equal_ref(const Cfg *c) {
    uint32_t i;
    for (i = 0; i < c->out_n; i++) {
        if (memcmp(c->out[i].data, c->ref_col[i], (size_t)c->rows * 4u) != 0) {
            return 0;
        }
    }
    return 1;
}

static void out_save_ref(Cfg *c) {
    uint32_t i;
    for (i = 0; i < c->out_n; i++) {
        memcpy(c->ref_col[i], c->out[i].data, (size_t)c->rows * 4u);
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Engine state for one program. */
typedef struct Engines {
    SuExecContext *vm;
    SuNativeArtifact *sc;
    SuNativeArtifact *av;
} Engines;

static uint32_t run_slot(int slot, Engines *e, Cfg *c) {
    switch (slot) {
    case SLOT_VM:
        return su_vm_invoke_v1(e->vm, &c->inv, &c->outs);
    case SLOT_SC_WRAP:
        return su_native_run_checked(e->sc, &c->inv, &c->outs);
    case SLOT_AV_WRAP:
        return su_native_run_checked(e->av, &c->inv, &c->outs);
    case SLOT_SC_KERN:
        (void)su_native_entry(e->sc)(&c->args);
        return SU_OK;
    case SLOT_AV_KERN:
        (void)su_native_entry(e->av)(&c->args);
        return SU_OK;
    default:
        return SU_E_INTERNAL;
    }
}

/* Runs one slot K times back to back; returns the total elapsed ns, or 0 on a non-OK status. */
static uint64_t time_slot(int slot, Engines *e, Cfg *c, uint32_t k) {
    uint64_t t0;
    uint64_t t1;
    uint32_t j;
    uint32_t st = SU_OK;
    t0 = now_ns();
    for (j = 0; j < k; j++) {
        st = run_slot(slot, e, c);
        if (st != SU_OK) {
            return 0;
        }
    }
    t1 = now_ns();
    return t1 - t0;
}

static int verify_slot(int slot, Engines *e, Cfg *c) {
    uint32_t st = run_slot(slot, e, c);
    return st == SU_OK && out_equal_ref(c);
}

static int run_config(const Prog *p, uint64_t rows, int misaligned, uint32_t avx2) {
    Engines e;
    SuLirModule m;
    SuNativeDiag d;
    Cfg c;
    int slots[SLOTS];
    uint32_t nslots = 0;
    uint32_t k[SLOTS];
    uint64_t seed = UINT64_C(20261012) ^ (rows * 31u) ^ (misaligned ? 7u : 0u) ^ (uint64_t)p->insn_count;
    uint32_t s;
    uint32_t j;
    int ok = 1;
    const char *align = misaligned ? "unaligned1" : "aligned";

    memset(&e, 0, sizeof e);
    make_module(p, &m);
    if (su_vm_context_create(&m, 1u, &e.vm) != SU_OK) {
        fprintf(stderr, "setup: vm context failed (%s)\n", p->name);
        return 0;
    }
    if (su_native_compile(&m, 1u, SU_NCAP_X86_64_SCALAR, &e.sc, &d) != SU_OK) {
        fprintf(stderr, "setup: scalar compile failed (%s): %s\n", p->name, d.message);
        return 0;
    }
    if (avx2 && su_native_compile(&m, 1u, SU_NCAP_X86_64_AVX2, &e.av, &d) != SU_OK) {
        fprintf(stderr, "setup: avx2 compile failed (%s): %s\n", p->name, d.message);
        return 0;
    }
    if (!cfg_setup(&c, p, rows, misaligned, seed)) {
        fprintf(stderr, "setup: allocation failed\n");
        return 0;
    }

    /* Reference: the VM is the oracle. Its output is saved before any native engine runs. */
    out_reset(&c);
    if (su_vm_invoke_v1(e.vm, &c.inv, &c.outs) != SU_OK) {
        fprintf(stderr, "verify: reference VM failed (%s %s %llu)\n", p->name, align, (unsigned long long)rows);
        ok = 0;
    }
    out_save_ref(&c);

    slots[nslots++] = SLOT_VM;
    slots[nslots++] = SLOT_SC_WRAP;
    slots[nslots++] = SLOT_SC_KERN;
    if (avx2) {
        slots[nslots++] = SLOT_AV_WRAP;
        slots[nslots++] = SLOT_AV_KERN;
    }

    for (j = 0; ok && j < nslots; j++) {
        out_reset(&c);
        if (!verify_slot(slots[j], &e, &c)) {
            fprintf(stderr, "verify FAILED: %s %s rows=%llu slot=%s\n", p->name, align,
                    (unsigned long long)rows, k_slot_name[slots[j]]);
            ok = 0;
            break;
        }
        printf("VERIFY\t%s\t%s\t%llu\t%s\tpass\n", p->name, align, (unsigned long long)rows,
               k_slot_name[slots[j]]);
    }

    /* Warm-up and calibration: K invocations per sample so each sample lasts at least MIN_SAMPLE_NS. */
    for (j = 0; ok && j < nslots; j++) {
        uint64_t t;
        uint64_t per;
        uint32_t w;
        for (w = 0; w < WARMUP; w++) {
            if (time_slot(slots[j], &e, &c, 1u) == 0u) {
                ok = 0;
            }
        }
        t = time_slot(slots[j], &e, &c, 1u);
        per = t == 0u ? 1u : t;
        k[j] = (uint32_t)(MIN_SAMPLE_NS / per);
        if (k[j] < 1u) {
            k[j] = 1u;
        }
        if (k[j] > MAX_K) {
            k[j] = MAX_K;
        }
        if (rows >= 1000000u) {
            k[j] = 1u;
        }
    }

    if (ok) {
        printf("CONFIG\t%s\t%s\t%llu\t%u", p->name, align, (unsigned long long)rows, p->bytes_per_row);
        for (j = 0; j < nslots; j++) {
            printf("\t%s:%u", k_slot_name[slots[j]], k[j]);
        }
        printf("\n");
        for (s = 0; ok && s < SAMPLES; s++) {
            /* rotating start so no slot always runs first or last */
            for (j = 0; ok && j < nslots; j++) {
                int idx = (int)((j + s) % nslots);
                uint64_t total = time_slot(slots[idx], &e, &c, k[idx]);
                if (total == 0u) {
                    fprintf(stderr, "timing: non-OK status in %s\n", k_slot_name[slots[idx]]);
                    ok = 0;
                    break;
                }
                printf("SAMPLE\t%s\t%s\t%llu\t%s\t%u\t%u\t%llu\n", p->name, align,
                       (unsigned long long)rows, k_slot_name[slots[idx]], k[idx], s,
                       (unsigned long long)total);
            }
        }
        /* post-timing check: the last outputs still equal the reference */
        for (j = 0; ok && j < nslots; j++) {
            out_reset(&c);
            if (!verify_slot(slots[j], &e, &c)) {
                fprintf(stderr, "post-timing verify FAILED: %s %s rows=%llu slot=%s\n", p->name, align,
                        (unsigned long long)rows, k_slot_name[slots[j]]);
                ok = 0;
            }
        }
    }

    cfg_free(&c);
    if (e.av != NULL) {
        su_native_artifact_free(e.av);
    }
    if (e.sc != NULL) {
        su_native_artifact_free(e.sc);
    }
    su_vm_context_destroy(e.vm);
    return ok;
}

int main(void) {
    static const uint64_t row_counts[2] = {1000000u, 1024u};
    Prog progs[2];
    uint32_t avx2 = (su_native_host_caps() & SU_NCAP_X86_64_AVX2) != 0u ? 1u : 0u;
    uint32_t pi;
    uint32_t ri;
    int mis;
    int all_ok = 1;

    printf("HOST\tavx2\t%u\n", avx2);
    printf("ASMJIT\t%s\n", su_native_asmjit_commit());
    make_movement(&progs[0]);
    make_arith(&progs[1]);
    for (pi = 0; pi < 2u; pi++) {
        for (mis = 0; mis < 2; mis++) {
            for (ri = 0; ri < 2u; ri++) {
                if (!run_config(&progs[pi], row_counts[ri], mis, avx2)) {
                    all_ok = 0;
                }
            }
        }
    }
    if (!all_ok) {
        printf("RESULT\tfail\n");
        return 1;
    }
    printf("RESULT\tpass\n");
    return 0;
}
