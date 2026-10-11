/* M1-01 scalar x86-64 native backend: public C API.
 *
 * Lowers one verified SU-LIR module (src/ir/su_lir.h) to machine code through the pinned
 * AsmJit library (third_party/asmjit, see planning/contracts/M1_NATIVE_CONTRACT.md). The kernel
 * ABI and identity structs are in include/sutekh/native/abi.h (frozen contract).
 *
 * Only backends/native/su_native_asmjit.cpp is C++. Built only when CMake option SUTEKH_NATIVE
 * is ON.
 *
 * su_native_run_checked performs the same wrapper checks as su_vm_invoke_v1 (mirrored in
 * su_native.c, because the VM's checks are file-local) before calling the kernel. Pointers handed
 * out by accessors are borrowed and valid until su_native_artifact_free. The module passed to
 * su_native_compile is borrowed and must outlive the artifact.
 *
 * Not thread-safe: the fault-injection hook is process-global. */
#ifndef SU_NATIVE_H
#define SU_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/native/abi.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SuNativeArtifact SuNativeArtifact;

typedef struct SuNativeDiag {
    uint32_t code;          /* SU_OK when no error */
    uint32_t failed_insn;   /* SU_LIR instruction index when attributable, else UINT32_MAX */
    char message[128];
} SuNativeDiag;

/* Lowers and publishes one artifact (code mapped R+X, never RWX). caps is the required
 * SU_NCAP_* set: exactly SU_NCAP_X86_64_SCALAR selects the scalar lowering; SU_NCAP_X86_64_AVX2
 * (alone or with SCALAR) selects the AVX2 lowering (M1-03, backends/native/avx2/). Any other value,
 * or AVX2 when su_native_host_caps() does not report it (or it was disabled by the test hook),
 * returns SU_E_CAPABILITY_DENIED. The AVX2 lowering returns SU_E_RESOURCE_LIMIT when its frame
 * would exceed SU_NATIVE_AVX2_FRAME_LIMIT_BYTES; callers then compile with SU_NCAP_X86_64_SCALAR
 * (see su_native_best_backend). Returns SU_E_RESOURCE_LIMIT on allocation or mapping failure with
 * *out NULL and nothing retained. */
uint32_t su_native_compile(const SuLirModule *m, uint32_t layout_generation, uint32_t caps,
                           SuNativeArtifact **out, SuNativeDiag *diag);

void su_native_artifact_free(SuNativeArtifact *a);

uint32_t su_native_artifact_info(const SuNativeArtifact *a, SuNativeArtifactInfo *out);

/* NULL when a is NULL. */
SuNativeKernelFn su_native_entry(const SuNativeArtifact *a);

/* One range per SU-LIR instruction, ascending, index == instruction index. */
uint32_t su_native_source_map(const SuNativeArtifact *a, const SuNativeSourceRange **ranges,
                              uint32_t *count);

uint32_t su_native_code(const SuNativeArtifact *a, const uint8_t **code, size_t *size);

/* Writes the AsmJit logger text (NUL-terminated) into buf. *len receives the text length
 * excluding the NUL. If cap is too small, returns SU_E_RESOURCE_LIMIT with *len set. */
uint32_t su_native_disassemble(const SuNativeArtifact *a, char *buf, size_t cap, size_t *len);

/* Row limit used by su_native_run_checked. Default min(module max_rows, SU_LIMIT_ROWS). */
uint32_t su_native_set_row_limit(SuNativeArtifact *a, uint64_t limit);

/* Wrapper checks of su_vm_invoke_v1, then the kernel. On SU_OK the staging columns are fully
 * written. On failure no column is written by the kernel. */
uint32_t su_native_run_checked(SuNativeArtifact *a, const SuInvocation *input, SuOutputs *out);

/* Host capabilities usable by compile: SU_NCAP_X86_64_SCALAR always, SU_NCAP_X86_64_AVX2 when
 * CPUID reports AVX2 (leaf 7 EBX[5], with OSXSAVE and AVX), XGETBV(0) has XCR0 bits 1 and 2 set,
 * and it is not disabled by su_native_test_disable_caps. Other architectures: scalar only. */
uint32_t su_native_host_caps(void);

/* Selection helper: SU_NCAP_X86_64_AVX2 when su_native_host_caps() reports it, else
 * SU_NCAP_X86_64_SCALAR. The caller passes the result to su_native_compile. If that compile
 * returns SU_E_RESOURCE_LIMIT (AVX2 frame bound), the caller retries with SU_NCAP_X86_64_SCALAR. */
uint32_t su_native_best_backend(void);

/* ---- test hooks (not for gameplay code) ---- */
/* Bits of SU_NCAP_* removed from su_native_host_caps. Persistent until called with 0. */
void su_native_test_disable_caps(uint32_t mask);
#define SU_NATIVE_FAIL_MMAP     UINT32_C(1)
#define SU_NATIVE_FAIL_MPROTECT UINT32_C(2)
/* Persistent until called with 0. Makes the exec-mapping step fail with SU_E_RESOURCE_LIMIT. */
void su_native_test_inject_failure(uint32_t mask);
/* Artifact hash for the given inputs (same function compile uses). */
void su_native_test_hash(const uint8_t semantic_hash[SU_HASH_BYTES], uint32_t layout_generation,
                         uint32_t caps, uint8_t out[SU_HASH_BYTES]);
/* AsmJit commit the library was pinned to (from third_party/asmjit.lock.json at configure time). */
const char *su_native_asmjit_commit(void);

#ifdef __cplusplus
}
#endif

#endif
