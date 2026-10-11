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
 * SU_NCAP_* set; only SU_NCAP_X86_64_SCALAR is available in this build, any other bit returns
 * SU_E_CAPABILITY_DENIED. Returns SU_E_RESOURCE_LIMIT on allocation or mapping failure with *out
 * NULL and nothing retained. */
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

/* ---- test hooks (not for gameplay code) ---- */
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
