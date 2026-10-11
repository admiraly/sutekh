/* Sutekh M1 native-kernel ABI contract, generation 1 (draft for M1-00 review).
 * Owner: contract owner / integrator. Consumers: M1-01 scalar backend, M1-02 lifetime and
 * recovery, M1-03 AVX2. C only; fixed-width integers; no enums, bitfields, callbacks or varargs.
 *
 * A native kernel is generated machine code that executes ONE verified system over ALL rows of
 * the fixed table, with exactly the semantics of the reference VM (u32_mod_v1). It is called only
 * through a trusted wrapper that has already performed every su_vm_invoke_v1 check (struct
 * sizes, generations, lengths, alias rejection, row limit). Generated code performs no checks of
 * its own except what is listed below, makes no calls, allocates nothing and touches only the
 * columns named in SuNativeArgs. */
#ifndef SUTEKH_NATIVE_ABI_H
#define SUTEKH_NATIVE_ABI_H

#include <stddef.h>
#include <stdint.h>
#include "sutekh/abi.h"

#define SU_NATIVE_ABI_VERSION UINT32_C(1)

/* Capability bits (host CPU features AND OS-enabled register state, never marketing names). */
#define SU_NCAP_X86_64_SCALAR UINT32_C(0x00000001) /* baseline x86-64, no SIMD requirement */
#define SU_NCAP_X86_64_AVX2   UINT32_C(0x00000002) /* AVX2 usable: CPUID and XGETBV YMM state */

/* Single argument passed to a kernel. Layout is the same on SysV and Win64 (all fields are
 * fixed width; pointers are 8 bytes). 40 bytes, no implicit padding.
 *
 * in_cols[i] is the i-th READABLE column and out_cols[j] the j-th WRITABLE staging column, in the
 * verified field-table order used by SuInvocation/SuOutputs (ascending field id). Column
 * pointers are at least 4-byte aligned and may be unaligned for 32-byte vector width; kernels
 * must use unaligned loads/stores or a proven-alignment path. Staging columns never overlap any
 * other column (wrapper guarantee). Pointers are valid for this call only (constitution C05). */
typedef struct SuNativeArgs {
    uint32_t struct_size;            /* sizeof(SuNativeArgs) */
    uint32_t abi_version;            /* SU_NATIVE_ABI_VERSION */
    uint64_t row_count;              /* every column holds exactly row_count u32 values; 0 allowed */
    const uint32_t *const *in_cols;  /* readable columns */
    uint32_t *const *out_cols;       /* writable staging columns */
} SuNativeArgs;

_Static_assert(sizeof(SuNativeArgs) == 32u, "SuNativeArgs size");
_Static_assert(offsetof(SuNativeArgs, row_count) == 8u, "SuNativeArgs.row_count offset");
_Static_assert(offsetof(SuNativeArgs, in_cols) == 16u, "SuNativeArgs.in_cols offset");
_Static_assert(offsetof(SuNativeArgs, out_cols) == 24u, "SuNativeArgs.out_cols offset");

/* Kernel entry: host C calling convention, one pointer argument (rdi on SysV, rcx on Win64),
 * returns uint32_t status in eax: SU_OK or SU_E_INTERNAL only. Kernels have no other failure
 * mode: the u32_mod_v1 operations are total. A kernel must:
 *  - preserve all callee-saved registers of the host ABI and keep the stack aligned as the ABI
 *    requires at any call site it would make (it makes none) and at entry;
 *  - not use the red zone below rsp unless the host ABI guarantees it (Win64 does not);
 *  - not modify MXCSR/x87 control state, not leave dirty YMM upper state (vzeroupper before ret
 *    when any VEX/YMM instruction ran), and not clobber the direction flag;
 *  - read each input row value as it was at kernel entry and write each staging column row
 *    exactly once;
 *  - handle row_count == 0 (writes nothing) and tails not divisible by the vector width. */
typedef uint32_t (*SuNativeKernelFn)(const SuNativeArgs *args);

/* Identity and provenance of one compiled artifact. Fixed-width, no pointers into generated
 * code that outlive the artifact. The artifact_hash covers everything below except the
 * runtime-only fields; two artifacts with equal hashes are interchangeable. */
typedef struct SuNativeArtifactInfo {
    uint32_t struct_size;            /* sizeof(SuNativeArtifactInfo) */
    uint32_t abi_version;            /* SU_NATIVE_ABI_VERSION */
    uint32_t artifact_format;        /* SU_ARTIFACT_FORMAT_VERSION of the semantic encoding */
    uint32_t layout_generation;      /* binding layout the kernel was lowered against */
    uint32_t required_caps;          /* SU_NCAP_* bits the kernel needs */
    uint32_t backend_id;             /* 1 = asmjit-x86-64-scalar, 2 = asmjit-x86-64-avx2 */
    uint32_t host_abi;               /* 1 = SysV x86-64, 2 = Win64 */
    uint32_t reserved0;              /* must be zero */
    uint8_t semantic_hash[SU_HASH_BYTES];   /* the SU-LIR module this kernel implements */
    uint8_t artifact_hash[SU_HASH_BYTES];   /* SHA-256 over inputs + backend/toolchain identity */
    uint64_t code_size;              /* bytes of machine code (diagnostics only) */
    uint32_t insn_count;             /* SU-LIR instruction count (source map length) */
    uint32_t reserved1;              /* must be zero */
} SuNativeArtifactInfo;

_Static_assert(sizeof(SuNativeArtifactInfo) == 112u, "SuNativeArtifactInfo size");

#define SU_NATIVE_BACKEND_ASMJIT_SCALAR UINT32_C(1)
#define SU_NATIVE_BACKEND_ASMJIT_AVX2   UINT32_C(2)
#define SU_NATIVE_HOST_ABI_SYSV         UINT32_C(1)
#define SU_NATIVE_HOST_ABI_WIN64        UINT32_C(2)

/* Source map entry: native byte range [begin, end) within the artifact's code for SU-LIR
 * instruction `insn`. Lowering emits one or more ranges per instruction, ascending by begin. */
typedef struct SuNativeSourceRange {
    uint32_t insn;
    uint32_t begin;
    uint32_t end;
    uint32_t reserved;               /* must be zero */
} SuNativeSourceRange;

_Static_assert(sizeof(SuNativeSourceRange) == 16u, "SuNativeSourceRange size");

#endif
