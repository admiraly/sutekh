/* M1-01 native backend: C glue (see su_native.h).
 *
 * Owns: compile orchestration, artifact hash (planning/contracts/M1_NATIVE_CONTRACT.md section 3),
 * executable publication (W^X: RW while writing, then R+X, never RWX), the mirrored wrapper checks
 * of su_vm_invoke_v1, and the kernel call. Lowering lives in su_native_asmjit.cpp. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "backends/native/su_native.h"

#include <stdint.h>

#include "backends/native/avx2/su_native_avx2.h"
#include <stdlib.h>
#include <string.h>

#include "backends/native/su_native_internal.h"
#include "backends/native/su_native_lower.h"
#include "backends/native/su_native_ops03.h"
#include "vm/su_vm_plan.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef SU_NATIVE_ASMJIT_COMMIT
#error "SU_NATIVE_ASMJIT_COMMIT must be defined by the build (third_party/asmjit.lock.json)"
#endif

#define SU_NATIVE_BACKEND_VERSION UINT32_C(1)
#define SU_NATIVE_NO_FAILED_INSN UINT32_MAX
#define SU_NATIVE_HASH_TAG "SUTEKH-NATIVE-ARTIFACT-V1"

/* The C++ lowering uses its own op and access numbers; they must equal the frozen values. */
_Static_assert(SU_NLOW_OP_CONST_U32 == SU_OP_CONST_U32, "op CONST");
_Static_assert(SU_NLOW_OP_LOAD_U32 == SU_OP_LOAD_U32, "op LOAD");
_Static_assert(SU_NLOW_OP_ADD_U32 == SU_OP_ADD_U32, "op ADD");
_Static_assert(SU_NLOW_OP_SUB_U32 == SU_OP_SUB_U32, "op SUB");
_Static_assert(SU_NLOW_OP_MUL_U32 == SU_OP_MUL_U32, "op MUL");
_Static_assert(SU_NLOW_OP_EQ_U32 == SU_OP_EQ_U32, "op EQ");
_Static_assert(SU_NLOW_OP_LT_U32 == SU_OP_LT_U32, "op LT");
_Static_assert(SU_NLOW_OP_SELECT_U32 == SU_OP_SELECT_U32, "op SELECT");
_Static_assert(SU_NLOW_OP_STORE_U32 == SU_OP_STORE_U32, "op STORE");
_Static_assert(SU_NLOW_ACCESS_READ == SU_ACCESS_READ, "access read");
_Static_assert(SU_NLOW_ACCESS_WRITE == SU_ACCESS_WRITE, "access write");

static uint32_t g_inject_mask = 0u;

void su_native_test_inject_failure(uint32_t mask) {
    g_inject_mask = mask;
}

const char *su_native_asmjit_commit(void) {
    return SU_NATIVE_ASMJIT_COMMIT;
}

/* ---- identity --------------------------------------------------------- */

static void put_u32(SuSha256 *h, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    su_sha256_update(h, b, sizeof b);
}

/* Backend id implied by the requested caps (M1-03: AVX2 when requested). */
static uint32_t backend_for_caps(uint32_t caps) {
    return (caps & SU_NCAP_X86_64_AVX2) != 0u ? SU_NATIVE_BACKEND_ASMJIT_AVX2
                                              : SU_NATIVE_BACKEND_ASMJIT_SCALAR;
}

/* Every input of M1_NATIVE_CONTRACT.md section 3. Variable-length text is length-prefixed.
 * Lowering option flags are the empty set in this backend (word = 0). */
void su_native_test_hash(const uint8_t semantic_hash[SU_HASH_BYTES], uint32_t layout_generation,
                         uint32_t caps, uint8_t out[SU_HASH_BYTES]) {
    SuSha256 h;
    const char *commit = SU_NATIVE_ASMJIT_COMMIT;
    uint32_t host_abi;
#if defined(_WIN32)
    host_abi = SU_NATIVE_HOST_ABI_WIN64;
#else
    host_abi = SU_NATIVE_HOST_ABI_SYSV;
#endif
    su_sha256_init(&h);
    su_sha256_update(&h, SU_NATIVE_HASH_TAG, sizeof SU_NATIVE_HASH_TAG);
    put_u32(&h, SU_NATIVE_ABI_VERSION);
    put_u32(&h, SU_ARTIFACT_FORMAT_VERSION);
    put_u32(&h, SU_NATIVE_BACKEND_VERSION);
    put_u32(&h, backend_for_caps(caps));
    put_u32(&h, host_abi);
    put_u32(&h, 0u);
    put_u32(&h, layout_generation);
    put_u32(&h, caps);
    su_sha256_update(&h, semantic_hash, SU_HASH_BYTES);
    put_u32(&h, (uint32_t)strlen(commit));
    su_sha256_update(&h, commit, strlen(commit));
    su_sha256_final(&h, out);
}

/* ---- executable publication ------------------------------------------- */

#if defined(_WIN32)

uint32_t su_native_exec_publish(const uint8_t *src, size_t size, uint8_t **code, size_t *map_size) {
    SYSTEM_INFO si;
    size_t page;
    size_t map;
    DWORD old = 0;
    uint8_t *p;
    GetSystemInfo(&si);
    page = si.dwPageSize;
    map = (size + page - 1u) / page * page;
    if ((g_inject_mask & SU_NATIVE_FAIL_MMAP) != 0u) {
        return SU_E_RESOURCE_LIMIT;
    }
    p = VirtualAlloc(NULL, map, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(p, src, size);
    if ((g_inject_mask & SU_NATIVE_FAIL_MPROTECT) != 0u || !VirtualProtect(p, map, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return SU_E_RESOURCE_LIMIT;
    }
    FlushInstructionCache(GetCurrentProcess(), p, size);
    *code = p;
    *map_size = map;
    return SU_OK;
}

void su_native_exec_release(uint8_t *code, size_t map_size) {
    (void)map_size;
    if (code != NULL) {
        VirtualFree(code, 0, MEM_RELEASE);
    }
}

#else

uint32_t su_native_exec_publish(const uint8_t *src, size_t size, uint8_t **code, size_t *map_size) {
    long page_l = sysconf(_SC_PAGESIZE);
    size_t page = page_l > 0 ? (size_t)page_l : 4096u;
    size_t map = (size + page - 1u) / page * page;
    uint8_t *p;
    if ((g_inject_mask & SU_NATIVE_FAIL_MMAP) != 0u) {
        return SU_E_RESOURCE_LIMIT;
    }
    /* Written while RW only. Never RWX. */
    p = mmap(NULL, map, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(p, src, size);
    if ((g_inject_mask & SU_NATIVE_FAIL_MPROTECT) != 0u || mprotect(p, map, PROT_READ | PROT_EXEC) != 0) {
        munmap(p, map);
        return SU_E_RESOURCE_LIMIT;
    }
    __builtin___clear_cache((char *)p, (char *)p + size);
    *code = p;
    *map_size = map;
    return SU_OK;
}

void su_native_exec_release(uint8_t *code, size_t map_size) {
    if (code != NULL) {
        munmap(code, map_size);
    }
}

#endif

/* ---- lowering glue ---------------------------------------------------- */

/* The C++ lowering of IR 0.3 cannot include the C-only contract headers; its opcode numbers must match. */
_Static_assert(SU_NLOW_OP_CONST_I32 == SU_OP_CONST_I32 && SU_NLOW_OP_CONST_I64 == SU_OP_CONST_I64 &&
                   SU_NLOW_OP_CONST_Q32 == SU_OP_CONST_Q32 && SU_NLOW_OP_LOAD_I32 == SU_OP_LOAD_I32 &&
                   SU_NLOW_OP_STORE_I32 == SU_OP_STORE_I32 && SU_NLOW_OP_LOAD_I64 == SU_OP_LOAD_I64 &&
                   SU_NLOW_OP_STORE_I64 == SU_OP_STORE_I64 && SU_NLOW_OP_LOAD_Q32 == SU_OP_LOAD_Q32 &&
                   SU_NLOW_OP_STORE_Q32 == SU_OP_STORE_Q32 && SU_NLOW_OP_LOAD_V3 == SU_OP_LOAD_V3 &&
                   SU_NLOW_OP_STORE_V3 == SU_OP_STORE_V3 && SU_NLOW_OP_AND_BOOL == SU_OP_AND_BOOL &&
                   SU_NLOW_OP_OR_BOOL == SU_OP_OR_BOOL && SU_NLOW_OP_NOT_BOOL == SU_OP_NOT_BOOL,
               "IR 0.3 field and bool opcodes");
_Static_assert(SU_NLOW_OP_I32_BASE == SU_OP_I32_BASE && SU_NLOW_OP_I64_BASE == SU_OP_I64_BASE &&
                   SU_NLOW_OP_Q32_BASE == SU_OP_Q32_BASE && SU_NLOW_X_ADD == SU_OPX_ADD && SU_NLOW_X_SUB == SU_OPX_SUB &&
                   SU_NLOW_X_MUL == SU_OPX_MUL && SU_NLOW_X_DIV == SU_OPX_DIV && SU_NLOW_X_NEG == SU_OPX_NEG &&
                   SU_NLOW_X_ABS == SU_OPX_ABS && SU_NLOW_X_MIN == SU_OPX_MIN && SU_NLOW_X_MAX == SU_OPX_MAX &&
                   SU_NLOW_X_CLAMP == SU_OPX_CLAMP && SU_NLOW_X_EQ == SU_OPX_EQ && SU_NLOW_X_LT == SU_OPX_LT &&
                   SU_NLOW_X_LE == SU_OPX_LE && SU_NLOW_X_SELECT == SU_OPX_SELECT && SU_NLOW_X_SHL == SU_OPX_SHL &&
                   SU_NLOW_X_SHR == SU_OPX_SHR && SU_NLOW_X_COUNT == SU_OPX_COUNT,
               "IR 0.3 scalar families");
_Static_assert(SU_NLOW_OP_SQRT_Q32 == SU_OP_SQRT_Q32 && SU_NLOW_OP_RSQRT_Q32 == SU_OP_RSQRT_Q32 &&
                   SU_NLOW_OP_I32_FROM_U32 == SU_OP_I32_FROM_U32 && SU_NLOW_OP_U32_FROM_I32 == SU_OP_U32_FROM_I32 &&
                   SU_NLOW_OP_I64_FROM_I32 == SU_OP_I64_FROM_I32 && SU_NLOW_OP_I32_FROM_I64 == SU_OP_I32_FROM_I64 &&
                   SU_NLOW_OP_Q32_FROM_I32 == SU_OP_Q32_FROM_I32 && SU_NLOW_OP_Q32_FROM_I64 == SU_OP_Q32_FROM_I64 &&
                   SU_NLOW_OP_Q32_TO_I32_FLOOR == SU_OP_Q32_TO_I32_FLOOR &&
                   SU_NLOW_OP_Q32_TO_I32_ROUND == SU_OP_Q32_TO_I32_ROUND && SU_NLOW_OP_Q32_FROM_RAW == SU_OP_Q32_FROM_RAW &&
                   SU_NLOW_OP_Q32_RAW == SU_OP_Q32_RAW && SU_NLOW_OP_Q32_TO_I64_FLOOR == SU_OP_Q32_TO_I64_FLOOR,
               "IR 0.3 conversions");
_Static_assert(SU_NLOW_OP_MAKE_V3 == SU_OP_MAKE_V3 && SU_NLOW_OP_V3_GET == SU_OP_V3_GET &&
                   SU_NLOW_OP_V3_ADD == SU_OP_V3_ADD && SU_NLOW_OP_V3_SUB == SU_OP_V3_SUB &&
                   SU_NLOW_OP_V3_SCALE == SU_OP_V3_SCALE && SU_NLOW_OP_V3_NEG == SU_OP_V3_NEG &&
                   SU_NLOW_OP_V3_DOT == SU_OP_V3_DOT && SU_NLOW_OP_V3_CROSS == SU_OP_V3_CROSS &&
                   SU_NLOW_OP_V3_LEN == SU_OP_V3_LEN && SU_NLOW_OP_V3_LEN_SQ == SU_OP_V3_LEN_SQ &&
                   SU_NLOW_OP_V3_NORMALIZE == SU_OP_V3_NORMALIZE && SU_NLOW_OP_V3_CLAMP_LEN == SU_OP_V3_CLAMP_LEN &&
                   SU_NLOW_OP_SELECT_V3 == SU_OP_SELECT_V3 && SU_NLOW_OP_MAKE_QUAT == SU_OP_MAKE_QUAT &&
                   SU_NLOW_OP_QUAT_GET == SU_OP_QUAT_GET && SU_NLOW_OP_QUAT_MUL == SU_OP_QUAT_MUL &&
                   SU_NLOW_OP_QUAT_CONJ == SU_OP_QUAT_CONJ && SU_NLOW_OP_QUAT_ROTATE == SU_OP_QUAT_ROTATE &&
                   SU_NLOW_OP_QUAT_NORMALIZE == SU_OP_QUAT_NORMALIZE &&
                   SU_NLOW_OP_QUAT_INTEGRATE == SU_OP_QUAT_INTEGRATE && SU_NLOW_OP_SELECT_QUAT == SU_OP_SELECT_QUAT,
               "IR 0.3 vec3 and quaternion opcodes");

/* Addresses of the IR 0.3 C helpers by opcode (0 = emitted inline). */
static void fill_helpers03(uint64_t helpers[SU_NLOW_HELPER_SLOTS]) {
    uint32_t op;
    for (op = 0; op < SU_NLOW_HELPER_SLOTS; op++) {
        SuNativeHelper03 h = su_native_helper03(op);
        uintptr_t addr = 0;
        if (h != NULL) {
            memcpy(&addr, &h, sizeof addr < sizeof h ? sizeof addr : sizeof h); /* function pointer to address */
        }
        helpers[op] = (uint64_t)addr;
    }
}

static void lowered_free(SuNativeLowered *l) {
    if (l == NULL) {
        return;
    }
    free(l->bytes);
    free(l->insn_ranges);
    free(l->disasm);
    memset(l, 0, sizeof *l);
}

static uint32_t map_lower_status(uint32_t st) {
    switch (st) {
    case SU_NLOW_OK:
        return SU_OK;
    case SU_NLOW_RESOURCE:
        return SU_E_RESOURCE_LIMIT;
    default:
        return SU_E_INTERNAL;
    }
}

/* Flattens the verified module into plain arrays. The caller frees *words and *access. */
static uint32_t build_lower_input(const SuLirModule *m, SuNativeLowerInput *in, uint32_t **words,
                                  uint32_t **access) {
    uint32_t i;
    *words = calloc(5u * (size_t)m->insn_count + 1u, sizeof **words);
    *access = calloc((size_t)m->field_count + 1u, sizeof **access);
    if (*words == NULL || *access == NULL) {
        free(*words);
        free(*access);
        *words = NULL;
        *access = NULL;
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < m->insn_count; i++) {
        (*words)[5u * i + 0u] = m->insns[i].op;
        (*words)[5u * i + 1u] = m->insns[i].dst;
        (*words)[5u * i + 2u] = m->insns[i].a;
        (*words)[5u * i + 3u] = m->insns[i].b;
        (*words)[5u * i + 4u] = m->insns[i].c;
    }
    for (i = 0; i < m->field_count; i++) {
        (*access)[i] = m->fields[i].access;
    }
    in->register_count = m->register_count;
    in->field_count = m->field_count;
    in->insn_count = m->insn_count;
    in->field_access = *access;
    in->insn_words = *words;
    in->helpers = NULL;
    in->guard_count = 0u;
    in->guard_insns = NULL;
    in->guard_checks = NULL;
    in->identity_count = 0u;
    in->identity_lanes = NULL;
    in->pre_count = 0u;
    in->pre_insns = NULL;
    return SU_OK;
}

/* ---- artifact lifetime ------------------------------------------------ */

void su_native_artifact_free(SuNativeArtifact *a) {
    if (a == NULL) {
        return;
    }
    su_native_exec_release(a->code, a->map_size);
    free(a->ranges);
    free(a->disasm);
    free(a);
}

static void diag_set(SuNativeDiag *d, uint32_t code, uint32_t insn, const char *msg) {
    if (d == NULL) {
        return;
    }
    d->code = code;
    d->failed_insn = insn;
    d->message[0] = '\0';
    if (msg != NULL) {
        size_t n = strlen(msg);
        if (n >= sizeof d->message) {
            n = sizeof d->message - 1u;
        }
        memcpy(d->message, msg, n);
        d->message[n] = '\0';
    }
}

uint32_t su_native_compile(const SuLirModule *m, uint32_t layout_generation, uint32_t caps,
                           SuNativeArtifact **out, SuNativeDiag *diag) {
    SuNativeArtifact *a;
    SuNativeLowered low;
    SuNativeLowerInput in;
    SuNativeSourceRange *ranges = NULL;
    uint32_t *words = NULL;
    uint32_t *access = NULL;
    uint32_t st;
    uint32_t failed = SU_NATIVE_NO_FAILED_INSN;
    uint32_t host_abi;
    uint32_t k;
    int use_avx2;
    int is03;
    uint64_t helpers[SU_NLOW_HELPER_SLOTS];
    char msg[160];
    void *code_ptr;

    if (out == NULL) {
        diag_set(diag, SU_E_INVALID_ARGUMENT, SU_NATIVE_NO_FAILED_INSN, "out is NULL");
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    diag_set(diag, SU_OK, SU_NATIVE_NO_FAILED_INSN, NULL);
    if (m == NULL) {
        diag_set(diag, SU_E_INVALID_ARGUMENT, SU_NATIVE_NO_FAILED_INSN, "module is NULL");
        return SU_E_INVALID_ARGUMENT;
    }
    /* IR 0.2 (M2-00c2): entity ops run on the reference VM only (M2 contract section 3); a 0.3 module that uses
     * them is refused for the same reason. */
    if (m->uses_entity_ops != 0u || m->ir_version_minor == 2u) {
        diag_set(diag, SU_E_CAPABILITY_DENIED, SU_NATIVE_NO_FAILED_INSN, "ir_0_2_not_lowered");
        return SU_E_CAPABILITY_DENIED;
    }
    /* IR 0.3 (fixed_q32_v1): scalar lowering only (su_native_lower03.cpp). An AVX2 request is refused; the
     * caller compiles with SU_NCAP_X86_64_SCALAR (world.specialize "auto" does). */
    is03 = m->ir_version_minor >= 3u;
    if (is03 && caps != SU_NCAP_X86_64_SCALAR) {
        diag_set(diag, SU_E_CAPABILITY_DENIED, SU_NATIVE_NO_FAILED_INSN, "ir_0_3_avx2_not_lowered");
        return SU_E_CAPABILITY_DENIED;
    }
#if !(defined(__x86_64__) || defined(_M_X64))
    diag_set(diag, SU_E_CAPABILITY_DENIED, SU_NATIVE_NO_FAILED_INSN, "host is not x86-64");
    return SU_E_CAPABILITY_DENIED;
#endif
    if (caps == SU_NCAP_X86_64_SCALAR) {
        use_avx2 = 0;
    } else if ((caps & ~(SU_NCAP_X86_64_SCALAR | SU_NCAP_X86_64_AVX2)) == 0u &&
               (caps & SU_NCAP_X86_64_AVX2) != 0u) {
        if ((su_native_host_caps() & SU_NCAP_X86_64_AVX2) == 0u) {
            diag_set(diag, SU_E_CAPABILITY_DENIED, SU_NATIVE_NO_FAILED_INSN,
                     "AVX2 requested but the host does not report it usable");
            return SU_E_CAPABILITY_DENIED;
        }
        use_avx2 = 1;
    } else {
        diag_set(diag, SU_E_CAPABILITY_DENIED, SU_NATIVE_NO_FAILED_INSN,
                 "unknown capability set (SCALAR, or AVX2 with or without SCALAR)");
        return SU_E_CAPABILITY_DENIED;
    }
    if ((m->insn_count > 0u && m->insns == NULL) || (m->field_count > 0u && m->fields == NULL)) {
        diag_set(diag, SU_E_INVALID_ARGUMENT, SU_NATIVE_NO_FAILED_INSN, "module tables are NULL");
        return SU_E_INVALID_ARGUMENT;
    }
    if (m->register_count > SU_LIMIT_REGISTERS || m->field_count > SU_LIMIT_FIELDS ||
        m->insn_count > SU_LIMIT_INSTRUCTIONS) {
        diag_set(diag, SU_E_RESOURCE_LIMIT, SU_NATIVE_NO_FAILED_INSN, "module exceeds M0 limits");
        return SU_E_RESOURCE_LIMIT;
    }

    st = build_lower_input(m, &in, &words, &access);
    if (st != SU_OK) {
        diag_set(diag, st, SU_NATIVE_NO_FAILED_INSN, "cannot build lowering input");
        return st;
    }
    memset(&low, 0, sizeof low);
    msg[0] = '\0';
    if (is03) {
        SuVmPlan *plan = NULL;
        uint32_t *gi = NULL;
        uint8_t *gc = NULL;
        uint32_t *pre = NULL;
        fill_helpers03(helpers);
        in.helpers = helpers;
        /* The VM plan's identity-row guard, when the plan builder proves one (same analysis as the planned VM
         * tier). Without a plan or guard the kernel runs every row. */
        if (su_vm_plan_build(m, &plan) == SU_VM_PLAN_OK && plan->has_guard) {
            uint32_t g;
            gi = calloc(plan->n_guard + 1u, sizeof *gi);
            gc = calloc(plan->n_guard + 1u, sizeof *gc);
            if (gi != NULL && gc != NULL) {
                for (g = 0; g < plan->n_guard; g++) {
                    gi[g] = plan->guard[g].idx;
                    gc[g] = (uint8_t)plan->guard[g].check;
                }
                in.guard_count = plan->n_guard;
                in.guard_insns = gi;
                in.guard_checks = gc;
                in.identity_count = plan->n_identity;
                in.identity_lanes = plan->identity;
            }
            /* The guard slice may read hoisted registers: the instructions not in the plan's body. */
            pre = calloc(m->insn_count + 1u, sizeof *pre);
            if (pre != NULL && gi != NULL && gc != NULL) {
                uint32_t i, b = 0, np = 0;
                for (i = 0; i < m->insn_count; i++) {
                    if (b < plan->n_body && plan->body[b].idx == i) {
                        b++;
                    } else {
                        pre[np++] = i;
                    }
                }
                in.pre_count = np;
                in.pre_insns = pre;
            } else {
                in.guard_count = 0u; /* allocation failed: no guard (every row runs the body) */
                in.identity_count = 0u;
            }
        }
        st = map_lower_status(su_native_lower_x64_03(&in, &low, msg, sizeof msg, &failed));
        free(gi);
        free(gc);
        free(pre);
        su_vm_plan_free(plan);
    } else {
        st = map_lower_status(use_avx2 ? su_native_lower_avx2(&in, &low, msg, sizeof msg, &failed)
                                       : su_native_lower_x64(&in, &low, msg, sizeof msg, &failed));
    }
    free(words);
    free(access);
    if (st != SU_OK) {
        diag_set(diag, st, failed, msg);
        lowered_free(&low);
        return st;
    }

    if (m->insn_count > 0u) {
        ranges = calloc(m->insn_count, sizeof *ranges);
        if (ranges == NULL) {
            lowered_free(&low);
            diag_set(diag, SU_E_RESOURCE_LIMIT, SU_NATIVE_NO_FAILED_INSN, "allocation failed");
            return SU_E_RESOURCE_LIMIT;
        }
        for (k = 0; k < m->insn_count; k++) {
            ranges[k].insn = k;
            ranges[k].begin = low.insn_ranges[2u * k];
            ranges[k].end = low.insn_ranges[2u * k + 1u];
            ranges[k].reserved = 0u;
        }
    }

    a = calloc(1, sizeof *a);
    if (a == NULL) {
        free(ranges);
        lowered_free(&low);
        diag_set(diag, SU_E_RESOURCE_LIMIT, SU_NATIVE_NO_FAILED_INSN, "allocation failed");
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_native_exec_publish(low.bytes, low.size, &a->code, &a->map_size);
    if (st != SU_OK) {
        free(ranges);
        free(a);
        lowered_free(&low);
        diag_set(diag, st, SU_NATIVE_NO_FAILED_INSN, "executable mapping failed; nothing kept");
        return st;
    }
    a->code_size = low.size;
    code_ptr = a->code;
    memcpy(&a->fn, &code_ptr, sizeof code_ptr); /* object pointer to function pointer */
    a->ranges = ranges;
    a->disasm = low.disasm;
    a->disasm_len = low.disasm_len;
    low.disasm = NULL;
    lowered_free(&low);

    host_abi =
#if defined(_WIN32)
        SU_NATIVE_HOST_ABI_WIN64;
#else
        SU_NATIVE_HOST_ABI_SYSV;
#endif
    a->module = m;
    a->row_limit = (m->max_rows != 0u && m->max_rows < SU_LIMIT_ROWS) ? m->max_rows : SU_LIMIT_ROWS;
    a->info.struct_size = (uint32_t)sizeof a->info;
    a->info.abi_version = SU_NATIVE_ABI_VERSION;
    a->info.artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    a->info.layout_generation = layout_generation;
    a->info.required_caps = caps;
    a->info.backend_id = backend_for_caps(caps);
    a->info.host_abi = host_abi;
    memcpy(a->info.semantic_hash, m->semantic_hash, SU_HASH_BYTES);
    su_native_test_hash(m->semantic_hash, layout_generation, caps, a->info.artifact_hash);
    a->info.code_size = a->code_size;
    a->info.insn_count = m->insn_count;

    *out = a;
    return SU_OK;
}

uint32_t su_native_artifact_info(const SuNativeArtifact *a, SuNativeArtifactInfo *out) {
    if (a == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = a->info;
    return SU_OK;
}

SuNativeKernelFn su_native_entry(const SuNativeArtifact *a) {
    return a != NULL ? a->fn : NULL;
}

uint32_t su_native_source_map(const SuNativeArtifact *a, const SuNativeSourceRange **ranges,
                              uint32_t *count) {
    if (a == NULL || ranges == NULL || count == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *ranges = a->ranges;
    *count = a->info.insn_count;
    return SU_OK;
}

uint32_t su_native_code(const SuNativeArtifact *a, const uint8_t **code, size_t *size) {
    if (a == NULL || code == NULL || size == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *code = a->code;
    *size = a->code_size;
    return SU_OK;
}

uint32_t su_native_disassemble(const SuNativeArtifact *a, char *buf, size_t cap, size_t *len) {
    if (a == NULL || len == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *len = a->disasm_len;
    if (buf == NULL || cap <= a->disasm_len) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(buf, a->disasm, a->disasm_len + 1u);
    return SU_OK;
}

uint32_t su_native_set_row_limit(SuNativeArtifact *a, uint64_t limit) {
    if (a == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    a->row_limit = limit;
    return SU_OK;
}

/* ---- checked invocation (mirror of su_vm_invoke_v1 wrapper checks) ------ */

static long find_field(const SuLirModule *m, uint32_t id, uint32_t access) {
    uint32_t i;
    uint32_t lo = 0;
    uint32_t hi = m->field_count;
    /* Verified tables have unique IDs in ascending order: binary search, then the linear scan as before when it
     * misses (an unsorted table gives the linear answer). 2026-10-18. */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (m->fields[mid].id < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo < m->field_count && m->fields[lo].id == id && (m->fields[lo].access & access) != 0u) {
        return (long)lo;
    }
    for (i = 0; i < m->field_count; i++) {
        if (m->fields[i].id == id && (m->fields[i].access & access) != 0u) {
            return (long)i;
        }
    }
    return -1;
}

static uint32_t check_shape(const SuColumnBinding *b, uint32_t access) {
    if (b->struct_size != (uint32_t)sizeof(SuColumnBinding)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (b->elem_type != SU_TYPE_U32 || b->access != access || b->stride_bytes != 4u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (b->data == NULL) {
        return b->row_count == 0u ? SU_OK : SU_E_INVALID_ARGUMENT;
    }
    if (((uintptr_t)b->data % 4u) != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    return SU_OK;
}

static uint32_t check_schema(const SuNativeArtifact *a, const SuColumnBinding *b, uint64_t rows,
                             uint32_t access, uint8_t *bound, uint32_t **cols, uint32_t *cursor) {
    long idx = -1;
    const SuLirModule *m = a->module;
    uint32_t c = *cursor;
    if (b->layout_generation != a->info.layout_generation) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (b->row_count != rows) {
        return SU_E_SCHEMA_MISMATCH;
    }
    /* Bindings usually arrive in table order: walk on from the previous match, else search (same answer). */
    while (c < m->field_count && m->fields[c].id < b->field_id) {
        c++;
    }
    if (c < m->field_count && m->fields[c].id == b->field_id && (m->fields[c].access & access) != 0u) {
        idx = (long)c;
        *cursor = c + 1u;
    } else {
        idx = find_field(m, b->field_id, access);
    }
    if (idx < 0 || bound[idx] != 0u) {
        return SU_E_SCHEMA_MISMATCH;
    }
    bound[idx] = 1u;
    cols[idx] = (uint32_t *)b->data;
    return SU_OK;
}

static uint32_t check_set(const SuNativeArtifact *a, const SuColumnBinding *b, uint32_t n,
                          uint32_t access, uint64_t rows, uint8_t *bound, uint32_t **cols) {
    uint32_t i;
    uint32_t st;
    for (i = 0; i < n; i++) {
        st = check_shape(&b[i], access);
        if (st != SU_OK) {
            return st;
        }
    }
    uint32_t cursor = 0;
    for (i = 0; i < n; i++) {
        st = check_schema(a, &b[i], rows, access, bound, cols, &cursor);
        if (st != SU_OK) {
            return st;
        }
    }
    return SU_OK;
}

static int ranges_overlap(uintptr_t a0, uintptr_t a1, uintptr_t b0, uintptr_t b1) {
    return a0 < b1 && b0 < a1;
}

/* The same decision as the pairwise check below (an output range overlapping any other output or any input is
 * SU_E_INVALID_ARGUMENT; inputs may overlap each other; with at least one output, any range that would wrap is
 * SU_E_INVALID_ARGUMENT), in expected O(n): every range has the same length L, so two ranges overlap only when
 * their starts lie in the same or adjacent buckets floor(start / L). Each range is tested against the ranges
 * already placed in its three buckets (hash chains; a hash collision only adds tests), and refused when an
 * overlapping pair contains an output. 2026-10-18 (it was O(n^2) per kernel call). */
#define ALIAS_SLOTS 1024u
static uint32_t check_alias_buckets(const SuColumnBinding *in, uint32_t nin, const SuColumnBinding *out,
                                    uint32_t nout, uintptr_t bytes, int *done) {
    uintptr_t start[2u * SU_LIMIT_FIELDS];
    uint8_t is_out[2u * SU_LIMIT_FIELDS];
    int16_t next[2u * SU_LIMIT_FIELDS];
    int16_t head[ALIAS_SLOTS];
    uint32_t i;
    uint32_t n = 0;
    *done = 0;
    if ((uint64_t)nin + nout > 2u * SU_LIMIT_FIELDS) {
        return SU_OK; /* not handled here: the caller runs the pairwise check */
    }
    for (i = 0; i < nout; i++) {
        start[n] = (uintptr_t)out[i].data;
        is_out[n++] = 1u;
    }
    for (i = 0; i < nin; i++) {
        start[n] = (uintptr_t)in[i].data;
        is_out[n++] = 0u;
    }
    *done = 1;
    for (i = 0; i < n; i++) {
        if (start[i] > UINTPTR_MAX - bytes) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    memset(head, 0xFF, sizeof head); /* -1: empty */
    for (i = 0; i < n; i++) {
        uintptr_t b = start[i] / bytes;
        int d;
        for (d = -1; d <= 1; d++) {
            uintptr_t nb = b + (uintptr_t)(intptr_t)d;
            int16_t e;
            if ((d < 0 && b == 0u) || (d > 0 && nb == 0u)) {
                continue;
            }
            for (e = head[(uint32_t)((nb * (uintptr_t)0x9E3779B97F4A7C15ull) >> 54) & (ALIAS_SLOTS - 1u)]; e >= 0;
                 e = next[e]) {
                if ((is_out[i] || is_out[e]) && ranges_overlap(start[i], start[i] + bytes, start[e], start[e] + bytes)) {
                    return SU_E_INVALID_ARGUMENT;
                }
            }
        }
        {
            uint32_t h = (uint32_t)((b * (uintptr_t)0x9E3779B97F4A7C15ull) >> 54) & (ALIAS_SLOTS - 1u);
            next[i] = head[h];
            head[h] = (int16_t)i;
        }
    }
    return SU_OK;
}

static uint32_t check_alias(const SuColumnBinding *in, uint32_t nin, const SuColumnBinding *out,
                            uint32_t nout, uint64_t rows) {
    uintptr_t bytes;
    uint32_t i;
    uint32_t j;
    uint32_t k;
    uint32_t st;
    int done = 0;
    if (rows == 0u) {
        return SU_OK;
    }
    bytes = (uintptr_t)rows * 4u;
    if (nout == 0u) {
        return SU_OK;
    }
    st = check_alias_buckets(in, nin, out, nout, bytes, &done);
    if (done) {
        return st;
    }
    for (j = 0; j < nout; j++) {
        uintptr_t os = (uintptr_t)out[j].data;
        uintptr_t oe;
        if (os > UINTPTR_MAX - bytes) {
            return SU_E_INVALID_ARGUMENT;
        }
        oe = os + bytes;
        for (i = 0; i < nin; i++) {
            uintptr_t is = (uintptr_t)in[i].data;
            if (is > UINTPTR_MAX - bytes) {
                return SU_E_INVALID_ARGUMENT;
            }
            if (ranges_overlap(os, oe, is, is + bytes)) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
        for (k = 0; k < nout; k++) {
            uintptr_t ks;
            if (k == j) {
                continue;
            }
            ks = (uintptr_t)out[k].data;
            if (ks > UINTPTR_MAX - bytes) {
                return SU_E_INVALID_ARGUMENT;
            }
            if (ranges_overlap(os, oe, ks, ks + bytes)) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
    }
    return SU_OK;
}

uint32_t su_native_run_checked(SuNativeArtifact *a, const SuInvocation *input, SuOutputs *out) {
    const SuLirModule *m;
    uint32_t readable = 0u;
    uint32_t writable = 0u;
    uint32_t i;
    uint32_t st;
    uint64_t rows;
    uint32_t *in_by_field[SU_LIMIT_FIELDS];
    uint32_t *out_by_field[SU_LIMIT_FIELDS];
    uint8_t bound[SU_LIMIT_FIELDS];
    const uint32_t *in_ptr[SU_LIMIT_FIELDS];
    uint32_t *out_ptr[SU_LIMIT_FIELDS];
    SuNativeArgs args;
    SuNativeKernelFn fn;

    if (a == NULL || input == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    m = a->module;
    for (i = 0; i < m->field_count; i++) {
        if ((m->fields[i].access & SU_ACCESS_READ) != 0u) {
            readable++;
        }
        if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
            writable++;
        }
    }
    if (input->struct_size != (uint32_t)sizeof(SuInvocation) || input->abi_version != SU_ABI_VERSION) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (out->struct_size != (uint32_t)sizeof(SuOutputs) || out->reserved != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (input->binding_count != readable || out->binding_count != writable) {
        return SU_E_INVALID_ARGUMENT;
    }
    if ((input->binding_count > 0u && input->bindings == NULL) ||
        (out->binding_count > 0u && out->bindings == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (input->layout_generation != a->info.layout_generation) {
        return SU_E_SCHEMA_MISMATCH;
    }
    memset(in_by_field, 0, sizeof in_by_field);
    memset(out_by_field, 0, sizeof out_by_field);
    memset(bound, 0, sizeof bound);

    rows = input->row_count;
    st = check_set(a, input->bindings, input->binding_count, SU_ACCESS_READ, rows, bound, in_by_field);
    if (st != SU_OK) {
        return st;
    }
    memset(bound, 0, sizeof bound);
    st = check_set(a, out->bindings, out->binding_count, SU_ACCESS_WRITE, rows, bound, out_by_field);
    if (st != SU_OK) {
        return st;
    }
    if (rows > a->row_limit || rows > SIZE_MAX / sizeof(uint32_t)) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = check_alias(input->bindings, input->binding_count, out->bindings, out->binding_count, rows);
    if (st != SU_OK) {
        return st;
    }
    out->failed_insn = SU_NATIVE_NO_FAILED_INSN;
    if (rows == 0u) {
        return SU_OK;
    }

    /* Table-order ordinals: readable fields in in_ptr, writable fields in out_ptr. */
    {
        uint32_t ni = 0;
        uint32_t no = 0;
        for (i = 0; i < m->field_count; i++) {
            if ((m->fields[i].access & SU_ACCESS_READ) != 0u) {
                in_ptr[ni++] = in_by_field[i];
            }
            if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
                out_ptr[no++] = out_by_field[i];
            }
        }
    }
    args.struct_size = (uint32_t)sizeof args;
    args.abi_version = SU_NATIVE_ABI_VERSION;
    args.row_count = rows;
    args.in_cols = in_ptr;
    args.out_cols = out_ptr;
    fn = a->fn;
    st = fn(&args);
    if (st != SU_OK) {
        return st;
    }
    return SU_OK;
}
