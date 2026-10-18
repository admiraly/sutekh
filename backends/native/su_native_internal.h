/* Internal seams of the native backend. Not part of the public API.
 *  - su_native.c: C glue (compile orchestration, identity hash, checked invocation, exec mapping)
 *  - su_native_asmjit.cpp: AsmJit lowering, reached through su_native_lower.h only */
#ifndef SU_NATIVE_INTERNAL_H
#define SU_NATIVE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "backends/native/su_native.h"

struct SuNativeArtifact {
    SuNativeArtifactInfo info;
    const SuLirModule *module;     /* borrowed */
    uint64_t row_limit;
    uint8_t *code;                 /* R+X mapping of map_size bytes; code_size used */
    size_t code_size;
    size_t map_size;
    SuNativeKernelFn fn;
    SuNativeSourceRange *ranges;   /* insn_count entries */
    char *disasm;                  /* NUL-terminated */
    size_t disasm_len;
    void *unwind_table;            /* Windows: the registered RUNTIME_FUNCTION (inside the mapping); NULL = none */
};

/* Executable mapping (su_native.c): RW, copy, then R+X. Never RWX. */
uint32_t su_native_exec_publish(const uint8_t *src, size_t size, uint8_t **code, size_t *map_size);
void su_native_exec_release(uint8_t *code, size_t map_size);
/* As su_native_exec_publish, plus Windows x64 unwind data (2026-10-18): the UNWIND_INFO bytes and a
 * RUNTIME_FUNCTION for [0, func_end) are placed after the code in the same mapping (read-only once published) and
 * registered with RtlAddFunctionTable; *table receives the registered entry (NULL when nothing was registered:
 * other platforms, no unwind data, or the test seam SUTEKH_NATIVE_NO_UNWIND=1). Unregister with
 * su_native_exec_unregister before su_native_exec_release. */
uint32_t su_native_exec_publish_unwind(const uint8_t *src, size_t size, const uint8_t *unwind, uint32_t unwind_size,
                                       uint32_t func_end, uint8_t **code, size_t *map_size, void **table);
void su_native_exec_unregister(void *table);

#endif
