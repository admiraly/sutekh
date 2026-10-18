/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Windows x64 unwind data of native kernels (backends/native/su_native_unwind.h, su_native_exec_publish_unwind).
 * For an IR 0.1 scalar kernel, an IR 0.1 AVX2 kernel (when the CPU has AVX2) and the IR 0.3 racer.move kernel
 * (which calls C helpers):
 *   1. RtlLookupFunctionEntry finds the kernel's entry (begin 0, image base = the code mapping);
 *   2. a real access violation inside the kernel (one input column points at a PAGE_NOACCESS page) is delivered
 *      to the caller's __except: the system unwinder walks out of the kernel frame. In the filter the fault
 *      address is inside the kernel, and RtlVirtualUnwind from the fault context returns to a return address in
 *      this executable (the call in su_native_run_checked) with the stack pointer above the kernel frame;
 *   3. after su_native_artifact_free the entry is gone.
 * The crash seam of the worker (SU_NV_FAULT_CRASH) raises its fault outside the kernel; this test faults inside it.
 * Mutation: SUTEKH_NATIVE_NO_UNWIND=1 publishes without registration; check 1 then fails for every kernel. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "backends/native/su_native.h"
#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/native/abi.h"

static unsigned g_checks, g_failures;

static void check(int cond, const char *fmt, ...) {
    g_checks++;
    if (!cond) {
        va_list ap;
        g_failures++;
        fprintf(stderr, "FAIL: ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *b = NULL;
    *len = 0;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0) {
        rewind(f);
        b = malloc((size_t)sz + 1u);
        if (b != NULL && fread(b, 1, (size_t)sz, f) == (size_t)sz) {
            *len = (size_t)sz;
        } else {
            free(b);
            b = NULL;
        }
    }
    fclose(f);
    return b;
}

static int load_module(const char *capsule, const char *lir, SuLirModule *m) {
    size_t cn = 0, ln = 0;
    uint8_t *c = slurp(capsule, &cn), *l = slurp(lir, &ln);
    SuDiag d;
    uint32_t st;
    memset(m, 0, sizeof *m);
    if (c == NULL || l == NULL) {
        fprintf(stderr, "cannot read %s / %s\n", capsule, lir);
        free(c);
        free(l);
        return 0;
    }
    su_diag_init(&d);
    st = su_lir_verify(c, cn, l, ln, m, &d);
    free(c);
    free(l);
    if (st != SU_OK) {
        fprintf(stderr, "%s: verify %u at %s: %s\n", capsule, (unsigned)st, d.json_pointer, d.message);
        return 0;
    }
    return 1;
}

/* Fault filter state. */
static uint64_t g_code_lo, g_code_hi, g_mod_lo, g_mod_hi;
static int g_fault_in_kernel, g_unwound_to_caller, g_lookup_in_filter, g_sp_above;

static LONG filter(EXCEPTION_POINTERS *ep) {
    CONTEXT ctx = *ep->ContextRecord;
    DWORD64 base = 0, establisher = 0;
    PVOID handler_data = NULL;
    PRUNTIME_FUNCTION fe;
    uint64_t rip = ctx.Rip, rsp = ctx.Rsp;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_fault_in_kernel = rip >= g_code_lo && rip < g_code_hi;
    fe = RtlLookupFunctionEntry(rip, &base, NULL);
    g_lookup_in_filter = fe != NULL && base == g_code_lo;
    if (fe != NULL) {
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, rip, fe, &ctx, &handler_data, &establisher, NULL);
        g_unwound_to_caller = ctx.Rip >= g_mod_lo && ctx.Rip < g_mod_hi;
        g_sp_above = ctx.Rsp > rsp;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token" /* __try / __except (SEH) */
#endif
/* Runs the kernel with every column valid except the first input, which points at an inaccessible page. */
static int run_faulting(SuNativeArtifact *a, const SuLirModule *m, uint64_t rows) {
    SuColumnBinding in[SU_LIMIT_FIELDS], out[SU_LIMIT_FIELDS];
    uint32_t nin = 0, nout = 0, i;
    uint32_t *cols = calloc((size_t)rows * 2u * m->field_count + 1u, sizeof(uint32_t));
    uint8_t *noaccess = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    SuInvocation inv;
    SuOutputs outs;
    volatile int caught = 0;
    if (cols == NULL || noaccess == NULL) {
        free(cols);
        return -1;
    }
    for (i = 0; i < m->field_count; i++) {
        SuColumnBinding b;
        memset(&b, 0, sizeof b);
        b.struct_size = (uint32_t)sizeof b;
        b.field_id = m->fields[i].id;
        b.elem_type = SU_TYPE_U32;
        b.layout_generation = 1u;
        b.stride_bytes = 4u;
        b.row_count = rows;
        if ((m->fields[i].access & SU_ACCESS_READ) != 0u) {
            b.access = SU_ACCESS_READ;
            b.data = nin == 0u ? (void *)noaccess : (void *)(cols + (size_t)rows * (2u * i));
            in[nin++] = b;
        }
        if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
            b.access = SU_ACCESS_WRITE;
            b.data = cols + (size_t)rows * (2u * i + 1u);
            out[nout++] = b;
        }
    }
    memset(&inv, 0, sizeof inv);
    inv.struct_size = (uint32_t)sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = nin;
    inv.row_count = rows;
    inv.bindings = in;
    memset(&outs, 0, sizeof outs);
    outs.struct_size = (uint32_t)sizeof outs;
    outs.binding_count = nout;
    outs.bindings = out;
    g_fault_in_kernel = g_unwound_to_caller = g_lookup_in_filter = g_sp_above = 0;
    __try {
        (void)su_native_run_checked(a, &inv, &outs);
    } __except (filter(GetExceptionInformation())) {
        caught = 1;
    }
    VirtualFree(noaccess, 0, MEM_RELEASE);
    free(cols);
    return caught;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static void test_kernel(const char *name, const char *capsule, const char *lir, uint32_t caps) {
    SuLirModule m;
    SuNativeArtifact *a = NULL;
    SuNativeDiag d;
    const uint8_t *code = NULL;
    size_t size = 0;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION fe;
    uint32_t st;
    int caught;
    if (!load_module(capsule, lir, &m)) {
        check(0, "%s: module", name);
        return;
    }
    st = su_native_compile(&m, 1u, caps, &a, &d);
    check(st == SU_OK, "%s: compile %u", name, (unsigned)st);
    if (st != SU_OK) {
        su_lir_module_free(&m);
        return;
    }
    su_native_code(a, &code, &size);
    g_code_lo = (uint64_t)(uintptr_t)code;
    g_code_hi = g_code_lo + size;
    /* 1. The entry exists for an address in the kernel body. */
    fe = RtlLookupFunctionEntry((DWORD64)(uintptr_t)(code + size / 2u), &base, NULL);
    check(fe != NULL && base == g_code_lo && fe->BeginAddress == 0u && fe->EndAddress <= size &&
              fe->EndAddress > size / 2u,
          "%s: RtlLookupFunctionEntry finds the kernel (entry %p)", name, (void *)fe);
    /* 2. A fault inside the kernel reaches the caller's handler through the unwind data. */
    caught = run_faulting(a, &m, 64u);
    check(caught == 1, "%s: the access violation inside the kernel reached the caller's __except", name);
    check(g_fault_in_kernel, "%s: the fault address is inside the kernel code", name);
    check(g_lookup_in_filter, "%s: the kernel entry is found from the fault context", name);
    check(g_unwound_to_caller, "%s: RtlVirtualUnwind returns into this executable", name);
    check(g_sp_above, "%s: the unwound stack pointer is above the kernel frame", name);
    /* 3. Unregistered with the artifact. */
    su_native_artifact_free(a);
    fe = RtlLookupFunctionEntry((DWORD64)(uintptr_t)g_code_lo + 1u, &base, NULL);
    check(fe == NULL, "%s: the entry is removed with the artifact", name);
    su_lir_module_free(&m);
    printf("unwind %s: %u bytes of code, fault caught %d\n", name, (unsigned)size, caught);
}

int main(void) {
    HMODULE self = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)self + ((IMAGE_DOS_HEADER *)self)->e_lfanew);
    g_mod_lo = (uint64_t)(uintptr_t)self;
    g_mod_hi = g_mod_lo + nt->OptionalHeader.SizeOfImage;
    test_kernel("ir0.1 scalar", "tests/fixtures/t03/mul_overflow/capsule.json",
                "tests/fixtures/t03/mul_overflow/program.lir.json", SU_NCAP_X86_64_SCALAR);
    if ((su_native_host_caps() & SU_NCAP_X86_64_AVX2) != 0u) {
        test_kernel("ir0.1 avx2", "tests/fixtures/t03/mul_overflow/capsule.json",
                    "tests/fixtures/t03/mul_overflow/program.lir.json", SU_NCAP_X86_64_AVX2);
    } else {
        printf("unwind ir0.1 avx2: NOT RUN (no AVX2 on this CPU)\n");
    }
    test_kernel("ir0.3 racer.move", "capsules/space3d_racer/move.capsule.json", "capsules/space3d_racer/move.lir.json",
                SU_NCAP_X86_64_SCALAR);
    printf("native_unwind: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
